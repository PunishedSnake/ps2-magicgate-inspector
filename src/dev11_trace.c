/*
 * Briscoe dev11 Kbit diagnostic overlay.
 * CI/hardware candidate 2.
 *
 * This deliberately avoids changing the normal Inspector or core KELF probe.
 * Linker wrapping remembers which SIF client is bound to GET_KBIT, inspects the
 * returned key buffer when that RPC reports failure, then asks the project-owned
 * mgtrace IOP module to independently exercise F2/50..53 against the card.
 * Existing UI text functions are wrapped so the result appears on the normal
 * MagicGate page without adding another diagnostic screen.
 */

#include <tamtypes.h>
#include <sifrpc.h>
#include <secrsif.h>
#include <delaythread.h>
#include <stdio.h>
#include <string.h>

#include "magicgate.h"
#include "../mgtrace_protocol.h"

#define DEV11_BIND_RETRIES 100
#define DEV11_BIND_DELAY_USEC 1000

enum Dev11TraceCode {
    DEV11_TRACE_NONE = 0,
    DEV11_TRACE_RPC_UNAVAILABLE,
    DEV11_TRACE_RESET_FAILED,
    DEV11_TRACE_50_FAILED,
    DEV11_TRACE_51_FAILED,
    DEV11_TRACE_52_FAILED,
    DEV11_TRACE_53_FAILED,
    DEV11_TRACE_PASS_MECHA_EMPTY,
    DEV11_TRACE_PASS_MECHA_PARTIAL,
    DEV11_TRACE_PASS_SECR_PATH
};

static SifRpcClientData_t *KbitClient;
static SifRpcClientData_t TraceClient;
static MgTracePacket TracePacket __attribute__((aligned(64)));
static int TraceBound;
static int TraceCode;
static int TraceCommand;
static int TraceTransferRc;
static unsigned int TraceStat6c;
static unsigned int TraceId;
static unsigned int TraceStatus;
static int KbitFirstNonzero;
static int KbitSecondNonzero;
static char TraceStageText[96];

int __real_sceSifBindRpc(SifRpcClientData_t *cd, int sid, int mode);
int __real_sceSifCallRpc(SifRpcClientData_t *cd, int fno, int mode,
                         void *send, int ssize, void *receive, int rsize,
                         SifRpcEndFunc_t endfunc, void *efarg);
const char *__real_MagicGateResultText(MagicGateResult result);
const char *__real_MagicGateStageText(MagicGateStage stage);

static int AnyNonzero(const unsigned char *data, int size)
{
    int i;
    for (i = 0; i < size; i++) {
        if (data[i] != 0)
            return 1;
    }
    return 0;
}

static void ResetTraceState(void)
{
    memset(&TraceClient, 0, sizeof(TraceClient));
    memset(&TracePacket, 0, sizeof(TracePacket));
    TraceBound = 0;
    TraceCode = DEV11_TRACE_NONE;
    TraceCommand = -1;
    TraceTransferRc = -1;
    TraceStat6c = 0;
    TraceId = 0;
    TraceStatus = 0;
    KbitFirstNonzero = 0;
    KbitSecondNonzero = 0;
}

static int BindTraceRpc(void)
{
    int i;
    int rc = 0;

    if (TraceBound && TraceClient.server != NULL)
        return 0;

    memset(&TraceClient, 0, sizeof(TraceClient));
    for (i = 0; i < DEV11_BIND_RETRIES; i++) {
        rc = __real_sceSifBindRpc(&TraceClient, MGTRACE_RPC_ID, 0);
        if (rc >= 0 && TraceClient.server != NULL) {
            TraceBound = 1;
            return 0;
        }
        DelayThread(DEV11_BIND_DELAY_USEC);
    }

    return rc < 0 ? rc : -1;
}

static void CaptureStageFailure(int index, int command)
{
    TraceCommand = command;
    TraceTransferRc = TracePacket.transfer_rc[index];
    TraceStat6c = TracePacket.stat6c[index];
    TraceId = TracePacket.response_id[index];
    TraceStatus = TracePacket.response_status[index];
}

static void RunDirectCardTrace(int port, const unsigned char kbit[16])
{
    int rc;

    KbitFirstNonzero = AnyNonzero(kbit, 8);
    KbitSecondNonzero = AnyNonzero(kbit + 8, 8);

    rc = BindTraceRpc();
    if (rc < 0) {
        TraceCode = DEV11_TRACE_RPC_UNAVAILABLE;
        TraceTransferRc = rc;
        return;
    }

    memset(&TracePacket, 0, sizeof(TracePacket));
    TracePacket.port = port;
    TracePacket.slot = 0;
    memcpy(TracePacket.seed, kbit, 8);

    rc = __real_sceSifCallRpc(&TraceClient, MGTRACE_RPC_FN_RUN, 0,
                              &TracePacket, sizeof(TracePacket),
                              &TracePacket, sizeof(TracePacket), NULL, NULL);
    if (rc < 0) {
        TraceCode = DEV11_TRACE_RPC_UNAVAILABLE;
        TraceTransferRc = rc;
        return;
    }

    if (TracePacket.reset_ok != 1) {
        TraceCode = DEV11_TRACE_RESET_FAILED;
        TraceCommand = 0xF3;
        TraceTransferRc = TracePacket.reset_transfer_rc;
        TraceStat6c = TracePacket.reset_stat6c;
        TraceId = TracePacket.reset_id;
        TraceStatus = TracePacket.reset_status;
        return;
    }
    if (TracePacket.stage_ok[0] != 1) {
        TraceCode = DEV11_TRACE_50_FAILED;
        CaptureStageFailure(0, 0x50);
        return;
    }
    if (TracePacket.stage_ok[1] != 1) {
        TraceCode = DEV11_TRACE_51_FAILED;
        CaptureStageFailure(1, 0x51);
        return;
    }
    if (TracePacket.stage_ok[2] != 1) {
        TraceCode = DEV11_TRACE_52_FAILED;
        CaptureStageFailure(2, 0x52);
        return;
    }
    if (TracePacket.stage_ok[3] != 1) {
        TraceCode = DEV11_TRACE_53_FAILED;
        CaptureStageFailure(3, 0x53);
        return;
    }

    if (!KbitFirstNonzero && !KbitSecondNonzero)
        TraceCode = DEV11_TRACE_PASS_MECHA_EMPTY;
    else if (!KbitFirstNonzero || !KbitSecondNonzero)
        TraceCode = DEV11_TRACE_PASS_MECHA_PARTIAL;
    else
        TraceCode = DEV11_TRACE_PASS_SECR_PATH;
}

int __wrap_sceSifBindRpc(SifRpcClientData_t *cd, int sid, int mode)
{
    int rc = __real_sceSifBindRpc(cd, sid, mode);

    if (sid == SECRSIF_DOWNLOAD_GET_KBIT && rc >= 0 && cd->server != NULL) {
        KbitClient = cd;
        ResetTraceState();
    }

    return rc;
}

int __wrap_sceSifCallRpc(SifRpcClientData_t *cd, int fno, int mode,
                         void *send, int ssize, void *receive, int rsize,
                         SifRpcEndFunc_t endfunc, void *efarg)
{
    int rc;

    rc = __real_sceSifCallRpc(cd, fno, mode, send, ssize,
                              receive, rsize, endfunc, efarg);

    if (rc >= 0 && cd == KbitClient && receive != NULL) {
        struct SecrSifDownloadGetKbitParams *param;
        param = (struct SecrSifDownloadGetKbitParams *)receive;
        if (param->result == 0)
            RunDirectCardTrace(param->port, param->kbit);
    }

    return rc;
}

const char *__wrap_MagicGateResultText(MagicGateResult result)
{
    if (result != MG_RESULT_KBIT_FAILED || TraceCode == DEV11_TRACE_NONE)
        return __real_MagicGateResultText(result);

    switch (TraceCode) {
        case DEV11_TRACE_RPC_UNAVAILABLE:
            return "KBIT FAIL / TRACE RPC UNAVAILABLE";
        case DEV11_TRACE_RESET_FAILED:
            return "KBIT FAIL / CARD AUTH F3 FAILED";
        case DEV11_TRACE_50_FAILED:
            return "KBIT FAIL / CARD AUTH 0x50 FAILED";
        case DEV11_TRACE_51_FAILED:
            return "KBIT FAIL / CARD AUTH 0x51 FAILED";
        case DEV11_TRACE_52_FAILED:
            return "KBIT FAIL / CARD AUTH 0x52 FAILED";
        case DEV11_TRACE_53_FAILED:
            return "KBIT FAIL / CARD AUTH 0x53 FAILED";
        case DEV11_TRACE_PASS_MECHA_EMPTY:
            return "KBIT FAIL / MECHACON KEY FETCH SUSPECT";
        case DEV11_TRACE_PASS_MECHA_PARTIAL:
            return "KBIT FAIL / MECHACON KEY FETCH PARTIAL";
        case DEV11_TRACE_PASS_SECR_PATH:
            return "KBIT FAIL / SECR MCMAN PATH SUSPECT";
        default:
            return __real_MagicGateResultText(result);
    }
}

const char *__wrap_MagicGateStageText(MagicGateStage stage)
{
    if (stage != MG_STAGE_GET_KBIT || TraceCode == DEV11_TRACE_NONE)
        return __real_MagicGateStageText(stage);

    if (TraceCode >= DEV11_TRACE_RESET_FAILED && TraceCode <= DEV11_TRACE_53_FAILED) {
        snprintf(TraceStageText, sizeof(TraceStageText),
                 "GET KBIT / %02X tr=%d stat=%08X id=%02X st=%02X",
                 TraceCommand & 0xFF, TraceTransferRc, TraceStat6c,
                 TraceId & 0xFF, TraceStatus & 0xFF);
        return TraceStageText;
    }

    if (TraceCode == DEV11_TRACE_RPC_UNAVAILABLE) {
        snprintf(TraceStageText, sizeof(TraceStageText),
                 "GET KBIT / trace RPC rc=%d", TraceTransferRc);
        return TraceStageText;
    }

    snprintf(TraceStageText, sizeof(TraceStageText),
             "GET KBIT / direct 50-53 PASS / key halves %d/%d",
             KbitFirstNonzero, KbitSecondNonzero);
    return TraceStageText;
}
