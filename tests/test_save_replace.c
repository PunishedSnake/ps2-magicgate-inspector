/* SPDX-License-Identifier: MIT */
/* PS1 naming helpers and single-file replace planning. */
#include <string.h>

#include "ps1_card.h"
#include "save_replace.h"
#include "test_util.h"

static void test_ps1_names(void)
{
    char out[MCI_PS1_SAVE_NAME_MAX + 1u];

    CHECK(MciPs1NameLooksLikeProductCode("BASCUS-94163FF7S01"));
    CHECK(MciPs1NameLooksLikeProductCode("BESLES-12345"));
    CHECK(MciPs1NameLooksLikeProductCode("BISLPS-00700SAVE"));
    CHECK(!MciPs1NameLooksLikeProductCode("save"));
    CHECK(!MciPs1NameLooksLikeProductCode("BXSLUS-00001"));     /* region */
    CHECK(!MciPs1NameLooksLikeProductCode("BASLUS_00001"));     /* dash */
    CHECK(!MciPs1NameLooksLikeProductCode("BASLUS-0001A"));     /* digits */
    CHECK(!MciPs1NameLooksLikeProductCode("BASLUS-00001123456789")); /* > 20 */

    MciPs1NameFromPath("mass:/SAVES/BASCUS-94163FF7S01.bin", out);
    CHECK(strcmp(out, "BASCUS-94163FF7S01") == 0);
    MciPs1NameFromPath("mass:/x/save", out);
    CHECK(strcmp(out, "save") == 0);

    CHECK(MciPs1GuessNameFromText("Final Fantasy VII (SCUS-94163).bin", out));
    CHECK(strcmp(out, "BASCUS-94163") == 0);
    CHECK(MciPs1GuessNameFromText("mass:/POPS/SLES_123.45.Crash.bin", out));
    CHECK(strcmp(out, "BESLES-12345") == 0);
    CHECK(MciPs1GuessNameFromText("slps-01234 memory", out));
    CHECK(strcmp(out, "BISLPS-01234") == 0);
    CHECK(!MciPs1GuessNameFromText("my save 2024.bin", out));
    CHECK(!MciPs1GuessNameFromText("SCUS-941634", out));       /* 6 digits */
    CHECK(!MciPs1GuessNameFromText("XSCUS-94163", out));       /* not a word start */
    CHECK(!MciPs1GuessNameFromText("SCXS-94163", out));        /* unknown region */
    CHECK(!MciPs1GuessNameFromText(NULL, out));
}

static void test_rename(void)
{
    MciSaveArchive a;
    unsigned char body[8192];

    memset(body, 0, sizeof(body));
    body[0] = 'S'; body[1] = 'C';
    MciSaveArchiveInit(&a);
    a.generation = MCI_SAVE_GEN_PS1;
    a.file_count = 1;
    strcpy(a.files[0].name, "save");
    a.files[0].size = sizeof(body);
    a.files[0].data = body;
    CHECK_EQ_I(MciPs1ArchiveRename(&a, "BASCUS-94163FF7S01"), 0);
    CHECK(strcmp(a.files[0].name, "BASCUS-94163FF7S01") == 0);
    CHECK(strcmp(a.directory, "BASCUS-94163FF7S01") == 0);
    CHECK_EQ_I(MciPs1ArchiveValidate(&a), 0);
    CHECK_EQ_I(MciPs1ArchiveRename(&a, "BASCUS-94163FF7S01-TOOLONG"), MCI_SAVE_E_NAME);
    CHECK_EQ_I(MciPs1ArchiveRename(&a, "bad/name"), MCI_SAVE_E_NAME);
}

static void test_replace_plan(void)
{
    MciReplacePlan p;
    const char *existing[] = {"icon.sys", "list.ico", "DATA", "DATA.mci"};
    const unsigned char good_icon[4] = {'P', 'S', '2', 'D'};
    const unsigned char bad_icon[4] = {'X', 'X', 'X', 'X'};

    CHECK_EQ_I(MciReplacePlanMake("DATA", 5000, 5000, NULL, 0, existing, 4, 100, &p), 0);
    CHECK_EQ_U(p.old_clusters, 5);
    CHECK_EQ_U(p.new_clusters, 5);
    CHECK_EQ_U(p.required_free, 6);
    CHECK(!p.size_changed);
    CHECK(strcmp(p.temp_name, "DATA~1.mci") == 0); /* DATA.mci taken */

    CHECK_EQ_I(MciReplacePlanMake("DATA", 5000, 9000, NULL, 0, existing, 4, 9, &p), MCI_REPLACE_E_NO_SPACE);
    CHECK_EQ_I(MciReplacePlanMake("DATA", 5000, 9000, NULL, 0, existing, 4, 10, &p), 0);
    CHECK(p.size_changed);

    /* 32-char names: temp stays within the limit. */
    CHECK_EQ_I(MciReplacePlanMake("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", 1, 1, NULL, 0, NULL, 0, 10, &p), 0);
    CHECK_EQ_U(strlen(p.temp_name), 32);
    CHECK(strcmp(p.temp_name + 28, ".mci") == 0);

    CHECK_EQ_I(MciReplacePlanMake("icon.sys", 964, 964, bad_icon, 4, NULL, 0, 10, &p), MCI_REPLACE_E_BAD_ICONSYS);
    CHECK_EQ_I(MciReplacePlanMake("icon.sys", 964, 964, good_icon, 4, NULL, 0, 10, &p), 0);
    CHECK(p.is_icon_sys);
    CHECK_EQ_I(MciReplacePlanMake("../x", 1, 1, NULL, 0, NULL, 0, 10, &p), MCI_REPLACE_E_NAME);
    CHECK_EQ_I(MciReplacePlanMake("DATA", 1, MCI_SAVE_MAX_BYTES + 1u, NULL, 0, NULL, 0, 1 << 20, &p), MCI_REPLACE_E_TOO_BIG);
}

int main(void)
{
    test_ps1_names();
    test_rename();
    test_replace_plan();
    TEST_MAIN_END("test_save_replace");
}
