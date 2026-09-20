#!/usr/bin/env python3
from pathlib import Path

BRANCH_TAG = "Drebin dev4 image browser"


def replace_once(path, old, new):
    p = Path(path)
    s = p.read_text()
    if old not in s:
        raise SystemExit(f"{path}: expected marker not found for {BRANCH_TAG}")
    p.write_text(s.replace(old, new, 1))


# Preserve top-level directory metadata in the index and replay it on import.
replace_once(
    "src/card_image_fs.c",
    """        save->mode = entry.mode;\n        save->start_cluster = entry.cluster;\n""",
    """        save->mode = entry.mode;\n        memcpy(save->created, &entry.created, sizeof(save->created));\n        memcpy(save->modified, &entry.modified, sizeof(save->modified));\n        save->attr = entry.attr;\n        save->start_cluster = entry.cluster;\n""",
)
replace_once(
    "src/card_image_fs.c",
    """        top.mode = save->mode;\n        top.length = save->entry_count;\n        top.cluster = save->start_cluster;\n""",
    """        top.mode = save->mode;\n        memcpy(&top.created, save->created, sizeof(save->created));\n        memcpy(&top.modified, save->modified, sizeof(save->modified));\n        top.attr = save->attr;\n        top.length = save->entry_count;\n        top.cluster = save->start_cluster;\n""",
)

# Compile the filesystem browser/import engine.
replace_once(
    "Makefile",
    "src/kelf_cache.o src/card_raw_session.o src/card_image.o",
    "src/kelf_cache.o src/card_raw_session.o src/card_image.o src/card_image_fs.o",
)

# Controller integration.
replace_once(
    "src/app_main.c",
    '#include "card_image.h"\n',
    '#include "card_image.h"\n#include "card_image_fs.h"\n',
)

browser_code = r'''
static int CountSelectedImageSaves(const MciImageSaveList *list,
                                   u32 *bytes, u32 *clusters)
{
    int i;
    int selected = 0;
    *bytes = 0u;
    *clusters = 0u;
    for (i = 0; i < list->save_count; i++) {
        if (!list->saves[i].selected)
            continue;
        selected++;
        *bytes += list->saves[i].total_bytes;
        *clusters += list->saves[i].required_clusters;
    }
    return selected;
}

static void ShowImageImportReport(const MciImageImportReport *report, int rc)
{
    char message[620];
    MciGuiTone tone = rc == 0 ? MCI_GUI_TONE_SUCCESS : MCI_GUI_TONE_WARNING;

    if (report->result == MCI_IMAGE_FS_ROLLBACK_FAILED)
        tone = MCI_GUI_TONE_DANGER;
    snprintf(message, sizeof(message),
             "Result: %s (rc=%d)\n\nSelected: %d\nRestored: %d\nConflicts: %d\nFiles: %u\nDirectories: %u\nBytes written: %u\nRequired clusters: %u\nTarget free before restore: %d\nFailed path: %s\nRollback rc: %d",
             MciImageFsResultText(report->result), rc,
             report->selected_saves, report->restored_saves,
             report->conflict_saves, report->files_written,
             report->directories_written, report->bytes_written,
             report->required_clusters, report->target_free_clusters,
             report->failed_path[0] != '\0' ? report->failed_path : "none",
             report->rollback_rc);
    MciGuiRenderMessage("Selective image restore", message,
                        "CROSS or CIRCLE returns to the image browser.", tone);
    WaitForModalDismiss();
}

static void RunCardImageBrowser(int port, MciCardImageFormat format)
{
    MciImageSaveList list;
    MciImageImportReport import_report;
    char path[MCI_CARD_IMAGE_PATH_MAX];
    int cursor = 0;
    int rc;
    u32 held;
    u32 pressed;

    rc = MciCardImageFindLatest(port, format, path, sizeof(path));
    if (rc < 0) {
        MciGuiRenderMessage("Image browser unavailable",
                            "No matching Drebin image was found for the selected slot and format.",
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_WARNING);
        WaitForModalDismiss();
        return;
    }
    rc = MciImageFsScan(path, format, &list);
    if (rc < 0) {
        char message[360];
        snprintf(message, sizeof(message),
                 "The image passed filename discovery but its PS2 filesystem could not be indexed.\n\n%s\nResult: %s (rc=%d)",
                 path, MciImageFsResultText(list.result), rc);
        MciGuiRenderMessage("Image filesystem scan failed", message,
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_DANGER);
        WaitForModalDismiss();
        return;
    }
    rc = MciImageFsRefreshTargetConflicts(port, &list);
    if (rc < 0) {
        MciGuiRenderMessage("Target card unavailable",
                            "The image was indexed, but the selected destination card could not be checked for existing save directories.",
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_DANGER);
        WaitForModalDismiss();
        return;
    }

    for (;;) {
        enum { VISIBLE = 6 };
        char body[980];
        int first;
        int last;
        int i;
        int used;
        int selected;
        u32 selected_bytes;
        u32 selected_clusters;

        if (list.save_count == 0) {
            MciGuiRenderMessage("IMAGE BROWSER - Drebin",
                                "The image filesystem is valid, but it contains no top-level directories to restore.",
                                "CIRCLE returns to Card Tools.", MCI_GUI_TONE_INFO);
        } else {
            if (cursor >= list.save_count)
                cursor = list.save_count - 1;
            if (cursor < 0)
                cursor = 0;
            first = cursor - VISIBLE / 2;
            if (first < 0)
                first = 0;
            if (first + VISIBLE > list.save_count)
                first = list.save_count > VISIBLE ? list.save_count - VISIBLE : 0;
            last = first + VISIBLE;
            if (last > list.save_count)
                last = list.save_count;
            selected = CountSelectedImageSaves(&list, &selected_bytes,
                                               &selected_clusters);
            used = snprintf(body, sizeof(body),
                            "%s\nTarget: mc%d  Directories: %d%s\nSelected: %d  Data: %u KiB  Est. clusters: %u\n\n",
                            list.path, port, list.save_count,
                            list.truncated ? " (list truncated)" : "",
                            selected, (selected_bytes + 1023u) / 1024u,
                            selected_clusters);
            if (used < 0)
                used = 0;
            for (i = first; i < last && (unsigned int)used < sizeof(body); i++) {
                const MciImageSaveEntry *save = &list.saves[i];
                int n = snprintf(body + used, sizeof(body) - (unsigned int)used,
                                 "%c [%c] %-27.27s %5uK %s\n",
                                 i == cursor ? '>' : ' ',
                                 save->selected ? 'X' : ' ', save->name,
                                 (save->total_bytes + 1023u) / 1024u,
                                 save->conflict ? "EXISTS" : "");
                if (n > 0)
                    used += n;
            }
            if ((unsigned int)used < sizeof(body))
                snprintf(body + used, sizeof(body) - (unsigned int)used,
                         "\nEntry %d/%d. Existing destination directories are never overwritten by selective restore.",
                         cursor + 1, list.save_count);
            MciGuiRenderMessage("IMAGE BROWSER - Drebin", body,
                                "UP/DOWN browses. SQUARE marks. TRIANGLE toggles all available. CROSS restores marked saves. CIRCLE returns.",
                                selected > 0 ? MCI_GUI_TONE_SUCCESS : MCI_GUI_TONE_INFO);
        }

        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE)
            return;
        if (list.save_count == 0)
            continue;
        if (pressed & PAD_UP) {
            cursor = cursor == 0 ? list.save_count - 1 : cursor - 1;
            continue;
        }
        if (pressed & PAD_DOWN) {
            cursor = (cursor + 1) % list.save_count;
            continue;
        }
        if (pressed & PAD_SQUARE) {
            if (!list.saves[cursor].conflict)
                list.saves[cursor].selected ^= 1;
            continue;
        }
        if (pressed & PAD_TRIANGLE) {
            int any_available_unselected = 0;
            for (i = 0; i < list.save_count; i++) {
                if (!list.saves[i].conflict && !list.saves[i].selected) {
                    any_available_unselected = 1;
                    break;
                }
            }
            for (i = 0; i < list.save_count; i++)
                list.saves[i].selected = !list.saves[i].conflict &&
                                         any_available_unselected;
            continue;
        }
        if (pressed & PAD_CROSS) {
            char warning[560];
            int selected;
            u32 selected_bytes;
            u32 selected_clusters;

            selected = CountSelectedImageSaves(&list, &selected_bytes,
                                               &selected_clusters);
            if (selected == 0) {
                MciGuiRenderMessage("Nothing selected",
                                    "Mark one or more non-conflicting save directories with SQUARE before starting selective restore.",
                                    "CROSS or CIRCLE returns to the image browser.",
                                    MCI_GUI_TONE_INFO);
                WaitForModalDismiss();
                continue;
            }
            snprintf(warning, sizeof(warning),
                     "Restore %d selected director%s from %s to mc%d?\n\nAbout %u KiB of file data and %u destination clusters are required. Existing top-level directories are blocked rather than overwritten. If any new-file write fails, every object created by this restore attempt is deleted in reverse order.",
                     selected, selected == 1 ? "y" : "ies", list.path, port,
                     (selected_bytes + 1023u) / 1024u, selected_clusters);
            if (!ConfirmCardToolsDestructive("SELECTIVE IMAGE RESTORE", warning))
                continue;
            rc = MciImageFsImportSelected(port, &list, &import_report);
            ShowImageImportReport(&import_report, rc);
            (void)MciImageFsRefreshTargetConflicts(port, &list);
            for (i = 0; i < list.save_count; i++) {
                if (list.saves[i].conflict)
                    list.saves[i].selected = 0;
            }
            if (rc == 0)
                ResetSlotReports(port);
        }
    }
}

'''
replace_once(
    "src/app_main.c",
    "static void RunCardImageRestoreLatest(int port, MciCardImageFormat format)\n",
    browser_code + "static void RunCardImageRestoreLatest(int port, MciCardImageFormat format)\n",
)

replace_once(
    "src/app_main.c",
    "enum { CARD_TOOLS_ROWS = 8 };",
    "enum { CARD_TOOLS_ROWS = 10 };",
)
replace_once(
    "src/app_main.c",
    '"%c Exact restore latest .ps2 image\\n"\n                 "%c Exact restore latest .vmc image\\n"\n                 "%c Force format + verified .ps2 backup\\n"\n                 "%c Return\\n\\n"',
    '"%c Browse / selective restore .ps2\\n"\n                 "%c Browse / selective restore .vmc\\n"\n                 "%c Exact restore latest .ps2 image\\n"\n                 "%c Exact restore latest .vmc image\\n"\n                 "%c Force format + verified .ps2 backup\\n"\n                 "%c Return\\n\\n"',
)
replace_once(
    "src/app_main.c",
    """                 row == 2 ? '>' : ' ', row == 3 ? '>' : ' ',\n                 row == 4 ? '>' : ' ', row == 5 ? '>' : ' ',\n                 row == 6 ? '>' : ' ', row == 7 ? '>' : ' ');\n""",
    """                 row == 2 ? '>' : ' ', row == 3 ? '>' : ' ',\n                 row == 4 ? '>' : ' ', row == 5 ? '>' : ' ',\n                 row == 6 ? '>' : ' ', row == 7 ? '>' : ' ',\n                 row == 8 ? '>' : ' ', row == 9 ? '>' : ' ');\n""",
)
replace_once(
    "src/app_main.c",
    "row >= 4 && row <= 6 ? MCI_GUI_TONE_WARNING : MCI_GUI_TONE_INFO",
    "row >= 6 && row <= 8 ? MCI_GUI_TONE_WARNING : MCI_GUI_TONE_INFO",
)
replace_once(
    "src/app_main.c",
    """            case 4: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_PS2); break;\n            case 5: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_VMC); break;\n            case 6: RunForceFormatWithBackup(port); break;\n            default: return;\n""",
    """            case 4: RunCardImageBrowser(port, MCI_CARD_IMAGE_PS2); break;\n            case 5: RunCardImageBrowser(port, MCI_CARD_IMAGE_VMC); break;\n            case 6: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_PS2); break;\n            case 7: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_VMC); break;\n            case 8: RunForceFormatWithBackup(port); break;\n            default: return;\n""",
)
replace_once(
    "src/app_main.c",
    'scr_printf("PS2 Memory Card Inspector 0.4.0-dev3 Drebin\\n\\n");',
    'scr_printf("PS2 Memory Card Inspector 0.4.0-dev4 Drebin\\n\\n");',
)

# Let the normal CI build the development branch and label its artifact clearly.
replace_once(
    ".github/workflows/build.yml",
    "      - feat/0.4.0-dev3-drebin\n",
    "      - feat/0.4.0-dev3-drebin\n      - feat/0.4.0-dev4-drebin-browser\n",
)
replace_once(
    ".github/workflows/build.yml",
    "PS2-Memory-Card-Inspector-0.4.0-dev3-Drebin-${{ github.sha }}",
    "PS2-Memory-Card-Inspector-0.4.0-dev4-Drebin-${{ github.sha }}",
)

print("Drebin dev4 image browser integration applied")
