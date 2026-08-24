/* SPDX-License-Identifier: MIT */
#ifndef MCI_CARD_SAVE_PICKER_H
#define MCI_CARD_SAVE_PICKER_H

#define MCI_CARD_SAVE_DIRECTORY_MAX 33

/* Pick one top-level PS2 save directory from a physical card.
 * L1/R1 changes the source slot in-place. Returns 0 on selection, 1 on user
 * cancellation and <0 on card/filesystem failure. */
int MciCardSavePickerChoose(int *card_port,
                            char *directory,
                            unsigned int directory_size);

#endif /* MCI_CARD_SAVE_PICKER_H */
