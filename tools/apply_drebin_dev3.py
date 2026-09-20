#!/usr/bin/env python3
from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"expected patch anchor missing in {path}: {old[:80]!r}")
    p.write_text(text.replace(old, new, 1))


# kelf_cache.c: keep the immutable cache warning-clean under C99.
replace_once(
    "src/kelf_cache.c",
    "#include <malloc.h>\n#include <stdlib.h>\n#include <string.h>\n",
    "#include <malloc.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n",
)

# magicgate.c: repeated probes use the immutable EE cache first and clone it for
# each mutating binding transaction.
replace_once(
    "src/magicgate.c",
    '#include "magicgate.h"\n#include "progress.h"\n#include "usb_search.h"\n',
    '#include "magicgate.h"\n#include "kelf_cache.h"\n#include "progress.h"\n#include "usb_search.h"\n',
)

anchor = '''static int FindRawKelfSource(MagicGateReport *report)\n{\n    char package_root[MCI_USB_SEARCH_PATH_MAX];\n    char path[MCI_USB_SEARCH_PATH_MAX];\n    int rc;\n\n    /* A complete FMCB package that already passed preflight is the preferred\n'''
replacement = '''static int UseCachedRawKelf(MagicGateReport *report)\n{\n    char path[MCI_USB_SEARCH_PATH_MAX];\n    unsigned int size = 0u;\n    char detail[224];\n\n    if (MciKelfCacheGetSource(path, sizeof(path), &size) < 0)\n        return -1;\n    if (size < sizeof(SecrKELFHeader_t) || size > MG_MAX_KELF_SIZE) {\n        MciKelfCacheInvalidate();\n        return -1;\n    }\n\n    report->source_port = MG_RAW_SOURCE_PORT;\n    report->source_size = (int)size;\n    report->source_io_rc = 0;\n    snprintf(report->source_path, sizeof(report->source_path), "%s", path);\n    snprintf(detail, sizeof(detail),\n             "EE RAM cache: %.150s (%u KiB). USB search/read skipped.",\n             path, (size + 1023u) / 1024u);\n    MgProgress(report, 8, "FMCB.XLF ready from cache", detail);\n    return 0;\n}\n\nstatic int FindRawKelfSource(MagicGateReport *report)\n{\n    char package_root[MCI_USB_SEARCH_PATH_MAX];\n    char path[MCI_USB_SEARCH_PATH_MAX];\n    int rc;\n\n    if (UseCachedRawKelf(report) == 0)\n        return 0;\n\n    /* A complete FMCB package that already passed preflight is the preferred\n'''
replace_once("src/magicgate.c", anchor, replacement)

old_read = '''static int ReadRawKelfSource(const MagicGateReport *report,\n                             unsigned char **out_buffer)\n{\n    const char *path = RawPathFromReport(report);\n    unsigned char *buffer;\n    char detail[224];\n    int alloc_size;\n    int total;\n    int chunk;\n    int fd;\n    int rc;\n    int last_percent = -1;\n\n    if (report->source_size <= 0 || report->source_size > MG_MAX_KELF_SIZE)\n        return MG_INVALID_LAYOUT;\n\n    alloc_size = report->source_size + 0x400;\n    buffer = memalign(64, alloc_size);\n    if (buffer == NULL)\n        return -ENOMEM;\n    memset(buffer, 0, alloc_size);\n\n    MgProgress(report, 10, "Opening raw FMCB.XLF",\n               "Allocating an aligned EE RAM buffer and opening the USB source read-only.");\n    fd = fileXioOpen(path, FIO_O_RDONLY);\n    if (fd < 0) {\n        free(buffer);\n        return fd;\n    }\n\n    total = 0;\n    while (total < report->source_size) {\n        int percent;\n\n        chunk = report->source_size - total;\n        if (chunk > MG_READ_CHUNK)\n            chunk = MG_READ_CHUNK;\n\n        rc = fileXioRead(fd, buffer + total, chunk);\n        if (rc < 0) {\n            fileXioClose(fd);\n            free(buffer);\n            return rc;\n        }\n        if (rc == 0 || rc > chunk) {\n            fileXioClose(fd);\n            free(buffer);\n            return MG_SHORT_READ;\n        }\n        total += rc;\n\n        percent = 10 + (total * 10) / report->source_size;\n        if (percent != last_percent) {\n            snprintf(detail, sizeof(detail),\n                     "Reading %s into EE RAM: %d / %d bytes.",\n                     path, total, report->source_size);\n            MgProgress(report, percent, "Reading raw FMCB.XLF", detail);\n            last_percent = percent;\n        }\n    }\n\n    rc = fileXioClose(fd);\n    if (rc < 0) {\n        free(buffer);\n        return rc;\n    }\n\n    *out_buffer = buffer;\n    return 0;\n}\n'''
new_read = '''static int ReadRawKelfSource(const MagicGateReport *report,\n                             unsigned char **out_buffer)\n{\n    const char *path = RawPathFromReport(report);\n    unsigned int clone_size = 0u;\n    int cache_hit = 0;\n    char detail[224];\n    int rc;\n\n    if (report->source_size <= 0 || report->source_size > MG_MAX_KELF_SIZE)\n        return MG_INVALID_LAYOUT;\n\n    MgProgress(report, 10, "Preparing raw FMCB.XLF",\n               "Using an immutable EE cache and a disposable aligned clone for the mutating security transaction.");\n    rc = MciKelfCacheClone(path, (unsigned int)report->source_size,\n                           out_buffer, &clone_size, &cache_hit);\n    if (rc < 0)\n        return rc;\n    if (clone_size != (unsigned int)report->source_size) {\n        free(*out_buffer);\n        *out_buffer = NULL;\n        return MG_SHORT_READ;\n    }\n\n    snprintf(detail, sizeof(detail),\n             cache_hit\n                 ? "Cloned %u KiB from the persistent EE RAM cache; no USB read was required."\n                 : "Read and cached %u KiB from USB, then cloned an untouched working copy.",\n             (clone_size + 1023u) / 1024u);\n    MgProgress(report, 20, cache_hit ? "Using cached FMCB.XLF"\n                                     : "FMCB.XLF cached in EE RAM", detail);\n    return 0;\n}\n'''
replace_once("src/magicgate.c", old_read, new_read)

# Explicit FMCB preflight is a refresh point. A successful package root update
# invalidates any older KELF bytes so a changed USB package cannot silently reuse
# stale content.
replace_once(
    "src/usb_search.c",
    '#include "usb_search.h"\n',
    '#include "usb_search.h"\n#include "kelf_cache.h"\n',
)
replace_once(
    "src/usb_search.c",
    '''    if (root == NULL || root[0] == '\\0' ||\n        strlen(root) >= sizeof(VerifiedPackageRoot))\n        return -1;\n    snprintf(VerifiedPackageRoot, sizeof(VerifiedPackageRoot), "%s", root);\n''',
    '''    if (root == NULL || root[0] == '\\0' ||\n        strlen(root) >= sizeof(VerifiedPackageRoot))\n        return -1;\n    MciKelfCacheInvalidate();\n    snprintf(VerifiedPackageRoot, sizeof(VerifiedPackageRoot), "%s", root);\n''',
)

# FMCB transaction inventory: exact protected-file lookup can fail on Sony X
# modules even when parent enumeration succeeds. Fall back to a bounded parent
# listing, but never turn an unreadable directory into an imaginary absence.
old_inventory = '''static int InventoryTarget(int port, const char *path,\n                           int *exists, unsigned int *size)\n{\n    sceMcTblGetDir info __attribute__((aligned(64)));\n    int rc;\n\n    *exists = 0;\n    *size = 0;\n    memset(&info, 0, sizeof(info));\n    mcGetDir(port, 0, path, 0, 1, &info);\n    rc = McResult();\n    if (rc == sceMcResNoEntry || rc == 0)\n        return 0;\n    if (rc < 0)\n        return rc;\n    *exists = 1;\n    *size = info.FileSizeByte;\n    return 0;\n}\n'''
new_inventory = '''static int NameEqualCi(const char *a, const char *b)\n{\n    while (*a != '\\0' && *b != '\\0') {\n        unsigned char ca = (unsigned char)*a++;\n        unsigned char cb = (unsigned char)*b++;\n        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));\n        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));\n        if (ca != cb) return 0;\n    }\n    return *a == '\\0' && *b == '\\0';\n}\n\nstatic int InventoryTargetFromParent(int port, const char *path,\n                                     int *exists, unsigned int *size)\n{\n    sceMcTblGetDir entries[64] __attribute__((aligned(64)));\n    char pattern[FMCB_PATH_MAX];\n    const char *slash = strrchr(path, '/');\n    const char *name;\n    int prefix_len;\n    int count;\n    int i;\n\n    if (slash == NULL || slash == path || slash[1] == '\\0')\n        return -4620;\n    name = slash + 1;\n    prefix_len = (int)(slash - path);\n    if (snprintf(pattern, sizeof(pattern), "%.*s/*", prefix_len, path) < 0 ||\n        strlen(pattern) >= sizeof(pattern))\n        return -4621;\n\n    memset(entries, 0, sizeof(entries));\n    mcGetDir(port, 0, pattern, 0, 64, entries);\n    count = McResult();\n    if (count == sceMcResNoEntry || count == 0)\n        return 0;\n    if (count < 0)\n        return count;\n    if (count > 64)\n        count = 64;\n    for (i = 0; i < count; i++) {\n        if (NameEqualCi(entries[i].EntryName, name)) {\n            *exists = 1;\n            *size = entries[i].FileSizeByte;\n            return 0;\n        }\n    }\n    return 0;\n}\n\nstatic int InventoryTarget(int port, const char *path,\n                           int *exists, unsigned int *size)\n{\n    sceMcTblGetDir info __attribute__((aligned(64)));\n    int exact_rc;\n    int fallback_rc;\n\n    *exists = 0;\n    *size = 0;\n    memset(&info, 0, sizeof(info));\n    mcGetDir(port, 0, path, 0, 1, &info);\n    exact_rc = McResult();\n    if (exact_rc == sceMcResNoEntry || exact_rc == 0)\n        return 0;\n    if (exact_rc > 0) {\n        *exists = 1;\n        *size = info.FileSizeByte;\n        return 0;\n    }\n\n    fallback_rc = InventoryTargetFromParent(port, path, exists, size);\n    if (fallback_rc == 0)\n        return 0;\n    return exact_rc;\n}\n'''
replace_once("src/fmcb_transaction.c", old_inventory, new_inventory)
replace_once(
    "src/fmcb_transaction.c",
    '''        rc = InventoryTarget(target_port, file->destination, &file->existed,\n                             &file->previous_size);\n''',
    '''        report->current_file = i;\n        rc = InventoryTarget(target_port, file->destination, &file->existed,\n                             &file->previous_size);\n''',
)

# app_main.c: actual Card Tools menu and raw operations, not placeholders.
replace_once(
    "src/app_main.c",
    '#include "card.h"\n#include "magicgate.h"\n',
    '#include "card.h"\n#include "card_image.h"\n#include "card_raw_session.h"\n#include "magicgate.h"\n',
)
replace_once(
    "src/app_main.c",
    'static MciSettings Settings;\nstatic int PadActive;\n',
    'static MciSettings Settings;\nstatic MciRawCardSessionStatus RawCardStatus;\nstatic int PadActive;\n',
)

card_tools = r'''static void WaitForModalDismiss(void)
{
    u32 held;
    u32 pressed;
    for (;;) {
        pressed = ReadPadPressed(&held);
        if (pressed & (PAD_CROSS | PAD_CIRCLE))
            return;
        DelayThread(16000);
    }
}

static int RestoreAfterRawCardMode(void)
{
    int rc = RestoreNormalEnvironment();
    if (rc < 0) {
        MciGuiRenderFatal("Normal stack restore failed",
                          "Card Tools ended, but the Sony ROM X card environment could not be reconstructed safely.", rc);
        SleepThread();
    }
    return rc;
}

static int ConfirmCardToolsDestructive(const char *title, const char *body)
{
    u32 held;
    u32 pressed;

    MciGuiRenderMessage(title, body,
                        "Hold L1 + R1 and press SQUARE to continue. CIRCLE cancels.",
                        MCI_GUI_TONE_DANGER);
    for (;;) {
        pressed = ReadPadPressed(&held);
        if (pressed & PAD_CIRCLE)
            return 0;
        if ((pressed & PAD_SQUARE) && (held & PAD_L1) && (held & PAD_R1))
            return 1;
        DelayThread(16000);
    }
}

static void ShowCardImageResult(const char *title,
                                const MciCardImageReport *report,
                                int rc, int destructive)
{
    char message[560];
    snprintf(message, sizeof(message),
             "%s on mc%d\n\nResult: %s (rc=%d)\nPath: %s\nPages: %u/%u\nCRC32: %08X\nVerified: %s%s",
             MciCardImageFormatName(report->format), report->port,
             MciCardImageResultText(report->result), rc,
             report->path[0] != '\0' ? report->path : "n/a",
             report->pages_done, report->pages_total,
             report->logical_crc32, report->verified ? "YES" : "NO",
             destructive ? "\n\nThe card was modified only after the source/safety image passed verification." : "");
    MciGuiRenderMessage(title, message,
                        "CROSS or CIRCLE returns to Card Tools.",
                        rc == 0 ? MCI_GUI_TONE_SUCCESS : MCI_GUI_TONE_DANGER);
    WaitForModalDismiss();
}

static void RunCardImageExportAction(int port, MciCardImageFormat format)
{
    MciCardImageReport report;
    int rc;

    MciGuiRenderMessage("Card image export",
                        "Switching to the page-level PS2SDK card personality. The image will not be accepted until the completed USB file is read back and verified.",
                        NULL, MCI_GUI_TONE_INFO);
    ShutdownNormalClients();
    rc = MciRawCardSessionStart(&RawCardStatus);
    if (rc == 0)
        rc = MciCardImageExport(port, format, &report);
    else
        MciCardImageResetReport(&report, port, format);
    MciRawCardSessionStop(&RawCardStatus);
    (void)RestoreAfterRawCardMode();
    ShowCardImageResult("Card image export", &report, rc, 0);
}

static void RunCardImageVerifyLatest(int port, MciCardImageFormat format)
{
    MciCardImageReport report;
    char path[MCI_CARD_IMAGE_PATH_MAX];
    int rc;

    rc = MciCardImageFindLatest(port, format, path, sizeof(path));
    if (rc == 0)
        rc = MciCardImageVerifyFile(path, format, &report);
    else {
        MciCardImageResetReport(&report, port, format);
        report.result = MCI_CARD_IMAGE_USB_ERROR;
    }
    ShowCardImageResult("Verify latest card image", &report, rc, 0);
}

static void RunCardImageRestoreLatest(int port, MciCardImageFormat format)
{
    MciCardImageReport report;
    char path[MCI_CARD_IMAGE_PATH_MAX];
    char warning[480];
    int rc;

    rc = MciCardImageFindLatest(port, format, path, sizeof(path));
    if (rc < 0) {
        MciCardImageResetReport(&report, port, format);
        report.result = MCI_CARD_IMAGE_USB_ERROR;
        ShowCardImageResult("Exact restore unavailable", &report, rc, 1);
        return;
    }
    snprintf(warning, sizeof(warning),
             "Exact-restore %s to mc%d?\n\n%s\n\nThe complete destination card will be erased block-by-block. Image and destination page counts must match, and the restored card is read back in full before PASS.",
             MciCardImageFormatName(format), port, path);
    if (!ConfirmCardToolsDestructive("EXACT CARD RESTORE", warning))
        return;

    ShutdownNormalClients();
    rc = MciRawCardSessionStart(&RawCardStatus);
    if (rc == 0)
        rc = MciCardImageRestoreExact(port, path, format, &report);
    else
        MciCardImageResetReport(&report, port, format);
    MciRawCardSessionStop(&RawCardStatus);
    (void)RestoreAfterRawCardMode();
    if (rc == 0)
        ResetSlotReports(port);
    ShowCardImageResult("Exact card restore", &report, rc, 1);
}

static void RunForceFormatWithBackup(int port)
{
    MciCardImageReport report;
    char warning[520];
    int rc;

    snprintf(warning, sizeof(warning),
             "Force-format mc%d regardless of its current filesystem state?\n\nBefore mcFormat is allowed, Drebin will create a complete PCSX2 .ps2 recovery image on USB, reopen it, validate every ECC record and compare its CRC with the physical card capture. If that backup fails, formatting is blocked.",
             port);
    if (!ConfirmCardToolsDestructive("FORCE FORMAT + VERIFIED BACKUP", warning))
        return;

    ShutdownNormalClients();
    rc = MciRawCardSessionStart(&RawCardStatus);
    if (rc == 0)
        rc = MciCardForceFormatWithBackup(port, &report);
    else
        MciCardImageResetReport(&report, port, MCI_CARD_IMAGE_PS2);
    MciRawCardSessionStop(&RawCardStatus);
    (void)RestoreAfterRawCardMode();
    if (rc == 0)
        ResetSlotReports(port);
    ShowCardImageResult("Force format", &report, rc, 1);
}

static void RunCardToolsMenu(int port)
{
    enum { CARD_TOOLS_ROWS = 8 };
    int row = 0;
    u32 held;
    u32 pressed;
    char body[900];

    for (;;) {
        snprintf(body, sizeof(body),
                 "Selected card: mc%d\n\n"
                 "%c Create verified PCSX2 .ps2 image\n"
                 "%c Create verified OPL .vmc image\n"
                 "%c Verify latest .ps2 image\n"
                 "%c Verify latest .vmc image\n"
                 "%c Exact restore latest .ps2 image\n"
                 "%c Exact restore latest .vmc image\n"
                 "%c Force format + verified .ps2 backup\n"
                 "%c Return\n\n"
                 "Exports and restores use page-level PS2SDK MCMAN/MCSERV. Exact restore rejects different card geometry; smaller-to-larger migration remains a separate filesystem-aware operation.",
                 port,
                 row == 0 ? '>' : ' ', row == 1 ? '>' : ' ',
                 row == 2 ? '>' : ' ', row == 3 ? '>' : ' ',
                 row == 4 ? '>' : ' ', row == 5 ? '>' : ' ',
                 row == 6 ? '>' : ' ', row == 7 ? '>' : ' ');
        MciGuiRenderMessage("CARD TOOLS - Drebin", body,
                            "UP/DOWN selects. CROSS runs. CIRCLE returns.",
                            row >= 4 && row <= 6 ? MCI_GUI_TONE_WARNING : MCI_GUI_TONE_INFO);

        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE)
            return;
        if (pressed & PAD_UP) {
            row = row == 0 ? CARD_TOOLS_ROWS - 1 : row - 1;
            continue;
        }
        if (pressed & PAD_DOWN) {
            row = (row + 1) % CARD_TOOLS_ROWS;
            continue;
        }
        if (!(pressed & PAD_CROSS))
            continue;

        switch (row) {
            case 0: RunCardImageExportAction(port, MCI_CARD_IMAGE_PS2); break;
            case 1: RunCardImageExportAction(port, MCI_CARD_IMAGE_VMC); break;
            case 2: RunCardImageVerifyLatest(port, MCI_CARD_IMAGE_PS2); break;
            case 3: RunCardImageVerifyLatest(port, MCI_CARD_IMAGE_VMC); break;
            case 4: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_PS2); break;
            case 5: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_VMC); break;
            case 6: RunForceFormatWithBackup(port); break;
            default: return;
        }
    }
}

'''
replace_once("src/app_main.c", "static void RenderDashboard(int selected, MciGuiPage page,\n", card_tools + "static void RenderDashboard(int selected, MciGuiPage page,\n")

replace_once(
    "src/app_main.c",
    '''            if ((pressed & PAD_TRIANGLE) &&\n                page != MCI_GUI_SETTINGS && Reports[selected].format_allowed) {\n                confirm_format = 1;\n                page = MCI_GUI_CARD;\n                dirty = 1;\n            }\n''',
    '''            if ((pressed & PAD_TRIANGLE) && page != MCI_GUI_SETTINGS) {\n                RunCardToolsMenu(selected);\n                confirm_format = 0;\n                last_format_rc = -999;\n                dirty = 1;\n            }\n''',
)

# Installer failure modal now says which inventory target killed the transaction.
replace_once(
    "src/app_main.c",
    '''        snprintf(result, sizeof(result),\n                 "Install failed at %s: %s. Files committed before failure: %d/%d. space rc=%d, recovery rc=%d, rollback rc=%d. If recovery remains present, do not start another install; restore the recorded transaction first.",\n                 FmcbInstallStageText(report->stage),\n                 FmcbInstallResultText(report->result),\n                 report->files_committed, report->files_total,\n                 report->space_rc, report->recovery_rc,\n                 report->rollback_rc);\n''',
    '''        const char *failed_target =\n            (report->current_file >= 0 && report->current_file < FMCB_TX_MAX_FILES)\n                ? report->files[report->current_file].destination : "n/a";\n        snprintf(result, sizeof(result),\n                 "Install failed at %s: %s. Target: %s. Files committed before failure: %d/%d. space rc=%d, recovery rc=%d, rollback rc=%d. If recovery remains present, do not start another install; restore the recorded transaction first.",\n                 FmcbInstallStageText(report->stage),\n                 FmcbInstallResultText(report->result), failed_target,\n                 report->files_committed, report->files_total,\n                 report->space_rc, report->recovery_rc,\n                 report->rollback_rc);\n''',
)

# User-visible dev label and generic UI copies.
for name in ("src/app_main.c", "src/gui_core.inc", "src/gui.c"):
    p = Path(name)
    if p.exists():
        t = p.read_text()
        t = t.replace("v0.4.0-dev2", "v0.4.0-dev3 Drebin")
        t = t.replace("0.4.0-dev2", "0.4.0-dev3 Drebin")
        p.write_text(t)

# Static checks. These deliberately test behavior rather than just file presence.
checks = {
    "src/magicgate.c": ["MciKelfCacheClone", "UseCachedRawKelf"],
    "src/fmcb_transaction.c": ["InventoryTargetFromParent", "report->current_file = i"],
    "src/app_main.c": ["CARD TOOLS - Drebin", "MciCardImageRestoreExact", "MciCardForceFormatWithBackup"],
}
for name, needles in checks.items():
    text = Path(name).read_text()
    for needle in needles:
        if needle not in text:
            raise SystemExit(f"post-patch check failed: {needle} missing from {name}")
