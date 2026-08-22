/*
 * PS2 Memory Card Inspector - RAM-only MagicGate / KELF probe
 *
 * This file owns the high-level SECRSIF transaction and raw-KELF parsing. A
 * user-supplied FMCB.XLF is acquired under the known-good normal card stack,
 * kept in EE RAM across the isolated security-session reboot, and discarded
 * afterwards. Nothing in this probe writes the bound KELF back to a card.
 *
 * Only BIT entries marked for security download are constrained by SECRSIF's
 * 0x400-byte block RPC buffer; large plaintext payload entries merely advance
 * the KELF offset. Logical libmc -> physical SIO2 port translation and detailed
 * GET_KBIT failure classification are intentionally isolated in
 * magicgate_diag.c so this core transaction stays backend-neutral.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libmc.h>
#include <libsecr-common.h>
#include <secrsif.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "magicgate.h"

#define MG_RPC_RETRIES 300
#define MG_RPC_RETRY_USEC 1000
#define MG_CARD_RETRIES 4
#define MG_CARD_RETRY_USEC 20000
#define MG_READ_CHUNK 4096
#define MG_MAX_KELF_SIZE (4 * 1024 * 1024)
#define MG_RPC_TIMEOUT (-2100)
#define MG_SHORT_READ (-2101)
#define MG_INVALID_LAYOUT (-2102)
#define MG_RAW_SOURCE_PORT 9 /* display marker for USB source, not an mc port */

extern unsigned char secrsif_irx[];
extern unsigned int size_secrsif_irx;

static int SecrIopAvailable;
static int RpcBound;

static SifRpcClientData_t RpcDownloadHeader;
static SifRpcClientData_t RpcDownloadBlock;
static SifRpcClientData_t RpcGetKbit;
static SifRpcClientData_t RpcGetKc;
static SifRpcClientData_t RpcGetIcvps2;
static unsigned char RpcBuffer[0x1000] __attribute__((aligned(64)));

static const char *RawKelfCandidates[] = {
    "mass:/FMCB/SYSTEM/FMCB.XLF",
    "mass0:/FMCB/SYSTEM/FMCB.XLF",
    "mass1:/FMCB/SYSTEM/FMCB.XLF"
};

static int McSyncResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

void MagicGateResetReport(MagicGateReport *report, int target_port)
{
    memset(report, 0, sizeof(*report));
    report->target_port = target_port;
    report->source_port = -1;
    report->source_io_rc = -999;
    report->session_setup_rc = -999;
    report->session_mcinit_rc = -999;
    report->session_mcinfo_rc = -999;
    report->restore_rc = -999;
    report->rpc_rc = -999;
    report->header_rc = -999;
    report->header_reply_size = -1;
    report->failed_block = -1;
    report->kbit_rc = -999;
    report->kc_rc = -999;
    report->icvps2_rc = -999;
    report->stage = MG_STAGE_NOT_RUN;
    report->result = MG_RESULT_NOT_RUN;
}

void MagicGateResetKelfBuffer(MagicGateKelfBuffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->source_port = -1;
}

void MagicGateReleaseKelf(MagicGateKelfBuffer *buffer)
{
    if (buffer->data != NULL)
        free(buffer->data);
    MagicGateResetKelfBuffer(buffer);
}

/* SECRMAN is resident from the generated IOPRP; this starts only SECRSIF. */
int MagicGateLoadIopModules(MagicGateIopStatus *status)
{
    int rc;
    int start_rc = -999;

    memset(status, 0, sizeof(*status));
    status->secrman_load_rc = 0;
    status->secrman_start_rc = 0;
    status->secrsif_load_rc = -999;
    status->secrsif_start_rc = -999;

    SecrIopAvailable = 0;
    RpcBound = 0;
    memset(&RpcDownloadHeader, 0, sizeof(RpcDownloadHeader));
    memset(&RpcDownloadBlock, 0, sizeof(RpcDownloadBlock));
    memset(&RpcGetKbit, 0, sizeof(RpcGetKbit));
    memset(&RpcGetKc, 0, sizeof(RpcGetKc));
    memset(&RpcGetIcvps2, 0, sizeof(RpcGetIcvps2));

    rc = SifExecModuleBuffer(secrsif_irx, size_secrsif_irx, 0, NULL, &start_rc);
    status->secrsif_load_rc = rc;
    status->secrsif_start_rc = start_rc;
    if (rc < 0)
        return rc;

    status->available = 1;
    SecrIopAvailable = 1;
    return 0;
}

static int BindRpc(SifRpcClientData_t *client, int server_id)
{
    int i;
    int rc = 0;

    memset(client, 0, sizeof(*client));
    for (i = 0; i < MG_RPC_RETRIES; i++) {
        rc = sceSifBindRpc(client, server_id, 0);
        if (rc >= 0 && client->server != NULL)
            return 0;
        DelayThread(MG_RPC_RETRY_USEC);
    }

    return (rc < 0) ? rc : MG_RPC_TIMEOUT;
}

static int BindDownloadRpc(void)
{
    int rc;

    if (RpcBound)
        return 0;
    if (!SecrIopAvailable)
        return MG_RPC_TIMEOUT;

    sceSifInitRpc(0);

    rc = BindRpc(&RpcDownloadHeader, SECRSIF_DOWNLOAD_HEADER);
    if (rc < 0) return rc;
    rc = BindRpc(&RpcDownloadBlock, SECRSIF_DOWNLOAD_BLOCK);
    if (rc < 0) return rc;
    rc = BindRpc(&RpcGetKbit, SECRSIF_DOWNLOAD_GET_KBIT);
    if (rc < 0) return rc;
    rc = BindRpc(&RpcGetKc, SECRSIF_DOWNLOAD_GET_KC);
    if (rc < 0) return rc;
    rc = BindRpc(&RpcGetIcvps2, SECRSIF_DOWNLOAD_GET_ICVPS2);
    if (rc < 0) return rc;

    RpcBound = 1;
    return 0;
}

static int DownloadHeader(int port, int slot, const void *kelf,
                          SecrBitTable_t *bit_table, int *reply_size)
{
    struct SecrSifDownloadHeaderParams *param;
    int rc;

    memset(RpcBuffer, 0, sizeof(RpcBuffer));
    param = (struct SecrSifDownloadHeaderParams *)RpcBuffer;
    param->port = port;
    param->slot = slot;
    memcpy(param->buffer, kelf, sizeof(param->buffer));

    rc = sceSifCallRpc(&RpcDownloadHeader, 1, 0,
                       RpcBuffer, sizeof(RpcBuffer),
                       RpcBuffer, sizeof(RpcBuffer), NULL, NULL);
    if (rc < 0)
        return rc;

    memcpy(bit_table, &param->BitTable, sizeof(*bit_table));
    if (reply_size != NULL)
        *reply_size = param->size;
    return param->result;
}

static int DownloadBlock(const void *src, int size)
{
    struct SecrSifDownloadBlockParams *param;
    int rc;

    if (size < 0 || size > (int)sizeof(param->buffer))
        return MG_INVALID_LAYOUT;

    memset(RpcBuffer, 0, sizeof(RpcBuffer));
    param = (struct SecrSifDownloadBlockParams *)RpcBuffer;
    memcpy(param->buffer, src, size);
    param->size = size;

    rc = sceSifCallRpc(&RpcDownloadBlock, 1, 0,
                       RpcBuffer, sizeof(RpcBuffer),
                       RpcBuffer, sizeof(RpcBuffer), NULL, NULL);
    if (rc < 0)
        return rc;
    return param->result;
}

static int DownloadGetKbit(int port, int slot, unsigned char kbit[16])
{
    struct SecrSifDownloadGetKbitParams *param;
    int rc;

    memset(RpcBuffer, 0, sizeof(RpcBuffer));
    param = (struct SecrSifDownloadGetKbitParams *)RpcBuffer;
    param->port = port;
    param->slot = slot;

    rc = sceSifCallRpc(&RpcGetKbit, 1, 0,
                       RpcBuffer, sizeof(RpcBuffer),
                       RpcBuffer, sizeof(RpcBuffer), NULL, NULL);
    if (rc < 0)
        return rc;
    memcpy(kbit, param->kbit, 16);
    return param->result;
}

static int DownloadGetKc(int port, int slot, unsigned char kc[16])
{
    struct SecrSifDownloadGetKcParams *param;
    int rc;

    memset(RpcBuffer, 0, sizeof(RpcBuffer));
    param = (struct SecrSifDownloadGetKcParams *)RpcBuffer;
    param->port = port;
    param->slot = slot;

    rc = sceSifCallRpc(&RpcGetKc, 1, 0,
                       RpcBuffer, sizeof(RpcBuffer),
                       RpcBuffer, sizeof(RpcBuffer), NULL, NULL);
    if (rc < 0)
        return rc;
    memcpy(kc, param->kc, 16);
    return param->result;
}

static int DownloadGetIcvps2(unsigned char icvps2[8])
{
    struct SecrSifDownloadGetIcvps2Params *param;
    int rc;

    memset(RpcBuffer, 0, sizeof(RpcBuffer));
    param = (struct SecrSifDownloadGetIcvps2Params *)RpcBuffer;

    rc = sceSifCallRpc(&RpcGetIcvps2, 1, 0,
                       RpcBuffer, sizeof(RpcBuffer),
                       RpcBuffer, sizeof(RpcBuffer), NULL, NULL);
    if (rc < 0)
        return rc;
    memcpy(icvps2, param->icvps2, 8);
    return param->result;
}

static int FindRawKelfSource(MagicGateReport *report)
{
    iox_stat_t stat;
    unsigned int i;
    int rc = -1;

    for (i = 0; i < sizeof(RawKelfCandidates) / sizeof(RawKelfCandidates[0]); i++) {
        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(RawKelfCandidates[i], &stat);
        report->source_io_rc = rc;
        if (rc >= 0 && stat.size >= sizeof(SecrKELFHeader_t) &&
            stat.size <= MG_MAX_KELF_SIZE) {
            report->source_port = MG_RAW_SOURCE_PORT;
            report->source_size = (int)stat.size;
            snprintf(report->source_path, sizeof(report->source_path),
                     "RAW %s", RawKelfCandidates[i]);
            return 0;
        }
    }

    return rc < 0 ? rc : sceMcResNoEntry;
}

static const char *RawPathFromReport(const MagicGateReport *report)
{
    if (strncmp(report->source_path, "RAW ", 4) == 0)
        return report->source_path + 4;
    return report->source_path;
}

static int ReadRawKelfSource(const MagicGateReport *report,
                             unsigned char **out_buffer)
{
    const char *path = RawPathFromReport(report);
    unsigned char *buffer;
    int alloc_size;
    int total;
    int chunk;
    int fd;
    int rc;

    if (report->source_size <= 0 || report->source_size > MG_MAX_KELF_SIZE)
        return MG_INVALID_LAYOUT;

    alloc_size = report->source_size + 0x400;
    buffer = memalign(64, alloc_size);
    if (buffer == NULL)
        return -ENOMEM;
    memset(buffer, 0, alloc_size);

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0) {
        free(buffer);
        return fd;
    }

    total = 0;
    while (total < report->source_size) {
        chunk = report->source_size - total;
        if (chunk > MG_READ_CHUNK)
            chunk = MG_READ_CHUNK;

        rc = fileXioRead(fd, buffer + total, chunk);
        if (rc < 0) {
            fileXioClose(fd);
            free(buffer);
            return rc;
        }
        if (rc == 0 || rc > chunk) {
            fileXioClose(fd);
            free(buffer);
            return MG_SHORT_READ;
        }
        total += rc;
    }

    rc = fileXioClose(fd);
    if (rc < 0) {
        free(buffer);
        return rc;
    }

    *out_buffer = buffer;
    return 0;
}

static int ValidateKelf(const unsigned char *buffer, int size)
{
    const SecrKELFHeader_t *header;

    if (size < (int)sizeof(SecrKELFHeader_t))
        return MG_INVALID_LAYOUT;

    header = (const SecrKELFHeader_t *)buffer;
    if (header->KELF_header_size < sizeof(SecrKELFHeader_t) ||
        header->KELF_header_size > size)
        return MG_INVALID_LAYOUT;
    if (header->BIT_count > 63)
        return MG_INVALID_LAYOUT;

    return 0;
}

int MagicGatePrepareKelf(int target_port, MagicGateKelfBuffer *buffer,
                         MagicGateReport *report)
{
    const SecrKELFHeader_t *header;
    unsigned char *data = NULL;
    int rc;

    MagicGateResetReport(report, target_port);
    MagicGateResetKelfBuffer(buffer);

    report->stage = MG_STAGE_FIND_KELF;
    rc = FindRawKelfSource(report);
    if (rc < 0) {
        report->source_io_rc = rc;
        report->result = MG_RESULT_NO_TEST_KELF;
        return -1;
    }

    report->stage = MG_STAGE_READ_KELF;
    rc = ReadRawKelfSource(report, &data);
    if (rc < 0) {
        report->source_io_rc = rc;
        report->result = MG_RESULT_IO_ERROR;
        return -1;
    }

    report->stage = MG_STAGE_VALIDATE_KELF;
    rc = ValidateKelf(data, report->source_size);
    if (rc < 0) {
        report->source_io_rc = rc;
        report->result = MG_RESULT_INVALID_KELF;
        free(data);
        return -1;
    }

    buffer->data = data;
    buffer->size = report->source_size;
    buffer->source_port = report->source_port;
    snprintf(buffer->source_path, sizeof(buffer->source_path), "%s",
             report->source_path);

    header = (const SecrKELFHeader_t *)data;
    report->icvps2_required = (header->flags >> 1) & 1;
    report->source_io_rc = 0;
    report->stage = MG_STAGE_SESSION_SETUP;
    report->result = MG_RESULT_NOT_RUN;
    return 0;
}

int MagicGateProbePrepared(int target_port, const MagicGateKelfBuffer *buffer,
                           MagicGateReport *report)
{
    unsigned char kbit[16];
    unsigned char kc[16];
    unsigned char icvps2[8];
    SecrBitTable_t bit_table;
    unsigned int offset;
    unsigned int block_size;
    int type = 0;
    int free_clusters = 0;
    int formatted = 0;
    int rc;
    int i;

    if (buffer == NULL || buffer->data == NULL || buffer->size <= 0) {
        report->result = MG_RESULT_INVALID_KELF;
        return -1;
    }

    if (!SecrIopAvailable) {
        report->result = MG_RESULT_SECR_UNAVAILABLE;
        return -1;
    }

    report->stage = MG_STAGE_SESSION_CARD_CHECK;
    for (i = 0; i < MG_CARD_RETRIES; i++) {
        type = 0;
        free_clusters = 0;
        formatted = 0;
        mcGetInfo(target_port, 0, &type, &free_clusters, &formatted);
        rc = McSyncResult();

        report->session_mcinfo_rc = rc;
        report->session_type = type;
        report->session_free_clusters = free_clusters;
        report->session_formatted = formatted;

        if (rc != sceMcResChangedCard)
            break;
        if (type == MC_TYPE_PS2 && i == MG_CARD_RETRIES - 1)
            break;
        DelayThread(MG_CARD_RETRY_USEC);
    }

    if (rc < 0 && rc != sceMcResNoFormat &&
        !(rc == sceMcResChangedCard && type == MC_TYPE_PS2)) {
        report->result = MG_RESULT_SESSION_CARD_ERROR;
        return -1;
    }
    if (type != MC_TYPE_PS2) {
        report->result = MG_RESULT_TARGET_NOT_PS2;
        return -1;
    }

    report->stage = MG_STAGE_BIND_RPC;
    rc = BindDownloadRpc();
    report->rpc_rc = rc;
    if (rc < 0) {
        report->result = MG_RESULT_RPC_UNAVAILABLE;
        return -1;
    }

    memset(&bit_table, 0, sizeof(bit_table));
    report->stage = MG_STAGE_DOWNLOAD_HEADER;
    rc = DownloadHeader(target_port, 0, buffer->data, &bit_table,
                        &report->header_reply_size);
    if (rc < 0) {
        report->rpc_rc = rc;
        report->result = MG_RESULT_RPC_UNAVAILABLE;
        return -1;
    }
    report->header_rc = rc;
    if (rc == 0) {
        report->result = MG_RESULT_HEADER_FAILED;
        return -1;
    }

    report->block_count = bit_table.header.block_count;
    if (report->block_count < 0 || report->block_count > 63 ||
        bit_table.header.headersize > (unsigned int)buffer->size) {
        report->result = MG_RESULT_INVALID_KELF;
        return -1;
    }

    report->stage = MG_STAGE_DOWNLOAD_BLOCKS;
    offset = bit_table.header.headersize;
    for (i = 0; i < report->block_count; i++) {
        block_size = bit_table.blocks[i].size;

        /* Every BIT entry must fit within the raw KELF, but only entries marked
         * for security download are constrained by SECRSIF's 0x400-byte block
         * RPC buffer. Large plaintext payload entries are valid and only move
         * the source offset forward. */
        if (offset > (unsigned int)buffer->size ||
            block_size > (unsigned int)buffer->size - offset) {
            report->failed_block = i;
            report->result = MG_RESULT_INVALID_KELF;
            return -1;
        }

        if (bit_table.blocks[i].flags & 2) {
            report->encrypted_blocks++;
            if (block_size > 0x400) {
                report->failed_block = i;
                report->result = MG_RESULT_INVALID_KELF;
                return -1;
            }

            rc = DownloadBlock(buffer->data + offset, (int)block_size);
            if (rc < 0) {
                report->rpc_rc = rc;
                report->failed_block = i;
                report->result = MG_RESULT_RPC_UNAVAILABLE;
                return -1;
            }
            if (rc == 0) {
                report->failed_block = i;
                report->result = MG_RESULT_BLOCK_FAILED;
                return -1;
            }
            report->blocks_completed++;
        }

        offset += block_size;
    }

    report->stage = MG_STAGE_GET_KBIT;
    rc = DownloadGetKbit(target_port, 0, kbit);
    if (rc < 0) {
        report->rpc_rc = rc;
        report->result = MG_RESULT_RPC_UNAVAILABLE;
        return -1;
    }
    report->kbit_rc = rc;
    if (rc == 0) {
        report->result = MG_RESULT_KBIT_FAILED;
        return -1;
    }

    report->stage = MG_STAGE_GET_KC;
    rc = DownloadGetKc(target_port, 0, kc);
    if (rc < 0) {
        report->rpc_rc = rc;
        report->result = MG_RESULT_RPC_UNAVAILABLE;
        return -1;
    }
    report->kc_rc = rc;
    if (rc == 0) {
        report->result = MG_RESULT_KC_FAILED;
        return -1;
    }

    if (report->icvps2_required) {
        report->stage = MG_STAGE_GET_ICVPS2;
        rc = DownloadGetIcvps2(icvps2);
        if (rc < 0) {
            report->rpc_rc = rc;
            report->result = MG_RESULT_RPC_UNAVAILABLE;
            return -1;
        }
        report->icvps2_rc = rc;
        if (rc == 0) {
            report->result = MG_RESULT_ICVPS2_FAILED;
            return -1;
        }
    }

    report->stage = MG_STAGE_DONE;
    report->result = MG_RESULT_PASS;
    return 0;
}

const char *MagicGateStageText(MagicGateStage stage)
{
    switch (stage) {
        case MG_STAGE_FIND_KELF: return "FIND RAW FMCB.XLF";
        case MG_STAGE_READ_KELF: return "READ RAW FMCB.XLF";
        case MG_STAGE_VALIDATE_KELF: return "VALIDATE RAW KELF";
        case MG_STAGE_SESSION_SETUP: return "SETUP MG SESSION";
        case MG_STAGE_SESSION_CARD_CHECK: return "MG SESSION CARD CHECK";
        case MG_STAGE_BIND_RPC: return "BIND SECR RPC";
        case MG_STAGE_DOWNLOAD_HEADER: return "DOWNLOAD HEADER";
        case MG_STAGE_DOWNLOAD_BLOCKS: return "DOWNLOAD BLOCKS";
        case MG_STAGE_GET_KBIT: return "GET KBIT";
        case MG_STAGE_GET_KC: return "GET KC";
        case MG_STAGE_GET_ICVPS2: return "GET ICVPS2";
        case MG_STAGE_DONE: return "DONE";
        default: return "NOT RUN";
    }
}

const char *MagicGateResultText(MagicGateResult result)
{
    switch (result) {
        case MG_RESULT_PASS: return "PASS";
        case MG_RESULT_NO_TEST_KELF: return "RAW FMCB.XLF REQUIRED";
        case MG_RESULT_IO_ERROR: return "RAW KELF READ ERROR";
        case MG_RESULT_INVALID_KELF: return "INVALID/UNSUPPORTED RAW KELF";
        case MG_RESULT_SESSION_SETUP_FAILED: return "MG SESSION SETUP FAILED";
        case MG_RESULT_SESSION_CARD_ERROR: return "MG SESSION CARD ERROR";
        case MG_RESULT_RPC_UNAVAILABLE: return "SECR RPC FAILURE";
        case MG_RESULT_HEADER_FAILED: return "HEADER BIND FAILED";
        case MG_RESULT_BLOCK_FAILED: return "BLOCK BIND FAILED";
        case MG_RESULT_KBIT_FAILED: return "KBIT FAILED";
        case MG_RESULT_KC_FAILED: return "KC FAILED";
        case MG_RESULT_ICVPS2_FAILED: return "ICVPS2 FAILED";
        case MG_RESULT_SECR_UNAVAILABLE: return "SECR MODULES UNAVAILABLE";
        case MG_RESULT_TARGET_NOT_PS2: return "TARGET IS NOT A PS2 CARD";
        default: return "NOT RUN";
    }
}
