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
#include <string.h>

#include "image_read_ahead.h"
#include "r5900_memops.h"

#define MCI_IMAGE_READ_AHEAD_MAX_STRIDE 528
#define MCI_IMAGE_READ_AHEAD_PAGES 32
#define MCI_IMAGE_READ_AHEAD_BYTES (MCI_IMAGE_READ_AHEAD_MAX_STRIDE * MCI_IMAGE_READ_AHEAD_PAGES)

static unsigned char Cache[MCI_IMAGE_READ_AHEAD_BYTES] __attribute__((aligned(64)));
static int Enabled;
static int CacheFd = -1;
static int CacheStride;
static int CacheOffset;
static int CacheLength;

int __real_fileXioRead(int fd, void *buffer, int size);

static void ResetCache(void)
{
    CacheFd = -1;
    CacheStride = 0;
    CacheOffset = 0;
    CacheLength = 0;
}

void MciImageReadAheadSetEnabled(int enabled)
{
    Enabled = enabled ? 1 : 0;
    ResetCache();
}

int __wrap_fileXioRead(int fd, void *buffer, int size)
{
    int remaining;
    int rc;
    int copy_size;

    if (!Enabled || buffer == NULL || (size != 512 && size != 528))
        return __real_fileXioRead(fd, buffer, size);

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
            if (capacity <= 0)
                break;
            rc = __real_fileXioRead(fd, Cache + CacheLength, capacity);
            if (rc <= 0) {
                if (CacheLength == 0)
                    return rc;
                break;
            }
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
