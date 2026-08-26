/* SPDX-License-Identifier: MIT */
/*
 * Sequential USB image read-ahead for Drebin verification/restore.
 *
 * card_image.c intentionally presents 512/528-byte logical-page reads. On real
 * PS2 USBHDFSD, issuing one fileXioRead RPC per page makes a 64 MiB verification
 * pass needlessly expensive. This wrapper coalesces those reads while preserving
 * exact page-sized semantics to callers.
 *
 * Production keeps the hardware-used 32-page baseline. The backing size is
 * 32 * 528 = 16896 bytes, which is deliberately divisible by both supported
 * logical strides: 33 * 512-byte VMC records or 32 * 528-byte PCSX2 records.
 * P0 Performance Lab variants also build 16 and 64 pages. Larger is not assumed
 * faster: the EE has an 8 KiB D-cache and USB/BOT/fileXio have their own request
 * granularity, so real-hardware p50/p95/p99/max decides the useful trade-off.
 *
 * Optional async mode keeps exactly one next refill outstanding while the EE
 * consumes the current refill. This is a two-buffer ownership pipeline, not a
 * deeper USB command queue: Mass Storage BOT remains command-serialized and the
 * current PS2SDK fileXio client exposes one global async completion state.
 *
 * The mode is explicitly enabled only around strictly sequential image readers.
 * It must remain disabled for the filesystem browser, which performs fileXioLseek
 * between cluster/page reads.
 */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <timer.h>
#include <string.h>

#include "diag_log.h"
#include "image_read_ahead.h"
#include "r5900_memops.h"

#ifndef MCI_IMAGE_READ_AHEAD_PAGES
#define MCI_IMAGE_READ_AHEAD_PAGES 32
#endif

#ifndef MCI_IMAGE_READ_AHEAD_ASYNC
#define MCI_IMAGE_READ_AHEAD_ASYNC 0
#endif

#if MCI_IMAGE_READ_AHEAD_PAGES != 16 && \
    MCI_IMAGE_READ_AHEAD_PAGES != 32 && \
    MCI_IMAGE_READ_AHEAD_PAGES != 64
#error "MCI_IMAGE_READ_AHEAD_PAGES must be 16, 32 or 64"
#endif

/* The 16-page backing size is 8448 bytes. It is exact for 528-byte records but
 * splits a 512-byte VMC record, which makes ownership across two async buffers
 * needlessly ambiguous. Keep that useful synchronous batch candidate, but only
 * pipeline common sizes whose refill boundary is exact for both formats. */
#if MCI_IMAGE_READ_AHEAD_ASYNC && MCI_IMAGE_READ_AHEAD_PAGES == 16
#error "async image read-ahead requires 32 or 64 pages"
#endif

#if MCI_IMAGE_READ_AHEAD_ASYNC
#define MCI_IMAGE_READ_AHEAD_SLOTS 2
#else
#define MCI_IMAGE_READ_AHEAD_SLOTS 1
#endif

#define MCI_IMAGE_READ_AHEAD_MAX_STRIDE 528
#define MCI_IMAGE_READ_AHEAD_BYTES \
    (MCI_IMAGE_READ_AHEAD_MAX_STRIDE * MCI_IMAGE_READ_AHEAD_PAGES)
#define MCI_IO_LATENCY_BUCKETS 256u
#define MCI_IO_TICKS_PER_MS (kBUSCLK / 1000u)

typedef struct MciImageReadSlot {
    int fd;
    int stride;
    int offset;
    int length;
} MciImageReadSlot;

static unsigned char Cache[MCI_IMAGE_READ_AHEAD_SLOTS][MCI_IMAGE_READ_AHEAD_BYTES]
    __attribute__((aligned(64)));
static MciImageReadSlot Slots[MCI_IMAGE_READ_AHEAD_SLOTS];
static unsigned int EnableDepth;
static int ActiveFd = -1;
static int ActiveStride;
static int CurrentSlot = -1;
static int LastError;

/* Whole-operation counters. These deliberately measure the real user workload,
 * not a synthetic microbenchmark. Nested users share one ownership interval. */
static u64 OperationStart;
static u64 UnderlyingTicks;
static u64 LogicalBytes;
static u64 UnderlyingBytes;
static u64 MaxReadTicks;
static u64 AsyncWaitTicks;
static u32 LogicalReads;
static u32 UnderlyingReads;
static u32 AsyncSubmits;
static u32 AsyncReadyHits;
static u32 AsyncWaits;
static u32 AsyncFallbacks;
static u32 ReadLatencyHistogram[MCI_IO_LATENCY_BUCKETS];

#if MCI_IMAGE_READ_AHEAD_ASYNC
static int AsyncActive;
static int AsyncSlot = -1;
static int ReadySlot = -1;
static int PrefetchDisabled;
static u64 AsyncIssueTicks;
#endif

int __real_fileXioRead(int fd, void *buffer, int size);

static void ResetSlot(int index)
{
    if (index < 0 || index >= MCI_IMAGE_READ_AHEAD_SLOTS)
        return;
    Slots[index].fd = -1;
    Slots[index].stride = 0;
    Slots[index].offset = 0;
    Slots[index].length = 0;
}

static void ResetCacheState(void)
{
    int i;

    for (i = 0; i < MCI_IMAGE_READ_AHEAD_SLOTS; i++)
        ResetSlot(i);
    ActiveFd = -1;
    ActiveStride = 0;
    CurrentSlot = -1;
    LastError = 0;
#if MCI_IMAGE_READ_AHEAD_ASYNC
    AsyncActive = 0;
    AsyncSlot = -1;
    ReadySlot = -1;
    PrefetchDisabled = 0;
    AsyncIssueTicks = 0u;
#endif
}

static void ResetStats(void)
{
    OperationStart = GetTimerSystemTime();
    UnderlyingTicks = 0u;
    LogicalBytes = 0u;
    UnderlyingBytes = 0u;
    MaxReadTicks = 0u;
    AsyncWaitTicks = 0u;
    LogicalReads = 0u;
    UnderlyingReads = 0u;
    AsyncSubmits = 0u;
    AsyncReadyHits = 0u;
    AsyncWaits = 0u;
    AsyncFallbacks = 0u;
    memset(ReadLatencyHistogram, 0, sizeof(ReadLatencyHistogram));
}

static void RecordReadLatency(u64 ticks)
{
    u64 bucket64 = ticks / (u64)MCI_IO_TICKS_PER_MS;
    unsigned int bucket = bucket64 >= MCI_IO_LATENCY_BUCKETS
                              ? MCI_IO_LATENCY_BUCKETS - 1u
                              : (unsigned int)bucket64;
    ReadLatencyHistogram[bucket]++;
    if (ticks > MaxReadTicks)
        MaxReadTicks = ticks;
}

static unsigned int ReadPercentileMsFloor(unsigned int percentile)
{
    unsigned int target;
    unsigned int seen = 0u;
    unsigned int i;

    if (UnderlyingReads == 0u)
        return 0u;
    target = (UnderlyingReads * percentile + 99u) / 100u;
    for (i = 0u; i < MCI_IO_LATENCY_BUCKETS; i++) {
        seen += ReadLatencyHistogram[i];
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

static int TimedRead(int fd, unsigned char *buffer, int size)
{
    int rc;
    u64 begin;
    u64 elapsed;

    fileXioSetBlockMode(FXIO_WAIT);
    begin = GetTimerSystemTime();
    rc = __real_fileXioRead(fd, buffer, size);
    elapsed = GetTimerSystemTime() - begin;
    UnderlyingTicks += elapsed;
    UnderlyingReads++;
    RecordReadLatency(elapsed);
    if (rc > 0)
        UnderlyingBytes += (u64)(unsigned int)rc;
    return rc;
}

#if MCI_IMAGE_READ_AHEAD_ASYNC
static int FillSyncAtLeast(int index, int fd, int stride, int minimum)
{
    MciImageReadSlot *slot = &Slots[index];

    ResetSlot(index);
    slot->fd = fd;
    slot->stride = stride;
    while (slot->length < minimum) {
        int capacity = (int)sizeof(Cache[index]) - slot->length;
        int rc;

        if (capacity <= 0)
            break;
        rc = TimedRead(fd, Cache[index] + slot->length, capacity);
        if (rc <= 0)
            return slot->length > 0 ? slot->length : rc;
        slot->length += rc;
    }
    return slot->length;
}

static int FinishAsync(void)
{
    int poll_rc;
    int result = -999;
    int completed_slot;
    u64 now;
    u64 service_ticks;

    if (!AsyncActive)
        return 0;

    completed_slot = AsyncSlot;
    poll_rc = fileXioWaitAsync(FXIO_NOWAIT, &result);
    if (poll_rc < 0) {
        now = GetTimerSystemTime();
        service_ticks = now - AsyncIssueTicks;
        UnderlyingTicks += service_ticks;
        RecordReadLatency(service_ticks);
        fileXioSetBlockMode(FXIO_WAIT);
        AsyncActive = 0;
        AsyncSlot = -1;
        ResetSlot(completed_slot);
        LastError = poll_rc;
        return poll_rc;
    }

    if (poll_rc == FXIO_INCOMPLETE) {
        u64 wait_begin = GetTimerSystemTime();
        AsyncWaits++;
        poll_rc = fileXioWaitAsync(FXIO_WAIT, &result);
        now = GetTimerSystemTime();
        AsyncWaitTicks += now - wait_begin;
        if (poll_rc < 0) {
            service_ticks = now - AsyncIssueTicks;
            UnderlyingTicks += service_ticks;
            RecordReadLatency(service_ticks);
            fileXioSetBlockMode(FXIO_WAIT);
            AsyncActive = 0;
            AsyncSlot = -1;
            ResetSlot(completed_slot);
            LastError = poll_rc;
            return poll_rc;
        }
        if (poll_rc != FXIO_COMPLETE) {
            fileXioSetBlockMode(FXIO_WAIT);
            AsyncActive = 0;
            AsyncSlot = -1;
            ResetSlot(completed_slot);
            LastError = -2;
            return -2;
        }
    } else if (poll_rc == FXIO_COMPLETE) {
        AsyncReadyHits++;
        now = GetTimerSystemTime();
    } else {
        fileXioSetBlockMode(FXIO_WAIT);
        AsyncActive = 0;
        AsyncSlot = -1;
        ResetSlot(completed_slot);
        LastError = -3;
        return -3;
    }

    service_ticks = now - AsyncIssueTicks;
    UnderlyingTicks += service_ticks;
    RecordReadLatency(service_ticks);
    fileXioSetBlockMode(FXIO_WAIT);
    AsyncActive = 0;
    AsyncSlot = -1;

    if (result < 0 || result > (int)sizeof(Cache[completed_slot])) {
        ResetSlot(completed_slot);
        LastError = result < 0 ? result : -4;
        return LastError;
    }

    Slots[completed_slot].offset = 0;
    Slots[completed_slot].length = result;
    if (result > 0)
        UnderlyingBytes += (u64)(unsigned int)result;
    ReadySlot = completed_slot;
    return result;
}

static int SubmitAsync(int index, int fd, int stride)
{
    int rc;

    if (PrefetchDisabled || AsyncActive || ReadySlot >= 0)
        return 0;

    ResetSlot(index);
    Slots[index].fd = fd;
    Slots[index].stride = stride;
    fileXioSetBlockMode(FXIO_NOWAIT);
    AsyncIssueTicks = GetTimerSystemTime();
    rc = __real_fileXioRead(fd, Cache[index], (int)sizeof(Cache[index]));
    UnderlyingReads++;
    AsyncSubmits++;

    if (rc < 0) {
        u64 elapsed = GetTimerSystemTime() - AsyncIssueTicks;
        UnderlyingTicks += elapsed;
        RecordReadLatency(elapsed);
        fileXioSetBlockMode(FXIO_WAIT);
        AsyncFallbacks++;
        PrefetchDisabled = 1;
        LastError = rc;
        ResetSlot(index);
        return 0;
    }

    /* CURRENT IMPLEMENTATION: PS2SDK fileXio returns zero when a NOWAIT RPC is
     * accepted. Keep a defensive positive-result path so a future synchronous
     * fallback cannot silently lose bytes if that contract changes. */
    if (rc > 0) {
        u64 elapsed = GetTimerSystemTime() - AsyncIssueTicks;
        UnderlyingTicks += elapsed;
        UnderlyingBytes += (u64)(unsigned int)rc;
        RecordReadLatency(elapsed);
        fileXioSetBlockMode(FXIO_WAIT);
        Slots[index].length = rc;
        ReadySlot = index;
        AsyncReadyHits++;
        return 0;
    }

    AsyncActive = 1;
    AsyncSlot = index;
    return 0;
}
#endif

int MciImageReadAheadDrain(void)
{
#if MCI_IMAGE_READ_AHEAD_ASYNC
    int rc = FinishAsync();
    fileXioSetBlockMode(FXIO_WAIT);
    return rc < 0 ? rc : 0;
#else
    return 0;
#endif
}

void MciImageReadAheadSetEnabled(int enabled)
{
    int teardown_rc = 0;

    if (enabled) {
        if (EnableDepth++ == 0u) {
            ResetCacheState();
            ResetStats();
        }
        return;
    }

    if (EnableDepth == 0u)
        return;
    EnableDepth--;
    if (EnableDepth != 0u)
        return;

    teardown_rc = MciImageReadAheadDrain();
    if (teardown_rc < 0 && LastError == 0)
        LastError = teardown_rc;

    MciDiagLogTracePrintf(
        "IMAGE-IO",
        "read-ahead end logical_calls=%u logical_bytes=%llu underlying_calls=%u underlying_bytes=%llu underlying_ticks=%llu operation_ticks=%llu batch_pages=%u async=%d async_submits=%u async_ready=%u async_waits=%u async_wait_ticks=%llu async_fallbacks=%u last_error=%d batch_p50_ms_floor=%u batch_p95_ms_floor=%u batch_p99_ms_floor=%u batch_max_us=%llu",
        LogicalReads, (unsigned long long)LogicalBytes,
        UnderlyingReads, (unsigned long long)UnderlyingBytes,
        (unsigned long long)UnderlyingTicks,
        (unsigned long long)(GetTimerSystemTime() - OperationStart),
        (unsigned int)MCI_IMAGE_READ_AHEAD_PAGES,
        MCI_IMAGE_READ_AHEAD_ASYNC ? 1 : 0,
        AsyncSubmits, AsyncReadyHits, AsyncWaits,
        (unsigned long long)AsyncWaitTicks, AsyncFallbacks, LastError,
        ReadPercentileMsFloor(50u), ReadPercentileMsFloor(95u),
        ReadPercentileMsFloor(99u),
        (unsigned long long)TicksToUsec(MaxReadTicks));
    ResetCacheState();
}

int __wrap_fileXioRead(int fd, void *buffer, int size)
{
    int rc;

    if (EnableDepth == 0u)
        return __real_fileXioRead(fd, buffer, size);

    if (buffer == NULL || (size != 512 && size != 528)) {
        rc = MciImageReadAheadDrain();
        if (rc < 0)
            return rc;
        fileXioSetBlockMode(FXIO_WAIT);
        return __real_fileXioRead(fd, buffer, size);
    }

    LogicalReads++;
    LogicalBytes += (u64)(unsigned int)size;

#if !MCI_IMAGE_READ_AHEAD_ASYNC
    if (fd != ActiveFd || size != ActiveStride) {
        ResetCacheState();
        ActiveFd = fd;
        ActiveStride = size;
        CurrentSlot = 0;
    }

    if (Slots[0].length - Slots[0].offset < size) {
        int remaining = Slots[0].length - Slots[0].offset;

        if (remaining > 0)
            memmove(Cache[0], Cache[0] + Slots[0].offset,
                    (unsigned int)remaining);
        Slots[0].offset = 0;
        Slots[0].length = remaining > 0 ? remaining : 0;
        Slots[0].fd = fd;
        Slots[0].stride = size;

        while (Slots[0].length < size) {
            int capacity = (int)sizeof(Cache[0]) - Slots[0].length;
            if (capacity <= 0)
                break;
            rc = TimedRead(fd, Cache[0] + Slots[0].length, capacity);
            if (rc <= 0) {
                if (Slots[0].length == 0)
                    return rc;
                break;
            }
            Slots[0].length += rc;
        }
    }

    rc = Slots[0].length - Slots[0].offset;
    if (rc <= 0)
        return 0;
    if (rc > size)
        rc = size;
    MciFastCopy(buffer, Cache[0] + Slots[0].offset, (unsigned int)rc);
    Slots[0].offset += rc;
    return rc;
#else
    unsigned char *out = (unsigned char *)buffer;

    if (fd != ActiveFd || size != ActiveStride) {
        rc = MciImageReadAheadDrain();
        if (rc < 0)
            return rc;
        ResetCacheState();
        ActiveFd = fd;
        ActiveStride = size;
    }

    if (CurrentSlot < 0) {
        rc = FillSyncAtLeast(0, fd, size, size);
        if (rc <= 0)
            return rc;
        CurrentSlot = 0;
        (void)SubmitAsync(1, fd, size);
    }

    for (;;) {
        MciImageReadSlot *current = &Slots[CurrentSlot];
        int remaining = current->length - current->offset;

        if (remaining >= size) {
            MciFastCopy(out, Cache[CurrentSlot] + current->offset,
                        (unsigned int)size);
            current->offset += size;
            return size;
        }

        if (remaining == 0) {
            int old_slot = CurrentSlot;
            int next_slot;

            if (AsyncActive) {
                rc = FinishAsync();
                if (rc < 0)
                    return rc;
            }

            if (ReadySlot >= 0) {
                next_slot = ReadySlot;
                ReadySlot = -1;
                CurrentSlot = next_slot;
                ResetSlot(old_slot);
                if (Slots[next_slot].length == 0)
                    return 0;
                (void)SubmitAsync(old_slot, fd, size);
                continue;
            }

            /* Immediate NOWAIT submission failure disables speculation for this
             * fd. The dependency point retries synchronously, preserving the
             * production error semantics instead of turning an optimization
             * failure into data loss. */
            rc = FillSyncAtLeast(old_slot, fd, size, size);
            if (rc <= 0)
                return rc;
            CurrentSlot = old_slot;
            if (!PrefetchDisabled)
                (void)SubmitAsync(old_slot ^ 1, fd, size);
            continue;
        }

        /* Rare short-read boundary. Normal 32/64-page refills are exact for
         * both image strides, so this branch is not on the steady-state path.
         * Preserve the logical one-record contract by joining the current tail
         * with the already-issued next refill. */
        {
            int total = 0;
            int old_slot = CurrentSlot;
            int next_slot;
            int need;
            int available;
            int take;

            MciFastCopy(out, Cache[old_slot] + Slots[old_slot].offset,
                        (unsigned int)remaining);
            Slots[old_slot].offset += remaining;
            total = remaining;

            if (AsyncActive) {
                rc = FinishAsync();
                /* Completion failure leaves the IOP file-position semantics
                 * unspecified for retry. Fail the logical record immediately
                 * instead of returning a partial record to ReadExact(). */
                if (rc < 0)
                    return rc;
            }

            if (ReadySlot < 0) {
                while (total < size) {
                    rc = TimedRead(fd, out + total, size - total);
                    if (rc <= 0)
                        return total > 0 ? total : rc;
                    total += rc;
                }
                ResetSlot(old_slot);
                CurrentSlot = -1;
                return total;
            }

            next_slot = ReadySlot;
            ReadySlot = -1;
            need = size - total;
            available = Slots[next_slot].length - Slots[next_slot].offset;
            take = available < need ? available : need;
            if (take > 0) {
                MciFastCopy(out + total,
                            Cache[next_slot] + Slots[next_slot].offset,
                            (unsigned int)take);
                Slots[next_slot].offset += take;
                total += take;
            }

            ResetSlot(old_slot);
            CurrentSlot = next_slot;

            if (total < size) {
                if (Slots[next_slot].length == 0)
                    return total;
                while (total < size) {
                    rc = TimedRead(fd, out + total, size - total);
                    if (rc <= 0)
                        return total > 0 ? total : rc;
                    total += rc;
                }
                if (Slots[next_slot].offset >= Slots[next_slot].length) {
                    ResetSlot(next_slot);
                    CurrentSlot = -1;
                }
                return total;
            }

            (void)SubmitAsync(old_slot, fd, size);
            return total;
        }
    }
#endif
}
