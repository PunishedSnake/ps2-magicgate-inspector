/*
 * PS2 Memory Card Inspector - FMCB normal-install planning and USB preflight
 *
 * The manifest follows the normal CEX install shape used by FreeMcBoot while
 * remaining payload-free: the user supplies the package under mass:/FMCB.
 * 0.4 resolves region icon names and legacy OSDSYS destination filenames here
 * so preflight and the later transaction engine cannot disagree.
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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "fmcb_install.h"
#include "progress.h"

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
    {"SYSTEM/ENDVDPL.XRX",    "SYS-CONF/endvdpl.irx",   FMCB_FILE_REQUIRED | FMCB_FILE_KELF},
    {"SYSTEM/FMCB.ICN",       "REGION_SYSTEM/FMCB.icn", FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYSTEM/B?ICON.SYS",     "REGION_SYSTEM/icon.sys", FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/FMCB_CFG.ELF", "SYS-CONF/FMCB_CFG.ELF",  FMCB_FILE_REQUIRED | FMCB_FILE_CONFIG},
    {"SYS-CONF/FREEMCB.CNF",  "SYS-CONF/FREEMCB.CNF",   FMCB_FILE_REQUIRED | FMCB_FILE_CONFIG},
    {"SYS-CONF/ICON.SYS",     "SYS-CONF/icon.sys",      FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/SYSCONF.ICN",  "SYS-CONF/sysconf.icn",   FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/USBD.IRX",     "SYS-CONF/USBD.IRX",      FMCB_FILE_REQUIRED},
    {"SYS-CONF/USBHDFSD.IRX", "SYS-CONF/USBHDFSD.IRX",  FMCB_FILE_REQUIRED}
};

static const char *MassRoots[] = {
    "mass:/FMCB",
    "mass0:/FMCB",
    "mass1:/FMCB"
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

static void ResolveOsdName(unsigned int rom_version, char out[32])
{
    if (rom_version < 0x0130u) {
        unsigned int version;
        if (rom_version == 0x0100u || rom_version == 0x0101u)
            version = 0x0130u;
        else
            version = (rom_version + 0x000Fu) & ~0x000Fu;
        snprintf(out, 32, "osd%03X.elf", version >> 4);
    } else {
        snprintf(out, 32, "osdmain.elf");
    }
}

void FmcbBuildInstallPlan(int target_port, char region_letter,
                          unsigned int rom_version, FmcbInstallPlan *plan)
{
    int i;

    memset(plan, 0, sizeof(*plan));
    plan->target_port = target_port;
    plan->region_letter = region_letter;
    plan->rom_version = rom_version;

    if (region_letter == 'I' || region_letter == 'A' ||
        region_letter == 'E' || region_letter == 'C') {
        snprintf(plan->destination_system, sizeof(plan->destination_system),
                 "B%cEXEC-SYSTEM", region_letter);
    } else {
        snprintf(plan->destination_system, sizeof(plan->destination_system),
                 "UNKNOWN");
    }
    ResolveOsdName(rom_version, plan->destination_osd);

    for (i = 0; i < FmcbPackageEntryCount(); i++) {
        const FmcbPackageEntry *entry = &NormalInstallManifest[i];
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
                      "Starting the EE/IOP file service used for ROMVER and USB package access.");
    rc = ExecEmbedded(fileXio_irx, size_fileXio_irx, &start_rc);
    status->filexio_module_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 54, "Loading the USB device stack",
                      "Starting USBD before the mass-storage filesystem driver.");
    rc = ExecEmbedded(usbd_irx, size_usbd_irx, &start_rc);
    status->usbd_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 70, "Loading the USB mass-storage driver",
                      "Starting USBHDFSD so a user-supplied FMCB package can be read from mass:.");
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
                      "mass: access is available for FMCB package discovery and installation sources.");
    return 0;
}

void FmcbShutdownMassBackend(FmcbMassBackendStatus *status)
{
    if (status != NULL && status->available)
        fileXioExit();
    if (status != NULL)
        status->available = 0;
}

static char DetectConsole(char *romver_region, unsigned int *rom_version)
{
    char romver[17];
    char version_text[5];
    int fd;
    int rc;

    memset(romver, 0, sizeof(romver));
    if (romver_region != NULL) *romver_region = '?';
    if (rom_version != NULL) *rom_version = 0;

    fd = fileXioOpen("rom0:ROMVER", FIO_O_RDONLY);
    if (fd < 0)
        return '?';
    rc = fileXioRead(fd, romver, sizeof(romver) - 1);
    fileXioClose(fd);
    if (rc <= 4)
        return '?';

    memcpy(version_text, romver, 4);
    version_text[4] = '\0';
    if (rom_version != NULL)
        *rom_version = (unsigned int)strtoul(version_text, NULL, 16);
    if (romver_region != NULL)
        *romver_region = romver[4];

    switch (romver[4]) {
        case 'J': return 'I';
        case 'A':
        case 'H': return 'A';
        case 'E': return 'E';
        case 'C': return 'C';
        default: return '?';
    }
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

static int ProbeRoot(const char *root, int target_port, FmcbPackageReport *report)
{
    iox_stat_t stat;
    char full_path[FMCB_PATH_MAX + FMCB_SOURCE_ROOT_MAX + 4];
    char relative[FMCB_PATH_MAX];
    char detail[224];
    char region;
    char romver_region;
    unsigned int rom_version;
    int i;
    int rc;
    int count;

    MciProgressUpdate(MCI_PROGRESS_FMCB, 20, "Reading ROMVER",
                      "Resolving console ROM revision and MagicGate region for exact FMCB destinations.");
    region = DetectConsole(&romver_region, &rom_version);

    MciProgressUpdate(MCI_PROGRESS_FMCB, 26, "Building the install plan",
                      "Resolving region-specific icons, OSDSYS filename and required KELF/resources.");
    FmcbBuildInstallPlan(target_port, region, rom_version, &report->plan);
    report->plan.romver_region = romver_region;
    report->entry_count = FmcbPackageEntryCount();
    snprintf(report->source_root, sizeof(report->source_root), "%s", root);

    if (region == '?') {
        report->status = FMCB_PACKAGE_UNSUPPORTED_CONSOLE;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "Preflight cannot resolve this console region",
                          "ROMVER did not map to a supported normal-install destination; no card writes were attempted.");
        return -1;
    }

    count = report->entry_count;
    for (i = 0; i < count && i < FMCB_MAX_PACKAGE_ENTRIES; i++) {
        const FmcbPackageEntry *entry = &NormalInstallManifest[i];
        FmcbPackageFileStatus *file = &report->files[i];
        int percent = 30 + ((i * 60) / (count > 0 ? count : 1));

        ResolveSourcePath(entry, region, relative, sizeof(relative));
        memset(file, 0, sizeof(*file));
        file->flags = entry->flags;
        file->stat_rc = -999;
        snprintf(file->relative_path, sizeof(file->relative_path), "%s", relative);
        snprintf(full_path, sizeof(full_path), "%s/%s", root, relative);

        snprintf(detail, sizeof(detail), "Checking %s (%s).", relative,
                 (entry->flags & FMCB_FILE_REQUIRED) ? "required" : "optional");
        MciProgressUpdate(MCI_PROGRESS_FMCB, percent,
                          "Scanning the package manifest", detail);

        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(full_path, &stat);
        file->stat_rc = rc;
        if (rc >= 0 && stat.size > 0) {
            file->found = 1;
            file->size = stat.size;
            report->total_found_bytes += stat.size;
            if (entry->flags & FMCB_FILE_REQUIRED) report->found_required++;
            if (entry->flags & FMCB_FILE_OPTIONAL) report->found_optional++;
        } else if (entry->flags & FMCB_FILE_REQUIRED) {
            report->missing_required++;
        }
    }

    report->plan.package_complete = (report->missing_required == 0);
    report->status = report->plan.package_complete ? FMCB_PACKAGE_READY
                                                    : FMCB_PACKAGE_INCOMPLETE;
    snprintf(detail, sizeof(detail),
             "Found %d/%d required; %d missing. Target %s/%s, ROM %04X.",
             report->found_required, report->plan.required_files,
             report->missing_required, report->plan.destination_system,
             report->plan.destination_osd, report->plan.rom_version);
    MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                      "FMCB package preflight complete", detail);
    return report->plan.package_complete ? 0 : -1;
}

int FmcbProbeMassPackage(int target_port, const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report)
{
    iox_stat_t stat;
    char detail[192];
    unsigned int i;
    int rc;

    FmcbResetPackageReport(report, target_port);
    MciProgressUpdate(MCI_PROGRESS_FMCB, 3, "Checking the USB package backend",
                      "Verifying that fileXio and mass: are available before inspecting the install package.");
    if (backend == NULL || !backend->available) {
        report->status = FMCB_PACKAGE_SOURCE_UNAVAILABLE;
        report->source_probe_rc = -1;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100, "FMCB package source unavailable",
                          "The USB mass-storage backend is not active; no package files could be inspected.");
        return -1;
    }

    for (i = 0; i < sizeof(MassRoots) / sizeof(MassRoots[0]); i++) {
        int percent = 8 + (int)i * 4;
        snprintf(detail, sizeof(detail), "Looking for the package root at %s.", MassRoots[i]);
        MciProgressUpdate(MCI_PROGRESS_FMCB, percent, "Locating the FMCB package", detail);
        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(MassRoots[i], &stat);
        report->source_probe_rc = rc;
        if (rc >= 0)
            return ProbeRoot(MassRoots[i], target_port, report);
    }

    report->status = FMCB_PACKAGE_NOT_FOUND;
    MciProgressUpdate(MCI_PROGRESS_FMCB, 100, "FMCB package not found",
                      "Checked mass:/FMCB, mass0:/FMCB and mass1:/FMCB; no readable package root was found.");
    return -1;
}

const char *FmcbPackageStatusText(FmcbPackageStatus status)
{
    switch (status) {
        case FMCB_PACKAGE_SOURCE_UNAVAILABLE: return "SOURCE UNAVAILABLE";
        case FMCB_PACKAGE_NOT_FOUND: return "PACKAGE NOT FOUND";
        case FMCB_PACKAGE_INCOMPLETE: return "INCOMPLETE";
        case FMCB_PACKAGE_READY: return "READY FOR VERIFIED INSTALL";
        case FMCB_PACKAGE_UNSUPPORTED_CONSOLE: return "UNSUPPORTED/UNKNOWN REGION";
        default: return "NOT SCANNED";
    }
}
