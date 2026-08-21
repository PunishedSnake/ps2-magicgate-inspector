#ifndef MCI_FMCB_INSTALL_H
#define MCI_FMCB_INSTALL_H

/*
 * Optional FreeMcBoot installation support.
 *
 * This module does not contain or embed FreeMcBoot payloads.  It describes the
 * package contract and installation plan for user-supplied files.  The first
 * Briscoe implementation is deliberately read-only: package discovery and
 * preflight are separated from the future destructive commit stage.
 */

typedef enum FmcbFileFlags {
    FMCB_FILE_REQUIRED = 1 << 0,
    FMCB_FILE_KELF = 1 << 1,
    FMCB_FILE_CONFIG = 1 << 2,
    FMCB_FILE_RESOURCE = 1 << 3,
    FMCB_FILE_OPTIONAL = 1 << 4
} FmcbFileFlags;

typedef struct FmcbPackageEntry {
    const char *source_path;
    const char *destination_kind;
    unsigned int flags;
} FmcbPackageEntry;

typedef struct FmcbInstallPlan {
    int target_port;
    char region_letter;
    int required_files;
    int kelf_files;
    int config_files;
    int resource_files;
    int optional_files;
    int package_complete;
    int magicgate_required;
} FmcbInstallPlan;

/* Baseline normal-install manifest; early-Japanese update files are separate. */
int FmcbPackageEntryCount(void);
const FmcbPackageEntry *FmcbPackageEntryAt(int index);

/*
 * Build a non-destructive plan from manifest metadata.  File existence and
 * sizing are intentionally supplied later by a source backend (mass:, host:,
 * etc.) so installer policy is not tied to one filesystem driver.
 */
void FmcbBuildInstallPlan(int target_port, char region_letter,
                          FmcbInstallPlan *plan);

#endif /* MCI_FMCB_INSTALL_H */
