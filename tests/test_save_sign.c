/* SPDX-License-Identifier: MIT */
/* Signed/encrypted containers vs. MemcardRex and Apollo output
 * (tests/fixtures/oracle). Run from tests/. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps1_card.h"
#include "save_codec_ps2.h"
#include "save_sign.h"
#include "save_transfer.h"
#include "test_util.h"

static unsigned char *load(const char *rel, u32 *size)
{
    char path[256];
    FILE *f;
    long n;
    unsigned char *b;
    snprintf(path, sizeof(path), "fixtures/%s", rel);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *size = (u32)n;
    return b;
}

static void expect_same(const unsigned char *a, u32 an, const unsigned char *b, u32 bn,
                        const char *what)
{
    g_checks++;
    if (an != bn || memcmp(a, b, an) != 0) {
        u32 i = 0;
        while (i < an && i < bn && a[i] == b[i]) i++;
        g_failures++;
        fprintf(stderr, "%s differs (sizes %u/%u, first diff @0x%x)\n", what, an, bn, i);
    }
}

static void test_ps1(void)
{
    u32 n, on;
    unsigned char *raw = load("ps1/card.mcd", &n);
    MciPs1Card *card = malloc(sizeof(*card));
    MciSaveArchive a;
    unsigned char *out = malloc(0x30000);
    unsigned char *oracle;
    MciPs1Container kind;
    int rc;

    memcpy(card->image, raw, MCI_PS1_CARD_BYTES);

    /* VMP: verify oracle, reproduce byte-for-byte, detect tampering. */
    oracle = load("oracle/card.vmp", &on);
    CHECK_EQ_U(MciVmpVerify(oracle, on), MCI_SIG_OK);
    rc = MciPs1CardWrite(card, MCI_PS1_CONTAINER_VMP, out, 0x30000);
    CHECK_EQ_I(rc, (int)MCI_VMP_BYTES);
    expect_same(out, (u32)rc, oracle, on, "VMP vs MemcardRex");
    CHECK_EQ_I(MciPs1CardLoad(card, oracle, on, &kind), 0);
    CHECK_EQ_U(kind, MCI_PS1_CONTAINER_VMP);
    CHECK_EQ_I(MciPs1LastIntegrity(), MCI_SIG_OK);
    oracle[0x5000] ^= 1;
    CHECK_EQ_U(MciVmpVerify(oracle, on), MCI_SIG_BAD);
    free(oracle);

    /* MCX: decrypt oracle to our card, encrypt to the identical file. */
    oracle = load("oracle/card.mcx", &on);
    CHECK(MciMcxLooksValid(oracle, on));
    CHECK_EQ_U(MciPs1DetectCardContainer(oracle, on, NULL), MCI_PS1_CONTAINER_MCX);
    CHECK_EQ_I(MciPs1CardLoad(card, oracle, on, &kind), 0);
    CHECK_EQ_U(kind, MCI_PS1_CONTAINER_MCX);
    CHECK_EQ_I(MciPs1LastIntegrity(), MCI_SIG_OK);
    CHECK(memcmp(card->image, raw, MCI_PS1_CARD_BYTES) == 0);
    rc = MciPs1CardWrite(card, MCI_PS1_CONTAINER_MCX, out, 0x30000);
    expect_same(out, (u32)rc, oracle, on, "MCX vs MemcardRex");
    {
        int gen = 0;
        CHECK_EQ_U(MciSaveTransferDetectBuffer(oracle, 4096, on, "usb:/x.mcx", &gen),
                   MCI_SAVE_FORMAT_IMAGE_PS1);
        CHECK_EQ_I(gen, 1);
    }
    oracle[0x10000] ^= 0x40; /* corrupt ciphertext: SHA-256 must catch it */
    CHECK_EQ_I(MciPs1CardLoad(card, oracle, on, NULL), 0);
    CHECK_EQ_I(MciPs1LastIntegrity(), MCI_SIG_BAD);
    free(oracle);
    memcpy(card->image, raw, MCI_PS1_CARD_BYTES);

    /* PS1 PSV: identical to MemcardRex's export of slot 1. */
    CHECK_EQ_I(MciPs1CardExtract(card, 1, &a), 0);
    oracle = load("oracle/ps1_save1.memcardrex.psv", &on);
    CHECK_EQ_U(MciPsvVerify(oracle, on), MCI_SIG_OK);
    rc = MciPs1EncodePsv(&a, out, 0x30000);
    CHECK_EQ_I(rc, (int)on);
    expect_same(out, (u32)rc, oracle, on, "PS1 PSV vs MemcardRex");
    {
        MciSaveArchive back;
        CHECK_EQ_I(MciPs1DecodeSingle(oracle, on, NULL, &back, NULL), 0);
        CHECK(back.files[0].size == a.files[0].size &&
              memcmp(back.files[0].data, a.files[0].data, a.files[0].size) == 0);
        CHECK(strcmp(back.files[0].name, a.files[0].name) == 0);
        MciSaveArchiveFree(&back);
    }
    free(oracle);

    /* The generator's PSV is deliberately unsigned. */
    oracle = load("ps1/save1.psv", &on);
    CHECK_EQ_U(MciPsvVerify(oracle, on), MCI_SIG_UNSIGNED);
    free(oracle);

    /* Action Replay writer round-trips through the decoder. */
    rc = MciPs1EncodeAr(&a, out, 0x30000);
    CHECK_EQ_I(rc, (int)(54u + a.files[0].size));
    {
        MciSaveArchive back;
        MciPs1SingleKind k;
        CHECK_EQ_I(MciPs1DecodeSingle(out, (u32)rc, NULL, &back, &k), 0);
        CHECK_EQ_U(k, MCI_PS1_SINGLE_AR);
        CHECK(strcmp(back.files[0].name, a.files[0].name) == 0);
        CHECK(memcmp(back.files[0].data, a.files[0].data, a.files[0].size) == 0);
        MciSaveArchiveFree(&back);
    }
    MciSaveArchiveFree(&a);
    free(raw);
    free(card);
    free(out);
}

static void test_ps2(void)
{
    u32 n, on;
    unsigned char *psu = load("ps2/save.psu", &n);
    unsigned char *oracle = load("oracle/ps2_save.apollo.psv", &on);
    unsigned char *out;
    MciSaveArchive a, back;
    int rc;
    u32 i;

    CHECK_EQ_I(MciPs2Decode(psu, n, &a, NULL), 0);
    out = malloc(MciPs2PsvSize(&a));
    rc = MciPs2EncodePsv(&a, out, MciPs2PsvSize(&a));
    CHECK_EQ_I(rc, (int)on);
    expect_same(out, (u32)rc, oracle, on, "PS2 PSV vs Apollo signature");
    CHECK_EQ_U(MciPsvVerify(oracle, on), MCI_SIG_OK);
    /* icon.sys located in the PS2 header (sysPos / sysSize). */
    CHECK_EQ_U(MciLe32(out + 0x48), 964);
    CHECK_EQ_U(MciLe32(out + 0x40), 964u + 12345u + 3001u);

    CHECK_EQ_I(MciPs2Decode(out, (u32)rc, &back, NULL), 0);
    CHECK_EQ_U(back.file_count, a.file_count);
    for (i = 0; i < a.file_count; i++) {
        CHECK(strcmp(back.files[i].name, a.files[i].name) == 0);
        CHECK(back.files[i].size == a.files[i].size &&
              memcmp(back.files[i].data, a.files[i].data, a.files[i].size) == 0);
        CHECK_EQ_U(back.files[i].mode, a.files[i].mode);
    }
    CHECK(memcmp(&back.created, &a.created, 8) == 0);

    out[0x200] ^= 1;
    CHECK_EQ_U(MciPsvVerify(out, (u32)rc), MCI_SIG_BAD);
    memset(out + MCI_PSV_SIG_OFFSET, 0, 20);
    CHECK_EQ_U(MciPsvVerify(out, (u32)rc), MCI_SIG_UNSIGNED);
    CHECK_EQ_I(MciPsvSign(out, 10), -1);

    MciSaveArchiveFree(&back);
    MciSaveArchiveFree(&a);
    free(out);
    free(psu);
    free(oracle);
}

int main(void)
{
    test_ps1();
    test_ps2();
    TEST_MAIN_END("test_save_sign");
}
