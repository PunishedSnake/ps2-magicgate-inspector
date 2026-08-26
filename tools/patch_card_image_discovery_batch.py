#!/usr/bin/env python3
"""Apply the P0 batched image-name discovery experiment.

The checked-in card_image.c remains the direct-slot baseline. CI applies this
transform only in an isolated performance branch/worktree. Exact source guards
make drift fail closed.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one source match, found {count}")
    return text.replace(old, new, 1)


def replace_function(text: str, signature: str, replacement: str, label: str) -> str:
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"{label}: signature not found")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"{label}: opening brace not found")
    depth = 0
    end = None
    for pos in range(brace, len(text)):
        ch = text[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = pos + 1
                break
    if end is None:
        raise SystemExit(f"{label}: closing brace not found")
    return text[:start] + replacement.rstrip() + text[end:]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--probe", action="store_true")
    args = parser.parse_args()

    text = args.source.read_text(encoding="utf-8")

    if args.probe:
        text = replace_once(
            text,
            '#include "card_image.h"\n#include "card_math.h"\n#include "progress.h"\n',
            '#include "card_image.h"\n#include "card_math.h"\n#include "diag_log.h"\n#include "progress.h"\n',
            "probe include",
        )

    helper = r'''
#define IMAGE_DISCOVERY_BATCH_ENTRIES 128u
#define IMAGE_DISCOVERY_NAME_SLOTS 100u

typedef struct MciImageNameIndex {
    unsigned char occupied[IMAGE_DISCOVERY_NAME_SLOTS];
} MciImageNameIndex;

static int BuildImageNameIndex(const char *directory, int port,
                               MciCardImageFormat format,
                               MciImageNameIndex *name_index)
{
    const char *ext = format == MCI_CARD_IMAGE_PS2 ? "ps2" : "vmc";
    const unsigned int count_limit = IMAGE_DISCOVERY_BATCH_ENTRIES;
    const unsigned int bytes =
        (unsigned int)(sizeof(struct fileXioDirEntry) * count_limit);
    unsigned char *allocation;
    struct fileXioDirEntry *entries;
    char prefix[16];
    char suffix[8];
    size_t prefix_len;
    size_t suffix_len;
    int count;
    int i;

    if (directory == NULL || name_index == NULL)
        return 0;

    /* Allocator alignment and SIF/DMA destination alignment are separate
     * contracts. Reserve 63 extra bytes and explicitly align the transient
     * fileXioGetdir destination to 64 bytes; free the original allocation. */
    allocation = (unsigned char *)malloc(bytes + 63u);
    if (allocation == NULL)
        return 0;
    entries = (struct fileXioDirEntry *)
        ((((u32)allocation) + 63u) & ~63u);

    count = fileXioGetdir(directory, entries, count_limit);
    if (count < 0 || (unsigned int)count >= count_limit) {
'''
    if args.probe:
        helper += r'''        MciDiagLogTracePrintf(
            "IMAGE-DISCOVERY",
            "getdir directory=%s count=%d limit=%u authoritative=0 fallback=1",
            directory, count, count_limit);
'''
    helper += r'''        free(allocation);
        return 0;
    }

    memset(name_index, 0, sizeof(*name_index));
    snprintf(prefix, sizeof(prefix), "mc%d-", port);
    snprintf(suffix, sizeof(suffix), ".%s", ext);
    prefix_len = strlen(prefix);
    suffix_len = strlen(suffix);

    for (i = 0; i < count; i++) {
        const char *name = entries[i].filename;
        size_t name_len = strlen(name);
        int slot;

        if (name_len != prefix_len + 2u + suffix_len)
            continue;
        if (strncmp(name, prefix, prefix_len) != 0)
            continue;
        if (name[prefix_len] < '0' || name[prefix_len] > '9' ||
            name[prefix_len + 1u] < '0' || name[prefix_len + 1u] > '9')
            continue;
        if (strcmp(name + prefix_len + 2u, suffix) != 0)
            continue;
        slot = (name[prefix_len] - '0') * 10 +
               (name[prefix_len + 1u] - '0');
        name_index->occupied[slot] = 1u;
    }
'''
    if args.probe:
        helper += r'''    MciDiagLogTracePrintf(
        "IMAGE-DISCOVERY",
        "getdir directory=%s count=%d limit=%u authoritative=1 fallback=0 bytes=%u",
        directory, count, count_limit, bytes);
'''
    helper += r'''    free(allocation);
    return 1;
}

'''

    text = replace_once(
        text,
        'static int UniqueImagePath(int port, MciCardImageFormat format,\n',
        helper + 'static int UniqueImagePath(int port, MciCardImageFormat format,\n',
        "batch helper insertion",
    )

    unique = r'''static int UniqueImagePath(int port, MciCardImageFormat format,
                           char *path, unsigned int path_size)
{
    char directory[64];
    iox_stat_t stat;
    MciImageNameIndex name_index;
    const char *ext = format == MCI_CARD_IMAGE_PS2 ? "ps2" : "vmc";
    int index;

    if (EnsureImageDirectory(directory, sizeof(directory)) < 0)
        return -1;

    if (BuildImageNameIndex(directory, port, format, &name_index)) {
        for (index = 0; index < 100; index++) {
            if (!name_index.occupied[index]) {
                snprintf(path, path_size, "%s/mc%d-%02d.%s",
                         directory, port, index, ext);
                return 0;
            }
        }
        return -2;
    }

    /* Saturated/unavailable directory enumeration falls back to the exact
     * legacy GetStat search, preserving behavior for pathological directories. */
    for (index = 0; index < 100; index++) {
        snprintf(path, path_size, "%s/mc%d-%02d.%s", directory, port, index, ext);
        memset(&stat, 0, sizeof(stat));
        if (fileXioGetStat(path, &stat) < 0)
            return 0;
    }
    return -2;
}
'''
    text = replace_function(
        text,
        'static int UniqueImagePath(int port, MciCardImageFormat format,',
        unique,
        "UniqueImagePath",
    )

    latest = r'''int MciCardImageFindLatest(int port, MciCardImageFormat format,
                           char *path, unsigned int path_size)
{
    iox_stat_t stat;
    MciImageNameIndex name_index;
    const char *ext = format == MCI_CARD_IMAGE_PS2 ? "ps2" : "vmc";
    unsigned int root;
    int index;

    if (path == NULL || path_size == 0u)
        return -1;
    for (root = 0; root < sizeof(MassRoots) / sizeof(MassRoots[0]); root++) {
        char directory[64];
        int indexed;

        snprintf(directory, sizeof(directory), "%s%s",
                 MassRoots[root], IMAGE_DIR_NAME);
        indexed = BuildImageNameIndex(directory, port, format, &name_index);
        if (indexed) {
            for (index = 99; index >= 0; index--) {
                if (!name_index.occupied[index])
                    continue;
                snprintf(path, path_size, "%s/mc%d-%02d.%s",
                         directory, port, index, ext);
                /* fileXioGetdir currently exposes stat.attr, not full stat.mode.
                 * Keep one authoritative GetStat for any name we might return. */
                memset(&stat, 0, sizeof(stat));
                if (fileXioGetStat(path, &stat) >= 0 &&
                    FIO_S_ISREG(stat.mode) && stat.size > 0u)
                    return 0;
            }
            continue;
        }

        for (index = 99; index >= 0; index--) {
            snprintf(path, path_size, "%s%s/mc%d-%02d.%s",
                     MassRoots[root], IMAGE_DIR_NAME, port, index, ext);
            memset(&stat, 0, sizeof(stat));
            if (fileXioGetStat(path, &stat) >= 0 &&
                FIO_S_ISREG(stat.mode) && stat.size > 0u)
                return 0;
        }
    }
    path[0] = '\0';
    return -ENOENT;
}
'''
    text = replace_function(
        text,
        'int MciCardImageFindLatest(int port, MciCardImageFormat format,',
        latest,
        "MciCardImageFindLatest",
    )

    args.source.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
