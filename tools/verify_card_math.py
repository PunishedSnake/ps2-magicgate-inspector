#!/usr/bin/env python3
"""Host-side equivalence harness for Drebin's planned EE CRC/ECC fast path.

The production image engine currently contains a deliberately simple reference
implementation. Before replacing its bit-at-a-time work with lookup tables or
R5900/MMI code, keep an executable definition of the required semantics here.

This test proves:
- table-driven CRC32 equals the existing Drebin bitwise update semantics;
- CRC32 of the canonical 123456789 vector is CB F4 39 26;
- every possible byte gets the same ECC column/parity lookup as the reference;
- branchless lookup ECC equals the current 128-byte algorithm on structured and
  deterministic pseudo-random pages;
- incremental CRC calls produce the same result as a single full-buffer call.
"""

from __future__ import annotations

import random

POLY = 0xEDB88320
BIT_MASKS = (0x07, 0x16, 0x25, 0x34, 0x43, 0x52, 0x61, 0x70)


def crc32_reference(crc: int, data: bytes) -> int:
    crc = (~crc) & 0xFFFFFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc >> 1) ^ ((0 - (crc & 1)) & POLY)) & 0xFFFFFFFF
    return (~crc) & 0xFFFFFFFF


def make_crc_table() -> tuple[int, ...]:
    table = []
    for value in range(256):
        crc = value
        for _ in range(8):
            crc = (crc >> 1) ^ (POLY if (crc & 1) else 0)
        table.append(crc & 0xFFFFFFFF)
    return tuple(table)


CRC_TABLE = make_crc_table()


def crc32_table(crc: int, data: bytes) -> int:
    crc = (~crc) & 0xFFFFFFFF
    for value in data:
        crc = CRC_TABLE[(crc ^ value) & 0xFF] ^ (crc >> 8)
    return (~crc) & 0xFFFFFFFF


def parity8_reference(value: int) -> int:
    value ^= value >> 4
    value ^= value >> 2
    value ^= value >> 1
    return value & 1


def column_mask_reference(value: int) -> int:
    mask = 0
    for bit, contribution in enumerate(BIT_MASKS):
        if value & (1 << bit):
            mask ^= contribution
    return mask


def make_ecc_table() -> tuple[int, ...]:
    return tuple(
        column_mask_reference(value) | (parity8_reference(value) << 8)
        for value in range(256)
    )


ECC_TABLE = make_ecc_table()


def ecc128_reference(data: bytes) -> int:
    assert len(data) == 128
    column = 0x77
    line0 = 0x7F
    line1 = 0x7F
    for index, value in enumerate(data):
        column ^= column_mask_reference(value)
        if parity8_reference(value):
            line0 ^= (~index) & 0xFF
            line1 ^= index
    return column | (line0 << 8) | (line1 << 16)


def ecc128_table_branchless(data: bytes) -> int:
    assert len(data) == 128
    column = 0x77
    line0 = 0x7F
    line1 = 0x7F
    for index, value in enumerate(data):
        entry = ECC_TABLE[value]
        parity_mask = 0xFF if (entry & 0x100) else 0
        column ^= entry & 0xFF
        line0 ^= ((~index) & 0xFF) & parity_mask
        line1 ^= index & parity_mask
    return column | (line0 << 8) | (line1 << 16)


def deterministic_pages() -> list[bytes]:
    pages = [
        bytes(128),
        bytes([0xFF]) * 128,
        bytes(range(128)),
        bytes((index * 37 + 11) & 0xFF for index in range(128)),
        bytes((index ^ (index << 3)) & 0xFF for index in range(128)),
    ]
    rng = random.Random(0xD2EB1A)
    for _ in range(4096):
        pages.append(bytes(rng.getrandbits(8) for _ in range(128)))
    return pages


def main() -> None:
    for value in range(256):
        entry = ECC_TABLE[value]
        assert (entry & 0xFF) == column_mask_reference(value)
        assert ((entry >> 8) & 1) == parity8_reference(value)

    assert crc32_reference(0, b"123456789") == 0xCBF43926
    assert crc32_table(0, b"123456789") == 0xCBF43926

    rng = random.Random(0x5900)
    for size in (0, 1, 2, 15, 16, 31, 32, 127, 128, 511, 512, 8192):
        for _ in range(64):
            data = bytes(rng.getrandbits(8) for _ in range(size))
            reference = crc32_reference(0, data)
            assert crc32_table(0, data) == reference

            split = size // 2
            incremental = crc32_table(0, data[:split])
            incremental = crc32_table(incremental, data[split:])
            assert incremental == reference

    for page in deterministic_pages():
        assert ecc128_table_branchless(page) == ecc128_reference(page)

    print(
        "card math equivalence PASS: "
        "CRC table=256x32-bit, ECC table=256x9-bit semantics, "
        "4096 deterministic pseudo-random ECC pages"
    )


if __name__ == "__main__":
    main()
