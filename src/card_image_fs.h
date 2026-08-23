#ifndef MCI_CARD_IMAGE_FS_H
#define MCI_CARD_IMAGE_FS_H

#include <tamtypes.h>

#include "card_image.h"

#define MCI_IMAGE_SAVE_MAX 128
#define MCI_IMAGE_SAVE_NAME_MAX 33
#define MCI_IMAGE_SAVE_PATH_MAX 256

typedef enum MciImageFsResult {
    MCI_IMAGE_FS_NOT_RUN = 0,
    MCI_IMAGE_FS_OK,
    MCI_IMAGE_FS_IMAGE_INVALID,
    MCI_IMAGE_FS_CORRUPT,
    MCI_IMAGE_FS_TOO_MANY_SAVES,
    MCI_IMAGE_FS_TARGET_UNAVAILABLE,
    MCI_IMAGE_FS_TARGET_FULL,
    MCI_IMAGE_FS_CONFLICT,
    MCI_IMAGE_FS_IMPORT_FAILED,
    MCI_IMAGE_FS_ROLLBACK_FAILED
} MciImageFsResult;

typedef struct MciImageSaveEntry {
    char name[MCI_IMAGE_SAVE_NAME_MAX];
    u16 mode;
    u32 start_cluster;
    u32 entry_count;
    u32 total_bytes;
    u32 file_count;
    u32 directory_count;
    u32 required_clusters;
    int selected;
    int conflict;
} MciImageSaveEntry;

typedef struct MciImageSaveList {
    char path[MCI_CARD_IMAGE_PATH_MAX];
    MciCardImageFormat format;
    MciCardGeometry geometry;
    MciImageFsResult result;
    int save_count;
    int truncated;
    u32 total_bytes;
    MciImageSaveEntry saves[MCI_IMAGE_SAVE_MAX];
} MciImageSaveList;

typedef struct MciImageImportReport {
    MciImageFsResult result;
    int target_port;
    int selected_saves;
    int restored_saves;
    int conflict_saves;
    int failed_save_index;
    int rollback_rc;
    int target_free_clusters;
    u32 required_clusters;
    u32 bytes_written;
    u32 files_written;
    u32 directories_written;
    char failed_path[MCI_IMAGE_SAVE_PATH_MAX];
} MciImageImportReport;

const char *MciImageFsResultText(MciImageFsResult result);
int MciImageFsScan(const char *path, MciCardImageFormat format,
                   MciImageSaveList *list);
int MciImageFsRefreshTargetConflicts(int target_port, MciImageSaveList *list);
int MciImageFsImportSelected(int target_port, MciImageSaveList *list,
                             MciImageImportReport *report);

#endif /* MCI_CARD_IMAGE_FS_H */
