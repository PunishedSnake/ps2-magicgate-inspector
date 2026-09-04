#!/usr/bin/env python3
"""Fail-closed cleanup for warnings found in the P0 qualification builds.

This is deliberately separate from timing candidates. It fixes one real path-size
contract bug plus compiler-visible truncation/dead-code diagnostics without changing
P0 transport defaults or enabling new optimizations.
"""

from pathlib import Path
import sys


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match in {path}, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def patch_magic(path: Path) -> None:
    replace_once(
        path,
        'static const char SuperblockMagic[28] = "Sony PS2 Memory Card Format ";',
        'static const char SuperblockMagic[28] __attribute__((nonstring)) = "Sony PS2 Memory Card Format ";',
        "nonstring superblock magic",
    )


def main(root: Path) -> None:
    for rel in (
        "src/raw_bulk_read.c",
        "src/image_quick_verify.c",
        "src/card_image.c",
        "src/card_image_fs.c",
    ):
        patch_magic(root / rel)

    raw = root / "src/raw_bulk_read.c"
    replace_once(
        raw,
        "    int rc;\n    int sequential;\n\n    if (!Stats.bound || buffer == NULL || page < 0 || Pending)\n",
        "    int rc;\n#if MCI_RAW_BULK_ASYNC\n    int sequential;\n#endif\n\n    if (!Stats.bound || buffer == NULL || page < 0 || Pending)\n",
        "raw sequential declaration",
    )
    replace_once(
        raw,
        "    requested = (u32)page;\n    sequential = LastPageValid && LastPort == port && LastSlot == slot &&\n                 requested == LastPage + 1u;\n\n    if (!CacheContains(CurrentCache, port, slot, requested)) {\n",
        "    requested = (u32)page;\n#if MCI_RAW_BULK_ASYNC\n    sequential = LastPageValid && LastPort == port && LastSlot == slot &&\n                 requested == LastPage + 1u;\n#endif\n\n    if (!CacheContains(CurrentCache, port, slot, requested)) {\n",
        "raw sequential assignment",
    )

    app = root / "src/app_main.c"
    replace_once(
        app,
        "static void RunCardImageVerifyLatest(int port, MciCardImageFormat format)",
        "static void __attribute__((unused)) RunCardImageVerifyLatest(int port, MciCardImageFormat format)",
        "legacy verify-latest annotation",
    )
    replace_once(app, "                        char message[360];\n", "                        char message[512];\n", "legacy recovery modal buffer")

    appv2 = root / "src/app_main_v2.c"
    replace_once(appv2, "    char message[760];\n", "    char message[1024];\n", "save-transfer result buffer")
    replace_once(
        appv2,
        "        char message[320];\n        snprintf(message, sizeof(message),\n                 \"The selected image could not be indexed as a PS2 save filesystem.",
        "        char message[384];\n        snprintf(message, sizeof(message),\n                 \"The selected image could not be indexed as a PS2 save filesystem.",
        "image scan error buffer",
    )
    replace_once(appv2, "                        char message[360];\n", "                        char message[512];\n", "v2 recovery modal buffer")

    gui = root / "src/gui.c"
    replace_once(
        gui,
        '        snprintf(line, sizeof(line), "Found at: %s%s",\n                 root_len > 43u ? "..." : "", root_tail);',
        '        snprintf(line, sizeof(line), "Found at: %s%.43s",\n                 root_len > 43u ? "..." : "", root_tail);',
        "FMCB source tail formatting",
    )
    replace_once(
        gui,
        '            snprintf(LastMagicGateMarqueeSource,\n                     sizeof(LastMagicGateMarqueeSource), "%s", source);',
        '            snprintf(LastMagicGateMarqueeSource,\n                     sizeof(LastMagicGateMarqueeSource), "%.*s",\n                     (int)sizeof(LastMagicGateMarqueeSource) - 1, source);',
        "MagicGate marquee bounded copy",
    )
    replace_once(
        gui,
        '    snprintf(line, sizeof(line), "SOURCE  %s  %s%s",\n             MciCardImageFormatName(list->format),\n             strlen(list->path) > 47u ? "..." : "", tail);',
        '    snprintf(line, sizeof(line), "SOURCE  %s  %s%.47s",\n             MciCardImageFormatName(list->format),\n             strlen(list->path) > 47u ? "..." : "", tail);',
        "image-browser source tail formatting",
    )

    core = root / "src/gui_core.inc"
    replace_once(
        core,
        '    snprintf(line, sizeof(line), "mass: %s   source: %s",\n             mass != NULL && mass->available ? "AVAILABLE" : "UNAVAILABLE",\n             r->source_root[0] ? r->source_root : "not resolved");',
        '    snprintf(line, sizeof(line), "mass: %s   source: %.96s",\n             mass != NULL && mass->available ? "AVAILABLE" : "UNAVAILABLE",\n             r->source_root[0] ? r->source_root : "not resolved");',
        "GUI source-root display bound",
    )

    progress = root / "src/progress.c"
    replace_once(
        progress,
        "        if (arrow != NULL) {\n            unsigned int prefix = (unsigned int)(arrow - safe_detail);\n            if (prefix + strlen(arrow + 4) + 12u < sizeof(normalized_detail)) {\n                snprintf(normalized_detail, sizeof(normalized_detail),\n                         \"%.*s | format: %s\", (int)prefix, safe_detail, arrow + 4);\n                safe_detail = normalized_detail;\n            }\n        }",
        "        if (arrow != NULL) {\n            unsigned int prefix = (unsigned int)(arrow - safe_detail);\n            unsigned int suffix = (unsigned int)strlen(arrow + 4);\n            static const char separator[] = \" | format: \";\n            if (prefix + (sizeof(separator) - 1u) + suffix + 1u <=\n                sizeof(normalized_detail)) {\n                memcpy(normalized_detail, safe_detail, prefix);\n                memcpy(normalized_detail + prefix, separator, sizeof(separator) - 1u);\n                memcpy(normalized_detail + prefix + sizeof(separator) - 1u,\n                       arrow + 4, suffix + 1u);\n                safe_detail = normalized_detail;\n            }\n        }",
        "progress bounded composition",
    )

    recovery_h = root / "src/fmcb_recovery.h"
    replace_once(
        recovery_h,
        "#define FMCB_RECOVERY_PATH_MAX 128",
        "/* source_root can use the full 192-byte package path. Recovery appends\n * /MCI-RECOVERY, so the recovery path must not be smaller than its producer. */\n#define FMCB_RECOVERY_PATH_MAX (FMCB_SOURCE_ROOT_MAX + 32)",
        "recovery path capacity",
    )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <tree-root>")
    main(Path(sys.argv[1]))
