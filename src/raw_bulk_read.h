#ifndef MCI_RAW_BULK_READ_H
#define MCI_RAW_BULK_READ_H

#include <tamtypes.h>

typedef struct MciRawBulkReadStats {
    int bind_rc;
    int bound;
    unsigned int rpc_calls;
    unsigned int cache_hits;
    unsigned int fallback_calls;
    unsigned int pages_fetched;
    unsigned int ecc_warning_pages;
    int last_rpc_rc;
    u64 rpc_ticks;
} MciRawBulkReadStats;

/* Reset all EE-local state before/after the temporary raw-card IOP personality. */
void MciRawBulkReadReset(void);

/* Bind a second EE RPC client to the patched legacy MCSERV. Failure is not
 * fatal: callers transparently fall back to stock mcReadPage semantics. */
int MciRawBulkReadBind(void);

/* Try to satisfy one logical mcReadPage request from a 16-page bulk cache.
 * Returns 1 when handled and arms MciRawBulkReadSyncPending(), 0 when the caller
 * must issue the ordinary libmc mcReadPage RPC instead. */
int MciRawBulkReadTryPage(int port, int slot, int page, void *buffer);

/* Emulate the asynchronous libmc mcReadPage/mcSync contract after a bulk request
 * that was completed synchronously under the hood. Returns 1 if it consumed a
 * pending bulk result, 0 otherwise. */
int MciRawBulkReadSyncPending(int mode, int *cmd, int *result, int *sync_rc);

void MciRawBulkReadGetStats(MciRawBulkReadStats *stats);

#endif /* MCI_RAW_BULK_READ_H */
