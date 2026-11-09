/* SPDX-License-Identifier: MIT */
/*
 * Drebin Card Tools / Save Transfer integration layer.
 *
 * The hardware-qualified 0.4 controller is included unchanged below, with its
 * entry point renamed. This translation unit then supplies the real entry point
 * and the Card Tools v2 workflow. The raw/MagicGate/FMCB helpers therefore stay
 * byte-for-byte source-identical to the qualified controller while the new UI
 * can be tested independently.
 */

#define main MciLegacyMain
#include "app_main.c"
#undef main

#include <fileXio_rpc.h>
#include <iox_stat.h>
#include <io_common.h>

#include "card_save_picker.h"
#include "ps1_card.h"
#include "save_replace_card.h"
#include "save_transfer.h"
#include "usb_file_picker.h"
#include "usb_file_picker_ui.h"

static int PreparePs1RawNameV2(const MciSaveTransferProbe *probe, const char *path);

void MciGuiRenderCardToolsV2(int selected, int selected_item);

static void WaitForPadNeutralV2(void)
{
    u32 held = 0;
    do {
        (void)ReadPadPressed(&held);
        DelayThread(16000);
    } while (held != 0u);
}

static int ConfirmCrossV2(const char *title, const char *body,
                          MciGuiTone tone)
{
    u32 held;
    u32 pressed;

    WaitForPadNeutralV2();
    MciGuiRenderMessage(title, body,
                        "CROSS continues. CIRCLE cancels.", tone);
    for (;;) {
        pressed = ReadPadPressed(&held);
        if (pressed & PAD_CIRCLE)
            return 0;
        if (pressed & PAD_CROSS)
            return 1;
        DelayThread(16000);
    }
}

static int TransferImageFormat(MciSaveTransferFormat format,
                               MciCardImageFormat *image_format)
{
    if (image_format == NULL)
        return -1;
    if (format == MCI_SAVE_FORMAT_IMAGE_PS2) {
        *image_format = MCI_CARD_IMAGE_PS2;
        return 0;
    }
    if (format == MCI_SAVE_FORMAT_IMAGE_VMC) {
        *image_format = MCI_CARD_IMAGE_VMC;
        return 0;
    }
    return -2;
}

static void ShowCardImageResultV2(const char *title,
                                  const MciCardImageReport *report,
                                  int rc, int destructive)
{
    char message[640];
    const char *tail = "";

    if (destructive) {
        tail = rc == 0
                   ? "\n\nDestructive writes reached PASS only after the operation's source/recovery verification contract completed."
                   : "\n\nThis destructive workflow did not reach PASS. Do not assume the destination state is unchanged; preserve the trace and inspect the reported stage before retrying.";
    }
    snprintf(message, sizeof(message),
             "%s on mc%d\n\nResult: %s (rc=%d)\nPath: %s\nPages: %u/%u\nCRC32: %08X\nVerified: %s\nRaw stack: mcInit=%d mcInfo(issue/sync/result)=%d/%d/%d%s",
             MciCardImageFormatName(report->format), report->port,
             MciCardImageResultText(report->result), rc,
             report->path[0] != '\0' ? report->path : "n/a",
             report->pages_done, report->pages_total,
             report->logical_crc32, report->verified ? "YES" : "NO",
             RawCardStatus.mcinit_rc, RawCardStatus.mcinfo_issue_rc,
             RawCardStatus.mcinfo_sync_rc, RawCardStatus.mcinfo_result, tail);
    MciGuiRenderMessage(title, message,
                        "CROSS or CIRCLE returns to Card Tools.",
                        rc == 0 ? MCI_GUI_TONE_SUCCESS : MCI_GUI_TONE_DANGER);
    WaitForPadNeutralV2();
    WaitForModalDismiss();
}

static void ShowSaveTransferResultV2(const char *title,
                                     const MciSaveTransferReport *report,
                                     int rc)
{
    char message[1600];
    MciGuiTone tone;

    if (report->result == MCI_SAVE_TRANSFER_OK)
        tone = MCI_GUI_TONE_SUCCESS;
    else if (report->result == MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT ||
             report->result == MCI_SAVE_TRANSFER_TARGET_CONFLICT)
        tone = MCI_GUI_TONE_WARNING;
    else
        tone = MCI_GUI_TONE_DANGER;

    snprintf(message, sizeof(message),
             "Result: %s (rc=%d)\nFormat: %s%s\nCard: mc%d%s\n\nSource: %s\nDestination: %s\nSave: %s%s%s\nSaves: %d/%d  files: %d/%d  verified: %d\nBytes: %u\nRequired %s: %d  target free: %d\nRollback rc: %d\nFailed path: %s",
             MciSaveTransferResultText(report->result), rc,
             MciSaveTransferFormatName(report->format),
             report->checksum_ok == 0 ? "  (integrity check FAILED)"
             : report->checksum_ok == 1 ? "  (integrity verified)" : "",
             report->card_port, report->generation == 1 ? " (PS1)" : "",
             report->source_path[0] ? report->source_path : "n/a",
             report->destination[0] ? report->destination : "n/a",
             report->save_directory[0] ? report->save_directory : "n/a",
             report->title[0] ? "  " : "", report->title,
             report->saves_written, report->saves_total,
             report->files_written, report->files_total,
             report->files_verified, report->bytes_written,
             report->generation == 1 ? "blocks" : "clusters",
             report->required_clusters, report->target_free_clusters,
             report->rollback_rc,
             report->failed_path[0] ? report->failed_path : "none");
    MciGuiRenderMessage(title, message,
                        "CROSS or CIRCLE returns to Card Tools.", tone);
    WaitForPadNeutralV2();
    WaitForModalDismiss();
}

static int EnsureDirectoryV2(const char *path)
{
    iox_stat_t stat;
    int rc;

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    if (rc >= 0)
        return FIO_S_ISDIR(stat.mode) ? 0 : -1;
    rc = fileXioMkdir(path, 0777);
    if (rc < 0)
        return rc;
    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    return rc >= 0 && FIO_S_ISDIR(stat.mode) ? 0 : -2;
}

static void SafeUsbBasenameV2(const char *source, char *out,
                              unsigned int out_size)
{
    unsigned int used = 0u;
    const unsigned char *p = (const unsigned char *)source;

    if (out_size == 0u)
        return;
    while (*p != '\0' && used + 1u < out_size) {
        unsigned char c = *p++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            out[used++] = (char)c;
        else
            out[used++] = '_';
    }
    if (used == 0u && out_size > 1u) {
        out[0] = 's'; out[1] = '\0';
        return;
    }
    out[used] = '\0';
}

static int PreparePsuExportPathV2(const char *save_directory,
                                  char *path, unsigned int path_size)
{
    char mci[128];
    char saves[144];
    char base[48];
    int root_index;

    SafeUsbBasenameV2(save_directory, base, sizeof(base));
    for (root_index = 0; root_index < 3; root_index++) {
        const char *root = MciUsbPickerRootName(root_index);
        int dd;
        int suffix;

        if (root == NULL)
            continue;
        dd = fileXioDopen(root);
        if (dd < 0)
            continue;
        fileXioDclose(dd);
        snprintf(mci, sizeof(mci), "%sMCI", root);
        snprintf(saves, sizeof(saves), "%sMCI/SAVES", root);
        if (EnsureDirectoryV2(mci) < 0 || EnsureDirectoryV2(saves) < 0)
            continue;

        for (suffix = 0; suffix < 100; suffix++) {
            iox_stat_t stat;
            int n;
            if (suffix == 0)
                n = snprintf(path, path_size, "%s/%s.psu", saves, base);
            else
                n = snprintf(path, path_size, "%s/%s-%02d.psu", saves, base,
                             suffix);
            if (n < 0 || (unsigned int)n >= path_size)
                return -3;
            memset(&stat, 0, sizeof(stat));
            if (fileXioGetStat(path, &stat) < 0)
                return 0;
        }
    }
    path[0] = '\0';
    return -4;
}

static void RunSaveImportV2(int *active_port)
{
    MciSaveTransferProbe probe;
    MciSaveTransferReport report;
    MciSaveTransferFormat picked_format = MCI_SAVE_FORMAT_UNKNOWN;
    char path[MCI_SAVE_TRANSFER_PATH_MAX];
    char confirm[640];
    int target_port = *active_port;
    int rc;

    rc = MciUsbPickerChoose(MCI_USB_PICKER_SAVE_ANY,
                            "IMPORT SINGLE SAVE", "DESTINATION",
                            &target_port, path, sizeof(path), &picked_format);
    *active_port = target_port;
    if (rc == 1)
        return;
    if (rc < 0) {
        MciGuiRenderMessage("Save import picker failed",
                            "The USB save browser could not return a source file.",
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_DANGER);
        WaitForPadNeutralV2();
        WaitForModalDismiss();
        return;
    }

    memset(&probe, 0, sizeof(probe));
    rc = MciSaveTransferProbeFile(path, &probe);
    if (rc < 0) {
        MciSaveTransferResetReport(&report, target_port, picked_format);
        report.result = MCI_SAVE_TRANSFER_INVALID_CONTAINER;
        snprintf(report.source_path, sizeof(report.source_path), "%s", path);
        ShowSaveTransferResultV2("Save import", &report, rc);
        return;
    }

    if (!probe.importable) {
        MciSaveTransferResetReport(&report, target_port, probe.format);
        report.result = MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT;
        snprintf(report.source_path, sizeof(report.source_path), "%s", path);
        snprintf(report.destination, sizeof(report.destination), "mc%d:", target_port);
        ShowSaveTransferResultV2("Save import", &report, -300);
        return;
    }

    if (!PreparePs1RawNameV2(&probe, path))
        return;
    if (probe.generation == 1)
        snprintf(confirm, sizeof(confirm),
                 "Import to the PS1 card in mc%d?\n\nSource: %s\nFormat: %s\nSize: %u KiB\n\nThe destination must be a formatted PS1 card. A card image imports every save it holds, all-or-nothing. Saves whose name already exists are never overwritten. Each save is read back and compared before PASS.\n\nNOTE: creating a PS1 save clears deleted (undeletable) entries on that card.",
                 target_port, path, MciSaveTransferFormatName(probe.format),
                 (unsigned int)((probe.size + 1023u) / 1024u));
    else
        snprintf(confirm, sizeof(confirm),
                 "Import this save to mc%d?\n\nSource: %s\nFormat: %s\nSize: %u KiB\n\nThe destination must be a formatted PS2 card. The container is decoded and validated in RAM first. Existing top-level save directories are never overwritten. Every new file is reopened and compared byte-for-byte before PASS; a failed transaction rolls back only objects it created.",
                 target_port, path, MciSaveTransferFormatName(probe.format),
                 (unsigned int)((probe.size + 1023u) / 1024u));
    if (!ConfirmCrossV2("SAVE IMPORT CONFIRMATION", confirm,
                        MCI_GUI_TONE_WARNING)) {
        MciSaveTransferSetPs1NameOverride(NULL);
        return;
    }

    rc = MciSaveTransferImportFile(target_port, path, &report);
    MciSaveTransferSetPs1NameOverride(NULL);
    if (rc == 0)
        ResetSlotReports(target_port);
    ShowSaveTransferResultV2("Save import", &report, rc);
}

static void RunSaveExportV2(int *active_port)
{
    MciSaveTransferReport report;
    char directory[MCI_CARD_SAVE_DIRECTORY_MAX];
    char path[MCI_SAVE_TRANSFER_PATH_MAX];
    int source_port = *active_port;
    int rc;

    rc = MciCardSavePickerChoose(&source_port, directory, sizeof(directory));
    *active_port = source_port;
    if (rc == 1)
        return;
    if (rc < 0) {
        MciSaveTransferResetReport(&report, source_port, MCI_SAVE_FORMAT_PSU);
        report.result = MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
        ShowSaveTransferResultV2("PSU export", &report, rc);
        return;
    }
    rc = PreparePsuExportPathV2(directory, path, sizeof(path));
    if (rc < 0) {
        MciSaveTransferResetReport(&report, source_port, MCI_SAVE_FORMAT_PSU);
        report.result = MCI_SAVE_TRANSFER_IO_ERROR;
        snprintf(report.source_path, sizeof(report.source_path), "mc%d:/%s",
                 source_port, directory);
        ShowSaveTransferResultV2("PSU export", &report, rc);
        return;
    }

    rc = MciSaveTransferExportPsu(source_port, directory, path, &report);
    ShowSaveTransferResultV2("PSU export", &report, rc);
}

/* Pad-driven PS1 save-name editor. Returns 1 when confirmed (name updated),
 * 0 when cancelled. */
static int EditPs1NameV2(char name[MCI_PS1_SAVE_NAME_MAX + 1u], const char *source)
{
    static const char Charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_ "
        "abcdefghijklmnopqrstuvwxyz.!#$%&()+,;=@[]^{}~'";
    char guess[MCI_PS1_SAVE_NAME_MAX + 1u];
    char body[900];
    char shown[64];
    int have_guess = MciPs1GuessNameFromText(source, guess);
    int cursor = 0;
    u32 held;
    u32 pressed;

    WaitForPadNeutralV2();
    for (;;) {
        int len = (int)strlen(name);
        int i;
        int used = 0;
        const char *verdict;

        for (i = 0; i <= len && i < (int)MCI_PS1_SAVE_NAME_MAX && used < (int)sizeof(shown) - 4; i++) {
            char c = i < len ? name[i] : ' ';
            if (i == cursor)
                used += snprintf(shown + used, sizeof(shown) - (unsigned int)used, "[%c]", c);
            else if (i < len)
                shown[used++] = c;
        }
        shown[used] = '\0';
        verdict = len == 0 ? "EMPTY - a name is required"
                  : MciPs1NameLooksLikeProductCode(name)
                        ? "looks like a PS1 save name"
                        : "WARNING: not a product-code name; games will not find it";
        snprintf(body, sizeof(body),
                 "This raw PS1 save has no header, so its name comes from the file name. PS1 games only see a save whose name matches exactly, e.g. BASCUS-94163FF7S01 (B + region A/E/I + product code + the game's own suffix). Copy the name from another save of the same game if you have one.\n\nSource: %s\n\nName: %s\n%s\n%s%s",
                 source, shown, verdict,
                 have_guess ? "SQUARE: use product code found in the file name: " : "",
                 have_guess ? guess : "");
        MciGuiRenderMessage("PS1 SAVE NAME", body,
                            "LEFT/RIGHT move  UP/DOWN char  TRIANGLE delete  X OK  O cancel",
                            MciPs1NameLooksLikeProductCode(name) ? MCI_GUI_TONE_INFO
                                                                  : MCI_GUI_TONE_WARNING);
        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE)
            return 0;
        if ((pressed & PAD_CROSS) && len > 0 && MciPs1NameIsSafe(name))
            return 1;
        if ((pressed & PAD_SQUARE) && have_guess) {
            /* Keep any identifier suffix the user already typed. */
            char merged[MCI_PS1_SAVE_NAME_MAX + 1u];
            const char *suffix = MciPs1NameLooksLikeProductCode(name) ? name + 12 : "";
            snprintf(merged, sizeof(merged), "%s%s", guess, suffix);
            memcpy(name, merged, strlen(merged) + 1u);
            cursor = (int)strlen(name) < (int)MCI_PS1_SAVE_NAME_MAX ? (int)strlen(name) : (int)MCI_PS1_SAVE_NAME_MAX - 1;
            continue;
        }
        if (pressed & PAD_LEFT)
            cursor = cursor > 0 ? cursor - 1 : 0;
        if ((pressed & PAD_RIGHT) && cursor < len && cursor + 1 < (int)MCI_PS1_SAVE_NAME_MAX)
            cursor++;
        if ((pressed & PAD_TRIANGLE) && cursor < len) {
            memmove(name + cursor, name + cursor + 1, (size_t)(len - cursor));
            if (cursor > 0 && cursor >= (int)strlen(name))
                cursor--;
        }
        if (pressed & (PAD_UP | PAD_DOWN)) {
            const char *at = cursor < len ? strchr(Charset, name[cursor]) : NULL;
            int idx = at != NULL ? (int)(at - Charset) : -1;
            int n = (int)sizeof(Charset) - 1;
            idx = (pressed & PAD_UP) ? (idx + 1) % n : (idx <= 0 ? n - 1 : idx - 1);
            if (cursor >= len) {
                if (len >= (int)MCI_PS1_SAVE_NAME_MAX)
                    continue;
                name[len] = Charset[idx];
                name[len + 1] = '\0';
            } else {
                name[cursor] = Charset[idx];
            }
        }
    }
}

/* For raw PS1 saves: confirm or edit the save name and arm the override.
 * Returns 1 to continue, 0 when the user cancelled. */
static int PreparePs1RawNameV2(const MciSaveTransferProbe *probe, const char *path)
{
    char name[MCI_PS1_SAVE_NAME_MAX + 1u];

    MciSaveTransferSetPs1NameOverride(NULL);
    if (probe->format != MCI_SAVE_FORMAT_RAW_PS1)
        return 1;
    MciPs1NameFromPath(path, name);
    if (!MciPs1NameLooksLikeProductCode(name)) {
        char guess[MCI_PS1_SAVE_NAME_MAX + 1u];
        if (MciPs1GuessNameFromText(path, guess))
            memcpy(name, guess, strlen(guess) + 1u);
    }
    if (!EditPs1NameV2(name, path))
        return 0;
    return MciSaveTransferSetPs1NameOverride(name) == 0;
}

/* Small list chooser on top of the message renderer. Returns the chosen
 * index, or -1 when cancelled with CIRCLE. */
static int ChooseOptionV2(const char *title, const char *intro,
                          const char *const *options, int count)
{
    char body[1024];
    int row = 0;
    u32 held;
    u32 pressed;

    WaitForPadNeutralV2();
    for (;;) {
        int used = snprintf(body, sizeof(body), "%s\n\n", intro);
        int i;
        for (i = 0; i < count && used > 0 && used < (int)sizeof(body); i++)
            used += snprintf(body + used, sizeof(body) - (unsigned int)used,
                             "%s %s\n", i == row ? ">" : " ", options[i]);
        MciGuiRenderMessage(title, body,
                            "DPAD choose   CROSS confirm   CIRCLE cancel",
                            MCI_GUI_TONE_INFO);
        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE)
            return -1;
        if (pressed & PAD_CROSS)
            return row;
        if (pressed & PAD_UP)
            row = row == 0 ? count - 1 : row - 1;
        else if (pressed & PAD_DOWN)
            row = row + 1 >= count ? 0 : row + 1;
    }
}

static void RunSaveConvertV2(int *active_port)
{
    MciSaveTransferReport report;
    MciSaveTransferProbe probe;
    MciSaveTransferFormat picked_format = MCI_SAVE_FORMAT_UNKNOWN;
    char path[MCI_SAVE_TRANSFER_PATH_MAX];
    char out_path[MCI_SAVE_TRANSFER_PATH_MAX];
    char confirm[640];
    int port = *active_port;
    int rc;

    rc = MciUsbPickerChoose(MCI_USB_PICKER_SAVE_ANY,
                            "CONVERT SAVE ON USB", "SOURCE",
                            &port, path, sizeof(path), &picked_format);
    *active_port = port;
    if (rc != 0)
        return;
    memset(&probe, 0, sizeof(probe));
    if (MciSaveTransferProbeFile(path, &probe) < 0 || !probe.importable) {
        MciSaveTransferResetReport(&report, -1, probe.format);
        report.result = MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT;
        snprintf(report.source_path, sizeof(report.source_path), "%s", path);
        ShowSaveTransferResultV2("Save conversion", &report, -300);
        return;
    }
    if (!PreparePs1RawNameV2(&probe, path))
        return;
    {
        MciSaveConvertTarget targets[8];
        const char *names[8];
        int n = MciSaveConvertTargetsFor(probe.format, probe.generation, targets);
        int i;
        int choice;

        for (i = 0; i < n; i++)
            names[i] = MciSaveConvertTargetName(targets[i]);
        snprintf(confirm, sizeof(confirm),
                 "Source: %s\nFormat: %s\n\nChoose the output format. It is written next to the source and re-read before PASS; signed formats are verified after signing. No memory card is touched and nothing is overwritten.",
                 path, MciSaveTransferFormatName(probe.format));
        choice = n > 0 ? ChooseOptionV2("CONVERT SAVE ON USB", confirm, names, n) : -1;
        if (choice < 0) {
            MciSaveTransferSetPs1NameOverride(NULL);
            return;
        }
        rc = MciSaveTransferConvertFileTo(path, targets[choice], out_path,
                                          sizeof(out_path), &report);
        MciSaveTransferSetPs1NameOverride(NULL);
    }
    ShowSaveTransferResultV2("Save conversion", &report, rc);
}

static void RunReplaceFileV2(int *active_port)
{
    static char names[24][33];
    static u32 sizes[24];
    const char *labels[24];
    char label_text[24][48];
    char directory[MCI_CARD_SAVE_DIRECTORY_MAX];
    char usb_path[MCI_SAVE_TRANSFER_PATH_MAX];
    char backup_path[MCI_SAVE_TRANSFER_PATH_MAX];
    char backup_base[48];
    char body[1100];
    MciSaveTransferFormat picked = MCI_SAVE_FORMAT_UNKNOWN;
    MciReplaceReport report;
    iox_stat_t st;
    int port = *active_port;
    int usb_port;
    int count;
    int choice;
    int i;
    int rc;

    rc = MciCardSavePickerChoose(&port, directory, sizeof(directory));
    *active_port = port;
    if (rc != 0)
        return;
    count = MciSaveListFiles(port, directory, names, sizes, 24);
    if (count <= 0) {
        MciGuiRenderMessage("Replace file", "The save has no files or could not be listed.",
                            "CROSS or CIRCLE returns to Card Tools.", MCI_GUI_TONE_DANGER);
        WaitForPadNeutralV2();
        WaitForModalDismiss();
        return;
    }
    for (i = 0; i < count; i++) {
        snprintf(label_text[i], sizeof(label_text[i]), "%-24s %8u B", names[i], sizes[i]);
        labels[i] = label_text[i];
    }
    snprintf(body, sizeof(body), "mc%d:/%s\nChoose the file to replace:", port, directory);
    choice = ChooseOptionV2("REPLACE FILE IN SAVE", body, labels, count);
    if (choice < 0)
        return;

    usb_port = port;
    rc = MciUsbPickerChoose(MCI_USB_PICKER_FILE_ANY, "REPLACEMENT DATA", "SOURCE",
                            &usb_port, usb_path, sizeof(usb_path), &picked);
    if (rc != 0)
        return;
    memset(&st, 0, sizeof(st));
    if (fileXioGetStat(usb_path, &st) < 0 || !FIO_S_ISREG(st.mode)) {
        MciGuiRenderMessage("Replace file", "The replacement file cannot be read.",
                            "CROSS or CIRCLE returns to Card Tools.", MCI_GUI_TONE_DANGER);
        WaitForPadNeutralV2();
        WaitForModalDismiss();
        return;
    }
    snprintf(backup_base, sizeof(backup_base), "%s_BEFORE_REPLACE", directory);
    if (PreparePsuExportPathV2(backup_base, backup_path, sizeof(backup_path)) < 0) {
        MciGuiRenderMessage("Replace file", "No writable USB location for the backup. Nothing was changed.",
                            "CROSS or CIRCLE returns to Card Tools.", MCI_GUI_TONE_DANGER);
        WaitForPadNeutralV2();
        WaitForModalDismiss();
        return;
    }
    snprintf(body, sizeof(body),
             "Replace mc%d:/%s/%s\nwith %s?\n\nCurrent size: %u bytes\nNew size:     %u bytes%s\n\n1. The whole save is first exported to %s and verified.\n2. The new data is written to a temporary file and verified.\n3. The original is removed and the copy takes its name, attributes and dates.\n\nThe data is written exactly as given: Drebin cannot check that the game accepts it.",
             port, directory, names[choice], usb_path, sizes[choice], (u32)st.size,
             (u32)st.size != sizes[choice]
                 ? "\nWARNING: sizes differ. Many games reject a data file of the wrong size."
                 : "",
             backup_path);
    if (!ConfirmCrossV2("REPLACE FILE CONFIRMATION", body, MCI_GUI_TONE_WARNING))
        return;

    rc = MciSaveReplaceFile(port, directory, names[choice], usb_path, backup_path, &report);
    if (report.ok) {
        snprintf(body, sizeof(body),
                 "Replaced mc%d:/%s/%s (%u -> %u bytes).\nRead back and compared: PASS.\n%s\n\nBackup of the previous save: %s",
                 port, directory, names[choice], report.plan.old_size, report.plan.new_size,
                 report.used_rename ? "Renamed in place." : "Rename unsupported: final file rewritten and verified.",
                 backup_path);
        ResetSlotReports(port);
    } else {
        snprintf(body, sizeof(body),
                 "Replace FAILED at: %s (rc=%d)\n\nOriginal file: %s\nTemporary copy: %s\nBackup: %s\n\n%s",
                 MciReplaceStageText(report.failed_stage), rc,
                 report.original_intact ? "untouched" : "removed",
                 report.temp_left_on_card ? report.plan.temp_name : "none",
                 report.failed_stage > MCI_REPLACE_STAGE_BACKUP ? backup_path : "not needed / not written",
                 report.original_intact ? "The save is unchanged."
                     : "Restore the save from the backup with IMPORT SAVE (delete the damaged save first in the PS2 browser).");
        if (report.failed_stage == MCI_REPLACE_STAGE_PREFLIGHT && report.rc < 0 &&
            report.rc >= MCI_REPLACE_E_BAD_ICONSYS) {
            size_t len = strlen(body);
            snprintf(body + len, sizeof(body) - len, "\nReason: %s", MciReplaceErrorText(report.rc));
        }
    }
    MciGuiRenderMessage("Replace file", body, "CROSS or CIRCLE returns to Card Tools.",
                        report.ok ? MCI_GUI_TONE_SUCCESS : MCI_GUI_TONE_DANGER);
    WaitForPadNeutralV2();
    WaitForModalDismiss();
}

static void RunSelectiveRestorePickerV2(int *active_port)
{
    MciImageSaveList list;
    MciImageImportReport report;
    MciSaveTransferFormat picked_format = MCI_SAVE_FORMAT_UNKNOWN;
    MciCardImageFormat format;
    char path[MCI_CARD_IMAGE_PATH_MAX];
    int target_port = *active_port;
    int row = 0;
    int first = 0;
    int free_clusters;
    int rc;
    u32 held;
    u32 pressed;

    rc = MciUsbPickerChoose(MCI_USB_PICKER_IMAGE_ANY,
                            "BROWSE / RESTORE IMAGE", "DESTINATION",
                            &target_port, path, sizeof(path), &picked_format);
    *active_port = target_port;
    if (rc == 1)
        return;
    if (rc < 0 || TransferImageFormat(picked_format, &format) < 0) {
        MciGuiRenderMessage("Image Browser",
                            "The picker did not return a supported .ps2 or .vmc image.",
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_WARNING);
        WaitForPadNeutralV2();
        WaitForModalDismiss();
        return;
    }

    rc = MciImageFsScan(path, format, &list);
    if (rc < 0 || list.save_count <= 0) {
        char message[384];
        snprintf(message, sizeof(message),
                 "The selected image could not be indexed as a PS2 save filesystem.\n\n%s\nResult: %s (rc=%d)",
                 path, MciImageFsResultText(list.result), rc);
        MciGuiRenderMessage("Image Browser", message,
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_DANGER);
        WaitForPadNeutralV2();
        WaitForModalDismiss();
        return;
    }

    rc = RefreshImageBrowserDestination(target_port, &list, &free_clusters);
    if (rc < 0) {
        MciGuiRenderMessage("Image Browser",
                            "The selected destination card could not be checked for conflicts and free space.",
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_DANGER);
        WaitForPadNeutralV2();
        WaitForModalDismiss();
        return;
    }

    WaitForPadNeutralV2();
    for (;;) {
        int requested_port = target_port;

        MciGuiRenderImageBrowser(target_port, &list, row, first, free_clusters);
        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE) {
            *active_port = target_port;
            return;
        }
        if (pressed & PAD_L1)
            requested_port = 0;
        else if (pressed & PAD_R1)
            requested_port = 1;
        if (requested_port != target_port) {
            int previous_port = target_port;
            target_port = requested_port;
            rc = RefreshImageBrowserDestination(target_port, &list, &free_clusters);
            if (rc < 0) {
                char message[224];
                target_port = previous_port;
                (void)RefreshImageBrowserDestination(target_port, &list,
                                                     &free_clusters);
                snprintf(message, sizeof(message),
                         "mc%d is not available as a formatted destination. Keeping mc%d selected.",
                         requested_port, target_port);
                MciGuiRenderMessage("Destination card unavailable", message,
                                    "CROSS or CIRCLE returns to the image browser.",
                                    MCI_GUI_TONE_WARNING);
                WaitForPadNeutralV2();
                WaitForModalDismiss();
            }
            *active_port = target_port;
            continue;
        }
        if (pressed & PAD_UP)
            row = row == 0 ? list.save_count - 1 : row - 1;
        else if (pressed & PAD_DOWN)
            row = (row + 1) % list.save_count;
        if (row < first)
            first = row;
        else if (row >= first + 7)
            first = row - 6;

        if ((pressed & PAD_SQUARE) && !list.saves[row].conflict) {
            if (list.saves[row].selected)
                list.saves[row].selected = 0;
            else if (free_clusters >= 0 &&
                     SelectedImageClusters(&list) + list.saves[row].required_clusters <=
                         (u32)free_clusters)
                list.saves[row].selected = 1;
        }

        if (pressed & PAD_TRIANGLE) {
            u32 remaining = free_clusters > 0 ? (u32)free_clusters : 0u;
            int i;
            for (i = 0; i < list.save_count; i++) {
                list.saves[i].selected = 0;
                if (!list.saves[i].conflict &&
                    list.saves[i].required_clusters <= remaining) {
                    list.saves[i].selected = 1;
                    remaining -= list.saves[i].required_clusters;
                }
            }
        }

        if ((pressed & PAD_CROSS) && SelectedImageClusters(&list) > 0u) {
            rc = MciImageFsImportSelected(target_port, &list, &report);
            if (rc == 0)
                ResetSlotReports(target_port);
            *active_port = target_port;
            ShowSelectiveRestoreResult(&report, rc);
            return;
        }
    }
}

static void RunExactRestorePickerV2(int *active_port)
{
    MciCardImageReport report;
    MciSaveTransferFormat picked_format = MCI_SAVE_FORMAT_UNKNOWN;
    MciCardImageFormat format;
    char path[MCI_CARD_IMAGE_PATH_MAX];
    char warning[560];
    int target_port = *active_port;
    int rc;

    rc = MciUsbPickerChoose(MCI_USB_PICKER_IMAGE_ANY,
                            "EXACT RESTORE IMAGE", "DESTINATION",
                            &target_port, path, sizeof(path), &picked_format);
    *active_port = target_port;
    if (rc == 1)
        return;
    if (rc < 0 || TransferImageFormat(picked_format, &format) < 0) {
        MciCardImageResetReport(&report, target_port, MCI_CARD_IMAGE_PS2);
        report.result = MCI_CARD_IMAGE_FORMAT_ERROR;
        ShowCardImageResultV2("Exact restore unavailable", &report, rc, 1);
        return;
    }

    snprintf(warning, sizeof(warning),
             "Exact-restore %s to mc%d?\n\nSource: %s\n\nThe complete destination card will be erased block-by-block. Image and destination page counts must match, and the restored card is read back in full before PASS.",
             MciCardImageFormatName(format), target_port, path);
    if (!ConfirmCardToolsDestructive("EXACT CARD RESTORE", warning))
        return;

    ShutdownNormalClients();
    rc = MciRawCardSessionStart(&RawCardStatus);
    if (rc == 0)
        rc = MciCardImageRestoreExact(target_port, path, format, &report);
    else
        MciCardImageResetReport(&report, target_port, format);
    MciRawCardSessionStop(&RawCardStatus);
    (void)RestoreAfterRawCardMode();
    if (rc == 0)
        ResetSlotReports(target_port);
    ShowCardImageResultV2("Exact card restore", &report, rc, 1);
}

static void RunForceFormatWithBackupV2(int port)
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
    ShowCardImageResultV2("Force format", &report, rc, 1);
}

static int RunCardToolsMenuV2(int port)
{
    int active_port = port;
    int item = 0;
    u32 held;
    u32 pressed;

    WaitForPadNeutralV2();
    for (;;) {
        MciGuiRenderCardToolsV2(active_port, item);
        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE)
            return active_port;
        if (pressed & PAD_L1)
            active_port = 0;
        else if (pressed & PAD_R1)
            active_port = 1;
        /* 10 tiles, 2 columns x 5 rows. */
        if (pressed & (PAD_LEFT | PAD_RIGHT))
            item = (item & ~1) | ((item & 1) ^ 1);
        if (pressed & PAD_UP)
            item = item < 2 ? item + 8 : item - 2;
        else if (pressed & PAD_DOWN)
            item = item >= 8 ? item - 8 : item + 2;
        if (!(pressed & PAD_CROSS))
            continue;

        switch (item) {
            case 0:
                RunCardImageExportAction(active_port, MCI_CARD_IMAGE_PS2);
                break;
            case 1:
                RunCardImageExportAction(active_port, MCI_CARD_IMAGE_VMC);
                break;
            case 2:
                RunSelectiveRestorePickerV2(&active_port);
                break;
            case 3:
                RunExactRestorePickerV2(&active_port);
                break;
            case 4:
                RunSaveImportV2(&active_port);
                break;
            case 5:
                RunSaveExportV2(&active_port);
                break;
            case 6:
                RunForceFormatWithBackupV2(active_port);
                break;
            case 7:
                RunSaveConvertV2(&active_port);
                break;
            case 8:
                RunReplaceFileV2(&active_port);
                break;
            default:
                return active_port;
        }
        WaitForPadNeutralV2();
    }
}

int main(int argc, char *argv[])
{
    int selected = 0;
    MciGuiPage page = MCI_GUI_CARD;
    int settings_row = 0;
    int last_video_rc = -999;
    int confirm_format = 0;
    int confirm_install = 0;
    int confirm_recovery = 0;
    int install_result_modal = 0;
    int last_format_rc = -999;
    int init_rc;
    int fmcb_rc;
    int dirty = 1;
    u64 last_marquee_tick = 0;
    u32 held;
    u32 pressed;

    (void)argc;
    (void)argv;
    MciSettingsDefaults(&Settings);
    memset(&RecoveryStatus, 0, sizeof(RecoveryStatus));
    RecoveryStatus.target_port = -1;

    init_scr();
    if (MciGuiInit() < 0) {
        scr_clear();
        scr_printf("PS2 Memory Card Inspector 0.4.1 Drebin\n\n");
        scr_printf("GS frontend initialization failed.\n");
        SleepThread();
    }

    MciGuiRenderMessage("Starting",
                        "Initializing the hardware-validated Sony ROM X memory-card stack.",
                        NULL, MCI_GUI_TONE_INFO);
    init_rc = InitNormalCardStack();
    if (init_rc < 0) {
        MciGuiRenderFatal("Initialization failed",
                          "The normal memory-card environment could not be initialized.", init_rc);
        SleepThread();
    }

    ResetSlotReports(0);
    ResetSlotReports(1);
    fmcb_rc = FmcbInitMassBackend(&FmcbMassStatus);
    if (fmcb_rc >= 0)
        (void)LoadSavedSettingsAfterMass(&last_video_rc);
    (void)RefreshRecoveryStatus();
    if (RecoveryStatus.present)
        page = MCI_GUI_FMCB;

    while (1) {
        if (!dirty && !install_result_modal && !confirm_install &&
            !confirm_recovery && MciGuiNeedsAnimation() &&
            GetTimerSystemTime() - last_marquee_tick >= MSec2TimerBusClock(80u))
            dirty = 1;
        if (dirty) {
            RenderDashboard(selected, page, settings_row, last_video_rc,
                            confirm_format, last_format_rc);
            last_marquee_tick = GetTimerSystemTime();
            dirty = 0;
        }

        pressed = ReadPadPressed(&held);

        if (install_result_modal) {
            if (pressed & (PAD_CROSS | PAD_CIRCLE)) {
                install_result_modal = 0;
                dirty = 1;
            }
            DelayThread(16000);
            continue;
        }

        if (confirm_recovery) {
            if (pressed & PAD_CIRCLE) {
                confirm_recovery = 0;
                dirty = 1;
            } else if ((pressed & PAD_TRIANGLE) &&
                       (held & PAD_L1) && (held & PAD_R1)) {
                confirm_recovery = 0;
                (void)RunPendingRecovery();
                install_result_modal = 1;
            }
            DelayThread(16000);
            continue;
        }

        if (confirm_install) {
            if (pressed & PAD_CIRCLE) {
                confirm_install = 0;
                dirty = 1;
            } else if ((pressed & PAD_SQUARE) &&
                       (held & PAD_L1) && (held & PAD_R1)) {
                confirm_install = 0;
                (void)RunVerifiedInstaller(selected);
                install_result_modal = 1;
            }
            DelayThread(16000);
            continue;
        }

        if (pressed & PAD_SELECT)
            break;

        if (confirm_format) {
            if (pressed & PAD_CIRCLE) {
                confirm_format = 0;
                dirty = 1;
            } else if ((pressed & PAD_TRIANGLE) &&
                       (held & PAD_L1) && (held & PAD_R1)) {
                last_format_rc = CardFormat(selected, &Reports[selected]);
                MagicGateResetReport(&MgReports[selected], selected);
                FmcbResetPackageReport(&FmcbReports[selected], selected);
                confirm_format = 0;
                dirty = 1;
            }
        } else {
            if (page == MCI_GUI_SETTINGS) {
                if (pressed & PAD_UP) {
                    settings_row = settings_row == 0 ? SETTINGS_ROW_COUNT - 1 : settings_row - 1;
                    dirty = 1;
                } else if (pressed & PAD_DOWN) {
                    settings_row = (settings_row + 1) % SETTINGS_ROW_COUNT;
                    dirty = 1;
                }
                if (pressed & PAD_LEFT) {
                    ChangeSetting(settings_row, -1);
                    dirty = 1;
                } else if (pressed & PAD_RIGHT) {
                    ChangeSetting(settings_row, 1);
                    dirty = 1;
                }
            } else if (pressed & (PAD_UP | PAD_DOWN)) {
                selected ^= 1;
                last_format_rc = -999;
                dirty = 1;
            }

            if (pressed & PAD_L1) {
                page = page == MCI_GUI_CARD
                           ? (MciGuiPage)(MCI_GUI_PAGE_COUNT - 1)
                           : (MciGuiPage)((unsigned int)page - 1u);
                dirty = 1;
            } else if (pressed & PAD_R1) {
                page = (MciGuiPage)(((unsigned int)page + 1u) % MCI_GUI_PAGE_COUNT);
                dirty = 1;
            }

            if (pressed & PAD_CROSS) {
                if (page == MCI_GUI_SETTINGS) {
                    if (settings_row == 0) {
                        last_video_rc = MciGuiApplyVideoMode(Settings.video_mode);
                        if (last_video_rc < 0)
                            Settings.video_mode = MciGuiCurrentVideoMode();
                    }
                } else if (held & PAD_L2) {
                    RunSelectedFullScan(selected);
                } else {
                    RunSelectedPageTest(selected, page);
                }
                dirty = 1;
            }

            if ((pressed & PAD_SQUARE) && page == MCI_GUI_SETTINGS) {
                char config_path[MCI_SETTINGS_CONFIG_PATH_MAX];
                char message[320];
                int save_rc;

                config_path[0] = '\0';
                save_rc = SaveCurrentSettings(config_path, sizeof(config_path));
                if (save_rc == 0) {
                    snprintf(message, sizeof(message),
                             "Settings saved to:\n%s\n\nThey will be loaded automatically on the next boot. The display mode is applied after USB initialization so Native remains the safe startup fallback.",
                             config_path);
                    MciGuiRenderMessage("SETTINGS SAVED", message,
                                        "CROSS or CIRCLE returns to Settings.",
                                        MCI_GUI_TONE_SUCCESS);
                } else {
                    snprintf(message, sizeof(message),
                             "Could not save MCINSPECTOR.CFG to USB (rc=%d). Current in-memory settings are unchanged.",
                             save_rc);
                    MciGuiRenderMessage("SETTINGS SAVE FAILED", message,
                                        "CROSS or CIRCLE returns to Settings.",
                                        MCI_GUI_TONE_DANGER);
                }
                install_result_modal = 1;
                dirty = 1;
            }

            if ((pressed & PAD_SQUARE) && page == MCI_GUI_FMCB) {
                (void)RefreshRecoveryStatus();
                if (RecoveryStatus.present) {
                    if (!RecoveryStatus.valid) {
                        MciGuiRenderMessage("Recovery journal requires inspection",
                                            "An FMCB recovery directory exists, but neither checksummed journal slot is valid. A new installation is blocked so the evidence is not overwritten.",
                                            "CROSS or CIRCLE returns to the dashboard.",
                                            MCI_GUI_TONE_DANGER);
                        install_result_modal = 1;
                    } else {
                        char message[512];
                        snprintf(message, sizeof(message),
                                 "Recover the interrupted FMCB transaction recorded for mc%d?\n\nState: %s\nPrepared destinations: %d\nUSB root: %s\n\nRecovery validates the transaction marker, restores captured destinations in reverse order, and removes the journal. An unarmed zero-destination journal is safely discarded without touching FMCB files.",
                                 RecoveryStatus.target_port,
                                 FmcbRecoveryStateText(RecoveryStatus.state),
                                 RecoveryStatus.prepared_files,
                                 RecoveryStatus.source_root);
                        MciGuiRenderMessage("FMCB RECOVERY CONFIRMATION", message,
                                            "Hold L1 + R1 and press TRIANGLE to recover. CIRCLE cancels.",
                                            MCI_GUI_TONE_DANGER);
                        confirm_recovery = 1;
                    }
                } else if (FmcbReports[selected].status != FMCB_PACKAGE_READY) {
                    MciGuiRenderMessage("Installer locked",
                                        "Run FMCB Preflight with CROSS first. The cross-region installer is armed only after every required I/A/E/C source and destination resolves.",
                                        "CROSS or CIRCLE returns to the dashboard.",
                                        MCI_GUI_TONE_WARNING);
                    install_result_modal = 1;
                } else {
                    const FmcbInstallPlan *plan = &FmcbReports[selected].plan;
                    const char *compact;
                    char message[640];

                    if (plan->compact_unlock_active)
                        compact = "DEX/MechaPwn DEX profile: compact reference manifest ACTIVE; CEX-only ENDVDPL is omitted.";
                    else if (plan->compact_unlock_candidate)
                        compact = "Unqualified DEX-like/region-unlocked MechaCon: CEX payload retained until this exact profile is proven on real hardware.";
                    else
                        compact = "Retail region policy: normal CEX manifest.";

                    snprintf(message, sizeof(message),
                             "Install normal FreeMcBoot to mc%d:\n\nTarget: %s/%s\nROMVER: %04X%c  MechaCon: %u.%02u\nPolicy: %s\nVerify: %s\n%s\n\nThe card, MagicGate and package will be revalidated. Space is simulated before writes. Every replaced target is persisted and verified on USB. %s An interrupted transaction can be recovered on the marked target card.",
                             selected, plan->destination_system,
                             plan->destination_osd, plan->rom_version,
                             plan->romver_region,
                             plan->console.mecha_major,
                             plan->console.mecha_minor,
                             MciConsoleRegionPolicyText(&plan->console),
                             MciInstallVerifyModeName(Settings.install_verify_mode),
                             compact,
                             Settings.install_verify_mode == MCI_INSTALL_VERIFY_ENFORCED
                                 ? "Every card write is also read back byte-for-byte."
                                 : Settings.install_verify_mode == MCI_INSTALL_VERIFY_REQUIRED
                                       ? "Required files are read back; optional files skip that second read."
                                       : "WARNING: card destinations are not read back after writing.");
                    MciGuiRenderMessage("FMCB INSTALL CONFIRMATION", message,
                                        "Hold L1 + R1 and press SQUARE to install. CIRCLE cancels.",
                                        MCI_GUI_TONE_DANGER);
                    confirm_install = 1;
                }
            }

            if ((pressed & PAD_TRIANGLE) && page != MCI_GUI_SETTINGS) {
                selected = RunCardToolsMenuV2(selected);
                confirm_format = 0;
                last_format_rc = -999;
                dirty = 1;
            }
        }
        DelayThread(16000);
    }

    MciGuiRenderMessage("Exiting", "Closing controller and USB clients.",
                        NULL, MCI_GUI_TONE_INFO);
    ShutdownNormalClients();
    SifExitRpc();
    return 0;
}
