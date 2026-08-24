/* SPDX-License-Identifier: MIT */
/*
 * Table-driven EE-side CRC32 and PS2 memory-card ECC.
 *
 * The old imaging code evaluates the CRC polynomial eight times for every byte
 * and reconstructs ECC parity/column contribution bit by bit for every source
 * byte. Those are useful reference implementations but poor hot loops once SIF
 * and USB transaction counts have been reduced.
 *
 * Two small tables are built once on first use from the same reference rules:
 *   CRC: 256 * u32 = 1024 bytes
 *   ECC: 256 * u16 =  512 bytes (low byte=column contribution, bit 8=parity)
 *
 * A generated-at-runtime table avoids storing a large opaque constant dump in
 * source while paying only a tiny one-time setup cost. The hot path then uses a
 * predictable table lookup and four-byte unrolling for CRC, and a packed lookup
 * with branchless line-parity masking for ECC.
 */

#include <tamtypes.h>
#include <string.h>

#include "card_math.h"

#define MCI_CRC32_POLY 0xEDB88320u

static u32 CrcTable[256] __attribute__((aligned(64)));
static u16 EccTable[256] __attribute__((aligned(64)));
static int TablesReady;

static unsigned char ReferenceParity8(unsigned char value)
{
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return value & 1u;
}

static unsigned char ReferenceColumnMask(unsigned char value)
{
    static const unsigned char BitMasks[8] = {
        0x07, 0x16, 0x25, 0x34, 0x43, 0x52, 0x61, 0x70
    };
    unsigned char mask = 0;
    unsigned int bit;

    for (bit = 0; bit < 8u; bit++) {
        if (value & (1u << bit))
            mask ^= BitMasks[bit];
    }
    return mask;
}

static void EnsureTables(void)
{
    unsigned int value;

    if (TablesReady)
        return;

    for (value = 0u; value < 256u; value++) {
        u32 crc = value;
        unsigned int bit;
        for (bit = 0u; bit < 8u; bit++)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & MCI_CRC32_POLY);
        CrcTable[value] = crc;
        EccTable[value] = (u16)ReferenceColumnMask((unsigned char)value) |
                          ((u16)ReferenceParity8((unsigned char)value) << 8);
    }

    TablesReady = 1;
}

static inline u32 CrcByte(u32 crc, unsigned char value)
{
    return CrcTable[(crc ^ value) & 0xFFu] ^ (crc >> 8);
}

u32 MciCardMathCrc32Update(u32 crc, const unsigned char *data,
                           unsigned int size)
{
    unsigned int offset = 0u;

    if (data == NULL && size != 0u)
        return crc;
    EnsureTables();

    crc = ~crc;

    /* Four-byte scalar unroll is intentional. It removes 75% of the loop
     * branches without dragging a multi-kilobyte slicing-by-N table through the
     * EE's small data cache. Later MMI/assembly variants can compete with this
     * implementation under the same equivalence and hardware benchmark gates. */
    while (size - offset >= 4u) {
        crc = CrcByte(crc, data[offset + 0u]);
        crc = CrcByte(crc, data[offset + 1u]);
        crc = CrcByte(crc, data[offset + 2u]);
        crc = CrcByte(crc, data[offset + 3u]);
        offset += 4u;
    }
    while (offset < size) {
        crc = CrcByte(crc, data[offset]);
        offset++;
    }
    return ~crc;
}

u32 MciCardMathEcc128(const unsigned char data[128])
{
    unsigned char column = 0x77;
    unsigned char line0 = 0x7F;
    unsigned char line1 = 0x7F;
    unsigned int i;

    if (data == NULL)
        return 0u;
    EnsureTables();

    for (i = 0u; i < 128u; i++) {
        u16 entry = EccTable[data[i]];
        unsigned char parity_mask =
            (unsigned char)(0u - (unsigned int)((entry >> 8) & 1u));

        column ^= (unsigned char)entry;
        line0 ^= (unsigned char)(((unsigned char)(~i)) & parity_mask);
        line1 ^= (unsigned char)(((unsigned char)i) & parity_mask);
    }

    return (u32)column | ((u32)line0 << 8) | ((u32)line1 << 16);
}

void MciCardMathBuildEcc12(const unsigned char data[512],
                           unsigned char ecc[12])
{
    unsigned int chunk;

    if (data == NULL || ecc == NULL)
        return;
    for (chunk = 0u; chunk < 4u; chunk++) {
        u32 value = MciCardMathEcc128(data + chunk * 128u);
        ecc[chunk * 3u + 0u] = (unsigned char)(value & 0xFFu);
        ecc[chunk * 3u + 1u] = (unsigned char)((value >> 8) & 0xFFu);
        ecc[chunk * 3u + 2u] = (unsigned char)((value >> 16) & 0xFFu);
    }
}

void MciCardMathBuildSpare(const unsigned char data[512],
                           unsigned char spare[16])
{
    if (data == NULL || spare == NULL)
        return;
    MciCardMathBuildEcc12(data, spare);
    memset(spare + 12, 0, 4u);
}
