/* SPDX-License-Identifier: MIT */
/* AES-128, SHA-1 and HMAC-SHA1. See mci_crypto.h. */

#include <tamtypes.h>
#include <string.h>

#include "mci_crypto.h"

/* ---------------------------------------------------------------- AES */

static const u8 SBox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static u8 InvSBox[256];
static int InvReady;

static void EnsureInv(void)
{
    unsigned int i;
    if (InvReady)
        return;
    for (i = 0u; i < 256u; i++)
        InvSBox[SBox[i]] = (u8)i;
    InvReady = 1;
}

static u8 XTime(u8 x)
{
    return (u8)((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0u));
}

static u8 Mul(u8 a, u8 b)
{
    u8 r = 0u;
    while (b) {
        if (b & 1u)
            r ^= a;
        a = XTime(a);
        b >>= 1;
    }
    return r;
}

void MciAes128Init(MciAes128 *ctx, const u8 key[16])
{
    static const u8 Rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
    u8 *w = ctx->round_keys;
    unsigned int i;

    EnsureInv();
    memcpy(w, key, 16);
    for (i = 4u; i < 44u; i++) {
        u8 t[4];
        memcpy(t, w + (i - 1u) * 4u, 4);
        if ((i % 4u) == 0u) {
            u8 tmp = t[0];
            t[0] = (u8)(SBox[t[1]] ^ Rcon[i / 4u - 1u]);
            t[1] = SBox[t[2]];
            t[2] = SBox[t[3]];
            t[3] = SBox[tmp];
        }
        w[i * 4u + 0u] = (u8)(w[(i - 4u) * 4u + 0u] ^ t[0]);
        w[i * 4u + 1u] = (u8)(w[(i - 4u) * 4u + 1u] ^ t[1]);
        w[i * 4u + 2u] = (u8)(w[(i - 4u) * 4u + 2u] ^ t[2]);
        w[i * 4u + 3u] = (u8)(w[(i - 4u) * 4u + 3u] ^ t[3]);
    }
}

static void AddRoundKey(u8 s[16], const u8 *k)
{
    unsigned int i;
    for (i = 0u; i < 16u; i++)
        s[i] ^= k[i];
}

void MciAes128EncryptBlock(const MciAes128 *ctx, const u8 in[16], u8 out[16])
{
    u8 s[16];
    unsigned int round;
    unsigned int i;

    memcpy(s, in, 16);
    AddRoundKey(s, ctx->round_keys);
    for (round = 1u; round <= 10u; round++) {
        u8 t[16];
        for (i = 0u; i < 16u; i++)
            s[i] = SBox[s[i]];
        /* ShiftRows: state is column-major, byte r of column c at s[4c+r]. */
        for (i = 0u; i < 16u; i++)
            t[i] = s[(i + 4u * (i % 4u)) % 16u];
        if (round != 10u) {
            for (i = 0u; i < 4u; i++) {
                u8 *c = t + 4u * i;
                u8 a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
                c[0] = (u8)(XTime(a0) ^ XTime(a1) ^ a1 ^ a2 ^ a3);
                c[1] = (u8)(a0 ^ XTime(a1) ^ XTime(a2) ^ a2 ^ a3);
                c[2] = (u8)(a0 ^ a1 ^ XTime(a2) ^ XTime(a3) ^ a3);
                c[3] = (u8)(XTime(a0) ^ a0 ^ a1 ^ a2 ^ XTime(a3));
            }
        }
        memcpy(s, t, 16);
        AddRoundKey(s, ctx->round_keys + 16u * round);
    }
    memcpy(out, s, 16);
}

void MciAes128DecryptBlock(const MciAes128 *ctx, const u8 in[16], u8 out[16])
{
    u8 s[16];
    int round;
    unsigned int i;

    EnsureInv();
    memcpy(s, in, 16);
    AddRoundKey(s, ctx->round_keys + 160u);
    for (round = 9; round >= 0; round--) {
        u8 t[16];
        /* InvShiftRows */
        for (i = 0u; i < 16u; i++)
            t[(i + 4u * (i % 4u)) % 16u] = s[i];
        for (i = 0u; i < 16u; i++)
            t[i] = InvSBox[t[i]];
        AddRoundKey(t, ctx->round_keys + 16u * (unsigned int)round);
        if (round != 0) {
            for (i = 0u; i < 4u; i++) {
                u8 *c = t + 4u * i;
                u8 a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
                c[0] = (u8)(Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9));
                c[1] = (u8)(Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13));
                c[2] = (u8)(Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11));
                c[3] = (u8)(Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14));
            }
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

void MciAes128CbcEncrypt(const MciAes128 *ctx, u8 iv[16], u8 *buf, u32 len)
{
    u32 off;
    unsigned int i;

    for (off = 0u; off + 16u <= len; off += 16u) {
        for (i = 0u; i < 16u; i++)
            buf[off + i] ^= iv[i];
        MciAes128EncryptBlock(ctx, buf + off, buf + off);
        memcpy(iv, buf + off, 16);
    }
}

void MciAes128CbcDecrypt(const MciAes128 *ctx, u8 iv[16], u8 *buf, u32 len)
{
    u32 off;
    unsigned int i;

    for (off = 0u; off + 16u <= len; off += 16u) {
        u8 cipher[16];
        memcpy(cipher, buf + off, 16);
        MciAes128DecryptBlock(ctx, buf + off, buf + off);
        for (i = 0u; i < 16u; i++)
            buf[off + i] ^= iv[i];
        memcpy(iv, cipher, 16);
    }
}

/* -------------------------------------------------------------- SHA-1 */

static u32 Rol(u32 x, unsigned int n)
{
    return (x << n) | (x >> (32u - n));
}

static void Sha1Block(MciSha1 *ctx, const u8 *p)
{
    u32 w[80];
    u32 a, b, c, d, e;
    unsigned int i;

    for (i = 0u; i < 16u; i++)
        w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) |
               ((u32)p[4 * i + 2] << 8) | (u32)p[4 * i + 3];
    for (i = 16u; i < 80u; i++)
        w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2];
    d = ctx->state[3]; e = ctx->state[4];
    for (i = 0u; i < 80u; i++) {
        u32 f, k, t;
        if (i < 20u)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40u) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60u) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else              { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        t = Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

void MciSha1Init(MciSha1 *ctx)
{
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->length = 0u;
    ctx->used = 0u;
}

void MciSha1Update(MciSha1 *ctx, const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;

    ctx->length += len;
    while (len > 0u) {
        u32 take = 64u - ctx->used;
        if (take > len)
            take = len;
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (ctx->used == 64u) {
            Sha1Block(ctx, ctx->block);
            ctx->used = 0u;
        }
    }
}

void MciSha1Final(MciSha1 *ctx, u8 digest[20])
{
    u64 bits = ctx->length * 8u;
    u8 pad = 0x80u;
    u8 len_be[8];
    unsigned int i;

    MciSha1Update(ctx, &pad, 1);
    pad = 0u;
    while (ctx->used != 56u)
        MciSha1Update(ctx, &pad, 1);
    for (i = 0u; i < 8u; i++)
        len_be[i] = (u8)(bits >> (56u - 8u * i));
    MciSha1Update(ctx, len_be, 8);
    for (i = 0u; i < 5u; i++) {
        digest[4 * i] = (u8)(ctx->state[i] >> 24);
        digest[4 * i + 1] = (u8)(ctx->state[i] >> 16);
        digest[4 * i + 2] = (u8)(ctx->state[i] >> 8);
        digest[4 * i + 3] = (u8)ctx->state[i];
    }
}

void MciSha1Digest(const void *data, u32 len, u8 digest[20])
{
    MciSha1 ctx;
    MciSha1Init(&ctx);
    MciSha1Update(&ctx, data, len);
    MciSha1Final(&ctx, digest);
}

void MciHmacSha1(const u8 *key, u32 key_len, const void *data, u32 len,
                 u8 digest[20])
{
    u8 k[64];
    u8 pad[64];
    u8 inner[20];
    MciSha1 ctx;
    unsigned int i;

    memset(k, 0, sizeof(k));
    if (key_len > 64u)
        MciSha1Digest(key, key_len, k);
    else
        memcpy(k, key, key_len);
    for (i = 0u; i < 64u; i++)
        pad[i] = (u8)(k[i] ^ 0x36u);
    MciSha1Init(&ctx);
    MciSha1Update(&ctx, pad, 64);
    MciSha1Update(&ctx, data, len);
    MciSha1Final(&ctx, inner);
    for (i = 0u; i < 64u; i++)
        pad[i] = (u8)(k[i] ^ 0x5Cu);
    MciSha1Init(&ctx);
    MciSha1Update(&ctx, pad, 64);
    MciSha1Update(&ctx, inner, 20);
    MciSha1Final(&ctx, digest);
}
