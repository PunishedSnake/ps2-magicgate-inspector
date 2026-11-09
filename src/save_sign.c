/* SPDX-License-Identifier: MIT */
/* PSV / VMP signatures and MCX encryption. See save_sign.h. */

#include <tamtypes.h>
#include <stdlib.h>
#include <string.h>

#include "mci_crypto.h"
#include "save_sign.h"
#include "sha256.h"

static const u8 Ps1Key[16] = {
    0xAB, 0x5A, 0xBC, 0x9F, 0xC1, 0xF4, 0x9D, 0xE6,
    0xA0, 0x51, 0xDB, 0xAE, 0xFA, 0x51, 0x88, 0x59
};
static const u8 Ps2Key[16] = {
    0xEA, 0x02, 0xCE, 0xEF, 0x5B, 0xB4, 0xD2, 0x99,
    0x8F, 0x61, 0x19, 0x10, 0xD7, 0x7F, 0x51, 0xC6
};
static const u8 SaveIv[16] = {
    0xB3, 0x0F, 0xFE, 0xED, 0xB7, 0xDC, 0x5E, 0xB7,
    0x13, 0x3D, 0xA6, 0x0D, 0x1B, 0x6B, 0x2C, 0xDC
};
static const u8 McxKey[16] = {
    0x81, 0xD9, 0xCC, 0xE9, 0x71, 0xA9, 0x49, 0x9B,
    0x04, 0xAD, 0xDC, 0x48, 0x30, 0x7F, 0x07, 0x92
};
static const u8 McxIv[16] = {
    0x13, 0xC2, 0xE7, 0x69, 0x4B, 0xEC, 0x69, 0x6D,
    0x52, 0xCF, 0x00, 0x09, 0x2A, 0xC1, 0xF2, 0x72
};

static void DeriveKey(const u8 seed[20], int ps2, u8 key[20])
{
    MciAes128 aes;
    unsigned int i;

    if (!ps2) {
        u8 a[16];
        u8 b[16];
        MciAes128Init(&aes, Ps1Key);
        MciAes128DecryptBlock(&aes, seed, a);
        MciAes128EncryptBlock(&aes, seed, b);
        for (i = 0u; i < 16u; i++)
            key[i] = (u8)(a[i] ^ SaveIv[i]);
        for (i = 0u; i < 4u; i++)
            key[16u + i] = (u8)(b[i] ^ seed[16u + i]);
    } else {
        u8 buf[64];
        u8 iv[16];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, seed, 20);
        memcpy(iv, SaveIv, 16);
        MciAes128Init(&aes, Ps2Key);
        MciAes128CbcDecrypt(&aes, iv, buf, sizeof(buf));
        memcpy(key, buf, 20);
    }
}

static int AllZero(const u8 *p, unsigned int n)
{
    while (n--)
        if (*p++)
            return 0;
    return 1;
}

/* HMAC over the file with the 20-byte signature field treated as zero,
 * without modifying the caller's buffer. */
static void Signature(const unsigned char *data, u32 len, u32 sig_off,
                      const u8 key[20], u8 out[20])
{
    static const u8 Zero[20] = {0};
    u8 k[64];
    u8 pad[64];
    u8 inner[20];
    MciSha1 ctx;
    unsigned int i;

    memset(k, 0, sizeof(k));
    memcpy(k, key, 20);
    for (i = 0u; i < 64u; i++)
        pad[i] = (u8)(k[i] ^ 0x36u);
    MciSha1Init(&ctx);
    MciSha1Update(&ctx, pad, 64);
    MciSha1Update(&ctx, data, sig_off);
    MciSha1Update(&ctx, Zero, 20);
    MciSha1Update(&ctx, data + sig_off + 20u, len - sig_off - 20u);
    MciSha1Final(&ctx, inner);
    for (i = 0u; i < 64u; i++)
        pad[i] = (u8)(k[i] ^ 0x5Cu);
    MciSha1Init(&ctx);
    MciSha1Update(&ctx, pad, 64);
    MciSha1Update(&ctx, inner, 20);
    MciSha1Final(&ctx, out);
}

static int SignAt(unsigned char *buf, u32 len, u32 seed_off, u32 sig_off, int ps2)
{
    u8 key[20];

    if (buf == NULL || len < sig_off + 20u || len < seed_off + 20u)
        return -1;
    if (AllZero(buf + seed_off, 20u)) {
        memset(buf + sig_off, 0, 20);
        MciSha1Digest(buf, len, buf + seed_off);
    }
    DeriveKey(buf + seed_off, ps2, key);
    Signature(buf, len, sig_off, key, buf + sig_off);
    return 0;
}

static MciSigState VerifyAt(const unsigned char *buf, u32 len, u32 seed_off,
                            u32 sig_off, int ps2)
{
    u8 key[20];
    u8 sig[20];

    if (AllZero(buf + sig_off, 20u))
        return MCI_SIG_UNSIGNED;
    DeriveKey(buf + seed_off, ps2, key);
    Signature(buf, len, sig_off, key, sig);
    return memcmp(sig, buf + sig_off, 20) == 0 ? MCI_SIG_OK : MCI_SIG_BAD;
}

static int PsvType(const unsigned char *psv, u32 len)
{
    if (psv == NULL || len < 0x84u || memcmp(psv, "\0VSP", 4) != 0)
        return 0;
    if (psv[MCI_PSV_TYPE_OFFSET] == 1u || psv[MCI_PSV_TYPE_OFFSET] == 2u)
        return psv[MCI_PSV_TYPE_OFFSET];
    return 0;
}

int MciPsvSign(unsigned char *psv, u32 len)
{
    int type = PsvType(psv, len);
    if (type == 0)
        return -1;
    return SignAt(psv, len, MCI_PSV_SEED_OFFSET, MCI_PSV_SIG_OFFSET, type == 2);
}

MciSigState MciPsvVerify(const unsigned char *psv, u32 len)
{
    int type = PsvType(psv, len);
    if (type == 0)
        return MCI_SIG_UNKNOWN;
    return VerifyAt(psv, len, MCI_PSV_SEED_OFFSET, MCI_PSV_SIG_OFFSET, type == 2);
}

int MciVmpSign(unsigned char *vmp, u32 len)
{
    if (vmp == NULL || len != MCI_VMP_BYTES || memcmp(vmp, "\0PMV", 4) != 0)
        return -1;
    return SignAt(vmp, len, MCI_VMP_SEED_OFFSET, MCI_VMP_SIG_OFFSET, 0);
}

MciSigState MciVmpVerify(const unsigned char *vmp, u32 len)
{
    if (vmp == NULL || len != MCI_VMP_BYTES || memcmp(vmp, "\0PMV", 4) != 0)
        return MCI_SIG_UNKNOWN;
    return VerifyAt(vmp, len, MCI_VMP_SEED_OFFSET, MCI_VMP_SIG_OFFSET, 0);
}

const char *MciSigStateText(MciSigState state)
{
    switch (state) {
        case MCI_SIG_UNSIGNED: return "unsigned";
        case MCI_SIG_OK: return "valid";
        case MCI_SIG_BAD: return "INVALID";
        default: return "n/a";
    }
}

int MciMcxEncrypt(const unsigned char card[0x20000], unsigned char out[0x200A0])
{
    MciAes128 aes;
    u8 iv[16];

    if (card == NULL || out == NULL)
        return -1;
    memset(out, 0, MCI_MCX_BYTES);
    memcpy(out + 0x80u, card, 0x20000u);
    sha256_buffer(out, 0x20080u, out + 0x20080u);
    memcpy(iv, McxIv, 16);
    MciAes128Init(&aes, McxKey);
    MciAes128CbcEncrypt(&aes, iv, out, MCI_MCX_BYTES);
    return (int)MCI_MCX_BYTES;
}

static unsigned char *DecryptCopy(const unsigned char *mcx)
{
    unsigned char *plain = (unsigned char *)malloc(MCI_MCX_BYTES);
    MciAes128 aes;
    u8 iv[16];

    if (plain == NULL)
        return NULL;
    memcpy(plain, mcx, MCI_MCX_BYTES);
    memcpy(iv, McxIv, 16);
    MciAes128Init(&aes, McxKey);
    MciAes128CbcDecrypt(&aes, iv, plain, MCI_MCX_BYTES);
    return plain;
}

int MciMcxLooksValid(const unsigned char *mcx, u32 len)
{
    MciAes128 aes;
    u8 iv[16];
    u8 block[16];
    unsigned int i;

    /* Decrypting block 8 needs only ciphertext block 7 as its IV. */
    if (mcx == NULL || len != MCI_MCX_BYTES)
        return 0;
    memcpy(iv, mcx + 0x70u, 16);
    MciAes128Init(&aes, McxKey);
    MciAes128DecryptBlock(&aes, mcx + 0x80u, block);
    for (i = 0u; i < 16u; i++)
        block[i] ^= iv[i];
    return block[0] == 'M' && block[1] == 'C';
}

int MciMcxDecrypt(const unsigned char *mcx, u32 len, unsigned char card[0x20000])
{
    unsigned char *plain;
    u8 digest[32];
    int ok;

    if (!MciMcxLooksValid(mcx, len) || card == NULL)
        return -1;
    plain = DecryptCopy(mcx);
    if (plain == NULL)
        return -2;
    sha256_buffer(plain, 0x20080u, digest);
    ok = memcmp(digest, plain + 0x20080u, 32) == 0;
    memcpy(card, plain + 0x80u, 0x20000u);
    free(plain);
    return ok;
}
