/* SPDX-License-Identifier: MIT */
#include <tamtypes.h>
#include <stdlib.h>
#include <string.h>

#include "save_archive.h"

void MciSaveArchiveInit(MciSaveArchive *archive)
{
    if (archive != NULL)
        memset(archive, 0, sizeof(*archive));
}

void MciSaveArchiveFree(MciSaveArchive *archive)
{
    if (archive == NULL)
        return;
    free(archive->storage);
    memset(archive, 0, sizeof(*archive));
}

const char *MciSaveCodecErrorText(int rc)
{
    switch (rc) {
        case MCI_SAVE_OK: return "ok";
        case MCI_SAVE_E_ARGS: return "bad arguments";
        case MCI_SAVE_E_FORMAT: return "not this container format";
        case MCI_SAVE_E_TRUNCATED: return "file is truncated";
        case MCI_SAVE_E_CORRUPT: return "container is corrupt";
        case MCI_SAVE_E_NAME: return "unsafe save or file name";
        case MCI_SAVE_E_LIMIT: return "save exceeds supported limits";
        case MCI_SAVE_E_NOMEM: return "out of memory";
        case MCI_SAVE_E_SUBDIR: return "save contains a subdirectory";
        case MCI_SAVE_E_DECOMPRESS: return "decompression failed";
        case MCI_SAVE_E_CHECKSUM: return "checksum mismatch";
        case MCI_SAVE_E_UNSUPPORTED: return "unsupported variant";
        case MCI_SAVE_E_FULL: return "not enough free blocks";
        case MCI_SAVE_E_EXISTS: return "a save with this name exists";
        default: return "unknown error";
    }
}

int MciSaveNameIsSafe(const char *name)
{
    unsigned int i;

    if (name == NULL || name[0] == '\0')
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (i = 0u; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];
        if (i >= MCI_SAVE_NAME_MAX)
            return 0;
        if (c < 0x20u || c >= 0x7Fu || c == '/' || c == '\\' ||
            c == '*' || c == '?' || c == ':')
            return 0;
    }
    return 1;
}

int MciSaveCopyName(char out[MCI_SAVE_NAME_MAX + 1u], const void *field,
                    unsigned int field_size)
{
    const unsigned char *p = (const unsigned char *)field;
    unsigned int i;

    if (out == NULL || field == NULL)
        return MCI_SAVE_E_ARGS;
    for (i = 0u; i < field_size && i < MCI_SAVE_NAME_MAX; i++) {
        if (p[i] == 0u)
            break;
        out[i] = (char)p[i];
    }
    /* A name that fills more than 32 bytes without NUL is not a PS2 name. */
    if (i == MCI_SAVE_NAME_MAX && i < field_size && p[i] != 0u)
        return MCI_SAVE_E_NAME;
    out[i] = '\0';
    return MciSaveNameIsSafe(out) ? MCI_SAVE_OK : MCI_SAVE_E_NAME;
}

int MciPs1NameIsSafe(const char *name)
{
    unsigned int i;

    if (name == NULL || name[0] == '\0')
        return 0;
    for (i = 0u; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];
        if (i >= MCI_PS1_SAVE_NAME_MAX)
            return 0;
        if (c < 0x20u || c >= 0x7Fu || c == '/' || c == '\\' ||
            c == '*' || c == '?')
            return 0;
    }
    return 1;
}

u32 MciSaveArchiveTotalBytes(const MciSaveArchive *archive)
{
    u32 total = 0u;
    u32 i;

    if (archive == NULL)
        return 0u;
    for (i = 0u; i < archive->file_count; i++)
        total += archive->files[i].size;
    return total;
}

u32 MciSaveArchiveRequiredClusters(const MciSaveArchive *archive)
{
    u32 clusters;
    u32 i;

    if (archive == NULL)
        return 0u;
    /* Directory: 512-byte entries, two per 1 KiB cluster, plus . and .. */
    clusters = (archive->file_count + 2u + 1u) / 2u;
    for (i = 0u; i < archive->file_count; i++)
        clusters += (archive->files[i].size + 1023u) / 1024u;
    return clusters;
}

u16 MciLe16(const unsigned char *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

u32 MciLe32(const unsigned char *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

void MciPutLe16(unsigned char *p, u16 v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)(v >> 8);
}

void MciPutLe32(unsigned char *p, u32 v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)(v >> 24);
}
