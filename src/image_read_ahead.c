/* SPDX-License-Identifier: MIT */
/*
 * Sequential USB image read-ahead for Drebin verification/restore.
 *
 * card_image.c intentionally uses tiny 512/528-byte logical-page reads. That is
 * simple, but on real PS2 USBHDFSD it turns a 64 MiB verification pass into
 * 131072 separate fileXioRead RPCs. This wrapper batches up to sixteen pages in
 * one underlying read while presenting the original exact-size read semantics
 * to the caller.
 *
 * The mode is explicitly enabled only around strictly sequential image readers.
 * It must remain disabled for the filesystem browser, which performs fileXioLseek
 * between cluster/page reads.
 */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <string.h>

#include "image_read_ahead.h"

#define MCI_IMAGE_READ_AHEAD_MAX_STRIDE 528
#define MCI_IMAGE_READ_AHEAD_PAGES 16
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
    memcpy(buffer, Cache + CacheOffset, (unsigned int)copy_size);
    CacheOffset += copy_size;
    return copy_size;
}
