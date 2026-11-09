/* SPDX-License-Identifier: MIT */
#ifndef MCI_CRYPTO_H
#define MCI_CRYPTO_H

/*
 * Small, portable crypto primitives needed by save containers:
 *   AES-128 (FIPS-197) ECB/CBC, SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104).
 * Written for clarity, not speed or side-channel resistance: the inputs are
 * public save-container keys and the user's own save data.
 */

#include <tamtypes.h>

typedef struct MciAes128 {
    u8 round_keys[176];
} MciAes128;

void MciAes128Init(MciAes128 *ctx, const u8 key[16]);
void MciAes128EncryptBlock(const MciAes128 *ctx, const u8 in[16], u8 out[16]);
void MciAes128DecryptBlock(const MciAes128 *ctx, const u8 in[16], u8 out[16]);
/* In-place CBC over whole 16-byte blocks; iv is updated. len % 16 == 0. */
void MciAes128CbcEncrypt(const MciAes128 *ctx, u8 iv[16], u8 *buf, u32 len);
void MciAes128CbcDecrypt(const MciAes128 *ctx, u8 iv[16], u8 *buf, u32 len);

typedef struct MciSha1 {
    u32 state[5];
    u64 length;
    u8 block[64];
    u32 used;
} MciSha1;

void MciSha1Init(MciSha1 *ctx);
void MciSha1Update(MciSha1 *ctx, const void *data, u32 len);
void MciSha1Final(MciSha1 *ctx, u8 digest[20]);
void MciSha1Digest(const void *data, u32 len, u8 digest[20]);

void MciHmacSha1(const u8 *key, u32 key_len, const void *data, u32 len,
                 u8 digest[20]);

#endif /* MCI_CRYPTO_H */
