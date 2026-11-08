/* SPDX-License-Identifier: MIT */
/* Host tests for src/mc_fs_format.c: layout, validation and health decode. */
#include <stddef.h>
#include <string.h>

#include "mc_fs_format.h"
#include "test_util.h"

static void make_official_8mb(unsigned char page[512])
{
    MciMcSuperblock sb;
    unsigned i;

    memset(page, 0xFF, 512);
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, MCI_MC_SUPERBLOCK_MAGIC, 28);
    memcpy(sb.version, "1.2.0.0", 7);
    sb.page_len = 512;
    sb.pages_per_cluster = 2;
    sb.pages_per_block = 16;
    sb.unused = 0xFF00;
    sb.clusters_per_card = 8192;
    sb.alloc_offset = 41;
    sb.alloc_end = 8135;
    sb.rootdir_cluster = 0;
    sb.backup_block1 = 1023;
    sb.backup_block2 = 1022;
    sb.ifc_list[0] = 8;
    for (i = 0; i < 32u; i++)
        sb.bad_block_list[i] = 0xFFFFFFFFu;
    sb.card_type = 2;
    sb.card_flags = 0x52;
    memcpy(page, &sb, sizeof(sb));
}

/* Real 64 MiB card from docs/CARD_TOOLS.md (2026 hardware qualification). */
static void make_real_64mb(unsigned char page[512])
{
    MciMcSuperblock sb;
    make_official_8mb(page);
    memcpy(&sb, page, sizeof(sb));
    sb.pages_per_block = 32;
    sb.clusters_per_card = 65536;
    sb.alloc_offset = 266;
    sb.alloc_end = 65238;
    sb.backup_block1 = 4095;
    sb.backup_block2 = 4094;
    memcpy(page, &sb, sizeof(sb));
}

static MciMcSuperblock *SB(unsigned char page[512])
{
    return (MciMcSuperblock *)page;
}

static void test_layout(void)
{
    CHECK_EQ_U(offsetof(MciMcSuperblock, page_len), 0x28);
    CHECK_EQ_U(offsetof(MciMcSuperblock, clusters_per_card), 0x30);
    CHECK_EQ_U(offsetof(MciMcSuperblock, rootdir_cluster), 0x3C);
    CHECK_EQ_U(offsetof(MciMcSuperblock, backup_block1), 0x40);
    CHECK_EQ_U(offsetof(MciMcSuperblock, ifc_list), 0x50);
    CHECK_EQ_U(offsetof(MciMcSuperblock, bad_block_list), 0xD0);
    CHECK_EQ_U(offsetof(MciMcSuperblock, card_type), 0x150);
    CHECK_EQ_U(offsetof(MciMcSuperblock, card_flags), 0x151);
    CHECK_EQ_U(offsetof(MciMcDirEntry, created), 0x08);
    CHECK_EQ_U(offsetof(MciMcDirEntry, cluster), 0x10);
    CHECK_EQ_U(offsetof(MciMcDirEntry, modified), 0x18);
    CHECK_EQ_U(offsetof(MciMcDirEntry, attr), 0x20);
    CHECK_EQ_U(offsetof(MciMcDirEntry, name), 0x40);
    CHECK_EQ_U(sizeof(MciMcDirEntry), 512);
}

static void test_validate(void)
{
    unsigned char page[512];

    make_official_8mb(page);
    CHECK_EQ_U(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_GEOMETRY), 0);
    CHECK_EQ_U(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_FILESYSTEM), 0);
    CHECK_EQ_U(MciMcSuperblockTotalPages(SB(page)), 16384);
    CHECK(MciMcKnownPageCount(16384));
    CHECK(!MciMcKnownPageCount(16000));

    /* Regression: pages_per_block=32 on real 64 MiB cards must be accepted. */
    make_real_64mb(page);
    CHECK_EQ_U(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_FILESYSTEM), 0);
    CHECK_EQ_U(MciMcSuperblockTotalPages(SB(page)), 131072);

    make_official_8mb(page); page[0] = 'X';
    CHECK_EQ_I(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_GEOMETRY), MCI_MC_SB_BAD_MAGIC);

    make_official_8mb(page); SB(page)->page_len = 1024;
    CHECK_EQ_I(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_GEOMETRY), MCI_MC_SB_BAD_PAGE_LEN);

    make_official_8mb(page); SB(page)->pages_per_block = 0;
    CHECK_EQ_I(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_GEOMETRY), MCI_MC_SB_BAD_BLOCK_SIZE);

    /* ppc=4 is acceptable geometry for raw imaging but not walkable FS. */
    make_official_8mb(page); SB(page)->pages_per_cluster = 4; SB(page)->clusters_per_card = 4096;
    CHECK_EQ_U(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_GEOMETRY), 0);
    CHECK_EQ_I(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_FILESYSTEM), MCI_MC_SB_BAD_CLUSTER_SIZE);

    make_official_8mb(page); SB(page)->alloc_end = 9000;
    CHECK_EQ_U(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_GEOMETRY), 0);
    CHECK_EQ_I(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_FILESYSTEM), MCI_MC_SB_BAD_ALLOC_AREA);

    /* u32 wrap in alloc_offset + alloc_end must not pass. */
    make_official_8mb(page); SB(page)->alloc_offset = 10; SB(page)->alloc_end = 0xFFFFFFF8u;
    CHECK_EQ_I(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_FILESYSTEM), MCI_MC_SB_BAD_ALLOC_AREA);

    make_official_8mb(page); SB(page)->rootdir_cluster = 3;
    CHECK_EQ_I(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_FILESYSTEM), MCI_MC_SB_BAD_ROOTDIR);

    make_official_8mb(page); SB(page)->clusters_per_card = 0x40000000u;
    CHECK_EQ_I(MciMcSuperblockValidate(SB(page), MCI_MC_VALIDATE_GEOMETRY), MCI_MC_SB_BAD_PAGE_COUNT);

    CHECK_EQ_I(MciMcSuperblockValidate(NULL, MCI_MC_VALIDATE_GEOMETRY), MCI_MC_SB_BAD_ARGS);
    CHECK(strcmp(MciMcSuperblockErrorText(MCI_MC_SB_BAD_ROOTDIR), "bad root directory") == 0);
}

static void test_health(void)
{
    unsigned char page[512];
    MciMcHealth h;
    char flags[32];

    make_official_8mb(page);
    CHECK_EQ_U(MciMcHealthDecode(page, &h), 0);
    CHECK_EQ_U(h.flags, 0);
    CHECK_EQ_U(h.total_blocks, 1024);
    CHECK_EQ_U(h.bad_block_count, 0);
    CHECK_EQ_U(h.ifc_count, 1);
    CHECK(strcmp(h.version, "1.2.0.0") == 0);
    CHECK(strcmp(MciMcHealthVerdict(&h), "ok") == 0);
    MciMcCardFlagsText(h.card_flags, flags, sizeof(flags));
    CHECK(strcmp(flags, "ERASE0") == 0); /* 0x52: 0x10 set, 0x01/0x08 clear */

    make_real_64mb(page);
    CHECK_EQ_U(MciMcHealthDecode(page, &h), 0);
    CHECK(strcmp(MciMcHealthVerdict(&h), "ok") == 0);
    CHECK_EQ_U(h.total_blocks, 4096);

    /* Zeroed bad-block table (third-party formatters) is not 32 bad blocks. */
    make_official_8mb(page); memset(SB(page)->bad_block_list, 0, 128);
    MciMcHealthDecode(page, &h);
    CHECK_EQ_U(h.bad_block_count, 0);

    make_official_8mb(page);
    SB(page)->bad_block_list[0] = 12;
    SB(page)->bad_block_list[1] = 2000;
    SB(page)->card_flags = MCI_MC_CF_USE_ECC | MCI_MC_CF_BAD_BLOCK;
    MciMcHealthDecode(page, &h);
    CHECK_EQ_U(h.bad_block_count, 2);
    CHECK_EQ_U(h.bad_blocks[0], 12);
    CHECK(h.flags & MCI_MC_HEALTH_HAS_BAD_BLOCKS);
    CHECK(h.flags & MCI_MC_HEALTH_BAD_BLOCK_OUT_OF_RANGE);
    CHECK(strcmp(MciMcHealthVerdict(&h), "warn") == 0);
    MciMcCardFlagsText(h.card_flags, flags, sizeof(flags));
    CHECK(strcmp(flags, "ECC BADBLK") == 0);
    MciMcCardFlagsText(0, flags, sizeof(flags));
    CHECK(strcmp(flags, "none") == 0);

    make_official_8mb(page); SB(page)->backup_block2 = 1023;
    MciMcHealthDecode(page, &h);
    CHECK(h.flags & MCI_MC_HEALTH_BACKUP_BLOCKS_EQUAL);

    make_official_8mb(page); SB(page)->backup_block1 = 4000;
    MciMcHealthDecode(page, &h);
    CHECK(h.flags & MCI_MC_HEALTH_BACKUP_BLOCK_OUT_OF_RANGE);

    /* Backup block 10 = clusters 80..87, inside alloc area 41..8175. */
    make_official_8mb(page); SB(page)->backup_block1 = 10;
    MciMcHealthDecode(page, &h);
    CHECK(h.flags & MCI_MC_HEALTH_BACKUP_IN_ALLOC_AREA);

    make_official_8mb(page); SB(page)->ifc_list[0] = 0;
    MciMcHealthDecode(page, &h);
    CHECK(h.flags & MCI_MC_HEALTH_IFC_INVALID);

    make_official_8mb(page); SB(page)->card_type = 1;
    MciMcHealthDecode(page, &h);
    CHECK(h.flags & MCI_MC_HEALTH_NONSTANDARD_CARD_TYPE);

    /* Non-printable version bytes are sanitised for sidecars/UI. */
    make_official_8mb(page); SB(page)->version[0] = '\x01';
    MciMcHealthDecode(page, &h);
    CHECK(h.version[0] == '?');
    CHECK(h.flags & MCI_MC_HEALTH_NONSTANDARD_VERSION);

    make_official_8mb(page); page[3] = 0;
    CHECK_EQ_I(MciMcHealthDecode(page, &h), MCI_MC_SB_BAD_MAGIC);
    CHECK(strcmp(MciMcHealthVerdict(&h), "invalid") == 0);
    CHECK_EQ_I(MciMcHealthDecode(NULL, &h), MCI_MC_SB_BAD_ARGS);
}

int main(void)
{
    test_layout();
    test_validate();
    test_health();
    TEST_MAIN_END("test_mc_fs_format");
}
