/* SPDX-License-Identifier: MIT */
/*
 * Sequential USB image read-ahead for Drebin verification/restore.
 *
 * card_image.c intentionally presents 512/528-byte logical-page reads. On real
 * PS2 USBHDFSD, issuing one fileXioRead RPC per page makes a 64 MiB verification
 * pass needlessly expensive. This wrapper batches thirty-two pages in one
 * underlying read while preserving exact page-sized semantics to callers.
 *
 * 32 is intentional rather than arbitrary:
 *   32 * 512 = 16384 bytes for VMC
 *   32 * 528 = 16896 bytes = 33 * 512-byte sectors for PCSX2 raw images
 * Thus both formats refill the cache on complete USB/FAT sector boundaries.
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

#define MCI_IMAGE_READ_AHEAD_MAX_STRIDE 528
#define MCI_IMAGE_READ_AHEAD_PAGES 32
#define MCI_IMAGE_READ_AHEAD_BYTES (MCI_IMAGE_READ_AHEAD_MAX_STRIDE * MCI_IMAGE_READ_AHEAD_PAGES)

static unsigned char Cache[MCI_IMAGE_READ_AHEAD_BYTES] __attribute__((aligned(64)));
static unsigned int EnableDepth;
static int CacheFd = -1;
static int CacheStride;
static int CacheOffset;
static int CacheLength;

/* Whole-operation counters. These deliberately measure the real user workload,
 * not a synthetic microbenchmark. Nested users share one ownership interval. */
static u64 OperationStart;
static u64 UnderlyingTicks;
static u64 LogicalBytes;
static u64 UnderlyingBytes;
static u32 LogicalReads;
static u32 UnderlyingReads;

int __real_fileXioRead(int fd, void *buffer, int size);

static void ResetCache(void)
{
    CacheFd = -1;
    CacheStride = 0;
    CacheOffset = 0;
    CacheLength = 0;
}

static void ResetStats(void)
{
    OperationStart = GetTimerSystemTime();
    UnderlyingTicks = 0u;
    LogicalBytes = 0u;
    UnderlyingBytes = 0u;
    LogicalReads = 0u;
    UnderlyingReads = 0u;
}

void MciImageReadAheadSetEnabled(int enabled)
{
    if (enabled) {
        if (EnableDepth++ == 0u) {
            ResetCache();
            ResetStats();
        }
        return;
    }

    if (EnableDepth == 0u)
        return;
    EnableDepth--;
    if (EnableDepth != 0u)
        return;

    MciDiagLogTracePrintf(
        "IMAGE-IO",
        "read-ahead end logical_calls=%u logical_bytes=%llu underlying_calls=%u underlying_bytes=%llu underlying_ticks=%llu operation_ticks=%llu batch_pages=%u",
        LogicalReads, (unsigned long long)LogicalBytes,
        UnderlyingReads, (unsigned long long)UnderlyingBytes,
        (unsigned long long)UnderlyingTicks,
        (unsigned long long)(GetTimerSystemTime() - OperationStart),
        (unsigned int)MCI_IMAGE_READ_AHEAD_PAGES);
    ResetCache();
}

int __wrap_fileXioRead(int fd, void *buffer, int size)
{
    int remaining;
    int rc;
    int copy_size;

    if (EnableDepth == 0u || buffer == NULL || (size != 512 && size != 528))
        return __real_fileXioRead(fd, buffer, size);

    LogicalReads++;
    LogicalBytes += (u64)(unsigned int)size;

    if (fd != CacheFd || size != CacheStride) {
        ResetCache();
        CacheFd = fd;
        CacheStride = size;
    }

    remaining = CacheLength - CacheOffset;
    if (remaining < size) {
        /* This move can overlap, so it intentionally remains memmove rather
         * than using the non-overlapping LQ/SQ stream primitive. */
        if (remaining > 0)
            memmove(Cache, Cache + CacheOffset, (unsigned int)remaining);
        CacheOffset = 0;
        CacheLength = remaining > 0 ? remaining : 0;

        while (CacheLength < size) {
            int capacity = (int)sizeof(Cache) - CacheLength;
            u64 begin;
            if (capacity <= 0)
                break;
            begin = GetTimerSystemTime();
            rc = __real_fileXioRead(fd, Cache + CacheLength, capacity);
            UnderlyingTicks += GetTimerSystemTime() - begin;
            UnderlyingReads++;
            if (rc <= 0) {
                if (CacheLength == 0)
                    return rc;
                break;
            }
            UnderlyingBytes += (u64)(unsigned int)rc;
            CacheLength += rc;
        }
    }

    remaining = CacheLength - CacheOffset;
    if (remaining <= 0)
        return 0;
    copy_size = remaining < size ? remaining : size;
    /* Cache is 64-byte aligned and the normal 512/528-byte destination buffers
     * are aligned by the image engine. MciFastCopy uses native R5900 LQ/SQ in
     * that common case and falls back to memcpy for any unusual caller. */
    MciFastCopy(buffer, Cache + CacheOffset, (unsigned int)copy_size);
    CacheOffset += copy_size;
    return copy_size;
}
