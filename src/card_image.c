/* SPDX-License-Identifier: MIT */
/*
 * Drebin memory-card image engine.
 *
 * .vmc is emitted as 512-byte logical NAND pages. .ps2 is emitted in the raw
 * PCSX2 layout: 512 data bytes followed by 16 spare bytes containing four
 * 3-byte ECC values plus four zero bytes. Physical spare bytes are not exposed
 * by libmc, so .ps2 ECC is regenerated from the corrected page data.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_image.h"
#include "card_math.h"
#include "progress.h"
#include "r5900_memops.h"

#define IMAGE_PAGE_DATA 512u
#define IMAGE_PAGE_SPARE 16u
#define IMAGE_PAGE_RAW (IMAGE_PAGE_DATA + IMAGE_PAGE_SPARE)
#define IMAGE_DEFAULT_PAGES_PER_BLOCK 16u
#define IMAGE_MAX_PAGES 262144u /* 128 MiB logical card data */
#define IMAGE_PROGRESS_INTERVAL 128u
#define IMAGE_DIR_NAME "MCI"

#pragma pack(push, 1)
typedef struct MciPs2Superblock {
    char magic[28];
    char version[12];
    u16 page_len;
    u16 pages_per_cluster;
    u16 pages_per_block;
    u16 unused;
    u32 clusters_per_card;
    u32 alloc_offset;
    u32 alloc_end;
    u32 rootdir_cluster;
    u32 backup_block1;
    u32 backup_block2;
} MciPs2Superblock;
#pragma pack(pop)

static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";
static const char *const MassRoots[] = {"mass:/", "mass0:/", "mass1:/"};

static int CompleteMcCommand(int issue_rc)
{
    int result = -999;
    int sync_rc;

    /* libmc can reject a command before an RPC is queued. Never call mcSync in
     * that case: doing so used to turn the useful immediate -1 into our -999
     * sentinel and produced the misleading CARD GEOMETRY ERROR seen on dev3. */
    if (issue_rc < 0)
        return issue_rc;
    sync_rc = mcSync(MC_WAIT, NULL, &result);
    if (sync_rc < 0)
        return sync_rc;
    if (sync_rc != 1)
        return -998;
    return result;
}

static int ReadPage(int port, u32 page, unsigned char data[IMAGE_PAGE_DATA])
{
    return CompleteMcCommand(mcReadPage(port, 0, (int)page, data));
}

static int WritePage(int port, u32 page, const unsigned char data[IMAGE_PAGE_DATA])
{
    return CompleteMcCommand(mcWritePage(port, 0, (int)page, (void *)data));
}

static int EraseBlock(int port, u32 block)
{
    return CompleteMcCommand(mcEraseBlock(port, 0, (int)block, -1));
}

static int SpareMatches(const unsigned char data[IMAGE_PAGE_DATA],
                        const unsigned char spare[IMAGE_PAGE_SPARE])
{
    unsigned char expected[IMAGE_PAGE_SPARE] __attribute__((aligned(16)));

    MciCardMathBuildSpare(data, expected);
    return memcmp(spare, expected, 12u) == 0;
}

static int ParseSuperblock(const unsigned char page[IMAGE_PAGE_DATA],
                           MciCardGeometry *geometry)
{
    const MciPs2Superblock *sb = (const MciPs2Superblock *)page;
    u64 pages;

    if (memcmp(sb->magic, SuperblockMagic, sizeof(SuperblockMagic)) != 0)
        return -1;
    if (sb->page_len != IMAGE_PAGE_DATA || sb->pages_per_cluster == 0u ||
        sb->pages_per_block == 0u || sb->clusters_per_card == 0u)
        return -2;
    pages = (u64)sb->pages_per_cluster * (u64)sb->clusters_per_card;
    if (pages == 0u || pages > IMAGE_MAX_PAGES)
        return -3;

    memset(geometry, 0, sizeof(*geometry));
    geometry->page_size = sb->page_len;
    geometry->pages_per_cluster = sb->pages_per_cluster;
    geometry->pages_per_block = sb->pages_per_block;
    geometry->clusters_per_card = sb->clusters_per_card;
    geometry->total_pages = (u32)pages;
    geometry->from_superblock = 1;
    return 0;
}

static int KnownPageCount(u32 pages)
{
    switch (pages) {
        case 8192u:    /* 4 MiB */
        case 16384u:   /* 8 MiB */
        case 32768u:   /* 16 MiB */
        case 65536u:   /* 32 MiB */
        case 131072u:  /* 64 MiB */
        case 262144u:  /* 128 MiB */
            return 1;
        default:
            return 0;
    }
}

void MciCardImageResetReport(MciCardImageReport *report, int port,
                             MciCardImageFormat format)
{
    memset(report, 0, sizeof(*report));
    report->port = port;
    report->format = format;
    report->result = MCI_CARD_IMAGE_NOT_RUN;
    report->verify_rc = -999;
    report->format_rc = -999;
}

const char *MciCardImageFormatName(MciCardImageFormat format)
{
    return format == MCI_CARD_IMAGE_PS2 ? "PCSX2 .ps2" : "OPL .vmc";
}

const char *MciCardImageResultText(MciCardImageResult result)
{
    switch (result) {
        case MCI_CARD_IMAGE_OK: return "PASS";
        case MCI_CARD_IMAGE_NO_CARD: return "NO PS2 CARD";
        case MCI_CARD_IMAGE_GEOMETRY_ERROR: return "CARD GEOMETRY ERROR";
        case MCI_CARD_IMAGE_USB_ERROR: return "USB I/O ERROR";
        case MCI_CARD_IMAGE_READ_ERROR: return "CARD READ ERROR";
        case MCI_CARD_IMAGE_WRITE_ERROR: return "CARD WRITE ERROR";
        case MCI_CARD_IMAGE_VERIFY_ERROR: return "IMAGE VERIFY ERROR";
        case MCI_CARD_IMAGE_SIZE_MISMATCH: return "IMAGE/CARD SIZE MISMATCH";
        case MCI_CARD_IMAGE_FORMAT_ERROR: return "FORMAT ERROR";
        default: return "NOT RUN";
    }
}

int MciCardImageProbeGeometry(int port, MciCardGeometry *geometry)
{
    static const u32 CandidatePages[] = {
        262144u, 131072u, 65536u, 32768u, 16384u, 8192u
    };
    unsigned char page[IMAGE_PAGE_DATA] __attribute__((aligned(64)));
    unsigned int i;
    int type = MC_TYPE_NONE;
    int free_clusters = -1;
    int formatted = 0;
    int rc;

    if (geometry == NULL)
        return -1;
    memset(geometry, 0, sizeof(*geometry));

    /* Legacy MCMAN keeps per-port card type state. Probe the actual selected
     * slot before the first raw-page RPC; mc1 must not inherit mc0's state. A
     * changed-card (-1) or no-format (-2) result is valid if type says PS2. */
    rc = CompleteMcCommand(mcGetInfo(port, 0, &type, &free_clusters, &formatted));
    if (rc < -2)
        return rc;
    if (type != MC_TYPE_PS2)
        return sceMcResFailDetect;

    rc = ReadPage(port, 0u, page);
    if (rc < 0)
        return rc;
    if (ParseSuperblock(page, geometry) == 0)
        return 0;

    /* Unformatted cards have no usable superblock. Probe conservative standard
     * capacity boundaries using read-only page commands and keep the largest
     * address the card accepts. */
    for (i = 0; i < sizeof(CandidatePages) / sizeof(CandidatePages[0]); i++) {
        rc = ReadPage(port, CandidatePages[i] - 1u, page);
        if (rc >= 0) {
            geometry->page_size = IMAGE_PAGE_DATA;
            geometry->pages_per_cluster = 2u;
            geometry->pages_per_block = IMAGE_DEFAULT_PAGES_PER_BLOCK;
            geometry->total_pages = CandidatePages[i];
            geometry->clusters_per_card = CandidatePages[i] / 2u;
            geometry->from_superblock = 0;
            return 0;
        }
    }
    return -2;
}

static int EnsureImageDirectory(char *directory, unsigned int directory_size)
{
    iox_stat_t stat;
    unsigned int root;
    int fd;
    int rc;

    for (root = 0; root < sizeof(MassRoots) / sizeof(MassRoots[0]); root++) {
        fd = fileXioDopen(MassRoots[root]);
        if (fd < 0)
            continue;
        fileXioDclose(fd);
        if (snprintf(directory, directory_size, "%s%s",
                     MassRoots[root], IMAGE_DIR_NAME) < 0)
            continue;
        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(directory, &stat);
        if (rc >= 0 && FIO_S_ISDIR(stat.mode))
            return 0;
        rc = fileXioMkdir(directory, 0777);
        if (rc >= 0) {
            memset(&stat, 0, sizeof(stat));
            if (fileXioGetStat(directory, &stat) >= 0 && FIO_S_ISDIR(stat.mode))
                return 0;
        }
    }
    return -1;
}

static int UniqueImagePath(int port, MciCardImageFormat format,
                           char *path, unsigned int path_size)
{
    char directory[64];
    iox_stat_t stat;
    const char *ext = format == MCI_CARD_IMAGE_PS2 ? "ps2" : "vmc";
    int index;

    if (EnsureImageDirectory(directory, sizeof(directory)) < 0)
        return -1;
    for (index = 0; index < 100; index++) {
        snprintf(path, path_size, "%s/mc%d-%02d.%s", directory, port, index, ext);
        memset(&stat, 0, sizeof(stat));
        if (fileXioGetStat(path, &stat) < 0)
            return 0;
    }
    return -2;
}

static int WriteExact(int fd, const void *buffer, unsigned int size)
{
    const unsigned char *p = (const unsigned char *)buffer;
    unsigned int done = 0u;
    int rc;

    while (done < size) {
        rc = fileXioWrite(fd, p + done, (int)(size - done));
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += (unsigned int)rc;
    }
    return 0;
}

static int ReadExact(int fd, void *buffer, unsigned int size)
{
    unsigned char *p = (unsigned char *)buffer;
    unsigned int done = 0u;
    int rc;

    while (done < size) {
        rc = fileXioRead(fd, p + done, (int)(size - done));
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += (unsigned int)rc;
    }
    return 0;
}

static int WriteSidecar(const MciCardImageReport *report)
{
    char path[MCI_CARD_IMAGE_PATH_MAX + 16];
    char text[512];
    int fd;
    int length;
    int rc;

    snprintf(path, sizeof(path), "%s.mci.txt", report->path);
    length = snprintf(text, sizeof(text),
                      "Drebin PS2 card image\nformat=%s\nport=mc%d\npage_size=%u\npages_per_cluster=%u\npages_per_block=%u\ntotal_pages=%u\nlogical_bytes=%llu\ncrc32=%08X\nverified=%d\n",
                      MciCardImageFormatName(report->format), report->port,
                      report->geometry.page_size,
                      report->geometry.pages_per_cluster,
                      report->geometry.pages_per_block,
                      report->geometry.total_pages,
                      (unsigned long long)report->geometry.total_pages * IMAGE_PAGE_DATA,
                      report->logical_crc32, report->verified);
    if (length <= 0 || (unsigned int)length >= sizeof(text))
        return -1;
    (void)fileXioRemove(path);
    fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT);
    if (fd < 0)
        return fd;
    rc = WriteExact(fd, text, (unsigned int)length);
    if (fileXioClose(fd) < 0 && rc == 0)
        rc = -1;
    return rc;
}

int MciCardImageVerifyFile(const char *path, MciCardImageFormat format,
                           MciCardImageReport *report)
{
    unsigned char raw[IMAGE_PAGE_RAW] __attribute__((aligned(64)));
    MciCardGeometry sb_geometry;
    iox_stat_t stat;
    u32 stride = format == MCI_CARD_IMAGE_PS2 ? IMAGE_PAGE_RAW : IMAGE_PAGE_DATA;
    u32 pages;
    u32 page;
    u32 crc = 0u;
    int fd;
    int rc;

    if (path == NULL || report == NULL)
        return -1;
    MciCardImageResetReport(report, -1, format);
    snprintf(report->path, sizeof(report->path), "%s", path);

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    if (rc < 0 || stat.size == 0u || (stat.size % stride) != 0u) {
        report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
        report->verify_rc = rc < 0 ? rc : -2;
        return report->verify_rc;
    }
    pages = stat.size / stride;
    if (pages == 0u || pages > IMAGE_MAX_PAGES || !KnownPageCount(pages)) {
        report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
        report->verify_rc = -3;
        return -3;
    }

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0) {
        report->result = MCI_CARD_IMAGE_USB_ERROR;
        report->verify_rc = fd;
        return fd;
    }
    memset(&sb_geometry, 0, sizeof(sb_geometry));
    for (page = 0u; page < pages; page++) {
        rc = ReadExact(fd, raw, stride);
        if (rc < 0) {
            fileXioClose(fd);
            report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
            report->verify_rc = rc;
            return rc;
        }
        if (format == MCI_CARD_IMAGE_PS2 && !SpareMatches(raw, raw + IMAGE_PAGE_DATA)) {
            fileXioClose(fd);
            report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
            report->verify_rc = -4;
            return -4;
        }
        if (page == 0u && ParseSuperblock(raw, &sb_geometry) == 0 &&
            sb_geometry.total_pages != pages) {
            fileXioClose(fd);
            report->result = MCI_CARD_IMAGE_SIZE_MISMATCH;
            report->verify_rc = -5;
            return -5;
        }
        crc = MciCardMathCrc32Update(crc, raw, IMAGE_PAGE_DATA);
        if ((page % IMAGE_PROGRESS_INTERVAL) == 0u) {
            int percent = (int)(((u64)page * 100u) / pages);
            MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, percent,
                              "Verifying memory-card image",
                              "Reading the image back and validating logical data plus .ps2 ECC records.");
        }
    }
    fileXioClose(fd);

    report->geometry = sb_geometry;
    if (!report->geometry.total_pages) {
        report->geometry.page_size = IMAGE_PAGE_DATA;
        report->geometry.pages_per_cluster = 2u;
        report->geometry.pages_per_block = IMAGE_DEFAULT_PAGES_PER_BLOCK;
        report->geometry.total_pages = pages;
        report->geometry.clusters_per_card = pages / 2u;
    }
    report->pages_done = pages;
    report->pages_total = pages;
    report->output_bytes = stat.size;
    report->logical_crc32 = crc;
    report->verified = 1;
    report->verify_rc = 0;
    report->result = MCI_CARD_IMAGE_OK;
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100,
                      "Image verification complete",
                      "The image is structurally valid and its entire logical page stream was read successfully.");
    return 0;
}

int MciCardImageExport(int port, MciCardImageFormat format,
                       MciCardImageReport *report)
{
    unsigned char page_data[IMAGE_PAGE_DATA] __attribute__((aligned(64)));
    unsigned char raw[IMAGE_PAGE_RAW] __attribute__((aligned(64)));
    MciCardImageReport verify;
    MciCardGeometry geometry;
    u32 page;
    u32 crc = 0u;
    int fd;
    int rc;

    if (report == NULL)
        return -1;
    MciCardImageResetReport(report, port, format);

    rc = MciCardImageProbeGeometry(port, &geometry);
    if (rc < 0) {
        report->result = rc <= -10 ? MCI_CARD_IMAGE_NO_CARD
                                   : MCI_CARD_IMAGE_GEOMETRY_ERROR;
        report->verify_rc = rc;
        return rc;
    }
    report->geometry = geometry;
    report->pages_total = geometry.total_pages;

    rc = UniqueImagePath(port, format, report->path, sizeof(report->path));
    if (rc < 0) {
        report->result = MCI_CARD_IMAGE_USB_ERROR;
        return rc;
    }
    fd = fileXioOpen(report->path, FIO_O_WRONLY | FIO_O_CREAT);
    if (fd < 0) {
        report->result = MCI_CARD_IMAGE_USB_ERROR;
        return fd;
    }

    for (page = 0u; page < geometry.total_pages; page++) {
        rc = ReadPage(port, page, page_data);
        if (rc < 0) {
            fileXioClose(fd);
            (void)fileXioRemove(report->path);
            report->result = MCI_CARD_IMAGE_READ_ERROR;
            return rc;
        }
        crc = MciCardMathCrc32Update(crc, page_data, sizeof(page_data));
        if (format == MCI_CARD_IMAGE_PS2) {
            MciFastCopy(raw, page_data, IMAGE_PAGE_DATA);
            MciCardMathBuildSpare(page_data, raw + IMAGE_PAGE_DATA);
            rc = WriteExact(fd, raw, sizeof(raw));
        } else {
            rc = WriteExact(fd, page_data, sizeof(page_data));
        }
        if (rc < 0) {
            fileXioClose(fd);
            (void)fileXioRemove(report->path);
            report->result = MCI_CARD_IMAGE_USB_ERROR;
            return rc;
        }
        report->pages_done = page + 1u;
        if ((page % IMAGE_PROGRESS_INTERVAL) == 0u) {
            char detail[160];
            int percent = (int)(((u64)(page + 1u) * 90u) / geometry.total_pages);
            snprintf(detail, sizeof(detail),
                     "mc%d page %u/%u -> %s", port, page + 1u,
                     geometry.total_pages, MciCardImageFormatName(format));
            MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, percent,
                              "Creating memory-card image", detail);
        }
    }
    rc = fileXioClose(fd);
    if (rc < 0) {
        report->result = MCI_CARD_IMAGE_USB_ERROR;
        return rc;
    }

    report->logical_crc32 = crc;
    report->output_bytes = (u64)geometry.total_pages *
                           (format == MCI_CARD_IMAGE_PS2 ? IMAGE_PAGE_RAW : IMAGE_PAGE_DATA);
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 92,
                      "Verifying the completed image",
                      "Reopening the USB file and validating the complete stream before reporting success.");
    rc = MciCardImageVerifyFile(report->path, format, &verify);
    report->verify_rc = rc;
    if (rc < 0 || !verify.verified || verify.logical_crc32 != crc) {
        report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
        return rc < 0 ? rc : -6;
    }
    report->verified = 1;
    report->result = MCI_CARD_IMAGE_OK;
    (void)WriteSidecar(report);
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100,
                      "Verified card image saved",
                      "The USB image was written, read back in full and matched the captured logical card CRC.");
    return 0;
}

int MciCardImageFindLatest(int port, MciCardImageFormat format,
                           char *path, unsigned int path_size)
{
    iox_stat_t stat;
    const char *ext = format == MCI_CARD_IMAGE_PS2 ? "ps2" : "vmc";
    unsigned int root;
    int index;

    if (path == NULL || path_size == 0u)
        return -1;
    for (root = 0; root < sizeof(MassRoots) / sizeof(MassRoots[0]); root++) {
        for (index = 99; index >= 0; index--) {
            snprintf(path, path_size, "%s%s/mc%d-%02d.%s",
                     MassRoots[root], IMAGE_DIR_NAME, port, index, ext);
            memset(&stat, 0, sizeof(stat));
            if (fileXioGetStat(path, &stat) >= 0 && FIO_S_ISREG(stat.mode) && stat.size > 0u)
                return 0;
        }
    }
    path[0] = '\0';
    return -ENOENT;
}

static int OpenImageAndGetPages(const char *path, MciCardImageFormat format,
                                u32 *pages)
{
    iox_stat_t stat;
    u32 stride = format == MCI_CARD_IMAGE_PS2 ? IMAGE_PAGE_RAW : IMAGE_PAGE_DATA;
    int fd;

    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(path, &stat) < 0 || stat.size == 0u ||
        (stat.size % stride) != 0u)
        return -1;
    *pages = stat.size / stride;
    fd = fileXioOpen(path, FIO_O_RDONLY);
    return fd;
}

int MciCardImageRestoreExact(int port, const char *path,
                             MciCardImageFormat format,
                             MciCardImageReport *report)
{
    unsigned char raw[IMAGE_PAGE_RAW] __attribute__((aligned(64)));
    unsigned char readback[IMAGE_PAGE_DATA] __attribute__((aligned(64)));
    MciCardImageReport verify;
    MciCardGeometry destination;
    u32 pages;
    u32 page;
    u32 expected_crc;
    u32 actual_crc = 0u;
    u32 pages_per_block;
    int fd;
    int rc;

    if (report == NULL || path == NULL)
        return -1;
    MciCardImageResetReport(report, port, format);
    snprintf(report->path, sizeof(report->path), "%s", path);

    rc = MciCardImageVerifyFile(path, format, &verify);
    if (rc < 0) {
        report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
        report->verify_rc = rc;
        return rc;
    }
    expected_crc = verify.logical_crc32;
    pages = verify.geometry.total_pages;

    rc = MciCardImageProbeGeometry(port, &destination);
    if (rc < 0) {
        report->result = MCI_CARD_IMAGE_GEOMETRY_ERROR;
        return rc;
    }
    if (destination.total_pages != pages) {
        report->result = MCI_CARD_IMAGE_SIZE_MISMATCH;
        return -20;
    }
    pages_per_block = destination.pages_per_block != 0u
                          ? destination.pages_per_block
                          : IMAGE_DEFAULT_PAGES_PER_BLOCK;

    fd = OpenImageAndGetPages(path, format, &pages);
    if (fd < 0) {
        report->result = MCI_CARD_IMAGE_USB_ERROR;
        return fd;
    }

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 1,
                      "Restoring exact memory-card image",
                      "The destination geometry matches. Erasing and rewriting the card block by block.");
    for (page = 0u; page < pages; page++) {
        u32 stride = format == MCI_CARD_IMAGE_PS2 ? IMAGE_PAGE_RAW : IMAGE_PAGE_DATA;
        if ((page % pages_per_block) == 0u) {
            rc = EraseBlock(port, page / pages_per_block);
            if (rc < 0) {
                fileXioClose(fd);
                report->result = MCI_CARD_IMAGE_WRITE_ERROR;
                return rc;
            }
        }
        rc = ReadExact(fd, raw, stride);
        if (rc < 0) {
            fileXioClose(fd);
            report->result = MCI_CARD_IMAGE_USB_ERROR;
            return rc;
        }
        if (format == MCI_CARD_IMAGE_PS2 && !SpareMatches(raw, raw + IMAGE_PAGE_DATA)) {
            fileXioClose(fd);
            report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
            return -21;
        }
        rc = WritePage(port, page, raw);
        if (rc < 0) {
            fileXioClose(fd);
            report->result = MCI_CARD_IMAGE_WRITE_ERROR;
            return rc;
        }
        report->pages_done = page + 1u;
        if ((page % IMAGE_PROGRESS_INTERVAL) == 0u) {
            int percent = 2 + (int)(((u64)(page + 1u) * 78u) / pages);
            MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, percent,
                              "Writing memory-card pages",
                              "Each erase block is programmed only after the source image has passed structural verification.");
        }
    }
    fileXioClose(fd);

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 82,
                      "Verifying restored card",
                      "Reading every destination page back and comparing the resulting logical CRC with the image.");
    for (page = 0u; page < pages; page++) {
        rc = ReadPage(port, page, readback);
        if (rc < 0) {
            report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
            report->verify_rc = rc;
            return rc;
        }
        actual_crc = MciCardMathCrc32Update(actual_crc, readback, sizeof(readback));
        if ((page % IMAGE_PROGRESS_INTERVAL) == 0u) {
            int percent = 82 + (int)(((u64)(page + 1u) * 18u) / pages);
            MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, percent,
                              "Verifying restored card",
                              "Read-back is in progress; the image is not accepted until the full logical CRC matches.");
        }
    }
    if (actual_crc != expected_crc) {
        report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
        report->verify_rc = -22;
        return -22;
    }

    report->geometry = destination;
    report->pages_total = pages;
    report->logical_crc32 = actual_crc;
    report->verified = 1;
    report->verify_rc = 0;
    report->result = MCI_CARD_IMAGE_OK;
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100,
                      "Exact restore verified",
                      "Every destination page was read back and the card CRC matches the source image.");
    return 0;
}

int MciCardForceFormatWithBackup(int port, MciCardImageReport *report)
{
    int rc;

    if (report == NULL)
        return -1;
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 1,
                      "Creating pre-format recovery image",
                      "Force format is locked until a complete .ps2 safety image is written and verified on USB.");
    rc = MciCardImageExport(port, MCI_CARD_IMAGE_PS2, report);
    if (rc < 0 || !report->verified)
        return rc < 0 ? rc : -30;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 98,
                      "Formatting memory card",
                      "The verified safety image is complete. Formatting the selected PS2 card now.");
    rc = CompleteMcCommand(mcFormat(port, 0));
    report->format_rc = rc;
    if (rc < 0) {
        report->result = MCI_CARD_IMAGE_FORMAT_ERROR;
        return rc;
    }
    report->result = MCI_CARD_IMAGE_OK;
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100,
                      "Force format complete",
                      "The card was formatted only after its verified .ps2 recovery image had been secured on USB.");
    return 0;
}
