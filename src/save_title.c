/* SPDX-License-Identifier: MIT */
/* Minimal, fail-closed mcIcon/icon.sys title decoder. */

#include <tamtypes.h>
#include <string.h>

#include "save_title.h"

/* PS2SDK mcIcon layout:
 * 0x000 head[4]
 * 0x004 type
 * 0x006 nlOffset
 * 0x008 unknown2
 * 0x00C trans
 * 0x010 bgCol[4]       (64 bytes)
 * 0x050 lightDir[3]    (48 bytes)
 * 0x080 lightCol[3]    (48 bytes)
 * 0x0B0 lightAmbient   (16 bytes)
 * 0x0C0 title[34]
 */
#define ICON_SYS_TITLE_OFFSET 192u
#define ICON_SYS_TITLE_UNITS 34u
#define ICON_SYS_NL_OFFSET 6u

static u16 ReadLe16(const unsigned char *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

int MciSaveTitleDecodeIconSys(const void *data, unsigned int size,
                              char *out, unsigned int out_size)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned int nl;
    unsigned int i;
    unsigned int used = 0u;
    unsigned int printable = 0u;
    int pending_space = 0;

    if (data == NULL || out == NULL || out_size < 2u ||
        size < ICON_SYS_TITLE_OFFSET + ICON_SYS_TITLE_UNITS * 2u)
        return -1;
    out[0] = '\0';
    if (memcmp(p, "PS2D", 4) != 0)
        return -2;
    nl = (unsigned int)ReadLe16(p + ICON_SYS_NL_OFFSET);

    for (i = 0u; i < ICON_SYS_TITLE_UNITS; i++) {
        u16 code = ReadLe16(p + ICON_SYS_TITLE_OFFSET + i * 2u);
        unsigned char c;

        if (code == 0u)
            break;
        if (i == nl && used > 0u)
            pending_space = 1;

        /* mcIcon stores Shift-JIS values in 16-bit units. ASCII maps directly
         * and covers the vast majority of western save titles. Full-width
         * spaces are normalized; unsupported double-byte glyphs are skipped.
         * If no useful ASCII survives, the caller keeps the raw directory id. */
        if (code == 0x8140u) {
            pending_space = 1;
            continue;
        }
        if (code >= 0x20u && code <= 0x7Eu) {
            c = (unsigned char)code;
        } else {
            pending_space = 1;
            continue;
        }

        if (pending_space && used > 0u && out[used - 1u] != ' ') {
            if (used + 1u >= out_size)
                break;
            out[used++] = ' ';
        }
        pending_space = 0;
        if (used + 1u >= out_size)
            break;
        out[used++] = (char)c;
        if (c != ' ')
            printable++;
    }

    while (used > 0u && out[used - 1u] == ' ')
        used--;
    out[used] = '\0';
    return printable >= 3u ? 0 : -3;
}
