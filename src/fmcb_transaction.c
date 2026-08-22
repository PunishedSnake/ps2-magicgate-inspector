/* SPDX-License-Identifier: MIT */
/*
 * Verified FreeMcBoot normal-install transaction.
 *
 * This deliberately does not implement multi-install/cross-model page-linking.
 * It installs the normal package manifest resolved by fmcb_install.c and treats
 * every target as a transaction participant: capture previous contents, load
 * source, bind KELF in RAM when required, write/flush/close, reopen and compare
 * the complete file, then retain rollback state until the whole set commits.
 *
 * Power-loss atomicity is not something the ROM X filesystem can magically
 * conjure up. Runtime failures are rolled back in-process; a later milestone
 * can add a persistent USB recovery journal for interrupted power scenarios.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fmcb_transaction.h"
#include "progress.h"

#define TX_CHUNK 4096u
#define TX_SPECIAL_SYSTEM_ATTR \
    (MC_ATTR_READABLE | MC_ATTR_WRITEABLE | MC_ATTR_EXECUTABLE | \
     MC_ATTR_PROTECTED | MC_ATTR_SUBDIR | sceMcFile0400)

typedef struct BackupState {
    unsigned char *data;
    unsigned int size;
    int existed;
    int valid;
} BackupState;

static int McResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static int CloseCardFile(int fd)
{
    mcClose(fd);
    return McResult();
}

static void TxProgress(int percent, const char *action, const char *detail)
{
    MciProgressUpdate(MCI_PROGRESS_FMCB, percent, action, detail);
}

void FmcbInstallResetReport(FmcbInstallReport *report, int target_port)
{
    int i;

    memset(report, 0, sizeof(*report));
    report->target_port = target_port;
    report->stage = FMCB_INSTALL_NOT_RUN;
    report->result = FMCB_INSTALL_RESULT_NOT_RUN;
    report->current_file = -1;
    report->rollback_rc = 0;
    for (i = 0; i < FMCB_TX_MAX_FILES; i++) {
        report->files[i].backup_rc = -999;
        report->files[i].bind_rc = -999;
        report->files[i].write_rc = -999;
        report->files[i].verify_rc = -999;
    }
}

static int EnsureDirectory(int port, const char *path, int special,
                           int *created)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int rc;

    *created = 0;
    memset(&info, 0, sizeof(info));
    mcGetDir(port, 0, path, 0, 1, &info);
    rc = McResult();
    if (rc < 0 && rc != sceMcResNoEntry)
        return rc;
    if (rc <= 0) {
        mcMkDir(port, 0, path);
        rc = McResult();
        if (rc < 0)
            return rc;
        *created = 1;
    }

    if (special) {
        memset(&info, 0, sizeof(info));
        info.AttrFile = TX_SPECIAL_SYSTEM_ATTR;
        mcSetFileInfo(port, 0, path, &info, sceMcFileInfoAttr);
        rc = McResult();
        if (rc < 0)
            return rc;
    }
    return 0;
}

static int ReadMassFile(const char *path, unsigned char **data,
                        unsigned int *size)
{
    iox_stat_t stat;
    unsigned char *buffer;
    unsigned int total;
    int fd;
    int rc;

    *data = NULL;
    *size = 0;
    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    if (rc < 0 || stat.size == 0)
        return rc < 0 ? rc : -4100;
    if (stat.size > 4u * 1024u * 1024u)
        return -4101;

    buffer = memalign(64, stat.size);
    if (buffer == NULL)
        return -4102;
    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0) {
        free(buffer);
        return fd;
    }

    total = 0;
    while (total < stat.size) {
        unsigned int chunk = stat.size - total;
        if (chunk > TX_CHUNK)
            chunk = TX_CHUNK;
        rc = fileXioRead(fd, buffer + total, chunk);
        if (rc <= 0) {
            fileXioClose(fd);
            free(buffer);
            return rc < 0 ? rc : -4103;
        }
        total += (unsigned int)rc;
    }
    fileXioClose(fd);
    *data = buffer;
    *size = stat.size;
    return 0;
}

static int CaptureTarget(int port, const char *path, BackupState *backup)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    unsigned char *buffer;
    unsigned int total;
    int fd;
    int rc;

    memset(backup, 0, sizeof(*backup));
    memset(&info, 0, sizeof(info));
    mcGetDir(port, 0, path, 0, 1, &info);
    rc = McResult();
    if (rc < 0 && rc != sceMcResNoEntry)
        return rc;
    if (rc <= 0) {
        backup->valid = 1;
        return 0;
    }

    backup->existed = 1;
    backup->size = info.FileSizeByte;
    if (backup->size == 0) {
        backup->valid = 1;
        return 0;
    }
    buffer = memalign(64, backup->size);
    if (buffer == NULL)
        return -4200;

    mcOpen(port, 0, path, FIO_O_RDONLY);
    fd = McResult();
    if (fd < 0) {
        free(buffer);
        return fd;
    }
    total = 0;
    while (total < backup->size) {
        unsigned int chunk = backup->size - total;
        if (chunk > TX_CHUNK)
            chunk = TX_CHUNK;
        mcRead(fd, buffer + total, chunk);
        rc = McResult();
        if (rc <= 0) {
            CloseCardFile(fd);
            free(buffer);
            return rc < 0 ? rc : -4201;
        }
        total += (unsigned int)rc;
    }
    rc = CloseCardFile(fd);
    if (rc < 0) {
        free(buffer);
        return rc;
    }
    backup->data = buffer;
    backup->valid = 1;
    return 0;
}

static int DeleteTarget(int port, const char *path)
{
    int rc;
    mcDelete(port, 0, path);
    rc = McResult();
    if (rc == sceMcResNoEntry)
        return 0;
    return rc;
}

static int WriteCardFile(int port, const char *path,
                         const unsigned char *data, unsigned int size)
{
    unsigned int total;
    int fd;
    int rc;

    rc = DeleteTarget(port, path);
    if (rc < 0)
        return rc;

    mcOpen(port, 0, path, FIO_O_WRONLY | FIO_O_CREAT);
    fd = McResult();
    if (fd < 0)
        return fd;

    total = 0;
    while (total < size) {
        unsigned int chunk = size - total;
        if (chunk > TX_CHUNK)
            chunk = TX_CHUNK;
        mcWrite(fd, data + total, chunk);
        rc = McResult();
        if (rc != (int)chunk) {
            CloseCardFile(fd);
            return rc < 0 ? rc : -4300;
        }
        total += chunk;
    }

    mcFlush(fd);
    rc = McResult();
    if (rc < 0) {
        CloseCardFile(fd);
        return rc;
    }
    return CloseCardFile(fd);
}

static int VerifyCardFile(int port, const char *path,
                          const unsigned char *expected, unsigned int size)
{
    unsigned char buffer[TX_CHUNK] __attribute__((aligned(64)));
    unsigned int total;
    int fd;
    int rc;

    mcOpen(port, 0, path, FIO_O_RDONLY);
    fd = McResult();
    if (fd < 0)
        return fd;

    total = 0;
    while (total < size) {
        unsigned int chunk = size - total;
        if (chunk > TX_CHUNK)
            chunk = TX_CHUNK;
        mcRead(fd, buffer, chunk);
        rc = McResult();
        if (rc != (int)chunk) {
            CloseCardFile(fd);
            return rc < 0 ? rc : -4400;
        }
        if (memcmp(buffer, expected + total, chunk) != 0) {
            CloseCardFile(fd);
            return -4401;
        }
        total += chunk;
    }

    /* An extra byte catches stale tail data if a replacement somehow failed to
     * truncate to the new payload size. */
    mcRead(fd, buffer, 1);
    rc = McResult();
    if (rc != 0) {
        CloseCardFile(fd);
        return rc < 0 ? rc : -4402;
    }
    return CloseCardFile(fd);
}

static int RestoreBackup(int port, const char *path, const BackupState *backup)
{
    if (!backup->valid)
        return -4500;
    if (!backup->existed)
        return DeleteTarget(port, path);
    return WriteCardFile(port, path, backup->data, backup->size);
}

static int IsFreemcbCnf(const char *source)
{
    const char *name = strrchr(source, '/');
    if (name == NULL)
        name = source;
    else
        name++;
    return strcmp(name, "FREEMCB.CNF") == 0;
}

static void FreeBackups(BackupState backups[FMCB_TX_MAX_FILES])
{
    int i;
    for (i = 0; i < FMCB_TX_MAX_FILES; i++) {
        if (backups[i].data != NULL)
            free(backups[i].data);
        backups[i].data = NULL;
    }
}

static int Rollback(int port, FmcbInstallReport *report,
                    BackupState backups[FMCB_TX_MAX_FILES])
{
    int i;
    int first_error = 0;

    report->stage = FMCB_INSTALL_ROLLBACK;
    TxProgress(2, "Rolling back the FMCB transaction",
               "Restoring every destination touched by this run before reporting failure.");

    for (i = report->current_file; i >= 0; i--) {
        FmcbInstallFileReport *file = &report->files[i];
        int rc;
        if (file->skipped || !backups[i].valid)
            continue;
        rc = RestoreBackup(port, file->destination, &backups[i]);
        if (rc < 0 && first_error == 0)
            first_error = rc;
    }

    if (report->created_sysconf_dir) {
        mcDelete(port, 0, "/SYS-CONF");
        (void)McResult();
    }
    if (report->created_system_dir) {
        char path[48];
        snprintf(path, sizeof(path), "/%s", report->files_total > 0
                 ? "" : "");
        /* System directory removal is handled below by the caller because its
         * resolved name is owned by the package plan. */
    }
    report->rollback_rc = first_error;
    return first_error;
}

int FmcbInstallNormalTransactional(int target_port,
                                   const FmcbPackageReport *package,
                                   const FmcbInstallOptions *options,
                                   FmcbBindKelfCallback bind_kelf,
                                   void *bind_userdata,
                                   FmcbInstallReport *report)
{
    BackupState backups[FMCB_TX_MAX_FILES];
    char system_dir[48];
    char source_path[FMCB_PATH_MAX + FMCB_SOURCE_ROOT_MAX + 4];
    char detail[240];
    int i;
    int rc = 0;

    memset(backups, 0, sizeof(backups));
    FmcbInstallResetReport(report, target_port);
    report->stage = FMCB_INSTALL_PRECONDITIONS;

    if (package == NULL || options == NULL || bind_kelf == NULL ||
        package->status != FMCB_PACKAGE_READY ||
        !package->plan.package_complete || !options->verify_every_file) {
        report->result = FMCB_INSTALL_RESULT_REJECTED;
        return -1;
    }

    report->files_total = package->entry_count;
    if (report->files_total > FMCB_TX_MAX_FILES)
        report->files_total = FMCB_TX_MAX_FILES;

    report->stage = FMCB_INSTALL_CREATE_DIRS;
    snprintf(system_dir, sizeof(system_dir), "/%s", package->plan.destination_system);
    TxProgress(3, "Creating/verifying FMCB directories",
               "Ensuring the region system directory and SYS-CONF exist before any file replacement begins.");
    rc = EnsureDirectory(target_port, system_dir, 1, &report->created_system_dir);
    if (rc < 0)
        goto target_failure;
    rc = EnsureDirectory(target_port, "/SYS-CONF", 0, &report->created_sysconf_dir);
    if (rc < 0)
        goto target_failure;

    for (i = 0; i < report->files_total; i++) {
        const FmcbPackageEntry *entry = FmcbPackageEntryAt(i);
        const FmcbPackageFileStatus *source_status = &package->files[i];
        FmcbInstallFileReport *file = &report->files[i];
        unsigned char *source = NULL;
        unsigned int source_size = 0;
        int base_percent = 7 + (i * 86) / (report->files_total > 0 ? report->files_total : 1);

        report->current_file = i;
        file->flags = entry != NULL ? entry->flags : 0;
        file->size = source_status->size;
        snprintf(file->source, sizeof(file->source), "%s", source_status->relative_path);
        if (FmcbResolveDestination(&package->plan, i,
                                   file->destination, sizeof(file->destination)) < 0) {
            rc = -4600;
            report->result = FMCB_INSTALL_RESULT_SOURCE_IO;
            goto failure;
        }

        report->stage = FMCB_INSTALL_BACKUP_TARGET;
        snprintf(detail, sizeof(detail), "Inventorying %s before replacement.", file->destination);
        TxProgress(base_percent, "Capturing destination rollback state", detail);
        rc = CaptureTarget(target_port, file->destination, &backups[i]);
        file->backup_rc = rc;
        if (rc < 0) {
            report->result = FMCB_INSTALL_RESULT_TARGET_IO;
            goto failure;
        }
        file->existed = backups[i].existed;

        if (options->preserve_existing_cnfs && file->existed && IsFreemcbCnf(file->source)) {
            file->skipped = 1;
            file->write_rc = 0;
            file->verify_rc = 0;
            report->files_committed++;
            continue;
        }

        report->stage = FMCB_INSTALL_READ_SOURCE;
        snprintf(source_path, sizeof(source_path), "%s/%s",
                 package->source_root, source_status->relative_path);
        snprintf(detail, sizeof(detail), "Loading %s into EE RAM before touching its destination.", source_status->relative_path);
        TxProgress(base_percent + 1, "Reading installation source", detail);
        rc = ReadMassFile(source_path, &source, &source_size);
        if (rc < 0) {
            report->result = FMCB_INSTALL_RESULT_SOURCE_IO;
            goto failure;
        }
        file->size = source_size;

        if (file->flags & FMCB_FILE_KELF) {
            report->stage = FMCB_INSTALL_BIND_KELF;
            snprintf(detail, sizeof(detail),
                     "Binding %s to mc%d in RAM through the PS2SDK 2.0 SECRMAN personality.",
                     source_status->relative_path, target_port);
            TxProgress(base_percent + 2, "Binding KELF", detail);
            rc = bind_kelf(target_port, source, source_size, bind_userdata);
            file->bind_rc = rc;
            if (rc < 0) {
                free(source);
                report->result = FMCB_INSTALL_RESULT_BIND_FAILED;
                goto failure;
            }
        } else {
            file->bind_rc = 0;
        }

        report->stage = FMCB_INSTALL_WRITE_TARGET;
        snprintf(detail, sizeof(detail), "Writing %u bytes to mc%d:%s, then flushing and closing.",
                 source_size, target_port, file->destination);
        TxProgress(base_percent + 4, "Writing FMCB destination", detail);
        rc = WriteCardFile(target_port, file->destination, source, source_size);
        file->write_rc = rc;
        if (rc < 0) {
            free(source);
            report->result = FMCB_INSTALL_RESULT_TARGET_IO;
            goto failure;
        }

        report->stage = FMCB_INSTALL_VERIFY_TARGET;
        TxProgress(base_percent + 6, "Reopening and verifying FMCB destination",
                   "Reading the complete committed file back from the memory card and comparing every byte.");
        rc = VerifyCardFile(target_port, file->destination, source, source_size);
        file->verify_rc = rc;
        free(source);
        if (rc < 0) {
            report->result = FMCB_INSTALL_RESULT_VERIFY_FAILED;
            goto failure;
        }
        report->files_committed++;
    }

    report->stage = FMCB_INSTALL_DONE;
    report->result = FMCB_INSTALL_RESULT_PASS;
    report->current_file = report->files_total - 1;
    TxProgress(100, "Verified FMCB normal install complete",
               "Every selected target was written, reopened and compared successfully. Rollback state can now be discarded.");
    FreeBackups(backups);
    return 0;

 target_failure:
    report->result = FMCB_INSTALL_RESULT_TARGET_IO;
 failure:
    if (Rollback(target_port, report, backups) < 0)
        report->result = FMCB_INSTALL_RESULT_ROLLBACK_FAILED;

    if (report->created_system_dir) {
        mcDelete(target_port, 0, system_dir);
        rc = McResult();
        if (rc < 0 && rc != sceMcResNoEntry && report->rollback_rc == 0)
            report->rollback_rc = rc;
    }
    FreeBackups(backups);
    return -1;
}

const char *FmcbInstallStageText(FmcbInstallStage stage)
{
    switch (stage) {
        case FMCB_INSTALL_PRECONDITIONS: return "PRECONDITIONS";
        case FMCB_INSTALL_CREATE_DIRS: return "CREATE DIRS";
        case FMCB_INSTALL_BACKUP_TARGET: return "BACKUP TARGET";
        case FMCB_INSTALL_READ_SOURCE: return "READ SOURCE";
        case FMCB_INSTALL_BIND_KELF: return "BIND KELF";
        case FMCB_INSTALL_WRITE_TARGET: return "WRITE TARGET";
        case FMCB_INSTALL_VERIFY_TARGET: return "VERIFY TARGET";
        case FMCB_INSTALL_ROLLBACK: return "ROLLBACK";
        case FMCB_INSTALL_DONE: return "DONE";
        default: return "NOT RUN";
    }
}

const char *FmcbInstallResultText(FmcbInstallResult result)
{
    switch (result) {
        case FMCB_INSTALL_RESULT_PASS: return "PASS / VERIFIED";
        case FMCB_INSTALL_RESULT_REJECTED: return "REJECTED BY PRECONDITIONS";
        case FMCB_INSTALL_RESULT_SOURCE_IO: return "SOURCE I/O FAILURE";
        case FMCB_INSTALL_RESULT_BIND_FAILED: return "KELF BIND FAILURE";
        case FMCB_INSTALL_RESULT_TARGET_IO: return "TARGET I/O FAILURE";
        case FMCB_INSTALL_RESULT_VERIFY_FAILED: return "READ-BACK VERIFY FAILURE";
        case FMCB_INSTALL_RESULT_ROLLBACK_FAILED: return "ROLLBACK FAILURE";
        default: return "NOT RUN";
    }
}
