#!/usr/bin/env python3
"""Apply the P0 target-card conflict batching experiment.

Baseline selective-restore conflict refresh issues one mcGetDir + mcSync for every
source save name. The pinned PS2SDK mcGetDir API supports multiple directory
entries per RPC and XMCMAN keeps continuation state when mode is non-zero, so an
EE-side root listing can amortize the EE<->IOP round trip without weakening the
final pre-write conflict check.

Tracked C remains unchanged; CI applies this transformer to isolated A/B trees.
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
    parser.add_argument("--batch-size", type=int, choices=(16, 32, 64), default=32)
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

    globals_text = f'''

/* P0 target-conflict directory batch.
 * sceMcTblGetDir is 64-byte aligned/currently 64 bytes. Keep the result table
 * in BSS so refresh does not add a multi-KiB stack frame. One mcGetDir RPC asks
 * XMCSERV for several root entries; mode=1 continues the same MCMAN directory
 * cursor if the previous batch filled completely. */
#define FS_TARGET_CONFLICT_BATCH {args.batch_size}
static sceMcTblGetDir TargetConflictEntries[FS_TARGET_CONFLICT_BATCH]
    __attribute__((aligned(64)));
'''
    if args.probe:
        globals_text += '''static u32 TargetConflictRpcBatches;
static u32 TargetConflictEntriesScanned;
static u32 TargetConflictNameComparisons;
'''

    text = replace_once(
        text,
        'static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";\n',
        'static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";\n'
        + globals_text,
        "conflict batch globals",
    )

    old = r'''static int TargetExists(int port, const char *path)
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
'''

    new = r'''static int TargetEntryNameMatches(const sceMcTblGetDir *entry,
                                  const char *name)
{
    unsigned int entry_len = 0u;
    unsigned int name_len;

    while (entry_len < sizeof(entry->EntryName) && entry->EntryName[entry_len] != 0u)
        entry_len++;
    name_len = (unsigned int)strlen(name);
    return name_len == entry_len &&
           memcmp(entry->EntryName, name, entry_len) == 0;
}

int MciImageFsRefreshTargetConflicts(int target_port, MciImageSaveList *list)
{
    int mode = 0;
    int unresolved;
    int conflicts = 0;
    int i;
'''
    if args.probe:
        new += r'''
    TargetConflictRpcBatches = 0u;
    TargetConflictEntriesScanned = 0u;
    TargetConflictNameComparisons = 0u;
'''
    new += r'''
    if (list == NULL)
        return -1;

    unresolved = list->save_count;
    for (i = 0; i < list->save_count; i++)
        list->saves[i].conflict = 0;
    if (unresolved <= 0)
        return 0;

    for (;;) {
        int rc;
        int entry_index;

        memset(TargetConflictEntries, 0, sizeof(TargetConflictEntries));
        mcGetDir(target_port, 0, "/*", mode, FS_TARGET_CONFLICT_BATCH,
                 TargetConflictEntries);
        rc = McResult();
        if (rc < 0)
            return rc;
'''
    if args.probe:
        new += r'''        TargetConflictRpcBatches++;
        TargetConflictEntriesScanned += (u32)rc;
'''
    new += r'''
        if (rc == 0)
            break;

        for (entry_index = 0; entry_index < rc; entry_index++) {
            for (i = 0; i < list->save_count; i++) {
                if (list->saves[i].conflict)
                    continue;
'''
    if args.probe:
        new += r'''                TargetConflictNameComparisons++;
'''
    new += r'''                if (!TargetEntryNameMatches(&TargetConflictEntries[entry_index],
                                            list->saves[i].name))
                    continue;
                list->saves[i].conflict = 1;
                conflicts++;
                unresolved--;
                break;
            }
            if (unresolved == 0)
                break;
        }

        if (unresolved == 0 || rc < FS_TARGET_CONFLICT_BATCH)
            break;
        mode = 1;
    }
'''
    if args.probe:
        new += r'''
    MciDiagLogTracePrintf(
        "IMAGE-FS-CONFLICT",
        "target=mc%d batch=%d rpc_batches=%u entries=%u comparisons=%u conflicts=%d",
        target_port, FS_TARGET_CONFLICT_BATCH, TargetConflictRpcBatches,
        TargetConflictEntriesScanned, TargetConflictNameComparisons, conflicts);
'''
    new += r'''    return conflicts;
}
'''

    text = replace_once(text, old, new, "target conflict batching")
    args.source.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
