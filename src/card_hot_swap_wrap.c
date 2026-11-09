/* SPDX-License-Identifier: MIT */

#include <libmc.h>
#include <stdio.h>
#include <string.h>

#include "card_hot_swap.h"
#include "card_image_fs.h"
#include "card_save_picker.h"
#include "save_transfer.h"

int __real_MciImageFsRefreshTargetConflicts(int target_port,
                                            MciImageSaveList *list);
int __real_MciSaveTransferImportFile(int target_port, const char *path,
                                     MciSaveTransferReport *report);
int __real_MciCardSavePickerChoose(int *card_port, char *directory,
                                   unsigned int directory_size);

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
    MciSaveTransferProbe probe;
    MciSaveTransferFormat format = MciSaveTransferFormatFromPath(path);
    int expected = MC_TYPE_PS2;
    int rc;

    /* Settle the slot for the card generation the container needs: PS1
     * containers go to PS1 cards, everything else to PS2 cards. */
    if (path != NULL && MciSaveTransferProbeFile(path, &probe) == 0) {
        format = probe.format;
        if (probe.generation == 1)
            expected = MC_TYPE_PSX;
    }
    rc = MciNormalCardProbeFormattedType(target_port, expected, NULL);

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

int __wrap_MciCardSavePickerChoose(int *card_port, char *directory,
                                   unsigned int directory_size)
{
    /* Settle both slots before the interactive picker so L1/R1 can move to a
     * card that was physically replaced since the previous screen. Empty slots
     * are allowed here; the real picker will present their normal warning. */
    (void)MciNormalCardProbeFormatted(0, NULL);
    (void)MciNormalCardProbeFormatted(1, NULL);
    return __real_MciCardSavePickerChoose(card_port, directory, directory_size);
}
