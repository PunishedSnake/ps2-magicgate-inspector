/* SPDX-License-Identifier: MIT */
#ifndef MCI_LZARI_H
#define MCI_LZARI_H

/*
 * LZARI decoder (Haruhiko Okumura, 1989, public domain algorithm) as used by
 * Action Replay MAX / MAX Drive (.max, .pws) saves.
 *
 * Parameters are fixed by the format: 4096-byte window initialised to spaces,
 * match length 3..60, 15-bit arithmetic coder. Compatible with mymc's
 * lzari.py and Okumura's LZARI.C.
 *
 * The decoder never reads past `in_size` (missing bits read as zero, as in the
 * reference implementations) and never writes past `out_size`.
 * Returns 0 on success, negative on bad arguments.
 */

#include <tamtypes.h>

int MciLzariDecode(const unsigned char *in, u32 in_size,
                   unsigned char *out, u32 out_size);

#endif /* MCI_LZARI_H */
