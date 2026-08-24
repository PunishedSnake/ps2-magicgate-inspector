/* SPDX-License-Identifier: MIT */
/* Presentation-only title overlay for Image Browser. */

#include <stdio.h>
#include <string.h>

#include "diag_log.h"
#include "gui.h"
#include "image_save_title.h"

void __real_MciGuiRenderImageBrowser(int target_port,
                                     const MciImageSaveList *list,
                                     int selected_row,
                                     int first_row,
                                     int free_clusters);

/* Keep the filesystem identifier untouched in the real list. The renderer gets
 * a display copy with icon.sys titles substituted only for its visible name
 * field, so restore/conflict code continues to use BASLUS/BESLES/etc exactly.
 *
 * USBHDFSD hardware rule: the title resolver opens the image again while it
 * walks icon.sys files. Persistent DREBIN.LOG writes therefore stay paused for
 * the whole cache-fill interval, even though this path is read-only. We do not
 * assume that cross-descriptor corruption is safe merely because the image fd
 * was opened O_RDONLY. */
void __wrap_MciGuiRenderImageBrowser(int target_port,
                                     const MciImageSaveList *list,
                                     int selected_row,
                                     int first_row,
                                     int free_clusters)
{
    static MciImageSaveList display;
    int i;

    if (list == NULL) {
        __real_MciGuiRenderImageBrowser(target_port, list, selected_row,
                                        first_row, free_clusters);
        return;
    }
    memcpy(&display, list, sizeof(display));
    MciDiagLogSetMassWritePaused(1);
    for (i = 0; i < display.save_count && i < MCI_IMAGE_SAVE_MAX; i++) {
        const char *title = MciImageSaveDisplayTitle(list, i);
        if (title != NULL && title[0] != '\0')
            snprintf(display.saves[i].name, sizeof(display.saves[i].name),
                     "%s", title);
    }
    MciDiagLogSetMassWritePaused(0);
    __real_MciGuiRenderImageBrowser(target_port, &display, selected_row,
                                    first_row, free_clusters);
}
