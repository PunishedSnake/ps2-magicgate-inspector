/* SPDX-License-Identifier: MIT */
/*
 * Drebin image filesystem browser and selective restore engine.
 *
 * Unlike exact NAND restore, this layer treats .ps2/.vmc images as immutable
 * PS2 filesystems. Top-level save directories can therefore be restored to a
 * different-capacity card without copying the source superblock/FAT geometry.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_image_fs.h"
#include "progress.h"
#include "save_title.h"

#define FS_PAGE_SIZE 512u
#define FS_PS2_STRIDE 528u
#define FS_MAX_CLUSTER_BYTES 1024u
#define FS_MODE_FILE 0x0010u
#define FS_MODE_DIR 0x0020u
#define FS_MODE_EXISTS 0x8000u
#define FS_FAT_END 0xFFFFFFFFu
#define FS_FAT_NEXT_MASK 0x7FFFFFFFu
#define FS_IMPORT_CREATED_MAX 384
#define FS_MAX_DEPTH 12
#define FS_ICON_PREFIX_MAX 1024u

#pragma pack(push, 1)
typedef struct MciFsSuperblock {
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
} MciFsSuperblock;

typedef struct MciFsDirEntry {
    u16 mode;
    u16 unused;
    u32 length;
    sceMcStDateTime created;
    u32 cluster;
    u32 dir_entry;
    sceMcStDateTime modified;
    u32 attr;
    u32 unused2[7];
    char name[32];
    u8 unused3[416];
} MciFsDirEntry;
#pragma pack(pop)

typedef struct MciFsImage {
    int fd;
    MciCardImageFormat format;
    u32 stride;
    u32 cluster_bytes;
    u32 dir_entries_per_cluster;
    u32 fat_entries_per_cluster;
    MciFsSuperblock sb;
} MciFsImage;

typedef struct MciFsStats {
    u32 bytes;
    u32 files;
    u32 dirs;
    u32 clusters;
} MciFsStats;

typedef struct MciFsDirCursor {
    u32 current_relative;
    u32 cluster_index;
    int loaded;
    unsigned char cluster[FS_MAX_CLUSTER_BYTES] __attribute__((aligned(64)));
} MciFsDirCursor;

typedef struct MciImportTxn {
    int port;
    MciFsImage *image;
    MciImageImportReport *report;
    char (*created)[MCI_IMAGE_SAVE_PATH_MAX];
    int created_count;
} MciImportTxn;

static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";

static int McResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static u32 CeilDiv(u32 value, u32 unit)
{
    return value == 0u ? 0u : (value + unit - 1u) / unit;
}

static int IsDotName(const char *name)
{
    return name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static int NameEqualCi(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int SafeEntryName(const MciFsDirEntry *entry, char out[33])
{
    unsigned int i;

    for (i = 0; i < 32u; i++) {
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

static int ReadExact(int fd, void *buffer, unsigned int size)
{
    unsigned char *p = (unsigned char *)buffer;
    unsigned int done = 0u;
    int rc;

    while (done < size) {
        rc = fileXioRead(fd, p + done, (int)(size - done));
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += (unsigned int)rc;
    }
    return 0;
}

static int ReadLogicalPage(MciFsImage *image, u32 page,
                           unsigned char data[FS_PAGE_SIZE])
{
    u64 offset = (u64)page * (u64)image->stride;
    int rc;

    if (page >= image->sb.clusters_per_card * image->sb.pages_per_cluster)
        return -1;
    rc = fileXioLseek(image->fd, (int)offset, SEEK_SET);
    if (rc < 0 || (u64)rc != offset)
        return rc < 0 ? rc : -2;
    return ReadExact(image->fd, data, FS_PAGE_SIZE);
}

static int ReadCluster(MciFsImage *image, u32 absolute_cluster,
                       unsigned char buffer[FS_MAX_CLUSTER_BYTES])
{
    u32 page;
    u32 i;
    int rc;

    if (absolute_cluster >= image->sb.clusters_per_card)
        return -1;
    memset(buffer, 0, FS_MAX_CLUSTER_BYTES);
    page = absolute_cluster * image->sb.pages_per_cluster;
    for (i = 0; i < image->sb.pages_per_cluster; i++) {
        rc = ReadLogicalPage(image, page + i, buffer + i * FS_PAGE_SIZE);
        if (rc < 0)
            return rc;
    }
    return 0;
}

static int FatEntry(MciFsImage *image, u32 relative_cluster, u32 *value)
{
    unsigned char cluster[FS_MAX_CLUSTER_BYTES];
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
    if (indirect_cluster == FS_FAT_END || indirect_cluster >= image->sb.clusters_per_card)
        return -3;
    rc = ReadCluster(image, indirect_cluster, cluster);
    if (rc < 0)
        return rc;
    memcpy(&fat_cluster, cluster + indirect_offset * sizeof(u32), sizeof(fat_cluster));
    if (fat_cluster == FS_FAT_END || fat_cluster >= image->sb.clusters_per_card)
        return -4;
    rc = ReadCluster(image, fat_cluster, cluster);
    if (rc < 0)
        return rc;
    memcpy(value, cluster + fat_offset * sizeof(u32), sizeof(*value));
    return 0;
}

static int NextRelativeCluster(MciFsImage *image, u32 current, u32 *next)
{
    u32 fat;
    int rc = FatEntry(image, current, &fat);
    if (rc < 0)
        return rc;
    if (fat == FS_FAT_END)
        return 1;
    if ((fat & 0x80000000u) == 0u)
        return -5;
    *next = fat & FS_FAT_NEXT_MASK;
    if (*next >= image->sb.alloc_end || *next == current)
        return -6;
    return 0;
}

static void DirCursorInit(MciFsDirCursor *cursor, u32 start_relative)
{
    memset(cursor, 0, sizeof(*cursor));
    cursor->current_relative = start_relative;
}

/* Sequential directory access. The previous ReadDirEntryAt() started from the
 * directory's first cluster for every entry index and re-walked the same FAT
 * prefix. Scans/imports consume entries monotonically, so retain ownership of
 * the current cluster and advance only when the index crosses a cluster edge. */
static int DirCursorRead(MciFsImage *image, MciFsDirCursor *cursor,
                         u32 index, MciFsDirEntry *entry)
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
        rc = NextRelativeCluster(image, cursor->current_relative, &next);
        if (rc != 0)
            return -3;
        cursor->current_relative = next;
        cursor->cluster_index++;
        cursor->loaded = 0;
    }

    if (!cursor->loaded) {
        rc = ReadCluster(image, image->sb.alloc_offset + cursor->current_relative,
                         cursor->cluster);
        if (rc < 0)
            return rc;
        cursor->loaded = 1;
    }

    memcpy(entry, cursor->cluster + slot * sizeof(MciFsDirEntry), sizeof(*entry));
    return 0;
}

static int ReadFilePrefix(MciFsImage *image, const MciFsDirEntry *entry,
                          unsigned char *buffer, unsigned int capacity,
                          unsigned int *read_size)
{
    unsigned char cluster[FS_MAX_CLUSTER_BYTES] __attribute__((aligned(64)));
    u32 current = entry->cluster;
    u32 remaining = entry->length;
    unsigned int done = 0u;
    int rc;

    if (buffer == NULL || read_size == NULL)
        return -1;
    if (remaining > capacity)
        remaining = capacity;

    while (remaining > 0u) {
        u32 chunk;
        if (current == FS_FAT_END || current >= image->sb.alloc_end)
            return -2;
        rc = ReadCluster(image, image->sb.alloc_offset + current, cluster);
        if (rc < 0)
            return rc;
        chunk = remaining > image->cluster_bytes ? image->cluster_bytes : remaining;
        memcpy(buffer + done, cluster, chunk);
        done += chunk;
        remaining -= chunk;
        if (remaining > 0u) {
            u32 next;
            rc = NextRelativeCluster(image, current, &next);
            if (rc != 0)
                return -3;
            current = next;
        }
    }
    *read_size = done;
    return 0;
}

static int OpenImage(const char *path, MciCardImageFormat format,
                     MciFsImage *image, MciCardGeometry *geometry)
{
    MciCardImageReport verify;
    unsigned char first[FS_PAGE_SIZE];
    int rc;

    memset(image, 0, sizeof(*image));
    image->fd = -1;
    rc = MciCardImageVerifyFile(path, format, &verify);
    if (rc < 0)
        return rc;

    image->format = format;
    image->stride = format == MCI_CARD_IMAGE_PS2 ? FS_PS2_STRIDE : FS_PAGE_SIZE;
    image->fd = fileXioOpen(path, FIO_O_RDONLY);
    if (image->fd < 0)
        return image->fd;

    /* Read page zero before sb geometry is available. */
    rc = fileXioLseek(image->fd, 0, SEEK_SET);
    if (rc < 0 || ReadExact(image->fd, first, sizeof(first)) < 0) {
        fileXioClose(image->fd);
        image->fd = -1;
        return -2;
    }
    memcpy(&image->sb, first, sizeof(image->sb));
    /* pages_per_block is physical erase geometry, not a filesystem-browser
     * constraint. The browser never erases the source image. */
    if (memcmp(image->sb.magic, SuperblockMagic, sizeof(SuperblockMagic)) != 0 ||
        image->sb.page_len != FS_PAGE_SIZE ||
        (image->sb.pages_per_cluster != 1u && image->sb.pages_per_cluster != 2u) ||
        image->sb.pages_per_block == 0u ||
        image->sb.clusters_per_card == 0u ||
        image->sb.alloc_offset >= image->sb.clusters_per_card ||
        image->sb.alloc_end == 0u ||
        image->sb.alloc_offset + image->sb.alloc_end > image->sb.clusters_per_card ||
        image->sb.rootdir_cluster != 0u) {
        fileXioClose(image->fd);
        image->fd = -1;
        return -3;
    }
    image->cluster_bytes = image->sb.page_len * image->sb.pages_per_cluster;
    if (image->cluster_bytes == 0u || image->cluster_bytes > FS_MAX_CLUSTER_BYTES ||
        (image->cluster_bytes % sizeof(MciFsDirEntry)) != 0u) {
        fileXioClose(image->fd);
        image->fd = -1;
        return -4;
    }
    image->dir_entries_per_cluster = image->cluster_bytes / sizeof(MciFsDirEntry);
    image->fat_entries_per_cluster = image->cluster_bytes / sizeof(u32);
    if (geometry != NULL)
        *geometry = verify.geometry;
    return 0;
}

static void CloseImage(MciFsImage *image)
{
    if (image->fd >= 0)
        fileXioClose(image->fd);
    image->fd = -1;
}

static int AccumulateDirectory(MciFsImage *image, u32 start_relative,
                               u32 entry_count, MciFsStats *stats, int depth,
                               char *display_title, unsigned int title_size)
{
    MciFsDirCursor cursor;
    MciFsDirEntry entry;
    char name[33];
    u32 i;
    int rc;

    if (depth > FS_MAX_DEPTH || start_relative >= image->sb.alloc_end)
        return -1;
    stats->dirs++;
    stats->clusters += CeilDiv(entry_count, image->dir_entries_per_cluster);
    DirCursorInit(&cursor, start_relative);

    for (i = 2u; i < entry_count; i++) {
        rc = DirCursorRead(image, &cursor, i, &entry);
        if (rc < 0)
            return rc;
        if ((entry.mode & FS_MODE_EXISTS) == 0u)
            continue;
        if (SafeEntryName(&entry, name) < 0 || IsDotName(name))
            continue;
        if (entry.mode & FS_MODE_FILE) {
            stats->files++;
            stats->bytes += entry.length;
            stats->clusters += CeilDiv(entry.length, image->cluster_bytes);

            /* Presentation metadata belongs to this index pass. Decode only the
             * top-level save's icon.sys and keep failure non-fatal: a missing or
             * unusual title must never make an otherwise browseable save invalid. */
            if (depth == 0 && display_title != NULL && title_size > 0u &&
                display_title[0] == '\0' && NameEqualCi(name, "icon.sys")) {
                unsigned char icon[FS_ICON_PREFIX_MAX] __attribute__((aligned(64)));
                unsigned int got = 0u;
                if (ReadFilePrefix(image, &entry, icon, sizeof(icon), &got) == 0)
                    (void)MciSaveTitleDecodeIconSys(icon, got,
                                                    display_title, title_size);
            }
        } else if (entry.mode & FS_MODE_DIR) {
            if (entry.cluster == FS_FAT_END || entry.cluster >= image->sb.alloc_end)
                return -2;
            rc = AccumulateDirectory(image, entry.cluster, entry.length,
                                     stats, depth + 1, NULL, 0u);
            if (rc < 0)
                return rc;
        }
    }
    return 0;
}

const char *MciImageFsResultText(MciImageFsResult result)
{
    switch (result) {
        case MCI_IMAGE_FS_OK: return "PASS";
        case MCI_IMAGE_FS_IMAGE_INVALID: return "IMAGE INVALID";
        case MCI_IMAGE_FS_CORRUPT: return "FILESYSTEM CORRUPT";
        case MCI_IMAGE_FS_TOO_MANY_SAVES: return "SAVE LIST TRUNCATED";
        case MCI_IMAGE_FS_TARGET_UNAVAILABLE: return "TARGET CARD UNAVAILABLE";
        case MCI_IMAGE_FS_TARGET_FULL: return "TARGET CARD FULL";
        case MCI_IMAGE_FS_CONFLICT: return "SAVE ALREADY EXISTS";
        case MCI_IMAGE_FS_IMPORT_FAILED: return "IMPORT FAILED";
        case MCI_IMAGE_FS_ROLLBACK_FAILED: return "ROLLBACK FAILED";
        default: return "NOT RUN";
    }
}

int MciImageFsScan(const char *path, MciCardImageFormat format,
                   MciImageSaveList *list)
{
    MciFsImage image;
    MciFsDirCursor root_cursor;
    MciFsDirEntry root;
    MciFsDirEntry entry;
    MciFsStats stats;
    char name[33];
    u32 i;
    int rc;

    if (path == NULL || list == NULL)
        return -1;
    memset(list, 0, sizeof(*list));
    list->format = format;
    snprintf(list->path, sizeof(list->path), "%s", path);
    list->result = MCI_IMAGE_FS_NOT_RUN;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 5, "Opening memory-card image",
                      "Verifying the complete image first, then parsing its PS2 superblock, indirect FAT and root directory.");
    rc = OpenImage(path, format, &image, &list->geometry);
    if (rc < 0) {
        list->result = MCI_IMAGE_FS_IMAGE_INVALID;
        return rc;
    }

    DirCursorInit(&root_cursor, image.sb.rootdir_cluster);
    rc = DirCursorRead(&image, &root_cursor, 0u, &root);
    if (rc < 0 || root.length < 2u || root.length > image.sb.alloc_end * image.dir_entries_per_cluster) {
        CloseImage(&image);
        list->result = MCI_IMAGE_FS_CORRUPT;
        return rc < 0 ? rc : -2;
    }

    for (i = 2u; i < root.length; i++) {
        MciImageSaveEntry *save;
        char display_title[MCI_SAVE_TITLE_MAX];
        int percent;

        rc = DirCursorRead(&image, &root_cursor, i, &entry);
        if (rc < 0) {
            CloseImage(&image);
            list->result = MCI_IMAGE_FS_CORRUPT;
            return rc;
        }
        if ((entry.mode & (FS_MODE_EXISTS | FS_MODE_DIR)) !=
            (FS_MODE_EXISTS | FS_MODE_DIR))
            continue;
        if (SafeEntryName(&entry, name) < 0 || IsDotName(name))
            continue;
        if (list->save_count >= MCI_IMAGE_SAVE_MAX) {
            list->truncated = 1;
            break;
        }
        if (entry.cluster == FS_FAT_END || entry.cluster >= image.sb.alloc_end) {
            CloseImage(&image);
            list->result = MCI_IMAGE_FS_CORRUPT;
            return -3;
        }

        memset(&stats, 0, sizeof(stats));
        memset(display_title, 0, sizeof(display_title));
        rc = AccumulateDirectory(&image, entry.cluster, entry.length, &stats, 0,
                                 display_title, sizeof(display_title));
        if (rc < 0) {
            CloseImage(&image);
            list->result = MCI_IMAGE_FS_CORRUPT;
            return rc;
        }
        save = &list->saves[list->save_count++];
        snprintf(save->name, sizeof(save->name), "%s", name);
        if (display_title[0] != '\0')
            snprintf(save->display_title, sizeof(save->display_title), "%s",
                     display_title);
        save->mode = entry.mode;
        memcpy(save->created, &entry.created, sizeof(save->created));
        memcpy(save->modified, &entry.modified, sizeof(save->modified));
        save->attr = entry.attr;
        save->start_cluster = entry.cluster;
        save->entry_count = entry.length;
        save->total_bytes = stats.bytes;
        save->file_count = stats.files;
        save->directory_count = stats.dirs;
        save->required_clusters = stats.clusters;
        list->total_bytes += stats.bytes;

        percent = 10 + (int)((i * 85u) / root.length);
        MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, percent,
                          "Scanning save directories",
                          "Walking directory entries and FAT chains without modifying the image or inserted card.");
    }
    CloseImage(&image);
    list->result = list->truncated ? MCI_IMAGE_FS_TOO_MANY_SAVES : MCI_IMAGE_FS_OK;
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100, "Image browser index ready",
                      "Top-level PS2 directories and available icon.sys titles are indexed in one pass.");
    return 0;
}

static int TargetExists(int port, const char *path)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int rc;

    memset(&info, 0, sizeof(info));
    mcGetDir(port, 0, path, 0, 1, &info);
    rc = McResult();
    if (rc > 0)
        return 1;
    if (rc == 0 || rc == sceMcResNoEntry)
        return 0;
    return rc;
}

int MciImageFsRefreshTargetConflicts(int target_port, MciImageSaveList *list)
{
    int i;
    int conflicts = 0;

    if (list == NULL)
        return -1;
    for (i = 0; i < list->save_count; i++) {
        char path[MCI_IMAGE_SAVE_PATH_MAX];
        int rc;
        snprintf(path, sizeof(path), "/%s", list->saves[i].name);
        rc = TargetExists(target_port, path);
        if (rc < 0)
            return rc;
        list->saves[i].conflict = rc == 1;
        if (list->saves[i].conflict)
            conflicts++;
    }
    return conflicts;
}

static int RecordCreated(MciImportTxn *txn, const char *path)
{
    if (txn->created_count >= FS_IMPORT_CREATED_MAX)
        return -1;
    snprintf(txn->created[txn->created_count], MCI_IMAGE_SAVE_PATH_MAX,
             "%s", path);
    txn->created_count++;
    return 0;
}

static int SetMetadata(int port, const char *path, const MciFsDirEntry *entry)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int rc;

    memset(&info, 0, sizeof(info));
    info._Create = entry->created;
    info._Modify = entry->modified;
    info.AttrFile = entry->mode;
    mcSetFileInfo(port, 0, path, &info,
                  sceMcFileInfoCreate | sceMcFileInfoModify | sceMcFileInfoAttr);
    rc = McResult();
    return rc;
}

static int BuildChildPath(const char *parent, const char *name,
                          char *out, unsigned int out_size)
{
    int written = snprintf(out, out_size, "%s/%s", parent, name);
    if (written <= 0 || (unsigned int)written >= out_size)
        return -1;
    return 0;
}

static int VerifyImportedFile(MciImportTxn *txn, const MciFsDirEntry *entry,
                              const char *path)
{
    unsigned char source[FS_MAX_CLUSTER_BYTES] __attribute__((aligned(64)));
    unsigned char target[FS_MAX_CLUSTER_BYTES] __attribute__((aligned(64)));
    u32 current = entry->cluster;
    u32 remaining = entry->length;
    u32 next;
    int fd;
    int rc;

    mcOpen(txn->port, 0, path, FIO_O_RDONLY);
    fd = McResult();
    if (fd < 0)
        return fd;

    while (remaining > 0u) {
        u32 chunk = remaining > txn->image->cluster_bytes
                        ? txn->image->cluster_bytes : remaining;
        if (current == FS_FAT_END || current >= txn->image->sb.alloc_end) {
            mcClose(fd); (void)McResult();
            return -30;
        }
        rc = ReadCluster(txn->image, txn->image->sb.alloc_offset + current,
                         source);
        if (rc < 0) {
            mcClose(fd); (void)McResult();
            return rc;
        }
        mcRead(fd, target, (int)chunk);
        rc = McResult();
        if (rc != (int)chunk || memcmp(source, target, chunk) != 0) {
            mcClose(fd); (void)McResult();
            return rc < 0 ? rc : -31;
        }
        remaining -= chunk;
        if (remaining > 0u) {
            rc = NextRelativeCluster(txn->image, current, &next);
            if (rc != 0) {
                mcClose(fd); (void)McResult();
                return -32;
            }
            current = next;
        }
    }
    mcRead(fd, target, 1);
    rc = McResult();
    if (rc != 0) {
        mcClose(fd); (void)McResult();
        return rc < 0 ? rc : -33;
    }
    mcClose(fd);
    return McResult();
}

static int ImportFile(MciImportTxn *txn, const MciFsDirEntry *entry,
                      const char *path)
{
    unsigned char cluster[FS_MAX_CLUSTER_BYTES] __attribute__((aligned(64)));
    u32 current = entry->cluster;
    u32 remaining = entry->length;
    u32 next;
    int fd;
    int rc;

    mcOpen(txn->port, 0, path, FIO_O_WRONLY | FIO_O_CREAT);
    fd = McResult();
    if (fd < 0)
        return fd;
    if (RecordCreated(txn, path) < 0) {
        mcClose(fd);
        (void)McResult();
        return -1;
    }

    while (remaining > 0u) {
        u32 chunk;
        if (current == FS_FAT_END || current >= txn->image->sb.alloc_end) {
            mcClose(fd);
            (void)McResult();
            return -2;
        }
        rc = ReadCluster(txn->image, txn->image->sb.alloc_offset + current,
                         cluster);
        if (rc < 0) {
            mcClose(fd);
            (void)McResult();
            return rc;
        }
        chunk = remaining > txn->image->cluster_bytes
                    ? txn->image->cluster_bytes : remaining;
        mcWrite(fd, cluster, (int)chunk);
        rc = McResult();
        if (rc != (int)chunk) {
            mcClose(fd);
            (void)McResult();
            return rc < 0 ? rc : -3;
        }
        txn->report->bytes_written += chunk;
        remaining -= chunk;
        if (remaining > 0u) {
            rc = NextRelativeCluster(txn->image, current, &next);
            if (rc != 0) {
                mcClose(fd);
                (void)McResult();
                return -4;
            }
            current = next;
        }
    }
    mcFlush(fd);
    rc = McResult();
    mcClose(fd);
    if (McResult() < 0 && rc >= 0)
        rc = -5;
    if (rc < 0)
        return rc;
    rc = VerifyImportedFile(txn, entry, path);
    if (rc < 0)
        return rc;
    rc = SetMetadata(txn->port, path, entry);
    if (rc < 0)
        return rc;
    txn->report->files_written++;
    return 0;
}

static int ImportDirectory(MciImportTxn *txn, const MciFsDirEntry *dir_entry,
                           const char *path, int depth)
{
    MciFsDirCursor cursor;
    MciFsDirEntry entry;
    char name[33];
    char child[MCI_IMAGE_SAVE_PATH_MAX];
    u32 i;
    int rc;

    if (depth > FS_MAX_DEPTH)
        return -1;
    mcMkDir(txn->port, 0, path);
    rc = McResult();
    if (rc < 0)
        return rc;
    if (RecordCreated(txn, path) < 0)
        return -2;
    txn->report->directories_written++;

    DirCursorInit(&cursor, dir_entry->cluster);
    for (i = 2u; i < dir_entry->length; i++) {
        rc = DirCursorRead(txn->image, &cursor, i, &entry);
        if (rc < 0)
            return rc;
        if ((entry.mode & FS_MODE_EXISTS) == 0u)
            continue;
        if (SafeEntryName(&entry, name) < 0 || IsDotName(name))
            continue;
        if (BuildChildPath(path, name, child, sizeof(child)) < 0)
            return -3;
        if (entry.mode & FS_MODE_FILE) {
            rc = ImportFile(txn, &entry, child);
        } else if (entry.mode & FS_MODE_DIR) {
            if (entry.cluster == FS_FAT_END || entry.cluster >= txn->image->sb.alloc_end)
                return -4;
            rc = ImportDirectory(txn, &entry, child, depth + 1);
        } else {
            continue;
        }
        if (rc < 0) {
            snprintf(txn->report->failed_path,
                     sizeof(txn->report->failed_path), "%s", child);
            return rc;
        }
    }
    return SetMetadata(txn->port, path, dir_entry);
}

static int RollbackCreated(MciImportTxn *txn)
{
    int i;
    int first_error = 0;

    for (i = txn->created_count - 1; i >= 0; i--) {
        int rc;
        mcDelete(txn->port, 0, txn->created[i]);
        rc = McResult();
        if (rc < 0 && rc != sceMcResNoEntry && first_error == 0)
            first_error = rc;
    }
    return first_error;
}

static int TargetCardInfo(int port, int *free_clusters)
{
    int type = MC_TYPE_NONE;
    int free = 0;
    int formatted = 0;
    int rc;

    mcGetInfo(port, 0, &type, &free, &formatted);
    rc = McResult();
    if (rc <= -10 || type != MC_TYPE_PS2 || !formatted)
        return rc <= -10 ? rc : -1;
    *free_clusters = free;
    return 0;
}

int MciImageFsImportSelected(int target_port, MciImageSaveList *list,
                             MciImageImportReport *report)
{
    MciFsImage image;
    MciImportTxn txn;
    MciFsDirEntry top;
    u32 required = 0u;
    int free_clusters = 0;
    int conflicts;
    int i;
    int rc;

    if (list == NULL || report == NULL)
        return -1;
    memset(report, 0, sizeof(*report));
    report->target_port = target_port;
    report->failed_save_index = -1;
    report->result = MCI_IMAGE_FS_NOT_RUN;

    for (i = 0; i < list->save_count; i++) {
        if (list->saves[i].selected) {
            report->selected_saves++;
            required += list->saves[i].required_clusters;
        }
    }
    report->required_clusters = required;
    if (report->selected_saves == 0)
        return -2;

    rc = TargetCardInfo(target_port, &free_clusters);
    report->target_free_clusters = free_clusters;
    if (rc < 0) {
        report->result = MCI_IMAGE_FS_TARGET_UNAVAILABLE;
        return rc;
    }
    conflicts = MciImageFsRefreshTargetConflicts(target_port, list);
    if (conflicts < 0) {
        report->result = MCI_IMAGE_FS_TARGET_UNAVAILABLE;
        return conflicts;
    }
    for (i = 0; i < list->save_count; i++) {
        if (list->saves[i].selected && list->saves[i].conflict)
            report->conflict_saves++;
    }
    if (report->conflict_saves > 0) {
        report->result = MCI_IMAGE_FS_CONFLICT;
        return -3;
    }
    if (required > (u32)free_clusters) {
        report->result = MCI_IMAGE_FS_TARGET_FULL;
        return -4;
    }

    rc = OpenImage(list->path, list->format, &image, NULL);
    if (rc < 0) {
        report->result = MCI_IMAGE_FS_IMAGE_INVALID;
        return rc;
    }
    memset(&txn, 0, sizeof(txn));
    txn.port = target_port;
    txn.image = &image;
    txn.report = report;
    txn.created = malloc(FS_IMPORT_CREATED_MAX * MCI_IMAGE_SAVE_PATH_MAX);
    if (txn.created == NULL) {
        CloseImage(&image);
        report->result = MCI_IMAGE_FS_IMPORT_FAILED;
        return -ENOMEM;
    }

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 2, "Restoring selected saves",
                      "Source filesystem verified. Creating only the selected top-level directories on the destination card.");
    for (i = 0; i < list->save_count; i++) {
        MciImageSaveEntry *save = &list->saves[i];
        char path[MCI_IMAGE_SAVE_PATH_MAX];
        int percent;

        if (!save->selected)
            continue;
        memset(&top, 0, sizeof(top));
        top.mode = save->mode;
        top.length = save->entry_count;
        top.cluster = save->start_cluster;
        memcpy(&top.created, save->created, sizeof(save->created));
        memcpy(&top.modified, save->modified, sizeof(save->modified));
        top.attr = save->attr;
        snprintf(top.name, sizeof(top.name), "%s", save->name);
        snprintf(path, sizeof(path), "/%s", save->name);
        report->failed_save_index = i;
        snprintf(report->failed_path, sizeof(report->failed_path), "%s", path);
        rc = ImportDirectory(&txn, &top, path, 0);
        if (rc < 0)
            goto fail;
        report->restored_saves++;
        percent = 5 + (report->restored_saves * 90) / report->selected_saves;
        MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, percent,
                          "Restoring selected saves",
                          "Files are streamed from the image FAT chains into a freshly created destination save directory.");
    }

    free(txn.created);
    CloseImage(&image);
    report->failed_save_index = -1;
    report->failed_path[0] = '\0';
    report->result = MCI_IMAGE_FS_OK;
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100, "Selective restore complete",
                      "Selected saves were restored without changing the destination card geometry or unrelated saves.");
    return 0;

fail:
    report->rollback_rc = RollbackCreated(&txn);
    free(txn.created);
    CloseImage(&image);
    report->result = report->rollback_rc < 0
                         ? MCI_IMAGE_FS_ROLLBACK_FAILED
                         : MCI_IMAGE_FS_IMPORT_FAILED;
    return rc;
}
