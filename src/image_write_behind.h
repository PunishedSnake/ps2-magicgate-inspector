#ifndef MCI_IMAGE_WRITE_BEHIND_H
#define MCI_IMAGE_WRITE_BEHIND_H

/*
 * Enable/disable the image-only fileXioWrite batching wrapper.
 *
 * While enabled, 512-byte VMC pages and 528-byte PCSX2 raw records are
 * collected in groups of 32 before reaching USBHDFSD. All supported card page
 * counts are multiples of 32, so a successful full-image export drains the
 * cache before the image descriptor is closed.
 */
void MciImageWriteBehindSetEnabled(int enabled);

#endif /* MCI_IMAGE_WRITE_BEHIND_H */
