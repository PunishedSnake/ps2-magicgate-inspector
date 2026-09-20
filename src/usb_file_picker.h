/* SPDX-License-Identifier: MIT */
#ifndef MCI_USB_FILE_PICKER_H
#define MCI_USB_FILE_PICKER_H

#include <tamtypes.h>

#include "save_transfer.h"

#define MCI_USB_PICKER_PATH_MAX 256
#define MCI_USB_PICKER_NAME_MAX 128
#define MCI_USB_PICKER_MAX_ENTRIES 128

typedef enum MciUsbPickerFilter {
    MCI_USB_PICKER_IMAGE_PS2 = 0,
    MCI_USB_PICKER_IMAGE_VMC,
    MCI_USB_PICKER_IMAGE_ANY,
    MCI_USB_PICKER_SAVE_ANY
} MciUsbPickerFilter;

typedef struct MciUsbPickerEntry {
    char name[MCI_USB_PICKER_NAME_MAX];
    char path[MCI_USB_PICKER_PATH_MAX];
    u64 size;
    int is_directory;
    MciSaveTransferFormat format;
} MciUsbPickerEntry;

typedef struct MciUsbPickerList {
    char path[MCI_USB_PICKER_PATH_MAX];
    int root_index;
    int entry_count;
    int truncated;
    MciUsbPickerFilter filter;
    MciUsbPickerEntry entries[MCI_USB_PICKER_MAX_ENTRIES];
} MciUsbPickerList;

int MciUsbPickerOpenFirst(MciUsbPickerFilter filter, MciUsbPickerList *list);
int MciUsbPickerScan(const char *path, int root_index,
                     MciUsbPickerFilter filter, MciUsbPickerList *list);
int MciUsbPickerEnter(const MciUsbPickerList *list, int index,
                      MciUsbPickerList *next);
int MciUsbPickerParent(const MciUsbPickerList *list, MciUsbPickerList *parent);
int MciUsbPickerCycleRoot(const MciUsbPickerList *list, int direction,
                          MciUsbPickerList *next);
const char *MciUsbPickerRootName(int root_index);

#endif /* MCI_USB_FILE_PICKER_H */
