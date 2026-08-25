/* SPDX-License-Identifier: MIT */
/* Read-only icon.sys title resolver for the image browser presentation layer. */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <stdio.h>
#include <string.h>

#include "image_save_title.h"
#include "save_title.h"

#define TITLE_PAGE_SIZE 512u
#define TITLE_PS2_STRIDE 528u
#define TITLE_MAX_CLUSTER_BYTES 1024u
#define TITLE_FAT_END 0xFFFFFFFFu
#define TITLE_FAT_NEXT_MASK 0x7FFFFFFFu
#define TITLE_MODE_FILE 0x0010u
#define TITLE_MODE_EXISTS 0x8000u

#pragma pack(push, 1)
typedef struct TitleSuperblock {
    char magic[28];
    char version[12];
    u16 page_len;
    u16 pages_per_cluster;
    u16 pages_per_block;
    u16 unused;
    u32 clusters_per_card;
    u32 alloc_offset;
    u32 alloc_end;
    u32 rootdir_cluster;
    u32 backup_block1;
    u32 backup_block2;
    u64 padding0x48;
    u32 ifc_list[32];
    u32 bad_block_list[32];
    u8 card_type;
    u8 card_flags;
} TitleSuperblock;

typedef struct TitleDirEntry {
    u16 mode;
    u16 unused;
    u32 length;
    u8 created[8];
    u32 cluster;
    u32 dir_entry;
    u8 modified[8];
    u32 attr;
    u32 unused2[7];
    char name[32];
    u8 unused3[416];
} TitleDirEntry;
#pragma pack(pop)

typedef struct TitleImage {
    int fd;
    u32 stride;
    u32 cluster_bytes;
    u32 dir_entries_per_cluster;
    u32 fat_entries_per_cluster;
    TitleSuperblock sb;
} TitleImage;

typedef struct TitleDirCursor {
    u32 current_relative;
    u32 cluster_index;
    int loaded;
    unsigned char cluster[TITLE_MAX_CLUSTER_BYTES];
} TitleDirCursor;

static char CachePath[MCI_CARD_IMAGE_PATH_MAX];
static MciCardImageFormat CacheFormat;
static int CacheCount = -1;
static char CacheTitles[MCI_IMAGE_SAVE_MAX][MCI_SAVE_TITLE_MAX];
static unsigned char CacheValid[MCI_IMAGE_SAVE_MAX];

static int name_equal_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        int ca = (unsigned char)*a++;
        int cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int safe_name(const TitleDirEntry *entry, char out[33])
{
    unsigned int i;
    for (i = 0u; i < 32u; i++) {
        unsigned char c = (unsigned char)entry->name[i];
        if (c == 0u) {
            out[i] = '\0';
            return i > 0u ? 0 : -1;
        }
        if (c < 0x20u || c == '/' || c == '*' || c == '?')
            return -1;
        out[i] = (char)c;
    }
    out[32] = '\0';
    return 0;
}

static int read_exact(int fd, void *buffer, unsigned int size)
{
    unsigned char *p = (unsigned char *)buffer;
    unsigned int done = 0u;
    while (done < size) {
        int rc = fileXioRead(fd, p + done, (int)(size - done));
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += (unsigned int)rc;
    }
    return 0;
}

static int read_page(TitleImage *image, u32 page,
                     unsigned char data[TITLE_PAGE_SIZE])
{
    u64 off = (u64)page * image->stride;
    s64 rc = fileXioLseek64(image->fd, (s64)off, SEEK_SET);
    if (rc < 0 || (u64)rc != off)
        return rc < 0 ? (int)rc : -2;
    return read_exact(image->fd, data, TITLE_PAGE_SIZE);
}

static int read_cluster(TitleImage *image, u32 absolute_cluster,
                        unsigned char buffer[TITLE_MAX_CLUSTER_BYTES])
{
    u32 i;
    u32 page;
    int rc;
    if (absolute_cluster >= image->sb.clusters_per_card)
        return -1;
    memset(buffer, 0, TITLE_MAX_CLUSTER_BYTES);
    page = absolute_cluster * image->sb.pages_per_cluster;
    for (i = 0u; i < image->sb.pages_per_cluster; i++) {
        rc = read_page(image, page + i, buffer + i * TITLE_PAGE_SIZE);
        if (rc < 0)
            return rc;
    }
    return 0;
}

static int fat_entry(TitleImage *image, u32 relative_cluster, u32 *value)
{
    unsigned char cluster[TITLE_MAX_CLUSTER_BYTES];
    u32 fat_offset;
    u32 indirect_index;
    u32 indirect_offset;
    u32 double_index;
    u32 indirect_cluster;
    u32 fat_cluster;
    int rc;

    if (relative_cluster >= image->sb.alloc_end || value == NULL)
        return -1;
    fat_offset = relative_cluster % image->fat_entries_per_cluster;
    indirect_index = relative_cluster / image->fat_entries_per_cluster;
    indirect_offset = indirect_index % image->fat_entries_per_cluster;
    double_index = indirect_index / image->fat_entries_per_cluster;
    if (double_index >= 32u)
        return -2;
    indirect_cluster = image->sb.ifc_list[double_index];
    if (indirect_cluster == TITLE_FAT_END ||
        indirect_cluster >= image->sb.clusters_per_card)
        return -3;
    rc = read_cluster(image, indirect_cluster, cluster);
    if (rc < 0)
        return rc;
    memcpy(&fat_cluster, cluster + indirect_offset * sizeof(u32), sizeof(u32));
    if (fat_cluster == TITLE_FAT_END || fat_cluster >= image->sb.clusters_per_card)
        return -4;
    rc = read_cluster(image, fat_cluster, cluster);
    if (rc < 0)
        return rc;
    memcpy(value, cluster + fat_offset * sizeof(u32), sizeof(u32));
    return 0;
}

static int next_cluster(TitleImage *image, u32 current, u32 *next)
{
    u32 fat;
    int rc = fat_entry(image, current, &fat);
    if (rc < 0)
        return rc;
    if (fat == TITLE_FAT_END)
        return 1;
    if ((fat & 0x80000000u) == 0u)
        return -5;
    *next = fat & TITLE_FAT_NEXT_MASK;
    return *next < image->sb.alloc_end && *next != current ? 0 : -6;
}

static void dir_cursor_init(TitleDirCursor *cursor, u32 start_relative)
{
    memset(cursor, 0, sizeof(*cursor));
    cursor->current_relative = start_relative;
}

/* Sequential directory cursor. The former ReadDirEntryAt implementation began
 * at the first cluster for every index, making a directory scan repeatedly walk
 * the same FAT prefix. This cursor advances ownership of the current cluster
 * once and reuses its already-read 1 KiB payload for adjacent directory entries. */
static int dir_cursor_read(TitleImage *image, TitleDirCursor *cursor,
                           u32 index, TitleDirEntry *entry)
{
    u32 wanted_cluster;
    u32 slot;
    int rc;

    if (image == NULL || cursor == NULL || entry == NULL ||
        cursor->current_relative >= image->sb.alloc_end)
        return -1;
    wanted_cluster = index / image->dir_entries_per_cluster;
    slot = index % image->dir_entries_per_cluster;
    if (wanted_cluster < cursor->cluster_index)
        return -2;

    while (cursor->cluster_index < wanted_cluster) {
        u32 next;
        rc = next_cluster(image, cursor->current_relative, &next);
        if (rc != 0)
            return -3;
        cursor->current_relative = next;
        cursor->cluster_index++;
        cursor->loaded = 0;
    }

    if (!cursor->loaded) {
        rc = read_cluster(image,
                          image->sb.alloc_offset + cursor->current_relative,
                          cursor->cluster);
        if (rc < 0)
            return rc;
        cursor->loaded = 1;
    }
    memcpy(entry, cursor->cluster + slot * sizeof(TitleDirEntry),
           sizeof(*entry));
    return 0;
}

static int read_file_prefix(TitleImage *image, const TitleDirEntry *entry,
                            unsigned char *buffer, unsigned int capacity,
                            unsigned int *read_size)
{
    unsigned char cluster[TITLE_MAX_CLUSTER_BYTES];
    u32 current = entry->cluster;
    u32 remaining = entry->length;
    u32 next;
    unsigned int done = 0u;
    int rc;

    if (remaining > capacity)
        remaining = capacity;
    while (remaining > 0u) {
        u32 chunk;
        if (current == TITLE_FAT_END || current >= image->sb.alloc_end)
            return -1;
        rc = read_cluster(image, image->sb.alloc_offset + current, cluster);
        if (rc < 0)
            return rc;
        chunk = remaining > image->cluster_bytes ? image->cluster_bytes : remaining;
        memcpy(buffer + done, cluster, chunk);
        done += chunk;
        remaining -= chunk;
        if (remaining > 0u) {
            rc = next_cluster(image, current, &next);
            if (rc != 0)
                return -2;
            current = next;
        }
    }
    *read_size = done;
    return 0;
}

static int open_image(const MciImageSaveList *list, TitleImage *image)
{
    unsigned char first[TITLE_PAGE_SIZE];
    int rc;

    memset(image, 0, sizeof(*image));
    image->fd = fileXioOpen(list->path, FIO_O_RDONLY);
    if (image->fd < 0)
        return image->fd;
    image->stride = list->format == MCI_CARD_IMAGE_PS2
                        ? TITLE_PS2_STRIDE : TITLE_PAGE_SIZE;
    rc = read_page(image, 0u, first);
    if (rc < 0)
        goto fail;
    memcpy(&image->sb, first, sizeof(image->sb));
    if (memcmp(image->sb.magic, "Sony PS2 Memory Card Format ", 28u) != 0 ||
        image->sb.page_len != TITLE_PAGE_SIZE ||
        (image->sb.pages_per_cluster != 1u && image->sb.pages_per_cluster != 2u) ||
        image->sb.clusters_per_card == 0u ||
        image->sb.alloc_offset >= image->sb.clusters_per_card ||
        image->sb.alloc_end == 0u ||
        image->sb.alloc_offset + image->sb.alloc_end > image->sb.clusters_per_card)
        goto fail_invalid;
    image->cluster_bytes = image->sb.page_len * image->sb.pages_per_cluster;
    if (image->cluster_bytes == 0u ||
        image->cluster_bytes > TITLE_MAX_CLUSTER_BYTES ||
        image->cluster_bytes % sizeof(TitleDirEntry) != 0u)
        goto fail_invalid;
    image->dir_entries_per_cluster = image->cluster_bytes / sizeof(TitleDirEntry);
    image->fat_entries_per_cluster = image->cluster_bytes / sizeof(u32);
    return 0;

fail_invalid:
    rc = -3;
fail:
    fileXioClose(image->fd);
    image->fd = -1;
    return rc;
}

static int title_for_save(TitleImage *image, const MciImageSaveEntry *save,
                          char out[MCI_SAVE_TITLE_MAX])
{
    unsigned char icon[1024];
    TitleDirCursor cursor;
    TitleDirEntry entry;
    char name[33];
    u32 i;
    int rc;

    dir_cursor_init(&cursor, save->start_cluster);
    for (i = 2u; i < save->entry_count; i++) {
        unsigned int got = 0u;
        rc = dir_cursor_read(image, &cursor, i, &entry);
        if (rc < 0)
            return rc;
        if ((entry.mode & (TITLE_MODE_EXISTS | TITLE_MODE_FILE)) !=
            (TITLE_MODE_EXISTS | TITLE_MODE_FILE))
            continue;
        if (safe_name(&entry, name) < 0 || !name_equal_ci(name, "icon.sys"))
            continue;
        rc = read_file_prefix(image, &entry, icon, sizeof(icon), &got);
        if (rc < 0)
            return rc;
        return MciSaveTitleDecodeIconSys(icon, got, out, MCI_SAVE_TITLE_MAX);
    }
    return -10;
}

static void build_cache(const MciImageSaveList *list)
{
    TitleImage image;
    int i;
    int rc;

    memset(CacheTitles, 0, sizeof(CacheTitles));
    memset(CacheValid, 0, sizeof(CacheValid));
    snprintf(CachePath, sizeof(CachePath), "%s", list->path);
    CacheFormat = list->format;
    CacheCount = list->save_count;

    rc = open_image(list, &image);
    if (rc < 0)
        return;
    for (i = 0; i < list->save_count && i < MCI_IMAGE_SAVE_MAX; i++) {
        rc = title_for_save(&image, &list->saves[i], CacheTitles[i]);
        if (rc == 0)
            CacheValid[i] = 1u;
    }
    fileXioClose(image.fd);
}

void MciImageSaveTitleInvalidate(void)
{
    CachePath[0] = '\0';
    CacheFormat = MCI_CARD_IMAGE_VMC;
    CacheCount = -1;
    memset(CacheValid, 0, sizeof(CacheValid));
}

const char *MciImageSaveDisplayTitle(const MciImageSaveList *list, int index)
{
    if (list == NULL || index < 0 || index >= list->save_count)
        return "?";
    if (CacheCount != list->save_count || CacheFormat != list->format ||
        strcmp(CachePath, list->path) != 0)
        build_cache(list);
    if (index < MCI_IMAGE_SAVE_MAX && CacheValid[index])
        return CacheTitles[index];
    return list->saves[index].name;
}
