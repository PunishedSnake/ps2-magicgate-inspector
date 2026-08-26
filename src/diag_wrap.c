/* SPDX-License-Identifier: MIT */
/* High-value operation wrappers for the persistent Drebin trace. */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <libmc.h>
#include <stdio.h>
#include <string.h>

#include "card_image.h"
#include "card_image_fs.h"
#include "card_raw_session.h"
#include "diag_log.h"
#include "fmcb_transaction.h"
#include "force_format_vmc.h"
#include "image_quick_verify.h"
#include "image_read_ahead.h"
#include "image_write_behind.h"
#include "raw_bulk_read.h"

int __real_MciCardImageProbeGeometry(int port, MciCardGeometry *geometry);
int __real_MciCardImageExport(int port, MciCardImageFormat format,
                              MciCardImageReport *report);
int __real_MciCardImageVerifyFile(const char *path, MciCardImageFormat format,
                                  MciCardImageReport *report);
int __real_MciCardImageRestoreExact(int port, const char *path,
                                    MciCardImageFormat format,
                                    MciCardImageReport *report);
int __real_MciImageFsScan(const char *path, MciCardImageFormat format,
                          MciImageSaveList *list);
int __real_MciImageFsImportSelected(int target_port, MciImageSaveList *list,
                                    MciImageImportReport *report);
int __real_MciRawCardSessionStart(MciRawCardSessionStatus *status);
void __real_MciRawCardSessionStop(MciRawCardSessionStatus *status);
int __real_FmcbInitMassBackend(FmcbMassBackendStatus *status);
void __real_FmcbShutdownMassBackend(FmcbMassBackendStatus *status);
int __real_FmcbInstallNormalTransactional(int target_port,
                                          const FmcbPackageReport *package,
                                          const FmcbInstallOptions *options,
                                          FmcbBindKelfCallback bind_kelf,
                                          void *bind_userdata,
                                          FmcbRecoveryStatus *recovery,
                                          FmcbInstallReport *report);
int __real_mcReadPage(int port, int slot, int page, void *buffer);

static unsigned int MassBackendInitCalls;
static unsigned int RawReadTraceBudget;
static int TrustedImportVerifyActive;
static MciCardImageFormat TrustedImportFormat;
static char TrustedImportPath[MCI_CARD_IMAGE_PATH_MAX];

static void LogImageReport(const char *operation, int rc,
                           const MciCardImageReport *report)
{
    if (report == NULL) {
        MciDiagLogPrintf("IMAGE", "%s end rc=%d report=NULL", operation, rc);
        return;
    }

    MciDiagLogPrintf("IMAGE",
                     "%s end rc=%d result=%s port=mc%d format=%s path=%s pages=%u/%u bytes=%llu crc=%08X verified=%d verify_rc=%d format_rc=%d geometry=%u/%u/%u clusters=%u superblock=%d",
                     operation, rc, MciCardImageResultText(report->result),
                     report->port, MciCardImageFormatName(report->format),
                     report->path[0] != '\0' ? report->path : "n/a",
                     report->pages_done, report->pages_total,
                     (unsigned long long)report->output_bytes,
                     report->logical_crc32, report->verified,
                     report->verify_rc, report->format_rc,
                     report->geometry.page_size,
                     report->geometry.pages_per_cluster,
                     report->geometry.pages_per_block,
                     report->geometry.clusters_per_card,
                     report->geometry.from_superblock);
}

static int SyncPathDevice(const char *path)
{
    char device[16];
    const char *colon;
    unsigned int length;

    if (path == NULL)
        return -1;
    colon = strchr(path, ':');
    if (colon == NULL)
        return -2;
    length = (unsigned int)(colon - path) + 1u;
    if (length == 0u || length >= sizeof(device))
        return -3;
    memcpy(device, path, length);
    device[length] = '\0';
    return fileXioSync(device, 0);
}

int __wrap_FmcbInitMassBackend(FmcbMassBackendStatus *status)
{
    int rc = __real_FmcbInitMassBackend(status);
    unsigned int call = ++MassBackendInitCalls;

    if (call == 1u) {
        MciDiagLogPrintf("LIFECYCLE",
                         "boot mass backend init rc=%d; persistent logger deliberately remains RAM-only",
                         rc);
    } else if (rc >= 0 && status != NULL && status->available) {
        MciDiagLogSetIoAvailable(1);
        MciDiagLogPrintf("LIFECYCLE",
                         "restored mass backend init call=%u rc=%d available=%d",
                         call, rc, status->available);
    } else {
        MciDiagLogPrintf("LIFECYCLE",
                         "mass backend restore call=%u failed rc=%d available=%d",
                         call, rc, status != NULL ? status->available : -1);
    }
    return rc;
}

void __wrap_FmcbShutdownMassBackend(FmcbMassBackendStatus *status)
{
    MciDiagLogSetIoAvailable(0);
    __real_FmcbShutdownMassBackend(status);
}

int __wrap_MciRawCardSessionStart(MciRawCardSessionStatus *status)
{
    int rc;
    int bulk_rc = -999;

    RawReadTraceBudget = 0u;
    MciRawBulkReadReset();
    MciDiagLogPrintf("RAW", "session start begin");
    rc = __real_MciRawCardSessionStart(status);
    if (status == NULL) {
        MciDiagLogPrintf("RAW", "session start end rc=%d status=NULL", rc);
        return rc;
    }
    if (rc == 0 && status->ready) {
        /* This is an optional optimization layer. A stock/raw MCSERV mismatch
         * must never take imaging down: failure simply leaves mcReadPage on the
         * hardware-qualified one-page RPC path. */
        bulk_rc = MciRawBulkReadBind();
        RawReadTraceBudget = 4u;
    }
    MciDiagLogPrintf("RAW",
                     "session start end rc=%d ready=%d modules sio2=%d pad=%d mcman=%d mcserv=%d iomanx=%d filexio_mod=%d usbd=%d usbhdfsd=%d clients mcinit=%d mcinfo=%d/%d/%d type=%d free=%d formatted=%d filexio_init=%d bulk_bind=%d",
                     rc, status->ready, status->sio2_rc, status->pad_rc,
                     status->mcman_rc, status->mcserv_rc, status->iomanx_rc,
                     status->filexio_module_rc, status->usbd_rc,
                     status->usbhdfsd_rc, status->mcinit_rc,
                     status->mcinfo_issue_rc, status->mcinfo_sync_rc,
                     status->mcinfo_result, status->card_type,
                     status->free_clusters, status->formatted,
                     status->filexio_init_rc, bulk_rc);
    return rc;
}

void __wrap_MciRawCardSessionStop(MciRawCardSessionStatus *status)
{
    MciRawBulkReadStats bulk;

    RawReadTraceBudget = 0u;
    MciRawBulkReadGetStats(&bulk);
    MciDiagLogPrintf("RAW-BULK",
                     "stats bound=%d bind_rc=%d rpc_calls=%u cache_hits=%u fallbacks=%u pages_fetched=%u ecc_warnings=%u last_rpc=%d rpc_ticks=%llu",
                     bulk.bound, bulk.bind_rc, bulk.rpc_calls, bulk.cache_hits,
                     bulk.fallback_calls, bulk.pages_fetched,
                     bulk.ecc_warning_pages, bulk.last_rpc_rc,
                     (unsigned long long)bulk.rpc_ticks);
    MciDiagLogPrintf("RAW", "session stop begin ready=%d",
                     status != NULL ? status->ready : -1);
    __real_MciRawCardSessionStop(status);
    MciRawBulkReadReset();
    MciDiagLogPrintf("RAW", "session stop end ready=%d",
                     status != NULL ? status->ready : -1);
}

int __wrap_mcReadPage(int port, int slot, int page, void *buffer)
{
    int rc;
    int bulk_handled;
    int trace = RawReadTraceBudget > 0u;

    if (trace) {
        RawReadTraceBudget--;
        MciDiagLogPrintf("RAW-RPC",
                         "mcReadPage issue begin port=%d slot=%d page=%d buffer=%p budget_after=%u",
                         port, slot, page, buffer, RawReadTraceBudget);
    }

    bulk_handled = MciRawBulkReadTryPage(port, slot, page, buffer);
    rc = bulk_handled ? 0 : __real_mcReadPage(port, slot, page, buffer);

    if (trace)
        MciDiagLogPrintf("RAW-RPC",
                         "mcReadPage issue end port=%d slot=%d page=%d rc=%d path=%s",
                         port, slot, page, rc,
                         bulk_handled ? "bulk-cache" : "libmc-fallback");
    return rc;
}

int __wrap_MciCardImageProbeGeometry(int port, MciCardGeometry *geometry)
{
    int rc;

    MciDiagLogPrintf("IMAGE", "geometry probe begin port=mc%d", port);
    rc = __real_MciCardImageProbeGeometry(port, geometry);
    if (geometry != NULL) {
        MciDiagLogPrintf("IMAGE",
                         "geometry probe end port=mc%d rc=%d page=%u cluster_pages=%u block_pages=%u total_pages=%u clusters=%u superblock=%d",
                         port, rc, geometry->page_size,
                         geometry->pages_per_cluster, geometry->pages_per_block,
                         geometry->total_pages, geometry->clusters_per_card,
                         geometry->from_superblock);
    } else {
        MciDiagLogPrintf("IMAGE", "geometry probe end port=mc%d rc=%d geometry=NULL",
                         port, rc);
    }
    return rc;
}

/*
 * USBHDFSD/fileXio hardware finding (2026-08-24): repeatedly opening, appending
 * and closing DREBIN.LOG while a long-lived image fd was open corrupted the
 * image stream itself. Real dumps contained literal diagnostic text inside
 * logical card pages and, for .ps2, inside spare/ECC bytes. Therefore the
 * persistent logger must never perform mass: writes while an image stream is
 * open. Trace remains in the EE ring and is flushed only after the operation
 * closes every image descriptor.
 */
int __wrap_MciCardImageExport(int port, MciCardImageFormat format,
                              MciCardImageReport *report)
{
    int rc;

    MciDiagLogPrintf("IMAGE", "export begin port=mc%d format=%s",
                     port, MciCardImageFormatName(format));
    MciDiagLogSetMassWritePaused(1);
    MciImageReadAheadSetEnabled(1);
    MciImageWriteBehindSetEnabled(1);
    rc = __real_MciCardImageExport(port, format, report);
    MciImageWriteBehindSetEnabled(0);
    MciImageReadAheadSetEnabled(0);
    MciDiagLogSetMassWritePaused(0);
    LogImageReport("export", rc, report);
    return rc;
}

int __wrap_MciCardImageVerifyFile(const char *path, MciCardImageFormat format,
                                  MciCardImageReport *report)
{
    int rc;
    int sync_rc;
    int trusted = TrustedImportVerifyActive && path != NULL &&
                  format == TrustedImportFormat &&
                  strcmp(path, TrustedImportPath) == 0;

    sync_rc = SyncPathDevice(path);
    MciDiagLogPrintf("IMAGE", "%s begin format=%s path=%s preverify_sync=%d",
                     trusted ? "trusted reopen verify" : "verify",
                     MciCardImageFormatName(format), path != NULL ? path : "NULL",
                     sync_rc);
    MciDiagLogSetMassWritePaused(1);
    MciImageReadAheadSetEnabled(1);
    if (trusted)
        rc = MciCardImageQuickReopenVerify(path, format, report);
    else
        rc = __real_MciCardImageVerifyFile(path, format, report);

    if (!trusted && rc == -4 && format == MCI_CARD_IMAGE_PS2 && path != NULL) {
        unsigned char actual[12];
        unsigned char expected[12];
        u32 page = 0u;
        int find_rc = MciCardImageFindFirstEccMismatch(path, &page,
                                                       actual, expected);
        if (find_rc == 1) {
            MciDiagLogPrintf("IMAGE-ECC",
                             "first mismatch page=%u actual=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X expected=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                             page,
                             actual[0], actual[1], actual[2], actual[3],
                             actual[4], actual[5], actual[6], actual[7],
                             actual[8], actual[9], actual[10], actual[11],
                             expected[0], expected[1], expected[2], expected[3],
                             expected[4], expected[5], expected[6], expected[7],
                             expected[8], expected[9], expected[10], expected[11]);
        } else {
            MciDiagLogPrintf("IMAGE-ECC",
                             "mismatch locator rc=%d after verifier rc=-4",
                             find_rc);
        }
    }

    MciImageReadAheadSetEnabled(0);
    MciDiagLogSetMassWritePaused(0);
    LogImageReport(trusted ? "trusted reopen verify" : "verify", rc, report);
    return rc;
}

int __wrap_MciCardImageRestoreExact(int port, const char *path,
                                    MciCardImageFormat format,
                                    MciCardImageReport *report)
{
    int rc;

    MciDiagLogPrintf("IMAGE", "exact restore begin port=mc%d format=%s path=%s",
                     port, MciCardImageFormatName(format),
                     path != NULL ? path : "NULL");
    MciDiagLogSetMassWritePaused(1);
    MciImageReadAheadSetEnabled(1);
    rc = __real_MciCardImageRestoreExact(port, path, format, report);
    MciImageReadAheadSetEnabled(0);
    MciDiagLogSetMassWritePaused(0);
    LogImageReport("exact restore", rc, report);
    return rc;
}

int __wrap_MciCardForceFormatWithBackup(int port, MciCardImageReport *report)
{
    int rc;

    MciDiagLogPrintf("IMAGE", "force format begin port=mc%d backup=OPL .vmc", port);
    MciDiagLogSetMassWritePaused(1);
    MciImageReadAheadSetEnabled(1);
    rc = MciCardForceFormatWithVmcBackup(port, report);
    MciImageReadAheadSetEnabled(0);
    MciDiagLogSetMassWritePaused(0);
    LogImageReport("force format", rc, report);
    return rc;
}

int __wrap_MciImageFsScan(const char *path, MciCardImageFormat format,
                          MciImageSaveList *list)
{
    int rc;

    MciDiagLogPrintf("IMAGE-FS", "scan begin format=%s path=%s",
                     MciCardImageFormatName(format), path != NULL ? path : "NULL");
    MciDiagLogSetMassWritePaused(1);
    rc = __real_MciImageFsScan(path, format, list);
    MciDiagLogSetMassWritePaused(0);
    MciDiagLogPrintf("IMAGE-FS", "scan end rc=%d result=%s saves=%d",
                     rc, list != NULL ? MciImageFsResultText(list->result) : "NULL",
                     list != NULL ? list->save_count : -1);
    return rc;
}

int __wrap_MciImageFsImportSelected(int target_port, MciImageSaveList *list,
                                    MciImageImportReport *report)
{
    int rc;

    MciDiagLogPrintf("IMAGE-FS", "import begin target=mc%d path=%s",
                     target_port,
                     list != NULL && list->path[0] != '\0' ? list->path : "NULL");

    TrustedImportVerifyActive = 0;
    TrustedImportPath[0] = '\0';
    if (list != NULL && list->path[0] != '\0') {
        snprintf(TrustedImportPath, sizeof(TrustedImportPath), "%s", list->path);
        TrustedImportFormat = list->format;
        TrustedImportVerifyActive = 1;
        MciDiagLogPrintf("IMAGE-FS",
                         "selected-save import trusts prior full scan; second pass reduced to size/superblock reopen validation");
    }

    MciDiagLogSetMassWritePaused(1);
    rc = __real_MciImageFsImportSelected(target_port, list, report);
    MciDiagLogSetMassWritePaused(0);
    TrustedImportVerifyActive = 0;
    TrustedImportPath[0] = '\0';

    MciDiagLogPrintf("IMAGE-FS",
                     "import end rc=%d result=%s selected=%d restored=%d files=%u dirs=%u bytes=%u rollback=%d",
                     rc, report != NULL ? MciImageFsResultText(report->result) : "NULL",
                     report != NULL ? report->selected_saves : -1,
                     report != NULL ? report->restored_saves : -1,
                     report != NULL ? report->files_written : 0u,
                     report != NULL ? report->directories_written : 0u,
                     report != NULL ? report->bytes_written : 0u,
                     report != NULL ? report->rollback_rc : -999);
    return rc;
}

int __wrap_FmcbInstallNormalTransactional(int target_port,
                                          const FmcbPackageReport *package,
                                          const FmcbInstallOptions *options,
                                          FmcbBindKelfCallback bind_kelf,
                                          void *bind_userdata,
                                          FmcbRecoveryStatus *recovery,
                                          FmcbInstallReport *report)
{
    int rc;
    int i;

    MciDiagLogSetIoAvailable(1);
    MciDiagLogPrintf("FMCB",
                     "transaction begin target=mc%d package=%s root=%s entries=%d verify_mode=%d preserve_cnfs=%d recovery_present=%d recovery_valid=%d",
                     target_port,
                     package != NULL ? FmcbPackageStatusText(package->status) : "NULL",
                     package != NULL && package->source_root[0] != '\0'
                         ? package->source_root : "n/a",
                     package != NULL ? package->entry_count : -1,
                     options != NULL ? (int)options->verify_mode : -1,
                     options != NULL ? options->preserve_existing_cnfs : -1,
                     recovery != NULL ? recovery->present : -1,
                     recovery != NULL ? recovery->valid : -1);

    rc = __real_FmcbInstallNormalTransactional(target_port, package, options,
                                                bind_kelf, bind_userdata,
                                                recovery, report);

    if (report == NULL) {
        MciDiagLogPrintf("FMCB", "transaction end rc=%d report=NULL", rc);
        return rc;
    }

    MciDiagLogPrintf("FMCB",
                     "transaction end rc=%d stage=%s result=%s current_file=%d committed=%d/%d space_rc=%d free=%d minimum=%d recovery_rc=%d rollback_rc=%d",
                     rc, FmcbInstallStageText(report->stage),
                     FmcbInstallResultText(report->result), report->current_file,
                     report->files_committed, report->files_total,
                     report->space_rc, report->free_clusters,
                     report->minimum_remaining_clusters,
                     report->recovery_rc, report->rollback_rc);

    for (i = 0; i < FMCB_TX_MAX_FILES; i++) {
        const FmcbInstallFileReport *file = &report->files[i];
        if (!file->selected && file->inventory_exact_rc == -999 &&
            file->backup_rc == -999 && file->write_rc == -999)
            continue;
        MciDiagLogPrintf("FMCB-FILE",
                         "index=%d selected=%d skipped=%d existed=%d src=%s dst=%s size=%u previous=%u inv=%d/%d/%d backup=%d bind=%d write=%d verify=%d verify_skipped=%d",
                         i, file->selected, file->skipped, file->existed,
                         file->source[0] != '\0' ? file->source : "n/a",
                         file->destination[0] != '\0' ? file->destination : "n/a",
                         file->size, file->previous_size,
                         file->inventory_exact_rc, file->inventory_parent_rc,
                         file->inventory_open_rc, file->backup_rc,
                         file->bind_rc, file->write_rc, file->verify_rc,
                         file->verify_skipped);
    }
    return rc;
}
