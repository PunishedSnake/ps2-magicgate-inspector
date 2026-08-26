/* SPDX-License-Identifier: MIT */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>
#include <stdio.h>
#include <string.h>

#include "card_math.h"
#include "image_quick_verify.h"

#define QV_PAGE_DATA 512u
#define QV_PAGE_RAW 528u
#define QV_MAX_PAGES 262144u

#pragma pack(push, 1)
typedef struct MciQuickSuperblock {
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
} MciQuickSuperblock;
#pragma pack(pop)

static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";

static int ReadExact(int fd, void *buffer, unsigned int size)
{
    unsigned char *p = (unsigned char *)buffer;
    unsigned int done = 0u;

    while (done < size) {
        int rc = fileXioRead(fd, p + done, (int)(size - done));
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += (unsigned int)rc;
    }
    return 0;
}

static int KnownPageCount(u32 pages)
{
    return pages == 8192u || pages == 16384u || pages == 32768u ||
           pages == 65536u || pages == 131072u || pages == 262144u;
}

static int ParseGeometry(const unsigned char page[QV_PAGE_DATA],
                         MciCardGeometry *geometry)
{
    const MciQuickSuperblock *sb = (const MciQuickSuperblock *)page;
    u64 pages;

    if (memcmp(sb->magic, SuperblockMagic, sizeof(SuperblockMagic)) != 0 ||
        sb->page_len != QV_PAGE_DATA || sb->pages_per_cluster == 0u ||
        sb->pages_per_block == 0u || sb->clusters_per_card == 0u)
        return -1;
    pages = (u64)sb->pages_per_cluster * sb->clusters_per_card;
    if (pages == 0u || pages > QV_MAX_PAGES || !KnownPageCount((u32)pages))
        return -2;

    memset(geometry, 0, sizeof(*geometry));
    geometry->page_size = sb->page_len;
    geometry->pages_per_cluster = sb->pages_per_cluster;
    geometry->pages_per_block = sb->pages_per_block;
    geometry->clusters_per_card = sb->clusters_per_card;
    geometry->total_pages = (u32)pages;
    geometry->from_superblock = 1;
    return 0;
}

int MciCardImageQuickReopenVerify(const char *path,
                                  MciCardImageFormat format,
                                  MciCardImageReport *report)
{
    unsigned char first[QV_PAGE_DATA] __attribute__((aligned(64)));
    iox_stat_t stat;
    MciCardGeometry geometry;
    u32 stride = format == MCI_CARD_IMAGE_PS2 ? QV_PAGE_RAW : QV_PAGE_DATA;
    u32 pages;
    int fd;
    int rc;

    if (path == NULL || report == NULL)
        return -1;
    MciCardImageResetReport(report, -1, format);
    snprintf(report->path, sizeof(report->path), "%s", path);

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    if (rc < 0 || stat.size == 0u || (stat.size % stride) != 0u)
        goto invalid;
    pages = stat.size / stride;
    if (!KnownPageCount(pages))
        goto invalid;

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0) {
        report->result = MCI_CARD_IMAGE_USB_ERROR;
        report->verify_rc = fd;
        return fd;
    }
    rc = ReadExact(fd, first, sizeof(first));
    fileXioClose(fd);
    if (rc < 0)
        goto invalid;
    if (ParseGeometry(first, &geometry) < 0 || geometry.total_pages != pages)
        goto invalid;

    report->geometry = geometry;
    report->pages_total = pages;
    report->pages_done = 1u;
    report->output_bytes = stat.size;
    report->verified = 1;
    report->verify_rc = 0;
    report->result = MCI_CARD_IMAGE_OK;
    return 0;

invalid:
    report->result = MCI_CARD_IMAGE_VERIFY_ERROR;
    report->verify_rc = -40;
    return -40;
}

int MciCardImageFindFirstEccMismatch(const char *path, u32 *page_out,
                                     unsigned char actual[12],
                                     unsigned char expected[12])
{
    unsigned char raw[QV_PAGE_RAW] __attribute__((aligned(64)));
    iox_stat_t stat;
    u32 pages;
    u32 page;
    int fd;

    if (path == NULL || page_out == NULL || actual == NULL || expected == NULL)
        return -1;
    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(path, &stat) < 0 || stat.size == 0u ||
        (stat.size % QV_PAGE_RAW) != 0u)
        return -2;
    pages = stat.size / QV_PAGE_RAW;
    if (!KnownPageCount(pages))
        return -3;

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;
    for (page = 0u; page < pages; page++) {
        if (ReadExact(fd, raw, sizeof(raw)) < 0) {
            fileXioClose(fd);
            return -4;
        }
        MciCardMathBuildEcc12(raw, expected);
        if (memcmp(raw + QV_PAGE_DATA, expected, 12u) != 0) {
            memcpy(actual, raw + QV_PAGE_DATA, 12u);
            *page_out = page;
            fileXioClose(fd);
            return 1;
        }
    }
    fileXioClose(fd);
    return 0;
}
