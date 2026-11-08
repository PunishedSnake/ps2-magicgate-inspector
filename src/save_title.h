/* SPDX-License-Identifier: MIT */
#ifndef MCI_SAVE_TITLE_H
#define MCI_SAVE_TITLE_H

#define MCI_SAVE_TITLE_MAX 96

/* Decode the human-readable title stored in a PS2 icon.sys (mcIcon) record.
 * The title is a 68-byte Shift-JIS byte string. The renderer is ASCII-only, so
 * single-byte ASCII is kept, full-width roman letters/digits/punctuation are
 * folded to ASCII, the nlOffset line break becomes a space, and titles with
 * fewer than three printable characters fail so callers keep the raw
 * directory name rather than display question-mark soup. */
int MciSaveTitleDecodeIconSys(const void *data, unsigned int size,
                              char *out, unsigned int out_size);

#endif /* MCI_SAVE_TITLE_H */
