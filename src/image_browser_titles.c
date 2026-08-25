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

/*
 * Transitional P0 view adapter.
 *
 * The old renderer copied the complete MciImageSaveList every time the browser
 * was redrawn just so icon.sys titles could replace the visible name field.
 * The list can hold 128 save records, so that copy needlessly churned a working
 * set larger than the EE D-cache.
 *
 * Card Tools and this renderer are single-threaded and synchronous. Temporarily
 * replace only the at-most-seven visible name fields, render immediately, then
 * restore the authoritative filesystem identifiers before returning. The raw
 * BASLUS/BESLES/etc names are therefore never observed by restore/conflict code
 * in their presentation form.
 *
 * This adapter disappears once display_title becomes part of the filesystem
 * index itself. Until then the title resolver still opens the image during the
 * first cache fill, so DREBIN.LOG mass writes remain paused around resolution.
 */
void __wrap_MciGuiRenderImageBrowser(int target_port,
                                     const MciImageSaveList *list,
                                     int selected_row,
                                     int first_row,
                                     int free_clusters)
{
    char saved_names[7][MCI_IMAGE_SAVE_NAME_MAX];
    int saved_index[7];
    int saved_count = 0;
    int i;
    MciImageSaveList *mutable_list;

    if (list == NULL) {
        __real_MciGuiRenderImageBrowser(target_port, list, selected_row,
                                        first_row, free_clusters);
        return;
    }

    mutable_list = (MciImageSaveList *)(void *)list;
    MciDiagLogSetMassWritePaused(1);
    for (i = 0; i < 7; i++) {
        int index = first_row + i;
        const char *title;

        if (index < 0 || index >= list->save_count ||
            saved_count >= (int)(sizeof(saved_index) / sizeof(saved_index[0])))
            break;
        title = MciImageSaveDisplayTitle(list, index);
        if (title == NULL || title[0] == '\0' ||
            strcmp(title, list->saves[index].name) == 0)
            continue;

        saved_index[saved_count] = index;
        snprintf(saved_names[saved_count], sizeof(saved_names[saved_count]),
                 "%s", list->saves[index].name);
        snprintf(mutable_list->saves[index].name,
                 sizeof(mutable_list->saves[index].name), "%s", title);
        saved_count++;
    }
    MciDiagLogSetMassWritePaused(0);

    __real_MciGuiRenderImageBrowser(target_port, list, selected_row,
                                    first_row, free_clusters);

    for (i = 0; i < saved_count; i++) {
        int index = saved_index[i];
        snprintf(mutable_list->saves[index].name,
                 sizeof(mutable_list->saves[index].name), "%s",
                 saved_names[i]);
    }
}
