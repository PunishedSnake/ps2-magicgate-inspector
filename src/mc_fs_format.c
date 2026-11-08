/* SPDX-License-Identifier: MIT */
/* Portable PS2 memory-card superblock validation and health decoding. */

#include <tamtypes.h>
#include <stdio.h>
#include <string.h>

#include "mc_fs_format.h"

int MciMcKnownPageCount(u32 pages)
{
    return pages == 8192u || pages == 16384u || pages == 32768u ||
           pages == 65536u || pages == 131072u || pages == 262144u;
}

u32 MciMcSuperblockTotalPages(const MciMcSuperblock *sb)
{
    u64 pages;

    if (sb == NULL)
        return 0u;
    pages = (u64)sb->pages_per_cluster * (u64)sb->clusters_per_card;
    if (pages > MCI_MC_MAX_PAGES)
        return 0u;
    return (u32)pages;
}

int MciMcSuperblockValidate(const MciMcSuperblock *sb, MciMcValidateLevel level)
{
    u32 pages;

    if (sb == NULL)
        return MCI_MC_SB_BAD_ARGS;
    if (memcmp(sb->magic, MCI_MC_SUPERBLOCK_MAGIC, MCI_MC_SUPERBLOCK_MAGIC_LEN) != 0)
        return MCI_MC_SB_BAD_MAGIC;
    if (sb->page_len != MCI_MC_PAGE_DATA)
        return MCI_MC_SB_BAD_PAGE_LEN;
    if (sb->pages_per_cluster == 0u)
        return MCI_MC_SB_BAD_CLUSTER_SIZE;
    if (sb->pages_per_block == 0u)
        return MCI_MC_SB_BAD_BLOCK_SIZE;
    if (sb->clusters_per_card == 0u)
        return MCI_MC_SB_BAD_CLUSTER_COUNT;
    pages = MciMcSuperblockTotalPages(sb);
    if (pages == 0u)
        return MCI_MC_SB_BAD_PAGE_COUNT;

    if (level == MCI_MC_VALIDATE_GEOMETRY)
        return MCI_MC_SB_OK;

    if (sb->pages_per_cluster != 1u && sb->pages_per_cluster != 2u)
        return MCI_MC_SB_BAD_CLUSTER_SIZE;
    if ((u32)sb->page_len * sb->pages_per_cluster > MCI_MC_MAX_CLUSTER_BYTES)
        return MCI_MC_SB_BAD_CLUSTER_SIZE;
    if (sb->alloc_offset >= sb->clusters_per_card || sb->alloc_end == 0u ||
        (u64)sb->alloc_offset + sb->alloc_end > sb->clusters_per_card)
        return MCI_MC_SB_BAD_ALLOC_AREA;
    if (sb->rootdir_cluster != 0u)
        return MCI_MC_SB_BAD_ROOTDIR;
    return MCI_MC_SB_OK;
}

const char *MciMcSuperblockErrorText(int rc)
{
    switch (rc) {
        case MCI_MC_SB_OK: return "ok";
        case MCI_MC_SB_BAD_ARGS: return "no superblock";
        case MCI_MC_SB_BAD_MAGIC: return "bad magic";
        case MCI_MC_SB_BAD_PAGE_LEN: return "bad page length";
        case MCI_MC_SB_BAD_CLUSTER_SIZE: return "bad cluster size";
        case MCI_MC_SB_BAD_BLOCK_SIZE: return "bad erase-block size";
        case MCI_MC_SB_BAD_CLUSTER_COUNT: return "bad cluster count";
        case MCI_MC_SB_BAD_PAGE_COUNT: return "bad page count";
        case MCI_MC_SB_BAD_ALLOC_AREA: return "bad allocation area";
        case MCI_MC_SB_BAD_ROOTDIR: return "bad root directory";
        default: return "unknown superblock error";
    }
}

int MciMcCardFlagsText(u8 card_flags, char *out, unsigned int out_size)
{
    int written;

    if (out == NULL || out_size == 0u)
        return 0;
    written = snprintf(out, out_size, "%s%s%s",
                       (card_flags & MCI_MC_CF_USE_ECC) ? "ECC " : "",
                       (card_flags & MCI_MC_CF_BAD_BLOCK) ? "BADBLK " : "",
                       (card_flags & MCI_MC_CF_ERASE_ZEROES) ? "ERASE0 " : "");
    if (written <= 0) {
        snprintf(out, out_size, "none");
        return (int)strlen(out);
    }
    if ((unsigned int)written >= out_size)
        written = (int)out_size - 1;
    /* Trim the trailing separator. */
    if (written > 0 && out[written - 1] == ' ')
        out[--written] = '\0';
    return written;
}

static int RangesOverlap(u64 a_begin, u64 a_end, u64 b_begin, u64 b_end)
{
    return a_begin < b_end && b_begin < a_end;
}

int MciMcHealthDecode(const unsigned char page0[MCI_MC_PAGE_DATA],
                      MciMcHealth *health)
{
    MciMcSuperblock sb;
    unsigned int i;

    if (health == NULL)
        return MCI_MC_SB_BAD_ARGS;
    memset(health, 0, sizeof(*health));
    if (page0 == NULL) {
        health->validate_rc = MCI_MC_SB_BAD_ARGS;
        return health->validate_rc;
    }
    memcpy(&sb, page0, sizeof(sb));

    health->validate_rc = MciMcSuperblockValidate(&sb, MCI_MC_VALIDATE_FILESYSTEM);
    memcpy(health->version, sb.version, 12u);
    health->version[12] = '\0';
    for (i = 0u; i < 12u; i++) {
        unsigned char c = (unsigned char)health->version[i];
        if (c == '\0')
            break;
        if (c < 0x20u || c > 0x7Eu)
            health->version[i] = '?';
    }
    health->card_type = sb.card_type;
    health->card_flags = sb.card_flags;
    health->backup_block1 = sb.backup_block1;
    health->backup_block2 = sb.backup_block2;
    if (health->validate_rc < 0)
        return health->validate_rc;

    health->total_pages = MciMcSuperblockTotalPages(&sb);
    health->total_blocks = health->total_pages / sb.pages_per_block;

    if (sb.card_type != MCI_MC_CARD_TYPE_PS2)
        health->flags |= MCI_MC_HEALTH_NONSTANDARD_CARD_TYPE;
    if (health->version[0] != '1' || health->version[1] != '.')
        health->flags |= MCI_MC_HEALTH_NONSTANDARD_VERSION;

    /* Unused bad-block slots are 0xFFFFFFFF on Sony-formatted cards. Some
     * third-party formatters zero the table instead; block 0 always holds the
     * superblock so it can never be a remapped bad block. */
    for (i = 0u; i < 32u; i++) {
        u32 block = sb.bad_block_list[i];
        if (block == 0xFFFFFFFFu || block == 0u)
            continue;
        health->bad_blocks[health->bad_block_count++] = block;
        if (block >= health->total_blocks)
            health->flags |= MCI_MC_HEALTH_BAD_BLOCK_OUT_OF_RANGE;
    }
    if (health->bad_block_count != 0u)
        health->flags |= MCI_MC_HEALTH_HAS_BAD_BLOCKS;

    if (sb.backup_block1 >= health->total_blocks ||
        sb.backup_block2 >= health->total_blocks) {
        health->flags |= MCI_MC_HEALTH_BACKUP_BLOCK_OUT_OF_RANGE;
    } else {
        u64 clusters_per_block = sb.pages_per_block / sb.pages_per_cluster;
        u64 alloc_begin = sb.alloc_offset;
        u64 alloc_end = (u64)sb.alloc_offset + sb.alloc_end;
        u64 b1 = (u64)sb.backup_block1 * clusters_per_block;
        u64 b2 = (u64)sb.backup_block2 * clusters_per_block;

        if (sb.backup_block1 == sb.backup_block2)
            health->flags |= MCI_MC_HEALTH_BACKUP_BLOCKS_EQUAL;
        if (clusters_per_block != 0u &&
            (RangesOverlap(b1, b1 + clusters_per_block, alloc_begin, alloc_end) ||
             RangesOverlap(b2, b2 + clusters_per_block, alloc_begin, alloc_end)))
            health->flags |= MCI_MC_HEALTH_BACKUP_IN_ALLOC_AREA;
    }

    for (i = 0u; i < 32u; i++) {
        u32 cluster = sb.ifc_list[i];
        if (cluster == 0u || cluster == 0xFFFFFFFFu)
            break;
        if (cluster >= sb.clusters_per_card) {
            health->flags |= MCI_MC_HEALTH_IFC_INVALID;
            break;
        }
        health->ifc_count++;
    }
    if (health->ifc_count == 0u)
        health->flags |= MCI_MC_HEALTH_IFC_INVALID;

    return MCI_MC_SB_OK;
}

const char *MciMcHealthVerdict(const MciMcHealth *health)
{
    const u32 warn_mask = MCI_MC_HEALTH_NONSTANDARD_CARD_TYPE |
                          MCI_MC_HEALTH_HAS_BAD_BLOCKS |
                          MCI_MC_HEALTH_BAD_BLOCK_OUT_OF_RANGE |
                          MCI_MC_HEALTH_BACKUP_BLOCK_OUT_OF_RANGE |
                          MCI_MC_HEALTH_BACKUP_BLOCKS_EQUAL |
                          MCI_MC_HEALTH_BACKUP_IN_ALLOC_AREA |
                          MCI_MC_HEALTH_IFC_INVALID |
                          MCI_MC_HEALTH_NONSTANDARD_VERSION;

    if (health == NULL || health->validate_rc < 0)
        return "invalid";
    return (health->flags & warn_mask) ? "warn" : "ok";
}
