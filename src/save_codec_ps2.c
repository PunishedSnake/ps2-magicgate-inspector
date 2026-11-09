/* SPDX-License-Identifier: MIT */
/* PS2 single-save container codecs. See save_codec_ps2.h. */

#include <tamtypes.h>
#include <stdlib.h>
#include <string.h>

#include "card_math.h"
#include "lzari.h"
#include "save_codec_ps2.h"
#include "tinf/tinf.h"

#define PSU_ENTRY 512u
#define PSU_CLUSTER 1024u
#define MAX_HEADER 0x5Cu
#define SPS_MAGIC_LEN 17u
#define SPS_HEAD 98u
#define CBS_ENTRY_HEAD 64u
#define PSV_HEADER 0x40u
#define PSV_PS2_HEADER 0x28u
#define PSV_DIRINFO 0x38u
#define PSV_FILEINFO 0x3Cu

static const unsigned char SpsMagic[SPS_MAGIC_LEN] = {
    0x0D, 0x00, 0x00, 0x00, 'S', 'h', 'a', 'r', 'k', 'P', 'o', 'r', 't',
    'S', 'a', 'v', 'e'
};

/* Initial RC4 permutation used by CodeBreaker (no key schedule). Constant
 * from mymc ps2save.py PS2SAVE_CBS_RC4S (public domain). */
static const unsigned char CbsRc4State[256] = {
    0x5f, 0x1f, 0x85, 0x6f, 0x31, 0xaa, 0x3b, 0x18, 0x21, 0xb9, 0xce, 0x1c,
    0x07, 0x4c, 0x9c, 0xb4, 0x81, 0xb8, 0xef, 0x98, 0x59, 0xae, 0xf9, 0x26,
    0xe3, 0x80, 0xa3, 0x29, 0x2d, 0x73, 0x51, 0x62, 0x7c, 0x64, 0x46, 0xf4,
    0x34, 0x1a, 0xf6, 0xe1, 0xba, 0x3a, 0x0d, 0x82, 0x79, 0x0a, 0x5c, 0x16,
    0x71, 0x49, 0x8e, 0xac, 0x8c, 0x9f, 0x35, 0x19, 0x45, 0x94, 0x3f, 0x56,
    0x0c, 0x91, 0x00, 0x0b, 0xd7, 0xb0, 0xdd, 0x39, 0x66, 0xa1, 0x76, 0x52,
    0x13, 0x57, 0xf3, 0xbb, 0x4e, 0xe5, 0xdc, 0xf0, 0x65, 0x84, 0xb2, 0xd6,
    0xdf, 0x15, 0x3c, 0x63, 0x1d, 0x89, 0x14, 0xbd, 0xd2, 0x36, 0xfe, 0xb1,
    0xca, 0x8b, 0xa4, 0xc6, 0x9e, 0x67, 0x47, 0x37, 0x42, 0x6d, 0x6a, 0x03,
    0x92, 0x70, 0x05, 0x7d, 0x96, 0x2f, 0x40, 0x90, 0xc4, 0xf1, 0x3e, 0x3d,
    0x01, 0xf7, 0x68, 0x1e, 0xc3, 0xfc, 0x72, 0xb5, 0x54, 0xcf, 0xe7, 0x41,
    0xe4, 0x4d, 0x83, 0x55, 0x12, 0x22, 0x09, 0x78, 0xfa, 0xde, 0xa7, 0x06,
    0x08, 0x23, 0xbf, 0x0f, 0xcc, 0xc1, 0x97, 0x61, 0xc5, 0x4a, 0xe6, 0xa0,
    0x11, 0xc2, 0xea, 0x74, 0x02, 0x87, 0xd5, 0xd1, 0x9d, 0xb7, 0x7e, 0x38,
    0x60, 0x53, 0x95, 0x8d, 0x25, 0x77, 0x10, 0x5e, 0x9b, 0x7f, 0xd8, 0x6e,
    0xda, 0xa2, 0x2e, 0x20, 0x4f, 0xcd, 0x8f, 0xcb, 0xbe, 0x5a, 0xe0, 0xed,
    0x2c, 0x9a, 0xd4, 0xe2, 0xaf, 0xd0, 0xa9, 0xe8, 0xad, 0x7a, 0xbc, 0xa8,
    0xf2, 0xee, 0xeb, 0xf5, 0xa6, 0x99, 0x28, 0x24, 0x6c, 0x2b, 0x75, 0x5d,
    0xf8, 0xd3, 0x86, 0x17, 0xfb, 0xc0, 0x7b, 0xb3, 0x58, 0xdb, 0xc7, 0x4b,
    0xff, 0x04, 0x50, 0xe9, 0x88, 0x69, 0xc9, 0x2a, 0xab, 0xfd, 0x5b, 0x1b,
    0x8a, 0xd9, 0xec, 0x27, 0x44, 0x0e, 0x33, 0xc8, 0x6b, 0x93, 0x32, 0x48,
    0xb6, 0x30, 0x43, 0xa5,
};

/* ------------------------------------------------------------ helpers */

typedef struct Builder {
    MciSaveArchive *archive;
    u32 used;
} Builder;

static int BuilderBegin(Builder *b, MciSaveArchive *archive, u32 total_bytes)
{
    MciSaveArchiveInit(archive);
    if (total_bytes > MCI_SAVE_MAX_BYTES)
        return MCI_SAVE_E_LIMIT;
    archive->storage = (unsigned char *)malloc(total_bytes ? total_bytes : 1u);
    if (archive->storage == NULL)
        return MCI_SAVE_E_NOMEM;
    archive->storage_size = total_bytes;
    archive->generation = MCI_SAVE_GEN_PS2;
    archive->dir_mode = MCI_PS2_DEFAULT_DIR_MODE;
    b->archive = archive;
    b->used = 0u;
    return MCI_SAVE_OK;
}

static int BuilderAdd(Builder *b, const void *name_field, unsigned int name_size,
                      u16 mode, const unsigned char *data, u32 size)
{
    MciSaveArchive *a = b->archive;
    MciSaveFile *f;
    unsigned int i;
    int rc;

    if (a->file_count >= MCI_SAVE_MAX_FILES)
        return MCI_SAVE_E_LIMIT;
    if (size > a->storage_size - b->used)
        return MCI_SAVE_E_CORRUPT;
    f = &a->files[a->file_count];
    memset(f, 0, sizeof(*f));
    rc = MciSaveCopyName(f->name, name_field, name_size);
    if (rc < 0)
        return rc;
    for (i = 0u; i < a->file_count; i++)
        if (strcmp(a->files[i].name, f->name) == 0)
            return MCI_SAVE_E_CORRUPT; /* duplicate entry */
    if ((mode & MCI_MC_MODE_DIR) != 0u)
        return MCI_SAVE_E_SUBDIR;
    if ((mode & MCI_MC_MODE_FILE) == 0u)
        mode = MCI_PS2_DEFAULT_FILE_MODE;
    f->mode = mode;
    f->size = size;
    memcpy(a->storage + b->used, data, size);
    f->data = a->storage + b->used;
    b->used += size;
    a->file_count++;
    return MCI_SAVE_OK;
}

static int BuilderFinish(Builder *b, int rc)
{
    if (rc == MCI_SAVE_OK && b->archive->file_count == 0u)
        rc = MCI_SAVE_E_CORRUPT;
    if (rc != MCI_SAVE_OK)
        MciSaveArchiveFree(b->archive);
    return rc;
}

static void CopyTod(MciMcDateTime *out, const unsigned char *p)
{
    memcpy(out, p, sizeof(*out));
}

static u16 DirModeOr(u32 mode)
{
    return (mode & MCI_MC_MODE_DIR) ? (u16)mode : (u16)MCI_PS2_DEFAULT_DIR_MODE;
}

const char *MciPs2ContainerName(MciPs2Container kind)
{
    switch (kind) {
        case MCI_PS2_CONTAINER_PSU: return "PS2 PSU (EMS/uLaunchELF)";
        case MCI_PS2_CONTAINER_MAX: return "PS2 Action Replay MAX (.max)";
        case MCI_PS2_CONTAINER_CBS: return "PS2 CodeBreaker (.cbs)";
        case MCI_PS2_CONTAINER_SPS: return "PS2 SharkPort/X-Port (.sps/.xps)";
        case MCI_PS2_CONTAINER_PSV: return "PS3 PS2 save (.psv)";
        default: return "none";
    }
}

static int PsuLooksValid(const unsigned char *data, u32 size)
{
    u16 m0, m1, m2;

    if (size < PSU_ENTRY * 3u)
        return 0;
    m0 = MciLe16(data);
    m1 = MciLe16(data + PSU_ENTRY);
    m2 = MciLe16(data + 2u * PSU_ENTRY);
    return (m0 & MCI_MC_MODE_DIR) && (m1 & MCI_MC_MODE_DIR) && (m2 & MCI_MC_MODE_DIR) &&
           memcmp(data + PSU_ENTRY + 64u, ".\0", 2) == 0 &&
           memcmp(data + 2u * PSU_ENTRY + 64u, "..\0", 3) == 0;
}

MciPs2Container MciPs2DetectContainer(const unsigned char *data, u32 size)
{
    if (data == NULL)
        return MCI_PS2_CONTAINER_NONE;
    if (size >= MAX_HEADER && memcmp(data, "Ps2PowerSave", 12) == 0)
        return MCI_PS2_CONTAINER_MAX;
    if (size >= 4u && memcmp(data, "CFU\0", 4) == 0)
        return MCI_PS2_CONTAINER_CBS;
    if (size >= SPS_MAGIC_LEN && memcmp(data, SpsMagic, SPS_MAGIC_LEN) == 0)
        return MCI_PS2_CONTAINER_SPS;
    if (size >= PSV_HEADER && memcmp(data, "\0VSP", 4) == 0 && MciLe32(data + 0x3Cu) == 2u)
        return MCI_PS2_CONTAINER_PSV;
    if (PsuLooksValid(data, size))
        return MCI_PS2_CONTAINER_PSU;
    return MCI_PS2_CONTAINER_NONE;
}

/* ---------------------------------------------------------------- PSU */

static int DecodePsu(const unsigned char *data, u32 size, MciSaveArchive *archive)
{
    Builder b;
    u32 count;
    u32 pos;
    u32 total = 0u;
    u32 i;
    int rc;

    if (!PsuLooksValid(data, size))
        return MCI_SAVE_E_FORMAT;
    count = MciLe32(data + 4u);
    if (count < 2u || count - 2u > MCI_SAVE_MAX_FILES)
        return MCI_SAVE_E_LIMIT;
    count -= 2u;
    /* Pass 1: bounds and totals. */
    pos = 3u * PSU_ENTRY;
    for (i = 0u; i < count; i++) {
        u32 len;
        u32 padded;
        if (size - pos < PSU_ENTRY)
            return MCI_SAVE_E_TRUNCATED;
        len = MciLe32(data + pos + 4u);
        padded = (len + PSU_CLUSTER - 1u) / PSU_CLUSTER * PSU_CLUSTER;
        if (len > MCI_SAVE_MAX_BYTES || padded > size - pos - PSU_ENTRY)
            return MCI_SAVE_E_TRUNCATED;
        total += len;
        if (total > MCI_SAVE_MAX_BYTES)
            return MCI_SAVE_E_LIMIT;
        pos += PSU_ENTRY + padded;
    }
    if (pos != size)
        return MCI_SAVE_E_CORRUPT;

    rc = BuilderBegin(&b, archive, total);
    if (rc < 0)
        return rc;
    rc = MciSaveCopyName(archive->directory, data + 64u, 32u);
    archive->dir_mode = DirModeOr(MciLe16(data));
    CopyTod(&archive->created, data + 8u);
    CopyTod(&archive->modified, data + 24u);
    pos = 3u * PSU_ENTRY;
    for (i = 0u; rc == MCI_SAVE_OK && i < count; i++) {
        const unsigned char *e = data + pos;
        u32 len = MciLe32(e + 4u);
        rc = BuilderAdd(&b, e + 64u, 32u, MciLe16(e), e + PSU_ENTRY, len);
        if (rc == MCI_SAVE_OK) {
            CopyTod(&archive->files[archive->file_count - 1u].created, e + 8u);
            CopyTod(&archive->files[archive->file_count - 1u].modified, e + 24u);
        }
        pos += PSU_ENTRY + (len + PSU_CLUSTER - 1u) / PSU_CLUSTER * PSU_CLUSTER;
    }
    return BuilderFinish(&b, rc);
}

u32 MciPs2PsuSize(const MciSaveArchive *archive)
{
    u32 total;
    u32 i;

    if (archive == NULL || archive->generation != MCI_SAVE_GEN_PS2)
        return 0u;
    total = 3u * PSU_ENTRY;
    for (i = 0u; i < archive->file_count; i++)
        total += PSU_ENTRY + (archive->files[i].size + PSU_CLUSTER - 1u) /
                                 PSU_CLUSTER * PSU_CLUSTER;
    return total;
}

static void PutPsuEntry(unsigned char *e, u16 mode, u32 length,
                        const MciMcDateTime *created, const MciMcDateTime *modified,
                        const char *name)
{
    memset(e, 0, PSU_ENTRY);
    MciPutLe16(e, mode);
    MciPutLe32(e + 4u, length);
    memcpy(e + 8u, created, 8u);
    memcpy(e + 24u, modified, 8u);
    memcpy(e + 64u, name, strlen(name));
}

int MciPs2EncodePsu(const MciSaveArchive *archive, unsigned char *out,
                    u32 out_size)
{
    u32 need = MciPs2PsuSize(archive);
    u32 pos;
    u32 i;

    if (need == 0u || out == NULL || !MciSaveNameIsSafe(archive->directory))
        return MCI_SAVE_E_ARGS;
    if (out_size < need)
        return MCI_SAVE_E_LIMIT;
    memset(out, 0, need);
    PutPsuEntry(out, archive->dir_mode ? archive->dir_mode : MCI_PS2_DEFAULT_DIR_MODE,
                archive->file_count + 2u, &archive->created, &archive->modified,
                archive->directory);
    PutPsuEntry(out + PSU_ENTRY, MCI_PS2_DEFAULT_DIR_MODE, 0u,
                &archive->created, &archive->modified, ".");
    PutPsuEntry(out + 2u * PSU_ENTRY, MCI_PS2_DEFAULT_DIR_MODE, 0u,
                &archive->created, &archive->modified, "..");
    pos = 3u * PSU_ENTRY;
    for (i = 0u; i < archive->file_count; i++) {
        const MciSaveFile *f = &archive->files[i];
        PutPsuEntry(out + pos, f->mode ? f->mode : MCI_PS2_DEFAULT_FILE_MODE,
                    f->size, &f->created, &f->modified, f->name);
        pos += PSU_ENTRY;
        memcpy(out + pos, f->data, f->size);
        pos += (f->size + PSU_CLUSTER - 1u) / PSU_CLUSTER * PSU_CLUSTER;
    }
    return (int)need;
}

/* ---------------------------------------------------------------- MAX */

static u32 MaxNextOffset(u32 off)
{
    /* Next entry begins at roundup(off + 8, 16) - 8. */
    return ((off + 8u + 15u) & ~15u) - 8u;
}

static int DecodeMax(const unsigned char *data, u32 size, MciSaveArchive *archive,
                     MciPs2DecodeInfo *info)
{
    unsigned char header[MAX_HEADER];
    unsigned char *plain = NULL;
    Builder b;
    u32 stored_crc;
    u32 crc;
    u32 clen;
    u32 nfiles;
    u32 dlen;
    u32 avail;
    u32 off;
    u32 total = 0u;
    u32 i;
    int rc;

    if (size < MAX_HEADER || memcmp(data, "Ps2PowerSave", 12) != 0)
        return MCI_SAVE_E_FORMAT;
    stored_crc = MciLe32(data + 12u);
    clen = MciLe32(data + 80u);
    nfiles = MciLe32(data + 84u);
    dlen = MciLe32(data + 88u);
    if (nfiles == 0u || nfiles > MCI_SAVE_MAX_FILES || dlen > MCI_SAVE_MAX_BYTES)
        return MCI_SAVE_E_LIMIT;

    /* compressedSize counts the 4-byte length field that precedes the LZARI
     * stream; some writers store the uncompressed size instead. */
    avail = size - MAX_HEADER;
    if (clen != dlen && clen >= 4u && clen - 4u <= avail)
        avail = clen - 4u;

    memcpy(header, data, MAX_HEADER);
    memset(header + 12u, 0, 4u);
    crc = MciCardMathCrc32Update(0u, header, MAX_HEADER);
    crc = MciCardMathCrc32Update(crc, data + MAX_HEADER, avail);
    if (info != NULL) {
        info->checksum_present = 1;
        info->checksum_ok = crc == stored_crc;
    }

    plain = (unsigned char *)malloc(dlen ? dlen : 1u);
    if (plain == NULL)
        return MCI_SAVE_E_NOMEM;
    if (MciLzariDecode(data + MAX_HEADER, avail, plain, dlen) < 0) {
        free(plain);
        return MCI_SAVE_E_DECOMPRESS;
    }

    /* Pass 1 over the decompressed stream. */
    off = 0u;
    for (i = 0u; i < nfiles; i++) {
        u32 len;
        if (dlen < 36u || off > dlen - 36u) { free(plain); return MCI_SAVE_E_TRUNCATED; }
        len = MciLe32(plain + off);
        if (len > dlen - off - 36u) { free(plain); return MCI_SAVE_E_TRUNCATED; }
        total += len;
        off = MaxNextOffset(off + 36u + len);
    }

    rc = BuilderBegin(&b, archive, total);
    if (rc < 0) { free(plain); return rc; }
    rc = MciSaveCopyName(archive->directory, data + 16u, 32u);
    off = 0u;
    for (i = 0u; rc == MCI_SAVE_OK && i < nfiles; i++) {
        u32 len = MciLe32(plain + off);
        rc = BuilderAdd(&b, plain + off + 4u, 32u, MCI_PS2_DEFAULT_FILE_MODE,
                        plain + off + 36u, len);
        off = MaxNextOffset(off + 36u + len);
    }
    if (rc == MCI_SAVE_OK && data[48] != 0u)
        (void)MciSaveCopyName(archive->title, data + 48u, 32u);
    free(plain);
    if (info != NULL)
        info->timestamps_present = 0;
    return BuilderFinish(&b, rc);
}

/* ---------------------------------------------------------------- CBS */

static void Rc4Crypt(unsigned char *buf, u32 len)
{
    unsigned char s[256];
    unsigned int i = 0u;
    unsigned int j = 0u;
    u32 n;

    memcpy(s, CbsRc4State, sizeof(s));
    for (n = 0u; n < len; n++) {
        unsigned char t;
        i = (i + 1u) & 0xFFu;
        j = (j + s[i]) & 0xFFu;
        t = s[i]; s[i] = s[j]; s[j] = t;
        buf[n] ^= s[(s[i] + s[j]) & 0xFFu];
    }
}

static int DecodeCbs(const unsigned char *data, u32 size, MciSaveArchive *archive)
{
    unsigned char *body = NULL;
    unsigned char *plain = NULL;
    unsigned int plain_len;
    Builder b;
    u32 hlen;
    u32 dlen;
    u32 flen;
    u32 avail;
    u32 dirmode;
    u32 off;
    u32 total = 0u;
    u32 nfiles = 0u;
    int rc;

    if (size < 12u || memcmp(data, "CFU\0", 4) != 0)
        return MCI_SAVE_E_FORMAT;
    hlen = MciLe32(data + 8u);
    if (hlen < 92u + 32u || hlen > size)
        return MCI_SAVE_E_CORRUPT;
    dlen = MciLe32(data + 12u);
    flen = MciLe32(data + 16u);
    if (dlen > MCI_SAVE_MAX_BYTES)
        return MCI_SAVE_E_LIMIT;
    /* flen is either the body length or the whole file length. */
    avail = size - hlen;
    if (flen == avail || flen == size)
        ;
    else if (flen < avail)
        avail = flen;
    else
        return MCI_SAVE_E_TRUNCATED;

    body = (unsigned char *)malloc(avail ? avail : 1u);
    plain = (unsigned char *)malloc(dlen ? dlen : 1u);
    if (body == NULL || plain == NULL) {
        free(body); free(plain);
        return MCI_SAVE_E_NOMEM;
    }
    memcpy(body, data + hlen, avail);
    Rc4Crypt(body, avail);
    plain_len = dlen;
    rc = tinf_zlib_uncompress(plain, &plain_len, body, avail);
    if (rc != TINF_OK && avail > 2u) {
        /* Tolerate a missing/odd Adler trailer: raw-inflate after the
         * 2-byte zlib header, as zlib's decompressobj effectively does. */
        plain_len = dlen;
        rc = tinf_uncompress(plain, &plain_len, body + 2u, avail - 2u);
    }
    free(body);
    if (rc != TINF_OK || plain_len != dlen) {
        free(plain);
        return MCI_SAVE_E_DECOMPRESS;
    }

    for (off = 0u; off < dlen; nfiles++) {
        u32 len;
        if (dlen - off < CBS_ENTRY_HEAD) { free(plain); return MCI_SAVE_E_TRUNCATED; }
        len = MciLe32(plain + off + 16u);
        if (len > dlen - off - CBS_ENTRY_HEAD) { free(plain); return MCI_SAVE_E_TRUNCATED; }
        total += len;
        off += CBS_ENTRY_HEAD + len;
        if (nfiles >= MCI_SAVE_MAX_FILES) { free(plain); return MCI_SAVE_E_LIMIT; }
    }

    rc = BuilderBegin(&b, archive, total);
    if (rc < 0) { free(plain); return rc; }
    rc = MciSaveCopyName(archive->directory, data + 20u, 32u);
    CopyTod(&archive->created, data + 52u);
    CopyTod(&archive->modified, data + 60u);
    /* mymc reads the directory mode at 0x4C; Apollo documents 0x48. */
    dirmode = MciLe32(data + 76u);
    if ((dirmode & MCI_MC_MODE_DIR) == 0u)
        dirmode = MciLe32(data + 72u);
    archive->dir_mode = DirModeOr(dirmode);
    off = 0u;
    while (rc == MCI_SAVE_OK && off < dlen) {
        const unsigned char *e = plain + off;
        u32 len = MciLe32(e + 16u);
        rc = BuilderAdd(&b, e + 32u, 32u, MciLe16(e + 20u), e + CBS_ENTRY_HEAD, len);
        if (rc == MCI_SAVE_OK) {
            CopyTod(&archive->files[archive->file_count - 1u].created, e);
            CopyTod(&archive->files[archive->file_count - 1u].modified, e + 8u);
        }
        off += CBS_ENTRY_HEAD + len;
    }
    if (rc == MCI_SAVE_OK && hlen > 92u && data[92] != 0u) {
        u32 n = hlen - 92u < sizeof(archive->title) - 1u ? hlen - 92u
                                                        : (u32)sizeof(archive->title) - 1u;
        u32 k;
        for (k = 0u; k < n && data[92u + k] != 0u; k++)
            archive->title[k] = (data[92u + k] >= 0x20u && data[92u + k] < 0x7Fu)
                                    ? (char)data[92u + k] : ' ';
        archive->title[k] = '\0';
    }
    free(plain);
    return BuilderFinish(&b, rc);
}

/* ------------------------------------------------------- SharkPort/X-Port */

static int SkipLongString(const unsigned char *data, u32 size, u32 *pos)
{
    u32 len;
    if (size - *pos < 4u)
        return MCI_SAVE_E_TRUNCATED;
    len = MciLe32(data + *pos);
    *pos += 4u;
    if (len > size - *pos)
        return MCI_SAVE_E_TRUNCATED;
    *pos += len;
    return MCI_SAVE_OK;
}

static u16 Swap16(u16 v)
{
    return (u16)((v >> 8) | (v << 8));
}

static int DecodeSps(const unsigned char *data, u32 size, MciSaveArchive *archive)
{
    Builder b;
    u32 pos = SPS_MAGIC_LEN + 4u;
    u32 dir_pos;
    u32 hlen;
    u32 count;
    u32 total = 0u;
    u32 i;
    int rc;

    if (size < SPS_MAGIC_LEN + 4u || memcmp(data, SpsMagic, SPS_MAGIC_LEN) != 0)
        return MCI_SAVE_E_FORMAT;
    if ((rc = SkipLongString(data, size, &pos)) < 0 ||   /* dirname */
        (rc = SkipLongString(data, size, &pos)) < 0 ||   /* datestamp */
        (rc = SkipLongString(data, size, &pos)) < 0)     /* comment */
        return rc;
    if (size - pos < 4u)
        return MCI_SAVE_E_TRUNCATED;
    pos += 4u; /* total length of the following data */
    if (size - pos < SPS_HEAD)
        return MCI_SAVE_E_TRUNCATED;
    dir_pos = pos;
    hlen = MciLe16(data + pos);
    if (hlen < SPS_HEAD || hlen > size - pos)
        return MCI_SAVE_E_CORRUPT;
    count = MciLe32(data + pos + 66u);
    if (count < 2u || count - 2u > MCI_SAVE_MAX_FILES)
        return MCI_SAVE_E_LIMIT;
    count -= 2u;
    pos += hlen;

    {   /* Pass 1 */
        u32 p = pos;
        for (i = 0u; i < count; i++) {
            u32 h, len;
            if (size - p < SPS_HEAD)
                return MCI_SAVE_E_TRUNCATED;
            h = MciLe16(data + p);
            len = MciLe32(data + p + 66u);
            if (h < SPS_HEAD || h > size - p || len > size - p - h)
                return MCI_SAVE_E_TRUNCATED;
            total += len;
            if (total > MCI_SAVE_MAX_BYTES)
                return MCI_SAVE_E_LIMIT;
            p += h + len;
        }
    }

    rc = BuilderBegin(&b, archive, total);
    if (rc < 0)
        return rc;
    rc = MciSaveCopyName(archive->directory, data + dir_pos + 2u, 64u);
    archive->dir_mode = DirModeOr(Swap16(MciLe16(data + dir_pos + 78u)));
    CopyTod(&archive->created, data + dir_pos + 82u);
    CopyTod(&archive->modified, data + dir_pos + 90u);
    for (i = 0u; rc == MCI_SAVE_OK && i < count; i++) {
        u32 h = MciLe16(data + pos);
        u32 len = MciLe32(data + pos + 66u);
        rc = BuilderAdd(&b, data + pos + 2u, 64u, Swap16(MciLe16(data + pos + 78u)),
                        data + pos + h, len);
        if (rc == MCI_SAVE_OK) {
            CopyTod(&archive->files[archive->file_count - 1u].created, data + pos + 82u);
            CopyTod(&archive->files[archive->file_count - 1u].modified, data + pos + 90u);
        }
        pos += h + len;
    }
    return BuilderFinish(&b, rc);
}

/* ---------------------------------------------------------------- PSV */

static int DecodePsv(const unsigned char *data, u32 size, MciSaveArchive *archive)
{
    Builder b;
    u32 count;
    u32 table;
    u32 total = 0u;
    u32 i;
    int rc;

    if (size < PSV_HEADER + PSV_PS2_HEADER + PSV_DIRINFO ||
        memcmp(data, "\0VSP", 4) != 0 || MciLe32(data + 0x3Cu) != 2u)
        return MCI_SAVE_E_FORMAT;
    count = MciLe32(data + PSV_HEADER + 36u);
    if (count == 0u || count > MCI_SAVE_MAX_FILES)
        return MCI_SAVE_E_LIMIT;
    table = PSV_HEADER + PSV_PS2_HEADER + PSV_DIRINFO;
    if ((size - table) / PSV_FILEINFO < count)
        return MCI_SAVE_E_TRUNCATED;
    for (i = 0u; i < count; i++) {
        const unsigned char *fi = data + table + i * PSV_FILEINFO;
        u32 len = MciLe32(fi + 16u);
        u32 at = MciLe32(fi + 56u);
        if (at > size || len > size - at)
            return MCI_SAVE_E_TRUNCATED;
        total += len;
        if (total > MCI_SAVE_MAX_BYTES)
            return MCI_SAVE_E_LIMIT;
    }

    rc = BuilderBegin(&b, archive, total);
    if (rc < 0)
        return rc;
    {
        const unsigned char *di = data + PSV_HEADER + PSV_PS2_HEADER;
        CopyTod(&archive->created, di);
        CopyTod(&archive->modified, di + 8u);
        archive->dir_mode = DirModeOr(MciLe32(di + 20u));
        rc = MciSaveCopyName(archive->directory, di + 24u, 32u);
    }
    for (i = 0u; rc == MCI_SAVE_OK && i < count; i++) {
        const unsigned char *fi = data + table + i * PSV_FILEINFO;
        rc = BuilderAdd(&b, fi + 24u, 32u, (u16)MciLe32(fi + 20u),
                        data + MciLe32(fi + 56u), MciLe32(fi + 16u));
        if (rc == MCI_SAVE_OK) {
            CopyTod(&archive->files[archive->file_count - 1u].created, fi);
            CopyTod(&archive->files[archive->file_count - 1u].modified, fi + 8u);
        }
    }
    return BuilderFinish(&b, rc);
}

/* ---------------------------------------------------------- dispatcher */

int MciPs2Decode(const unsigned char *data, u32 size, MciSaveArchive *archive,
                 MciPs2DecodeInfo *info)
{
    MciPs2Container kind;
    MciPs2DecodeInfo local;

    if (info == NULL)
        info = &local;
    memset(info, 0, sizeof(*info));
    info->timestamps_present = 1;
    if (data == NULL || archive == NULL)
        return MCI_SAVE_E_ARGS;
    MciSaveArchiveInit(archive);
    kind = MciPs2DetectContainer(data, size);
    info->kind = kind;
    switch (kind) {
        case MCI_PS2_CONTAINER_PSU: return DecodePsu(data, size, archive);
        case MCI_PS2_CONTAINER_MAX: return DecodeMax(data, size, archive, info);
        case MCI_PS2_CONTAINER_CBS: return DecodeCbs(data, size, archive);
        case MCI_PS2_CONTAINER_SPS: return DecodeSps(data, size, archive);
        case MCI_PS2_CONTAINER_PSV: return DecodePsv(data, size, archive);
        default: return MCI_SAVE_E_FORMAT;
    }
}
