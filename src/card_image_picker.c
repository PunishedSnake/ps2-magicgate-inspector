/* SPDX-License-Identifier: MIT */
/* Interactive replacement for the old newest-Drebin-image lookup. */

#include <tamtypes.h>
#include <delaythread.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>

#include "card_image.h"
#include "gui.h"
#include "usb_file_picker.h"

int __real_MciCardImageFindLatest(int port, MciCardImageFormat format,
                                  char *path, unsigned int path_size);

static int PadState(u32 *state)
{
    struct padButtonStatus buttons;
    int rc = padGetState(0, 0);
    if (rc != PAD_STATE_STABLE && rc != PAD_STATE_FINDCTP1)
        return 0;
    if (padRead(0, 0, &buttons) == 0)
        return 0;
    *state = 0xFFFFu ^ buttons.btns;
    return 1;
}

static void WaitNeutral(void)
{
    u32 state = 0;
    int stable = 0;
    while (stable < 2) {
        if (PadState(&state) && state == 0u)
            stable++;
        else
            stable = 0;
        DelayThread(16000);
    }
}

static u32 ReadPressed(u32 *old_state)
{
    u32 state = *old_state;
    u32 pressed = 0;
    if (PadState(&state)) {
        pressed = state & ~*old_state;
        *old_state = state;
    }
    return pressed;
}

static void SizeText(u64 size, char out[20])
{
    if (size >= 1024u * 1024u)
        snprintf(out, 20, "%u MiB", (unsigned int)(size / (1024u * 1024u)));
    else if (size >= 1024u)
        snprintf(out, 20, "%u KiB", (unsigned int)((size + 1023u) / 1024u));
    else
        snprintf(out, 20, "%u B", (unsigned int)size);
}

static void RenderPicker(int target_port, MciCardImageFormat format,
                         const MciUsbPickerList *list, int row, int first)
{
    char body[920];
    char path_line[104];
    const char *tail;
    int i;
    int used;

    tail = strlen(list->path) > 72u ? list->path + strlen(list->path) - 72u
                                   : list->path;
    snprintf(path_line, sizeof(path_line), "%s%s",
             strlen(list->path) > 72u ? "..." : "", tail);
    used = snprintf(body, sizeof(body),
                    "SOURCE FILE  %s\nDESTINATION  mc%d   expected: %s\n\n",
                    path_line, target_port, MciCardImageFormatName(format));
    for (i = 0; i < 7 && first + i < list->entry_count && used > 0 &&
                (unsigned int)used < sizeof(body); i++) {
        const MciUsbPickerEntry *entry = &list->entries[first + i];
        char size[20];
        int n;
        if (entry->is_directory)
            snprintf(size, sizeof(size), "<DIR>");
        else
            SizeText(entry->size, size);
        n = snprintf(body + used, sizeof(body) - (unsigned int)used,
                     "%c %-43.43s %10s\n",
                     first + i == row ? '>' : ' ', entry->name, size);
        if (n < 0)
            break;
        used += n;
    }
    if (list->truncated && (unsigned int)used < sizeof(body) - 40u)
        snprintf(body + used, sizeof(body) - (unsigned int)used,
                 "\n[Directory list truncated to %d entries]",
                 MCI_USB_PICKER_MAX_ENTRIES);

    MciGuiRenderMessage("USB IMAGE FILE PICKER", body,
                        "UP/DOWN Move   X Open/Choose   CIRCLE Parent/Cancel   TRIANGLE USB device",
                        MCI_GUI_TONE_INFO);
}

static int PickImage(int target_port, MciCardImageFormat format,
                     char *path, unsigned int path_size)
{
    MciUsbPickerList list;
    MciUsbPickerList next;
    MciUsbPickerFilter filter = format == MCI_CARD_IMAGE_PS2
                                    ? MCI_USB_PICKER_IMAGE_PS2
                                    : MCI_USB_PICKER_IMAGE_VMC;
    int row = 0;
    int first = 0;
    int rc;
    u32 old_state = 0;

    rc = MciUsbPickerOpenFirst(filter, &list);
    if (rc < 0) {
        MciGuiRenderMessage("USB image picker unavailable",
                            "No readable mass:/, mass0:/ or mass1:/ filesystem could be opened.",
                            "CROSS or CIRCLE returns.", MCI_GUI_TONE_WARNING);
        return rc;
    }
    WaitNeutral();
    for (;;) {
        u32 pressed;
        if (list.entry_count == 0)
            row = first = 0;
        else if (row >= list.entry_count)
            row = list.entry_count - 1;
        RenderPicker(target_port, format, &list, row, first);
        do {
            pressed = ReadPressed(&old_state);
            if (pressed == 0u)
                DelayThread(16000);
        } while (pressed == 0u);

        if ((pressed & PAD_UP) && list.entry_count > 0)
            row = row == 0 ? list.entry_count - 1 : row - 1;
        else if ((pressed & PAD_DOWN) && list.entry_count > 0)
            row = (row + 1) % list.entry_count;
        if (row < first)
            first = row;
        else if (row >= first + 7)
            first = row - 6;

        if (pressed & PAD_TRIANGLE) {
            if (MciUsbPickerCycleRoot(&list, 1, &next) == 0) {
                list = next;
                row = first = 0;
            }
            continue;
        }
        if (pressed & PAD_CIRCLE) {
            rc = MciUsbPickerParent(&list, &next);
            if (rc == 0) {
                list = next;
                row = first = 0;
                continue;
            }
            /* Existing Card Tools callers still treat every negative lookup as
             * "no image". Keep cancellation explicit for now; the v2 menu will
             * consume this code without the legacy warning modal. */
            return -2000;
        }
        if ((pressed & PAD_CROSS) && list.entry_count > 0) {
            const MciUsbPickerEntry *entry = &list.entries[row];
            if (entry->is_directory) {
                rc = MciUsbPickerEnter(&list, row, &next);
                if (rc == 0) {
                    list = next;
                    row = first = 0;
                }
                continue;
            }
            if (strlen(entry->path) + 1u > path_size)
                return -2001;
            snprintf(path, path_size, "%s", entry->path);
            return 0;
        }
    }
}

int __wrap_MciCardImageFindLatest(int port, MciCardImageFormat format,
                                  char *path, unsigned int path_size)
{
    if (path == NULL || path_size == 0u)
        return -1;
    path[0] = '\0';
    return PickImage(port, format, path, path_size);
}
