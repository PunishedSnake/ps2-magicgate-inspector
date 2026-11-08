/* SPDX-License-Identifier: MIT */
/* Host tests for src/card_math.c: CRC32 and PS2 memory-card ECC.
 *
 * The production code is checked against two independent oracles:
 *  - PCSX2 FolderMemoryCard::CalculateECC's algorithm (table-driven), and
 *  - a port of mymc ps2mc_ecc._ecc_calculate (column masks from cpmasks).
 */
#include <string.h>

#include "card_math.h"
#include "test_util.h"

/* PCSX2's 256-byte table equals column_mask(b) | parity(b) << 7 with the
 * mymc cpmasks column layout (checked against PCSX2 master on 2026-11-08).
 * It is regenerated here rather than copied so no GPL text enters the repo. */
static unsigned char Pcsx2Table[256];

static int parity8(unsigned v);

static void build_pcsx2_table(void)
{
    static const unsigned char cpmasks[7] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0};
    unsigned b, k;
    for (b = 0; b < 256u; b++) {
        unsigned v = (unsigned)parity8(b) << 7;
        for (k = 0; k < 7u; k++)
            v |= (unsigned)parity8(b & cpmasks[k]) << k;
        Pcsx2Table[b] = (unsigned char)v;
    }
    /* Spot values from the PCSX2 source table. */
    CHECK_EQ_U(Pcsx2Table[0x00], 0x00);
    CHECK_EQ_U(Pcsx2Table[0x01], 0x87);
    CHECK_EQ_U(Pcsx2Table[0x02], 0x96);
    CHECK_EQ_U(Pcsx2Table[0x10], 0xC3);
    CHECK_EQ_U(Pcsx2Table[0xFF], 0x00);
}

static void pcsx2_ecc(const unsigned char *data, unsigned char ecc[3])
{
    int i;
    ecc[0] = ecc[1] = ecc[2] = 0;
    for (i = 0; i < 0x80; i++) {
        const int c = Pcsx2Table[data[i]];
        ecc[0] ^= (unsigned char)c;
        if (c & 0x80) {
            ecc[1] ^= (unsigned char)~i;
            ecc[2] ^= (unsigned char)i;
        }
    }
    ecc[0] = (unsigned char)(~ecc[0] & 0x77);
    ecc[1] = (unsigned char)(~ecc[1] & 0x7f);
    ecc[2] = (unsigned char)(~ecc[2] & 0x7f);
}

static int parity8(unsigned v)
{
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (int)(v & 1u);
}

static void mymc_ecc(const unsigned char *s, unsigned char ecc[3], int keep_bit7)
{
    static const unsigned char cpmasks[7] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0};
    unsigned column = 0x77, l0 = 0x7F, l1 = 0x7F;
    int i, k;
    for (i = 0; i < 128; i++) {
        unsigned mask = 0;
        for (k = 0; k < 7; k++)
            mask |= (unsigned)parity8(s[i] & cpmasks[k]) << k;
        column ^= mask;
        if (parity8(s[i])) {
            l0 ^= (unsigned)~i & 0xFFu;
            l1 ^= (unsigned)i;
        }
    }
    ecc[0] = (unsigned char)column;
    ecc[1] = (unsigned char)(keep_bit7 ? (l0 & 0xFFu) : (l0 & 0x7Fu));
    ecc[2] = (unsigned char)l1;
}

static void test_crc32(void)
{
    const char *v = "123456789";
    unsigned char buf[4096];
    unsigned i;
    u32 whole, split;

    CHECK_EQ_U(MciCardMathCrc32Update(0u, (const unsigned char *)v, 9u), 0xCBF43926u);
    CHECK_EQ_U(MciCardMathCrc32Update(0u, NULL, 0u), 0u);
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = rng_byte();
    whole = MciCardMathCrc32Update(0u, buf, sizeof(buf));
    for (i = 0; i <= 37u; i++) {
        split = MciCardMathCrc32Update(0u, buf, i);
        split = MciCardMathCrc32Update(split, buf + i, (unsigned)sizeof(buf) - i);
        CHECK_EQ_U(split, whole);
    }
}

static void test_ecc_known_vectors(void)
{
    unsigned char chunk[128];
    u32 v;

    memset(chunk, 0, sizeof(chunk));
    v = MciCardMathEcc128(chunk);
    CHECK_EQ_U(v, 0x7F7F77u);

    /* One odd-parity byte at index 0: line0 ^= ~0 -> bit 7 must be masked. */
    chunk[0] = 0x01;
    v = MciCardMathEcc128(chunk);
    CHECK_EQ_U((v >> 8) & 0xFFu, 0x00u);
    CHECK_EQ_U(v & 0xFFu, 0x70u);
    CHECK_EQ_U((v >> 16) & 0xFFu, 0x7Fu);
}

static void test_ecc_matches_oracles(void)
{
    unsigned char page[512];
    unsigned char spare[16];
    unsigned iter, chunk;

    for (iter = 0; iter < 20000u; iter++) {
        unsigned i;
        switch (iter) {
            case 0: memset(page, 0x00, sizeof(page)); break;
            case 1: memset(page, 0xFF, sizeof(page)); break;
            case 2: for (i = 0; i < 512u; i++) page[i] = (unsigned char)i; break;
            default: for (i = 0; i < 512u; i++) page[i] = rng_byte(); break;
        }
        MciCardMathBuildSpare(page, spare);
        for (chunk = 0; chunk < 4u; chunk++) {
            unsigned char a[3], b[3];
            pcsx2_ecc(page + chunk * 128u, a);
            mymc_ecc(page + chunk * 128u, b, 0);
            CHECK(memcmp(a, b, 3) == 0);
            CHECK(memcmp(spare + chunk * 3u, a, 3) == 0);
        }
        CHECK(spare[12] == 0 && spare[13] == 0 && spare[14] == 0 && spare[15] == 0);
        CHECK_EQ_U(MciCardMathSpareCompare(page, spare), MCI_ECC_MATCH_CANONICAL);
    }
}

static void test_ecc_legacy_acceptance(void)
{
    unsigned char page[512];
    unsigned char spare[16];
    unsigned i, chunk, legacy_hits = 0;

    for (i = 0; i < 4000u; i++) {
        unsigned j;
        int any_bit7 = 0;
        for (j = 0; j < 512u; j++)
            page[j] = rng_byte();
        /* Reproduce exactly what Drebin 0.4.0/0.4.1 wrote. */
        for (chunk = 0; chunk < 4u; chunk++) {
            mymc_ecc(page + chunk * 128u, spare + chunk * 3u, 1);
            if (spare[chunk * 3u + 1u] & 0x80u)
                any_bit7 = 1;
        }
        memset(spare + 12, 0, 4);
        if (any_bit7) {
            legacy_hits++;
            CHECK_EQ_U(MciCardMathSpareCompare(page, spare), MCI_ECC_MATCH_LEGACY_DREBIN04);
        } else {
            CHECK_EQ_U(MciCardMathSpareCompare(page, spare), MCI_ECC_MATCH_CANONICAL);
        }

        /* Any other single-bit corruption of the stored ECC is a mismatch. */
        for (j = 0; j < 12u; j++) {
            unsigned bit;
            for (bit = 0; bit < 8u; bit++) {
                unsigned char bad[16];
                memcpy(bad, spare, sizeof(bad));
                bad[j] ^= (unsigned char)(1u << bit);
                if ((j % 3u) == 1u && bit == 7u)
                    continue; /* toggling the legacy bit is by design accepted */
                CHECK_EQ_U(MciCardMathSpareCompare(page, bad), MCI_ECC_MATCH_NONE);
            }
        }
        /* Corrupting data is a mismatch too. */
        page[rng_byte() % 512u] ^= 0x10u;
        CHECK_EQ_U(MciCardMathSpareCompare(page, spare), MCI_ECC_MATCH_NONE);
    }
    /* Sanity: the artefact is common, roughly 15/16 of random pages. */
    CHECK(legacy_hits > 3000u);
}

int main(void)
{
    build_pcsx2_table();
    test_crc32();
    test_ecc_known_vectors();
    test_ecc_matches_oracles();
    test_ecc_legacy_acceptance();
    TEST_MAIN_END("test_card_math");
}
