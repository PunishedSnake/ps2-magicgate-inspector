/*
 * Briscoe dev12/dev13 exact GET_KBIT diagnostic and SECR port correction.
 *
 * dev12 proved that both tested Sony cards reach the real first card_encrypt()
 * with both Mechacon-prepared Kbit halves present, then fail F2/50 with
 * stat6c=0001D100 (RX error + missing ACK). That exposed an EE-side caller bug:
 * the Inspector passed logical memory-card ports 0/1 into SECRSIF, while the
 * original FreeMcBoot Installer calls SecrDownloadFile(2 + port, slot, ...).
 * CardAuth uses the supplied value directly as the SIO2 channel, therefore
 * 0/1 addresses controller channels and 2/3 addresses the memory-card channels.
 *
 * dev13 corrects the port at the SECRSIF RPC boundary for HEADER, GET_KBIT and
 * GET_KC while retaining dev12's source-level failure record. This keeps the
 * ordinary libmc side on logical ports 0/1 and makes only SECRMAN see the same
 * physical SIO2 port numbering used by the reference FMCB installer.
 */

#include <tamtypes.h>
#include <sifrpc.h>
#include <libsecr-common.h>
#include <secrsif.h>
#include <stdio.h>
#include <string.h>

#include "magicgate.h"

#define DEV12_MAGIC0 0xD2
#define DEV12_MAGIC1 0x12

enum Dev12Stage {
    DEV12_STAGE_NONE = 0,
    DEV12_STAGE_MECHA0 = 1,
    DEV12_STAGE_MECHA1 = 2,
    DEV12_STAGE_CARD0 = 3,
    DEV12_STAGE_CARD1 = 4,
    DEV12_STAGE_NO_RECORD = 0x7f
};

enum Dev12CardReason {
    DEV12_REASON_NONE = 0,
    DEV12_REASON_CALLBACK = 1,
    DEV12_REASON_SIO2 = 2,
    DEV12_REASON_ID = 3,
    DEV12_REASON_STATUS = 4,
    DEV12_REASON_CHECKSUM = 5,
    DEV12_REASON_NO_HANDLER = 6,
    DEV12_REASON_UNKNOWN = 7
};

typedef struct Dev12Record {
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
} Dev12Record;

static SifRpcClientData_t *HeaderClient;
static SifRpcClientData_t *KbitClient;
static SifRpcClientData_t *KcClient;
static Dev12Record Record;
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

static int PhysicalSecrPort(int port)
{
    if (port >= 0 && port <= 1)
        return port + 2;
    return port;
}

static void DecodeRecord(const unsigned char data[16])
{
    ClearRecord();
    if (data[0] != DEV12_MAGIC0 || data[1] != DEV12_MAGIC1) {
        Record.stage = DEV12_STAGE_NO_RECORD;
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
        case DEV12_REASON_CALLBACK: return "MCMAN CALLBACK";
        case DEV12_REASON_SIO2:
            if ((Record.stat6c & 0x0000C000) == 0x0000C000)
                return "SIO2 RX+NOACK";
            if (Record.stat6c & 0x00008000)
                return "SIO2 NOACK";
            if (Record.stat6c & 0x00004000)
                return "SIO2 RX";
            if (Record.stat6c & 0x00002000)
                return "SIO2 TX";
            return "SIO2";
        case DEV12_REASON_ID: return "CARD ID";
        case DEV12_REASON_STATUS: return "CARD STATUS";
        case DEV12_REASON_CHECKSUM: return "CARD CHECKSUM";
        case DEV12_REASON_NO_HANDLER: return "NO MC HANDLER";
        case DEV12_REASON_UNKNOWN: return "UNKNOWN CARD PATH";
        default: return "UNKNOWN";
    }
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

    /* The reference FMCB installer calls SecrDownloadFile(2 + port, ...).
     * Mirror that at the RPC boundary while the rest of Inspector continues to
     * use libmc's logical 0/1 numbering. Only functions carrying a card port
     * are adjusted; DOWNLOAD_BLOCK and GET_ICVPS2 have no port field. */
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
    if (result != MG_RESULT_KBIT_FAILED)
        return __real_MagicGateResultText(result);

    if (!Record.valid) {
        if (Record.stage == DEV12_STAGE_NO_RECORD)
            return "KBIT FAIL / DEV12 RECORD MISSING";
        return __real_MagicGateResultText(result);
    }

    switch (Record.stage) {
        case DEV12_STAGE_MECHA0:
            return "KBIT FAIL / MECHACON KBIT HALF 0";
        case DEV12_STAGE_MECHA1:
            return "KBIT FAIL / MECHACON KBIT HALF 1";
        case DEV12_STAGE_CARD0:
            return "KBIT FAIL / CARD ENCRYPT HALF 0";
        case DEV12_STAGE_CARD1:
            return "KBIT FAIL / CARD ENCRYPT HALF 1";
        default:
            return "KBIT FAIL / DEV12 BAD STAGE";
    }
}

const char *__wrap_MagicGateStageText(MagicGateStage stage)
{
    if (stage != MG_STAGE_GET_KBIT || !Record.valid)
        return __real_MagicGateStageText(stage);

    if (Record.stage == DEV12_STAGE_MECHA0 || Record.stage == DEV12_STAGE_MECHA1) {
        snprintf(StageText, sizeof(StageText),
                 "GET KBIT / MECHA h%d rc=%d pre=%u/%u",
                 Record.stage == DEV12_STAGE_MECHA0 ? 0 : 1,
                 Record.stage == DEV12_STAGE_MECHA0 ? Record.mecha0 : Record.mecha1,
                 Record.pre0, Record.pre1);
        return StageText;
    }

    snprintf(StageText, sizeof(StageText),
             "GET KBIT / h%d %02X %s tr=%d stat=%08X id=%02X st=%02X pre=%u/%u",
             Record.stage == DEV12_STAGE_CARD0 ? 0 : 1,
             Record.command, ReasonText(), (int)Record.transfer_rc,
             Record.stat6c, Record.id, Record.status,
             Record.pre0, Record.pre1);
    return StageText;
}
