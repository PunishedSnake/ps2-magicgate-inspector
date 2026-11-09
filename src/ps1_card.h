/* SPDX-License-Identifier: MIT */
#ifndef MCI_PS1_CARD_H
#define MCI_PS1_CARD_H

/*
 * PlayStation (PS1) memory-card filesystem and container formats.
 *
 * A PS1 card is 128 KiB = 16 blocks of 8 KiB = 1024 frames of 128 bytes.
 * Block 0 holds the header frame ("MC"), 15 directory frames, the broken
 * sector list (frames 16..35), replacement frames (36..55) and a copy of the
 * header at frame 63. Blocks 1..15 hold save data. Each directory frame:
 *
 *   0x00 u32 state   0xA0 free, 0x51 first, 0x52 middle, 0x53 last,
 *                    0xA1..0xA3 deleted first/middle/last
 *   0x04 u32 size    total bytes of the save (first block only)
 *   0x08 u16 next    next directory slot 0..14, 0xFFFF = end
 *   0x0A char[21]    save name (country + product code + identifier)
 *   0x7F u8  xor     XOR of bytes 0x00..0x7E
 *
 * Whole-card containers (all wrap the same 131072-byte image):
 *   raw       .mcr .mcd .mc .bin .ddf .ps .psm .mci .vmc (POPS)  -- "MC" @0
 *   DexDrive  .gme (.dex)  3904-byte header "123-456-STD", comments
 *   VGS       .mem .vgs    64-byte header "VgsM"
 *   PSP       .vmp         128-byte header "\0PMV", HMAC-SHA1 signed
 *   MCX       .mcx         AES-128-CBC encrypted card + SHA-256 (0x200A0)
 *
 * Single-save containers decode into a one-file MciSaveArchive:
 *   MCS       .mcs .ps1        128-byte directory frame + save body
 *   AR/GS/XP  .psx .mcb .mcx .pda .ps1   54-byte header + save body
 *   raw       (no header)      save body only, "SC" @0; name from filename
 *   PSV       .psv             PS3 export, type 1, HMAC-SHA1 signed
 *
 * Formats from: Martin Korth "Nocash PSX Specs" memory-card chapter; DexDrive
 * GME and VGS header layouts as documented by MemcardRex (facts only, no code
 * copied); PS3 PSV layout per psdevwiki / mymc++ format_psv.
 */

#include <tamtypes.h>

#include "save_archive.h"

#define MCI_PS1_CARD_BYTES 131072u
#define MCI_PS1_FRAME_BYTES 128u
#define MCI_PS1_FRAMES 1024u
#define MCI_PS1_SLOTS 15u

#define MCI_PS1_STATE_FREE 0xA0u
#define MCI_PS1_STATE_FIRST 0x51u
#define MCI_PS1_STATE_MIDDLE 0x52u
#define MCI_PS1_STATE_LAST 0x53u
#define MCI_PS1_STATE_DEL_FIRST 0xA1u
#define MCI_PS1_STATE_DEL_MIDDLE 0xA2u
#define MCI_PS1_STATE_DEL_LAST 0xA3u
#define MCI_PS1_LINK_END 0xFFFFu

#define MCI_GME_HEADER_BYTES 3904u
#define MCI_VGS_HEADER_BYTES 64u
#define MCI_VMP_HEADER_BYTES 128u

typedef enum MciPs1Container {
    MCI_PS1_CONTAINER_NONE = 0,
    MCI_PS1_CONTAINER_RAW,
    MCI_PS1_CONTAINER_GME,
    MCI_PS1_CONTAINER_VGS,
    MCI_PS1_CONTAINER_VMP,
    MCI_PS1_CONTAINER_MCX
} MciPs1Container;

typedef struct MciPs1Card {
    unsigned char image[MCI_PS1_CARD_BYTES];
} MciPs1Card;

typedef struct MciPs1SaveInfo {
    unsigned int first_slot;
    unsigned int block_count;
    unsigned int slots[MCI_PS1_SLOTS];
    u32 size;                       /* from the first directory frame */
    char name[MCI_PS1_SAVE_NAME_MAX + 1u];
    char title[64];                 /* ASCII fold of the SC title, may be "" */
} MciPs1SaveInfo;

typedef struct MciPs1CardSummary {
    int header_ok;
    unsigned int bad_xor_frames;    /* directory frames with wrong checksum */
    unsigned int broken_chains;     /* orphaned or looping link chains */
    unsigned int free_slots;
    unsigned int save_count;
    MciPs1SaveInfo saves[MCI_PS1_SLOTS];
} MciPs1CardSummary;

/* Detect a whole-card container; *offset receives the image offset. */
MciPs1Container MciPs1DetectCardContainer(const unsigned char *data, u32 size,
                                          u32 *offset);
const char *MciPs1ContainerName(MciPs1Container kind);

/* Load from any whole-card container (copies 128 KiB into card). */
int MciPs1CardLoad(MciPs1Card *card, const unsigned char *data, u32 size,
                   MciPs1Container *kind_out);
void MciPs1CardFormat(MciPs1Card *card);
int MciPs1CardScan(const MciPs1Card *card, MciPs1CardSummary *summary);
unsigned char MciPs1FrameXor(const unsigned char frame[MCI_PS1_FRAME_BYTES]);

/* Copy one save (identified by its first slot) into a PS1 archive. */
int MciPs1CardExtract(const MciPs1Card *card, unsigned int first_slot,
                      MciSaveArchive *archive);
/* Insert a PS1 archive; fails with E_EXISTS on a name clash, E_FULL when
 * there are not enough free (or deleted) slots. Card is unchanged on error. */
int MciPs1CardInsert(MciPs1Card *card, const MciSaveArchive *archive);

/* Serialise the card in any container; VMP is signed, MCX encrypted. */
u32 MciPs1ContainerSize(MciPs1Container kind);
int MciPs1CardWrite(const MciPs1Card *card, MciPs1Container kind,
                    unsigned char *out, u32 out_size);

/* Single-save containers. `filename` is only used for headerless raw saves
 * (its basename, without extension, becomes the PS1 save name). */
typedef enum MciPs1SingleKind {
    MCI_PS1_SINGLE_NONE = 0,
    MCI_PS1_SINGLE_MCS,
    MCI_PS1_SINGLE_AR,
    MCI_PS1_SINGLE_RAW,
    MCI_PS1_SINGLE_PSV
} MciPs1SingleKind;

MciPs1SingleKind MciPs1DetectSingle(const unsigned char *data, u32 size);
const char *MciPs1SingleName(MciPs1SingleKind kind);
int MciPs1DecodeSingle(const unsigned char *data, u32 size,
                       const char *filename, MciSaveArchive *archive,
                       MciPs1SingleKind *kind_out);
/* Encode a PS1 archive as MCS (128 + n*8192 bytes). */
u32 MciPs1McsSize(const MciSaveArchive *archive);
int MciPs1EncodeMcs(const MciSaveArchive *archive, unsigned char *out,
                    u32 out_size);
/* Signed PS3 PSV (0x84 + body), MemcardRex-compatible layout. */
u32 MciPs1PsvSize(const MciSaveArchive *archive);
int MciPs1EncodePsv(const MciSaveArchive *archive, unsigned char *out,
                    u32 out_size);
/* Action Replay / GameShark / Xploder .psx (54 + body). */
u32 MciPs1ArSize(const MciSaveArchive *archive);
int MciPs1EncodeAr(const MciSaveArchive *archive, unsigned char *out,
                   u32 out_size);
/* Last MCX/VMP integrity result from MciPs1CardLoad: MCI_SIG_* value. */
int MciPs1LastIntegrity(void);

/*
 * PS1 save names. Games find their saves by exact name:
 *
 *   B + region (A America, E Europe, I Japan/Asia) + product code
 *   (4 letters, '-', 5 digits) + game-chosen identifier (0..8 chars)
 *   e.g. "BASCUS-94163FF7S01"
 *
 * Headerless raw saves carry no name, so it comes from the file name.
 */
int MciPs1NameLooksLikeProductCode(const char *name);
/* File-name stem (no directory, no extension), max 20 chars. */
void MciPs1NameFromPath(const char *path, char out[MCI_PS1_SAVE_NAME_MAX + 1u]);
/* Finds a product code anywhere in free text ("Final Fantasy VII
 * (SCUS-94163).bin", "SCUS_941.63") and writes "BASCUS-94163". Returns 1
 * if found. The game identifier suffix cannot be guessed. */
int MciPs1GuessNameFromText(const char *text, char out[MCI_PS1_SAVE_NAME_MAX + 1u]);
/* Rename a decoded PS1 archive (validated). */
int MciPs1ArchiveRename(MciSaveArchive *archive, const char *name);

/* Validate a PS1 archive body (non-empty, multiple of 8 KiB, <= 15 blocks,
 * "SC" title frame) and name. */
int MciPs1ArchiveValidate(const MciSaveArchive *archive);

#endif /* MCI_PS1_CARD_H */
