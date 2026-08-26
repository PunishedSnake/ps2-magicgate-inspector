/* SPDX-License-Identifier: MIT */
/*
 * Small R5900-native memory primitives for Drebin's aligned stream buffers.
 *
 * Keep this deliberately boring. The goal is not to replace libc or turn the
 * project into an assembly museum. The image caches already use 64-byte aligned
 * storage and move 512/528-byte records, so LQ/SQ can move two 16-byte qwords
 * per loop with an optional final qword.
 *
 * Every branch delay slot is explicit. In particular, the loop branch keeps a
 * NOP delay slot instead of scheduling a useful instruction into it, avoiding
 * the documented R5900 short-loop erratum pattern even if assembler workaround
 * flags change in a future toolchain.
 */

    .set push
    .set noreorder
    .text
    .align 4
    .globl MciR5900CopyQwords
    .type MciR5900CopyQwords, @function
    .ent MciR5900CopyQwords

MciR5900CopyQwords:
    /* a0=dst, a1=src, a2=number of 16-byte qwords */
    srl     $t2, $a2, 1
    andi    $a2, $a2, 1
    beqz    $t2, 2f
    nop

1:
    lq      $t0, 0($a1)
    lq      $t1, 16($a1)
    sq      $t0, 0($a0)
    sq      $t1, 16($a0)
    addiu   $a1, $a1, 32
    addiu   $a0, $a0, 32
    addiu   $t2, $t2, -1
    bnez    $t2, 1b
    nop

2:
    beqz    $a2, 3f
    nop
    lq      $t0, 0($a1)
    sq      $t0, 0($a0)

3:
    jr      $ra
    nop

    .end MciR5900CopyQwords
    .size MciR5900CopyQwords, .-MciR5900CopyQwords
    .set pop
