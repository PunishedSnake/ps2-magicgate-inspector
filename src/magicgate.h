#ifndef MCI_MAGICGATE_H
#define MCI_MAGICGATE_H

#include <errno.h>
#include <delaythread.h>

/*
 * MagicGate / KELF capability probe for PS2 Memory Card Inspector.
 *
 * KELF acquisition happens under the hardware-validated normal ROM X stack.
 * The raw user-supplied FMCB.XLF is kept in EE RAM across an isolated SECR
 * session and discarded afterwards. The probe never writes a bound KELF to a
 * memory card.
 */

typedef enum MagicGateStage {
    MG_STAGE_NOT_RUN = 0,
    MG_STAGE_FIND_KELF,
    MG_STAGE_READ_KELF,
    MG_STAGE_VALIDATE_KELF,
    MG_STAGE_SESSION_SETUP,
    MG_STAGE_SESSION_CARD_CHECK,
    MG_STAGE_BIND_RPC,
    MG_STAGE_DOWNLOAD_HEADER,
    MG_STAGE_DOWNLOAD_BLOCKS,
    MG_STAGE_GET_KBIT,
    MG_STAGE_GET_KC,
    MG_STAGE_GET_ICVPS2,
    MG_STAGE_DONE
} MagicGateStage;

typedef enum MagicGateResult {
    MG_RESULT_NOT_RUN = 0,
    MG_RESULT_PASS,
    MG_RESULT_NO_TEST_KELF,
    MG_RESULT_IO_ERROR,
    MG_RESULT_INVALID_KELF,
    MG_RESULT_SESSION_SETUP_FAILED,
    MG_RESULT_SESSION_CARD_ERROR,
    MG_RESULT_RPC_UNAVAILABLE,
    MG_RESULT_HEADER_FAILED,
    MG_RESULT_BLOCK_FAILED,
    MG_RESULT_KBIT_FAILED,
    MG_RESULT_KC_FAILED,
    MG_RESULT_ICVPS2_FAILED,
    MG_RESULT_SECR_UNAVAILABLE,
    MG_RESULT_TARGET_NOT_PS2
} MagicGateResult;

typedef struct MagicGateIopStatus {
    int secrman_load_rc;
    int secrman_start_rc;
    int secrsif_load_rc;
    int secrsif_start_rc;
    int available;
} MagicGateIopStatus;

typedef struct MagicGateKelfBuffer {
    unsigned char *data;
    int size;
    int source_port;
    char source_path[64];
} MagicGateKelfBuffer;

typedef struct MagicGateReport {
    int target_port;
    int source_port;
    char source_path[64];
    int source_size;
    int source_io_rc;

    MagicGateStage stage;
    MagicGateResult result;

    int session_setup_rc;
    int session_mcinit_rc;
    int session_mcinfo_rc;
    int session_type;
    int session_free_clusters;
    int session_formatted;
    int restore_rc;

    int rpc_rc;
    int header_rc;
    int header_reply_size;
    int block_count;
    int encrypted_blocks;
    int blocks_completed;
    int failed_block;
    int kbit_rc;
    int kc_rc;
    int icvps2_required;
    int icvps2_rc;
} MagicGateReport;

/* Called inside the isolated SECR session while loadfile RPC is active. */
int MagicGateLoadIopModules(MagicGateIopStatus *status);

void MagicGateResetReport(MagicGateReport *report, int target_port);
void MagicGateResetKelfBuffer(MagicGateKelfBuffer *buffer);

/* Run under the normal, known-good memory-card stack before any IOP reboot. */
int MagicGatePrepareKelf(int target_port, MagicGateKelfBuffer *buffer,
                         MagicGateReport *report);
void MagicGateReleaseKelf(MagicGateKelfBuffer *buffer);

/* Run only after the isolated SECR personality and MCMAN are ready. */
int MagicGateProbePrepared(int target_port, const MagicGateKelfBuffer *buffer,
                           MagicGateReport *report);

const char *MagicGateStageText(MagicGateStage stage);
const char *MagicGateResultText(MagicGateResult result);

#endif /* MCI_MAGICGATE_H */
