/* SPDX-License-Identifier: MIT */
/*
 * Card-side import of decoded save archives (MAX/CBS/SPS/XPS/PSV and every
 * PS1 container) and USB-side container conversion.
 *
 * Contract, identical to the PSU importer:
 *  - decode and validate the whole container in RAM before touching a card;
 *  - refuse existing top-level saves (never overwrite);
 *  - check free space up front;
 *  - re-open every created file and compare it byte-for-byte;
 *  - on any failure delete only the objects this transaction created.
 *
 * PS1 targets go through mcman's PS1 file API (mcOpen/mcWrite on the PS1
 * root). Note mcman's PS1 O_CREAT path clears deleted-save records on the
 * card; the UI warns before confirming.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_hot_swap.h"
#include "diag_log.h"
#include "ps1_card.h"
#include "save_archive.h"
#include "save_codec_ps2.h"
#include "save_transfer.h"

#define AR_CHUNK 16384u
#define AR_MAX_SOURCE (MCI_SAVE_MAX_BYTES + 1024u * 1024u)
#define AR_MAX_CREATED (MCI_SAVE_MAX_FILES + 2)

static unsigned char ArVerify[AR_CHUNK] __attribute__((aligned(64)));
static unsigned char ArWrite[AR_CHUNK] __attribute__((aligned(64)));

static int McResult(void)
{
    int result = -999;
    int rc = mcSync(MC_WAIT, NULL, &result);
    return rc < 0 ? rc : result;
}

/* ------------------------------------------------------------ file I/O */

static int LoadSource(const char *path, unsigned char **out, u32 *out_size)
{
    iox_stat_t st;
    unsigned char *buf;
    u32 done = 0u;
    int fd;

    *out = NULL;
    *out_size = 0u;
    memset(&st, 0, sizeof(st));
    if (fileXioGetStat(path, &st) < 0 || !FIO_S_ISREG(st.mode) || st.size <= 0)
        return -100;
    if ((u64)st.size > AR_MAX_SOURCE)
        return -101;
    buf = (unsigned char *)malloc((u32)st.size);
    if (buf == NULL)
        return -102;
    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0) {
        free(buf);
        return fd;
    }
    while (done < (u32)st.size) {
        int rc = fileXioRead(fd, buf + done, (int)((u32)st.size - done));
        if (rc <= 0) {
            fileXioClose(fd);
            free(buf);
            return rc < 0 ? rc : -103;
        }
        done += (u32)rc;
    }
    fileXioClose(fd);
    *out = buf;
    *out_size = done;
    return 0;
}

static int WriteVerifiedFile(const char *path, const unsigned char *data, u32 size)
{
    unsigned char *check;
    u32 done = 0u;
    int fd;
    int rc = 0;

    (void)fileXioRemove(path);
    fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
    if (fd < 0)
        return fd;
    while (done < size) {
        int w = fileXioWrite(fd, data + done, (int)(size - done));
        if (w <= 0) { rc = w < 0 ? w : -110; break; }
        done += (u32)w;
    }
    if (fileXioClose(fd) < 0 && rc == 0)
        rc = -111;
    if (rc < 0) {
        (void)fileXioRemove(path);
        return rc;
    }
    if (LoadSource(path, &check, &done) < 0 || done != size ||
        memcmp(check, data, size) != 0) {
        free(check);
        (void)fileXioRemove(path);
        return -112;
    }
    free(check);
    return 0;
}

/* ----------------------------------------------------- shared card ops */

static int TargetExists(int port, const char *path)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int rc;

    memset(&info, 0, sizeof(info));
    rc = mcGetDir(port, 0, path, 0, 1, &info);
    if (rc < 0)
        return rc;
    rc = McResult();
    if (rc > 0)
        return 1;
    if (rc == 0 || rc == sceMcResNoEntry)
        return 0;
    return rc;
}

static int Record(char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX], int *count,
                  const char *path)
{
    if (*count >= AR_MAX_CREATED)
        return -1;
    snprintf(created[*count], MCI_SAVE_TRANSFER_FAILED_PATH_MAX, "%s", path);
    (*count)++;
    return 0;
}

static int Rollback(int port, char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX],
                    int count)
{
    int first_error = 0;
    int i;

    for (i = count - 1; i >= 0; i--) {
        int rc = mcDelete(port, 0, created[i]);
        if (rc >= 0)
            rc = McResult();
        if (rc < 0 && rc != sceMcResNoEntry && first_error == 0)
            first_error = rc;
    }
    return first_error;
}

/* Writes one file from memory, then re-reads and compares it. */
static int WriteCardFile(int port, const char *path, const unsigned char *data,
                         u32 size, char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX],
                         int *created_count, MciSaveTransferReport *report)
{
    u32 done = 0u;
    int fd;
    int rc;

    rc = mcOpen(port, 0, path, FIO_O_WRONLY | FIO_O_CREAT);
    if (rc < 0)
        return rc;
    fd = McResult();
    if (fd < 0)
        return fd;
    if (Record(created, created_count, path) < 0) {
        mcClose(fd); (void)McResult();
        return -20;
    }
    while (done < size) {
        u32 chunk = size - done > AR_CHUNK ? AR_CHUNK : size - done;
        /* libmc DMA wants an aligned EE buffer. */
        memcpy(ArWrite, data + done, chunk);
        rc = mcWrite(fd, ArWrite, (int)chunk);
        if (rc >= 0)
            rc = McResult();
        if (rc != (int)chunk) {
            mcClose(fd); (void)McResult();
            return rc < 0 ? rc : -21;
        }
        report->bytes_written += chunk;
        done += chunk;
    }
    rc = mcFlush(fd);
    if (rc >= 0)
        rc = McResult();
    mcClose(fd);
    if (McResult() < 0 && rc >= 0)
        rc = -22;
    if (rc < 0)
        return rc;

    rc = mcOpen(port, 0, path, FIO_O_RDONLY);
    if (rc < 0)
        return rc;
    fd = McResult();
    if (fd < 0)
        return fd;
    for (done = 0u; done < size;) {
        u32 chunk = size - done > AR_CHUNK ? AR_CHUNK : size - done;
        rc = mcRead(fd, ArVerify, (int)chunk);
        if (rc >= 0)
            rc = McResult();
        if (rc != (int)chunk || memcmp(ArVerify, data + done, chunk) != 0) {
            mcClose(fd); (void)McResult();
            return rc < 0 ? rc : -24;
        }
        done += chunk;
    }
    rc = mcRead(fd, ArVerify, 1);
    if (rc >= 0)
        rc = McResult();
    mcClose(fd);
    (void)McResult();
    if (rc != 0)
        return rc < 0 ? rc : -25;
    report->files_verified++;
    return 0;
}

static int SetPs2Metadata(int port, const char *path, u16 mode,
                          const MciMcDateTime *created, const MciMcDateTime *modified,
                          int timestamps)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int flags = sceMcFileInfoAttr;
    int rc;

    memset(&info, 0, sizeof(info));
    info.AttrFile = mode;
    if (timestamps) {
        memcpy(&info._Create, created, sizeof(info._Create));
        memcpy(&info._Modify, modified, sizeof(info._Modify));
        flags |= sceMcFileInfoCreate | sceMcFileInfoModify;
    }
    rc = mcSetFileInfo(port, 0, path, &info, flags);
    if (rc < 0)
        return rc;
    return McResult();
}

static int HasTimestamp(const MciMcDateTime *t)
{
    return t->year != 0u && t->month != 0u && t->day != 0u;
}

/* -------------------------------------------------------- PS2 archive */

static int ImportPs2Archive(int port, const MciSaveArchive *a,
                            MciSaveTransferReport *report)
{
    char dir[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    char file[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    int created_count = 0;
    int free_clusters = -1;
    int required;
    u32 i;
    int rc;

    report->generation = 2;
    snprintf(report->save_directory, sizeof(report->save_directory), "%s", a->directory);
    report->files_total = (int)a->file_count;
    report->required_clusters = required = (int)MciSaveArchiveRequiredClusters(a);
    snprintf(dir, sizeof(dir), "/%s", a->directory);
    snprintf(report->destination, sizeof(report->destination), "mc%d:%s", port, dir);

    rc = MciNormalCardProbeFormattedType(port, MC_TYPE_PS2, &free_clusters);
    report->target_free_clusters = free_clusters;
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
        return rc;
    }
    rc = TargetExists(port, dir);
    if (rc != 0) {
        report->result = rc > 0 ? MCI_SAVE_TRANSFER_TARGET_CONFLICT
                                : MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
        return rc > 0 ? -102 : rc;
    }
    if (required > free_clusters) {
        report->result = MCI_SAVE_TRANSFER_TARGET_FULL;
        return -103;
    }

    created = malloc(AR_MAX_CREATED * MCI_SAVE_TRANSFER_FAILED_PATH_MAX);
    if (created == NULL) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        return -104;
    }
    rc = mcMkDir(port, 0, dir);
    if (rc >= 0)
        rc = McResult();
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        goto rollback;
    }
    Record(created, &created_count, dir);

    for (i = 0u; i < a->file_count; i++) {
        const MciSaveFile *f = &a->files[i];
        snprintf(file, sizeof(file), "%s/%s", dir, f->name);
        snprintf(report->failed_path, sizeof(report->failed_path), "%s", file);
        rc = WriteCardFile(port, file, f->data, f->size, created, &created_count, report);
        if (rc < 0) {
            report->result = (rc == -24 || rc == -25) ? MCI_SAVE_TRANSFER_VERIFY_ERROR
                                                      : MCI_SAVE_TRANSFER_IO_ERROR;
            goto rollback;
        }
        rc = SetPs2Metadata(port, file, f->mode, &f->created, &f->modified,
                            HasTimestamp(&f->created));
        if (rc < 0) {
            report->result = MCI_SAVE_TRANSFER_IO_ERROR;
            goto rollback;
        }
        report->files_written++;
    }
    rc = SetPs2Metadata(port, dir, a->dir_mode ? a->dir_mode : MCI_PS2_DEFAULT_DIR_MODE,
                        &a->created, &a->modified, HasTimestamp(&a->created));
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        goto rollback;
    }
    free(created);
    report->failed_path[0] = '\0';
    report->saves_total = report->saves_written = 1;
    report->result = MCI_SAVE_TRANSFER_OK;
    return 0;

rollback:
    report->rollback_rc = Rollback(port, created, created_count);
    if (report->rollback_rc < 0)
        report->result = MCI_SAVE_TRANSFER_ROLLBACK_FAILED;
    free(created);
    return rc < 0 ? rc : -1;
}

/* -------------------------------------------------------- PS1 archives */

static int ImportPs1Archives(int port, const MciSaveArchive *list, u32 count,
                             MciSaveTransferReport *report)
{
    char path[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    int created_count = 0;
    int free_blocks = -1;
    u32 need = 0u;
    u32 i;
    int rc;

    report->generation = 1;
    report->saves_total = (int)count;
    report->files_total = (int)count;
    for (i = 0u; i < count; i++)
        need += list[i].files[0].size / MCI_PS1_BLOCK_BYTES;
    report->required_clusters = (int)need;   /* PS1: blocks */
    if (count == 1u)
        snprintf(report->save_directory, sizeof(report->save_directory), "%s",
                 list[0].files[0].name);
    snprintf(report->destination, sizeof(report->destination), "mc%d: (PS1)", port);

    rc = MciNormalCardProbeFormattedType(port, MC_TYPE_PSX, &free_blocks);
    report->target_free_clusters = free_blocks;
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
        return rc;
    }
    /* All-or-nothing preflight: every name must be new, and everything must
     * fit, before the first byte is written. */
    for (i = 0u; i < count; i++) {
        snprintf(path, sizeof(path), "/%s", list[i].files[0].name);
        rc = TargetExists(port, path);
        if (rc != 0) {
            snprintf(report->failed_path, sizeof(report->failed_path), "%s", path);
            report->result = rc > 0 ? MCI_SAVE_TRANSFER_TARGET_CONFLICT
                                    : MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
            return rc > 0 ? -102 : rc;
        }
    }
    if ((int)need > free_blocks) {
        report->result = MCI_SAVE_TRANSFER_TARGET_FULL;
        return -103;
    }

    created = malloc(AR_MAX_CREATED * MCI_SAVE_TRANSFER_FAILED_PATH_MAX);
    if (created == NULL) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        return -104;
    }
    for (i = 0u; i < count; i++) {
        const MciSaveFile *f = &list[i].files[0];
        snprintf(path, sizeof(path), "/%s", f->name);
        snprintf(report->failed_path, sizeof(report->failed_path), "%s", path);
        rc = WriteCardFile(port, path, f->data, f->size, created, &created_count, report);
        if (rc < 0) {
            report->result = (rc == -24 || rc == -25) ? MCI_SAVE_TRANSFER_VERIFY_ERROR
                                                      : MCI_SAVE_TRANSFER_IO_ERROR;
            report->rollback_rc = Rollback(port, created, created_count);
            if (report->rollback_rc < 0)
                report->result = MCI_SAVE_TRANSFER_ROLLBACK_FAILED;
            free(created);
            return rc;
        }
        report->files_written++;
        report->saves_written++;
    }
    free(created);
    report->failed_path[0] = '\0';
    report->result = MCI_SAVE_TRANSFER_OK;
    return 0;
}

/* Decodes a PS1 card image into up to 15 archives. */
static int DecodePs1Card(const unsigned char *data, u32 size, MciSaveArchive **out,
                         u32 *count)
{
    MciPs1Card *card = (MciPs1Card *)malloc(sizeof(*card));
    MciPs1CardSummary summary;
    MciSaveArchive *list;
    u32 i;
    int rc;

    *out = NULL;
    *count = 0u;
    if (card == NULL)
        return MCI_SAVE_E_NOMEM;
    rc = MciPs1CardLoad(card, data, size, NULL);
    if (rc == 0)
        rc = MciPs1CardScan(card, &summary);
    if (rc == 0 && summary.save_count == 0u)
        rc = MCI_SAVE_E_CORRUPT;
    if (rc < 0) {
        free(card);
        return rc;
    }
    list = (MciSaveArchive *)calloc(summary.save_count, sizeof(*list));
    if (list == NULL) {
        free(card);
        return MCI_SAVE_E_NOMEM;
    }
    for (i = 0u; i < summary.save_count; i++) {
        rc = MciPs1CardExtract(card, summary.saves[i].first_slot, &list[i]);
        if (rc < 0) {
            while (i > 0u)
                MciSaveArchiveFree(&list[--i]);
            free(list);
            free(card);
            return rc;
        }
    }
    free(card);
    *out = list;
    *count = summary.save_count;
    return 0;
}

/* --------------------------------------------------------- dispatcher */

int MciSaveTransferImportDecoded(int target_port, const char *path,
                                 MciSaveTransferReport *report);

int MciSaveTransferImportDecoded(int target_port, const char *path,
                                 MciSaveTransferReport *report)
{
    unsigned char *data = NULL;
    u32 size = 0u;
    MciSaveArchive *list = NULL;
    u32 count = 0u;
    int generation = 0;
    MciSaveTransferFormat format;
    int rc;
    u32 i;

    rc = LoadSource(path, &data, &size);
    format = rc == 0 ? MciSaveTransferDetectBuffer(data, size, size, path, &generation)
                     : MCI_SAVE_FORMAT_UNKNOWN;
    MciSaveTransferResetReport(report, target_port, format);
    snprintf(report->source_path, sizeof(report->source_path), "%s", path);
    report->checksum_ok = -1;
    if (rc < 0) {
        report->result = rc == -101 ? MCI_SAVE_TRANSFER_INVALID_CONTAINER
                                    : MCI_SAVE_TRANSFER_IO_ERROR;
        return rc;
    }
    MciDiagLogPrintf("SAVE", "decoded import begin target=mc%d path=%s format=%s size=%u",
                     target_port, path, MciSaveTransferFormatName(format), size);
    MciDiagLogSetMassWritePaused(1);

    if (generation == 2) {
        MciPs2DecodeInfo info;
        list = (MciSaveArchive *)malloc(sizeof(*list));
        if (list == NULL) {
            rc = MCI_SAVE_E_NOMEM;
        } else {
            rc = MciPs2Decode(data, size, list, &info);
            count = rc == 0 ? 1u : 0u;
            if (info.checksum_present)
                report->checksum_ok = info.checksum_ok;
        }
    } else if (generation == 1) {
        if (format == MCI_SAVE_FORMAT_IMAGE_PS1 || format == MCI_SAVE_FORMAT_GME_PS1) {
            rc = DecodePs1Card(data, size, &list, &count);
        } else {
            list = (MciSaveArchive *)malloc(sizeof(*list));
            rc = list == NULL ? MCI_SAVE_E_NOMEM
                              : MciPs1DecodeSingle(data, size, path, list, NULL);
            count = rc == 0 ? 1u : 0u;
        }
    } else {
        rc = MCI_SAVE_E_FORMAT;
    }
    free(data);
    report->decode_rc = rc;

    if (rc < 0) {
        report->result = rc == MCI_SAVE_E_FORMAT ? MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT
                                                 : MCI_SAVE_TRANSFER_INVALID_CONTAINER;
        rc = rc == MCI_SAVE_E_FORMAT ? -300 : -200 + rc;
    } else {
        if (count == 1u && list[0].title[0] != '\0')
            snprintf(report->title, sizeof(report->title), "%s", list[0].title);
        rc = generation == 2 ? ImportPs2Archive(target_port, &list[0], report)
                             : ImportPs1Archives(target_port, list, count, report);
    }

    for (i = 0u; i < count; i++)
        MciSaveArchiveFree(&list[i]);
    free(list);
    MciDiagLogSetMassWritePaused(0);
    MciDiagLogPrintf("SAVE",
                     "decoded import end rc=%d result=%s decode=%s target=mc%d gen=%d saves=%d/%d files=%d/%d verified=%d bytes=%u crc=%d rollback=%d",
                     rc, MciSaveTransferResultText(report->result),
                     MciSaveCodecErrorText(report->decode_rc), target_port,
                     report->generation, report->saves_written, report->saves_total,
                     report->files_written, report->files_total,
                     report->files_verified, report->bytes_written,
                     report->checksum_ok, report->rollback_rc);
    return rc;
}

/* --------------------------------------------------------- conversion */

static int ReplaceExtension(const char *path, const char *ext, char *out,
                            unsigned int out_size)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    unsigned int base = (dot != NULL && (slash == NULL || dot > slash))
                            ? (unsigned int)(dot - path) : (unsigned int)strlen(path);
    int n;

    if (base + strlen(ext) + 1u > out_size)
        return -1;
    n = snprintf(out, out_size, "%.*s%s", (int)base, path, ext);
    return n > 0 && (unsigned int)n < out_size ? 0 : -1;
}

int MciSaveTransferConvertFile(const char *path, char *out_path,
                               unsigned int out_path_size,
                               MciSaveTransferReport *report)
{
    unsigned char *data = NULL;
    unsigned char *out = NULL;
    u32 size = 0u;
    u32 out_size = 0u;
    MciSaveArchive archive;
    int generation = 0;
    MciSaveTransferFormat format;
    const char *ext = NULL;
    int rc;

    if (path == NULL || out_path == NULL || report == NULL)
        return -1;
    out_path[0] = '\0';
    MciSaveArchiveInit(&archive);
    rc = LoadSource(path, &data, &size);
    format = rc == 0 ? MciSaveTransferDetectBuffer(data, size, size, path, &generation)
                     : MCI_SAVE_FORMAT_UNKNOWN;
    MciSaveTransferResetReport(report, -1, format);
    snprintf(report->source_path, sizeof(report->source_path), "%s", path);
    report->checksum_ok = -1;
    report->generation = generation;
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        return rc;
    }

    if (generation == 2) {
        MciPs2DecodeInfo info;
        rc = MciPs2Decode(data, size, &archive, &info);
        if (info.checksum_present)
            report->checksum_ok = info.checksum_ok;
        if (rc == 0) {
            out_size = MciPs2PsuSize(&archive);
            out = (unsigned char *)malloc(out_size);
            rc = out == NULL ? MCI_SAVE_E_NOMEM : MciPs2EncodePsu(&archive, out, out_size);
            ext = ".psu";
        }
    } else if (format == MCI_SAVE_FORMAT_IMAGE_PS1 || format == MCI_SAVE_FORMAT_GME_PS1) {
        MciPs1Card *card = (MciPs1Card *)malloc(sizeof(*card));
        rc = card == NULL ? MCI_SAVE_E_NOMEM : MciPs1CardLoad(card, data, size, NULL);
        if (rc == 0) {
            out_size = MCI_PS1_CARD_BYTES;
            out = (unsigned char *)malloc(out_size);
            rc = out == NULL ? MCI_SAVE_E_NOMEM
                             : MciPs1CardWrite(card, MCI_PS1_CONTAINER_RAW, out, out_size);
            ext = ".mcd";
        }
        free(card);
    } else if (generation == 1) {
        rc = MciPs1DecodeSingle(data, size, path, &archive, NULL);
        if (rc == 0) {
            out_size = MciPs1McsSize(&archive);
            out = (unsigned char *)malloc(out_size);
            rc = out == NULL ? MCI_SAVE_E_NOMEM : MciPs1EncodeMcs(&archive, out, out_size);
            ext = ".mcs";
        }
    } else {
        rc = MCI_SAVE_E_FORMAT;
    }
    free(data);
    report->decode_rc = rc < 0 ? rc : 0;
    if (archive.directory[0] != '\0')
        snprintf(report->save_directory, sizeof(report->save_directory), "%s",
                 archive.directory);
    if (archive.title[0] != '\0')
        snprintf(report->title, sizeof(report->title), "%s", archive.title);
    report->files_total = (int)archive.file_count;
    MciSaveArchiveFree(&archive);

    if (rc < 0) {
        free(out);
        report->result = rc == MCI_SAVE_E_FORMAT ? MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT
                                                 : MCI_SAVE_TRANSFER_INVALID_CONTAINER;
        return rc == MCI_SAVE_E_FORMAT ? -300 : -200 + rc;
    }
    if (ReplaceExtension(path, ext, out_path, out_path_size) < 0 ||
        strcmp(out_path, path) == 0) {
        /* Already in the target format: write alongside as *.conv<ext>. */
        if (ReplaceExtension(path, ".conv", out_path, out_path_size) < 0 ||
            strlen(out_path) + strlen(ext) >= out_path_size) {
            free(out);
            report->result = MCI_SAVE_TRANSFER_IO_ERROR;
            return -120;
        }
        strcat(out_path, ext);
    }
    {
        iox_stat_t st;
        if (fileXioGetStat(out_path, &st) >= 0) {
            free(out);
            snprintf(report->destination, sizeof(report->destination), "%s", out_path);
            report->result = MCI_SAVE_TRANSFER_TARGET_CONFLICT;
            return -121;
        }
    }
    snprintf(report->destination, sizeof(report->destination), "%s", out_path);
    rc = WriteVerifiedFile(out_path, out, out_size);
    free(out);
    if (rc < 0) {
        report->result = rc == -112 ? MCI_SAVE_TRANSFER_VERIFY_ERROR : MCI_SAVE_TRANSFER_IO_ERROR;
        return rc;
    }
    report->bytes_written = out_size;
    report->files_written = report->files_verified = 1;
    report->result = MCI_SAVE_TRANSFER_OK;
    MciDiagLogPrintf("SAVE", "convert ok %s -> %s bytes=%u", path, out_path, out_size);
    return 0;
}
