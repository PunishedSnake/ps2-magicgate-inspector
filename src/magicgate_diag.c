/* SPDX-License-Identifier: MIT */
/*
 * MagicGate SECR port bridge and failed-GET_KBIT diagnostics.
 *
 * Hardware validation established two requirements that belong at the SECRSIF
 * boundary rather than in the ordinary filesystem code:
 *
 *  - libmc exposes logical memory-card ports 0/1, while SECRMAN CardAuth uses
 *    physical SIO2 memory-card channels 2/3 directly;
 *  - a failed GET_KBIT must distinguish Mechacon preparation from the real
 *    card-side F2/50..53 transform without replaying additional commands.
 *
 * The PS2SDK 2.0 SECRMAN 1.4 build emits a compact 16-byte diagnostic record
 * only when GET_KBIT fails. Successful SECRMAN behavior is unchanged. This EE
 * shim decodes that record and classifies the hardware-validated first-command
 * RX/no-ACK signature as unsupported CardAuth; other failures remain protocol
 * errors or indeterminate infrastructure/Mechacon failures.
 */

#include <tamtypes.h>
#include <sifrpc.h>
#include <libsecr-common.h>
#include <secrsif.h>
#include <stdio.h>
#include <string.h>

#include "magicgate.h"

#define MGDIAG_MAGIC0 0xD2
#define MGDIAG_MAGIC1 0x12

enum MgDiagStage {
    MGDIAG_STAGE_NONE = 0,
    MGDIAG_STAGE_MECHA0 = 1,
    MGDIAG_STAGE_MECHA1 = 2,
    MGDIAG_STAGE_CARD0 = 3,
    MGDIAG_STAGE_CARD1 = 4,
    MGDIAG_STAGE_NO_RECORD = 0x7f
};

enum MgDiagCardReason {
    MGDIAG_REASON_NONE = 0,
    MGDIAG_REASON_CALLBACK = 1,
    MGDIAG_REASON_SIO2 = 2,
    MGDIAG_REASON_ID = 3,
    MGDIAG_REASON_STATUS = 4,
    MGDIAG_REASON_CHECKSUM = 5,
    MGDIAG_REASON_NO_HANDLER = 6,
    MGDIAG_REASON_UNKNOWN = 7
};

typedef struct MgDiagRecord {
    int valid;
    unsigned char stage;
    unsigned char command;
    unsigned char reason;
    signed char transfer_rc;
    unsigned char id;
    unsigned char status;
    unsigned int stat6c;
    unsigned char pre0;
    unsigned char pre1;
    unsigned char mecha0;
    unsigned char mecha1;
} MgDiagRecord;

static SifRpcClientData_t *HeaderClient;
static SifRpcClientData_t *KbitClient;
static SifRpcClientData_t *KcClient;
static MgDiagRecord Record;
static char StageText[128];

int __real_sceSifBindRpc(SifRpcClientData_t *cd, int sid, int mode);
int __real_sceSifCallRpc(SifRpcClientData_t *cd, int fno, int mode,
                         void *send, int ssize, void *receive, int rsize,
                         SifRpcEndFunc_t endfunc, void *efarg);
const char *__real_MagicGateResultText(MagicGateResult result);
const char *__real_MagicGateStageText(MagicGateStage stage);

static void ClearRecord(void)
{
    memset(&Record, 0, sizeof(Record));
}

/*
 * SECRMAN receives a raw SIO2 channel index, not a libmc logical port. Keep
 * 0/1 everywhere else and translate only the three SECR RPCs that carry a card
 * port. This mirrors the reference FreeMcBoot binding path (2 + logical port).
 */
static int PhysicalSecrPort(int port)
{
    if (port >= 0 && port <= 1)
        return port + 2;
    return port;
}

static void DecodeRecord(const unsigned char data[16])
{
    ClearRecord();
    if (data[0] != MGDIAG_MAGIC0 || data[1] != MGDIAG_MAGIC1) {
        Record.stage = MGDIAG_STAGE_NO_RECORD;
        return;
    }

    Record.valid = 1;
    Record.stage = data[2];
    Record.command = data[3];
    Record.reason = data[4];
    Record.transfer_rc = (signed char)data[5];
    Record.id = data[6];
    Record.status = data[7];
    Record.stat6c = (unsigned int)data[8] |
                    ((unsigned int)data[9] << 8) |
                    ((unsigned int)data[10] << 16) |
                    ((unsigned int)data[11] << 24);
    Record.pre0 = data[12];
    Record.pre1 = data[13];
    Record.mecha0 = data[14];
    Record.mecha1 = data[15];
}

static const char *ReasonText(void)
{
    switch (Record.reason) {
        case MGDIAG_REASON_CALLBACK: return "MCMAN CALLBACK";
        case MGDIAG_REASON_SIO2:
            if ((Record.stat6c & 0x0000C000) == 0x0000C000)
                return "SIO2 RX+NOACK";
            if (Record.stat6c & 0x00008000)
                return "SIO2 NOACK";
            if (Record.stat6c & 0x00004000)
                return "SIO2 RX";
            if (Record.stat6c & 0x00002000)
                return "SIO2 TX";
            return "SIO2";
        case MGDIAG_REASON_ID: return "CARD ID";
        case MGDIAG_REASON_STATUS: return "CARD STATUS";
        case MGDIAG_REASON_CHECKSUM: return "CARD CHECKSUM";
        case MGDIAG_REASON_NO_HANDLER: return "NO MC HANDLER";
        case MGDIAG_REASON_UNKNOWN: return "UNKNOWN CARD PATH";
        default: return "UNKNOWN";
    }
}

/*
 * This exact signature was reproduced on a third-party 64 MB card without
 * functional MagicGate while known-good cards completed the same backend with
 * FUNCTIONAL. It therefore means that the card did not ACK the first CardAuth
 * command, not that Inspector selected the wrong SIO2 channel.
 */
static int LooksLikeNoMagicGateAck(void)
{
    return Record.valid &&
           Record.stage == MGDIAG_STAGE_CARD0 &&
           Record.command == 0x50 &&
           Record.reason == MGDIAG_REASON_SIO2 &&
           Record.pre0 != 0 && Record.pre1 != 0 &&
           (Record.stat6c & 0x00008000) != 0 &&
           Record.id == 0xFF && Record.status == 0xFF;
}

int __wrap_sceSifBindRpc(SifRpcClientData_t *cd, int sid, int mode)
{
    int rc = __real_sceSifBindRpc(cd, sid, mode);

    if (rc >= 0 && cd->server != NULL) {
        if ((unsigned int)sid == SECRSIF_DOWNLOAD_HEADER)
            HeaderClient = cd;
        else if ((unsigned int)sid == SECRSIF_DOWNLOAD_GET_KBIT) {
            KbitClient = cd;
            ClearRecord();
        } else if ((unsigned int)sid == SECRSIF_DOWNLOAD_GET_KC)
            KcClient = cd;
    }

    return rc;
}

int __wrap_sceSifCallRpc(SifRpcClientData_t *cd, int fno, int mode,
                         void *send, int ssize, void *receive, int rsize,
                         SifRpcEndFunc_t endfunc, void *efarg)
{
    int rc;

    if (fno == 1 && send != NULL) {
        if (cd == HeaderClient) {
            struct SecrSifDownloadHeaderParams *param;
            param = (struct SecrSifDownloadHeaderParams *)send;
            param->port = PhysicalSecrPort(param->port);
        } else if (cd == KbitClient) {
            struct SecrSifDownloadGetKbitParams *param;
            param = (struct SecrSifDownloadGetKbitParams *)send;
            param->port = PhysicalSecrPort(param->port);
        } else if (cd == KcClient) {
            struct SecrSifDownloadGetKcParams *param;
            param = (struct SecrSifDownloadGetKcParams *)send;
            param->port = PhysicalSecrPort(param->port);
        }
    }

    rc = __real_sceSifCallRpc(cd, fno, mode, send, ssize,
                              receive, rsize, endfunc, efarg);

    if (rc >= 0 && cd == KbitClient && receive != NULL) {
        struct SecrSifDownloadGetKbitParams *param;
        param = (struct SecrSifDownloadGetKbitParams *)receive;
        if (param->result == 0)
            DecodeRecord(param->kbit);
        else
            ClearRecord();
    }

    return rc;
}

const char *__wrap_MagicGateResultText(MagicGateResult result)
{
    if (result == MG_RESULT_PASS)
        return "FUNCTIONAL";

    if (result != MG_RESULT_KBIT_FAILED)
        return __real_MagicGateResultText(result);

    if (!Record.valid) {
        if (Record.stage == MGDIAG_STAGE_NO_RECORD)
            return "TEST INDETERMINATE / KBIT FAILURE";
        return __real_MagicGateResultText(result);
    }

    if (LooksLikeNoMagicGateAck())
        return "NOT SUPPORTED / NO CARD AUTH ACK";

    if (Record.stage == MGDIAG_STAGE_MECHA0 ||
        Record.stage == MGDIAG_STAGE_MECHA1)
        return "TEST INDETERMINATE / MECHACON";

    if (Record.stage == MGDIAG_STAGE_CARD0 ||
        Record.stage == MGDIAG_STAGE_CARD1)
        return "PROTOCOL ERROR / CARD AUTH";

    return "TEST INDETERMINATE / BAD DIAG RECORD";
}

const char *__wrap_MagicGateStageText(MagicGateStage stage)
{
    if (stage != MG_STAGE_GET_KBIT || !Record.valid)
        return __real_MagicGateStageText(stage);

    if (Record.stage == MGDIAG_STAGE_MECHA0 ||
        Record.stage == MGDIAG_STAGE_MECHA1) {
        snprintf(StageText, sizeof(StageText),
                 "GET KBIT / MECHA h%d rc=%d pre=%u/%u",
                 Record.stage == MGDIAG_STAGE_MECHA0 ? 0 : 1,
                 Record.stage == MGDIAG_STAGE_MECHA0 ? Record.mecha0 : Record.mecha1,
                 Record.pre0, Record.pre1);
        return StageText;
    }

    snprintf(StageText, sizeof(StageText),
             "GET KBIT / h%d %02X %s tr=%d stat=%08X id=%02X st=%02X pre=%u/%u",
             Record.stage == MGDIAG_STAGE_CARD0 ? 0 : 1,
             Record.command, ReasonText(), (int)Record.transfer_rc,
             Record.stat6c, Record.id, Record.status,
             Record.pre0, Record.pre1);
    return StageText;
}
