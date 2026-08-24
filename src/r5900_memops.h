/* SPDX-License-Identifier: MIT */
#ifndef MCI_R5900_MEMOPS_H
#define MCI_R5900_MEMOPS_H

#include <tamtypes.h>
#include <string.h>

/*
 * Native R5900 128-bit copy primitive.
 *
 * MciR5900CopyQwords() is implemented in r5900_memops.S with LQ/SQ and expects
 * both pointers to be 16-byte aligned. qwords is a count of 16-byte units.
 * Callers should normally use MciFastCopy(), which preserves ordinary memcpy
 * semantics by falling back whenever alignment/size do not satisfy that
 * contract.
 *
 * This is intentionally a narrow primitive rather than a replacement for the
 * C library memcpy. Drebin uses it only on hot image-stream buffers whose
 * alignment is under our control. That keeps the assembler path measurable,
 * reversible and easy to audit while we qualify it on real R5900 hardware.
 */
void MciR5900CopyQwords(void *dst, const void *src, unsigned int qwords);

static inline void MciFastCopy(void *dst, const void *src, unsigned int size)
{
    if (size != 0u &&
        ((((u32)dst | (u32)src | (u32)size) & 0x0Fu) == 0u)) {
        MciR5900CopyQwords(dst, src, size >> 4);
        return;
    }
    memcpy(dst, src, size);
}

#endif /* MCI_R5900_MEMOPS_H */
