#ifndef MCI_FMCB_TRANSACTION_H
#define MCI_FMCB_TRANSACTION_H

#include "fmcb_install.h"
#include "fmcb_recovery.h"

#define FMCB_TX_MAX_FILES FMCB_MAX_PACKAGE_ENTRIES
#define FMCB_MC_CLUSTER_BYTES 1024u
#define FMCB_TX_SAFETY_RESERVE_CLUSTERS 16u

typedef enum FmcbInstallStage {
    FMCB_INSTALL_NOT_RUN = 0,
    FMCB_INSTALL_PRECONDITIONS,
    FMCB_INSTALL_SPACE_CHECK,
    FMCB_INSTALL_RECOVERY_PREPARE,
    FMCB_INSTALL_CREATE_DIRS,
    FMCB_INSTALL_BACKUP_TARGET,
    FMCB_INSTALL_READ_SOURCE,
    FMCB_INSTALL_BIND_KELF,
    FMCB_INSTALL_WRITE_TARGET,
    FMCB_INSTALL_VERIFY_TARGET,
    FMCB_INSTALL_ROLLBACK,
    FMCB_INSTALL_RECOVERY_FINISH,
    FMCB_INSTALL_DONE
} FmcbInstallStage;

typedef enum FmcbInstallResult {
    FMCB_INSTALL_RESULT_NOT_RUN = 0,
    FMCB_INSTALL_RESULT_PASS,
    FMCB_INSTALL_RESULT_REJECTED,
    FMCB_INSTALL_RESULT_NO_SPACE,
    FMCB_INSTALL_RESULT_RECOVERY_REQUIRED,
    FMCB_INSTALL_RESULT_RECOVERY_IO,
    FMCB_INSTALL_RESULT_SOURCE_IO,
    FMCB_INSTALL_RESULT_BIND_FAILED,
    FMCB_INSTALL_RESULT_TARGET_IO,
    FMCB_INSTALL_RESULT_VERIFY_FAILED,
    FMCB_INSTALL_RESULT_ROLLBACK_FAILED
} FmcbInstallResult;

typedef struct FmcbInstallOptions {
    int preserve_existing_cnfs;
    int verify_every_file;
} FmcbInstallOptions;

typedef struct FmcbInstallFileReport {
    char source[FMCB_PATH_MAX];
    char destination[FMCB_PATH_MAX];
    unsigned int flags;
    unsigned int size;
    unsigned int previous_size;
    unsigned int required_clusters;
    unsigned int reclaimable_clusters;
    int selected;
    int skipped;
    int existed;
    int backup_rc;
    int bind_rc;
    int write_rc;
    int verify_rc;
} FmcbInstallFileReport;

typedef struct FmcbInstallReport {
    int target_port;
    FmcbInstallStage stage;
    FmcbInstallResult result;
    int current_file;
    int files_total;
    int files_committed;
    int rollback_rc;
    int recovery_rc;
    int space_rc;
    int free_clusters;
    unsigned int payload_clusters;
    unsigned int reclaimable_clusters;
    unsigned int reserve_clusters;
    int minimum_remaining_clusters;
    int created_system_dir;
    int created_sysconf_dir;
    FmcbInstallFileReport files[FMCB_TX_MAX_FILES];
} FmcbInstallReport;

/* The transaction engine owns source/persistent-backup/write/verify/rollback.
 * The caller supplies KELF binding because entering/leaving the isolated
 * SECRMAN IOP personality is application-level lifecycle state. The callback
 * must return with the normal Sony ROM X filesystem environment restored. */
typedef int (*FmcbBindKelfCallback)(int target_port,
                                    unsigned char *buffer,
                                    unsigned int size,
                                    void *userdata);

void FmcbInstallResetReport(FmcbInstallReport *report, int target_port);
int FmcbInstallNormalTransactional(int target_port,
                                   const FmcbPackageReport *package,
                                   const FmcbInstallOptions *options,
                                   FmcbBindKelfCallback bind_kelf,
                                   void *bind_userdata,
                                   FmcbRecoveryStatus *recovery,
                                   FmcbInstallReport *report);
const char *FmcbInstallStageText(FmcbInstallStage stage);
const char *FmcbInstallResultText(FmcbInstallResult result);

#endif /* MCI_FMCB_TRANSACTION_H */
