#ifndef MCI_FMCB_INSTALL_H
#define MCI_FMCB_INSTALL_H

/*
 * Optional FreeMcBoot installation support.
 *
 * The Inspector never embeds FreeMcBoot payloads.  Briscoe only discovers and
 * validates files supplied by the user.  The API below is deliberately split
 * into source bring-up, read-only package preflight and (future) commit stages.
 */

#define FMCB_MAX_PACKAGE_ENTRIES 16
#define FMCB_SOURCE_ROOT_MAX 32
#define FMCB_PATH_MAX 128

typedef enum FmcbFileFlags {
    FMCB_FILE_REQUIRED = 1 << 0,
    FMCB_FILE_KELF = 1 << 1,
    FMCB_FILE_CONFIG = 1 << 2,
    FMCB_FILE_RESOURCE = 1 << 3,
    FMCB_FILE_OPTIONAL = 1 << 4
} FmcbFileFlags;

typedef enum FmcbPackageStatus {
    FMCB_PACKAGE_NOT_SCANNED = 0,
    FMCB_PACKAGE_SOURCE_UNAVAILABLE,
    FMCB_PACKAGE_NOT_FOUND,
    FMCB_PACKAGE_INCOMPLETE,
    FMCB_PACKAGE_READY,
    FMCB_PACKAGE_UNSUPPORTED_CONSOLE
} FmcbPackageStatus;

typedef struct FmcbPackageEntry {
    const char *source_path;
    const char *destination_kind;
    unsigned int flags;
} FmcbPackageEntry;

typedef struct FmcbInstallPlan {
    int target_port;
    char region_letter;
    char romver_region;
    char destination_system[24];
    int required_files;
    int kelf_files;
    int config_files;
    int resource_files;
    int optional_files;
    int package_complete;
    int magicgate_required;
} FmcbInstallPlan;

typedef struct FmcbPackageFileStatus {
    int found;
    int stat_rc;
    unsigned int size;
    unsigned int flags;
    char relative_path[FMCB_PATH_MAX];
} FmcbPackageFileStatus;

typedef struct FmcbMassBackendStatus {
    int available;
    int iomanx_rc;
    int filexio_module_rc;
    int usbd_rc;
    int usbhdfsd_rc;
    int filexio_init_rc;
} FmcbMassBackendStatus;

typedef struct FmcbPackageReport {
    FmcbPackageStatus status;
    FmcbInstallPlan plan;
    char source_root[FMCB_SOURCE_ROOT_MAX];
    int source_probe_rc;
    int entry_count;
    int found_required;
    int missing_required;
    int found_optional;
    unsigned int total_found_bytes;
    FmcbPackageFileStatus files[FMCB_MAX_PACKAGE_ENTRIES];
} FmcbPackageReport;

/* Baseline normal-install manifest; early-Japanese update files stay separate. */
int FmcbPackageEntryCount(void);
const FmcbPackageEntry *FmcbPackageEntryAt(int index);

void FmcbBuildInstallPlan(int target_port, char region_letter,
                          FmcbInstallPlan *plan);

/* Optional PS2SDK USB/fileXio source stack. Failure never disables Inspector. */
int FmcbInitMassBackend(FmcbMassBackendStatus *status);

/*
 * Scan user-supplied packages under mass:/FMCB, mass0:/FMCB and mass1:/FMCB.
 * This is strictly read-only: no directory creation, KELF rebinding or memory
 * card writes are performed here.
 */
void FmcbResetPackageReport(FmcbPackageReport *report, int target_port);
int FmcbProbeMassPackage(int target_port, const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report);

const char *FmcbPackageStatusText(FmcbPackageStatus status);

#endif /* MCI_FMCB_INSTALL_H */
