#!/usr/bin/env python3
"""Apply the P0 two-page image-filesystem cluster batching experiment.

The tracked C source stays as the readable production baseline. CI applies this
transformer only to isolated A/B worktrees. Every replacement is asserted so
source drift fails closed instead of silently producing a partial candidate.
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

/* P0 two-page cluster transport scratch.
 *
 * OpenImage accepts only one- or two-page clusters. For the common two-page
 * case the old ReadCluster issued one seek + one 512-byte read per logical
 * page. VMC pages are physically contiguous, so one 1024-byte read is enough.
 * PCSX2 .ps2 records carry 16 spare/ECC bytes after each 512-byte data page;
 * reading the two complete 528-byte records in one 1056-byte request removes
 * the second seek/read RPC and then strips the two spare regions locally.
 *
 * This scratch lives in BSS instead of a recursive filesystem stack frame.
 * The image-FS engine consumes one source image at a time, so ownership is
 * transient to ReadCluster and no state survives the call. */
static unsigned char ClusterBatchRaw[FS_PS2_STRIDE * 2u]
    __attribute__((aligned(64)));
'''
    if args.probe:
        globals_text += r'''static u32 ClusterBatchClusters;
static u32 ClusterLegacyClusters;
static u64 ClusterBatchPhysicalBytes;
'''

    text = replace_once(
        text,
        'static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";\n',
        'static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";\n'
        + globals_text,
        "cluster scratch globals",
    )

    old_read_cluster = r'''static int ReadCluster(MciFsImage *image, u32 absolute_cluster,
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
'''

    new_read_cluster = r'''static int ReadCluster(MciFsImage *image, u32 absolute_cluster,
                       unsigned char buffer[FS_MAX_CLUSTER_BYTES])
{
    u32 page;
    u32 i;
    int rc;

    if (absolute_cluster >= image->sb.clusters_per_card)
        return -1;
    memset(buffer, 0, FS_MAX_CLUSTER_BYTES);
    page = absolute_cluster * image->sb.pages_per_cluster;

    if (image->sb.pages_per_cluster == 2u) {
        u64 offset = (u64)page * (u64)image->stride;
        unsigned int physical_bytes;

        if (image->stride != FS_PAGE_SIZE && image->stride != FS_PS2_STRIDE)
            return -2;
        physical_bytes = image->stride * 2u;
        rc = fileXioLseek(image->fd, (int)offset, SEEK_SET);
        if (rc < 0 || (u64)rc != offset)
            return rc < 0 ? rc : -3;

        if (image->stride == FS_PAGE_SIZE) {
            rc = ReadExact(image->fd, buffer, FS_MAX_CLUSTER_BYTES);
        } else {
            rc = ReadExact(image->fd, ClusterBatchRaw, physical_bytes);
            if (rc == 0) {
                memcpy(buffer, ClusterBatchRaw, FS_PAGE_SIZE);
                memcpy(buffer + FS_PAGE_SIZE,
                       ClusterBatchRaw + FS_PS2_STRIDE, FS_PAGE_SIZE);
            }
        }
'''
    if args.probe:
        new_read_cluster += r'''        if (rc == 0) {
            ClusterBatchClusters++;
            ClusterBatchPhysicalBytes += physical_bytes;
        }
'''
    new_read_cluster += r'''        return rc;
    }

'''
    if args.probe:
        new_read_cluster += r'''    ClusterLegacyClusters++;
'''
    new_read_cluster += r'''    /* Defensive fallback preserves the original helper semantics if a future
     * format admits a cluster geometry other than the currently validated 1/2. */
    for (i = 0; i < image->sb.pages_per_cluster; i++) {
        rc = ReadLogicalPage(image, page + i, buffer + i * FS_PAGE_SIZE);
        if (rc < 0)
            return rc;
    }
    return 0;
}
'''

    text = replace_once(
        text,
        old_read_cluster,
        new_read_cluster,
        "ReadCluster batching",
    )

    if args.probe:
        text = replace_once(
            text,
            '    memset(image, 0, sizeof(*image));\n    image->fd = -1;\n',
            '    memset(image, 0, sizeof(*image));\n'
            '    ClusterBatchClusters = 0u;\n'
            '    ClusterLegacyClusters = 0u;\n'
            '    ClusterBatchPhysicalBytes = 0u;\n'
            '    image->fd = -1;\n',
            "probe reset",
        )

        close_old = '''static void CloseImage(MciFsImage *image)\n{\n    if (image->fd >= 0)\n        fileXioClose(image->fd);\n    image->fd = -1;\n}\n'''
        close_new = '''static void CloseImage(MciFsImage *image)\n{\n    MciDiagLogTracePrintf(\n        "IMAGE-FS-CLUSTER",\n        "cluster transport batched=%u legacy=%u physical_bytes=%llu",\n        ClusterBatchClusters, ClusterLegacyClusters,\n        (unsigned long long)ClusterBatchPhysicalBytes);\n    if (image->fd >= 0)\n        fileXioClose(image->fd);\n    image->fd = -1;\n}\n'''
        text = replace_once(text, close_old, close_new, "probe close log")

    args.source.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
