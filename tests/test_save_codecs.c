/* SPDX-License-Identifier: MIT */
/* Host tests for save_codec_ps2.c, ps1_card.c, lzari.c and save_archive.c
 * against tests/fixtures (see tools/gen_save_fixtures.py). Run from tests/. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_math.h"
#include "ps1_card.h"
#include "save_archive.h"
#include "save_codec_ps2.h"
#include "save_transfer.h"
#include "test_util.h"

typedef struct Expect {
    char name[40];
    u32 size;
    u32 crc;
} Expect;

static char g_ps2dir[40];
static Expect g_ps2[8];
static unsigned g_ps2n;
static Expect g_ps1[4];
static unsigned g_ps1n;

static unsigned char *load(const char *rel, u32 *size)
{
    char path[256];
    FILE *f;
    long n;
    unsigned char *buf;

    snprintf(path, sizeof(path), "fixtures/%s", rel);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "missing fixture %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1u);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n)
        exit(2);
    fclose(f);
    *size = (u32)n;
    return buf;
}

static void load_manifest(void)
{
    u32 size;
    char *text = (char *)load("manifest.txt", &size);
    char *line;

    text[size] = '\0';
    for (line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        Expect e;
        char kind[16];
        if (line[0] == '#')
            continue;
        if (sscanf(line, "ps2dir %39s", g_ps2dir) == 1)
            continue;
        if (sscanf(line, "%15s %39s %u %x", kind, e.name, &e.size, &e.crc) != 4)
            continue;
        if (strcmp(kind, "ps2file") == 0)
            g_ps2[g_ps2n++] = e;
        else if (strcmp(kind, "ps1save") == 0)
            g_ps1[g_ps1n++] = e;
    }
    free(text);
    CHECK_EQ_U(g_ps2n, 4);
    CHECK_EQ_U(g_ps1n, 2);
}

static u32 crc(const unsigned char *d, u32 n)
{
    return MciCardMathCrc32Update(0u, d, n);
}

static void expect_ps2_archive(const MciSaveArchive *a, const char *label)
{
    unsigned i;

    CHECK_EQ_U(a->generation, MCI_SAVE_GEN_PS2);
    if (strcmp(a->directory, g_ps2dir) != 0) {
        g_failures++;
        fprintf(stderr, "%s: dir '%s'\n", label, a->directory);
    }
    CHECK_EQ_U(a->file_count, g_ps2n);
    for (i = 0; i < a->file_count && i < g_ps2n; i++) {
        CHECK(strcmp(a->files[i].name, g_ps2[i].name) == 0);
        CHECK_EQ_U(a->files[i].size, g_ps2[i].size);
        CHECK_EQ_U(crc(a->files[i].data, a->files[i].size), g_ps2[i].crc);
        CHECK(a->files[i].mode & MCI_MC_MODE_FILE);
    }
    CHECK(a->dir_mode & MCI_MC_MODE_DIR);
    CHECK_EQ_U(MciSaveArchiveTotalBytes(a), 964u + 12345u + 3001u);
    CHECK_EQ_U(MciSaveArchiveRequiredClusters(a), 3u + 1u + 13u + 3u);
}

/* Every strict prefix must fail cleanly; random bit flips must never crash
 * (ASan/UBSan make "crash" observable). */
static void fuzz_ps2(const unsigned char *data, u32 size)
{
    unsigned char *copy = malloc(size);
    MciSaveArchive a;
    u32 n;
    unsigned k;

    for (n = 0; n < size; n += (size > 4096u ? 7u : 1u)) {
        int rc = MciPs2Decode(data, n, &a, NULL);
        if (rc == 0) {
            /* A prefix may only succeed if it still describes the same save
             * (PSU/CBS cannot; MAX tolerates trailing padding by design). */
            CHECK_EQ_U(a.file_count, g_ps2n);
            MciSaveArchiveFree(&a);
        }
    }
    for (k = 0; k < 600u; k++) {
        memcpy(copy, data, size);
        copy[(unsigned)(rng_byte() | (rng_byte() << 8) | (rng_byte() << 16)) % size] ^=
            (unsigned char)(1u << (rng_byte() & 7u));
        if (MciPs2Decode(copy, size, &a, NULL) == 0)
            MciSaveArchiveFree(&a);
    }
    free(copy);
}

static void test_ps2(void)
{
    static const char *files[] = {"ps2/save.psu", "ps2/save.max", "ps2/save.cbs",
                                  "ps2/save.sps", "ps2/save.psv"};
    static const MciPs2Container kinds[] = {MCI_PS2_CONTAINER_PSU, MCI_PS2_CONTAINER_MAX,
                                            MCI_PS2_CONTAINER_CBS, MCI_PS2_CONTAINER_SPS,
                                            MCI_PS2_CONTAINER_PSV};
    static const unsigned char tod[8] = {0, 30, 15, 12, 8, 11, 0xEA, 0x07};
    u32 psu_size;
    unsigned char *psu = load("ps2/save.psu", &psu_size);
    unsigned i;

    for (i = 0; i < 5u; i++) {
        u32 size;
        unsigned char *data = load(files[i], &size);
        MciSaveArchive a;
        MciPs2DecodeInfo info;
        int rc;

        CHECK_EQ_U(MciPs2DetectContainer(data, size), kinds[i]);
        rc = MciPs2Decode(data, size, &a, &info);
        if (rc != 0)
            fprintf(stderr, "%s: %s\n", files[i], MciSaveCodecErrorText(rc));
        CHECK_EQ_I(rc, 0);
        if (rc == 0) {
            u32 out_size = MciPs2PsuSize(&a);
            unsigned char *out = malloc(out_size);
            expect_ps2_archive(&a, files[i]);
            if (kinds[i] == MCI_PS2_CONTAINER_MAX) {
                CHECK(info.checksum_present && info.checksum_ok);
                CHECK(!info.timestamps_present);
            } else {
                CHECK(memcmp(&a.created, tod, 8) == 0);
                CHECK(memcmp(&a.files[1].created, tod, 8) == 0);
            }
            /* Any container -> PSU. Timestamped sources reproduce the
             * reference PSU byte for byte. */
            CHECK_EQ_I(MciPs2EncodePsu(&a, out, out_size), (int)psu_size);
            if (kinds[i] != MCI_PS2_CONTAINER_MAX)
                CHECK(out_size == psu_size && memcmp(out, psu, psu_size) == 0);
            free(out);
            MciSaveArchiveFree(&a);
        }
        fuzz_ps2(data, size);
        free(data);
    }

    /* Hostile names: traversal and separators are rejected. */
    {
        unsigned char *bad = malloc(psu_size);
        MciSaveArchive a;
        memcpy(bad, psu, psu_size);
        memcpy(bad + 3 * 512 + 64, "../evil", 8);
        CHECK_EQ_I(MciPs2Decode(bad, psu_size, &a, NULL), MCI_SAVE_E_NAME);
        memcpy(bad, psu, psu_size);
        bad[3 * 512] = 0x27; bad[3 * 512 + 1] = 0x84; /* entry becomes a dir */
        CHECK_EQ_I(MciPs2Decode(bad, psu_size, &a, NULL), MCI_SAVE_E_SUBDIR);
        free(bad);
    }
    free(psu);
    CHECK_EQ_U(MciPs2DetectContainer((const unsigned char *)"nothing", 7), MCI_PS2_CONTAINER_NONE);
}

static void expect_ps1(const MciSaveArchive *a, unsigned idx)
{
    CHECK_EQ_U(a->generation, MCI_SAVE_GEN_PS1);
    CHECK_EQ_U(a->file_count, 1);
    CHECK(strcmp(a->files[0].name, g_ps1[idx].name) == 0);
    CHECK_EQ_U(a->files[0].size, g_ps1[idx].size);
    CHECK_EQ_U(crc(a->files[0].data, a->files[0].size), g_ps1[idx].crc);
}

static void test_ps1_cards(void)
{
    static const char *files[] = {"ps1/card.mcd", "ps1/card.gme", "ps1/card.vgs", "ps1/card.vmp"};
    static const MciPs1Container kinds[] = {MCI_PS1_CONTAINER_RAW, MCI_PS1_CONTAINER_GME,
                                            MCI_PS1_CONTAINER_VGS, MCI_PS1_CONTAINER_VMP};
    MciPs1Card *ref = malloc(sizeof(*ref));
    MciPs1Card *card = malloc(sizeof(*card));
    MciPs1CardSummary sum;
    u32 size;
    unsigned char *raw = load("ps1/card.mcd", &size);
    unsigned i;

    memcpy(ref->image, raw, MCI_PS1_CARD_BYTES);
    free(raw);
    for (i = 0; i < 4u; i++) {
        unsigned char *data = load(files[i], &size);
        MciPs1Container kind;
        CHECK_EQ_I(MciPs1CardLoad(card, data, size, &kind), 0);
        CHECK_EQ_U(kind, kinds[i]);
        CHECK(memcmp(card->image, ref->image, MCI_PS1_CARD_BYTES) == 0);
        free(data);
    }

    CHECK_EQ_I(MciPs1CardScan(ref, &sum), 0);
    CHECK(sum.header_ok);
    CHECK_EQ_U(sum.bad_xor_frames, 0);
    CHECK_EQ_U(sum.broken_chains, 0);
    CHECK_EQ_U(sum.save_count, 2);
    CHECK_EQ_U(sum.free_slots, 12); /* 15 - 3 used (deleted slot is reusable) */
    CHECK(strcmp(sum.saves[0].name, g_ps1[0].name) == 0);
    CHECK_EQ_U(sum.saves[0].block_count, 1);
    CHECK(strcmp(sum.saves[0].title, "DREBIN PS1 ONE") == 0);
    CHECK(strcmp(sum.saves[1].name, g_ps1[1].name) == 0);
    CHECK_EQ_U(sum.saves[1].block_count, 2);
    CHECK_EQ_U(sum.saves[1].slots[0], 1);
    CHECK_EQ_U(sum.saves[1].slots[1], 2);

    for (i = 0; i < 2u; i++) {
        MciSaveArchive a;
        CHECK_EQ_I(MciPs1CardExtract(ref, sum.saves[i].first_slot, &a), 0);
        expect_ps1(&a, i);
        MciSaveArchiveFree(&a);
    }

    /* Write every writable container and load it back. */
    {
        MciPs1Container w[] = {MCI_PS1_CONTAINER_RAW, MCI_PS1_CONTAINER_GME, MCI_PS1_CONTAINER_VGS};
        for (i = 0; i < 3u; i++) {
            u32 n = MciPs1ContainerSize(w[i]);
            unsigned char *out = malloc(n);
            MciPs1Container kind;
            CHECK_EQ_I(MciPs1CardWrite(ref, w[i], out, n), (int)n);
            CHECK_EQ_I(MciPs1CardLoad(card, out, n, &kind), 0);
            CHECK_EQ_U(kind, w[i]);
            CHECK(memcmp(card->image, ref->image, MCI_PS1_CARD_BYTES) == 0);
            if (w[i] == MCI_PS1_CONTAINER_GME) {
                CHECK_EQ_U(out[22 + 1], 0x51);   /* slot 1 state */
                CHECK_EQ_U(out[38 + 1], 2);      /* slot 1 link low byte */
            }
            free(out);
        }
        CHECK_EQ_I(MciPs1CardWrite(ref, MCI_PS1_CONTAINER_VMP, card->image, 1), MCI_SAVE_E_LIMIT);
        CHECK_EQ_I(MciPs1CardWrite(ref, MCI_PS1_CONTAINER_NONE, card->image, 1), MCI_SAVE_E_UNSUPPORTED);
    }

    /* Insert: name clash, slot preference, full card. */
    {
        MciSaveArchive a;
        MciPs1CardSummary s2;
        unsigned char *mcs = load("ps1/save.mcs", &size);
        CHECK_EQ_I(MciPs1DecodeSingle(mcs, size, NULL, &a, NULL), 0);
        free(mcs);
        memcpy(card, ref, sizeof(*card));
        CHECK_EQ_I(MciPs1CardInsert(card, &a), MCI_SAVE_E_EXISTS);
        CHECK(memcmp(card->image, ref->image, MCI_PS1_CARD_BYTES) == 0);

        strcpy(a.files[0].name, "BESLES-55555NEWSAVE");
        CHECK_EQ_I(MciPs1CardInsert(card, &a), 0);
        CHECK_EQ_I(MciPs1CardScan(card, &s2), 0);
        CHECK_EQ_U(s2.save_count, 3);
        CHECK_EQ_U(s2.bad_xor_frames, 0);
        CHECK_EQ_U(s2.saves[2].slots[0], 3); /* free slots before deleted 5 */
        CHECK_EQ_U(s2.saves[2].slots[1], 4);
        CHECK_EQ_U(card->image[128 * 6], 0xA1); /* deleted save untouched */
        {
            MciSaveArchive back;
            CHECK_EQ_I(MciPs1CardExtract(card, 3, &back), 0);
            CHECK(back.files[0].size == a.files[0].size &&
                  memcmp(back.files[0].data, a.files[0].data, a.files[0].size) == 0);
            MciSaveArchiveFree(&back);
        }
        /* Fill the card: 10 free slots left (12 - 2), 5 saves of 2 blocks. */
        for (i = 0; i < 5u; i++) {
            snprintf(a.files[0].name, sizeof(a.files[0].name), "BESLES-6666%uFILL", i);
            CHECK_EQ_I(MciPs1CardInsert(card, &a), 0);
        }
        snprintf(a.files[0].name, sizeof(a.files[0].name), "BESLES-77770OVER");
        memcpy(ref, card, sizeof(*card));
        CHECK_EQ_I(MciPs1CardInsert(card, &a), MCI_SAVE_E_FULL);
        CHECK(memcmp(card->image, ref->image, MCI_PS1_CARD_BYTES) == 0);
        MciSaveArchiveFree(&a);
    }

    /* Fresh format + corruption detection. */
    MciPs1CardFormat(card);
    CHECK_EQ_I(MciPs1CardScan(card, &sum), 0);
    CHECK_EQ_U(sum.save_count, 0);
    CHECK_EQ_U(sum.free_slots, 15);
    CHECK_EQ_U(sum.bad_xor_frames, 0);
    card->image[128 * 3 + 20] ^= 1;
    MciPs1CardScan(card, &sum);
    CHECK_EQ_U(sum.bad_xor_frames, 1);

    raw = load("ps1/card.mcd", &size);
    memcpy(card->image, raw, MCI_PS1_CARD_BYTES);
    free(raw);
    card->image[128 * 3 + 8] = 1;   /* slot 2 (LAST) now links back to slot 1 */
    card->image[128 * 3 + 9] = 0;
    MciPs1CardScan(card, &sum);
    CHECK_EQ_U(sum.save_count, 1);
    CHECK(sum.broken_chains >= 1);

    free(ref);
    free(card);
}

static void test_ps1_singles(void)
{
    static const char *files[] = {"ps1/save.mcs", "ps1/save.psx",
                                  "ps1/BASLUS-00001RPGDATA.bin", "ps1/save1.psv"};
    static const MciPs1SingleKind kinds[] = {MCI_PS1_SINGLE_MCS, MCI_PS1_SINGLE_AR,
                                             MCI_PS1_SINGLE_RAW, MCI_PS1_SINGLE_PSV};
    unsigned i;
    u32 mcs_size;
    unsigned char *mcs = load("ps1/save.mcs", &mcs_size);

    for (i = 0; i < 4u; i++) {
        u32 size;
        unsigned char *data = load(files[i], &size);
        MciSaveArchive a;
        MciPs1SingleKind kind;
        int rc = MciPs1DecodeSingle(data, size, "usb:/SAVES/BASLUS-00001RPGDATA.bin", &a, &kind);
        CHECK_EQ_I(rc, 0);
        CHECK_EQ_U(kind, kinds[i]);
        if (rc == 0) {
            u32 n = MciPs1McsSize(&a);
            unsigned char *out = malloc(n);
            expect_ps1(&a, 1);
            CHECK(strcmp(a.title, "RPG DATA 2") == 0);
            CHECK_EQ_I(MciPs1EncodeMcs(&a, out, n), (int)mcs_size);
            CHECK(memcmp(out, mcs, mcs_size) == 0);
            free(out);
            MciSaveArchiveFree(&a);
        }
        /* Truncations never succeed or crash. */
        {
            u32 cut;
            for (cut = 0; cut < size; cut += 97u) {
                if (MciPs1DecodeSingle(data, cut, "x", &a, NULL) == 0)
                    MciSaveArchiveFree(&a);
            }
        }
        free(data);
    }
    free(mcs);

    /* PS2 containers are not mistaken for PS1 singles and vice versa. */
    {
        u32 size;
        unsigned char *data = load("ps2/save.psv", &size);
        CHECK_EQ_U(MciPs1DetectSingle(data, size), MCI_PS1_SINGLE_NONE);
        free(data);
        data = load("ps1/save1.psv", &size);
        CHECK_EQ_U(MciPs2DetectContainer(data, size), MCI_PS2_CONTAINER_NONE);
        free(data);
    }
}

static void test_detect(void)
{
    static const struct {
        const char *file;
        const char *as_name; /* deliberately misleading or missing extension */
        MciSaveTransferFormat want;
        int gen;
    } cases[] = {
        {"ps2/save.psu", "usb:/x/save.bin", MCI_SAVE_FORMAT_PSU, 2},
        {"ps2/save.max", "usb:/x/save.max", MCI_SAVE_FORMAT_MAX, 2},
        {"ps2/save.max", "usb:/x/save.pws", MCI_SAVE_FORMAT_PWS, 2},
        {"ps2/save.cbs", "usb:/x/SAVE", MCI_SAVE_FORMAT_CBS, 2},
        {"ps2/save.sps", "usb:/x/save.sps", MCI_SAVE_FORMAT_SPS, 2},
        {"ps2/save.sps", "usb:/x/save.xps", MCI_SAVE_FORMAT_XPS, 2},
        {"ps2/save.psv", "usb:/x/save.psv", MCI_SAVE_FORMAT_PSV_PS2, 2},
        {"ps1/save1.psv", "usb:/x/save.psv", MCI_SAVE_FORMAT_PSV_PS1, 1},
        {"ps1/card.mcd", "usb:/POPS/SLES_123.45.VMC", MCI_SAVE_FORMAT_IMAGE_PS1, 1},
        {"ps1/card.gme", "usb:/x/card.dex", MCI_SAVE_FORMAT_GME_PS1, 1},
        {"ps1/card.vgs", "usb:/x/card.mem", MCI_SAVE_FORMAT_IMAGE_PS1, 1},
        {"ps1/card.vmp", "usb:/x/card.vmp", MCI_SAVE_FORMAT_IMAGE_PS1, 1},
        {"ps1/save.mcs", "usb:/x/save.ps1", MCI_SAVE_FORMAT_MCS_PS1, 1},
        {"ps1/save.psx", "usb:/x/save.mcb", MCI_SAVE_FORMAT_PSX_PS1, 1},
        {"ps1/BASLUS-00001RPGDATA.bin", "usb:/x/BASLUS-00001RPGDATA", MCI_SAVE_FORMAT_RAW_PS1, 1},
    };
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        u32 size;
        unsigned char *data = load(cases[i].file, &size);
        int gen = -1;
        /* The console probe only reads the first 4 KiB. */
        MciSaveTransferFormat got = MciSaveTransferDetectBuffer(
            data, size < 4096u ? size : 4096u, size, cases[i].as_name, &gen);
        if (got != cases[i].want || gen != cases[i].gen) {
            g_failures++;
            fprintf(stderr, "detect %s as %s: got %d gen %d\n", cases[i].file,
                    cases[i].as_name, (int)got, gen);
        }
        g_checks++;
        CHECK(MciSaveTransferFormatImportable(got));
        free(data);
    }
    {
        const unsigned char junk[64] = {0};
        int gen = -1;
        CHECK_EQ_U(MciSaveTransferDetectBuffer(junk, 64, 64, "usb:/x.bin", &gen),
                   MCI_SAVE_FORMAT_UNKNOWN);
        CHECK_EQ_U(MciSaveTransferDetectBuffer(junk, 64, 8u << 20, "usb:/card.ps2", &gen),
                   MCI_SAVE_FORMAT_IMAGE_PS2);
        CHECK(!MciSaveTransferFormatImportable(MCI_SAVE_FORMAT_IMAGE_PS2));
    }
}

int main(void)
{
    load_manifest();
    test_detect();
    test_ps2();
    test_ps1_cards();
    test_ps1_singles();
    TEST_MAIN_END("test_save_codecs");
}
