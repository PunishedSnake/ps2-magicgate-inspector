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
#include <string.h>

#include "image_write_behind.h"

#define MCI_IMAGE_WRITE_MAX_STRIDE 528
#define MCI_IMAGE_WRITE_PAGES 32
#define MCI_IMAGE_WRITE_BYTES (MCI_IMAGE_WRITE_MAX_STRIDE * MCI_IMAGE_WRITE_PAGES)

static unsigned char Cache[MCI_IMAGE_WRITE_BYTES] __attribute__((aligned(64)));
static int Enabled;
static int CacheFd = -1;
static int CacheStride;
static int CacheLength;

int __real_fileXioWrite(int fd, const void *buffer, int size);

static void ResetCache(void)
{
    CacheFd = -1;
    CacheStride = 0;
    CacheLength = 0;
}

static int FlushCache(void)
{
    int done = 0;

    while (done < CacheLength) {
        int rc = __real_fileXioWrite(CacheFd, Cache + done,
                                     CacheLength - done);
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += rc;
    }
    CacheLength = 0;
    return 0;
}

void MciImageWriteBehindSetEnabled(int enabled)
{
    /* A successful image always ends on a 32-page boundary. If an export
     * aborted in the middle of a batch, its caller closes/removes the partial
     * image before disabling this wrapper, so never try to flush a stale fd. */
    Enabled = enabled ? 1 : 0;
    ResetCache();
}

int __wrap_fileXioWrite(int fd, const void *buffer, int size)
{
    int rc;

    if (!Enabled || buffer == NULL || (size != 512 && size != 528))
        return __real_fileXioWrite(fd, buffer, size);

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

    memcpy(Cache + CacheLength, buffer, (unsigned int)size);
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
