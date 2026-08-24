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
#include "save_transfer.h"
#include "usb_file_picker.h"
#include "usb_file_picker_ui.h"

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
    char message[760];
    MciGuiTone tone;

    if (report->result == MCI_SAVE_TRANSFER_OK)
        tone = MCI_GUI_TONE_SUCCESS;
    else if (report->result == MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT ||
             report->result == MCI_SAVE_TRANSFER_TARGET_CONFLICT)
        tone = MCI_GUI_TONE_WARNING;
    else
        tone = MCI_GUI_TONE_DANGER;

    snprintf(message, sizeof(message),
             "Result: %s (rc=%d)\nFormat: %s\nCard: mc%d\n\nSource: %s\nDestination: %s\nSave directory: %s\nFiles: %d/%d  verified: %d\nBytes: %u\nRequired clusters: %d  target free: %d\nRollback rc: %d\nFailed path: %s",
             MciSaveTransferResultText(report->result), rc,
             MciSaveTransferFormatName(report->format), report->card_port,
             report->source_path[0] ? report->source_path : "n/a",
             report->destination[0] ? report->destination : "n/a",
             report->save_directory[0] ? report->save_directory : "n/a",
             report->files_written, report->files_total,
             report->files_verified, report->bytes_written,
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

    if (probe.format != MCI_SAVE_FORMAT_PSU) {
        MciSaveTransferResetReport(&report, target_port, probe.format);
        report.result = MCI_SAVE_TRANSFER_UNSUPPORTED_FORMAT;
        snprintf(report.source_path, sizeof(report.source_path), "%s", path);
        snprintf(report.destination, sizeof(report.destination), "mc%d:", target_port);
        ShowSaveTransferResultV2("Save import", &report, -300);
        return;
    }

    snprintf(confirm, sizeof(confirm),
             "Import this save to mc%d?\n\nSource: %s\nFormat: %s\nSize: %u KiB\n\nThe destination must be a formatted PS2 card. Existing top-level save directories are never overwritten. Every new file is reopened and compared byte-for-byte before PASS; a failed transaction rolls back only objects it created.",
             target_port, path, MciSaveTransferFormatName(probe.format),
             (unsigned int)((probe.size + 1023u) / 1024u));
    if (!ConfirmCrossV2("SAVE IMPORT CONFIRMATION", confirm,
                        MCI_GUI_TONE_WARNING))
        return;

    rc = MciSaveTransferImportFile(target_port, path, &report);
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
        char message[320];
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
        if (pressed & (PAD_LEFT | PAD_RIGHT))
            item = (item & ~1) | ((item & 1) ^ 1);
        if (pressed & PAD_UP)
            item = item < 2 ? item + 6 : item - 2;
        else if (pressed & PAD_DOWN)
            item = item >= 6 ? item - 6 : item + 2;
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
        scr_printf("PS2 Memory Card Inspector 0.4.0-dev4 Drebin\n\n");
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
    (void)fmcb_rc;
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
                        char message[360];
                        snprintf(message, sizeof(message),
                                 "Recover the interrupted FMCB transaction recorded for mc%d?\n\nState: %s\nPrepared destinations: %d\nUSB root: %s\n\nRecovery validates the card transaction marker, restores every captured destination in reverse order, verifies restored files, then removes the journal.",
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
                                        "Run FMCB Preflight with CROSS first. The normal installer is armed only for a package that resolves every required source and destination.",
                                        "CROSS or CIRCLE returns to the dashboard.",
                                        MCI_GUI_TONE_WARNING);
                    install_result_modal = 1;
                } else {
                    const FmcbInstallPlan *plan = &FmcbReports[selected].plan;
                    const char *compact;
                    char message[640];

                    if (plan->compact_unlock_active)
                        compact = "Real DEX profile: compact reference manifest ACTIVE; ENDVDPL is omitted.";
                    else if (plan->compact_unlock_candidate)
                        compact = "DEX-like/region-unlocked MechaCon detected: compact manifest candidate found, but the CEX payload is retained until hardware validation proves ENDVDPL can be omitted safely.";
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
