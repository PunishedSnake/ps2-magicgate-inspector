#ifndef MCI_IMAGE_QUICK_VERIFY_H
#define MCI_IMAGE_QUICK_VERIFY_H

#include <tamtypes.h>

#include "card_image.h"

/* Revalidate image size and page-zero filesystem geometry without a second
 * complete sequential pass. Used only after Image Browser already completed a
 * full verification in the same selection workflow. */
int MciCardImageQuickReopenVerify(const char *path,
                                  MciCardImageFormat format,
                                  MciCardImageReport *report);

/* Failure-path diagnostic: locate the first PCSX2 spare/ECC mismatch and
 * return the stored and expected twelve ECC bytes. */
int MciCardImageFindFirstEccMismatch(const char *path, u32 *page_out,
                                     unsigned char actual[12],
                                     unsigned char expected[12]);

#endif /* MCI_IMAGE_QUICK_VERIFY_H */
