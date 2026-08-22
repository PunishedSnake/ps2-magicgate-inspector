#ifndef MCI_FMCB_RECOVERY_H
#define MCI_FMCB_RECOVERY_H

#include "fmcb_install.h"

#define FMCB_RECOVERY_PATH_MAX 128
#define FMCB_RECOVERY_CARD_MARKER "/__MCI04.TXN"

typedef enum FmcbRecoveryState {
    FMCB_RECOVERY_NONE = 0,
    FMCB_RECOVERY_ACTIVE,
    FMCB_RECOVERY_ROLLING_BACK,
    FMCB_RECOVERY_CORRUPT
} FmcbRecoveryState;

typedef struct FmcbRecoveryStatus {
    FmcbRecoveryState state;
    int present;
    int valid;
    int target_port;
    int prepared_files;
    unsigned int sequence;
    unsigned int marker_token;
    int probe_rc;
    char source_root[FMCB_SOURCE_ROOT_MAX];
    char recovery_root[FMCB_RECOVERY_PATH_MAX];
} FmcbRecoveryStatus;

/* Scan known FMCB package roots for an unfinished journal. An invalid journal
 * is reported as present/corrupt so a new install cannot overwrite evidence. */
int FmcbRecoveryProbe(const FmcbMassBackendStatus *backend,
                      FmcbRecoveryStatus *status);

/* Create a dual-slot, checksummed journal before the first memory-card write. */
int FmcbRecoveryBegin(const FmcbPackageReport *package,
                      FmcbRecoveryStatus *status);

/* After the USB journal exists, write and read-back a tiny transaction token to
 * the target card. Recovery refuses to modify a card without the matching
 * token, preventing a stale journal from being applied to a different card. */
int FmcbRecoveryArmCard(FmcbRecoveryStatus *status, int target_port);

/* Persist the original destination (or its absence) to USB, verify the backup,
 * then atomically advance the journal. The target itself is never modified. */
int FmcbRecoveryCaptureTarget(FmcbRecoveryStatus *status,
                              int target_port,
                              int manifest_index,
                              const char *destination,
                              int *existed,
                              unsigned int *size);

/* Record card directories created by this transaction so recovery can remove
 * them if they are still empty after all original files have been restored. */
int FmcbRecoveryRecordDirectories(FmcbRecoveryStatus *status,
                                  const char *system_dir,
                                  int created_system_dir,
                                  int created_sysconf_dir);

/* Restore every prepared destination in reverse order from persistent USB
 * backups. Safe to run again after another interruption, but only on the card
 * carrying the matching transaction marker. */
int FmcbRecoveryRun(FmcbRecoveryStatus *status, int *rollback_rc);

/* Mark a successful transaction committed, then remove the card marker,
 * backups and both journal slots. */
int FmcbRecoveryFinish(FmcbRecoveryStatus *status);

const char *FmcbRecoveryStateText(FmcbRecoveryState state);

#endif /* MCI_FMCB_RECOVERY_H */
