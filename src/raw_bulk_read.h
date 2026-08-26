#ifndef MCI_RAW_BULK_READ_H
#define MCI_RAW_BULK_READ_H

#include <tamtypes.h>

typedef struct MciRawBulkReadStats {
    int bind_rc;
    int bound;
    unsigned int batch_pages;
    int async_enabled;
    unsigned int rpc_calls;
    unsigned int cache_hits;
    unsigned int fallback_calls;
    unsigned int pages_fetched;
    unsigned int ecc_warning_pages;
    unsigned int async_submits;
    unsigned int async_ready_hits;
    unsigned int async_waits;
    unsigned int async_discards;
    int last_rpc_rc;
    u64 rpc_ticks;
    u64 async_wait_ticks;
} MciRawBulkReadStats;

/* Reset all EE-local state before/after the temporary raw-card IOP personality. */
void MciRawBulkReadReset(void);

/* Bind the private bulk-read client to the patched legacy MCSERV. Failure is not
 * fatal: callers transparently fall back to stock mcReadPage semantics. */
int MciRawBulkReadBind(void);

/* Supply the current card page count once geometry has been qualified. Async
 * read-ahead is deliberately disabled until this limit is known, preventing a
 * speculative final batch from reading beyond the physical card. */
void MciRawBulkReadSetPageLimit(u32 total_pages);

/* Complete any outstanding speculative SIF request before the raw IOP
 * personality is torn down. A speculative read error is recorded but is not a
 * correctness failure for a stream that never consumed that batch. */
int MciRawBulkReadDrain(void);

/* Emit one bounded end-of-operation summary, including RPC tail latency and
 * async ready/wait counters. This is deliberately not a per-batch trace. */
void MciRawBulkReadLogStats(const char *phase);

/* Try to satisfy one logical mcReadPage request from the configured bulk cache.
 * Returns 1 when handled and arms MciRawBulkReadSyncPending(), 0 when the caller
 * must issue the ordinary libmc mcReadPage RPC instead. */
int MciRawBulkReadTryPage(int port, int slot, int page, void *buffer);

/* Preserve libmc's public mcReadPage -> mcSync contract after the private bulk
 * layer has already made the requested page available. */
int MciRawBulkReadSyncPending(int mode, int *cmd, int *result, int *sync_rc);

void MciRawBulkReadGetStats(MciRawBulkReadStats *stats);

#endif /* MCI_RAW_BULK_READ_H */
