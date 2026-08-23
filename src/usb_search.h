#ifndef MCI_USB_SEARCH_H
#define MCI_USB_SEARCH_H

#define MCI_USB_SEARCH_PATH_MAX 192
#define MCI_USB_SEARCH_MAX_DEPTH 8
#define MCI_USB_SEARCH_MAX_DIRS 384

typedef void (*MciUsbSearchProgress)(const char *path,
                                     unsigned int directories_scanned,
                                     void *userdata);

/* Recursively search mass:/, mass0:/ and mass1:/ for FMCB.XLF.
 * If require_system_parent is non-zero, only .../SYSTEM/FMCB.XLF matches.
 * The walk is bounded by depth and directory-count limits so malformed or very
 * large USB trees cannot trap the UI indefinitely. */
int MciUsbFindFmcbXlf(char *out_path, unsigned int out_size,
                      int require_system_parent,
                      MciUsbSearchProgress progress, void *userdata);

/* Convert .../SYSTEM/FMCB.XLF into the package root used by the manifest. */
int MciUsbPackageRootFromXlf(const char *xlf_path,
                             char *out_root, unsigned int out_size);

#endif /* MCI_USB_SEARCH_H */
