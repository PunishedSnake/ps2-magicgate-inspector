/*
 * PS2 Memory Card Inspector - MagicGate / KELF diagnostics
 *
 * This module deliberately avoids the all-or-nothing SecrDownloadFile()
 * helper.  The Inspector needs to know which individual SECR operation failed,
 * and it must never hang forever waiting for an RPC server.  Therefore the
 * five download RPC endpoints are bound with finite retries and exercised one
 * stage at a time.
 *
 * The KELF is only modified inside EE RAM.  No bound Kbit/Kc/ICVPS2 data is
 * written back to a memory card by this module.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <libmc.h>
#include <libsecr-common.h>
#include <secrsif.h>
#include <io_common.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "magicgate.h"

#define MG_RPC_RETRIES 300
#define MG_RPC_RETRY_USEC 1000
#define MG_READ_CHUNK 4096
#define MG_MAX_KELF_SIZE (4 * 1024 * 1024)
#define MG_RPC_TIMEOUT (-2100)
#define MG_SHORT_READ (-2101)
#define MG_INVALID_LAYOUT (-2102)

extern unsigned char secrman_irx[];
extern unsigned int size_secrman_irx;
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
static sceMcTblGetDir MgDirEntry __attribute__((aligned(64)));

static const char *KelfCandidates[] = {
    "/BIEXEC-SYSTEM/osdmain.elf",
    "/BAEXEC-SYSTEM/osdmain.elf",
    "/BEEXEC-SYSTEM/osdmain.elf",
    "/BCEXEC-SYSTEM/osdmain.elf",
    "/BIEXEC-SYSTEM/osd130.elf",
    "/BEEXEC-SYSTEM/osd130.elf",
    "/BAEXEC-SYSTEM/osd130.elf",
    "/BAEXEC-SYSTEM/osd120.elf"
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

int MagicGateLoadIopModules(MagicGateIopStatus *status)
{
    int rc;
    int start_rc;

    memset(status, 0, sizeof(*status));
    status->secrman_load_rc = -999;
    status->secrman_start_rc = -999;
    status->secrsif_load_rc = -999;
    status->secrsif_start_rc = -999;

    SecrIopAvailable = 0;
    RpcBound = 0;

    SifInitIopHeap();
    sbv_patch_enable_lmb();

    start_rc = -999;
    rc = SifExecModuleBuffer(secrman_irx, size_secrman_irx, 0, NULL, &start_rc);
    status->secrman_load_rc = rc;
    status->secrman_start_rc = start_rc;
    if (rc < 0) {
        SifExitIopHeap();
        return rc;
    }

    start_rc = -999;
    rc = SifExecModuleBuffer(secrsif_irx, size_secrsif_irx, 0, NULL, &start_rc);
    status->secrsif_load_rc = rc;
    status->secrsif_start_rc = start_rc;

    SifExitIopHeap();

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
    memcpy(param->buffer, src, sizeof(param->buffer));
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

static int FindKelfOnPort(int port, char *path, int path_size, int *file_size)
{
    unsigned int i;
    int rc;

    for (i = 0; i < sizeof(KelfCandidates) / sizeof(KelfCandidates[0]); i++) {
        memset(&MgDirEntry, 0, sizeof(MgDirEntry));
        mcGetDir(port, 0, KelfCandidates[i], 0, 1, &MgDirEntry);
        rc = McSyncResult();

        if (rc > 0 && MgDirEntry.FileSizeByte >= sizeof(SecrKELFHeader_t)) {
            snprintf(path, path_size, "%s", KelfCandidates[i]);
            *file_size = (int)MgDirEntry.FileSizeByte;
            return 0;
        }
    }

    return sceMcResNoEntry;
}

static int FindKelfSource(int target_port, MagicGateReport *report)
{
    int ports[2];
    int i;
    int rc;

    /* Prefer the other slot: this is ideal for testing a blank/rejected card. */
    ports[0] = target_port ^ 1;
    ports[1] = target_port;

    for (i = 0; i < 2; i++) {
        rc = FindKelfOnPort(ports[i], report->source_path,
                            sizeof(report->source_path), &report->source_size);
        if (rc == 0) {
            report->source_port = ports[i];
            report->source_io_rc = 0;
            return 0;
        }
        report->source_io_rc = rc;
    }

    return sceMcResNoEntry;
}

static int ReadKelfSource(const MagicGateReport *report, unsigned char **out_buffer)
{
    unsigned char *buffer;
    int alloc_size;
    int total;
    int chunk;
    int fd;
    int rc;

    if (report->source_size <= 0 || report->source_size > MG_MAX_KELF_SIZE)
        return MG_INVALID_LAYOUT;

    /* Extra zero padding makes the fixed 0x400-byte SECR RPC copy safe. */
    alloc_size = report->source_size + 0x400;
    buffer = memalign(64, alloc_size);
    if (buffer == NULL)
        return -ENOMEM;
    memset(buffer, 0, alloc_size);

    mcOpen(report->source_port, 0, report->source_path, FIO_O_RDONLY);
    fd = McSyncResult();
    if (fd < 0) {
        free(buffer);
        return fd;
    }

    total = 0;
    while (total < report->source_size) {
        chunk = report->source_size - total;
        if (chunk > MG_READ_CHUNK)
            chunk = MG_READ_CHUNK;

        mcRead(fd, buffer + total, chunk);
        rc = McSyncResult();
        if (rc < 0) {
            mcClose(fd);
            McSyncResult();
            free(buffer);
            return rc;
        }
        if (rc == 0 || rc > chunk) {
            mcClose(fd);
            McSyncResult();
            free(buffer);
            return MG_SHORT_READ;
        }
        total += rc;
    }

    mcClose(fd);
    rc = McSyncResult();
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

int MagicGateProbeCard(int target_port, MagicGateReport *report)
{
    unsigned char *kelf = NULL;
    unsigned char kbit[16];
    unsigned char kc[16];
    unsigned char icvps2[8];
    const SecrKELFHeader_t *kelf_header;
    SecrBitTable_t bit_table;
    unsigned int offset;
    unsigned int block_size;
    int rc;
    int i;

    MagicGateResetReport(report, target_port);

    if (!SecrIopAvailable) {
        report->result = MG_RESULT_SECR_UNAVAILABLE;
        return -1;
    }

    report->stage = MG_STAGE_FIND_KELF;
    rc = FindKelfSource(target_port, report);
    if (rc < 0) {
        report->source_io_rc = rc;
        report->result = MG_RESULT_NO_TEST_KELF;
        return -1;
    }

    report->stage = MG_STAGE_READ_KELF;
    rc = ReadKelfSource(report, &kelf);
    if (rc < 0) {
        report->source_io_rc = rc;
        report->result = MG_RESULT_IO_ERROR;
        return -1;
    }

    report->stage = MG_STAGE_VALIDATE_KELF;
    rc = ValidateKelf(kelf, report->source_size);
    if (rc < 0) {
        report->source_io_rc = rc;
        report->result = MG_RESULT_INVALID_KELF;
        free(kelf);
        return -1;
    }
    kelf_header = (const SecrKELFHeader_t *)kelf;
    report->icvps2_required = (kelf_header->flags >> 1) & 1;

    report->stage = MG_STAGE_BIND_RPC;
    rc = BindDownloadRpc();
    report->rpc_rc = rc;
    if (rc < 0) {
        report->result = MG_RESULT_RPC_UNAVAILABLE;
        free(kelf);
        return -1;
    }

    memset(&bit_table, 0, sizeof(bit_table));
    report->stage = MG_STAGE_DOWNLOAD_HEADER;
    rc = DownloadHeader(target_port, 0, kelf, &bit_table,
                        &report->header_reply_size);
    if (rc < 0) {
        report->rpc_rc = rc;
        report->result = MG_RESULT_RPC_UNAVAILABLE;
        free(kelf);
        return -1;
    }
    report->header_rc = rc;
    if (rc == 0) {
        report->result = MG_RESULT_HEADER_FAILED;
        free(kelf);
        return -1;
    }

    report->block_count = bit_table.header.block_count;
    if (report->block_count < 0 || report->block_count > 63 ||
        bit_table.header.headersize > (unsigned int)report->source_size) {
        report->result = MG_RESULT_INVALID_KELF;
        free(kelf);
        return -1;
    }

    report->stage = MG_STAGE_DOWNLOAD_BLOCKS;
    offset = bit_table.header.headersize;
    for (i = 0; i < report->block_count; i++) {
        block_size = bit_table.blocks[i].size;
        if (offset > (unsigned int)report->source_size ||
            block_size > (unsigned int)report->source_size - offset ||
            block_size > 0x400) {
            report->failed_block = i;
            report->result = MG_RESULT_INVALID_KELF;
            free(kelf);
            return -1;
        }

        if (bit_table.blocks[i].flags & 2) {
            report->encrypted_blocks++;
            rc = DownloadBlock(kelf + offset, (int)block_size);
            if (rc < 0) {
                report->rpc_rc = rc;
                report->failed_block = i;
                report->result = MG_RESULT_RPC_UNAVAILABLE;
                free(kelf);
                return -1;
            }
            if (rc == 0) {
                report->failed_block = i;
                report->result = MG_RESULT_BLOCK_FAILED;
                free(kelf);
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
        free(kelf);
        return -1;
    }
    report->kbit_rc = rc;
    if (rc == 0) {
        report->result = MG_RESULT_KBIT_FAILED;
        free(kelf);
        return -1;
    }

    report->stage = MG_STAGE_GET_KC;
    rc = DownloadGetKc(target_port, 0, kc);
    if (rc < 0) {
        report->rpc_rc = rc;
        report->result = MG_RESULT_RPC_UNAVAILABLE;
        free(kelf);
        return -1;
    }
    report->kc_rc = rc;
    if (rc == 0) {
        report->result = MG_RESULT_KC_FAILED;
        free(kelf);
        return -1;
    }

    if (report->icvps2_required) {
        report->stage = MG_STAGE_GET_ICVPS2;
        rc = DownloadGetIcvps2(icvps2);
        if (rc < 0) {
            report->rpc_rc = rc;
            report->result = MG_RESULT_RPC_UNAVAILABLE;
            free(kelf);
            return -1;
        }
        report->icvps2_rc = rc;
        if (rc == 0) {
            report->result = MG_RESULT_ICVPS2_FAILED;
            free(kelf);
            return -1;
        }
    }

    report->stage = MG_STAGE_DONE;
    report->result = MG_RESULT_PASS;
    free(kelf);
    return 0;
}

const char *MagicGateStageText(MagicGateStage stage)
{
    switch (stage) {
        case MG_STAGE_FIND_KELF: return "FIND TEST KELF";
        case MG_STAGE_READ_KELF: return "READ TEST KELF";
        case MG_STAGE_VALIDATE_KELF: return "VALIDATE KELF";
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
        case MG_RESULT_NO_TEST_KELF: return "NO TEST KELF";
        case MG_RESULT_IO_ERROR: return "KELF READ ERROR";
        case MG_RESULT_INVALID_KELF: return "INVALID/UNSUPPORTED KELF";
        case MG_RESULT_RPC_UNAVAILABLE: return "SECR RPC FAILURE";
        case MG_RESULT_HEADER_FAILED: return "HEADER BIND FAILED";
        case MG_RESULT_BLOCK_FAILED: return "BLOCK BIND FAILED";
        case MG_RESULT_KBIT_FAILED: return "KBIT FAILED";
        case MG_RESULT_KC_FAILED: return "KC FAILED";
        case MG_RESULT_ICVPS2_FAILED: return "ICVPS2 FAILED";
        case MG_RESULT_SECR_UNAVAILABLE: return "SECR MODULES UNAVAILABLE";
        default: return "NOT RUN";
    }
}
