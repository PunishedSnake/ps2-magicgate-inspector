/*
 * PS2 Memory Card Inspector - FMCB normal-install planning and USB preflight
 *
 * The system-update destination is selected from the console's active runtime
 * profile, not from a sticker assumption. ROMVER owns the active OSDSYS lookup
 * path while console_profile.c cross-checks Dragon/Deckard MechaCon state and a
 * read-only MechaPwn NVM fingerprint. Deckard CEX region switching is rejected
 * here until the transaction engine can cover all region destinations safely.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <kernel.h>
#include <delaythread.h>
#include <iopheap.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <string.h>
#include <stdio.h>

#include "fmcb_install.h"
#include "progress.h"
#include "usb_search.h"

extern unsigned char iomanX_irx[];
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];
extern unsigned int size_fileXio_irx;
extern unsigned char usbd_irx[];
extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[];
extern unsigned int size_usbhdfsd_irx;

static const FmcbPackageEntry NormalInstallManifest[] = {
    {"SYSTEM/FMCB.XLF",       "REGION_SYSTEM/OSD",       FMCB_FILE_REQUIRED | FMCB_FILE_KELF},
    /* The reference FMCB installer omits ENDVDPL on a real DEX ROM. We preserve
     * that proven distinction, but do not yet assume that every DEX-like
     * MechaPwn configuration can omit it until that compact path is tested on
     * hardware. */
    {"SYSTEM/ENDVDPL.XRX",    "SYS-CONF/endvdpl.irx",   FMCB_FILE_REQUIRED | FMCB_FILE_KELF | FMCB_FILE_CEX_ONLY},
    {"SYSTEM/FMCB.ICN",       "REGION_SYSTEM/FMCB.icn", FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYSTEM/B?ICON.SYS",     "REGION_SYSTEM/icon.sys", FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/FMCB_CFG.ELF", "SYS-CONF/FMCB_CFG.ELF",  FMCB_FILE_REQUIRED | FMCB_FILE_CONFIG},
    {"SYS-CONF/FREEMCB.CNF",  "SYS-CONF/FREEMCB.CNF",   FMCB_FILE_REQUIRED | FMCB_FILE_CONFIG},
    {"SYS-CONF/ICON.SYS",     "SYS-CONF/icon.sys",      FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/SYSCONF.ICN",  "SYS-CONF/sysconf.icn",   FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/USBD.IRX",     "SYS-CONF/USBD.IRX",      FMCB_FILE_REQUIRED},
    {"SYS-CONF/USBHDFSD.IRX", "SYS-CONF/USBHDFSD.IRX",  FMCB_FILE_REQUIRED}
};

static int ExecEmbedded(const unsigned char *module, unsigned int size,
                        int *start_result)
{
    int result = -999;
    int rc = SifExecModuleBuffer((void *)module, size, 0, NULL, &result);
    if (start_result != NULL)
        *start_result = result;
    return rc;
}

int FmcbPackageEntryCount(void)
{
    return (int)(sizeof(NormalInstallManifest) / sizeof(NormalInstallManifest[0]));
}

const FmcbPackageEntry *FmcbPackageEntryAt(int index)
{
    if (index < 0 || index >= FmcbPackageEntryCount())
        return NULL;
    return &NormalInstallManifest[index];
}

int FmcbPackageEntrySelected(const FmcbInstallPlan *plan, int index)
{
    const FmcbPackageEntry *entry = FmcbPackageEntryAt(index);

    if (plan == NULL || entry == NULL)
        return 0;
    if ((entry->flags & FMCB_FILE_CEX_ONLY) && plan->console.rom_is_dex)
        return 0;
    return 1;
}

static void ResolveOsdName(unsigned int rom_version, char out[32])
{
    if (rom_version < 0x0130u) {
        unsigned int version;

        /* Match the reference FMCB installer exactly. v1.00/v1.01 require the
         * patched 1.30 update; other pre-1.30 ROMs look for the next 0x10
         * update filename. The value is already expressed as hexadecimal
         * hundredths, so there is deliberately no >> 4 here. */
        if (rom_version == 0x0100u || rom_version == 0x0101u)
            version = 0x0130u;
        else
            version = (rom_version + 0x0010u) & ~0x000Fu;
        snprintf(out, 32, "osd%03X.elf", version);
    } else {
        snprintf(out, 32, "osdmain.elf");
    }
}

void FmcbBuildInstallPlan(int target_port, const MciConsoleProfile *console,
                          FmcbInstallPlan *plan)
{
    int i;

    memset(plan, 0, sizeof(*plan));
    plan->target_port = target_port;
    if (console != NULL)
        plan->console = *console;
    else {
        plan->console.romver_region = '?';
        plan->console.mg_folder_region = '?';
    }

    plan->romver_region = plan->console.romver_region;
    plan->rom_version = plan->console.rom_version;
    plan->region_letter = plan->console.mg_folder_region;

    if (plan->region_letter == 'I' || plan->region_letter == 'A' ||
        plan->region_letter == 'E' || plan->region_letter == 'C') {
        snprintf(plan->destination_system, sizeof(plan->destination_system),
                 "B%cEXEC-SYSTEM", plan->region_letter);
    } else {
        snprintf(plan->destination_system, sizeof(plan->destination_system),
                 "UNKNOWN");
    }
    ResolveOsdName(plan->rom_version, plan->destination_osd);

    plan->compact_unlock_candidate = plan->console.compact_region_safe &&
                                     !plan->console.rom_is_dex;
    plan->compact_unlock_active = plan->console.rom_is_dex;

    for (i = 0; i < FmcbPackageEntryCount(); i++) {
        const FmcbPackageEntry *entry = &NormalInstallManifest[i];

        if (!FmcbPackageEntrySelected(plan, i))
            continue;
        plan->selected_files++;
        if (entry->flags & FMCB_FILE_REQUIRED) plan->required_files++;
        if (entry->flags & FMCB_FILE_KELF) plan->kelf_files++;
        if (entry->flags & FMCB_FILE_CONFIG) plan->config_files++;
        if (entry->flags & FMCB_FILE_RESOURCE) plan->resource_files++;
        if (entry->flags & FMCB_FILE_OPTIONAL) plan->optional_files++;
    }

    plan->package_complete = 0;
    plan->magicgate_required = (plan->kelf_files > 0);
}

int FmcbResolveDestination(const FmcbInstallPlan *plan, int entry_index,
                           char *path, unsigned int path_size)
{
    const FmcbPackageEntry *entry;
    const char *suffix;

    if (plan == NULL || path == NULL || path_size < 8u)
        return -1;
    entry = FmcbPackageEntryAt(entry_index);
    if (entry == NULL)
        return -2;

    if (strncmp(entry->destination_path, "REGION_SYSTEM/", 14) == 0) {
        suffix = entry->destination_path + 14;
        if (strcmp(suffix, "OSD") == 0)
            suffix = plan->destination_osd;
        snprintf(path, path_size, "/%s/%s", plan->destination_system, suffix);
    } else {
        snprintf(path, path_size, "/%s", entry->destination_path);
    }
    return 0;
}

int FmcbInitMassBackend(FmcbMassBackendStatus *status)
{
    int rc;
    int start_rc;

    memset(status, 0, sizeof(*status));
    status->iomanx_rc = -999;
    status->filexio_module_rc = -999;
    status->usbd_rc = -999;
    status->usbhdfsd_rc = -999;
    status->filexio_init_rc = -999;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 8,
                      "Preparing the USB package backend",
                      "Initializing LOADFILE and the IOP heap before starting fileXio and USB modules.");
    rc = SifLoadFileInit();
    if (rc < 0)
        return rc;

    SifInitIopHeap();
    sbv_patch_enable_lmb();

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 22, "Loading iomanX",
                      "Starting the extended IOP I/O manager used by fileXio and mass:.");
    rc = ExecEmbedded(iomanX_irx, size_iomanX_irx, &start_rc);
    status->iomanx_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 38, "Loading fileXio",
                      "Starting the EE/IOP file service used for ROMVER, MechaCon-aware preflight and USB package access.");
    rc = ExecEmbedded(fileXio_irx, size_fileXio_irx, &start_rc);
    status->filexio_module_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 54, "Loading the USB device stack",
                      "Starting USBD before the mass-storage filesystem driver.");
    rc = ExecEmbedded(usbd_irx, size_usbd_irx, &start_rc);
    status->usbd_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 70, "Loading the USB mass-storage driver",
                      "Starting USBHDFSD so a user-supplied FMCB package and recovery journal can use mass:.");
    rc = ExecEmbedded(usbhdfsd_irx, size_usbhdfsd_irx, &start_rc);
    status->usbhdfsd_rc = rc;

out:
    SifExitIopHeap();
    SifLoadFileExit();
    if (rc < 0)
        return rc;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 86, "Connecting the fileXio client",
                      "Binding the EE fileXio RPC client and waiting briefly for USB enumeration.");
    status->filexio_init_rc = fileXioInit();
    if (status->filexio_init_rc < 0)
        return status->filexio_init_rc;

    DelayThread(250000);
    status->available = 1;
    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 100, "USB package backend ready",
                      "mass: access is available for FMCB package discovery, installation sources and recovery state.");
    return 0;
}

void FmcbShutdownMassBackend(FmcbMassBackendStatus *status)
{
    if (status != NULL && status->available)
        fileXioExit();
    if (status != NULL)
        status->available = 0;
}

void FmcbResetPackageReport(FmcbPackageReport *report, int target_port)
{
    memset(report, 0, sizeof(*report));
    report->status = FMCB_PACKAGE_NOT_SCANNED;
    report->source_probe_rc = -999;
    report->plan.target_port = target_port;
}

static void ResolveSourcePath(const FmcbPackageEntry *entry, char region,
                              char *path, unsigned int path_size)
{
    snprintf(path, path_size, "%s", entry->source_path);
    if (strstr(path, "B?ICON.SYS") != NULL) {
        char *q = strchr(path, '?');
        if (q != NULL)
            *q = region;
    }
}

static void PackageSearchProgress(const char *path,
                                  unsigned int directories_scanned,
                                  void *userdata)
{
    char detail[224];
    int percent = 8 + (int)(directories_scanned / 10u);

    (void)userdata;
    if (percent > 15)
        percent = 15;
    snprintf(detail, sizeof(detail),
             "Searched %u folders; checking %.150s",
             directories_scanned, path != NULL ? path : "USB storage");
    MciProgressUpdate(MCI_PROGRESS_FMCB, percent,
                      "Searching USB storage for a FreeMcBoot package", detail);
}

static int ProbeRoot(const char *root, int target_port, FmcbPackageReport *report)
{
    iox_stat_t stat;
    MciConsoleProfile console;
    char full_path[FMCB_PATH_MAX + FMCB_SOURCE_ROOT_MAX + 4];
    char relative[FMCB_PATH_MAX];
    char detail[256];
    int profile_rc;
    int i;
    int rc;
    int count;

    MciProgressUpdate(MCI_PROGRESS_FMCB, 18, "Profiling ROMVER, MechaCon and NVM",
                      "Reading active ROMVER, MechaCon revision, region diagnostics and the read-only MechaPwn key-seed fingerprint before choosing an FMCB policy.");
    profile_rc = MciConsoleProfileProbe(&console);

    MciProgressUpdate(MCI_PROGRESS_FMCB, 26, "Building the install plan",
                      "Resolving the active system-update folder and rejecting region-switching states that require a verified cross-region transaction.");
    FmcbBuildInstallPlan(target_port, &console, &report->plan);
    report->entry_count = FmcbPackageEntryCount();
    snprintf(report->source_root, sizeof(report->source_root), "%s", root);

    if (profile_rc < 0 || console.mg_folder_region == '?' || console.is_psx) {
        report->status = FMCB_PACKAGE_UNSUPPORTED_CONSOLE;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "Preflight cannot resolve a supported PS2 target",
                          "The active ROMVER/console type did not map to a supported normal FMCB destination; no card writes were attempted.");
        return -1;
    }
    if (console.region_mismatch) {
        report->status = FMCB_PACKAGE_REGION_AMBIGUOUS;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "MechaPwn region transition is not settled",
                          "The detected Deckard DEX-like policy expects the A system-update region, but active ROMVER has not converged to it. Reboot the console before installing.");
        return -1;
    }
    if (console.cross_region_required) {
        report->status = FMCB_PACKAGE_CROSS_REGION_REQUIRED;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "Deckard MechaPwn CEX needs cross-region FMCB",
                          "A one-region install could stop booting after a later MechaPwn CEX region change. This build blocks writes until the verified transaction engine covers every regional system-update destination.");
        return -1;
    }

    count = report->entry_count;
    for (i = 0; i < count && i < FMCB_MAX_PACKAGE_ENTRIES; i++) {
        const FmcbPackageEntry *entry = &NormalInstallManifest[i];
        FmcbPackageFileStatus *file = &report->files[i];
        int percent = 30 + ((i * 60) / (count > 0 ? count : 1));

        ResolveSourcePath(entry, report->plan.region_letter, relative,
                          sizeof(relative));
        memset(file, 0, sizeof(*file));
        file->flags = entry->flags;
        file->selected = FmcbPackageEntrySelected(&report->plan, i);
        file->stat_rc = -999;
        snprintf(file->relative_path, sizeof(file->relative_path), "%s", relative);
        snprintf(full_path, sizeof(full_path), "%s/%s", root, relative);

        snprintf(detail, sizeof(detail), "Checking %s (%s%s).", relative,
                 file->selected ? "selected" : "omitted for real DEX",
                 (entry->flags & FMCB_FILE_REQUIRED) ? ", required" : "");
        MciProgressUpdate(MCI_PROGRESS_FMCB, percent,
                          "Scanning the package manifest", detail);

        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(full_path, &stat);
        file->stat_rc = rc;
        if (rc >= 0 && stat.size > 0) {
            file->found = 1;
            file->size = stat.size;
            if (file->selected) {
                report->total_found_bytes += stat.size;
                if (entry->flags & FMCB_FILE_REQUIRED) report->found_required++;
                if (entry->flags & FMCB_FILE_OPTIONAL) report->found_optional++;
            }
            if ((entry->flags & FMCB_FILE_CEX_ONLY) &&
                (report->plan.compact_unlock_active ||
                 report->plan.compact_unlock_candidate))
                report->plan.compact_possible_savings += stat.size;
        } else if (file->selected && (entry->flags & FMCB_FILE_REQUIRED)) {
            report->missing_required++;
        }
    }

    report->plan.package_complete = (report->missing_required == 0);
    report->status = report->plan.package_complete ? FMCB_PACKAGE_READY
                                                    : FMCB_PACKAGE_INCOMPLETE;
    snprintf(detail, sizeof(detail),
             "Found %d/%d required; target %s/%s. ROM %04X %c, Mecha %u.%02u, NVM sig=%s, policy: %s.",
             report->found_required, report->plan.required_files,
             report->plan.destination_system, report->plan.destination_osd,
             report->plan.rom_version, report->plan.romver_region,
             report->plan.console.mecha_major,
             report->plan.console.mecha_minor,
             report->plan.console.mechapwn_signature ? "MechaPwn" : "none",
             MciConsoleRegionPolicyText(&report->plan.console));
    MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                      "FMCB package preflight complete", detail);
    return report->plan.package_complete ? 0 : -1;
}

int FmcbProbeMassPackage(int target_port, const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report)
{
    MciConsoleProfile console;
    char xlf_path[MCI_USB_SEARCH_PATH_MAX];
    char package_root[FMCB_SOURCE_ROOT_MAX];
    char detail[256];
    int profile_rc = -1;
    int rc;
    int attempt;

    FmcbResetPackageReport(report, target_port);

    /* Resolve the console before package discovery so the dashboard never
     * reports `Region ?` merely because USB enumeration or a package search
     * failed. A freshly rebuilt IOP can need one short retry before ROM/CDVD
     * services are completely settled. */
    MciProgressUpdate(MCI_PROGRESS_FMCB, 3, "Identifying this PS2",
                      "Reading the active system region and console security profile before looking for FreeMcBoot files.");
    for (attempt = 0; attempt < 3; attempt++) {
        profile_rc = MciConsoleProfileProbe(&console);
        if (profile_rc == 0)
            break;
        if (attempt < 2)
            DelayThread(100000);
    }
    FmcbBuildInstallPlan(target_port, &console, &report->plan);
    report->entry_count = FmcbPackageEntryCount();

    if (profile_rc < 0 || console.mg_folder_region == '?' || console.is_psx) {
        report->status = FMCB_PACKAGE_UNSUPPORTED_CONSOLE;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "Console region could not be resolved",
                          "The active PS2 system region is still unavailable, so installation remains blocked. No card writes were attempted.");
        return -1;
    }
    if (console.region_mismatch) {
        report->status = FMCB_PACKAGE_REGION_AMBIGUOUS;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "MechaPwn region transition is not settled",
                          "The detected Deckard DEX-like policy expects the A system-update region, but active ROMVER has not converged to it. Reboot the console before installing.");
        return -1;
    }
    if (console.cross_region_required) {
        report->status = FMCB_PACKAGE_CROSS_REGION_REQUIRED;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "Deckard MechaPwn CEX needs cross-region FMCB",
                          "A one-region install could stop booting after a later MechaPwn CEX region change. This build blocks writes until the verified transaction covers every regional destination.");
        return -1;
    }

    MciProgressUpdate(MCI_PROGRESS_FMCB, 6, "Checking USB storage",
                      "Waiting for the USB filesystem to become readable. This can take a moment after a MagicGate security-session reboot.");
    if (backend == NULL || !backend->available ||
        MciUsbWaitForStorage(20u, 100000u) < 0) {
        report->status = FMCB_PACKAGE_SOURCE_UNAVAILABLE;
        report->source_probe_rc = -1;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100, "USB storage is not available",
                          "Connect a readable USB drive containing the FreeMcBoot package, then run preflight again.");
        return -1;
    }

    /* A cache entry exists only after the complete manifest has passed once.
     * Reuse its root to avoid a full tree walk, but re-run ProbeRoot so moved,
     * deleted or modified package files are still caught before installation. */
    if (MciUsbGetVerifiedPackageRoot(package_root, sizeof(package_root)) == 0) {
        snprintf(detail, sizeof(detail),
                 "Using the previously verified package at %.170s and rechecking its files.",
                 package_root);
        MciProgressUpdate(MCI_PROGRESS_FMCB, 10,
                          "Using cached FreeMcBoot package", detail);
        report->source_probe_rc = 0;
        rc = ProbeRoot(package_root, target_port, report);
        if (rc == 0 && report->status == FMCB_PACKAGE_READY)
            return 0;
        MciUsbClearVerifiedPackageRoot();
        FmcbResetPackageReport(report, target_port);
        FmcbBuildInstallPlan(target_port, &console, &report->plan);
        report->entry_count = FmcbPackageEntryCount();
    }

    MciProgressUpdate(MCI_PROGRESS_FMCB, 9,
                      "Searching USB storage for a FreeMcBoot package",
                      "Looking recursively for SYSTEM/FMCB.XLF. The package folder may be placed anywhere in visible USB folders.");
    rc = MciUsbFindFmcbXlf(xlf_path, sizeof(xlf_path), 1,
                           PackageSearchProgress, NULL);
    report->source_probe_rc = rc;
    if (rc == 0) {
        rc = MciUsbPackageRootFromXlf(xlf_path, package_root,
                                      sizeof(package_root));
        if (rc == 0) {
            snprintf(detail, sizeof(detail),
                     "Found FreeMcBoot at %.170s. Checking the complete installer package now.",
                     package_root);
            MciProgressUpdate(MCI_PROGRESS_FMCB, 16,
                              "FreeMcBoot package found", detail);
            report->source_probe_rc = 0;
            rc = ProbeRoot(package_root, target_port, report);
            if (rc == 0 && report->status == FMCB_PACKAGE_READY)
                (void)MciUsbRememberVerifiedPackageRoot(package_root);
            return rc;
        }
        report->source_probe_rc = rc;
    }

    report->status = FMCB_PACKAGE_NOT_FOUND;
    MciProgressUpdate(MCI_PROGRESS_FMCB, 100, "FreeMcBoot package not found",
                      "No complete-package anchor SYSTEM/FMCB.XLF was found in visible USB folders. A standalone FMCB.XLF can still be used for the MagicGate test.");
    return -1;
}

const char *FmcbPackageStatusText(FmcbPackageStatus status)
{
    switch (status) {
        case FMCB_PACKAGE_SOURCE_UNAVAILABLE: return "SOURCE UNAVAILABLE";
        case FMCB_PACKAGE_NOT_FOUND: return "PACKAGE NOT FOUND";
        case FMCB_PACKAGE_INCOMPLETE: return "INCOMPLETE";
        case FMCB_PACKAGE_READY: return "READY FOR VERIFIED INSTALL";
        case FMCB_PACKAGE_UNSUPPORTED_CONSOLE: return "UNSUPPORTED/UNKNOWN CONSOLE";
        case FMCB_PACKAGE_REGION_AMBIGUOUS: return "MECHAPWN REGION TRANSITION / REBOOT";
        case FMCB_PACKAGE_CROSS_REGION_REQUIRED: return "MECHAPWN CEX: CROSS-REGION REQUIRED";
        default: return "NOT SCANNED";
    }
}
