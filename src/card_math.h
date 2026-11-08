/* SPDX-License-Identifier: MIT */
#ifndef MCI_CARD_MATH_H
#define MCI_CARD_MATH_H

#include <tamtypes.h>

/*
 * Shared EE-side memory-card stream math.
 *
 * CRC32 semantics are identical to the original bit-at-a-time Drebin code.
 * ECC follows the PCSX2/mymc reference (7-bit second byte) since 0.4.2.
 * tools/verify_card_math.py and tests/test_card_math.c are the executable
 * reference gates for future table/MMI/assembly changes.
 */
u32 MciCardMathCrc32Update(u32 crc, const unsigned char *data,
                           unsigned int size);
u32 MciCardMathEcc128(const unsigned char data[128]);
void MciCardMathBuildEcc12(const unsigned char data[512],
                           unsigned char ecc[12]);
void MciCardMathBuildSpare(const unsigned char data[512],
                           unsigned char spare[16]);

/*
 * Compare twelve stored ECC bytes with the page data.
 *
 * CANONICAL        every chunk matches the PCSX2/mymc 7-bit form;
 * LEGACY_DREBIN04  at least one chunk carries the Drebin 0.4.0/0.4.1 bit-7
 *                  artefact in its second byte and is otherwise correct;
 * NONE             real mismatch.
 *
 * Producers always write canonical ECC. Consumers accept both so images made
 * by 0.4.x remain restorable and browsable.
 */
typedef enum MciCardMathEccMatch {
    MCI_ECC_MATCH_NONE = 0,
    MCI_ECC_MATCH_CANONICAL = 1,
    MCI_ECC_MATCH_LEGACY_DREBIN04 = 2
} MciCardMathEccMatch;

MciCardMathEccMatch MciCardMathSpareCompare(const unsigned char data[512],
                                            const unsigned char stored[12]);

#endif /* MCI_CARD_MATH_H */
