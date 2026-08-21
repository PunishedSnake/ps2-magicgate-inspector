/*
 * PS2 Memory Card Inspector - optional FMCB install planning
 *
 * This is an independent, payload-free description of the minimum normal
 * FreeMcBoot package we expect a user to provide locally.  No code here writes
 * to a memory card.  Later stages will plug a source backend into this manifest
 * and only enable the destructive commit path after package + card + MagicGate
 * preflight all succeed.
 */

#include <string.h>

#include "fmcb_install.h"

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

    /* Actual source probing will set package_complete in the next stage. */
    plan->package_complete = 0;
    plan->magicgate_required = (plan->kelf_files > 0);
}
