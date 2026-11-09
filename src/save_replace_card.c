/* SPDX-License-Identifier: MIT */
/*
 * Replace one file inside an existing PS2 save. See save_replace.h for the
 * sequence and its failure guarantees.
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
#include "save_replace_card.h"
#include "save_transfer.h"

#define RP_CHUNK 16384u
#define RP_MAX_FILES 64

static unsigned char RpBuf[RP_CHUNK] __attribute__((aligned(64)));
static sceMcTblGetDir RpTable[RP_MAX_FILES] __attribute__((aligned(64)));

static int McResult(void)
{
    int result = -999;
    int rc = mcSync(MC_WAIT, NULL, &result);
    return rc < 0 ? rc : result;
}

const char *MciReplaceStageText(MciReplaceStage stage)
{
    switch (stage) {
        case MCI_REPLACE_STAGE_PREFLIGHT: return "preflight";
        case MCI_REPLACE_STAGE_BACKUP: return "PSU backup";
        case MCI_REPLACE_STAGE_TEMP_WRITE: return "temporary copy";
        case MCI_REPLACE_STAGE_DELETE_ORIGINAL: return "delete original";
        case MCI_REPLACE_STAGE_RENAME: return "rename";
        case MCI_REPLACE_STAGE_METADATA: return "restore metadata";
        case MCI_REPLACE_STAGE_FINAL_VERIFY: return "final verify";
        case MCI_REPLACE_STAGE_DONE: return "done";
        default: return "none";
    }
}

static int ListDir(int port, const char *save_directory)
{
    char pattern[48];
    int rc;

    snprintf(pattern, sizeof(pattern), "/%s/*", save_directory);
    memset(RpTable, 0, sizeof(RpTable));
    rc = mcGetDir(port, 0, pattern, 0, RP_MAX_FILES, RpTable);
    if (rc < 0)
        return rc;
    return McResult();
}

int MciSaveListFiles(int port, const char *save_directory,
                     char names[][33], u32 *sizes, int max)
{
    int n = ListDir(port, save_directory);
    int out = 0;
    int i;

    if (n < 0)
        return n;
    for (i = 0; i < n && out < max; i++) {
        const char *nm = (const char *)RpTable[i].EntryName;
        if ((RpTable[i].AttrFile & MC_ATTR_SUBDIR) || strcmp(nm, ".") == 0 ||
            strcmp(nm, "..") == 0)
            continue;
        snprintf(names[out], 33, "%s", nm);
        if (sizes != NULL)
            sizes[out] = RpTable[i].FileSizeByte;
        out++;
    }
    return out;
}

static int LoadUsb(const char *path, unsigned char **out, u32 *size)
{
    iox_stat_t st;
    unsigned char *buf;
    u32 done = 0u;
    int fd;

    *out = NULL;
    memset(&st, 0, sizeof(st));
    if (fileXioGetStat(path, &st) < 0 || !FIO_S_ISREG(st.mode) || st.size < 0 ||
        (u64)st.size > MCI_SAVE_MAX_BYTES)
        return -100;
    buf = (unsigned char *)malloc(st.size > 0 ? (u32)st.size : 1u);
    if (buf == NULL)
        return -101;
    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0) {
        free(buf);
        return fd;
    }
    while (done < (u32)st.size) {
        int r = fileXioRead(fd, buf + done, (int)((u32)st.size - done));
        if (r <= 0) {
            fileXioClose(fd);
            free(buf);
            return r < 0 ? r : -102;
        }
        done += (u32)r;
    }
    fileXioClose(fd);
    *out = buf;
    *size = done;
    return 0;
}

static int WriteVerify(int port, const char *path, const unsigned char *data, u32 size)
{
    u32 done;
    int fd;
    int rc;

    rc = mcOpen(port, 0, path, FIO_O_WRONLY | FIO_O_CREAT);
    if (rc >= 0)
        rc = McResult();
    if (rc < 0)
        return rc;
    fd = rc;
    for (done = 0u; done < size;) {
        u32 chunk = size - done > RP_CHUNK ? RP_CHUNK : size - done;
        memcpy(RpBuf, data + done, chunk);
        rc = mcWrite(fd, RpBuf, (int)chunk);
        if (rc >= 0)
            rc = McResult();
        if (rc != (int)chunk) {
            mcClose(fd); (void)McResult();
            return rc < 0 ? rc : -30;
        }
        done += chunk;
    }
    rc = mcFlush(fd);
    if (rc >= 0)
        rc = McResult();
    mcClose(fd);
    if (McResult() < 0 && rc >= 0)
        rc = -31;
    if (rc < 0)
        return rc;

    rc = mcOpen(port, 0, path, FIO_O_RDONLY);
    if (rc >= 0)
        rc = McResult();
    if (rc < 0)
        return rc;
    fd = rc;
    for (done = 0u; done < size;) {
        u32 chunk = size - done > RP_CHUNK ? RP_CHUNK : size - done;
        rc = mcRead(fd, RpBuf, (int)chunk);
        if (rc >= 0)
            rc = McResult();
        if (rc != (int)chunk || memcmp(RpBuf, data + done, chunk) != 0) {
            mcClose(fd); (void)McResult();
            return rc < 0 ? rc : -32;
        }
        done += chunk;
    }
    rc = mcRead(fd, RpBuf, 1);
    if (rc >= 0)
        rc = McResult();
    mcClose(fd);
    (void)McResult();
    return rc == 0 ? 0 : (rc < 0 ? rc : -33);
}

static int Delete(int port, const char *path)
{
    int rc = mcDelete(port, 0, path);
    if (rc >= 0)
        rc = McResult();
    return rc;
}

int MciSaveReplaceFile(int port, const char *save_directory, const char *file_name,
                       const char *usb_path, const char *backup_path,
                       MciReplaceReport *report)
{
    const char *existing[RP_MAX_FILES];
    char names[RP_MAX_FILES][33];
    char final_path[80];
    char temp_path[80];
    sceMcTblGetDir original __attribute__((aligned(64)));
    MciSaveTransferReport xr;
    unsigned char *data = NULL;
    u32 size = 0u;
    int found = -1;
    int n;
    int i;
    int rc;

    if (report == NULL || save_directory == NULL || file_name == NULL ||
        usb_path == NULL || backup_path == NULL)
        return -1;
    memset(report, 0, sizeof(*report));
    report->original_intact = 1;
    report->failed_stage = MCI_REPLACE_STAGE_PREFLIGHT;
    snprintf(report->save_directory, sizeof(report->save_directory), "%s", save_directory);
    snprintf(report->file_name, sizeof(report->file_name), "%s", file_name);
    snprintf(report->source_path, sizeof(report->source_path), "%s", usb_path);
    snprintf(report->backup_path, sizeof(report->backup_path), "%s", backup_path);
    MciDiagLogPrintf("REPLACE", "begin mc%d:/%s/%s <- %s", port, save_directory,
                     file_name, usb_path);

    rc = MciNormalCardProbeFormattedType(port, MC_TYPE_PS2, &report->free_clusters);
    if (rc < 0)
        goto fail;
    n = ListDir(port, save_directory);
    if (n < 0) {
        rc = n;
        goto fail;
    }
    for (i = 0; i < n; i++) {
        snprintf(names[i], 33, "%s", (const char *)RpTable[i].EntryName);
        existing[i] = names[i];
        if (strcmp(names[i], file_name) == 0 && !(RpTable[i].AttrFile & MC_ATTR_SUBDIR)) {
            found = i;
            memcpy(&original, &RpTable[i], sizeof(original));
        }
    }
    if (found < 0) {
        rc = -40;
        goto fail;
    }
    rc = LoadUsb(usb_path, &data, &size);
    if (rc < 0)
        goto fail;
    rc = MciReplacePlanMake(file_name, original.FileSizeByte, size, data, size,
                            existing, (u32)n, report->free_clusters, &report->plan);
    if (rc < 0)
        goto fail;
    snprintf(final_path, sizeof(final_path), "/%s/%s", save_directory, file_name);
    snprintf(temp_path, sizeof(temp_path), "/%s/%s", save_directory, report->plan.temp_name);

    /* 1. Verified backup of the whole save. Nothing proceeds without it. */
    report->failed_stage = MCI_REPLACE_STAGE_BACKUP;
    rc = MciSaveTransferExportPsu(port, save_directory, backup_path, &xr);
    if (rc < 0 || xr.result != MCI_SAVE_TRANSFER_OK) {
        if (rc >= 0)
            rc = -41;
        goto fail;
    }

    MciDiagLogSetMassWritePaused(1);
    /* 2. Temporary copy, verified. The save is still untouched. */
    report->failed_stage = MCI_REPLACE_STAGE_TEMP_WRITE;
    rc = WriteVerify(port, temp_path, data, size);
    if (rc < 0) {
        (void)Delete(port, temp_path);
        goto fail_paused;
    }

    /* 3. Remove the original. From here the backup + temp are the safety net. */
    report->failed_stage = MCI_REPLACE_STAGE_DELETE_ORIGINAL;
    rc = Delete(port, final_path);
    if (rc < 0) {
        (void)Delete(port, temp_path);
        goto fail_paused;
    }
    report->original_intact = 0;
    report->temp_left_on_card = 1;

    /* 4. Rename, or rewrite from RAM where rename is unsupported. */
    report->failed_stage = MCI_REPLACE_STAGE_RENAME;
    /* mcman renames through SetFileInfo with the bare new name and the
     * sceMcFileAttrFile (0x10) flag; that is what mcRename forwards to. */
    {
        sceMcTblGetDir info __attribute__((aligned(64)));
        memset(&info, 0, sizeof(info));
        snprintf((char *)info.EntryName, sizeof(info.EntryName), "%s", file_name);
        rc = mcSetFileInfo(port, 0, temp_path, &info, sceMcFileAttrFile);
        if (rc >= 0)
            rc = McResult();
    }
    if (rc >= 0) {
        /* Trust the rename only if the final name now opens. */
        int fd = mcOpen(port, 0, final_path, FIO_O_RDONLY);
        if (fd >= 0)
            fd = McResult();
        if (fd >= 0) {
            mcClose(fd);
            (void)McResult();
        } else {
            rc = fd;
        }
    }
    if (rc >= 0) {
        report->used_rename = 1;
        report->temp_left_on_card = 0;
    } else {
        rc = WriteVerify(port, final_path, data, size);
        if (rc < 0)
            goto fail_paused;
        if (Delete(port, temp_path) >= 0)
            report->temp_left_on_card = 0;
    }

    /* 5. Keep the original's attributes and timestamps. */
    report->failed_stage = MCI_REPLACE_STAGE_METADATA;
    {
        sceMcTblGetDir info __attribute__((aligned(64)));
        memset(&info, 0, sizeof(info));
        info._Create = original._Create;
        info._Modify = original._Modify;
        info.AttrFile = original.AttrFile;
        rc = mcSetFileInfo(port, 0, final_path, &info,
                           sceMcFileInfoCreate | sceMcFileInfoModify | sceMcFileInfoAttr);
        if (rc >= 0)
            rc = McResult();
        if (rc < 0)
            goto fail_paused;
    }

    /* 6. Read the final file back once more. */
    report->failed_stage = MCI_REPLACE_STAGE_FINAL_VERIFY;
    {
        int fd;
        u32 done;
        rc = mcOpen(port, 0, final_path, FIO_O_RDONLY);
        if (rc >= 0)
            rc = McResult();
        if (rc < 0)
            goto fail_paused;
        fd = rc;
        for (done = 0u; done < size && rc >= 0;) {
            u32 chunk = size - done > RP_CHUNK ? RP_CHUNK : size - done;
            rc = mcRead(fd, RpBuf, (int)chunk);
            if (rc >= 0)
                rc = McResult();
            if (rc != (int)chunk || memcmp(RpBuf, data + done, chunk) != 0)
                rc = rc < 0 ? rc : -34;
            done += chunk;
        }
        mcClose(fd);
        (void)McResult();
        if (rc < 0)
            goto fail_paused;
    }

    MciDiagLogSetMassWritePaused(0);
    free(data);
    report->failed_stage = MCI_REPLACE_STAGE_DONE;
    report->ok = 1;
    report->rc = 0;
    MciDiagLogPrintf("REPLACE", "ok mc%d:/%s/%s bytes=%u rename=%d backup=%s", port,
                     save_directory, file_name, size, report->used_rename, backup_path);
    return 0;

fail_paused:
    MciDiagLogSetMassWritePaused(0);
fail:
    free(data);
    report->rc = rc;
    MciDiagLogPrintf("REPLACE", "fail stage=%s rc=%d original_intact=%d temp_left=%d backup=%s",
                     MciReplaceStageText(report->failed_stage), rc,
                     report->original_intact, report->temp_left_on_card, backup_path);
    return rc < 0 ? rc : -1;
}
