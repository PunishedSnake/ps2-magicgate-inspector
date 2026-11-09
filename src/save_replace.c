/* SPDX-License-Identifier: MIT */
/* Planning for single-file replacement inside a PS2 save. */

#include <tamtypes.h>
#include <stdio.h>
#include <string.h>

#include "save_replace.h"

const char *MciReplaceErrorText(int rc)
{
    switch (rc) {
        case MCI_REPLACE_OK: return "ok";
        case MCI_REPLACE_E_ARGS: return "bad arguments";
        case MCI_REPLACE_E_NAME: return "unsafe file name";
        case MCI_REPLACE_E_TOO_BIG: return "replacement is too large";
        case MCI_REPLACE_E_NO_SPACE: return "not enough free space for a safe replace";
        case MCI_REPLACE_E_TEMP_NAME: return "no free temporary name";
        case MCI_REPLACE_E_BAD_ICONSYS: return "replacement icon.sys is not a PS2 icon.sys";
        default: return "unknown error";
    }
}

static u32 Clusters(u32 bytes)
{
    return (bytes + 1023u) / 1024u;
}

static int NameTaken(const char *name, const char *const *existing, u32 count)
{
    u32 i;
    for (i = 0u; i < count; i++)
        if (existing[i] != NULL && strcmp(existing[i], name) == 0)
            return 1;
    return 0;
}

int MciReplacePlanMake(const char *file_name, u32 old_size, u32 new_size,
                       const unsigned char *new_head, u32 new_head_size,
                       const char *const *existing, u32 existing_count,
                       int free_clusters, MciReplacePlan *plan)
{
    unsigned int attempt;
    size_t base_len;

    if (file_name == NULL || plan == NULL)
        return MCI_REPLACE_E_ARGS;
    memset(plan, 0, sizeof(*plan));
    if (!MciSaveNameIsSafe(file_name))
        return MCI_REPLACE_E_NAME;
    if (new_size > MCI_SAVE_MAX_BYTES)
        return MCI_REPLACE_E_TOO_BIG;

    plan->old_size = old_size;
    plan->new_size = new_size;
    plan->old_clusters = Clusters(old_size);
    plan->new_clusters = Clusters(new_size);
    plan->size_changed = old_size != new_size;
    plan->is_icon_sys = strcmp(file_name, "icon.sys") == 0;
    if (plan->is_icon_sys &&
        (new_head == NULL || new_head_size < 4u || new_size < 964u ||
         memcmp(new_head, "PS2D", 4) != 0))
        return MCI_REPLACE_E_BAD_ICONSYS;

    /* Temp file and original coexist, plus possibly one new directory
     * cluster for the extra entry. */
    plan->required_free = plan->new_clusters + 1u;
    if (free_clusters < 0 || (u32)free_clusters < plan->required_free)
        return MCI_REPLACE_E_NO_SPACE;

    /* Temp name: "<name>.mci" or "<truncated>~N.mci", <= 32 chars. */
    base_len = strlen(file_name);
    for (attempt = 0u; attempt < 10u; attempt++) {
        char suffix[8];
        size_t keep;
        snprintf(suffix, sizeof(suffix), attempt ? "~%u.mci" : ".mci", attempt);
        keep = base_len;
        if (keep + strlen(suffix) > MCI_SAVE_NAME_MAX)
            keep = MCI_SAVE_NAME_MAX - strlen(suffix);
        memcpy(plan->temp_name, file_name, keep);
        memcpy(plan->temp_name + keep, suffix, strlen(suffix) + 1u);
        if (!NameTaken(plan->temp_name, existing, existing_count) &&
            strcmp(plan->temp_name, file_name) != 0)
            return MCI_REPLACE_OK;
    }
    plan->temp_name[0] = '\0';
    return MCI_REPLACE_E_TEMP_NAME;
}
