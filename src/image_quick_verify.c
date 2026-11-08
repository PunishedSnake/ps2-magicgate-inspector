/* SPDX-License-Identifier: MIT */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>
#include <stdio.h>
#include <string.h>

#include "card_math.h"
#include "mc_fs_format.h"
#include "image_quick_verify.h"

#define QV_PAGE_DATA 512u
#define QV_PAGE_RAW 528u
#define QV_MAX_PAGES 262144u


int __real_fileXioRead(int fd, void *buffer, int size);

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

/* Trusted reopen verification deliberately validates only the first logical
 * page after a prior full scan. Calling the wrapped fileXioRead here would make
 * the sequential read-ahead layer fetch a complete 16896-byte production batch
 * merely to consume 512 bytes. Bypass that optional batching layer for this
 * bounded one-page probe while retaining the normal fileXio RPC/cache contract. */
static int ReadExactDirect(int fd, void *buffer, unsigned int size)
{
    unsigned char *p = (unsigned char *)buffer;
    unsigned int done = 0u;

    while (done < size) {
        int rc = __real_fileXioRead(fd, p + done, (int)(size - done));
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += (unsigned int)rc;
    }
    return 0;
}

static int KnownPageCount(u32 pages)
{
    return MciMcKnownPageCount(pages);
}

static int ParseGeometry(const unsigned char page[QV_PAGE_DATA],
                         MciCardGeometry *geometry)
{
    MciMcSuperblock sb;
    u32 pages;

    memcpy(&sb, page, sizeof(sb));
    if (MciMcSuperblockValidate(&sb, MCI_MC_VALIDATE_GEOMETRY) < 0)
        return -1;
    pages = MciMcSuperblockTotalPages(&sb);
    if (pages == 0u || pages > QV_MAX_PAGES || !KnownPageCount(pages))
        return -2;

    memset(geometry, 0, sizeof(*geometry));
    geometry->page_size = sb.page_len;
    geometry->pages_per_cluster = sb.pages_per_cluster;
    geometry->pages_per_block = sb.pages_per_block;
    geometry->clusters_per_card = sb.clusters_per_card;
    geometry->total_pages = pages;
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
    rc = ReadExactDirect(fd, first, sizeof(first));
    fileXioClose(fd);
    if (rc < 0)
        goto invalid;
    if (ParseGeometry(first, &geometry) < 0 || geometry.total_pages != pages)
        goto invalid;

    (void)MciMcHealthDecode(first, &report->health);
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
        /* Legacy 0.4.x bit-7 ECC is accepted here exactly as by the full
         * verifier; only a real mismatch is reported. */
        if (MciCardMathSpareCompare(raw, raw + QV_PAGE_DATA) == MCI_ECC_MATCH_NONE) {
            MciCardMathBuildEcc12(raw, expected);
            memcpy(actual, raw + QV_PAGE_DATA, 12u);
            *page_out = page;
            fileXioClose(fd);
            return 1;
        }
    }
    fileXioClose(fd);
    return 0;
}
