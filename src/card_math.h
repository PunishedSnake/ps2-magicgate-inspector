/* SPDX-License-Identifier: MIT */
#ifndef MCI_CARD_MATH_H
#define MCI_CARD_MATH_H

#include <tamtypes.h>

/*
 * Shared EE-side memory-card stream math.
 *
 * Semantics are intentionally identical to the original bit-at-a-time Drebin
 * implementations. tools/verify_card_math.py is the executable reference gate
 * for future table/MMI/assembly changes.
 */
u32 MciCardMathCrc32Update(u32 crc, const unsigned char *data,
                           unsigned int size);
u32 MciCardMathEcc128(const unsigned char data[128]);
void MciCardMathBuildEcc12(const unsigned char data[512],
                           unsigned char ecc[12]);
void MciCardMathBuildSpare(const unsigned char data[512],
                           unsigned char spare[16]);

#endif /* MCI_CARD_MATH_H */
