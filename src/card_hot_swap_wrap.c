/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>

#include "card_hot_swap.h"
#include "card_image_fs.h"
#include "save_transfer.h"

int __real_MciImageFsRefreshTargetConflicts(int target_port,
                                            MciImageSaveList *list);
int __real_MciSaveTransferImportFile(int target_port, const char *path,
                                     MciSaveTransferReport *report);

int __wrap_MciImageFsRefreshTargetConflicts(int target_port,
                                            MciImageSaveList *list)
{
    int rc = MciNormalCardProbeFormatted(target_port, NULL);
    if (rc < 0)
        return rc;
    return __real_MciImageFsRefreshTargetConflicts(target_port, list);
}

int __wrap_MciSaveTransferImportFile(int target_port, const char *path,
                                     MciSaveTransferReport *report)
{
    MciSaveTransferFormat format = MciSaveTransferFormatFromPath(path);
    int rc = MciNormalCardProbeFormatted(target_port, NULL);

    if (rc < 0) {
        if (report != NULL) {
            MciSaveTransferResetReport(report, target_port, format);
            report->result = MCI_SAVE_TRANSFER_TARGET_UNAVAILABLE;
            if (path != NULL)
                snprintf(report->source_path, sizeof(report->source_path),
                         "%s", path);
        }
        return rc;
    }
    return __real_MciSaveTransferImportFile(target_port, path, report);
}
