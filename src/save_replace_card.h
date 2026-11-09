/* SPDX-License-Identifier: MIT */
#ifndef MCI_SAVE_REPLACE_CARD_H
#define MCI_SAVE_REPLACE_CARD_H

#include <tamtypes.h>

#include "save_replace.h"

#define MCI_REPLACE_PATH_MAX 256

typedef enum MciReplaceStage {
    MCI_REPLACE_STAGE_NONE = 0,
    MCI_REPLACE_STAGE_PREFLIGHT,
    MCI_REPLACE_STAGE_BACKUP,
    MCI_REPLACE_STAGE_TEMP_WRITE,
    MCI_REPLACE_STAGE_DELETE_ORIGINAL,
    MCI_REPLACE_STAGE_RENAME,
    MCI_REPLACE_STAGE_METADATA,
    MCI_REPLACE_STAGE_FINAL_VERIFY,
    MCI_REPLACE_STAGE_DONE
} MciReplaceStage;

typedef struct MciReplaceReport {
    int ok;
    int rc;
    MciReplaceStage failed_stage;
    MciReplacePlan plan;
    int free_clusters;
    int used_rename;           /* 1: mcRename, 0: rewrote final from RAM */
    int original_intact;       /* 1 if the original file was never deleted */
    int temp_left_on_card;     /* 1 if the verified temp copy remains */
    char save_directory[33];
    char file_name[33];
    char source_path[MCI_REPLACE_PATH_MAX];
    char backup_path[MCI_REPLACE_PATH_MAX];
} MciReplaceReport;

/* Lists file names of a save directory (up to max). Returns count or < 0. */
int MciSaveListFiles(int port, const char *save_directory,
                     char names[][33], u32 *sizes, int max);

int MciSaveReplaceFile(int port, const char *save_directory, const char *file_name,
                       const char *usb_path, const char *backup_path,
                       MciReplaceReport *report);
const char *MciReplaceStageText(MciReplaceStage stage);

#endif /* MCI_SAVE_REPLACE_CARD_H */
