/*
 * PS2 Memory Card Inspector - optional FMCB install planning
 *
 * This is an independent, payload-free description of a normal FreeMcBoot
 * package plus a read-only mass: source backend.  Nothing in this file writes
 * to a memory card.  The eventual commit path will remain a separate stage so
 * package/card/MagicGate preflight can be audited independently.
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

extern unsigned char iomanX_irx[];
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];
extern unsigned int size_fileXio_irx;
extern unsigned char usbd_irx[];
extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[];
extern unsigned int size_usbhdfsd_irx;

static const FmcbPackageEntry NormalInstallManifest[] = {
    {"SYSTEM/FMCB.XLF",       "REGION_SYSTEM/osdmain.elf", FMCB_FILE_REQUIRED | FMCB_FILE_KELF},
    {"SYSTEM/ENDVDPL.XRX",    "SYS-CONF/endvdpl.irx",     FMCB_FILE_REQUIRED | FMCB_FILE_KELF},
    {"SYSTEM/FMCB.ICN",       "REGION_SYSTEM/FMCB.icn",   FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYSTEM/ICON.SYS",       "REGION_SYSTEM/icon.sys",   FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/FMCB_CFG.ELF", "SYS-CONF/FMCB_CFG.ELF",    FMCB_FILE_REQUIRED | FMCB_FILE_CONFIG},
    {"SYS-CONF/FREEMCB.CNF",  "SYS-CONF/FREEMCB.CNF",     FMCB_FILE_REQUIRED | FMCB_FILE_CONFIG},
    {"SYS-CONF/ICON.SYS",     "SYS-CONF/icon.sys",        FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/SYSCONF.ICN",  "SYS-CONF/sysconf.icn",     FMCB_FILE_REQUIRED | FMCB_FILE_RESOURCE},
    {"SYS-CONF/USBD.IRX",     "SYS-CONF/USBD.IRX",        FMCB_FILE_OPTIONAL},
    {"SYS-CONF/USBHDFSD.IRX", "SYS-CONF/USBHDFSD.IRX",    FMCB_FILE_OPTIONAL}
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
    int rc;

    rc = SifExecModuleBuffer((void *)module, size, 0, NULL, &result);
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

void FmcbBuildInstallPlan(int target_port, char region_letter,
                          FmcbInstallPlan *plan)
{
    int i;

    memset(plan, 0, sizeof(*plan));
    plan->target_port = target_port;
    plan->region_letter = region_letter;

    if (region_letter == 'I' || region_letter == 'A' ||
        region_letter == 'E' || region_letter == 'C') {
        snprintf(plan->destination_system, sizeof(plan->destination_system),
                 "B%cEXEC-SYSTEM", region_letter);
    } else {
        snprintf(plan->destination_system, sizeof(plan->destination_system),
                 "UNKNOWN");
    }

    for (i = 0; i < FmcbPackageEntryCount(); i++) {
        const FmcbPackageEntry *entry = &NormalInstallManifest[i];

        if (entry->flags & FMCB_FILE_REQUIRED)
            plan->required_files++;
        if (entry->flags & FMCB_FILE_KELF)
            plan->kelf_files++;
        if (entry->flags & FMCB_FILE_CONFIG)
            plan->config_files++;
        if (entry->flags & FMCB_FILE_RESOURCE)
            plan->resource_files++;
        if (entry->flags & FMCB_FILE_OPTIONAL)
            plan->optional_files++;
    }

    plan->package_complete = 0;
    plan->magicgate_required = (plan->kelf_files > 0);
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

    rc = SifLoadFileInit();
    if (rc < 0)
        return rc;

    SifInitIopHeap();
    sbv_patch_enable_lmb();

    rc = ExecEmbedded(iomanX_irx, size_iomanX_irx, &start_rc);
    status->iomanx_rc = rc;
    if (rc < 0)
        goto out;

    rc = ExecEmbedded(fileXio_irx, size_fileXio_irx, &start_rc);
    status->filexio_module_rc = rc;
    if (rc < 0)
        goto out;

    rc = ExecEmbedded(usbd_irx, size_usbd_irx, &start_rc);
    status->usbd_rc = rc;
    if (rc < 0)
        goto out;

    rc = ExecEmbedded(usbhdfsd_irx, size_usbhdfsd_irx, &start_rc);
    status->usbhdfsd_rc = rc;

out:
    SifExitIopHeap();
    SifLoadFileExit();

    if (rc < 0)
        return rc;

    status->filexio_init_rc = fileXioInit();
    if (status->filexio_init_rc < 0)
        return status->filexio_init_rc;

    /* Give USB mass-storage enumeration a short head start.  A later manual
     * preflight can always be retried if the device was inserted afterwards. */
    DelayThread(250000);
    status->available = 1;
    return 0;
}

static char DetectRegionLetter(char *romver_region)
{
    char romver[16];
    int fd;
    int rc;

    memset(romver, 0, sizeof(romver));
    if (romver_region != NULL)
        *romver_region = '?';

    fd = fileXioOpen("rom0:ROMVER", FIO_O_RDONLY);
    if (fd < 0)
        return '?';

    rc = fileXioRead(fd, romver, sizeof(romver) - 1);
    fileXioClose(fd);
    if (rc <= 4)
        return '?';

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

static int ProbeRoot(const char *root, int target_port, FmcbPackageReport *report)
{
    iox_stat_t stat;
    char full_path[FMCB_PATH_MAX + FMCB_SOURCE_ROOT_MAX + 4];
    char region;
    char romver_region;
    int i;
    int rc;

    region = DetectRegionLetter(&romver_region);
    FmcbBuildInstallPlan(target_port, region, &report->plan);
    report->plan.romver_region = romver_region;
    report->entry_count = FmcbPackageEntryCount();
    snprintf(report->source_root, sizeof(report->source_root), "%s", root);

    if (region == '?') {
        report->status = FMCB_PACKAGE_UNSUPPORTED_CONSOLE;
        return -1;
    }

    for (i = 0; i < report->entry_count && i < FMCB_MAX_PACKAGE_ENTRIES; i++) {
        const FmcbPackageEntry *entry = &NormalInstallManifest[i];
        FmcbPackageFileStatus *file = &report->files[i];

        memset(file, 0, sizeof(*file));
        file->flags = entry->flags;
        file->stat_rc = -999;
        snprintf(file->relative_path, sizeof(file->relative_path), "%s",
                 entry->source_path);
        snprintf(full_path, sizeof(full_path), "%s/%s", root,
                 entry->source_path);

        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(full_path, &stat);
        file->stat_rc = rc;
        if (rc >= 0 && stat.size > 0) {
            file->found = 1;
            file->size = stat.size;
            report->total_found_bytes += stat.size;
            if (entry->flags & FMCB_FILE_REQUIRED)
                report->found_required++;
            if (entry->flags & FMCB_FILE_OPTIONAL)
                report->found_optional++;
        } else if (entry->flags & FMCB_FILE_REQUIRED) {
            report->missing_required++;
        }
    }

    report->plan.package_complete = (report->missing_required == 0);
    report->status = report->plan.package_complete ? FMCB_PACKAGE_READY
                                                    : FMCB_PACKAGE_INCOMPLETE;
    return report->plan.package_complete ? 0 : -1;
}

int FmcbProbeMassPackage(int target_port, const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report)
{
    iox_stat_t stat;
    unsigned int i;
    int rc;

    FmcbResetPackageReport(report, target_port);

    if (backend == NULL || !backend->available) {
        report->status = FMCB_PACKAGE_SOURCE_UNAVAILABLE;
        report->source_probe_rc = -1;
        return -1;
    }

    for (i = 0; i < sizeof(MassRoots) / sizeof(MassRoots[0]); i++) {
        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(MassRoots[i], &stat);
        report->source_probe_rc = rc;
        if (rc >= 0)
            return ProbeRoot(MassRoots[i], target_port, report);
    }

    report->status = FMCB_PACKAGE_NOT_FOUND;
    return -1;
}

const char *FmcbPackageStatusText(FmcbPackageStatus status)
{
    switch (status) {
        case FMCB_PACKAGE_SOURCE_UNAVAILABLE: return "SOURCE UNAVAILABLE";
        case FMCB_PACKAGE_NOT_FOUND: return "PACKAGE NOT FOUND";
        case FMCB_PACKAGE_INCOMPLETE: return "INCOMPLETE";
        case FMCB_PACKAGE_READY: return "READY (READ-ONLY PREFLIGHT)";
        case FMCB_PACKAGE_UNSUPPORTED_CONSOLE: return "UNSUPPORTED/UNKNOWN REGION";
        default: return "NOT SCANNED";
    }
}
