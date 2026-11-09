/* SPDX-License-Identifier: MIT */
/*
 * LZARI decoder, written from Okumura's published algorithm (LZARI.C,
 * 1989-04-07, public domain). State lives in one heap/static context so the
 * EE stack stays small.
 */

#include <tamtypes.h>
#include <stdlib.h>
#include <string.h>

#include "lzari.h"

#define LZ_N 4096u
#define LZ_F 60u
#define LZ_THRESHOLD 2u
#define LZ_N_CHAR (256u - LZ_THRESHOLD + LZ_F) /* 314 */
#define LZ_M 15u
#define LZ_Q1 (1u << LZ_M)
#define LZ_Q2 (2u * LZ_Q1)
#define LZ_Q3 (3u * LZ_Q1)
#define LZ_Q4 (4u * LZ_Q1)
#define LZ_MAX_CUM (LZ_Q1 - 1u)

typedef struct LzariState {
    const unsigned char *in;
    u32 in_size;
    u32 in_pos;
    unsigned int bit_buffer;
    unsigned int bit_mask;

    u32 low;
    u32 high;
    u32 value;

    unsigned int sym_to_char[LZ_N_CHAR + 1u];
    unsigned int sym_freq[LZ_N_CHAR + 1u];
    unsigned int sym_cum[LZ_N_CHAR + 1u];
    unsigned int position_cum[LZ_N + 1u];
    unsigned char text[LZ_N];
} LzariState;

static unsigned int GetBit(LzariState *s)
{
    s->bit_mask >>= 1;
    if (s->bit_mask == 0u) {
        s->bit_buffer = s->in_pos < s->in_size ? s->in[s->in_pos] : 0u;
        s->in_pos++;
        s->bit_mask = 0x80u;
    }
    return (s->bit_buffer & s->bit_mask) != 0u;
}

static void StartModel(LzariState *s)
{
    unsigned int sym;
    unsigned int i;

    s->sym_cum[LZ_N_CHAR] = 0u;
    for (sym = LZ_N_CHAR; sym >= 1u; sym--) {
        s->sym_to_char[sym] = sym - 1u;
        s->sym_freq[sym] = 1u;
        s->sym_cum[sym - 1u] = s->sym_cum[sym] + s->sym_freq[sym];
    }
    s->sym_freq[0] = 0u;
    s->sym_to_char[0] = 0u;
    s->position_cum[LZ_N] = 0u;
    for (i = LZ_N; i >= 1u; i--)
        s->position_cum[i - 1u] = s->position_cum[i] + 10000u / (i + 200u);
}

static void UpdateModel(LzariState *s, unsigned int sym)
{
    unsigned int i;

    if (s->sym_cum[0] >= LZ_MAX_CUM) {
        unsigned int c = 0u;
        for (i = LZ_N_CHAR; i > 0u; i--) {
            s->sym_cum[i] = c;
            s->sym_freq[i] = (s->sym_freq[i] + 1u) >> 1;
            c += s->sym_freq[i];
        }
        s->sym_cum[0] = c;
    }
    for (i = sym; s->sym_freq[i] == s->sym_freq[i - 1u]; i--)
        ;
    if (i < sym) {
        unsigned int ch_i = s->sym_to_char[i];
        unsigned int ch_sym = s->sym_to_char[sym];
        s->sym_to_char[i] = ch_sym;
        s->sym_to_char[sym] = ch_i;
    }
    s->sym_freq[i]++;
    while (i > 0u) {
        i--;
        s->sym_cum[i]++;
    }
}

static unsigned int SearchSym(const LzariState *s, unsigned int x)
{
    unsigned int i = 1u;
    unsigned int j = LZ_N_CHAR;

    while (i < j) {
        unsigned int k = (i + j) / 2u;
        if (s->sym_cum[k] > x)
            i = k + 1u;
        else
            j = k;
    }
    return i;
}

static unsigned int SearchPos(const LzariState *s, unsigned int x)
{
    unsigned int i = 1u;
    unsigned int j = LZ_N;

    while (i < j) {
        unsigned int k = (i + j) / 2u;
        if (s->position_cum[k] > x)
            i = k + 1u;
        else
            j = k;
    }
    return i - 1u;
}

static void Renormalise(LzariState *s)
{
    for (;;) {
        if (s->low >= LZ_Q2) {
            s->value -= LZ_Q2;
            s->low -= LZ_Q2;
            s->high -= LZ_Q2;
        } else if (s->low >= LZ_Q1 && s->high <= LZ_Q3) {
            s->value -= LZ_Q1;
            s->low -= LZ_Q1;
            s->high -= LZ_Q1;
        } else if (s->high > LZ_Q2) {
            break;
        }
        s->low += s->low;
        s->high += s->high;
        s->value = 2u * s->value + GetBit(s);
    }
}

static unsigned int DecodeChar(LzariState *s)
{
    u64 range = (u64)(s->high - s->low);
    unsigned int total = s->sym_cum[0];
    unsigned int sym;
    unsigned int ch;

    sym = SearchSym(s, (unsigned int)((((u64)(s->value - s->low) + 1u) * total - 1u) / range));
    s->high = s->low + (u32)((range * s->sym_cum[sym - 1u]) / total);
    s->low += (u32)((range * s->sym_cum[sym]) / total);
    Renormalise(s);
    ch = s->sym_to_char[sym];
    UpdateModel(s, sym);
    return ch;
}

static unsigned int DecodePosition(LzariState *s)
{
    u64 range = (u64)(s->high - s->low);
    unsigned int total = s->position_cum[0];
    unsigned int pos;

    pos = SearchPos(s, (unsigned int)((((u64)(s->value - s->low) + 1u) * total - 1u) / range));
    s->high = s->low + (u32)((range * s->position_cum[pos]) / total);
    s->low += (u32)((range * s->position_cum[pos + 1u]) / total);
    Renormalise(s);
    return pos;
}

int MciLzariDecode(const unsigned char *in, u32 in_size,
                   unsigned char *out, u32 out_size)
{
    LzariState *s;
    unsigned int r;
    u32 count = 0u;
    unsigned int i;

    if ((in == NULL && in_size != 0u) || (out == NULL && out_size != 0u))
        return -1;
    if (out_size == 0u)
        return 0;
    s = (LzariState *)malloc(sizeof(*s));
    if (s == NULL)
        return -2;
    memset(s, 0, sizeof(*s));
    s->in = in;
    s->in_size = in_size;
    s->high = LZ_Q4;

    for (i = 0u; i < LZ_M + 2u; i++)
        s->value = 2u * s->value + GetBit(s);
    StartModel(s);
    memset(s->text, ' ', LZ_N - LZ_F);
    r = LZ_N - LZ_F;

    while (count < out_size) {
        unsigned int c = DecodeChar(s);
        if (c < 256u) {
            out[count++] = (unsigned char)c;
            s->text[r++] = (unsigned char)c;
            r &= LZ_N - 1u;
        } else {
            unsigned int start = (r - DecodePosition(s) - 1u) & (LZ_N - 1u);
            unsigned int len = c - 255u + LZ_THRESHOLD;
            unsigned int k;
            for (k = 0u; k < len && count < out_size; k++) {
                unsigned char b = s->text[(start + k) & (LZ_N - 1u)];
                out[count++] = b;
                s->text[r++] = b;
                r &= LZ_N - 1u;
            }
        }
    }
    free(s);
    return 0;
}
