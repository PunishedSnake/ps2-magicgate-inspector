/* SPDX-License-Identifier: MIT */
/*
 * Optional Performance Lab probe for the direct image write-slot path.
 *
 * This file is linked only when IMAGE_WRITE_PROBE=1. Production/performance
 * candidate builds keep it out of the binary so the per-record counters do not
 * perturb the primary wall-time A/B. The probe uses ld --wrap around the narrow
 * reserve/commit ownership API and therefore does not modify the write-behind
 * state machine itself.
 */

#include <tamtypes.h>

#include "diag_log.h"
#include "image_write_behind.h"

static unsigned int ProbeDepth;
static u32 ReserveCalls;
static u32 DirectReservations;
static u32 ReserveFallbacks;
static u32 ReserveErrors;
static u32 CommitCalls;
static u32 DirectCommits;
static u32 CommitErrors;
static u64 DirectCommittedBytes;

void __real_MciImageWriteBehindSetEnabled(int enabled);
int __real_MciImageWriteBehindReserve(int fd, int size, void **buffer);
int __real_MciImageWriteBehindCommit(int fd, void *buffer, int size);

static void ResetProbe(void)
{
    ReserveCalls = 0u;
    DirectReservations = 0u;
    ReserveFallbacks = 0u;
    ReserveErrors = 0u;
    CommitCalls = 0u;
    DirectCommits = 0u;
    CommitErrors = 0u;
    DirectCommittedBytes = 0u;
}

void __wrap_MciImageWriteBehindSetEnabled(int enabled)
{
    if (enabled) {
        if (ProbeDepth++ == 0u)
            ResetProbe();
        __real_MciImageWriteBehindSetEnabled(1);
        return;
    }

    if (ProbeDepth == 0u) {
        __real_MciImageWriteBehindSetEnabled(0);
        return;
    }

    ProbeDepth--;
    __real_MciImageWriteBehindSetEnabled(0);
    if (ProbeDepth != 0u)
        return;

    MciDiagLogTracePrintf(
        "IMAGE-IO-PROBE",
        "direct-slot reserve_calls=%u direct_reservations=%u reserve_fallbacks=%u reserve_errors=%u commit_calls=%u direct_commits=%u commit_errors=%u direct_committed_bytes=%llu",
        ReserveCalls, DirectReservations, ReserveFallbacks, ReserveErrors,
        CommitCalls, DirectCommits, CommitErrors,
        (unsigned long long)DirectCommittedBytes);
}

int __wrap_MciImageWriteBehindReserve(int fd, int size, void **buffer)
{
    int rc;

    ReserveCalls++;
    rc = __real_MciImageWriteBehindReserve(fd, size, buffer);
    if (rc > 0)
        DirectReservations++;
    else if (rc == 0)
        ReserveFallbacks++;
    else
        ReserveErrors++;
    return rc;
}

int __wrap_MciImageWriteBehindCommit(int fd, void *buffer, int size)
{
    int rc;

    CommitCalls++;
    rc = __real_MciImageWriteBehindCommit(fd, buffer, size);
    if (rc == size) {
        DirectCommits++;
        DirectCommittedBytes += (u64)(unsigned int)size;
    } else {
        CommitErrors++;
    }
    return rc;
}
