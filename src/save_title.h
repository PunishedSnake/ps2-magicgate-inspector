/* SPDX-License-Identifier: MIT */
#ifndef MCI_SAVE_TITLE_H
#define MCI_SAVE_TITLE_H

#define MCI_SAVE_TITLE_MAX 96

/* Decode the human-readable title stored in a PS2 icon.sys (mcIcon) record.
 * The current renderer/font path is ASCII-first: valid ASCII Shift-JIS code
 * points are preserved, line breaks are flattened to spaces, and titles that
 * contain no useful ASCII text fail so callers can retain the raw directory
 * name rather than display question-mark soup. */
int MciSaveTitleDecodeIconSys(const void *data, unsigned int size,
                              char *out, unsigned int out_size);

#endif /* MCI_SAVE_TITLE_H */
