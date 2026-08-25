/* SPDX-License-Identifier: MIT */
/*
 * Drebin EE client for the private bulk-read extension injected into the
 * temporary legacy MCSERV. The public libmc ABI remains unchanged: mcReadPage
 * still appears asynchronous to card_image.c, while this layer prefetches 16
 * logical pages with one SIF RPC and serves subsequent page requests from EE RAM.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <delaythread.h>
#include <libmc.h>
#include <timer.h>
#include <string.h>

#include "diag_log.h"
#include "raw_bulk_read.h"
#include "r5900_memops.h"

#ifndef MCI_ENABLE_R5900_BENCH
#define MCI_ENABLE_R5900_BENCH 0
#endif

#if MCI_ENABLE_R5900_BENCH
#include "r5900_bench.h"
#endif

#define MCI_MCSERV_RPC_ID 0x80000400
#define MCI_MCSERV_BULK_READ_CMD 0x81
#define MCI_BULK_PAGES 16
#define MCI_PAGE_BYTES 512
#define MCI_BULK_BYTES (MCI_BULK_PAGES * MCI_PAGE_BYTES)

/* High 16 bits returned by the IOP encode one ECC-warning bit per prefetched
 * page. Low 8 bits contain the number of pages DMA-transferred. */
typedef struct MciRawBulkRpcArgs {
    int port;
    int slot;
    u32 start_page;
    u32 page_count;
    void *ee_buffer;
} MciRawBulkRpcArgs;

static SifRpcClientData_t Client __attribute__((aligned(64)));
static MciRawBulkRpcArgs Send __attribute__((aligned(64)));
static int Receive __attribute__((aligned(64)));
static unsigned char Cache[MCI_BULK_BYTES] __attribute__((aligned(64)));
static MciRawBulkReadStats Stats;
static int CacheValid;
static int CachePort;
static int CacheSlot;
static u32 CacheStart;
static u32 CacheCount;
static u16 CacheWarningMask;
static int Pending;
static int PendingResult;

#if MCI_ENABLE_R5900_BENCH
static int CpuBenchDone;

static void RunCpuBenchOnce(void)
{
    MciR5900BenchReport report;
    int rc;

    if (CpuBenchDone)
        return;
    CpuBenchDone = 1;

    rc = MciR5900BenchRun(&report);
    if (rc < 0) {
        MciDiagLogPrintf("R5900-PERF", "kernel benchmark failed rc=%d", rc);
        return;
    }

    MciDiagLogPrintf(
        "R5900-PERF",
        "steady-state copy8k iter=%u cycles=%u dual=%u icmiss=%u dcmiss=%u branch=%u bpmiss=%u; crc8k iter=%u cycles=%u dual=%u icmiss=%u dcmiss=%u branch=%u bpmiss=%u; ecc512 iter=%u cycles=%u dual=%u icmiss=%u dcmiss=%u branch=%u bpmiss=%u hash=%08X",
        report.copy_iterations,
        report.copy_8k.cycles, report.copy_8k.dual_issues,
        report.copy_8k.icache_misses, report.copy_8k.dcache_misses,
        report.copy_8k.branches, report.copy_8k.branch_mispredicts,
        report.crc_iterations,
        report.crc_8k.cycles, report.crc_8k.dual_issues,
        report.crc_8k.icache_misses, report.crc_8k.dcache_misses,
        report.crc_8k.branches, report.crc_8k.branch_mispredicts,
        report.ecc_iterations,
        report.ecc_512.cycles, report.ecc_512.dual_issues,
        report.ecc_512.icache_misses, report.ecc_512.dcache_misses,
        report.ecc_512.branches, report.ecc_512.branch_mispredicts,
        report.result_hash);
}
#endif

static void InvalidateCache(void)
{
    CacheValid = 0;
    CachePort = -1;
    CacheSlot = -1;
    CacheStart = 0u;
    CacheCount = 0u;
    CacheWarningMask = 0u;
}

void MciRawBulkReadReset(void)
{
    memset(&Client, 0, sizeof(Client));
    memset(&Send, 0, sizeof(Send));
    Receive = 0;
    memset(&Stats, 0, sizeof(Stats));
    Stats.bind_rc = -999;
    Stats.last_rpc_rc = -999;
    InvalidateCache();
    Pending = 0;
    PendingResult = 0;
}

int MciRawBulkReadBind(void)
{
    int attempt;
    int rc = -1;

    memset(&Client, 0, sizeof(Client));
    InvalidateCache();
    Pending = 0;

#if MCI_ENABLE_R5900_BENCH
    /* Development/performance-lab build only. Normal backup latency and cache
     * state must not include a synthetic CPU benchmark before every first raw
     * session. Whole-operation telemetry is the production measurement path. */
    RunCpuBenchOnce();
#endif

    for (attempt = 0; attempt < 64; attempt++) {
        rc = sceSifBindRpc(&Client, MCI_MCSERV_RPC_ID, 0);
        if (rc < 0)
            break;
        if (Client.server != NULL) {
            Stats.bind_rc = 0;
            Stats.bound = 1;
            return 0;
        }
        DelayThread(1000);
    }

    Stats.bind_rc = rc < 0 ? rc : -2;
    Stats.bound = 0;
    return Stats.bind_rc;
}

static int FillCache(int port, int slot, u32 page)
{
    u32 start = page & ~(u32)(MCI_BULK_PAGES - 1);
    u64 begin;
    int rc;
    unsigned int pages;

    if (!Stats.bound)
        return -1;

    /* The IOP will DMA into this cached EE range. Write back any dirty lines
     * first so they can never overwrite the incoming transfer later, then
     * invalidate the range again once the synchronous RPC confirms DMA finish. */
    _SyncDCache(Cache, Cache + sizeof(Cache));

    Send.port = port;
    Send.slot = slot;
    Send.start_page = start;
    Send.page_count = MCI_BULK_PAGES;
    Send.ee_buffer = Cache;
    Receive = 0;

    begin = GetTimerSystemTime();
    rc = sceSifCallRpc(&Client, MCI_MCSERV_BULK_READ_CMD, 0,
                       &Send, sizeof(Send), &Receive, sizeof(Receive),
                       NULL, NULL);
    Stats.rpc_ticks += GetTimerSystemTime() - begin;
    Stats.rpc_calls++;
    Stats.last_rpc_rc = rc < 0 ? rc : Receive;
    if (rc < 0) {
        Stats.bound = 0;
        InvalidateCache();
        return rc;
    }
    if (Receive < 0) {
        InvalidateCache();
        return Receive;
    }

    pages = (unsigned int)Receive & 0xFFu;
    if (pages == 0u || pages > MCI_BULK_PAGES) {
        InvalidateCache();
        return -3;
    }

    _InvalidDCache(Cache, Cache + pages * MCI_PAGE_BYTES);
    CachePort = port;
    CacheSlot = slot;
    CacheStart = start;
    CacheCount = pages;
    CacheWarningMask = (u16)(((unsigned int)Receive >> 8) & 0xFFFFu);
    CacheValid = 1;
    Stats.pages_fetched += pages;
    return 0;
}

int MciRawBulkReadTryPage(int port, int slot, int page, void *buffer)
{
    u32 offset;
    int rc;

    if (!Stats.bound || buffer == NULL || page < 0 || Pending)
        return 0;

    if (!CacheValid || port != CachePort || slot != CacheSlot ||
        (u32)page < CacheStart || (u32)page >= CacheStart + CacheCount) {
        rc = FillCache(port, slot, (u32)page);
        if (rc < 0) {
            Stats.fallback_calls++;
            return 0;
        }
    } else {
        Stats.cache_hits++;
    }

    offset = (u32)page - CacheStart;
    /* Every cache page starts at a 512-byte boundary. The image engine also
     * supplies aligned page buffers, so the normal acquisition path now moves
     * this hot 512-byte copy through native R5900 LQ/SQ. MciFastCopy keeps the
     * wrapper safe for any future unaligned libmc caller. */
    MciFastCopy(buffer, Cache + offset * MCI_PAGE_BYTES, MCI_PAGE_BYTES);
    PendingResult = (CacheWarningMask & (1u << offset)) ? 1 : 0;
    if (PendingResult > 0)
        Stats.ecc_warning_pages++;
    Pending = 1;
    return 1;
}

int MciRawBulkReadSyncPending(int mode, int *cmd, int *result, int *sync_rc)
{
    if (!Pending)
        return 0;

    (void)mode; /* Bulk RPC already completed synchronously before mcReadPage returned. */
    if (cmd != NULL)
        *cmd = MC_FUNC_READ_PAGE;
    if (result != NULL)
        *result = PendingResult;
    if (sync_rc != NULL)
        *sync_rc = 1;
    Pending = 0;
    PendingResult = 0;
    return 1;
}

void MciRawBulkReadGetStats(MciRawBulkReadStats *stats)
{
    if (stats != NULL)
        *stats = Stats;
}
