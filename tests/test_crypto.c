/* SPDX-License-Identifier: MIT */
/* FIPS-197 AES-128, NIST SP 800-38A CBC, FIPS 180 SHA-1, RFC 2202 HMAC-SHA1. */
#include <string.h>

#include "mci_crypto.h"
#include "test_util.h"

static void hex(const char *s, u8 *out)
{
    unsigned i;
    for (i = 0; s[2 * i]; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (u8)v;
    }
}

int main(void)
{
    MciAes128 aes;
    u8 key[16], in[64], out[64], exp[64], iv[16];
    u8 d[20], e[20];
    static u8 million[1000000];

    /* FIPS-197 appendix C.1 */
    hex("000102030405060708090a0b0c0d0e0f", key);
    hex("00112233445566778899aabbccddeeff", in);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", exp);
    MciAes128Init(&aes, key);
    MciAes128EncryptBlock(&aes, in, out);
    CHECK(memcmp(out, exp, 16) == 0);
    MciAes128DecryptBlock(&aes, out, out);
    CHECK(memcmp(out, in, 16) == 0);

    /* SP 800-38A F.2.1/F.2.2 CBC-AES128 */
    hex("2b7e151628aed2a6abf7158809cf4f3c", key);
    hex("000102030405060708090a0b0c0d0e0f", iv);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", in);
    hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7", exp);
    MciAes128Init(&aes, key);
    memcpy(out, in, 64);
    MciAes128CbcEncrypt(&aes, iv, out, 64);
    CHECK(memcmp(out, exp, 64) == 0);
    hex("000102030405060708090a0b0c0d0e0f", iv);
    MciAes128CbcDecrypt(&aes, iv, out, 64);
    CHECK(memcmp(out, in, 64) == 0);

    /* SHA-1 */
    MciSha1Digest("abc", 3, d);
    hex("a9993e364706816aba3e25717850c26c9cd0d89d", e);
    CHECK(memcmp(d, e, 20) == 0);
    MciSha1Digest("", 0, d);
    hex("da39a3ee5e6b4b0d3255bfef95601890afd80709", e);
    CHECK(memcmp(d, e, 20) == 0);
    MciSha1Digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    hex("84983e441c3bd26ebaae4aa1f95129e5e54670f1", e);
    CHECK(memcmp(d, e, 20) == 0);
    memset(million, 'a', sizeof(million));
    MciSha1Digest(million, sizeof(million), d);
    hex("34aa973cd4c4daa4f61eeb2bdbad27316534016f", e);
    CHECK(memcmp(d, e, 20) == 0);

    /* RFC 2202 test cases 1, 2, 6 (key longer than block) */
    memset(key, 0x0b, 16);
    MciHmacSha1(key, 20 > 16 ? 16 : 16, "Hi There", 8, d);
    {
        u8 k20[20];
        memset(k20, 0x0b, 20);
        MciHmacSha1(k20, 20, "Hi There", 8, d);
        hex("b617318655057264e28bc0b6fb378c8ef146be00", e);
        CHECK(memcmp(d, e, 20) == 0);
    }
    MciHmacSha1((const u8 *)"Jefe", 4, "what do ya want for nothing?", 28, d);
    hex("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", e);
    CHECK(memcmp(d, e, 20) == 0);
    {
        u8 k80[80];
        memset(k80, 0xaa, 80);
        MciHmacSha1(k80, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
        hex("aa4ae5e15272d00e95705637ce8a3b55ed402112", e);
        CHECK(memcmp(d, e, 20) == 0);
    }
    TEST_MAIN_END("test_crypto");
}
