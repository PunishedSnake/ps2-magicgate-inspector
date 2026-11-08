/* SPDX-License-Identifier: MIT */
#ifndef MCI_MC_FS_FORMAT_H
#define MCI_MC_FS_FORMAT_H

/*
 * Single source of truth for the on-media PS2 memory-card filesystem layout.
 *
 * Before 0.4.2 four translation units carried their own copy of the
 * superblock struct (card_image.c, card_image_fs.c, image_quick_verify.c,
 * image_save_title.c) with slightly different validation rules. This header
 * owns the layout; mc_fs_format.c owns validation and health decoding.
 *
 * Everything here is portable C with no PS2SDK dependency beyond tamtypes.h so
 * it can be compiled and tested on the host (see tests/).
 *
 * Layout reference: Ross Ridge, "PlayStation 2 Memory Card File System", and
 * the ps2devwiki Memory Card page.
 */

#include <tamtypes.h>

#define MCI_MC_PAGE_DATA 512u
#define MCI_MC_PAGE_SPARE 16u
#define MCI_MC_PAGE_RAW (MCI_MC_PAGE_DATA + MCI_MC_PAGE_SPARE)
#define MCI_MC_MAX_CLUSTER_BYTES 1024u
#define MCI_MC_MAX_PAGES 262144u /* 128 MiB logical */

#define MCI_MC_MODE_READ 0x0001u
#define MCI_MC_MODE_WRITE 0x0002u
#define MCI_MC_MODE_EXEC 0x0004u
#define MCI_MC_MODE_PROTECTED 0x0008u
#define MCI_MC_MODE_FILE 0x0010u
#define MCI_MC_MODE_DIR 0x0020u
#define MCI_MC_MODE_HIDDEN 0x2000u
#define MCI_MC_MODE_EXISTS 0x8000u

#define MCI_MC_FAT_END 0xFFFFFFFFu
#define MCI_MC_FAT_FREE 0x7FFFFFFFu
#define MCI_MC_FAT_ALLOCATED_BIT 0x80000000u
#define MCI_MC_FAT_NEXT_MASK 0x7FFFFFFFu

#define MCI_MC_CARD_TYPE_PS2 2u

#define MCI_MC_CF_USE_ECC 0x01u
#define MCI_MC_CF_BAD_BLOCK 0x08u
#define MCI_MC_CF_ERASE_ZEROES 0x10u

#define MCI_MC_SUPERBLOCK_MAGIC "Sony PS2 Memory Card Format "
#define MCI_MC_SUPERBLOCK_MAGIC_LEN 28u

/* Byte-compatible with PS2SDK sceMcStDateTime. */
typedef struct MciMcDateTime {
    u8 resv;
    u8 sec;
    u8 min;
    u8 hour;
    u8 day;
    u8 month;
    u16 year;
} __attribute__((packed)) MciMcDateTime;

typedef struct MciMcSuperblock {
    char magic[28];
    char version[12];
    u16 page_len;
    u16 pages_per_cluster;
    u16 pages_per_block;
    u16 unused;
    u32 clusters_per_card;
    u32 alloc_offset;
    u32 alloc_end;
    u32 rootdir_cluster;
    u32 backup_block1;
    u32 backup_block2;
    u64 padding0x48;
    u32 ifc_list[32];
    u32 bad_block_list[32];
    u8 card_type;
    u8 card_flags;
} __attribute__((packed)) MciMcSuperblock;

typedef struct MciMcDirEntry {
    u16 mode;
    u16 unused;
    u32 length;
    MciMcDateTime created;
    u32 cluster;
    u32 dir_entry;
    MciMcDateTime modified;
    u32 attr;
    u32 unused2[7];
    char name[32];
    u8 unused3[416];
} __attribute__((packed)) MciMcDirEntry;

/* Compile-time layout gates; any drift breaks the build, not a card. */
typedef char MciMcDateTimeSizeCheck[(sizeof(MciMcDateTime) == 8u) ? 1 : -1];
typedef char MciMcSuperblockSizeCheck[(sizeof(MciMcSuperblock) == 0x152u) ? 1 : -1];
typedef char MciMcDirEntrySizeCheck[(sizeof(MciMcDirEntry) == 512u) ? 1 : -1];

/*
 * Validation levels.
 *
 * GEOMETRY   - enough to trust page/cluster counts for raw imaging and
 *              size cross-checks. Used by card_image.c and quick verify.
 * FILESYSTEM - additionally requires a walkable allocation area and root
 *              directory. Used by the image browser and title resolver.
 *
 * pages_per_block is physical erase geometry. It must be non-zero but is NOT
 * capped at the official 8 MiB value of 16 (real 64 MiB cards use 32).
 */
typedef enum MciMcValidateLevel {
    MCI_MC_VALIDATE_GEOMETRY = 0,
    MCI_MC_VALIDATE_FILESYSTEM = 1
} MciMcValidateLevel;

typedef enum MciMcSuperblockError {
    MCI_MC_SB_OK = 0,
    MCI_MC_SB_BAD_ARGS = -1,
    MCI_MC_SB_BAD_MAGIC = -2,
    MCI_MC_SB_BAD_PAGE_LEN = -3,
    MCI_MC_SB_BAD_CLUSTER_SIZE = -4,
    MCI_MC_SB_BAD_BLOCK_SIZE = -5,
    MCI_MC_SB_BAD_CLUSTER_COUNT = -6,
    MCI_MC_SB_BAD_PAGE_COUNT = -7,
    MCI_MC_SB_BAD_ALLOC_AREA = -8,
    MCI_MC_SB_BAD_ROOTDIR = -9
} MciMcSuperblockError;

int MciMcSuperblockValidate(const MciMcSuperblock *sb, MciMcValidateLevel level);
const char *MciMcSuperblockErrorText(int rc);
u32 MciMcSuperblockTotalPages(const MciMcSuperblock *sb);
int MciMcKnownPageCount(u32 pages);

/*
 * Read-only health summary decoded from the superblock alone. This never
 * touches the card; callers feed it page 0 from a card or image.
 */
#define MCI_MC_HEALTH_BAD_BLOCKS_MAX 32u

typedef enum MciMcHealthFlag {
    MCI_MC_HEALTH_NONSTANDARD_CARD_TYPE = 0x0001,
    /* 0x0002 reserved: Sony-formatted cards report card_flags=0x52 without
     * CF_USE_ECC, so that bit says nothing about ECC use and is not judged. */
    MCI_MC_HEALTH_HAS_BAD_BLOCKS = 0x0004,
    MCI_MC_HEALTH_BAD_BLOCK_OUT_OF_RANGE = 0x0008,
    MCI_MC_HEALTH_BACKUP_BLOCK_OUT_OF_RANGE = 0x0010,
    MCI_MC_HEALTH_BACKUP_BLOCKS_EQUAL = 0x0020,
    MCI_MC_HEALTH_BACKUP_IN_ALLOC_AREA = 0x0040,
    MCI_MC_HEALTH_IFC_INVALID = 0x0080,
    MCI_MC_HEALTH_NONSTANDARD_VERSION = 0x0100
} MciMcHealthFlag;

typedef struct MciMcHealth {
    int validate_rc;
    u32 flags;
    char version[13];
    u8 card_type;
    u8 card_flags;
    u32 total_pages;
    u32 total_blocks;
    u32 bad_block_count;
    u32 bad_blocks[MCI_MC_HEALTH_BAD_BLOCKS_MAX];
    u32 backup_block1;
    u32 backup_block2;
    u32 ifc_count;
} MciMcHealth;

int MciMcHealthDecode(const unsigned char page0[MCI_MC_PAGE_DATA],
                      MciMcHealth *health);
/* Short "ok" / "warn" / "invalid" verdict for UI/sidecars. */
const char *MciMcHealthVerdict(const MciMcHealth *health);
/* Writes "ECC BADBLK ERASE0" style flag mnemonics. Returns chars written. */
int MciMcCardFlagsText(u8 card_flags, char *out, unsigned int out_size);

#endif /* MCI_MC_FS_FORMAT_H */
