/* SPDX-License-Identifier: MIT */
#ifndef MCI_IMAGE_SAVE_TITLE_H
#define MCI_IMAGE_SAVE_TITLE_H

#include "card_image_fs.h"

/* Returns a cached human-readable icon.sys title for a save when one can be
 * decoded safely, otherwise returns the save's raw directory name. The raw
 * MciImageSaveEntry is never modified and remains authoritative for restore. */
const char *MciImageSaveDisplayTitle(const MciImageSaveList *list, int index);
void MciImageSaveTitleInvalidate(void);

#endif /* MCI_IMAGE_SAVE_TITLE_H */
