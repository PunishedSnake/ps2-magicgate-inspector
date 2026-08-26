/* SPDX-License-Identifier: MIT */
/*
 * Image-only USB write-behind for Drebin.
 *
 * The normal production build batches image records and writes them
 * synchronously. P0 performance-lab builds can additionally keep exactly one
 * fileXio write outstanding while the EE fills a second buffer. The queue depth
 * is deliberately one: USB Mass Storage BOT is command-serialized, and the
 * PS2SDK fileXio client itself exposes one global async completion state.
 */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <timer.h>
#include <string.h>

#include "diag_log.h"
#include "image_write_behind.h"
#include "r5900_memops.h"

#ifndef MCI_IMAGE_READ_AHEAD_ASYNC
#define MCI_IMAGE_READ_AHEAD_ASYNC 0
#endif

#if MCI_IMAGE_READ_AHEAD_ASYNC
#include "image_read_ahead.h"
#endif

#ifndef MCI_IMAGE_WRITE_PAGES
#define MCI_IMAGE_WRITE_PAGES 32
#endif

#ifndef MCI_IMAGE_WRITE_ASYNC
#define MCI_IMAGE_WRITE_ASYNC 0
#endif

#if MCI_IMAGE_WRITE_PAGES != 16 && MCI_IMAGE_WRITE_PAGES != 32 && \
    MCI_IMAGE_WRITE_PAGES != 64 && MCI_IMAGE_WRITE_PAGES != 128
#error "MCI_IMAGE_WRITE_PAGES must be 16, 32, 64 or 128"
#endif

#if MCI_IMAGE_WRITE_ASYNC
#define MCI_IMAGE_WRITE_SLOTS 2
#else
#define MCI_IMAGE_WRITE_SLOTS 1
#endif

#define MCI_IMAGE_WRITE_MAX_STRIDE 528
#define MCI_IMAGE_WRITE_BYTES (MCI_IMAGE_WRITE_MAX_STRIDE * MCI_IMAGE_WRITE_PAGES)
#define MCI_IO_LATENCY_BUCKETS 256u
#define MCI_IO_TICKS_PER_MS (kBUSCLK / 1000u)

typedef struct MciImageWriteSlot {
    int fd;
    int stride;
    int length;
} MciImageWriteSlot;

static unsigned char Cache[MCI_IMAGE_WRITE_SLOTS][MCI_IMAGE_WRITE_BYTES]
    __attribute__((aligned(64)));
static MciImageWriteSlot Slots[MCI_IMAGE_WRITE_SLOTS];
static unsigned int EnableDepth;
static int FillSlot;
static int ActiveFd;
static int ActiveStride;
static int LastError;

static u64 OperationStart;
static u64 UnderlyingTicks;
static u64 LogicalBytes;
static u64 UnderlyingBytes;
static u64 MaxWriteTicks;
static u64 AsyncWaitTicks;
static u32 LogicalWrites;
static u32 UnderlyingWrites;
static u32 AsyncSubmits;
static u32 AsyncReadyHits;
static u32 AsyncWaits;
static u32 WriteLatencyHistogram[MCI_IO_LATENCY_BUCKETS];

#if MCI_IMAGE_WRITE_ASYNC
static int AsyncActive;
static int AsyncSlot;
static int AsyncExpected;
static u64 AsyncIssueTicks;
#endif

int __real_fileXioWrite(int fd, const void *buffer, int size);
int __real_fileXioClose(int fd);

static void ResetSlot(int index)
{
    if (index < 0 || index >= MCI_IMAGE_WRITE_SLOTS)
        return;
    Slots[index].fd = -1;
    Slots[index].stride = 0;
    Slots[index].length = 0;
}

static void ResetPipeline(void)
{
    int i;

    for (i = 0; i < MCI_IMAGE_WRITE_SLOTS; i++)
        ResetSlot(i);
    FillSlot = 0;
    ActiveFd = -1;
    ActiveStride = 0;
    LastError = 0;
#if MCI_IMAGE_WRITE_ASYNC
    AsyncActive = 0;
    AsyncSlot = -1;
    AsyncExpected = 0;
    AsyncIssueTicks = 0u;
#endif
    /* fileXio block mode is global to the EE client. Never let a previous
     * aborted image operation leak NOWAIT semantics into unrelated callers. */
    fileXioSetBlockMode(FXIO_WAIT);
}

static void ResetStats(void)
{
    OperationStart = GetTimerSystemTime();
    UnderlyingTicks = 0u;
    LogicalBytes = 0u;
    UnderlyingBytes = 0u;
    MaxWriteTicks = 0u;
    AsyncWaitTicks = 0u;
    LogicalWrites = 0u;
    UnderlyingWrites = 0u;
    AsyncSubmits = 0u;
    AsyncReadyHits = 0u;
    AsyncWaits = 0u;
    memset(WriteLatencyHistogram, 0, sizeof(WriteLatencyHistogram));
}

static void RecordWriteLatency(u64 ticks)
{
    u64 bucket64 = ticks / (u64)MCI_IO_TICKS_PER_MS;
    unsigned int bucket = bucket64 >= MCI_IO_LATENCY_BUCKETS
                              ? MCI_IO_LATENCY_BUCKETS - 1u
                              : (unsigned int)bucket64;

    WriteLatencyHistogram[bucket]++;
    if (ticks > MaxWriteTicks)
        MaxWriteTicks = ticks;
}

static unsigned int WritePercentileMsFloor(unsigned int percentile)
{
    unsigned int target;
    unsigned int seen = 0u;
    unsigned int i;

    if (UnderlyingWrites == 0u)
        return 0u;
    target = (UnderlyingWrites * percentile + 99u) / 100u;
    for (i = 0u; i < MCI_IO_LATENCY_BUCKETS; i++) {
        seen += WriteLatencyHistogram[i];
        if (seen >= target)
            return i;
    }
    return MCI_IO_LATENCY_BUCKETS - 1u;
}

static u64 TicksToUsec(u64 ticks)
{
    u32 seconds = 0u;
    u32 useconds = 0u;

    TimerBusClock2USec(ticks, &seconds, &useconds);
    return (u64)seconds * 1000000u + useconds;
}

static int WriteSynchronously(int fd, const unsigned char *buffer,
                              int length, int offset)
{
    int done = offset;

    fileXioSetBlockMode(FXIO_WAIT);
    while (done < length) {
        int rc;
        u64 begin = GetTimerSystemTime();
        u64 elapsed;

        rc = __real_fileXioWrite(fd, buffer + done, length - done);
        elapsed = GetTimerSystemTime() - begin;
        UnderlyingTicks += elapsed;
        UnderlyingWrites++;
        RecordWriteLatency(elapsed);
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        UnderlyingBytes += (u64)(unsigned int)rc;
        done += rc;
    }
    return 0;
}

#if MCI_IMAGE_WRITE_ASYNC
static int FinishAsync(void)
{
    int poll_rc;
    int result = -999;
    int rc;
    u64 now;
    u64 wait_begin;
    u64 service_ticks;
    MciImageWriteSlot *slot;

    if (!AsyncActive)
        return 0;

    slot = &Slots[AsyncSlot];
    poll_rc = fileXioWaitAsync(FXIO_NOWAIT, &result);
    if (poll_rc < 0) {
        fileXioSetBlockMode(FXIO_WAIT);
        AsyncActive = 0;
        return poll_rc;
    }

    if (poll_rc == FXIO_INCOMPLETE) {
        wait_begin = GetTimerSystemTime();
        AsyncWaits++;
        poll_rc = fileXioWaitAsync(FXIO_WAIT, &result);
        now = GetTimerSystemTime();
        AsyncWaitTicks += now - wait_begin;
        if (poll_rc < 0) {
            fileXioSetBlockMode(FXIO_WAIT);
            AsyncActive = 0;
            return poll_rc;
        }
        if (poll_rc != FXIO_COMPLETE) {
            fileXioSetBlockMode(FXIO_WAIT);
            AsyncActive = 0;
            return -2;
        }
    } else if (poll_rc == FXIO_COMPLETE) {
        AsyncReadyHits++;
        now = GetTimerSystemTime();
    } else {
        fileXioSetBlockMode(FXIO_WAIT);
        AsyncActive = 0;
        return -3;
    }

    if (poll_rc == FXIO_COMPLETE && AsyncWaits == 0u)
        now = GetTimerSystemTime();
    else
        now = GetTimerSystemTime();

    service_ticks = now - AsyncIssueTicks;
    UnderlyingTicks += service_ticks;
    RecordWriteLatency(service_ticks);
    fileXioSetBlockMode(FXIO_WAIT);
    AsyncActive = 0;

    if (result <= 0) {
        ResetSlot(AsyncSlot);
        return result < 0 ? result : -1;
    }

    UnderlyingBytes += (u64)(unsigned int)result;
    rc = 0;
    if (result < AsyncExpected)
        rc = WriteSynchronously(slot->fd, Cache[AsyncSlot], AsyncExpected,
                                result);
    else if (result > AsyncExpected)
        rc = -4;

    ResetSlot(AsyncSlot);
    AsyncSlot = -1;
    AsyncExpected = 0;
    return rc;
}

static int SubmitAsync(int index)
{
    MciImageWriteSlot *slot = &Slots[index];
    int rc;

    if (AsyncActive || slot->length <= 0)
        return -1;

    fileXioSetBlockMode(FXIO_NOWAIT);
    AsyncIssueTicks = GetTimerSystemTime();
    rc = __real_fileXioWrite(slot->fd, Cache[index], slot->length);
    UnderlyingWrites++;
    AsyncSubmits++;
    if (rc < 0) {
        u64 elapsed = GetTimerSystemTime() - AsyncIssueTicks;
        UnderlyingTicks += elapsed;
        RecordWriteLatency(elapsed);
        fileXioSetBlockMode(FXIO_WAIT);
        return rc;
    }

    /* CURRENT IMPLEMENTATION: pinned PS2SDK fileXio returns zero when a NOWAIT
     * RPC was accepted. The real byte count is published through WaitAsync. */
    AsyncActive = 1;
    AsyncSlot = index;
    AsyncExpected = slot->length;
    return 0;
}
#endif

static int FlushFullSlot(int index)
{
#if MCI_IMAGE_WRITE_ASYNC
    int rc;

    rc = FinishAsync();
    if (rc < 0)
        return rc;
    rc = SubmitAsync(index);
    if (rc < 0)
        return rc;
    FillSlot = index ^ 1;
    ResetSlot(FillSlot);
    return 0;
#else
    int rc = WriteSynchronously(Slots[index].fd, Cache[index],
                                Slots[index].length, 0);
    ResetSlot(index);
    return rc;
#endif
}

static int DrainAsync(void)
{
#if MCI_IMAGE_WRITE_ASYNC
    int rc = FinishAsync();
    fileXioSetBlockMode(FXIO_WAIT);
    return rc;
#else
    return 0;
#endif
}

static int PendingBytes(void)
{
    int pending = 0;
    int i;

    for (i = 0; i < MCI_IMAGE_WRITE_SLOTS; i++)
        pending += Slots[i].length;
    return pending;
}

void MciImageWriteBehindSetEnabled(int enabled)
{
    int teardown_rc = 0;

    if (enabled) {
        if (EnableDepth++ == 0u) {
            ResetPipeline();
            ResetStats();
        }
        return;
    }

    if (EnableDepth == 0u)
        return;
    EnableDepth--;
    if (EnableDepth != 0u)
        return;

    /* Normally fileXioClose drained the only outstanding write. This final
     * drain is a teardown safety net for early-return/error paths. Do not flush
     * a partially filled batch: on aborted image creation its fd may already be
     * dead and the partial file is removed by the image engine. */
    teardown_rc = DrainAsync();
    if (teardown_rc < 0 && LastError == 0)
        LastError = teardown_rc;

    MciDiagLogTracePrintf(
        "IMAGE-IO",
        "write-behind end logical_calls=%u logical_bytes=%llu underlying_calls=%u underlying_bytes=%llu underlying_ticks=%llu operation_ticks=%llu batch_pages=%u async=%d async_submits=%u async_ready=%u async_waits=%u async_wait_ticks=%llu pending_bytes=%d last_error=%d batch_p50_ms_floor=%u batch_p95_ms_floor=%u batch_p99_ms_floor=%u batch_max_us=%llu",
        LogicalWrites, (unsigned long long)LogicalBytes,
        UnderlyingWrites, (unsigned long long)UnderlyingBytes,
        (unsigned long long)UnderlyingTicks,
        (unsigned long long)(GetTimerSystemTime() - OperationStart),
        (unsigned int)MCI_IMAGE_WRITE_PAGES, MCI_IMAGE_WRITE_ASYNC ? 1 : 0,
        AsyncSubmits, AsyncReadyHits, AsyncWaits,
        (unsigned long long)AsyncWaitTicks, PendingBytes(), LastError,
        WritePercentileMsFloor(50u), WritePercentileMsFloor(95u),
        WritePercentileMsFloor(99u),
        (unsigned long long)TicksToUsec(MaxWriteTicks));
    ResetPipeline();
}

int __wrap_fileXioWrite(int fd, const void *buffer, int size)
{
    MciImageWriteSlot *slot;
    int rc;

    if (EnableDepth == 0u || buffer == NULL || (size != 512 && size != 528)) {
        rc = DrainAsync();
        if (rc < 0)
            return rc;
        return __real_fileXioWrite(fd, buffer, size);
    }

    LogicalWrites++;
    LogicalBytes += (u64)(unsigned int)size;

    if (ActiveFd >= 0 && (fd != ActiveFd || size != ActiveStride)) {
        rc = DrainAsync();
        if (rc < 0) {
            LastError = rc;
            return rc;
        }
        ResetSlot(FillSlot);
        ActiveFd = -1;
        ActiveStride = 0;
    }
    if (ActiveFd < 0) {
        ActiveFd = fd;
        ActiveStride = size;
    }

    slot = &Slots[FillSlot];
    if (slot->length == 0) {
        slot->fd = fd;
        slot->stride = size;
    }

    if (slot->length + size > (int)sizeof(Cache[FillSlot])) {
        rc = FlushFullSlot(FillSlot);
        if (rc < 0) {
            LastError = rc;
            return rc;
        }
        slot = &Slots[FillSlot];
        slot->fd = fd;
        slot->stride = size;
    }

    MciFastCopy(Cache[FillSlot] + slot->length, buffer, (unsigned int)size);
    slot->length += size;

    if (slot->length == slot->stride * MCI_IMAGE_WRITE_PAGES) {
        rc = FlushFullSlot(FillSlot);
        if (rc < 0) {
            LastError = rc;
            return rc;
        }
    }

    /* Preserve the logical fileXioWrite contract expected by WriteExact(). The
     * close wrapper is the completion boundary that turns a deferred failure
     * into a visible error before verification/reopen can begin. */
    return size;
}

int __wrap_fileXioClose(int fd)
{
    int drain_rc = 0;
    int read_drain_rc = 0;
    int close_rc;

#if MCI_IMAGE_READ_AHEAD_ASYNC
    /* fileXio exposes one global NOWAIT completion state. A sequential read fd
     * must not be closed while its next refill still owns that state. */
    read_drain_rc = MciImageReadAheadDrain();
#endif

    if (EnableDepth != 0u) {
        drain_rc = DrainAsync();
        if (drain_rc < 0 && LastError == 0)
            LastError = drain_rc;

        if (fd == ActiveFd) {
            /* Successful full-card images are exact multiples of every allowed
             * batch size. Anything left here belongs to an aborted operation and
             * must not be pushed after the producer already reported failure. */
            ResetSlot(FillSlot);
            ActiveFd = -1;
            ActiveStride = 0;
        }
    }

    fileXioSetBlockMode(FXIO_WAIT);
    close_rc = __real_fileXioClose(fd);
    if (read_drain_rc < 0 && close_rc >= 0)
        return read_drain_rc;
    if (drain_rc < 0 && close_rc >= 0)
        return drain_rc;
    return close_rc;
}
