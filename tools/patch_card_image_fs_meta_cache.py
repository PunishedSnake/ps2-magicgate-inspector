#!/usr/bin/env python3
"""Apply the P0 immutable-image FAT metadata cache experiment.

The repository source remains the readable baseline. CI applies this transformer
only to isolated performance worktrees. Every replacement is asserted so source
drift fails closed instead of silently producing a half-patched candidate.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one source match, found {count}")
    return text.replace(old, new, 1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--probe", action="store_true")
    args = parser.parse_args()

    text = args.source.read_text(encoding="utf-8")

    if args.probe:
        text = replace_once(
            text,
            '#include "card_image_fs.h"\n#include "progress.h"\n',
            '#include "card_image_fs.h"\n#include "diag_log.h"\n#include "progress.h"\n',
            "probe include",
        )

    globals_text = r'''

/* P0 immutable-image metadata cache.
 *
 * FAT chain traversal repeatedly asks for the same indirect cluster and then
 * the same FAT cluster. The source image is immutable for the lifetime of an
 * MciFsImage, so keeping one cluster from each metadata level removes repeated
 * seek/read RPCs without weakening validation or changing filesystem state.
 * The cache is global BSS rather than per-call stack storage; ownership is
 * keyed by the active MciFsImage and reset on every OpenImage(). */
static const MciFsImage *MetaCacheOwner;
static int MetaIndirectValid;
static int MetaFatValid;
static u32 MetaIndirectCluster;
static u32 MetaFatCluster;
static unsigned char MetaIndirectData[FS_MAX_CLUSTER_BYTES]
    __attribute__((aligned(64)));
static unsigned char MetaFatData[FS_MAX_CLUSTER_BYTES]
    __attribute__((aligned(64)));
'''
    if args.probe:
        globals_text += r'''static u32 MetaIndirectHits;
static u32 MetaIndirectMisses;
static u32 MetaFatHits;
static u32 MetaFatMisses;
'''

    text = replace_once(
        text,
        'static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";\n',
        'static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";\n'
        + globals_text,
        "cache globals",
    )

    helper = r'''
static void ResetMetaCache(const MciFsImage *image)
{
    MetaCacheOwner = image;
    MetaIndirectValid = 0;
    MetaFatValid = 0;
    MetaIndirectCluster = 0u;
    MetaFatCluster = 0u;
'''
    if args.probe:
        helper += r'''    MetaIndirectHits = 0u;
    MetaIndirectMisses = 0u;
    MetaFatHits = 0u;
    MetaFatMisses = 0u;
'''
    helper += r'''}

static int ReadMetaClusterCached(MciFsImage *image, u32 absolute_cluster,
                                 int fat_level,
                                 const unsigned char **buffer)
{
    unsigned char *data;
    u32 *cached_cluster;
    int *valid;
    int rc;

    if (image == NULL || buffer == NULL)
        return -1;
    if (MetaCacheOwner != image)
        ResetMetaCache(image);

    if (fat_level) {
        data = MetaFatData;
        cached_cluster = &MetaFatCluster;
        valid = &MetaFatValid;
    } else {
        data = MetaIndirectData;
        cached_cluster = &MetaIndirectCluster;
        valid = &MetaIndirectValid;
    }

    if (*valid && *cached_cluster == absolute_cluster) {
'''
    if args.probe:
        helper += r'''        if (fat_level)
            MetaFatHits++;
        else
            MetaIndirectHits++;
'''
    helper += r'''        *buffer = data;
        return 0;
    }

    rc = ReadCluster(image, absolute_cluster, data);
    if (rc < 0)
        return rc;
    *cached_cluster = absolute_cluster;
    *valid = 1;
'''
    if args.probe:
        helper += r'''    if (fat_level)
        MetaFatMisses++;
    else
        MetaIndirectMisses++;
'''
    helper += r'''    *buffer = data;
    return 0;
}

'''

    text = replace_once(
        text,
        'static int FatEntry(MciFsImage *image, u32 relative_cluster, u32 *value)\n',
        helper + 'static int FatEntry(MciFsImage *image, u32 relative_cluster, u32 *value)\n',
        "cache helper insertion",
    )

    fat_start = text.index('static int FatEntry(')
    fat_end = text.index('static int NextRelativeCluster(', fat_start)
    fat = text[fat_start:fat_end]
    fat = replace_once(
        fat,
        '    unsigned char cluster[FS_MAX_CLUSTER_BYTES];\n',
        '    const unsigned char *cluster;\n',
        "FatEntry stack cluster removal",
    )
    fat = replace_once(
        fat,
        '    rc = ReadCluster(image, indirect_cluster, cluster);\n',
        '    rc = ReadMetaClusterCached(image, indirect_cluster, 0, &cluster);\n',
        "indirect cluster cache",
    )
    fat = replace_once(
        fat,
        '    rc = ReadCluster(image, fat_cluster, cluster);\n',
        '    rc = ReadMetaClusterCached(image, fat_cluster, 1, &cluster);\n',
        "FAT cluster cache",
    )
    text = text[:fat_start] + fat + text[fat_end:]

    text = replace_once(
        text,
        '    memset(image, 0, sizeof(*image));\n    image->fd = -1;\n',
        '    memset(image, 0, sizeof(*image));\n    ResetMetaCache(image);\n    image->fd = -1;\n',
        "OpenImage cache reset",
    )

    close_old = '''static void CloseImage(MciFsImage *image)\n{\n    if (image->fd >= 0)\n        fileXioClose(image->fd);\n    image->fd = -1;\n}\n'''
    close_new = '''static void CloseImage(MciFsImage *image)\n{\n'''
    if args.probe:
        close_new += '''    if (MetaCacheOwner == image) {\n        MciDiagLogTracePrintf(\n            "IMAGE-FS-CACHE",\n            "fat metadata indirect_hits=%u indirect_misses=%u fat_hits=%u fat_misses=%u",\n            MetaIndirectHits, MetaIndirectMisses, MetaFatHits, MetaFatMisses);\n    }\n'''
    close_new += '''    if (image->fd >= 0)\n        fileXioClose(image->fd);\n    image->fd = -1;\n    if (MetaCacheOwner == image)\n        ResetMetaCache(NULL);\n}\n'''
    text = replace_once(text, close_old, close_new, "CloseImage cache reset")

    args.source.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
