/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "sha256.h"
#include "test_util.h"

static void hex_of(const void *data, size_t size, char out[65])
{
    unsigned char d[32];
    sha256_buffer(data, size, d);
    sha256_hex(d, out);
}

int main(void)
{
    char hex[65];
    static unsigned char big[1000000];
    sha256_context_t ctx;
    unsigned char d1[32], d2[32];
    size_t i;

    hex_of("", 0, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    hex_of("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    memset(big, 'a', sizeof(big));
    hex_of(big, sizeof(big), hex);
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);

    /* Streaming in .ps2 record-sized pieces equals one-shot. */
    for (i = 0; i < sizeof(big); i++)
        big[i] = rng_byte();
    sha256_buffer(big, sizeof(big), d1);
    sha256_init(&ctx);
    for (i = 0; i < sizeof(big); i += 528u)
        sha256_update(&ctx, big + i, (sizeof(big) - i) < 528u ? sizeof(big) - i : 528u);
    sha256_final(&ctx, d2);
    CHECK(memcmp(d1, d2, 32) == 0);

    TEST_MAIN_END("test_sha256");
}
