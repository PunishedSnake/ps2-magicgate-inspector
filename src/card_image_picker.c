/* SPDX-License-Identifier: MIT */
/* Interactive replacement for the old newest-Drebin-image lookup. */

#include <stdio.h>

#include "card_image.h"
#include "usb_file_picker_ui.h"

int __real_MciCardImageFindLatest(int port, MciCardImageFormat format,
                                  char *path, unsigned int path_size);

int __wrap_MciCardImageFindLatest(int port, MciCardImageFormat format,
                                  char *path, unsigned int path_size)
{
    MciUsbPickerFilter filter;
    MciSaveTransferFormat picked_format;
    char title[80];
    int rc;

    (void)port;
    if (path == NULL || path_size == 0u)
        return -1;
    filter = format == MCI_CARD_IMAGE_PS2
                 ? MCI_USB_PICKER_IMAGE_PS2
                 : MCI_USB_PICKER_IMAGE_VMC;
    snprintf(title, sizeof(title), "CHOOSE %s IMAGE",
             MciCardImageFormatName(format));
    rc = MciUsbPickerChoose(filter, title, NULL, NULL,
                            path, path_size, &picked_format);
    if (rc == 0)
        return 0;
    /* The legacy Card Tools caller treats every negative lookup as "no image".
     * Preserve an explicit negative cancellation code until Card Tools v2 owns
     * the picker directly and can distinguish cancel from I/O failure. */
    return rc == 1 ? -2000 : rc;
}
