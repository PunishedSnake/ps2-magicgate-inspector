#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

OUT="${1:-artifacts/R5900_STATIC_REPORT.txt}"
CC=mips64r5900el-ps2-elf-gcc
SIZE=mips64r5900el-ps2-elf-size
NM=mips64r5900el-ps2-elf-nm
OBJDUMP=mips64r5900el-ps2-elf-objdump
PS2SDK_ROOT="${PS2SDK:-/usr/local/ps2dev/ps2sdk}"

mkdir -p "$(dirname "$OUT")"

COMMON_FLAGS="-D_EE -G0 -Wall -Wextra -std=gnu99 -fdata-sections -ffunction-sections -mtune=r5900 -I${PS2SDK_ROOT}/ee/include -I${PS2SDK_ROOT}/common/include -I."

$CC $COMMON_FLAGS -O2 -c src/card_math.c -o /tmp/mci-card-math-O2.o
$CC $COMMON_FLAGS -O3 -c src/card_math.c -o /tmp/mci-card-math-O3.o
$CC $COMMON_FLAGS -Os -c src/card_math.c -o /tmp/mci-card-math-Os.o

{
    echo "Drebin R5900 static optimization report"
    echo "========================================"
    echo
    echo "Compiler:"
    $CC --version | head -n 1
    echo "Target: $($CC -dumpmachine)"
    echo
    echo "Target options reported by GCC:"
    $CC -Q --help=target -c -x c /dev/null -o /tmp/mci-target-probe.o 2>/dev/null | \
        grep -E 'march=|mtune=|mfix-r5900' || true
    echo
    echo "Selected optimizer switches (-O2):"
    $CC -Q -O2 --help=optimizers -c -x c /dev/null -o /tmp/mci-opt-o2.o 2>/dev/null | \
        grep -E 'schedule|inline|unroll|vectorize|prefetch|gcse|tree-loop|ipa-' || true
    echo
    echo "Selected optimizer switches (-O3):"
    $CC -Q -O3 --help=optimizers -c -x c /dev/null -o /tmp/mci-opt-o3.o 2>/dev/null | \
        grep -E 'schedule|inline|unroll|vectorize|prefetch|gcse|tree-loop|ipa-' || true
    echo
    echo "Selected optimizer switches (-Os):"
    $CC -Q -Os --help=optimizers -c -x c /dev/null -o /tmp/mci-opt-os.o 2>/dev/null | \
        grep -E 'schedule|inline|unroll|vectorize|prefetch|gcse|tree-loop|ipa-' || true
    echo
    echo "Final ELF section sizes:"
    $SIZE MC_INSPECTOR.ELF
    echo
    echo "Largest final ELF symbols (top 40):"
    $NM -S --size-sort MC_INSPECTOR.ELF | tail -n 40
    echo
    echo "Production hot object sizes:"
    $SIZE src/raw_bulk_read.o src/r5900_memops.o src/r5900_perf.o src/r5900_bench.o \
          src/card_math.o src/image_read_ahead.o src/image_write_behind.o \
          src/image_quick_verify.o src/card_image.o
    echo
    echo "card_math compiler A/B section sizes:"
    $SIZE /tmp/mci-card-math-O2.o /tmp/mci-card-math-O3.o /tmp/mci-card-math-Os.o
    echo
    echo "card_math O2 symbols:"
    $NM -S --size-sort /tmp/mci-card-math-O2.o
    echo
    echo "card_math O3 symbols:"
    $NM -S --size-sort /tmp/mci-card-math-O3.o
    echo
    echo "card_math Os symbols:"
    $NM -S --size-sort /tmp/mci-card-math-Os.o
    echo
    echo "Production r5900_memops disassembly:"
    $OBJDUMP -dr src/r5900_memops.o
    echo
    echo "Production card_math disassembly:"
    $OBJDUMP -dr src/card_math.o
    echo
    echo "card_math -O3 candidate disassembly:"
    $OBJDUMP -dr /tmp/mci-card-math-O3.o
    echo
    echo "card_math -Os candidate disassembly:"
    $OBJDUMP -dr /tmp/mci-card-math-Os.o
} > "$OUT"

printf 'R5900 static report written to %s\n' "$OUT"
