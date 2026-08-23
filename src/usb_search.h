#ifndef MCI_USB_SEARCH_H
#define MCI_USB_SEARCH_H

#define MCI_USB_SEARCH_PATH_MAX 192
#define MCI_USB_SEARCH_MAX_DEPTH 8
#define MCI_USB_SEARCH_MAX_DIRS 384

typedef void (*MciUsbSearchProgress)(const char *path,
                                     unsigned int directories_scanned,
                                     void *userdata);

/* Wait until at least one mass: root can actually be opened. This is used
 * after an IOP rebuild, when USBHDFSD may be resident slightly before the FAT
 * volume is ready for directory walking. */
int MciUsbWaitForStorage(unsigned int attempts, unsigned int delay_usec);

/* Recursively search mass:/, mass0:/ and mass1:/ for FMCB.XLF.
 * If require_system_parent is non-zero, only .../SYSTEM/FMCB.XLF matches.
 * The walk is bounded by depth and directory-count limits. Dot-prefixed
 * directories are ignored because they are not valid FMCB package locations
 * and some USB/FAT combinations expose transient pseudo entries there. */
int MciUsbFindFmcbXlf(char *out_path, unsigned int out_size,
                      int require_system_parent,
                      MciUsbSearchProgress progress, void *userdata);

/* Convert .../SYSTEM/FMCB.XLF into the package root used by the manifest. */
int MciUsbPackageRootFromXlf(const char *xlf_path,
                             char *out_root, unsigned int out_size);

/* In-process cache for a package root that has already passed the complete
 * FMCB manifest preflight. The cache is never populated from a lone XLF. A
 * read verifies that SYSTEM/FMCB.XLF still exists; normal preflight then
 * rechecks the entire manifest without repeating the recursive tree walk. */
int MciUsbGetVerifiedPackageRoot(char *out_root, unsigned int out_size);
int MciUsbRememberVerifiedPackageRoot(const char *root);
void MciUsbClearVerifiedPackageRoot(void);

#endif /* MCI_USB_SEARCH_H */
