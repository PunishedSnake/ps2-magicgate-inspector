#ifndef MCI_FMCB_INSTALL_H
#define MCI_FMCB_INSTALL_H

/*
 * FreeMcBoot package preflight only.
 *
 * Briscoe currently discovers and validates a user-supplied package on USB and
 * derives the expected target layout. No function declared here commits FMCB
 * payloads to a memory card. The first future write-capable milestone must be
 * a separately reviewed bind/write/read-back/rollback transaction.
 */

#define FMCB_MAX_PACKAGE_FILES 16
#define FMCB_PATH_SIZE 96

#define FMCB_FILE_REQUIRED 0x01
#define FMCB_FILE_OPTIONAL 0x02
#define FMCB_FILE_KELF 0x04

typedef enum FmcbPackageStatus {
    FMCB_PACKAGE_NOT_SCANNED = 0,
    FMCB_PACKAGE_SOURCE_UNAVAILABLE,
    FMCB_PACKAGE_NOT_FOUND,
    FMCB_PACKAGE_INCOMPLETE,
    FMCB_PACKAGE_READY
} FmcbPackageStatus;

typedef struct FmcbMassBackendStatus {
    int usbd_load_rc;
    int usbd_start_rc;
    int usbhdfsd_load_rc;
    int usbhdfsd_start_rc;
    int filexio_init_rc;
    int available;
} FmcbMassBackendStatus;

typedef struct FmcbPackageFileStatus {
    char relative_path[FMCB_PATH_SIZE];
    unsigned int flags;
    int found;
    int stat_rc;
    unsigned int size;
} FmcbPackageFileStatus;

typedef struct FmcbInstallPlan {
    char romver_region;
    char region_letter;
    char destination_system[32];
    int required_files;
    int optional_files;
    int kelf_files;
} FmcbInstallPlan;

typedef struct FmcbPackageReport {
    int target_port;
    FmcbPackageStatus status;
    FmcbInstallPlan plan;
    char source_root[32];
    int source_probe_rc;
    int entry_count;
    int found_required;
    int missing_required;
    int found_optional;
    unsigned int total_found_bytes;
    FmcbPackageFileStatus files[FMCB_MAX_PACKAGE_FILES];
} FmcbPackageReport;

int FmcbInitMassBackend(FmcbMassBackendStatus *status);
void FmcbShutdownMassBackend(FmcbMassBackendStatus *status);
void FmcbResetPackageReport(FmcbPackageReport *report, int target_port);
int FmcbProbeMassPackage(int target_port,
                         const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report);
const char *FmcbPackageStatusText(FmcbPackageStatus status);

#endif /* MCI_FMCB_INSTALL_H */
