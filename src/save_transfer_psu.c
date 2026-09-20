/* SPDX-License-Identifier: MIT */
/*
 * Drebin single-save PSU import/export.
 *
 * PSU is intentionally the first writable container: it is an uncompressed
 * stream of native 512-byte PS2 directory entries plus file data padded to
 * 1024-byte boundaries. This implementation is based on the public format
 * description/behaviour used by uLaunchELF, mymc, CheatDevicePS2 and Apollo,
 * but is written around Drebin's fail-closed transaction rules.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_log.h"
#include "save_transfer.h"

#define PSU_ENTRY_SIZE 512u
#define PSU_CLUSTER_BYTES 1024u
#define PSU_MAX_ENTRIES 130
#define PSU_BUFFER_BYTES 16384u
#define PSU_MODE_FILE 0x0010u
#define PSU_MODE_DIR 0x0020u
#define PSU_MODE_EXISTS 0x8000u

#pragma pack(push, 1)
typedef struct MciPsuEntry {
    u16 mode;
    u16 unused;
    u32 length;
    sceMcStDateTime created;
    u32 cluster;
    u32 dir_entry;
    sceMcStDateTime modified;
    u32 attr;
    u32 unused2[7];
    char name[32];
    u8 unused3[416];
} MciPsuEntry;
#pragma pack(pop)

typedef char MciPsuEntrySizeCheck[(sizeof(MciPsuEntry) == PSU_ENTRY_SIZE) ? 1 : -1];

static unsigned char IoBuffer[PSU_BUFFER_BYTES] __attribute__((aligned(64)));
static unsigned char VerifyBuffer[PSU_BUFFER_BYTES] __attribute__((aligned(64)));

static int McResult(void)
{
    int result = -999;
    int rc = mcSync(MC_WAIT, NULL, &result);
    return rc < 0 ? rc : result;
}

static int SafeName(const char *name, int allow_dots)
{
    const unsigned char *p = (const unsigned char *)name;
    if (name == NULL || name[0] == '\0')
        return 0;
    if (!allow_dots && (strcmp(name, ".") == 0 || strcmp(name, "..") == 0))
        return 0;
    while (*p != '\0') {
        if (*p < 0x20u || *p == '/' || *p == '\\' || *p == ':' ||
            *p == '*' || *p == '?')
            return 0;
        p++;
    }
    return 1;
}

static int EntryName(const MciPsuEntry *entry, char out[33])
{
    unsigned int i;
    for (i = 0u; i < 32u; i++) {
        unsigned char c = (unsigned char)entry->name[i];
        if (c == 0u) {
            out[i] = '\0';
            return i > 0u ? 0 : -1;
        }
        out[i] = (char)c;
    }
    out[32] = '\0';
    return 0;
}

static u32 PaddedDataSize(u32 size)
{
    u32 rem = size % PSU_CLUSTER_BYTES;
    return rem == 0u ? size : size + (PSU_CLUSTER_BYTES - rem);
}

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

static int WriteExact(int fd, const void *buffer, unsigned int size)
{
    const unsigned char *p = (const unsigned char *)buffer;
    unsigned int done = 0u;
    while (done < size) {
        int rc = fileXioWrite(fd, p + done, (int)(size - done));
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += (unsigned int)rc;
    }
    return 0;
}

static int DeviceSync(const char *path)
{
    char device[16];
    const char *colon = path != NULL ? strchr(path, ':') : NULL;
    unsigned int len;
    if (colon == NULL)
        return -1;
    len = (unsigned int)(colon - path) + 1u;
    if (len >= sizeof(device))
        return -2;
    memcpy(device, path, len);
    device[len] = '\0';
    return fileXioSync(device, 0);
}

static int CardInfo(int port, int *free_clusters)
{
    int type = MC_TYPE_NONE;
    int free = -1;
    int formatted = 0;
    int rc;
    rc = mcGetInfo(port, 0, &type, &free, &formatted);
    if (rc < 0)
        return rc;
    rc = McResult();
    if (type != MC_TYPE_PS2 || !formatted || rc < -2)
        return rc < -2 ? rc : -1;
    if (free_clusters != NULL)
        *free_clusters = free;
    return 0;
}

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

static int SetMetadata(int port, const char *path, const MciPsuEntry *entry)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int rc;
    memset(&info, 0, sizeof(info));
    info._Create = entry->created;
    info._Modify = entry->modified;
    info.AttrFile = entry->mode;
    info.Reserve1 = entry->unused;
    info.Reserve2 = entry->attr;
    info.PdaAplNo = entry->unused2[0];
    rc = mcSetFileInfo(port, 0, path, &info,
                       sceMcFileInfoCreate | sceMcFileInfoModify |
                       sceMcFileInfoAttr);
    if (rc < 0)
        return rc;
    return McResult();
}

static void FromMcInfo(MciPsuEntry *entry, const sceMcTblGetDir *info)
{
    memset(entry, 0, sizeof(*entry));
    entry->mode = info->AttrFile;
    entry->unused = info->Reserve1;
    entry->length = info->FileSizeByte;
    entry->created = info->_Create;
    entry->modified = info->_Modify;
    entry->attr = info->Reserve2;
    entry->unused2[0] = info->PdaAplNo;
    memcpy(entry->name, info->EntryName, sizeof(entry->name));
}

static int JoinCardPath(const char *directory, const char *name,
                        char *out, unsigned int out_size)
{
    int n;
    if (name == NULL)
        n = snprintf(out, out_size, "/%s", directory);
    else
        n = snprintf(out, out_size, "/%s/%s", directory, name);
    return n >= 0 && (unsigned int)n < out_size ? 0 : -1;
}

static int ScanPsu(int fd, u64 file_size, MciPsuEntry *root,
                   int *file_count, int *required_clusters, u32 *total_bytes)
{
    MciPsuEntry entry;
    char name[33];
    int i;
    int files;
    int clusters;
    u32 bytes = 0u;
    s64 position;

    if (fileXioLseek64(fd, 0, SEEK_SET) < 0 || ReadExact(fd, root, sizeof(*root)) < 0)
        return -1;
    if (EntryName(root, name) < 0 || !SafeName(name, 0) ||
        (root->mode & PSU_MODE_DIR) == 0u || root->length < 2u ||
        root->length > PSU_MAX_ENTRIES)
        return -2;

    if (ReadExact(fd, &entry, sizeof(entry)) < 0 ||
        EntryName(&entry, name) < 0 || strcmp(name, ".") != 0)
        return -3;
    if (ReadExact(fd, &entry, sizeof(entry)) < 0 ||
        EntryName(&entry, name) < 0 || strcmp(name, "..") != 0)
        return -4;

    files = (int)root->length - 2;
    clusters = ((int)root->length + 1) / 2;
    for (i = 0; i < files; i++) {
        u32 padded;
        if (ReadExact(fd, &entry, sizeof(entry)) < 0)
            return -5;
        if (EntryName(&entry, name) < 0 || !SafeName(name, 0) ||
            (entry.mode & PSU_MODE_FILE) == 0u ||
            (entry.mode & PSU_MODE_DIR) != 0u)
            return -6;
        padded = PaddedDataSize(entry.length);
        position = fileXioLseek64(fd, (s64)padded, SEEK_CUR);
        if (position < 0 || (u64)position > file_size)
            return -7;
        if (entry.length > 0x7FFFFFFFu - bytes)
            return -8;
        bytes += entry.length;
        clusters += (int)((entry.length + PSU_CLUSTER_BYTES - 1u) /
                          PSU_CLUSTER_BYTES);
    }
    position = fileXioLseek64(fd, 0, SEEK_CUR);
    if (position < 0 || (u64)position != file_size)
        return -9;
    if (file_count != NULL) *file_count = files;
    if (required_clusters != NULL) *required_clusters = clusters;
    if (total_bytes != NULL) *total_bytes = bytes;
    return 0;
}

static int RecordCreated(char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX],
                         int *count, const char *path)
{
    if (*count >= PSU_MAX_ENTRIES)
        return -1;
    snprintf(created[*count], MCI_SAVE_TRANSFER_FAILED_PATH_MAX, "%s", path);
    (*count)++;
    return 0;
}

static int Rollback(int port,
                    char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX],
                    int count)
{
    int first_error = 0;
    int i;
    for (i = count - 1; i >= 0; i--) {
        int rc = mcDelete(port, 0, created[i]);
        if (rc < 0) {
            if (first_error == 0) first_error = rc;
            continue;
        }
        rc = McResult();
        if (rc < 0 && rc != sceMcResNoEntry && first_error == 0)
            first_error = rc;
    }
    return first_error;
}

static int WriteCardFileFromPsu(int source_fd, s64 data_offset,
                                const MciPsuEntry *entry, int port,
                                const char *target_path,
                                char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX],
                                int *created_count, MciSaveTransferReport *report)
{
    u32 remaining = entry->length;
    int fd;
    int rc;

    rc = mcOpen(port, 0, target_path, FIO_O_WRONLY | FIO_O_CREAT);
    if (rc < 0)
        return rc;
    fd = McResult();
    if (fd < 0)
        return fd;
    if (RecordCreated(created, created_count, target_path) < 0) {
        mcClose(fd); (void)McResult();
        return -20;
    }

    while (remaining > 0u) {
        unsigned int chunk = remaining > PSU_BUFFER_BYTES ? PSU_BUFFER_BYTES : remaining;
        rc = ReadExact(source_fd, IoBuffer, chunk);
        if (rc < 0)
            goto fail_open;
        rc = mcWrite(fd, IoBuffer, (int)chunk);
        if (rc < 0)
            goto fail_open;
        rc = McResult();
        if (rc != (int)chunk) {
            rc = rc < 0 ? rc : -21;
            goto fail_open;
        }
        report->bytes_written += chunk;
        remaining -= chunk;
    }
    rc = mcFlush(fd);
    if (rc >= 0)
        rc = McResult();
    mcClose(fd);
    if (McResult() < 0 && rc >= 0)
        rc = -22;
    if (rc < 0)
        return rc;

    /* Re-read both sources, byte-for-byte. */
    if (fileXioLseek64(source_fd, data_offset, SEEK_SET) != data_offset)
        return -23;
    rc = mcOpen(port, 0, target_path, FIO_O_RDONLY);
    if (rc < 0)
        return rc;
    fd = McResult();
    if (fd < 0)
        return fd;
    remaining = entry->length;
    while (remaining > 0u) {
        unsigned int chunk = remaining > PSU_BUFFER_BYTES ? PSU_BUFFER_BYTES : remaining;
        rc = ReadExact(source_fd, IoBuffer, chunk);
        if (rc < 0)
            goto fail_verify;
        rc = mcRead(fd, VerifyBuffer, (int)chunk);
        if (rc < 0)
            goto fail_verify;
        rc = McResult();
        if (rc != (int)chunk || memcmp(IoBuffer, VerifyBuffer, chunk) != 0) {
            rc = rc < 0 ? rc : -24;
            goto fail_verify;
        }
        remaining -= chunk;
    }
    rc = mcRead(fd, VerifyBuffer, 1);
    if (rc >= 0)
        rc = McResult();
    if (rc != 0) {
        rc = rc < 0 ? rc : -25;
        goto fail_verify;
    }
    mcClose(fd);
    rc = McResult();
    if (rc < 0)
        return rc;
    report->files_verified++;
    return SetMetadata(port, target_path, entry);

fail_verify:
    mcClose(fd); (void)McResult();
    return rc;
fail_open:
    mcClose(fd); (void)McResult();
    return rc;
}

const char *MciSaveTransferResultText(MciSaveTransferResult result)
{
    switch (result) {
        case MCI_SAVE_TRANSFER_OK: return "PASS";
        case MCI_SAVE_TRANSFER_INVALID_CONTAINER: return "INVALID CONTAINER";
        case MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT: return "UNSUPPORTED FORMAT";
        case MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE: return "TARGET UNAVAILABLE";
        case MCI_SAVE_TRANSFER_TARGET_CONFLICT: return "SAVE ALREADY EXISTS";
        case MCI_SAVE_TRANSFER_TARGET_FULL: return "TARGET FULL";
        case MCI_SAVE_TRANSFER_IO_ERROR: return "I/O ERROR";
        case MCI_SAVE_TRANSFER_VERIFY_ERROR: return "VERIFY ERROR";
        case MCI_SAVE_TRANSFER_ROLLBACK_FAILED: return "ROLLBACK FAILED";
        default: return "NOT RUN";
    }
}

void MciSaveTransferResetReport(MciSaveTransferReport *report, int card_port,
                                MciSaveTransferFormat format)
{
    if (report == NULL)
        return;
    memset(report, 0, sizeof(*report));
    report->result = MCI_SAVE_TRANSFER_NOT_RUN;
    report->format = format;
    report->card_port = card_port;
    report->target_free_clusters = -1;
}

int MciSaveTransferImportPsu(int target_port, const char *path,
                             MciSaveTransferReport *report)
{
    MciPsuEntry root;
    MciPsuEntry entry;
    iox_stat_t stat;
    char root_name[33];
    char target_dir[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    char target_file[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    char (*created)[MCI_SAVE_TRANSFER_FAILED_PATH_MAX] = NULL;
    int created_count = 0;
    int source_fd = -1;
    int file_count = 0;
    int required = 0;
    int free_clusters = -1;
    int i;
    int rc = -1;

    if (report == NULL || path == NULL)
        return -1;
    MciSaveTransferResetReport(report, target_port, MCI_SAVE_FORMAT_PSU);
    snprintf(report->source_path, sizeof(report->source_path), "%s", path);
    MciDiagLogPrintf("SAVE", "PSU import begin target=mc%d path=%s", target_port, path);
    MciDiagLogSetMassWritePaused(1);

    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(path, &stat) < 0 || !FIO_S_ISREG(stat.mode) || stat.size <= 0) {
        report->result = MCI_SAVE_TRANSFER_INVALID_CONTAINER;
        rc = -100;
        goto out;
    }
    source_fd = fileXioOpen(path, FIO_O_RDONLY);
    if (source_fd < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        rc = source_fd;
        goto out;
    }
    rc = ScanPsu(source_fd, (u64)stat.size, &root, &file_count, &required, NULL);
    if (rc < 0 || EntryName(&root, root_name) < 0) {
        report->result = MCI_SAVE_TRANSFER_INVALID_CONTAINER;
        goto out;
    }
    snprintf(report->save_directory, sizeof(report->save_directory), "%s", root_name);
    report->files_total = file_count;
    report->required_clusters = required;
    if (JoinCardPath(root_name, NULL, target_dir, sizeof(target_dir)) < 0) {
        report->result = MCI_SAVE_TRANSFER_INVALID_CONTAINER;
        rc = -101;
        goto out;
    }
    snprintf(report->destination, sizeof(report->destination), "mc%d:%s",
             target_port, target_dir);

    rc = CardInfo(target_port, &free_clusters);
    report->target_free_clusters = free_clusters;
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
        goto out;
    }
    rc = TargetExists(target_port, target_dir);
    if (rc != 0) {
        report->result = rc > 0 ? MCI_SAVE_TRANSFER_TARGET_CONFLICT
                                : MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
        if (rc > 0) rc = -102;
        goto out;
    }
    if (required > free_clusters) {
        report->result = MCI_SAVE_TRANSFER_TARGET_FULL;
        rc = -103;
        goto out;
    }

    created = malloc(PSU_MAX_ENTRIES * MCI_SAVE_TRANSFER_FAILED_PATH_MAX);
    if (created == NULL) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        rc = -104;
        goto out;
    }
    rc = mcMkDir(target_port, 0, target_dir);
    if (rc < 0 || (rc = McResult()) < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        goto rollback;
    }
    RecordCreated(created, &created_count, target_dir);

    if (fileXioLseek64(source_fd, (s64)(PSU_ENTRY_SIZE * 3u), SEEK_SET) !=
        (s64)(PSU_ENTRY_SIZE * 3u)) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        rc = -105;
        goto rollback;
    }
    for (i = 0; i < file_count; i++) {
        char name[33];
        s64 data_offset;
        s64 next_offset;
        u32 padded;
        if (ReadExact(source_fd, &entry, sizeof(entry)) < 0 ||
            EntryName(&entry, name) < 0 || !SafeName(name, 0)) {
            report->result = MCI_SAVE_TRANSFER_INVALID_CONTAINER;
            rc = -106;
            goto rollback;
        }
        if (JoinCardPath(root_name, name, target_file, sizeof(target_file)) < 0) {
            report->result = MCI_SAVE_TRANSFER_INVALID_CONTAINER;
            rc = -107;
            goto rollback;
        }
        snprintf(report->failed_path, sizeof(report->failed_path), "%s", target_file);
        data_offset = fileXioLseek64(source_fd, 0, SEEK_CUR);
        if (data_offset < 0) {
            report->result = MCI_SAVE_TRANSFER_IO_ERROR;
            rc = (int)data_offset;
            goto rollback;
        }
        rc = WriteCardFileFromPsu(source_fd, data_offset, &entry, target_port,
                                  target_file, created, &created_count, report);
        if (rc < 0) {
            report->result = (rc == -24 || rc == -25)
                                 ? MCI_SAVE_TRANSFER_VERIFY_ERROR
                                 : MCI_SAVE_TRANSFER_IO_ERROR;
            goto rollback;
        }
        report->files_written++;
        padded = PaddedDataSize(entry.length);
        next_offset = data_offset + (s64)padded;
        if (fileXioLseek64(source_fd, next_offset, SEEK_SET) != next_offset) {
            report->result = MCI_SAVE_TRANSFER_IO_ERROR;
            rc = -108;
            goto rollback;
        }
    }
    rc = SetMetadata(target_port, target_dir, &root);
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        goto rollback;
    }
    report->failed_path[0] = '\0';
    report->result = MCI_SAVE_TRANSFER_OK;
    rc = 0;
    goto out;

rollback:
    report->rollback_rc = Rollback(target_port, created, created_count);
    if (report->rollback_rc < 0)
        report->result = MCI_SAVE_TRANSFER_ROLLBACK_FAILED;
out:
    if (source_fd >= 0)
        fileXioClose(source_fd);
    free(created);
    MciDiagLogSetMassWritePaused(0);
    MciDiagLogPrintf("SAVE",
                     "PSU import end rc=%d result=%s target=mc%d dir=%s files=%d/%d verified=%d bytes=%u rollback=%d",
                     rc, MciSaveTransferResultText(report->result), target_port,
                     report->save_directory[0] ? report->save_directory : "n/a",
                     report->files_written, report->files_total,
                     report->files_verified, report->bytes_written,
                     report->rollback_rc);
    return rc;
}

static int ReadCardFileToPsu(int port, const char *path, u32 size, int out_fd)
{
    u32 remaining = size;
    int fd;
    int rc = mcOpen(port, 0, path, FIO_O_RDONLY);
    if (rc < 0)
        return rc;
    fd = McResult();
    if (fd < 0)
        return fd;
    while (remaining > 0u) {
        unsigned int chunk = remaining > PSU_BUFFER_BYTES ? PSU_BUFFER_BYTES : remaining;
        rc = mcRead(fd, IoBuffer, (int)chunk);
        if (rc < 0)
            goto out;
        rc = McResult();
        if (rc != (int)chunk) {
            rc = rc < 0 ? rc : -201;
            goto out;
        }
        rc = WriteExact(out_fd, IoBuffer, chunk);
        if (rc < 0)
            goto out;
        remaining -= chunk;
    }
    rc = 0;
out:
    mcClose(fd); (void)McResult();
    return rc;
}

static int VerifyPsuFileAgainstCard(int port, const char *path,
                                    const char *save_directory,
                                    MciSaveTransferReport *report)
{
    iox_stat_t stat;
    MciPsuEntry root;
    MciPsuEntry entry;
    int fd = -1;
    int file_count = 0;
    int i;
    int rc;

    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(path, &stat) < 0 || stat.size <= 0)
        return -1;
    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;
    rc = ScanPsu(fd, (u64)stat.size, &root, &file_count, NULL, NULL);
    if (rc < 0)
        goto out;
    if (fileXioLseek64(fd, (s64)(PSU_ENTRY_SIZE * 3u), SEEK_SET) !=
        (s64)(PSU_ENTRY_SIZE * 3u)) {
        rc = -2;
        goto out;
    }
    for (i = 0; i < file_count; i++) {
        char name[33];
        char card_path[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
        int mcfd;
        u32 remaining;
        s64 data_offset;
        u32 padded;
        if (ReadExact(fd, &entry, sizeof(entry)) < 0 || EntryName(&entry, name) < 0) {
            rc = -3;
            goto out;
        }
        if (JoinCardPath(save_directory, name, card_path, sizeof(card_path)) < 0) {
            rc = -4;
            goto out;
        }
        data_offset = fileXioLseek64(fd, 0, SEEK_CUR);
        if (data_offset < 0) { rc = (int)data_offset; goto out; }
        rc = mcOpen(port, 0, card_path, FIO_O_RDONLY);
        if (rc < 0) goto out;
        mcfd = McResult();
        if (mcfd < 0) { rc = mcfd; goto out; }
        remaining = entry.length;
        while (remaining > 0u) {
            unsigned int chunk = remaining > PSU_BUFFER_BYTES ? PSU_BUFFER_BYTES : remaining;
            rc = ReadExact(fd, IoBuffer, chunk);
            if (rc < 0) break;
            rc = mcRead(mcfd, VerifyBuffer, (int)chunk);
            if (rc < 0) break;
            rc = McResult();
            if (rc != (int)chunk || memcmp(IoBuffer, VerifyBuffer, chunk) != 0) {
                rc = rc < 0 ? rc : -5;
                break;
            }
            remaining -= chunk;
        }
        mcClose(mcfd); (void)McResult();
        if (rc < 0)
            goto out;
        report->files_verified++;
        padded = PaddedDataSize(entry.length);
        if (fileXioLseek64(fd, data_offset + (s64)padded, SEEK_SET) !=
            data_offset + (s64)padded) {
            rc = -6;
            goto out;
        }
    }
    rc = 0;
out:
    if (fd >= 0) fileXioClose(fd);
    return rc;
}

int MciSaveTransferExportPsu(int source_port, const char *save_directory,
                             const char *path, MciSaveTransferReport *report)
{
    sceMcTblGetDir root_info __attribute__((aligned(64)));
    sceMcTblGetDir *entries = NULL;
    MciPsuEntry root_entry;
    MciPsuEntry entry;
    char root_path[96];
    char wildcard[104];
    char file_path[MCI_SAVE_TRANSFER_FAILED_PATH_MAX];
    unsigned char padding[PSU_CLUSTER_BYTES];
    int out_fd = -1;
    int free_clusters;
    int count = -999;
    int dot = -1;
    int dotdot = -1;
    int files = 0;
    int i;
    int rc = -1;

    if (report == NULL || save_directory == NULL || path == NULL ||
        !SafeName(save_directory, 0))
        return -1;
    MciSaveTransferResetReport(report, source_port, MCI_SAVE_FORMAT_PSU);
    snprintf(report->save_directory, sizeof(report->save_directory), "%s", save_directory);
    snprintf(report->source_path, sizeof(report->source_path), "mc%d:/%s",
             source_port, save_directory);
    snprintf(report->destination, sizeof(report->destination), "%s", path);
    MciDiagLogPrintf("SAVE", "PSU export begin source=mc%d:/%s path=%s",
                     source_port, save_directory, path);
    MciDiagLogSetMassWritePaused(1);

    if (CardInfo(source_port, &free_clusters) < 0) {
        report->result = MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
        rc = -200;
        goto out;
    }
    if (JoinCardPath(save_directory, NULL, root_path, sizeof(root_path)) < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        rc = -201;
        goto out;
    }
    memset(&root_info, 0, sizeof(root_info));
    rc = mcGetDir(source_port, 0, root_path, 0, 1, &root_info);
    if (rc < 0 || (rc = McResult()) <= 0) {
        report->result = MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
        if (rc == 0) rc = -202;
        goto out;
    }

    entries = memalign(64, sizeof(sceMcTblGetDir) * PSU_MAX_ENTRIES);
    if (entries == NULL) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        rc = -203;
        goto out;
    }
    memset(entries, 0, sizeof(sceMcTblGetDir) * PSU_MAX_ENTRIES);
    snprintf(wildcard, sizeof(wildcard), "/%s/*", save_directory);
    rc = mcGetDir(source_port, 0, wildcard, 0, PSU_MAX_ENTRIES, entries);
    if (rc < 0 || (count = McResult()) < 2 || count >= PSU_MAX_ENTRIES) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        rc = count >= PSU_MAX_ENTRIES ? -204 : rc;
        goto out;
    }
    for (i = 0; i < count; i++) {
        entries[i].EntryName[sizeof(entries[i].EntryName) - 1] = '\0';
        if (strcmp((char *)entries[i].EntryName, ".") == 0)
            dot = i;
        else if (strcmp((char *)entries[i].EntryName, "..") == 0)
            dotdot = i;
        else if (entries[i].AttrFile & sceMcFileAttrFile)
            files++;
        else {
            /* Classic PSU is a flat single-save container. Never silently drop
             * a nested object just to produce a superficially valid archive. */
            report->result = MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT;
            rc = -205;
            goto out;
        }
    }
    if (dot < 0 || dotdot < 0 || files != count - 2) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        rc = -206;
        goto out;
    }

    out_fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    if (out_fd < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        rc = out_fd;
        goto out;
    }
    FromMcInfo(&root_entry, &root_info);
    root_entry.length = (u32)count;
    if (WriteExact(out_fd, &root_entry, sizeof(root_entry)) < 0) {
        rc = -207; goto fail_output;
    }
    FromMcInfo(&entry, &entries[dot]);
    if (WriteExact(out_fd, &entry, sizeof(entry)) < 0) { rc = -208; goto fail_output; }
    FromMcInfo(&entry, &entries[dotdot]);
    if (WriteExact(out_fd, &entry, sizeof(entry)) < 0) { rc = -209; goto fail_output; }

    memset(padding, 0xFF, sizeof(padding));
    report->files_total = files;
    for (i = 0; i < count; i++) {
        u32 pad;
        const char *name = (const char *)entries[i].EntryName;
        if (!(entries[i].AttrFile & sceMcFileAttrFile))
            continue;
        FromMcInfo(&entry, &entries[i]);
        if (!SafeName(name, 0) || JoinCardPath(save_directory, name,
                                               file_path, sizeof(file_path)) < 0) {
            rc = -210; goto fail_output;
        }
        snprintf(report->failed_path, sizeof(report->failed_path), "%s", file_path);
        if (WriteExact(out_fd, &entry, sizeof(entry)) < 0) { rc = -211; goto fail_output; }
        rc = ReadCardFileToPsu(source_port, file_path, entry.length, out_fd);
        if (rc < 0) goto fail_output;
        report->bytes_written += entry.length;
        pad = PaddedDataSize(entry.length) - entry.length;
        if (pad > 0u && WriteExact(out_fd, padding, pad) < 0) { rc = -212; goto fail_output; }
        report->files_written++;
    }
    fileXioClose(out_fd);
    out_fd = -1;
    rc = DeviceSync(path);
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_IO_ERROR;
        goto fail_closed;
    }
    rc = VerifyPsuFileAgainstCard(source_port, path, save_directory, report);
    if (rc < 0) {
        report->result = MCI_SAVE_TRANSFER_VERIFY_ERROR;
        goto fail_closed;
    }
    report->failed_path[0] = '\0';
    report->result = MCI_SAVE_TRANSFER_OK;
    rc = 0;
    goto out;

fail_output:
    report->result = MCI_SAVE_TRANSFER_IO_ERROR;
    if (out_fd >= 0) {
        fileXioClose(out_fd);
        out_fd = -1;
    }
fail_closed:
    fileXioRemove(path);
out:
    if (out_fd >= 0)
        fileXioClose(out_fd);
    free(entries);
    MciDiagLogSetMassWritePaused(0);
    MciDiagLogPrintf("SAVE",
                     "PSU export end rc=%d result=%s source=mc%d:/%s path=%s files=%d/%d verified=%d bytes=%u",
                     rc, MciSaveTransferResultText(report->result), source_port,
                     save_directory, path, report->files_written,
                     report->files_total, report->files_verified,
                     report->bytes_written);
    return rc;
}

int MciSaveTransferImportFile(int target_port, const char *path,
                              MciSaveTransferReport *report)
{
    MciSaveTransferFormat format = MciSaveTransferFormatFromPath(path);
    if (format == MCI_SAVE_FORMAT_PSU)
        return MciSaveTransferImportPsu(target_port, path, report);
    MciSaveTransferResetReport(report, target_port, format);
    if (report != NULL) {
        report->result = MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT;
        if (path != NULL)
            snprintf(report->source_path, sizeof(report->source_path), "%s", path);
    }
    return -300;
}
