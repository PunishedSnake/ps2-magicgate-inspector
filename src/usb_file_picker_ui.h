/* SPDX-License-Identifier: MIT */
#ifndef MCI_USB_FILE_PICKER_UI_H
#define MCI_USB_FILE_PICKER_UI_H

#include "usb_file_picker.h"

/* Interactive Card Tools picker. card_port may be NULL when the operation has
 * no card role; otherwise L1/R1 updates it in-place and the selected slot is
 * shown explicitly as SOURCE or DESTINATION according to card_role.
 *
 * Returns 0 with path/format filled, 1 for user cancellation, <0 for I/O or
 * bounds failure. */
int MciUsbPickerChoose(MciUsbPickerFilter filter,
                       const char *title,
                       const char *card_role,
                       int *card_port,
                       char *path,
                       unsigned int path_size,
                       MciSaveTransferFormat *format);

#endif /* MCI_USB_FILE_PICKER_UI_H */
