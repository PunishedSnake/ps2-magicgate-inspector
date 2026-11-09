/* SPDX-License-Identifier: MIT */
/* LZARI decoder vs. vectors encoded by mymc++'s independent LZARI encoder
 * (tests/fixtures/lzari_vectors.bin). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lzari.h"
#include "test_util.h"

int main(void)
{
    FILE *f = fopen("fixtures/lzari_vectors.bin", "rb");
    unsigned h[2];
    unsigned n = 0;

    if (f == NULL)
        return 2;
    while (fread(h, 4, 2, f) == 2) {
        unsigned char *raw = malloc(h[0] + 1u), *enc = malloc(h[1] + 1u), *out = malloc(h[0] + 1u);
        if (fread(raw, 1, h[0], f) != h[0] || fread(enc, 1, h[1], f) != h[1])
            return 2;
        CHECK_EQ_I(MciLzariDecode(enc, h[1], out, h[0]), 0);
        CHECK(memcmp(raw, out, h[0]) == 0);
        /* A truncated stream must still terminate within bounds. */
        if (h[1] > 4u)
            CHECK_EQ_I(MciLzariDecode(enc, h[1] / 2u, out, h[0]), 0);
        free(raw); free(enc); free(out);
        n++;
    }
    fclose(f);
    CHECK_EQ_U(n, 7);
    CHECK_EQ_I(MciLzariDecode(NULL, 4, NULL, 0), -1);
    TEST_MAIN_END("test_lzari");
}
