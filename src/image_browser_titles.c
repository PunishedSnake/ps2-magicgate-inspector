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
 * New filesystem indexes carry display_title directly. Older/partial indexes
 * still fall back to the standalone resolver until that duplicate parser is
 * completely retired. Only the at-most-seven visible filesystem-name fields are
 * temporarily substituted for the synchronous renderer and restored before the
 * caller regains control; restore/conflict identity stays authoritative.
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
    int needs_legacy_resolver = 0;
    int i;
    MciImageSaveList *mutable_list;

    if (list == NULL) {
        __real_MciGuiRenderImageBrowser(target_port, list, selected_row,
                                        first_row, free_clusters);
        return;
    }

    mutable_list = (MciImageSaveList *)(void *)list;
    for (i = 0; i < 7; i++) {
        int index = first_row + i;
        if (index < 0 || index >= list->save_count)
            break;
        if (list->saves[index].display_title[0] == '\0') {
            needs_legacy_resolver = 1;
            break;
        }
    }

    if (needs_legacy_resolver)
        MciDiagLogSetMassWritePaused(1);

    for (i = 0; i < 7; i++) {
        int index = first_row + i;
        const char *title;

        if (index < 0 || index >= list->save_count ||
            saved_count >= (int)(sizeof(saved_index) / sizeof(saved_index[0])))
            break;

        title = list->saves[index].display_title[0] != '\0'
                    ? list->saves[index].display_title
                    : MciImageSaveDisplayTitle(list, index);
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

    if (needs_legacy_resolver)
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
