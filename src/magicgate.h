#ifndef MCI_MAGICGATE_H
#define MCI_MAGICGATE_H

#include <errno.h>

/*
 * MagicGate / KELF diagnostics for PS2 Memory Card Inspector.
 *
 * The probe is intentionally non-destructive: it reads an existing KELF into
 * EE RAM, exercises the SECR download/binding protocol against the selected
 * card and discards the mutated working copy.  It never writes the bound KELF
 * back to either memory card.
 */

typedef enum MagicGateStage {
    MG_STAGE_NOT_RUN = 0,
    MG_STAGE_FIND_KELF,
    MG_STAGE_READ_KELF,
    MG_STAGE_VALIDATE_KELF,
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
    MG_RESULT_RPC_UNAVAILABLE,
    MG_RESULT_HEADER_FAILED,
    MG_RESULT_BLOCK_FAILED,
    MG_RESULT_KBIT_FAILED,
    MG_RESULT_KC_FAILED,
    MG_RESULT_ICVPS2_FAILED,
    MG_RESULT_SECR_UNAVAILABLE
} MagicGateResult;

typedef struct MagicGateIopStatus {
    int secrman_load_rc;
    int secrman_start_rc;
    int secrsif_load_rc;
    int secrsif_start_rc;
    int available;
} MagicGateIopStatus;

typedef struct MagicGateReport {
    int target_port;
    int source_port;
    char source_path[64];
    int source_size;
    int source_io_rc;

    MagicGateStage stage;
    MagicGateResult result;

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

/* Called while loadfile RPC is active, after XSIO2MAN has been loaded. */
int MagicGateLoadIopModules(MagicGateIopStatus *status);

void MagicGateResetReport(MagicGateReport *report, int target_port);
int MagicGateProbeCard(int target_port, MagicGateReport *report);

const char *MagicGateStageText(MagicGateStage stage);
const char *MagicGateResultText(MagicGateResult result);

#endif /* MCI_MAGICGATE_H */
