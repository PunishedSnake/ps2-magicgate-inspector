/* SPDX-License-Identifier: MIT */
#ifndef MCI_SAVE_REPLACE_H
#define MCI_SAVE_REPLACE_H

/*
 * "Replace one file inside an existing PS2 save" planning (portable).
 *
 * Card-side sequence (save_replace_card.c):
 *   1. export the whole save to a verified .psu backup on USB;
 *   2. write the new data to a temporary sibling name and verify it;
 *   3. delete the original file;
 *   4. rename temp -> original (or, if rename is unavailable, write the
 *      original name from RAM and verify, then delete the temp);
 *   5. restore the original file's attributes and timestamps.
 * Any failure before step 3 leaves the save untouched; after it the backup
 * and the verified temp copy both exist and the report names them.
 */

#include <tamtypes.h>

#include "save_archive.h"

typedef struct MciReplacePlan {
    u32 old_size;
    u32 new_size;
    u32 old_clusters;
    u32 new_clusters;
    u32 required_free;      /* clusters needed while temp and original coexist */
    int size_changed;
    int is_icon_sys;
    char temp_name[MCI_SAVE_NAME_MAX + 1u];
} MciReplacePlan;

typedef enum MciReplaceError {
    MCI_REPLACE_OK = 0,
    MCI_REPLACE_E_ARGS = -1,
    MCI_REPLACE_E_NAME = -2,
    MCI_REPLACE_E_TOO_BIG = -3,
    MCI_REPLACE_E_NO_SPACE = -4,
    MCI_REPLACE_E_TEMP_NAME = -5,
    MCI_REPLACE_E_BAD_ICONSYS = -6
} MciReplaceError;

const char *MciReplaceErrorText(int rc);

/* existing: names already in the save directory (for temp-name uniqueness).
 * new_head: first bytes of the replacement (checked for icon.sys). */
int MciReplacePlanMake(const char *file_name, u32 old_size, u32 new_size,
                       const unsigned char *new_head, u32 new_head_size,
                       const char *const *existing, u32 existing_count,
                       int free_clusters, MciReplacePlan *plan);

#endif /* MCI_SAVE_REPLACE_H */
