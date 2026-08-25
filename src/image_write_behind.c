/* SPDX-License-Identifier: MIT */
/*
 * Image-only USB write-behind for Drebin.
 *
 * A PCSX2 memory-card record is 528 bytes. Writing one record per fileXio RPC
 * repeatedly crosses 512-byte USB/FAT sector boundaries at awkward offsets.
 * Thirty-two records are exactly 16896 bytes, i.e. 33 complete 512-byte
 * sectors. The same batch size gives 16384 bytes for VMC pages. Real-hardware
 * testing already showed USBHDFSD to be sensitive to long-lived small-file I/O,
 * so use the smallest batch that is sector-aligned for both image formats.
 */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <timer.h>
#include <string.h>

#include "diag_log.h"
#include "image_write_behind.h"
#include "r5900_memops.h"

#define MCI_IMAGE_WRITE_MAX_STRIDE 528
#define MCI_IMAGE_WRITE_PAGES 32
#define MCI_IMAGE_WRITE_BYTES (MCI_IMAGE_WRITE_MAX_STRIDE * MCI_IMAGE_WRITE_PAGES)

static unsigned char Cache[MCI_IMAGE_WRITE_BYTES] __attribute__((aligned(64)));
static unsigned int EnableDepth;
static int CacheFd = -1;
static int CacheStride;
static int CacheLength;

static u64 OperationStart;
static u64 UnderlyingTicks;
static u64 LogicalBytes;
static u64 UnderlyingBytes;
static u32 LogicalWrites;
static u32 UnderlyingWrites;

int __real_fileXioWrite(int fd, const void *buffer, int size);

static void ResetCache(void)
{
    CacheFd = -1;
    CacheStride = 0;
    CacheLength = 0;
}

static void ResetStats(void)
{
    OperationStart = GetTimerSystemTime();
    UnderlyingTicks = 0u;
    LogicalBytes = 0u;
    UnderlyingBytes = 0u;
    LogicalWrites = 0u;
    UnderlyingWrites = 0u;
}

static int FlushCache(void)
{
    int done = 0;

    while (done < CacheLength) {
        int rc;
        u64 begin = GetTimerSystemTime();
        rc = __real_fileXioWrite(CacheFd, Cache + done,
                                 CacheLength - done);
        UnderlyingTicks += GetTimerSystemTime() - begin;
        UnderlyingWrites++;
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        UnderlyingBytes += (u64)(unsigned int)rc;
        done += rc;
    }
    CacheLength = 0;
    return 0;
}

void MciImageWriteBehindSetEnabled(int enabled)
{
    /* Nested wrappers share ownership. Only the outermost operation resets or
     * publishes counters, matching the logger/read-ahead critical-section model. */
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

    /* A successful image ends on a 32-page boundary. If an export aborted in
     * the middle of a batch, its caller has already closed/removed the partial
     * image, so intentionally discard stale cached bytes rather than touching a
     * dead descriptor during teardown. */
    MciDiagLogTracePrintf(
        "IMAGE-IO",
        "write-behind end logical_calls=%u logical_bytes=%llu underlying_calls=%u underlying_bytes=%llu underlying_ticks=%llu operation_ticks=%llu batch_pages=%u pending_bytes=%d",
        LogicalWrites, (unsigned long long)LogicalBytes,
        UnderlyingWrites, (unsigned long long)UnderlyingBytes,
        (unsigned long long)UnderlyingTicks,
        (unsigned long long)(GetTimerSystemTime() - OperationStart),
        (unsigned int)MCI_IMAGE_WRITE_PAGES, CacheLength);
    ResetCache();
}

int __wrap_fileXioWrite(int fd, const void *buffer, int size)
{
    int rc;

    if (EnableDepth == 0u || buffer == NULL || (size != 512 && size != 528))
        return __real_fileXioWrite(fd, buffer, size);

    LogicalWrites++;
    LogicalBytes += (u64)(unsigned int)size;

    if (CacheLength != 0 && (fd != CacheFd || size != CacheStride)) {
        rc = FlushCache();
        if (rc < 0) {
            ResetCache();
            return rc;
        }
    }
    if (CacheLength == 0) {
        CacheFd = fd;
        CacheStride = size;
    }

    if (CacheLength + size > (int)sizeof(Cache)) {
        rc = FlushCache();
        if (rc < 0) {
            ResetCache();
            return rc;
        }
        CacheFd = fd;
        CacheStride = size;
    }

    /* Both supported record sizes are multiples of 16 and the image engine's
     * page buffers are 64-byte aligned. Use LQ/SQ for that normal path while
     * preserving a memcpy fallback for any future caller with weaker alignment. */
    MciFastCopy(Cache + CacheLength, buffer, (unsigned int)size);
    CacheLength += size;

    if (CacheLength == CacheStride * MCI_IMAGE_WRITE_PAGES) {
        rc = FlushCache();
        if (rc < 0) {
            ResetCache();
            return rc;
        }
    }

    /* Preserve the exact fileXioWrite contract expected by WriteExact(). */
    return size;
}
