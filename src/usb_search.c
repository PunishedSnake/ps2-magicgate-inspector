/* SPDX-License-Identifier: MIT */
/* Bounded recursive FMCB source discovery for USB mass storage. */

#define NEWLIB_PORT_AWARE

#include <delaythread.h>
#include <fileXio_rpc.h>
#include <iox_stat.h>
#include <stdio.h>
#include <string.h>

#include "usb_search.h"

static const char *const SearchRoots[] = {
    "mass:/",
    "mass0:/",
    "mass1:/"
};

static char VerifiedPackageRoot[MCI_USB_SEARCH_PATH_MAX];

typedef struct SearchState {
    const char *filename;
    int require_system_parent;
    char *out_path;
    unsigned int out_size;
    unsigned int dirs_scanned;
    MciUsbSearchProgress progress;
    void *userdata;
} SearchState;

static int ascii_lower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static int name_equal_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *last_component(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static int parent_is_system(const char *directory)
{
    return name_equal_ci(last_component(directory), "SYSTEM");
}

static int entry_name_safe(const char *name, int is_directory)
{
    const unsigned char *p = (const unsigned char *)name;

    if (name == NULL || name[0] == '\0')
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;

    /* FMCB package discovery has no reason to enter hidden/pseudo directories.
     * In particular, this prevents malformed USBHDFSD/FAT directory aliases
     * such as repeated `.as/.as/...` from turning into a fake recursive tree. */
    if (is_directory && name[0] == '.')
        return 0;

    while (*p != '\0') {
        if (*p < 0x20u || *p == '/' || *p == '\\' || *p == ':')
            return 0;
        p++;
    }
    return 1;
}

static int join_path(const char *directory, const char *name,
                     char *out, unsigned int out_size)
{
    unsigned int len;
    int written;

    if (directory == NULL || name == NULL || out == NULL || out_size == 0)
        return -1;
    len = (unsigned int)strlen(directory);
    written = snprintf(out, out_size, "%s%s%s", directory,
                       len > 0 && directory[len - 1] == '/' ? "" : "/", name);
    return (written >= 0 && (unsigned int)written < out_size) ? 0 : -2;
}

int MciUsbWaitForStorage(unsigned int attempts, unsigned int delay_usec)
{
    unsigned int attempt;
    unsigned int i;

    if (attempts == 0u)
        attempts = 1u;
    for (attempt = 0; attempt < attempts; attempt++) {
        for (i = 0; i < sizeof(SearchRoots) / sizeof(SearchRoots[0]); i++) {
            int fd = fileXioDopen(SearchRoots[i]);
            if (fd >= 0) {
                fileXioDclose(fd);
                return 0;
            }
        }
        if (attempt + 1u < attempts && delay_usec > 0u)
            DelayThread(delay_usec);
    }
    return -1;
}

static int search_directory(const char *directory, unsigned int depth,
                            SearchState *state)
{
    iox_dirent_t entry;
    char child[MCI_USB_SEARCH_PATH_MAX];
    int fd;
    int rc;

    if (depth > MCI_USB_SEARCH_MAX_DEPTH ||
        state->dirs_scanned >= MCI_USB_SEARCH_MAX_DIRS)
        return 0;

    fd = fileXioDopen(directory);
    if (fd < 0)
        return 0;

    state->dirs_scanned++;
    if (state->progress != NULL &&
        (state->dirs_scanned == 1u || (state->dirs_scanned & 7u) == 0u))
        state->progress(directory, state->dirs_scanned, state->userdata);

    for (;;) {
        int is_directory;
        int is_regular;

        memset(&entry, 0, sizeof(entry));
        rc = fileXioDread(fd, &entry);
        if (rc <= 0)
            break;
        entry.name[sizeof(entry.name) - 1] = '\0';
        is_directory = FIO_S_ISDIR(entry.stat.mode);
        is_regular = FIO_S_ISREG(entry.stat.mode);
        if (!entry_name_safe(entry.name, is_directory))
            continue;
        if (join_path(directory, entry.name, child, sizeof(child)) < 0)
            continue;

        if (is_regular && name_equal_ci(entry.name, state->filename)) {
            if (!state->require_system_parent || parent_is_system(directory)) {
                snprintf(state->out_path, state->out_size, "%s", child);
                fileXioDclose(fd);
                return 1;
            }
        } else if (is_directory && depth < MCI_USB_SEARCH_MAX_DEPTH &&
                   state->dirs_scanned < MCI_USB_SEARCH_MAX_DIRS) {
            rc = search_directory(child, depth + 1u, state);
            if (rc == 1) {
                fileXioDclose(fd);
                return 1;
            }
        }
    }

    fileXioDclose(fd);
    return 0;
}

int MciUsbFindFmcbXlf(char *out_path, unsigned int out_size,
                      int require_system_parent,
                      MciUsbSearchProgress progress, void *userdata)
{
    SearchState state;
    unsigned int i;

    if (out_path == NULL || out_size < 16u)
        return -1;
    out_path[0] = '\0';

    memset(&state, 0, sizeof(state));
    state.filename = "FMCB.XLF";
    state.require_system_parent = require_system_parent;
    state.out_path = out_path;
    state.out_size = out_size;
    state.progress = progress;
    state.userdata = userdata;

    for (i = 0; i < sizeof(SearchRoots) / sizeof(SearchRoots[0]); i++) {
        if (search_directory(SearchRoots[i], 0, &state) == 1)
            return 0;
        if (state.dirs_scanned >= MCI_USB_SEARCH_MAX_DIRS)
            break;
    }
    return -2;
}

int MciUsbPackageRootFromXlf(const char *xlf_path,
                             char *out_root, unsigned int out_size)
{
    char temp[MCI_USB_SEARCH_PATH_MAX];
    char *slash;
    char *system_component;

    if (xlf_path == NULL || out_root == NULL || out_size == 0)
        return -1;
    if (strlen(xlf_path) >= sizeof(temp))
        return -2;
    snprintf(temp, sizeof(temp), "%s", xlf_path);

    slash = strrchr(temp, '/');
    if (slash == NULL || !name_equal_ci(slash + 1, "FMCB.XLF"))
        return -3;
    *slash = '\0';
    system_component = (char *)last_component(temp);
    if (!name_equal_ci(system_component, "SYSTEM"))
        return -4;

    slash = strrchr(temp, '/');
    if (slash == NULL)
        return -5;
    *slash = '\0';
    if (temp[0] == '\0')
        return -6;
    if (snprintf(out_root, out_size, "%s", temp) < 0 ||
        strlen(temp) >= out_size)
        return -7;
    return 0;
}

void MciUsbClearVerifiedPackageRoot(void)
{
    VerifiedPackageRoot[0] = '\0';
}

int MciUsbRememberVerifiedPackageRoot(const char *root)
{
    if (root == NULL || root[0] == '\0' ||
        strlen(root) >= sizeof(VerifiedPackageRoot))
        return -1;
    snprintf(VerifiedPackageRoot, sizeof(VerifiedPackageRoot), "%s", root);
    return 0;
}

int MciUsbGetVerifiedPackageRoot(char *out_root, unsigned int out_size)
{
    iox_stat_t stat;
    char xlf[MCI_USB_SEARCH_PATH_MAX];

    if (out_root == NULL || out_size == 0 || VerifiedPackageRoot[0] == '\0')
        return -1;
    if (join_path(VerifiedPackageRoot, "SYSTEM/FMCB.XLF", xlf,
                  sizeof(xlf)) < 0) {
        MciUsbClearVerifiedPackageRoot();
        return -2;
    }
    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(xlf, &stat) < 0 || !FIO_S_ISREG(stat.mode) ||
        stat.size == 0u) {
        MciUsbClearVerifiedPackageRoot();
        return -3;
    }
    if (strlen(VerifiedPackageRoot) >= out_size)
        return -4;
    snprintf(out_root, out_size, "%s", VerifiedPackageRoot);
    return 0;
}
