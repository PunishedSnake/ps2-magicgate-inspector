/* SPDX-License-Identifier: MIT */
/* Persistent immutable FMCB.XLF cache for repeated MagicGate transactions. */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "kelf_cache.h"
#include "usb_search.h"

#define KELF_CACHE_PAD 0x400u
#define KELF_CACHE_MAX (4u * 1024u * 1024u)
#define KELF_CACHE_CHUNK 4096u

static unsigned char *CachedData;
static unsigned int CachedSize;
static char CachedPath[MCI_USB_SEARCH_PATH_MAX];

void MciKelfCacheInvalidate(void)
{
    if (CachedData != NULL)
        free(CachedData);
    CachedData = NULL;
    CachedSize = 0u;
    CachedPath[0] = '\0';
}

int MciKelfCacheGetSource(char *path, unsigned int path_size,
                          unsigned int *size)
{
    if (CachedData == NULL || CachedSize == 0u || CachedPath[0] == '\0')
        return -1;
    if (path == NULL || path_size == 0u || strlen(CachedPath) >= path_size)
        return -2;
    snprintf(path, path_size, "%s", CachedPath);
    if (size != NULL)
        *size = CachedSize;
    return 0;
}

static int ReadWholeFile(const char *path, unsigned int size,
                         unsigned char **out)
{
    unsigned char *buffer;
    unsigned int total = 0u;
    int fd;
    int rc;

    buffer = memalign(64, size + KELF_CACHE_PAD);
    if (buffer == NULL)
        return -12;
    memset(buffer, 0, size + KELF_CACHE_PAD);

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0) {
        free(buffer);
        return fd;
    }
    while (total < size) {
        unsigned int chunk = size - total;
        if (chunk > KELF_CACHE_CHUNK)
            chunk = KELF_CACHE_CHUNK;
        rc = fileXioRead(fd, buffer + total, (int)chunk);
        if (rc <= 0 || (unsigned int)rc > chunk) {
            fileXioClose(fd);
            free(buffer);
            return rc < 0 ? rc : -2101;
        }
        total += (unsigned int)rc;
    }
    fileXioClose(fd);
    *out = buffer;
    return 0;
}

int MciKelfCacheClone(const char *path, unsigned int expected_size,
                      unsigned char **out_data, unsigned int *out_size,
                      int *cache_hit)
{
    iox_stat_t stat;
    unsigned char *clone;
    unsigned char *fresh = NULL;
    unsigned int size;
    int hit = 0;
    int rc;

    if (out_data == NULL || path == NULL || path[0] == '\0')
        return -1;
    *out_data = NULL;
    if (out_size != NULL)
        *out_size = 0u;
    if (cache_hit != NULL)
        *cache_hit = 0;

    if (CachedData != NULL && strcmp(CachedPath, path) == 0 &&
        (expected_size == 0u || CachedSize == expected_size)) {
        size = CachedSize;
        hit = 1;
    } else {
        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(path, &stat);
        if (rc < 0)
            return rc;
        if (!FIO_S_ISREG(stat.mode) || stat.size == 0u ||
            stat.size > KELF_CACHE_MAX ||
            (expected_size != 0u && stat.size != expected_size))
            return -2102;
        size = stat.size;
        rc = ReadWholeFile(path, size, &fresh);
        if (rc < 0)
            return rc;

        MciKelfCacheInvalidate();
        CachedData = fresh;
        CachedSize = size;
        snprintf(CachedPath, sizeof(CachedPath), "%s", path);
    }

    clone = memalign(64, size + KELF_CACHE_PAD);
    if (clone == NULL)
        return -12;
    memset(clone, 0, size + KELF_CACHE_PAD);
    memcpy(clone, CachedData, size);

    *out_data = clone;
    if (out_size != NULL)
        *out_size = size;
    if (cache_hit != NULL)
        *cache_hit = hit;
    return 0;
}
