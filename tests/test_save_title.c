/* SPDX-License-Identifier: MIT */
/* Host tests for src/save_title.c (icon.sys title decoder). */
#include <string.h>

#include "save_title.h"
#include "test_util.h"

#define TITLE_OFF 192u

static void icon_init(unsigned char icon[964], unsigned nl)
{
    memset(icon, 0, 964);
    memcpy(icon, "PS2D", 4);
    icon[6] = (unsigned char)(nl & 0xFFu);
    icon[7] = (unsigned char)(nl >> 8);
}

/* Encode ASCII as full-width Shift-JIS, the way retail titles are stored. */
static unsigned put_fullwidth(unsigned char *dst, const char *s)
{
    unsigned n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') { dst[n++] = 0x82; dst[n++] = (unsigned char)(0x60 + c - 'A'); }
        else if (c >= 'a' && c <= 'z') { dst[n++] = 0x82; dst[n++] = (unsigned char)(0x81 + c - 'a'); }
        else if (c >= '0' && c <= '9') { dst[n++] = 0x82; dst[n++] = (unsigned char)(0x4F + c - '0'); }
        else if (c == ' ') { dst[n++] = 0x81; dst[n++] = 0x40; }
        else if (c == ':') { dst[n++] = 0x81; dst[n++] = 0x46; }
        else if (c == '-') { dst[n++] = 0x81; dst[n++] = 0x7C; }
        else { dst[n++] = c; }
    }
    return n;
}

int main(void)
{
    unsigned char icon[964];
    char out[MCI_SAVE_TITLE_MAX];
    unsigned n;

    /* Retail style: two full-width lines split at nlOffset (in bytes). */
    n = put_fullwidth(icon + TITLE_OFF, "KINGDOM");
    icon_init(icon, 0);
    n = put_fullwidth(icon + TITLE_OFF, "KINGDOM");
    icon[6] = (unsigned char)n;
    put_fullwidth(icon + TITLE_OFF + n, "HEARTS");
    CHECK_EQ_U(MciSaveTitleDecodeIconSys(icon, sizeof(icon), out, sizeof(out)), 0);
    CHECK(strcmp(out, "KINGDOM HEARTS") == 0);

    /* Mixed case, digits, punctuation and full-width spaces. */
    icon_init(icon, 0);
    put_fullwidth(icon + TITLE_OFF, "Ace Combat 04: Data");
    CHECK_EQ_U(MciSaveTitleDecodeIconSys(icon, sizeof(icon), out, sizeof(out)), 0);
    CHECK(strcmp(out, "Ace Combat 04: Data") == 0);

    /* Homebrew style: plain single-byte ASCII. */
    icon_init(icon, 0);
    memcpy(icon + TITLE_OFF, "Your System Configuration", 25);
    CHECK_EQ_U(MciSaveTitleDecodeIconSys(icon, sizeof(icon), out, sizeof(out)), 0);
    CHECK(strcmp(out, "Your System Configuration") == 0);

    /* Kanji/kana are skipped as word breaks, ASCII survives. */
    icon_init(icon, 0);
    n = put_fullwidth(icon + TITLE_OFF, "FF");
    icon[TITLE_OFF + n++] = 0x83; icon[TITLE_OFF + n++] = 0x5E; /* katakana TA */
    icon[TITLE_OFF + n++] = 0x88; icon[TITLE_OFF + n++] = 0x9F; /* kanji */
    put_fullwidth(icon + TITLE_OFF + n, "XII");
    CHECK_EQ_U(MciSaveTitleDecodeIconSys(icon, sizeof(icon), out, sizeof(out)), 0);
    CHECK(strcmp(out, "FF XII") == 0);

    /* Pure Japanese title: no useful ASCII -> caller keeps directory name. */
    icon_init(icon, 0);
    for (n = 0; n < 16u; n += 2u) { icon[TITLE_OFF + n] = 0x88; icon[TITLE_OFF + n + 1] = 0x9F; }
    CHECK_EQ_I(MciSaveTitleDecodeIconSys(icon, sizeof(icon), out, sizeof(out)), -3);
    CHECK(out[0] == '\0');

    /* Truncated lead byte at the 68-byte limit stops cleanly. */
    icon_init(icon, 0);
    memset(icon + TITLE_OFF, 'A', 67);
    icon[TITLE_OFF + 67] = 0x82;
    icon[TITLE_OFF + 68] = 0x60; /* outside the title field: must not be read as trail */
    CHECK_EQ_U(MciSaveTitleDecodeIconSys(icon, sizeof(icon), out, sizeof(out)), 0);
    CHECK_EQ_U(strlen(out), 67);

    /* Output buffer bound is respected. */
    icon_init(icon, 0);
    put_fullwidth(icon + TITLE_OFF, "ABCDEFGHIJKLMNOP");
    CHECK_EQ_U(MciSaveTitleDecodeIconSys(icon, sizeof(icon), out, 8), 0);
    CHECK(strcmp(out, "ABCDEFG") == 0);

    /* Rejections. */
    CHECK_EQ_I(MciSaveTitleDecodeIconSys(icon, 200, out, sizeof(out)), -1);
    icon[0] = 'X';
    CHECK_EQ_I(MciSaveTitleDecodeIconSys(icon, sizeof(icon), out, sizeof(out)), -2);
    CHECK_EQ_I(MciSaveTitleDecodeIconSys(NULL, sizeof(icon), out, sizeof(out)), -1);

    TEST_MAIN_END("test_save_title");
}
