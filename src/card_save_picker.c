/* SPDX-License-Identifier: MIT */
/* Physical PS2 memory-card save picker used by single-save export. */

#include <tamtypes.h>
#include <delaythread.h>
#include <libmc.h>
#include <libpad.h>
#include <io_common.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_save_picker.h"
#include "gui.h"
#include "save_title.h"

#define MCI_CARD_SAVE_MAX 128
#define MCI_MC_ATTR_SUBDIR 0x0020u
#define ICON_SYS_MIN_BYTES 260

typedef struct MciCardSaveChoice {
    char directory[MCI_CARD_SAVE_DIRECTORY_MAX];
    char title[MCI_SAVE_TITLE_MAX];
} MciCardSaveChoice;

typedef struct MciCardSaveList {
    int count;
    int truncated;
    MciCardSaveChoice saves[MCI_CARD_SAVE_MAX];
} MciCardSaveList;

static int McResult(void)
{
    int result = -999;
    int rc = mcSync(MC_WAIT, NULL, &result);
    return rc < 0 ? rc : result;
}

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

static int CardReady(int port)
{
    int type = MC_TYPE_NONE;
    int free_clusters = -1;
    int formatted = 0;
    int rc = mcGetInfo(port, 0, &type, &free_clusters, &formatted);
    if (rc < 0)
        return rc;
    rc = McResult();
    if (rc < -2)
        return rc;
    return type == MC_TYPE_PS2 && formatted ? 0 : -1;
}

static int ReadIconTitle(int port, const char *directory,
                         char *title, unsigned int title_size)
{
    unsigned char data[512] __attribute__((aligned(64)));
    char path[80];
    int fd;
    int rc;

    if (title == NULL || title_size == 0u)
        return -1;
    title[0] = '\0';
    if (snprintf(path, sizeof(path), "/%s/icon.sys", directory) >= (int)sizeof(path))
        return -2;
    rc = mcOpen(port, 0, path, FIO_O_RDONLY);
    if (rc < 0)
        return rc;
    fd = McResult();
    if (fd < 0)
        return fd;
    memset(data, 0, sizeof(data));
    rc = mcRead(fd, data, sizeof(data));
    if (rc >= 0)
        rc = McResult();
    mcClose(fd);
    (void)McResult();
    if (rc < ICON_SYS_MIN_BYTES)
        return rc < 0 ? rc : -3;
    return MciSaveTitleDecodeIconSys(data, (unsigned int)rc, title, title_size);
}

static int ChoiceCompare(const void *a, const void *b)
{
    const MciCardSaveChoice *aa = (const MciCardSaveChoice *)a;
    const MciCardSaveChoice *bb = (const MciCardSaveChoice *)b;
    const char *ak = aa->title[0] ? aa->title : aa->directory;
    const char *bk = bb->title[0] ? bb->title : bb->directory;
    return strcasecmp(ak, bk);
}

static int ScanCard(int port, MciCardSaveList *list)
{
    sceMcTblGetDir *entries;
    int result;
    int rc;
    int i;

    if (list == NULL)
        return -1;
    memset(list, 0, sizeof(*list));
    rc = CardReady(port);
    if (rc < 0)
        return rc;

    entries = memalign(64, sizeof(sceMcTblGetDir) * MCI_CARD_SAVE_MAX);
    if (entries == NULL)
        return -2;
    memset(entries, 0, sizeof(sceMcTblGetDir) * MCI_CARD_SAVE_MAX);
    rc = mcGetDir(port, 0, "/*", 0, MCI_CARD_SAVE_MAX, entries);
    if (rc < 0) {
        free(entries);
        return rc;
    }
    result = McResult();
    if (result < 0) {
        free(entries);
        return result;
    }
    list->truncated = result >= MCI_CARD_SAVE_MAX;

    for (i = 0; i < result && i < MCI_CARD_SAVE_MAX; i++) {
        char name[33];
        MciCardSaveChoice *save;

        memcpy(name, entries[i].EntryName, 32);
        name[32] = '\0';
        if ((entries[i].AttrFile & MCI_MC_ATTR_SUBDIR) == 0u ||
            name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (list->count >= MCI_CARD_SAVE_MAX) {
            list->truncated = 1;
            break;
        }
        save = &list->saves[list->count++];
        snprintf(save->directory, sizeof(save->directory), "%s", name);
        if (ReadIconTitle(port, name, save->title, sizeof(save->title)) < 0)
            save->title[0] = '\0';
    }
    free(entries);
    if (list->count > 1)
        qsort(list->saves, (size_t)list->count, sizeof(list->saves[0]), ChoiceCompare);
    return 0;
}

static void RenderList(int port, const MciCardSaveList *list, int row, int first)
{
    char body[1024];
    int used;
    int i;

    used = snprintf(body, sizeof(body),
                    "SOURCE  mc%d\nPS2 save directories: %d%s\n\n",
                    port, list->count, list->truncated ? "+" : "");
    for (i = 0; i < 7 && first + i < list->count && used > 0 &&
                (unsigned int)used < sizeof(body); i++) {
        const MciCardSaveChoice *save = &list->saves[first + i];
        const char *label = save->title[0] ? save->title : save->directory;
        int n = snprintf(body + used, sizeof(body) - (unsigned int)used,
                         "%c %-42.42s  %-28.28s\n",
                         first + i == row ? '>' : ' ', label,
                         save->title[0] ? save->directory : "");
        if (n < 0)
            break;
        used += n;
    }
    MciGuiRenderMessage("EXPORT SAVE TO PSU", body,
                        "UP/DOWN Move   X Choose   L1 mc0   R1 mc1   CIRCLE Cancel",
                        MCI_GUI_TONE_INFO);
}

int MciCardSavePickerChoose(int *card_port,
                            char *directory,
                            unsigned int directory_size)
{
    MciCardSaveList list;
    int current_port;
    int row = 0;
    int first = 0;
    int scan_rc;
    u32 old_state = 0;

    if (card_port == NULL || directory == NULL || directory_size < 2u)
        return -1;
    current_port = (*card_port == 1) ? 1 : 0;
    directory[0] = '\0';
    scan_rc = ScanCard(current_port, &list);
    WaitNeutral();

    for (;;) {
        u32 pressed;
        if (scan_rc < 0) {
            char body[256];
            snprintf(body, sizeof(body),
                     "mc%d is not available as a formatted PS2 source card (rc=%d).\n\nUse L1/R1 to choose another slot.",
                     current_port, scan_rc);
            MciGuiRenderMessage("EXPORT SAVE TO PSU", body,
                                "L1 mc0   R1 mc1   CIRCLE Cancel",
                                MCI_GUI_TONE_WARNING);
        } else if (list.count == 0) {
            char body[192];
            snprintf(body, sizeof(body),
                     "mc%d contains no top-level PS2 save directories.", current_port);
            MciGuiRenderMessage("EXPORT SAVE TO PSU", body,
                                "L1 mc0   R1 mc1   CIRCLE Cancel",
                                MCI_GUI_TONE_WARNING);
        } else {
            if (row >= list.count)
                row = list.count - 1;
            RenderList(current_port, &list, row, first);
        }

        do {
            pressed = ReadPressed(&old_state);
            if (pressed == 0u)
                DelayThread(16000);
        } while (pressed == 0u);

        if (pressed & PAD_CIRCLE)
            return 1;
        if (pressed & PAD_L1) {
            current_port = 0;
            row = first = 0;
            scan_rc = ScanCard(current_port, &list);
            continue;
        }
        if (pressed & PAD_R1) {
            current_port = 1;
            row = first = 0;
            scan_rc = ScanCard(current_port, &list);
            continue;
        }
        if (scan_rc < 0 || list.count <= 0)
            continue;
        if (pressed & PAD_UP)
            row = row == 0 ? list.count - 1 : row - 1;
        else if (pressed & PAD_DOWN)
            row = (row + 1) % list.count;
        if (row < first)
            first = row;
        else if (row >= first + 7)
            first = row - 6;

        if (pressed & PAD_CROSS) {
            if (strlen(list.saves[row].directory) + 1u > directory_size)
                return -2;
            snprintf(directory, directory_size, "%s", list.saves[row].directory);
            *card_port = current_port;
            return 0;
        }
    }
}
