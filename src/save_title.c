/* SPDX-License-Identifier: MIT */
/* Minimal, fail-closed mcIcon/icon.sys title decoder. */

#include <tamtypes.h>
#include <string.h>

#include "save_title.h"

/* PS2SDK mcIcon layout:
 * 0x000 head[4]        "PS2D"
 * 0x004 type
 * 0x006 nlOffset       line-break position, in BYTES into title
 * 0x008 unknown2
 * 0x00C trans
 * 0x010 bgCol[4]       (64 bytes)
 * 0x050 lightDir[3]    (48 bytes)
 * 0x080 lightCol[3]    (48 bytes)
 * 0x0B0 lightAmbient   (16 bytes)
 * 0x0C0 title          68-byte NUL-terminated Shift-JIS byte string
 *
 * PS2SDK declares title as `unsigned short title[34]`, but the contents are a
 * plain Shift-JIS byte stream (mymc: struct "68s" + decode("shift_jis")).
 * 0.4.0/0.4.1 read it as little-endian 16-bit units, which made almost every
 * retail title fail and fall back to the directory name.
 */
#define ICON_SYS_TITLE_OFFSET 192u
#define ICON_SYS_TITLE_BYTES 68u
#define ICON_SYS_NL_OFFSET 6u

static u16 ReadLe16(const unsigned char *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static int IsSjisLead(unsigned char c)
{
    return (c >= 0x81u && c <= 0x9Fu) || (c >= 0xE0u && c <= 0xFCu);
}

static int IsSjisTrail(unsigned char c)
{
    return (c >= 0x40u && c <= 0x7Eu) || (c >= 0x80u && c <= 0xFCu);
}

/* Fold the full-width (JIS X 0208 row 1-3) characters that retail Western
 * titles are written in back to ASCII. Returns 0 if the glyph has no useful
 * ASCII equivalent; returns ' ' for spacing characters. */
static char FoldDoubleByte(unsigned char lead, unsigned char trail)
{
    u16 code = (u16)(((u16)lead << 8) | trail);

    if (code >= 0x824Fu && code <= 0x8258u)
        return (char)('0' + (code - 0x824Fu));
    if (code >= 0x8260u && code <= 0x8279u)
        return (char)('A' + (code - 0x8260u));
    if (code >= 0x8281u && code <= 0x829Au)
        return (char)('a' + (code - 0x8281u));

    switch (code) {
        case 0x8140u: return ' ';
        case 0x8141u: return ',';  /* ideographic comma */
        case 0x8142u: return '.';  /* ideographic full stop */
        case 0x8143u: return ',';
        case 0x8144u: return '.';
        case 0x8145u: return '.';  /* katakana middle dot */
        case 0x8146u: return ':';
        case 0x8147u: return ';';
        case 0x8148u: return '?';
        case 0x8149u: return '!';
        case 0x814Fu: return '^';
        case 0x8151u: return '_';
        case 0x815Bu: return '-';  /* prolonged sound mark */
        case 0x815Cu: return '-';
        case 0x815Du: return '-';
        case 0x815Eu: return '/';
        case 0x815Fu: return '\\';
        case 0x8160u: return '~';
        case 0x8162u: return '|';
        case 0x8165u: return '\'';
        case 0x8166u: return '\'';
        case 0x8167u: return '"';
        case 0x8168u: return '"';
        case 0x8169u: return '(';
        case 0x816Au: return ')';
        case 0x816Du: return '[';
        case 0x816Eu: return ']';
        case 0x816Fu: return '{';
        case 0x8170u: return '}';
        case 0x8175u: return '"';  /* corner brackets */
        case 0x8176u: return '"';
        case 0x817Bu: return '+';
        case 0x817Cu: return '-';
        case 0x8181u: return '=';
        case 0x8183u: return '<';
        case 0x8184u: return '>';
        case 0x818Fu: return '\\'; /* yen sign */
        case 0x8190u: return '$';
        case 0x8193u: return '%';
        case 0x8194u: return '#';
        case 0x8195u: return '&';
        case 0x8196u: return '*';
        case 0x8197u: return '@';
        default: return 0;
    }
}

int MciSjisFoldToAscii(const unsigned char *sjis, unsigned int max_bytes,
                       unsigned int break_at, char *out, unsigned int out_size)
{
    unsigned int i = 0u;
    unsigned int used = 0u;
    unsigned int printable = 0u;
    int pending_space = 0;

    if (sjis == NULL || out == NULL || out_size < 2u)
        return -1;
    out[0] = '\0';
    while (i < max_bytes) {
        unsigned char b = sjis[i];
        char c;

        if (b == 0u)
            break;
        if (i == break_at && used > 0u)
            pending_space = 1;

        if (IsSjisLead(b)) {
            if (i + 1u >= max_bytes || !IsSjisTrail(sjis[i + 1u]))
                break; /* truncated or malformed: keep what we have */
            c = FoldDoubleByte(b, sjis[i + 1u]);
            i += 2u;
        } else {
            c = (b >= 0x20u && b <= 0x7Eu) ? (char)b : 0;
            i += 1u; /* half-width katakana / control bytes are skipped */
        }

        if (c == 0 || c == ' ') {
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
        out[used++] = c;
        printable++;
    }

    while (used > 0u && out[used - 1u] == ' ')
        used--;
    out[used] = '\0';
    return (int)printable;
}

int MciSaveTitleDecodeIconSys(const void *data, unsigned int size,
                              char *out, unsigned int out_size)
{
    const unsigned char *p = (const unsigned char *)data;
    int printable;

    if (data == NULL || out == NULL || out_size < 2u ||
        size < ICON_SYS_TITLE_OFFSET + ICON_SYS_TITLE_BYTES)
        return -1;
    out[0] = '\0';
    if (memcmp(p, "PS2D", 4) != 0)
        return -2;
    printable = MciSjisFoldToAscii(p + ICON_SYS_TITLE_OFFSET,
                                   ICON_SYS_TITLE_BYTES,
                                   (unsigned int)ReadLe16(p + ICON_SYS_NL_OFFSET),
                                   out, out_size);
    return printable >= 3 ? 0 : -3;
}
