/* SPDX-License-Identifier: MIT */
/* Bounded, non-recursive USB browser used by Card Tools. */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <iox_stat.h>
#include <stdio.h>
#include <string.h>

#include "usb_file_picker.h"

static const char *const PickerRoots[] = {
    "mass:/",
    "mass0:/",
    "mass1:/"
};

static int ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int compare_names(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        int ca = ascii_lower((unsigned char)*a++);
        int cb = ascii_lower((unsigned char)*b++);
        if (ca != cb)
            return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int safe_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    if (name == NULL || name[0] == '\0' ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    while (*p != '\0') {
        if (*p < 0x20u || *p == '/' || *p == '\\' || *p == ':')
            return 0;
        p++;
    }
    return 1;
}

static int ends_with_icase(const char *text, const char *suffix)
{
    size_t tl = strlen(text);
    size_t sl = strlen(suffix);
    size_t i;
    if (tl < sl)
        return 0;
    text += tl - sl;
    for (i = 0; i < sl; i++)
        if (ascii_lower((unsigned char)text[i]) !=
            ascii_lower((unsigned char)suffix[i]))
            return 0;
    return 1;
}

static int accept_file(const char *name, MciUsbPickerFilter filter,
                       MciSaveTransferFormat *format)
{
    MciSaveTransferFormat f = MciSaveTransferFormatFromPath(name);
    MciSaveTransferFamily family = MciSaveTransferFormatFamily(f);

    if (filter == MCI_USB_PICKER_IMAGE_PS2)
        return f == MCI_SAVE_FORMAT_IMAGE_PS2 ? (*format = f, 1) : 0;
    if (filter == MCI_USB_PICKER_IMAGE_VMC)
        return f == MCI_SAVE_FORMAT_IMAGE_VMC ? (*format = f, 1) : 0;
    if (filter == MCI_USB_PICKER_IMAGE_ANY)
        return family == MCI_SAVE_FAMILY_FULL_PS2_IMAGE ? (*format = f, 1) : 0;
    if (filter == MCI_USB_PICKER_SAVE_ANY) {
        if (family == MCI_SAVE_FAMILY_PS2_SAVE ||
            family == MCI_SAVE_FAMILY_PS1_SAVE) {
            *format = f;
            return 1;
        }
        /* PSV generation is identified from its header later. Keep it visible
         * in the unified importer instead of hiding it because extension-only
         * probing intentionally leaves PSV ambiguous. */
        if (ends_with_icase(name, ".psv")) {
            *format = MCI_SAVE_FORMAT_UNKNOWN;
            return 1;
        }
    }
    return 0;
}

static int join_path(const char *directory, const char *name,
                     char *out, unsigned int out_size)
{
    size_t len = strlen(directory);
    int written = snprintf(out, out_size, "%s%s%s", directory,
                           len > 0 && directory[len - 1] == '/' ? "" : "/",
                           name);
    return written >= 0 && (unsigned int)written < out_size ? 0 : -1;
}

static void sort_entries(MciUsbPickerList *list)
{
    int i;
    int j;
    for (i = 1; i < list->entry_count; i++) {
        MciUsbPickerEntry key = list->entries[i];
        j = i - 1;
        while (j >= 0) {
            const MciUsbPickerEntry *cur = &list->entries[j];
            int move = 0;
            if (cur->is_directory != key.is_directory)
                move = !cur->is_directory && key.is_directory;
            else
                move = compare_names(cur->name, key.name) > 0;
            if (!move)
                break;
            list->entries[j + 1] = list->entries[j];
            j--;
        }
        list->entries[j + 1] = key;
    }
}

const char *MciUsbPickerRootName(int root_index)
{
    if (root_index < 0 || root_index >= (int)(sizeof(PickerRoots) / sizeof(PickerRoots[0])))
        return NULL;
    return PickerRoots[root_index];
}

int MciUsbPickerScan(const char *path, int root_index,
                     MciUsbPickerFilter filter, MciUsbPickerList *list)
{
    iox_dirent_t dirent;
    int fd;
    int rc;

    if (path == NULL || list == NULL || MciUsbPickerRootName(root_index) == NULL)
        return -1;
    memset(list, 0, sizeof(*list));
    list->root_index = root_index;
    list->filter = filter;
    if (strlen(path) >= sizeof(list->path))
        return -2;
    snprintf(list->path, sizeof(list->path), "%s", path);

    fd = fileXioDopen(path);
    if (fd < 0)
        return fd;
    for (;;) {
        MciUsbPickerEntry *out;
        int is_dir;
        int is_file;
        MciSaveTransferFormat format = MCI_SAVE_FORMAT_UNKNOWN;

        memset(&dirent, 0, sizeof(dirent));
        rc = fileXioDread(fd, &dirent);
        if (rc <= 0)
            break;
        dirent.name[sizeof(dirent.name) - 1] = '\0';
        is_dir = FIO_S_ISDIR(dirent.stat.mode);
        is_file = FIO_S_ISREG(dirent.stat.mode);
        if (!safe_name(dirent.name))
            continue;
        if (!is_dir && (!is_file || !accept_file(dirent.name, filter, &format)))
            continue;
        if (list->entry_count >= MCI_USB_PICKER_MAX_ENTRIES) {
            list->truncated = 1;
            continue;
        }
        out = &list->entries[list->entry_count];
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%s", dirent.name);
        if (join_path(path, dirent.name, out->path, sizeof(out->path)) < 0)
            continue;
        out->is_directory = is_dir;
        out->size = is_file ? (u64)dirent.stat.size : 0u;
        out->format = format;
        list->entry_count++;
    }
    fileXioDclose(fd);
    if (rc < 0)
        return rc;
    sort_entries(list);
    return 0;
}

int MciUsbPickerOpenFirst(MciUsbPickerFilter filter, MciUsbPickerList *list)
{
    int i;
    int last = -1;
    for (i = 0; i < (int)(sizeof(PickerRoots) / sizeof(PickerRoots[0])); i++) {
        int fd = fileXioDopen(PickerRoots[i]);
        if (fd < 0) {
            last = fd;
            continue;
        }
        fileXioDclose(fd);
        return MciUsbPickerScan(PickerRoots[i], i, filter, list);
    }
    return last < 0 ? last : -1;
}

int MciUsbPickerEnter(const MciUsbPickerList *list, int index,
                      MciUsbPickerList *next)
{
    if (list == NULL || next == NULL || index < 0 || index >= list->entry_count)
        return -1;
    if (!list->entries[index].is_directory)
        return -2;
    return MciUsbPickerScan(list->entries[index].path, list->root_index,
                            list->filter, next);
}

int MciUsbPickerParent(const MciUsbPickerList *list, MciUsbPickerList *parent)
{
    const char *root;
    char path[MCI_USB_PICKER_PATH_MAX];
    char *slash;
    size_t root_len;

    if (list == NULL || parent == NULL)
        return -1;
    root = MciUsbPickerRootName(list->root_index);
    if (root == NULL)
        return -2;
    if (strcmp(list->path, root) == 0)
        return 1;
    snprintf(path, sizeof(path), "%s", list->path);
    root_len = strlen(root);
    while (strlen(path) > root_len && path[strlen(path) - 1] == '/')
        path[strlen(path) - 1] = '\0';
    slash = strrchr(path, '/');
    if (slash == NULL || (size_t)(slash - path + 1) <= root_len) {
        snprintf(path, sizeof(path), "%s", root);
    } else {
        *slash = '\0';
    }
    return MciUsbPickerScan(path, list->root_index, list->filter, parent);
}

int MciUsbPickerCycleRoot(const MciUsbPickerList *list, int direction,
                          MciUsbPickerList *next)
{
    int roots = (int)(sizeof(PickerRoots) / sizeof(PickerRoots[0]));
    int base;
    int step;

    if (list == NULL || next == NULL || direction == 0)
        return -1;
    base = list->root_index;
    for (step = 1; step <= roots; step++) {
        int index = (base + (direction > 0 ? step : -step)) % roots;
        int fd;
        if (index < 0)
            index += roots;
        fd = fileXioDopen(PickerRoots[index]);
        if (fd < 0)
            continue;
        fileXioDclose(fd);
        return MciUsbPickerScan(PickerRoots[index], index, list->filter, next);
    }
    return -2;
}
