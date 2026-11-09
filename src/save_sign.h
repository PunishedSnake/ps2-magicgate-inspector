/* SPDX-License-Identifier: MIT */
#ifndef MCI_SAVE_SIGN_H
#define MCI_SAVE_SIGN_H

/*
 * Signed and encrypted save containers.
 *
 *  PSV (PS3 export of PS1/PS2 saves)
 *      seed  @0x08 (20 bytes), signature @0x1C (20 bytes), type @0x3C.
 *      signature = HMAC-SHA1(key20, whole file with the signature zeroed).
 *      key20 is derived from the seed with AES-128:
 *        PS1: ECB-decrypt(seed[0:16]) ^ IV  ||  (ECB-encrypt(seed[0:16]) ^
 *             (seed[16:20] || FF..))[0:4]
 *        PS2: CBC-decrypt(seed || 0*44, PS2 key, IV)[0:20]
 *  VMP (PSP/Vita PS1 card)
 *      seed @0x0C, signature @0x20, PS1 derivation, over the 0x20080 file.
 *  MCX (PS1 card for PS Vita PocketStation/"MCX" apps)
 *      AES-128-CBC over 0x200A0 bytes: 0x80 header, 128 KiB card,
 *      SHA-256 of the first 0x20080 bytes.
 *
 * The constants are the publicly documented save-container keys used by
 * MemcardRex, Apollo Save Tool and ps3-psvresigner; output is cross-checked
 * byte-for-byte against both tools in tests/test_save_sign.c.
 */

#include <tamtypes.h>

#define MCI_PSV_SEED_OFFSET 0x08u
#define MCI_PSV_SIG_OFFSET 0x1Cu
#define MCI_PSV_TYPE_OFFSET 0x3Cu
#define MCI_VMP_SEED_OFFSET 0x0Cu
#define MCI_VMP_SIG_OFFSET 0x20u
#define MCI_VMP_BYTES 0x20080u
#define MCI_MCX_BYTES 0x200A0u

typedef enum MciSigState {
    MCI_SIG_UNKNOWN = 0,   /* not a signable container */
    MCI_SIG_UNSIGNED,      /* signature field is all zero */
    MCI_SIG_OK,
    MCI_SIG_BAD
} MciSigState;

/* Sign in place. When the seed is all zero a deterministic seed
 * (SHA-1 of the unsigned file) is written first, like MemcardRex. */
int MciPsvSign(unsigned char *psv, u32 len);
MciSigState MciPsvVerify(const unsigned char *psv, u32 len);
int MciVmpSign(unsigned char *vmp, u32 len);
MciSigState MciVmpVerify(const unsigned char *vmp, u32 len);
const char *MciSigStateText(MciSigState state);

/* MCX: encrypt a raw 128 KiB card into out[0x200A0]; decrypt the other way.
 * Decrypt returns 1 when the trailing SHA-256 matches, 0 when it does not
 * (data still returned), negative when the plaintext is not a PS1 card. */
int MciMcxEncrypt(const unsigned char card[0x20000], unsigned char out[0x200A0]);
int MciMcxDecrypt(const unsigned char *mcx, u32 len, unsigned char card[0x20000]);
int MciMcxLooksValid(const unsigned char *mcx, u32 len);

#endif /* MCI_SAVE_SIGN_H */
