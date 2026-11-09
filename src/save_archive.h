/* SPDX-License-Identifier: MIT */
#ifndef MCI_SAVE_ARCHIVE_H
#define MCI_SAVE_ARCHIVE_H

/*
 * In-memory single-save model shared by every container decoder.
 *
 * A PS2 archive is one top-level directory with flat files (memory-card saves
 * never contain subdirectories; every supported container rejects them).
 * A PS1 archive is exactly one file: the 20-character PS1 save name and the
 * save body (n * 8192 bytes, first frame "SC").
 *
 * Portable: no PS2SDK dependency beyond tamtypes.h. Decoders validate fully
 * before returning success; callers never see a half-decoded archive.
 */

#include <tamtypes.h>

#include "mc_fs_format.h"

#define MCI_SAVE_NAME_MAX 32u            /* PS2 entry names, excl. NUL */
#define MCI_SAVE_MAX_FILES 128u          /* PS2 browser limit is far lower */
#define MCI_SAVE_MAX_BYTES (16u * 1024u * 1024u)
#define MCI_PS1_BLOCK_BYTES 8192u
#define MCI_PS1_SAVE_NAME_MAX 20u
#define MCI_PS1_MAX_BLOCKS 15u

typedef enum MciSaveGeneration {
    MCI_SAVE_GEN_UNKNOWN = 0,
    MCI_SAVE_GEN_PS2 = 2,
    MCI_SAVE_GEN_PS1 = 1
} MciSaveGeneration;

typedef enum MciSaveCodecError {
    MCI_SAVE_OK = 0,
    MCI_SAVE_E_ARGS = -1,
    MCI_SAVE_E_FORMAT = -2,      /* not this container / bad magic */
    MCI_SAVE_E_TRUNCATED = -3,
    MCI_SAVE_E_CORRUPT = -4,     /* structurally inconsistent */
    MCI_SAVE_E_NAME = -5,        /* unsafe or empty name */
    MCI_SAVE_E_LIMIT = -6,       /* exceeds a hard bound */
    MCI_SAVE_E_NOMEM = -7,
    MCI_SAVE_E_SUBDIR = -8,      /* container holds a directory */
    MCI_SAVE_E_DECOMPRESS = -9,
    MCI_SAVE_E_CHECKSUM = -10,
    MCI_SAVE_E_UNSUPPORTED = -11,
    MCI_SAVE_E_FULL = -12,       /* PS1 card image has no room */
    MCI_SAVE_E_EXISTS = -13      /* PS1 card image already has the name */
} MciSaveCodecError;

typedef struct MciSaveFile {
    char name[MCI_SAVE_NAME_MAX + 1u];
    u16 mode;
    MciMcDateTime created;
    MciMcDateTime modified;
    u32 size;
    const unsigned char *data;   /* points into MciSaveArchive.storage */
} MciSaveFile;

typedef struct MciSaveArchive {
    MciSaveGeneration generation;
    char directory[MCI_SAVE_NAME_MAX + 1u]; /* PS2 dir / PS1 save name */
    u16 dir_mode;
    MciMcDateTime created;
    MciMcDateTime modified;
    u32 file_count;
    MciSaveFile files[MCI_SAVE_MAX_FILES];
    unsigned char *storage;      /* single owned allocation */
    u32 storage_size;
    char title[96];              /* optional, from container metadata */
} MciSaveArchive;

void MciSaveArchiveInit(MciSaveArchive *archive);
void MciSaveArchiveFree(MciSaveArchive *archive);
const char *MciSaveCodecErrorText(int rc);

/* PS2 memory-card entry name rules (also used for the directory). */
int MciSaveNameIsSafe(const char *name);
/* Copies a fixed-width, possibly unterminated field and validates it. */
int MciSaveCopyName(char out[MCI_SAVE_NAME_MAX + 1u], const void *field,
                    unsigned int field_size);
/* PS1 directory-frame save names: printable ASCII, 1..20 chars. */
int MciPs1NameIsSafe(const char *name);

/* Total bytes and clusters the PS2 card would need (1 KiB clusters, one
 * cluster per two directory entries including "." and ".."). */
u32 MciSaveArchiveTotalBytes(const MciSaveArchive *archive);
u32 MciSaveArchiveRequiredClusters(const MciSaveArchive *archive);

/* Little-endian helpers shared by the codecs. */
u16 MciLe16(const unsigned char *p);
u32 MciLe32(const unsigned char *p);
void MciPutLe16(unsigned char *p, u16 v);
void MciPutLe32(unsigned char *p, u32 v);

#endif /* MCI_SAVE_ARCHIVE_H */
