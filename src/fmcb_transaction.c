/* SPDX-License-Identifier: MIT */
/*
 * Verified FreeMcBoot normal-install transaction.
 *
 * 0.4 deliberately does not implement multi-install/cross-model page linking.
 * The selected normal manifest is resolved by fmcb_install.c. Before the first
 * memory-card write we prove that the sequential replacement order fits the
 * free-cluster budget and create a durable USB recovery journal. Every target
 * is then captured to USB, every KELF is bound in RAM, every write is flushed,
 * reopened and compared byte-for-byte, and only a completely verified set is
 * allowed to discard the recovery journal.
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

static unsigned int ClustersForBytes(unsigned int size)
{
    if (size == 0u)
        return 0u;
    return (size + FMCB_MC_CLUSTER_BYTES - 1u) / FMCB_MC_CLUSTER_BYTES;
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
    report->recovery_rc = -999;
    report->space_rc = -999;
    report->free_clusters = -1;
    report->minimum_remaining_clusters = -1;
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

static int DirectoryExists(int port, const char *path, int *exists)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int rc;

    *exists = 0;
    memset(&info, 0, sizeof(info));
    mcGetDir(port, 0, path, 0, 1, &info);
    rc = McResult();
    if (rc == sceMcResNoEntry || rc == 0)
        return 0;
    if (rc < 0)
        return rc;
    *exists = 1;
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

    /* Catch stale tail data if a replacement somehow failed to truncate. */
    mcRead(fd, buffer, 1);
    rc = McResult();
    if (rc != 0) {
        CloseCardFile(fd);
        return rc < 0 ? rc : -4402;
    }
    return CloseCardFile(fd);
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

static int InventoryTarget(int port, const char *path,
                           int *exists, unsigned int *size)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int rc;

    *exists = 0;
    *size = 0;
    memset(&info, 0, sizeof(info));
    mcGetDir(port, 0, path, 0, 1, &info);
    rc = McResult();
    if (rc == sceMcResNoEntry || rc == 0)
        return 0;
    if (rc < 0)
        return rc;
    *exists = 1;
    *size = info.FileSizeByte;
    return 0;
}

static int PrepareInventory(int target_port,
                            const FmcbPackageReport *package,
                            const FmcbInstallOptions *options,
                            FmcbInstallReport *report)
{
    int i;

    report->files_total = 0;
    for (i = 0; i < package->entry_count && i < FMCB_TX_MAX_FILES; i++) {
        const FmcbPackageEntry *entry = FmcbPackageEntryAt(i);
        const FmcbPackageFileStatus *source_status = &package->files[i];
        FmcbInstallFileReport *file = &report->files[i];
        int rc;

        file->flags = entry != NULL ? entry->flags : 0u;
        file->selected = source_status->selected &&
                         FmcbPackageEntrySelected(&package->plan, i);
        snprintf(file->source, sizeof(file->source), "%s",
                 source_status->relative_path);
        if (!file->selected) {
            file->skipped = 1;
            continue;
        }
        report->files_total++;
        if (entry == NULL || !source_status->found || source_status->size == 0u)
            return -4600;
        rc = FmcbResolveDestination(&package->plan, i,
                                    file->destination,
                                    sizeof(file->destination));
        if (rc < 0)
            return rc;
        file->size = source_status->size;
        file->required_clusters = ClustersForBytes(file->size);

        rc = InventoryTarget(target_port, file->destination, &file->existed,
                             &file->previous_size);
        if (rc < 0)
            return rc;
        file->reclaimable_clusters = file->existed
                                         ? ClustersForBytes(file->previous_size)
                                         : 0u;
        if (options->preserve_existing_cnfs && file->existed &&
            IsFreemcbCnf(file->source)) {
            file->skipped = 1;
            file->required_clusters = 0;
            file->reclaimable_clusters = 0;
        }
    }
    return report->files_total > 0 ? 0 : -4601;
}

static int CheckSpace(int target_port, const FmcbPackageReport *package,
                      FmcbInstallReport *report)
{
    char system_dir[48];
    int type = 0;
    int free_clusters = 0;
    int formatted = 0;
    int info_rc;
    int system_exists = 0;
    int sysconf_exists = 0;
    int available;
    int minimum;
    int i;
    int rc;

    (void)package;
    mcGetInfo(target_port, 0, &type, &free_clusters, &formatted);
    info_rc = McResult();
    if (type != MC_TYPE_PS2 || !formatted || free_clusters < 0)
        return info_rc < 0 ? info_rc : -4610;

    snprintf(system_dir, sizeof(system_dir), "/%s",
             package->plan.destination_system);
    rc = DirectoryExists(target_port, system_dir, &system_exists);
    if (rc < 0)
        return rc;
    rc = DirectoryExists(target_port, "/SYS-CONF", &sysconf_exists);
    if (rc < 0)
        return rc;

    /* Two clusters per potentially new directory plus a fixed 16-cluster
     * guard band. The exact directory/FAT bookkeeping is driver-owned, so the
     * safety reserve intentionally errs on the side of refusing a nearly-full
     * card rather than discovering ENOSPC after the first replacement. */
    report->reserve_clusters = FMCB_TX_SAFETY_RESERVE_CLUSTERS;
    if (!system_exists)
        report->reserve_clusters += 2u;
    if (!sysconf_exists)
        report->reserve_clusters += 2u;
    report->free_clusters = free_clusters;
    report->payload_clusters = 0u;
    report->reclaimable_clusters = 0u;

    available = free_clusters - (int)report->reserve_clusters;
    minimum = available;
    if (available < 0)
        return -4611;

    for (i = 0; i < package->entry_count && i < FMCB_TX_MAX_FILES; i++) {
        const FmcbInstallFileReport *file = &report->files[i];
        if (!file->selected || file->skipped)
            continue;
        report->payload_clusters += file->required_clusters;
        report->reclaimable_clusters += file->reclaimable_clusters;

        /* Model the real replacement order: only the current file's old
         * clusters become reusable before its new payload must fit. Later
         * targets are not counted early. */
        available += (int)file->reclaimable_clusters;
        if (available < (int)file->required_clusters)
            return -4612 - i;
        available -= (int)file->required_clusters;
        if (available < minimum)
            minimum = available;
    }

    report->minimum_remaining_clusters = minimum;
    return 0;
}

static int RollbackPersistent(FmcbRecoveryStatus *recovery,
                              FmcbInstallReport *report)
{
    int rc;

    report->stage = FMCB_INSTALL_ROLLBACK;
    TxProgress(2, "Rolling back the FMCB transaction",
               "Restoring all prepared destinations from the persistent USB journal before reporting failure.");
    rc = FmcbRecoveryRun(recovery, &report->rollback_rc);
    report->recovery_rc = rc;
    return rc;
}

int FmcbInstallNormalTransactional(int target_port,
                                   const FmcbPackageReport *package,
                                   const FmcbInstallOptions *options,
                                   FmcbBindKelfCallback bind_kelf,
                                   void *bind_userdata,
                                   FmcbRecoveryStatus *recovery,
                                   FmcbInstallReport *report)
{
    char system_dir[48];
    char source_path[FMCB_PATH_MAX + FMCB_SOURCE_ROOT_MAX + 4];
    char detail[256];
    FmcbInstallResult failure_result = FMCB_INSTALL_RESULT_NOT_RUN;
    int recovery_started = 0;
    int i;
    int rc;

    FmcbInstallResetReport(report, target_port);
    report->stage = FMCB_INSTALL_PRECONDITIONS;

    if (package == NULL || options == NULL || bind_kelf == NULL ||
        recovery == NULL || package->status != FMCB_PACKAGE_READY ||
        !package->plan.package_complete ||
        (unsigned int)options->verify_mode >= MCI_INSTALL_VERIFY_MODE_COUNT ||
        package->plan.target_port != target_port) {
        report->result = FMCB_INSTALL_RESULT_REJECTED;
        return -1;
    }

    rc = PrepareInventory(target_port, package, options, report);
    if (rc < 0) {
        report->result = FMCB_INSTALL_RESULT_REJECTED;
        return rc;
    }

    report->stage = FMCB_INSTALL_SPACE_CHECK;
    TxProgress(1, "Checking memory-card space",
               "Simulating the exact replacement order with a filesystem safety reserve before creating any card object.");
    rc = CheckSpace(target_port, package, report);
    report->space_rc = rc;
    if (rc < 0) {
        report->result = FMCB_INSTALL_RESULT_NO_SPACE;
        return rc;
    }

    snprintf(detail, sizeof(detail),
             "free=%d clusters, payload=%u, reclaimable=%u, reserve=%u, minimum remaining=%d.",
             report->free_clusters, report->payload_clusters,
             report->reclaimable_clusters, report->reserve_clusters,
             report->minimum_remaining_clusters);
    TxProgress(2, "Space simulation passed", detail);

    report->stage = FMCB_INSTALL_RECOVERY_PREPARE;
    TxProgress(3, "Creating persistent recovery journal",
               "Preparing dual checksummed USB journal slots before the first memory-card write.");
    rc = FmcbRecoveryBegin(package, recovery);
    report->recovery_rc = rc;
    if (rc < 0) {
        report->result = (rc == -5140)
                             ? FMCB_INSTALL_RESULT_RECOVERY_REQUIRED
                             : FMCB_INSTALL_RESULT_RECOVERY_IO;
        return rc;
    }
    recovery_started = 1;

    report->stage = FMCB_INSTALL_CREATE_DIRS;
    snprintf(system_dir, sizeof(system_dir), "/%s",
             package->plan.destination_system);
    TxProgress(4, "Creating/verifying FMCB directories",
               "Ensuring the active region system directory and SYS-CONF exist; newly created directories are recorded in the recovery journal.");
    rc = EnsureDirectory(target_port, system_dir, 1,
                         &report->created_system_dir);
    if (rc < 0) {
        failure_result = FMCB_INSTALL_RESULT_TARGET_IO;
        goto failure;
    }
    rc = FmcbRecoveryRecordDirectories(recovery, system_dir,
                                       report->created_system_dir, 0);
    if (rc < 0) {
        failure_result = FMCB_INSTALL_RESULT_RECOVERY_IO;
        goto failure;
    }
    rc = EnsureDirectory(target_port, "/SYS-CONF", 0,
                         &report->created_sysconf_dir);
    if (rc < 0) {
        failure_result = FMCB_INSTALL_RESULT_TARGET_IO;
        goto failure;
    }
    rc = FmcbRecoveryRecordDirectories(recovery, system_dir,
                                       report->created_system_dir,
                                       report->created_sysconf_dir);
    if (rc < 0) {
        failure_result = FMCB_INSTALL_RESULT_RECOVERY_IO;
        goto failure;
    }

    for (i = 0; i < package->entry_count && i < FMCB_TX_MAX_FILES; i++) {
        const FmcbPackageFileStatus *source_status = &package->files[i];
        FmcbInstallFileReport *file = &report->files[i];
        unsigned char *source = NULL;
        unsigned int source_size = 0;
        unsigned int backup_size = 0;
        int backup_existed = 0;
        int selected_index;
        int base_percent;

        if (!file->selected)
            continue;
        if (file->skipped) {
            file->backup_rc = 0;
            file->write_rc = 0;
            file->verify_rc = 0;
            file->verify_skipped = 1;
            report->files_committed++;
            continue;
        }

        report->current_file = i;
        selected_index = report->files_committed;
        base_percent = 7 + selected_index * 86 /
                            (report->files_total > 0 ? report->files_total : 1);

        report->stage = FMCB_INSTALL_BACKUP_TARGET;
        snprintf(detail, sizeof(detail),
                 "Persisting mc%d:%s before replacement; the card is unchanged until this backup verifies on USB.",
                 target_port, file->destination);
        TxProgress(base_percent, "Capturing durable rollback state", detail);
        rc = FmcbRecoveryCaptureTarget(recovery, target_port, i,
                                       file->destination,
                                       &backup_existed, &backup_size);
        file->backup_rc = rc;
        if (rc < 0) {
            failure_result = FMCB_INSTALL_RESULT_RECOVERY_IO;
            goto failure;
        }
        /* Refuse a race/card swap between the read-only inventory and durable
         * capture. Re-running the whole install is safer than trusting stale
         * space accounting. */
        if (backup_existed != file->existed ||
            backup_size != file->previous_size) {
            failure_result = FMCB_INSTALL_RESULT_REJECTED;
            rc = -4620;
            goto failure;
        }

        report->stage = FMCB_INSTALL_READ_SOURCE;
        snprintf(source_path, sizeof(source_path), "%s/%s",
                 package->source_root, source_status->relative_path);
        snprintf(detail, sizeof(detail),
                 "Loading %s into EE RAM before touching its destination.",
                 source_status->relative_path);
        TxProgress(base_percent + 1, "Reading installation source", detail);
        rc = ReadMassFile(source_path, &source, &source_size);
        if (rc < 0) {
            failure_result = FMCB_INSTALL_RESULT_SOURCE_IO;
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
                failure_result = FMCB_INSTALL_RESULT_BIND_FAILED;
                goto failure;
            }
        } else {
            file->bind_rc = 0;
        }

        report->stage = FMCB_INSTALL_WRITE_TARGET;
        snprintf(detail, sizeof(detail),
                 "Writing %u bytes to mc%d:%s, then flushing and closing.",
                 source_size, target_port, file->destination);
        TxProgress(base_percent + 4, "Writing FMCB destination", detail);
        rc = WriteCardFile(target_port, file->destination, source, source_size);
        file->write_rc = rc;
        if (rc < 0) {
            free(source);
            failure_result = FMCB_INSTALL_RESULT_TARGET_IO;
            goto failure;
        }

        if (options->verify_mode == MCI_INSTALL_VERIFY_ENFORCED ||
            (options->verify_mode == MCI_INSTALL_VERIFY_REQUIRED &&
             (file->flags & FMCB_FILE_REQUIRED))) {
            report->stage = FMCB_INSTALL_VERIFY_TARGET;
            TxProgress(base_percent + 6,
                       "Reopening and verifying FMCB destination",
                       "Reading the complete committed file back from the memory card and comparing every byte against the bound/source RAM image.");
            rc = VerifyCardFile(target_port, file->destination, source, source_size);
            file->verify_rc = rc;
            file->verify_skipped = 0;
            free(source);
            if (rc < 0) {
                failure_result = FMCB_INSTALL_RESULT_VERIFY_FAILED;
                goto failure;
            }
        } else {
            file->verify_rc = 0;
            file->verify_skipped = 1;
            free(source);
        }
        report->files_committed++;
    }

    report->stage = FMCB_INSTALL_RECOVERY_FINISH;
    if (options->verify_mode == MCI_INSTALL_VERIFY_ENFORCED)
        TxProgress(97, "Committing the verified transaction",
                   "All selected files passed read-back verification. Marking the USB journal committed before removing recovery backups.");
    else if (options->verify_mode == MCI_INSTALL_VERIFY_REQUIRED)
        TxProgress(97, "Committing the required-file verified transaction",
                   "All required files passed read-back verification; optional destinations were intentionally not reread. Committing the recovery journal.");
    else
        TxProgress(97, "Committing the unverified transaction",
                   "Read-back comparison was disabled by the user. Durable rollback state remains authoritative until the journal is committed.");
    rc = FmcbRecoveryFinish(recovery);
    report->recovery_rc = rc;
    if (rc < 0) {
        report->result = FMCB_INSTALL_RESULT_RECOVERY_IO;
        return rc;
    }

    report->stage = FMCB_INSTALL_DONE;
    if (options->verify_mode == MCI_INSTALL_VERIFY_ENFORCED)
        report->result = FMCB_INSTALL_RESULT_PASS;
    else if (options->verify_mode == MCI_INSTALL_VERIFY_REQUIRED)
        report->result = FMCB_INSTALL_RESULT_PASS_REQUIRED_VERIFY;
    else
        report->result = FMCB_INSTALL_RESULT_PASS_UNVERIFIED;
    report->current_file = -1;
    if (options->verify_mode == MCI_INSTALL_VERIFY_ENFORCED)
        TxProgress(100, "Verified FMCB normal install complete",
                   "Every selected target was written, reopened and compared successfully; the persistent recovery journal is clean.");
    else if (options->verify_mode == MCI_INSTALL_VERIFY_REQUIRED)
        TxProgress(100, "FMCB install complete / required files verified",
                   "Required destinations were reopened and compared; optional files were committed without read-back comparison.");
    else
        TxProgress(100, "FMCB install complete / read-back disabled",
                   "The transaction committed successfully, but destination contents were not reread after writing.");
    return 0;

failure:
    report->result = failure_result == FMCB_INSTALL_RESULT_NOT_RUN
                         ? FMCB_INSTALL_RESULT_TARGET_IO
                         : failure_result;
    if (recovery_started && recovery->valid) {
        FmcbInstallResult original = report->result;
        if (RollbackPersistent(recovery, report) < 0)
            report->result = FMCB_INSTALL_RESULT_ROLLBACK_FAILED;
        else
            report->result = original;
    }
    return rc < 0 ? rc : -1;
}

const char *FmcbInstallStageText(FmcbInstallStage stage)
{
    switch (stage) {
        case FMCB_INSTALL_PRECONDITIONS: return "PRECONDITIONS";
        case FMCB_INSTALL_SPACE_CHECK: return "SPACE CHECK";
        case FMCB_INSTALL_RECOVERY_PREPARE: return "RECOVERY PREPARE";
        case FMCB_INSTALL_CREATE_DIRS: return "CREATE DIRS";
        case FMCB_INSTALL_BACKUP_TARGET: return "PERSIST BACKUP";
        case FMCB_INSTALL_READ_SOURCE: return "READ SOURCE";
        case FMCB_INSTALL_BIND_KELF: return "BIND KELF";
        case FMCB_INSTALL_WRITE_TARGET: return "WRITE TARGET";
        case FMCB_INSTALL_VERIFY_TARGET: return "VERIFY TARGET";
        case FMCB_INSTALL_ROLLBACK: return "ROLLBACK";
        case FMCB_INSTALL_RECOVERY_FINISH: return "RECOVERY FINISH";
        case FMCB_INSTALL_DONE: return "DONE";
        default: return "NOT RUN";
    }
}

const char *FmcbInstallResultText(FmcbInstallResult result)
{
    switch (result) {
        case FMCB_INSTALL_RESULT_PASS: return "PASS / VERIFIED";
        case FMCB_INSTALL_RESULT_PASS_REQUIRED_VERIFY: return "PASS / REQUIRED FILES VERIFIED";
        case FMCB_INSTALL_RESULT_PASS_UNVERIFIED: return "PASS / READ-BACK DISABLED";
        case FMCB_INSTALL_RESULT_REJECTED: return "REJECTED BY PRECONDITIONS";
        case FMCB_INSTALL_RESULT_NO_SPACE: return "INSUFFICIENT SAFE CARD SPACE";
        case FMCB_INSTALL_RESULT_RECOVERY_REQUIRED: return "INCOMPLETE RECOVERY JOURNAL EXISTS";
        case FMCB_INSTALL_RESULT_RECOVERY_IO: return "PERSISTENT RECOVERY I/O FAILURE";
        case FMCB_INSTALL_RESULT_SOURCE_IO: return "SOURCE I/O FAILURE";
        case FMCB_INSTALL_RESULT_BIND_FAILED: return "KELF BIND FAILURE";
        case FMCB_INSTALL_RESULT_TARGET_IO: return "TARGET I/O FAILURE";
        case FMCB_INSTALL_RESULT_VERIFY_FAILED: return "READ-BACK VERIFY FAILURE";
        case FMCB_INSTALL_RESULT_ROLLBACK_FAILED: return "ROLLBACK FAILURE / JOURNAL RETAINED";
        default: return "NOT RUN";
    }
}
