/* SPDX-License-Identifier: MIT */
/*
 * Drebin EE client for the private bulk-read extension injected into the
 * temporary legacy MCSERV.
 *
 * The public libmc ABI remains unchanged: callers still issue mcReadPage and
 * consume completion through mcSync. The private path batches physical card
 * reads and, in the hardware A/B build, keeps one next batch outstanding while
 * EE code consumes the current batch. This is deliberately a one-request-deep
 * pipeline: MCSERV exposes one server-data object and one staging buffer, so we
 * do not pretend that arbitrary RPC re-entry is safe.
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

#ifndef MCI_RAW_BULK_PAGES
#define MCI_RAW_BULK_PAGES 16
#endif

#ifndef MCI_RAW_BULK_ASYNC
#define MCI_RAW_BULK_ASYNC 0
#endif

#if MCI_RAW_BULK_PAGES != 4 && MCI_RAW_BULK_PAGES != 8 && MCI_RAW_BULK_PAGES != 16
#error "MCI_RAW_BULK_PAGES must be 4, 8 or 16"
#endif

#if MCI_RAW_BULK_ASYNC
#define MCI_CACHE_SLOTS 2
#else
#define MCI_CACHE_SLOTS 1
#endif

#if MCI_ENABLE_R5900_BENCH
#include "r5900_bench.h"
#endif

#define MCI_MCSERV_RPC_ID 0x80000400
#define MCI_MCSERV_BULK_READ_CMD 0x81
#define MCI_PAGE_BYTES 512
#define MCI_BULK_BYTES (MCI_RAW_BULK_PAGES * MCI_PAGE_BYTES)

/* High 16 bits returned by the IOP encode one ECC-warning bit per prefetched
 * page. Low 8 bits contain the number of pages DMA-transferred. */
typedef struct MciRawBulkRpcArgs {
    int port;
    int slot;
    u32 start_page;
    u32 page_count;
    void *ee_buffer;
} MciRawBulkRpcArgs;

typedef struct MciRawBulkCacheMeta {
    int valid;
    int port;
    int slot;
    u32 start;
    u32 count;
    u16 warning_mask;
} MciRawBulkCacheMeta;

static SifRpcClientData_t Client __attribute__((aligned(64)));
static MciRawBulkRpcArgs Send __attribute__((aligned(64)));
static int Receive __attribute__((aligned(64)));
static unsigned char CacheData[MCI_CACHE_SLOTS][MCI_BULK_BYTES]
    __attribute__((aligned(64)));
static MciRawBulkCacheMeta CacheMeta[MCI_CACHE_SLOTS];
static MciRawBulkReadStats Stats;
static int CurrentCache;
static u32 PageLimit;
static int Pending;
static int PendingResult;
static int LastPageValid;
static int LastPort;
static int LastSlot;
static u32 LastPage;

#if MCI_RAW_BULK_ASYNC
static int AsyncReceive __attribute__((aligned(64)));
static int AsyncActive;
static volatile int AsyncDone;
static int AsyncCache;
static int AsyncPort;
static int AsyncSlot;
static u32 AsyncStart;
static u32 AsyncCount;
static u64 AsyncIssueTicks;
static int AsyncSema = -1;
#endif

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

static void InvalidateCacheSlot(int index)
{
    if (index < 0 || index >= MCI_CACHE_SLOTS)
        return;
    memset(&CacheMeta[index], 0, sizeof(CacheMeta[index]));
    CacheMeta[index].port = -1;
    CacheMeta[index].slot = -1;
}

static void InvalidateAllCaches(void)
{
    int i;
    for (i = 0; i < MCI_CACHE_SLOTS; i++)
        InvalidateCacheSlot(i);
    CurrentCache = -1;
}

static u32 BatchStart(u32 page)
{
    return page & ~(u32)(MCI_RAW_BULK_PAGES - 1u);
}

static u32 BatchCount(u32 start)
{
    if (PageLimit != 0u) {
        u32 remaining;
        if (start >= PageLimit)
            return 0u;
        remaining = PageLimit - start;
        return remaining < MCI_RAW_BULK_PAGES ? remaining : MCI_RAW_BULK_PAGES;
    }
    return MCI_RAW_BULK_PAGES;
}

static int CacheContains(int index, int port, int slot, u32 page)
{
    const MciRawBulkCacheMeta *meta;
    if (index < 0 || index >= MCI_CACHE_SLOTS)
        return 0;
    meta = &CacheMeta[index];
    return meta->valid && meta->port == port && meta->slot == slot &&
           page >= meta->start && page < meta->start + meta->count;
}

static int CommitRpcResult(int index, int port, int slot, u32 start,
                           int rpc_result)
{
    unsigned int pages;

    Stats.last_rpc_rc = rpc_result;
    if (rpc_result < 0) {
        InvalidateCacheSlot(index);
        return rpc_result;
    }

    pages = (unsigned int)rpc_result & 0xFFu;
    if (pages == 0u || pages > MCI_RAW_BULK_PAGES) {
        InvalidateCacheSlot(index);
        return -3;
    }

    _InvalidDCache(CacheData[index], CacheData[index] + pages * MCI_PAGE_BYTES);
    CacheMeta[index].valid = 1;
    CacheMeta[index].port = port;
    CacheMeta[index].slot = slot;
    CacheMeta[index].start = start;
    CacheMeta[index].count = pages;
    CacheMeta[index].warning_mask =
        (u16)(((unsigned int)rpc_result >> 8) & 0xFFFFu);
    Stats.pages_fetched += pages;
    return 0;
}

static int FillCacheSync(int index, int port, int slot, u32 start)
{
    u32 count = BatchCount(start);
    u64 begin;
    int rc;

    if (!Stats.bound || count == 0u)
        return -1;

    /* IOP DMA targets cached EE memory. Push any dirty aliases out before the
     * transfer; CommitRpcResult invalidates the received range after completion. */
    _SyncDCache(CacheData[index], CacheData[index] + MCI_BULK_BYTES);

    Send.port = port;
    Send.slot = slot;
    Send.start_page = start;
    Send.page_count = count;
    Send.ee_buffer = CacheData[index];
    Receive = 0;

    begin = GetTimerSystemTime();
    rc = sceSifCallRpc(&Client, MCI_MCSERV_BULK_READ_CMD, 0,
                       &Send, sizeof(Send), &Receive, sizeof(Receive),
                       NULL, NULL);
    Stats.rpc_ticks += GetTimerSystemTime() - begin;
    Stats.rpc_calls++;
    if (rc < 0) {
        Stats.last_rpc_rc = rc;
        Stats.bound = 0;
        InvalidateCacheSlot(index);
        return rc;
    }
    return CommitRpcResult(index, port, slot, start, Receive);
}

#if MCI_RAW_BULK_ASYNC
static void AsyncEnd(void *unused)
{
    (void)unused;
    AsyncDone = 1;
    if (AsyncSema >= 0)
        iSignalSema(AsyncSema);
}

static int FinishAsync(void)
{
    int ready_before_wait;
    int wait_rc;
    int rc;
    u64 wait_begin;
    u64 now;

    if (!AsyncActive)
        return 0;

    ready_before_wait = AsyncDone != 0;
    wait_begin = GetTimerSystemTime();
    wait_rc = WaitSema(AsyncSema);
    now = GetTimerSystemTime();
    if (wait_rc < 0) {
        Stats.last_rpc_rc = wait_rc;
        AsyncActive = 0;
        InvalidateCacheSlot(AsyncCache);
        return wait_rc;
    }

    if (ready_before_wait) {
        Stats.async_ready_hits++;
    } else {
        Stats.async_waits++;
        Stats.async_wait_ticks += now - wait_begin;
    }
    Stats.rpc_ticks += now - AsyncIssueTicks;

    rc = CommitRpcResult(AsyncCache, AsyncPort, AsyncSlot,
                         AsyncStart, AsyncReceive);
    AsyncActive = 0;
    AsyncDone = 0;
    return rc;
}

static int StartAsync(int index, int port, int slot, u32 start)
{
    u32 count = BatchCount(start);
    int rc;

    if (!Stats.bound || AsyncActive || AsyncSema < 0 || count == 0u)
        return -1;

    InvalidateCacheSlot(index);
    _SyncDCache(CacheData[index], CacheData[index] + MCI_BULK_BYTES);

    Send.port = port;
    Send.slot = slot;
    Send.start_page = start;
    Send.page_count = count;
    Send.ee_buffer = CacheData[index];
    AsyncReceive = 0;
    AsyncDone = 0;
    AsyncCache = index;
    AsyncPort = port;
    AsyncSlot = slot;
    AsyncStart = start;
    AsyncCount = count;
    AsyncIssueTicks = GetTimerSystemTime();
    AsyncActive = 1;

    rc = sceSifCallRpc(&Client, MCI_MCSERV_BULK_READ_CMD,
                       SIF_RPC_M_NOWAIT,
                       &Send, sizeof(Send),
                       &AsyncReceive, sizeof(AsyncReceive),
                       AsyncEnd, NULL);
    Stats.rpc_calls++;
    Stats.async_submits++;
    if (rc < 0) {
        Stats.last_rpc_rc = rc;
        Stats.bound = 0;
        AsyncActive = 0;
        AsyncDone = 0;
        InvalidateCacheSlot(index);
        return rc;
    }
    return 0;
}

static void MaybeStartNext(int port, int slot, u32 page, int sequential)
{
    const MciRawBulkCacheMeta *meta;
    int next_index;
    u32 next_start;

    if (!sequential || PageLimit == 0u || AsyncActive || CurrentCache < 0)
        return;
    meta = &CacheMeta[CurrentCache];
    if (!meta->valid || meta->port != port || meta->slot != slot)
        return;

    next_start = meta->start + meta->count;
    if (next_start >= PageLimit || page >= next_start)
        return;

    next_index = CurrentCache ^ 1;
    (void)StartAsync(next_index, port, slot, next_start);
}
#endif

void MciRawBulkReadReset(void)
{
#if MCI_RAW_BULK_ASYNC
    /* Callers must drain before tearing down the IOP personality. Reset is also
     * used before a new session, where no request can legitimately be active. */
    AsyncActive = 0;
    AsyncDone = 0;
    AsyncCache = -1;
    AsyncPort = -1;
    AsyncSlot = -1;
    AsyncStart = 0u;
    AsyncCount = 0u;
    AsyncIssueTicks = 0u;
    AsyncReceive = 0;
    if (AsyncSema >= 0) {
        DeleteSema(AsyncSema);
        AsyncSema = -1;
    }
#endif
    memset(&Client, 0, sizeof(Client));
    memset(&Send, 0, sizeof(Send));
    Receive = 0;
    memset(&Stats, 0, sizeof(Stats));
    Stats.bind_rc = -999;
    Stats.last_rpc_rc = -999;
    Stats.batch_pages = MCI_RAW_BULK_PAGES;
    Stats.async_enabled = MCI_RAW_BULK_ASYNC ? 1 : 0;
    InvalidateAllCaches();
    PageLimit = 0u;
    Pending = 0;
    PendingResult = 0;
    LastPageValid = 0;
    LastPort = -1;
    LastSlot = -1;
    LastPage = 0u;
}

int MciRawBulkReadBind(void)
{
    int attempt;
    int rc = -1;

    memset(&Client, 0, sizeof(Client));
    InvalidateAllCaches();
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
#if MCI_RAW_BULK_ASYNC
            ee_sema_t sema;
            memset(&sema, 0, sizeof(sema));
            sema.max_count = 1;
            sema.init_count = 0;
            AsyncSema = CreateSema(&sema);
            if (AsyncSema < 0) {
                Stats.bind_rc = AsyncSema;
                Stats.bound = 0;
                return AsyncSema;
            }
#endif
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

void MciRawBulkReadSetPageLimit(u32 total_pages)
{
    PageLimit = total_pages;
}

int MciRawBulkReadDrain(void)
{
#if MCI_RAW_BULK_ASYNC
    if (AsyncActive)
        return FinishAsync();
#endif
    return 0;
}

int MciRawBulkReadTryPage(int port, int slot, int page, void *buffer)
{
    u32 requested;
    u32 start;
    u32 offset;
    int rc;
    int sequential;

    if (!Stats.bound || buffer == NULL || page < 0 || Pending)
        return 0;

    requested = (u32)page;
    sequential = LastPageValid && LastPort == port && LastSlot == slot &&
                 requested == LastPage + 1u;

    if (!CacheContains(CurrentCache, port, slot, requested)) {
        start = BatchStart(requested);
#if MCI_RAW_BULK_ASYNC
        if (AsyncActive && AsyncPort == port && AsyncSlot == slot &&
            AsyncStart == start) {
            int completed_cache = AsyncCache;
            rc = FinishAsync();
            if (rc == 0)
                CurrentCache = completed_cache;
        } else {
            if (AsyncActive) {
                Stats.async_discards++;
                (void)FinishAsync();
            }
            rc = -1;
        }
        if (rc < 0) {
            int fill_index = CurrentCache < 0 ? 0 : (CurrentCache ^ 1);
            rc = FillCacheSync(fill_index, port, slot, start);
            if (rc == 0)
                CurrentCache = fill_index;
        }
#else
        rc = FillCacheSync(0, port, slot, start);
        if (rc == 0)
            CurrentCache = 0;
#endif
        if (rc < 0 || !CacheContains(CurrentCache, port, slot, requested)) {
            Stats.fallback_calls++;
            LastPageValid = 0;
            return 0;
        }
    } else {
        Stats.cache_hits++;
    }

    offset = requested - CacheMeta[CurrentCache].start;
    MciFastCopy(buffer,
                CacheData[CurrentCache] + offset * MCI_PAGE_BYTES,
                MCI_PAGE_BYTES);
    PendingResult =
        (CacheMeta[CurrentCache].warning_mask & (1u << offset)) ? 1 : 0;
    if (PendingResult > 0)
        Stats.ecc_warning_pages++;

#if MCI_RAW_BULK_ASYNC
    MaybeStartNext(port, slot, requested, sequential);
#endif

    LastPageValid = 1;
    LastPort = port;
    LastSlot = slot;
    LastPage = requested;
    Pending = 1;
    return 1;
}

int MciRawBulkReadSyncPending(int mode, int *cmd, int *result, int *sync_rc)
{
    if (!Pending)
        return 0;

    (void)mode;
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
