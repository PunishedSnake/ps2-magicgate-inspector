/* SPDX-License-Identifier: MIT */
/* PS1 memory-card filesystem and container codecs. See ps1_card.h. */

#include <tamtypes.h>
#include <stdlib.h>
#include <string.h>

#include "ps1_card.h"
#include "save_sign.h"
#include "save_title.h"

#define DIR_FRAME(card, slot) ((card)->image + MCI_PS1_FRAME_BYTES * (1u + (slot)))
#define BLOCK_DATA(card, slot) ((card)->image + MCI_PS1_BLOCK_BYTES * (1u + (slot)))

static const char GmeMagic[11] = {'1', '2', '3', '-', '4', '5', '6', '-', 'S', 'T', 'D'};

unsigned char MciPs1FrameXor(const unsigned char frame[MCI_PS1_FRAME_BYTES])
{
    unsigned char x = 0u;
    unsigned int i;

    for (i = 0u; i < MCI_PS1_FRAME_BYTES - 1u; i++)
        x ^= frame[i];
    return x;
}

static void SetFrameXor(unsigned char *frame)
{
    frame[MCI_PS1_FRAME_BYTES - 1u] = MciPs1FrameXor(frame);
}

const char *MciPs1ContainerName(MciPs1Container kind)
{
    switch (kind) {
        case MCI_PS1_CONTAINER_RAW: return "PS1 raw card (.mcd/.mcr)";
        case MCI_PS1_CONTAINER_GME: return "DexDrive card (.gme)";
        case MCI_PS1_CONTAINER_VGS: return "VGS card (.mem/.vgs)";
        case MCI_PS1_CONTAINER_VMP: return "PSP card (.vmp)";
        case MCI_PS1_CONTAINER_MCX: return "MCX encrypted card (.mcx)";
        default: return "none";
    }
}

MciPs1Container MciPs1DetectCardContainer(const unsigned char *data, u32 size,
                                          u32 *offset)
{
    u32 off = 0u;
    MciPs1Container kind = MCI_PS1_CONTAINER_NONE;

    if (data == NULL)
        return MCI_PS1_CONTAINER_NONE;
    if (size == MCI_MCX_BYTES && MciMcxLooksValid(data, size)) {
        if (offset != NULL)
            *offset = 0x80u;
        return MCI_PS1_CONTAINER_MCX; /* image is encrypted; Load decrypts */
    }
    if (size >= MCI_GME_HEADER_BYTES + MCI_PS1_CARD_BYTES &&
        memcmp(data, GmeMagic, sizeof(GmeMagic)) == 0) {
        kind = MCI_PS1_CONTAINER_GME;
        off = MCI_GME_HEADER_BYTES;
    } else if (size >= MCI_VGS_HEADER_BYTES + MCI_PS1_CARD_BYTES &&
               memcmp(data, "VgsM", 4) == 0) {
        kind = MCI_PS1_CONTAINER_VGS;
        off = MCI_VGS_HEADER_BYTES;
    } else if (size >= MCI_VMP_HEADER_BYTES + MCI_PS1_CARD_BYTES &&
               memcmp(data, "\0PMV", 4) == 0) {
        kind = MCI_PS1_CONTAINER_VMP;
        off = MCI_VMP_HEADER_BYTES;
    } else if (size == MCI_PS1_CARD_BYTES && data[0] == 'M' && data[1] == 'C') {
        kind = MCI_PS1_CONTAINER_RAW;
        off = 0u;
    } else if (size == MCI_GME_HEADER_BYTES + MCI_PS1_CARD_BYTES &&
               data[MCI_GME_HEADER_BYTES] == 'M' &&
               data[MCI_GME_HEADER_BYTES + 1u] == 'C') {
        /* DexDrive files written by some tools omit the signature. */
        kind = MCI_PS1_CONTAINER_GME;
        off = MCI_GME_HEADER_BYTES;
    }
    if (kind != MCI_PS1_CONTAINER_NONE &&
        (data[off] != 'M' || data[off + 1u] != 'C'))
        kind = MCI_PS1_CONTAINER_NONE;
    if (offset != NULL)
        *offset = off;
    return kind;
}

void MciPs1CardFormat(MciPs1Card *card)
{
    unsigned int i;

    memset(card->image, 0, sizeof(card->image));
    card->image[0] = 'M';
    card->image[1] = 'C';
    SetFrameXor(card->image);
    for (i = 0u; i < MCI_PS1_SLOTS; i++) {
        unsigned char *f = DIR_FRAME(card, i);
        f[0] = MCI_PS1_STATE_FREE;
        f[8] = 0xFFu;
        f[9] = 0xFFu;
        SetFrameXor(f);
    }
    /* Broken sector list (frames 16..35): no entries. */
    for (i = 0u; i < 20u; i++) {
        unsigned char *f = card->image + MCI_PS1_FRAME_BYTES * (16u + i);
        memset(f, 0xFF, 4u);
        f[8] = 0xFFu;
        f[9] = 0xFFu;
        SetFrameXor(f);
    }
    /* Write-test frame 63 mirrors the header. */
    memcpy(card->image + MCI_PS1_FRAME_BYTES * 63u, card->image, MCI_PS1_FRAME_BYTES);
}

static int LastIntegrity = MCI_SIG_UNKNOWN;

int MciPs1LastIntegrity(void)
{
    return LastIntegrity;
}

int MciPs1CardLoad(MciPs1Card *card, const unsigned char *data, u32 size,
                   MciPs1Container *kind_out)
{
    u32 offset = 0u;
    MciPs1Container kind;

    LastIntegrity = MCI_SIG_UNKNOWN;
    if (card == NULL || data == NULL)
        return MCI_SAVE_E_ARGS;
    kind = MciPs1DetectCardContainer(data, size, &offset);
    if (kind_out != NULL)
        *kind_out = kind;
    if (kind == MCI_PS1_CONTAINER_NONE)
        return MCI_SAVE_E_FORMAT;
    if (kind == MCI_PS1_CONTAINER_MCX) {
        int rc = MciMcxDecrypt(data, size, card->image);
        if (rc < 0)
            return MCI_SAVE_E_CORRUPT;
        LastIntegrity = rc ? MCI_SIG_OK : MCI_SIG_BAD;
        return MCI_SAVE_OK;
    }
    if (kind == MCI_PS1_CONTAINER_VMP && size == MCI_VMP_BYTES)
        LastIntegrity = MciVmpVerify(data, size);
    memcpy(card->image, data + offset, MCI_PS1_CARD_BYTES);
    return MCI_SAVE_OK;
}

static int IsFirstState(unsigned char s) { return s == MCI_PS1_STATE_FIRST; }
static int IsLinkState(unsigned char s) { return s == MCI_PS1_STATE_MIDDLE || s == MCI_PS1_STATE_LAST; }
static int IsReusable(unsigned char s)
{
    return s == MCI_PS1_STATE_FREE || s == MCI_PS1_STATE_DEL_FIRST ||
           s == MCI_PS1_STATE_DEL_MIDDLE || s == MCI_PS1_STATE_DEL_LAST;
}

static void CopyPs1Name(char out[MCI_PS1_SAVE_NAME_MAX + 1u], const unsigned char *frame)
{
    unsigned int i;

    for (i = 0u; i < MCI_PS1_SAVE_NAME_MAX; i++) {
        unsigned char c = frame[0x0Au + i];
        if (c == 0u)
            break;
        out[i] = (char)c;
    }
    out[i] = '\0';
}

/* Follows a chain from a first-block slot. Returns block count or negative. */
static int FollowChain(const MciPs1Card *card, unsigned int first,
                       unsigned int slots[MCI_PS1_SLOTS])
{
    unsigned int seen = 0u;
    unsigned int count = 0u;
    unsigned int slot = first;

    for (;;) {
        const unsigned char *f = DIR_FRAME(card, slot);
        u16 next;

        if (seen & (1u << slot))
            return MCI_SAVE_E_CORRUPT; /* loop */
        seen |= 1u << slot;
        if (count >= MCI_PS1_SLOTS)
            return MCI_SAVE_E_CORRUPT;
        if (count == 0u ? !IsFirstState(f[0]) : !IsLinkState(f[0]))
            return MCI_SAVE_E_CORRUPT;
        slots[count++] = slot;
        next = MciLe16(f + 8u);
        if (next == MCI_PS1_LINK_END) {
            /* A multi-block chain must end on a LAST frame. */
            if (count > 1u && f[0] != MCI_PS1_STATE_LAST)
                return MCI_SAVE_E_CORRUPT;
            return (int)count;
        }
        if (next >= MCI_PS1_SLOTS || f[0] == MCI_PS1_STATE_LAST)
            return MCI_SAVE_E_CORRUPT;
        slot = next;
    }
}

int MciPs1CardScan(const MciPs1Card *card, MciPs1CardSummary *summary)
{
    unsigned int used = 0u;
    unsigned int slot;

    if (card == NULL || summary == NULL)
        return MCI_SAVE_E_ARGS;
    memset(summary, 0, sizeof(*summary));
    summary->header_ok = card->image[0] == 'M' && card->image[1] == 'C';
    if (!summary->header_ok)
        return MCI_SAVE_E_FORMAT;

    for (slot = 0u; slot < MCI_PS1_SLOTS; slot++) {
        const unsigned char *f = DIR_FRAME(card, slot);
        if (MciPs1FrameXor(f) != f[MCI_PS1_FRAME_BYTES - 1u])
            summary->bad_xor_frames++;
        if (IsReusable(f[0]))
            summary->free_slots++;
    }

    for (slot = 0u; slot < MCI_PS1_SLOTS; slot++) {
        const unsigned char *f = DIR_FRAME(card, slot);
        MciPs1SaveInfo *info;
        const unsigned char *sc;
        int n;
        unsigned int i;

        if (!IsFirstState(f[0]))
            continue;
        info = &summary->saves[summary->save_count];
        n = FollowChain(card, slot, info->slots);
        if (n < 0) {
            summary->broken_chains++;
            continue;
        }
        for (i = 0u; i < (unsigned int)n; i++)
            used |= 1u << info->slots[i];
        info->first_slot = slot;
        info->block_count = (unsigned int)n;
        info->size = MciLe32(f + 4u);
        CopyPs1Name(info->name, f);
        sc = BLOCK_DATA(card, slot);
        if (sc[0] == 'S' && sc[1] == 'C')
            (void)MciSjisFoldToAscii(sc + 4u, 64u, 0xFFFFFFFFu,
                                     info->title, sizeof(info->title));
        summary->save_count++;
    }

    /* Link frames not reachable from any first block are orphans. */
    for (slot = 0u; slot < MCI_PS1_SLOTS; slot++) {
        const unsigned char *f = DIR_FRAME(card, slot);
        if (IsLinkState(f[0]) && (used & (1u << slot)) == 0u)
            summary->broken_chains++;
    }
    return MCI_SAVE_OK;
}

int MciPs1ArchiveValidate(const MciSaveArchive *archive)
{
    const MciSaveFile *f;

    if (archive == NULL || archive->generation != MCI_SAVE_GEN_PS1 ||
        archive->file_count != 1u)
        return MCI_SAVE_E_ARGS;
    f = &archive->files[0];
    if (!MciPs1NameIsSafe(f->name))
        return MCI_SAVE_E_NAME;
    if (f->size == 0u || (f->size % MCI_PS1_BLOCK_BYTES) != 0u ||
        f->size > MCI_PS1_BLOCK_BYTES * MCI_PS1_MAX_BLOCKS)
        return MCI_SAVE_E_LIMIT;
    if (f->data == NULL || f->data[0] != 'S' || (f->data[1] != 'C' && f->data[1] != 'c'))
        return MCI_SAVE_E_CORRUPT;
    return MCI_SAVE_OK;
}

static int MakePs1Archive(MciSaveArchive *archive, const char *name,
                          const unsigned char *body, u32 size)
{
    MciSaveFile *f;

    MciSaveArchiveInit(archive);
    archive->storage = (unsigned char *)malloc(size ? size : 1u);
    if (archive->storage == NULL)
        return MCI_SAVE_E_NOMEM;
    memcpy(archive->storage, body, size);
    archive->storage_size = size;
    archive->generation = MCI_SAVE_GEN_PS1;
    archive->file_count = 1u;
    f = &archive->files[0];
    {
        size_t n = strlen(name);
        if (n > MCI_PS1_SAVE_NAME_MAX)
            n = MCI_PS1_SAVE_NAME_MAX;
        memcpy(f->name, name, n);
        f->name[n] = '\0';
    }
    memcpy(archive->directory, f->name, strlen(f->name) + 1u);
    f->size = size;
    f->data = archive->storage;
    if (size >= 68u && body[0] == 'S')
        (void)MciSjisFoldToAscii(body + 4u, 64u, 0xFFFFFFFFu,
                                 archive->title, sizeof(archive->title));
    {
        int rc = MciPs1ArchiveValidate(archive);
        if (rc < 0) {
            MciSaveArchiveFree(archive);
            return rc;
        }
    }
    return MCI_SAVE_OK;
}

int MciPs1CardExtract(const MciPs1Card *card, unsigned int first_slot,
                      MciSaveArchive *archive)
{
    unsigned int slots[MCI_PS1_SLOTS];
    char name[MCI_PS1_SAVE_NAME_MAX + 1u];
    unsigned char *body;
    int n;
    int rc;
    unsigned int i;

    if (card == NULL || archive == NULL || first_slot >= MCI_PS1_SLOTS)
        return MCI_SAVE_E_ARGS;
    n = FollowChain(card, first_slot, slots);
    if (n < 0)
        return n;
    body = (unsigned char *)malloc((u32)n * MCI_PS1_BLOCK_BYTES);
    if (body == NULL)
        return MCI_SAVE_E_NOMEM;
    for (i = 0u; i < (unsigned int)n; i++)
        memcpy(body + i * MCI_PS1_BLOCK_BYTES, BLOCK_DATA(card, slots[i]),
               MCI_PS1_BLOCK_BYTES);
    CopyPs1Name(name, DIR_FRAME(card, first_slot));
    rc = MakePs1Archive(archive, name, body, (u32)n * MCI_PS1_BLOCK_BYTES);
    free(body);
    return rc;
}

int MciPs1CardInsert(MciPs1Card *card, const MciSaveArchive *archive)
{
    MciPs1CardSummary summary;
    unsigned int need;
    unsigned int pick[MCI_PS1_SLOTS];
    unsigned int picked = 0u;
    unsigned int pass;
    unsigned int slot;
    unsigned int i;
    int rc;

    if (card == NULL || archive == NULL)
        return MCI_SAVE_E_ARGS;
    rc = MciPs1ArchiveValidate(archive);
    if (rc < 0)
        return rc;
    rc = MciPs1CardScan(card, &summary);
    if (rc < 0)
        return rc;
    for (i = 0u; i < summary.save_count; i++)
        if (strcmp(summary.saves[i].name, archive->files[0].name) == 0)
            return MCI_SAVE_E_EXISTS;

    need = archive->files[0].size / MCI_PS1_BLOCK_BYTES;
    /* Prefer never-used slots so deleted saves stay recoverable. */
    for (pass = 0u; pass < 2u && picked < need; pass++) {
        for (slot = 0u; slot < MCI_PS1_SLOTS && picked < need; slot++) {
            unsigned char s = DIR_FRAME(card, slot)[0];
            if (pass == 0u ? s == MCI_PS1_STATE_FREE
                           : (IsReusable(s) && s != MCI_PS1_STATE_FREE))
                pick[picked++] = slot;
        }
    }
    if (picked < need)
        return MCI_SAVE_E_FULL;
    /* Keep chain order ascending, as the PS1 BIOS allocates. */
    for (i = 1u; i < need; i++) {
        unsigned int key = pick[i];
        unsigned int j = i;
        while (j > 0u && pick[j - 1u] > key) {
            pick[j] = pick[j - 1u];
            j--;
        }
        pick[j] = key;
    }

    for (i = 0u; i < need; i++) {
        unsigned char *f = DIR_FRAME(card, pick[i]);
        memset(f, 0, MCI_PS1_FRAME_BYTES);
        if (i == 0u) {
            f[0] = MCI_PS1_STATE_FIRST;
            MciPutLe32(f + 4u, archive->files[0].size);
            memcpy(f + 0x0Au, archive->files[0].name, strlen(archive->files[0].name));
        } else {
            f[0] = (i + 1u == need) ? MCI_PS1_STATE_LAST : MCI_PS1_STATE_MIDDLE;
        }
        MciPutLe16(f + 8u, (u16)(i + 1u < need ? pick[i + 1u] : MCI_PS1_LINK_END));
        SetFrameXor(f);
        memcpy(BLOCK_DATA(card, pick[i]),
               archive->files[0].data + i * MCI_PS1_BLOCK_BYTES,
               MCI_PS1_BLOCK_BYTES);
    }
    return MCI_SAVE_OK;
}

u32 MciPs1ContainerSize(MciPs1Container kind)
{
    switch (kind) {
        case MCI_PS1_CONTAINER_RAW: return MCI_PS1_CARD_BYTES;
        case MCI_PS1_CONTAINER_GME: return MCI_GME_HEADER_BYTES + MCI_PS1_CARD_BYTES;
        case MCI_PS1_CONTAINER_VGS: return MCI_VGS_HEADER_BYTES + MCI_PS1_CARD_BYTES;
        case MCI_PS1_CONTAINER_VMP: return MCI_VMP_BYTES;
        case MCI_PS1_CONTAINER_MCX: return MCI_MCX_BYTES;
        default: return 0u;
    }
}

int MciPs1CardWrite(const MciPs1Card *card, MciPs1Container kind,
                    unsigned char *out, u32 out_size)
{
    u32 need = MciPs1ContainerSize(kind);
    unsigned char *img;
    unsigned int slot;

    if (card == NULL || out == NULL)
        return MCI_SAVE_E_ARGS;
    if (need == 0u)
        return MCI_SAVE_E_UNSUPPORTED;
    if (out_size < need)
        return MCI_SAVE_E_LIMIT;
    if (kind == MCI_PS1_CONTAINER_MCX)
        return MciMcxEncrypt(card->image, out) < 0 ? MCI_SAVE_E_ARGS : (int)need;
    memset(out, 0, need);
    img = out + (need - MCI_PS1_CARD_BYTES);
    memcpy(img, card->image, MCI_PS1_CARD_BYTES);

    if (kind == MCI_PS1_CONTAINER_GME) {
        memcpy(out, GmeMagic, sizeof(GmeMagic));
        out[18] = 0x01u;
        out[20] = 0x01u;
        out[21] = 'M';
        for (slot = 0u; slot < MCI_PS1_SLOTS; slot++) {
            const unsigned char *f = DIR_FRAME(card, slot);
            out[22u + slot] = f[0];
            out[38u + slot] = f[8];
        }
        /* 256-byte per-slot comment area at 64 + 256 * slot stays empty. */
    } else if (kind == MCI_PS1_CONTAINER_VGS) {
        memcpy(out, "VgsM", 4u);
        out[4] = 0x01u;
        out[8] = 0x01u;
        out[12] = 0x01u;
        out[17] = 0x02u;
    } else if (kind == MCI_PS1_CONTAINER_VMP) {
        memcpy(out, "\0PMV", 4u);
        out[4] = 0x80u; /* header length */
        if (MciVmpSign(out, need) < 0)
            return MCI_SAVE_E_ARGS;
    }
    return (int)need;
}

/* ---------------------------------------------------------------- singles */

const char *MciPs1SingleName(MciPs1SingleKind kind)
{
    switch (kind) {
        case MCI_PS1_SINGLE_MCS: return "PS1 MCS single save";
        case MCI_PS1_SINGLE_AR: return "PS1 Action Replay/GameShark/Xploder save";
        case MCI_PS1_SINGLE_RAW: return "PS1 raw single save";
        case MCI_PS1_SINGLE_PSV: return "PS3 PS1 save (.psv)";
        default: return "none";
    }
}

static int BodyOk(const unsigned char *body, u32 size)
{
    return size > 0u && (size % MCI_PS1_BLOCK_BYTES) == 0u &&
           size <= MCI_PS1_BLOCK_BYTES * MCI_PS1_MAX_BLOCKS &&
           body[0] == 'S' && (body[1] == 'C' || body[1] == 'c');
}

MciPs1SingleKind MciPs1DetectSingle(const unsigned char *data, u32 size)
{
    if (data == NULL || size < 2u)
        return MCI_PS1_SINGLE_NONE;
    if (size >= 0x84u && memcmp(data, "\0VSP", 4) == 0 && MciLe32(data + 0x3Cu) == 1u)
        return MCI_PS1_SINGLE_PSV;
    if (size > 128u && data[0] == MCI_PS1_STATE_FIRST &&
        BodyOk(data + 128u, size - 128u))
        return MCI_PS1_SINGLE_MCS;
    if (BodyOk(data, size))
        return MCI_PS1_SINGLE_RAW;
    if (size > 54u && BodyOk(data + 54u, size - 54u))
        return MCI_PS1_SINGLE_AR;
    return MCI_PS1_SINGLE_NONE;
}

static void NameFromFixed(char out[MCI_PS1_SAVE_NAME_MAX + 1u],
                          const unsigned char *p, unsigned int max)
{
    unsigned int i;

    for (i = 0u; i < max && i < MCI_PS1_SAVE_NAME_MAX && p[i] != 0u; i++)
        out[i] = (char)p[i];
    out[i] = '\0';
}

static void NameFromPath(char out[MCI_PS1_SAVE_NAME_MAX + 1u], const char *path)
{
    const char *base = path;
    const char *p;
    unsigned int i = 0u;

    out[0] = '\0';
    if (path == NULL)
        return;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;
    for (p = base; *p && *p != '.' && i < MCI_PS1_SAVE_NAME_MAX; p++)
        out[i++] = *p;
    out[i] = '\0';
}

int MciPs1DecodeSingle(const unsigned char *data, u32 size,
                       const char *filename, MciSaveArchive *archive,
                       MciPs1SingleKind *kind_out)
{
    char name[MCI_PS1_SAVE_NAME_MAX + 1u];
    MciPs1SingleKind kind;
    u32 body_size;
    u32 body_off;

    if (archive == NULL || data == NULL)
        return MCI_SAVE_E_ARGS;
    kind = MciPs1DetectSingle(data, size);
    if (kind_out != NULL)
        *kind_out = kind;
    switch (kind) {
        case MCI_PS1_SINGLE_MCS:
            NameFromFixed(name, data + 0x0Au, 21u);
            return MakePs1Archive(archive, name, data + 128u, size - 128u);
        case MCI_PS1_SINGLE_AR:
            NameFromFixed(name, data, 21u);
            return MakePs1Archive(archive, name, data + 54u, size - 54u);
        case MCI_PS1_SINGLE_RAW:
            NameFromPath(name, filename);
            return MakePs1Archive(archive, name, data, size);
        case MCI_PS1_SINGLE_PSV:
            body_size = MciLe32(data + 0x40u);
            body_off = MciLe32(data + 0x44u);
            if (body_off < 0x84u || body_off > size || body_size > size - body_off)
                return MCI_SAVE_E_TRUNCATED;
            if (!BodyOk(data + body_off, body_size))
                return MCI_SAVE_E_CORRUPT;
            NameFromFixed(name, data + 0x64u, 20u);
            return MakePs1Archive(archive, name, data + body_off, body_size);
        default:
            return MCI_SAVE_E_FORMAT;
    }
}

u32 MciPs1McsSize(const MciSaveArchive *archive)
{
    if (MciPs1ArchiveValidate(archive) < 0)
        return 0u;
    return MCI_PS1_FRAME_BYTES + archive->files[0].size;
}

int MciPs1EncodeMcs(const MciSaveArchive *archive, unsigned char *out,
                    u32 out_size)
{
    u32 need = MciPs1McsSize(archive);
    const MciSaveFile *f;

    if (need == 0u || out == NULL)
        return MCI_SAVE_E_ARGS;
    if (out_size < need)
        return MCI_SAVE_E_LIMIT;
    f = &archive->files[0];
    memset(out, 0, MCI_PS1_FRAME_BYTES);
    out[0] = MCI_PS1_STATE_FIRST;
    MciPutLe32(out + 4u, f->size);
    MciPutLe16(out + 8u, MCI_PS1_LINK_END);
    memcpy(out + 0x0Au, f->name, strlen(f->name));
    SetFrameXor(out);
    memcpy(out + MCI_PS1_FRAME_BYTES, f->data, f->size);
    return (int)need;
}

u32 MciPs1PsvSize(const MciSaveArchive *archive)
{
    if (MciPs1ArchiveValidate(archive) < 0)
        return 0u;
    return 0x84u + archive->files[0].size;
}

int MciPs1EncodePsv(const MciSaveArchive *archive, unsigned char *out,
                    u32 out_size)
{
    u32 need = MciPs1PsvSize(archive);
    const MciSaveFile *f;

    if (need == 0u || out == NULL)
        return MCI_SAVE_E_ARGS;
    if (out_size < need)
        return MCI_SAVE_E_LIMIT;
    f = &archive->files[0];
    memset(out, 0, 0x84u);
    memcpy(out, "\0VSP", 4u);
    out[0x38] = 0x14u;                 /* header size */
    out[0x3C] = 0x01u;                 /* PS1 */
    MciPutLe32(out + 0x40u, f->size);  /* save size */
    out[0x44] = 0x84u;                 /* data offset */
    out[0x49] = 0x02u;                 /* 0x200 block size */
    MciPutLe32(out + 0x5Cu, f->size);  /* data size */
    out[0x60] = 0x03u;                 /* constant 0x9003 */
    out[0x61] = 0x90u;
    memcpy(out + 0x64u, f->name, strlen(f->name));
    memcpy(out + 0x84u, f->data, f->size);
    return MciPsvSign(out, need) < 0 ? MCI_SAVE_E_ARGS : (int)need;
}

u32 MciPs1ArSize(const MciSaveArchive *archive)
{
    if (MciPs1ArchiveValidate(archive) < 0)
        return 0u;
    return 54u + archive->files[0].size;
}

int MciPs1EncodeAr(const MciSaveArchive *archive, unsigned char *out,
                   u32 out_size)
{
    u32 need = MciPs1ArSize(archive);
    const MciSaveFile *f;
    size_t tl;

    if (need == 0u || out == NULL)
        return MCI_SAVE_E_ARGS;
    if (out_size < need)
        return MCI_SAVE_E_LIMIT;
    f = &archive->files[0];
    memset(out, 0, 54u);
    memcpy(out, f->name, strlen(f->name));
    tl = strlen(archive->title);
    if (tl > 32u)
        tl = 32u;
    memcpy(out + 21u, archive->title, tl);
    memcpy(out + 54u, f->data, f->size);
    return (int)need;
}
