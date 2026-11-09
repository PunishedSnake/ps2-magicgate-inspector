/* SPDX-License-Identifier: MIT */
#ifndef MCI_SAVE_CODEC_PS2_H
#define MCI_SAVE_CODEC_PS2_H

/*
 * PS2 single-save container decoders (in memory) and PSU encoder.
 *
 *   PSU  EMS / uLaunchELF            512-byte entries, 1 KiB padded data
 *   MAX  Action Replay MAX / Drive   "Ps2PowerSave", CRC32, LZARI
 *   CBS  CodeBreaker                 "CFU\0", RC4 (fixed state) + zlib
 *   SPS  SharkPort / X-Port (XPS)    "\x0d\0\0\0SharkPortSave", 98-byte heads
 *   PSV  PS3 export of a PS2 save    "\0VSP", type 2 (import only; signed)
 *
 * Layouts: mymc ps2save.py (Ross Ridge, public domain) and psdevwiki/
 * mymc++ format_psv for PSV. Every decoder bounds-checks against the input
 * size, rejects subdirectories and unsafe names, and caps totals at
 * MCI_SAVE_MAX_BYTES / MCI_SAVE_MAX_FILES.
 */

#include <tamtypes.h>

#include "save_archive.h"

typedef enum MciPs2Container {
    MCI_PS2_CONTAINER_NONE = 0,
    MCI_PS2_CONTAINER_PSU,
    MCI_PS2_CONTAINER_MAX,
    MCI_PS2_CONTAINER_CBS,
    MCI_PS2_CONTAINER_SPS,
    MCI_PS2_CONTAINER_PSV
} MciPs2Container;

typedef struct MciPs2DecodeInfo {
    MciPs2Container kind;
    int checksum_present;   /* MAX CRC32 */
    int checksum_ok;
    int timestamps_present; /* MAX has none: importer substitutes "now" */
} MciPs2DecodeInfo;

#define MCI_PS2_DEFAULT_DIR_MODE 0x8427u
#define MCI_PS2_DEFAULT_FILE_MODE 0x8497u

MciPs2Container MciPs2DetectContainer(const unsigned char *data, u32 size);
const char *MciPs2ContainerName(MciPs2Container kind);
int MciPs2Decode(const unsigned char *data, u32 size, MciSaveArchive *archive,
                 MciPs2DecodeInfo *info);

/* PSU encoder for USB-side conversion. Returns bytes written or negative. */
u32 MciPs2PsuSize(const MciSaveArchive *archive);
int MciPs2EncodePsu(const MciSaveArchive *archive, unsigned char *out,
                    u32 out_size);

#endif /* MCI_SAVE_CODEC_PS2_H */
