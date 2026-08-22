#ifndef MCI_FMCB_INSTALL_H
#define MCI_FMCB_INSTALL_H

#include "console_profile.h"

/* FreeMcBoot package discovery plus the destination model shared by preflight
 * and the 0.4 verified normal-install transaction. */

#define FMCB_MAX_PACKAGE_ENTRIES 16
#define FMCB_PATH_MAX 96
#define FMCB_SOURCE_ROOT_MAX 32

#define FMCB_FILE_REQUIRED 0x01
#define FMCB_FILE_OPTIONAL 0x02
#define FMCB_FILE_KELF 0x04
#define FMCB_FILE_CONFIG 0x08
#define FMCB_FILE_RESOURCE 0x10
#define FMCB_FILE_CEX_ONLY 0x20

typedef enum FmcbPackageStatus {
    FMCB_PACKAGE_NOT_SCANNED = 0,
    FMCB_PACKAGE_SOURCE_UNAVAILABLE,
    FMCB_PACKAGE_NOT_FOUND,
    FMCB_PACKAGE_INCOMPLETE,
    FMCB_PACKAGE_READY,
    FMCB_PACKAGE_UNSUPPORTED_CONSOLE,
    FMCB_PACKAGE_REGION_AMBIGUOUS,
    FMCB_PACKAGE_CROSS_REGION_REQUIRED
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
    int selected;
    int found;
    int stat_rc;
    unsigned int size;
} FmcbPackageFileStatus;

typedef struct FmcbInstallPlan {
    int target_port;
    MciConsoleProfile console;
    char romver_region;
    unsigned int rom_version;
    char region_letter;
    char destination_system[32];
    char destination_osd[32];
    int required_files;
    int optional_files;
    int selected_files;
    int kelf_files;
    int config_files;
    int resource_files;
    int package_complete;
    int magicgate_required;
    int compact_unlock_candidate;
    int compact_unlock_active;
    unsigned int compact_possible_savings;
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
int FmcbPackageEntrySelected(const FmcbInstallPlan *plan, int index);
void FmcbBuildInstallPlan(int target_port, const MciConsoleProfile *console,
                          FmcbInstallPlan *plan);
int FmcbResolveDestination(const FmcbInstallPlan *plan, int entry_index,
                           char *path, unsigned int path_size);

int FmcbInitMassBackend(FmcbMassBackendStatus *status);
void FmcbShutdownMassBackend(FmcbMassBackendStatus *status);
void FmcbResetPackageReport(FmcbPackageReport *report, int target_port);
int FmcbProbeMassPackage(int target_port,
                         const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report);
const char *FmcbPackageStatusText(FmcbPackageStatus status);

#endif /* MCI_FMCB_INSTALL_H */
