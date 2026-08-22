#ifndef MCI_FMCB_INSTALL_H
#define MCI_FMCB_INSTALL_H

/*
 * FreeMcBoot package planning and read-only USB preflight.
 *
 * The data model intentionally describes both source files and their eventual
 * destination classes so the future installer can reuse the same validated
 * manifest. Nothing declared here performs a memory-card installation write;
 * Briscoe stops at package discovery/planning until a separate transactional
 * bind -> write -> reopen -> read-back -> verify -> rollback path is validated.
 */

#define FMCB_MAX_PACKAGE_ENTRIES 16
#define FMCB_PATH_MAX 96
#define FMCB_SOURCE_ROOT_MAX 32

#define FMCB_FILE_REQUIRED 0x01
#define FMCB_FILE_OPTIONAL 0x02
#define FMCB_FILE_KELF 0x04
#define FMCB_FILE_CONFIG 0x08
#define FMCB_FILE_RESOURCE 0x10

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
    const char *destination_path;
    unsigned int flags;
} FmcbPackageEntry;

typedef struct FmcbMassBackendStatus {
    int iomanx_rc;
    int filexio_module_rc;
    int usbd_rc;
    int usbhdfsd_rc;
    int filexio_init_rc;
    int available;
} FmcbMassBackendStatus;

typedef struct FmcbPackageFileStatus {
    char relative_path[FMCB_PATH_MAX];
    unsigned int flags;
    int found;
    int stat_rc;
    unsigned int size;
} FmcbPackageFileStatus;

typedef struct FmcbInstallPlan {
    int target_port;
    char romver_region;
    char region_letter;
    char destination_system[32];
    int required_files;
    int optional_files;
    int kelf_files;
    int config_files;
    int resource_files;
    int package_complete;
    int magicgate_required;
} FmcbInstallPlan;

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

int FmcbPackageEntryCount(void);
const FmcbPackageEntry *FmcbPackageEntryAt(int index);
void FmcbBuildInstallPlan(int target_port, char region_letter,
                          FmcbInstallPlan *plan);

int FmcbInitMassBackend(FmcbMassBackendStatus *status);
void FmcbShutdownMassBackend(FmcbMassBackendStatus *status);
void FmcbResetPackageReport(FmcbPackageReport *report, int target_port);
int FmcbProbeMassPackage(int target_port,
                         const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report);
const char *FmcbPackageStatusText(FmcbPackageStatus status);

#endif /* MCI_FMCB_INSTALL_H */
