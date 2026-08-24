/* SPDX-License-Identifier: MIT */
/* Presentation-only title overlay for Image Browser. */

#include <stdio.h>
#include <string.h>

#include "gui.h"
#include "image_save_title.h"

void __real_MciGuiRenderImageBrowser(int target_port,
                                     const MciImageSaveList *list,
                                     int selected_row,
                                     int first_row,
                                     int free_clusters);

/* Keep the filesystem identifier untouched in the real list. The renderer gets
 * a display copy with icon.sys titles substituted only for its visible name
 * field, so restore/conflict code continues to use BASLUS/BESLES/etc exactly. */
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
    for (i = 0; i < display.save_count && i < MCI_IMAGE_SAVE_MAX; i++) {
        const char *title = MciImageSaveDisplayTitle(list, i);
        if (title != NULL && title[0] != '\0')
            snprintf(display.saves[i].name, sizeof(display.saves[i].name),
                     "%s", title);
    }
    __real_MciGuiRenderImageBrowser(target_port, &display, selected_row,
                                    first_row, free_clusters);
}
