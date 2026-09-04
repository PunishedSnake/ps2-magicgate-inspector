# P0 FINAL-SAFE v2 composition manifest

This manifest freezes the next synchronous P0 composition. It does not replace
the previous FINAL-SAFE artifact; it adds one newly qualified remove-work
transform so real hardware can measure the delta explicitly.

## Composition base

- `c88f50787b15bf253e11e5bc75017ac1ed4cdd79`

The base already contains the earlier export copy-elision and direct producer ->
write-slot ownership work.

## Pinned transformers

1. image-name discovery batch
   - branch: `perf/p0-image-discovery-batch`
   - SHA: `f82c536de1c872214699856df43652d509807295`
2. image-FS two-page cluster batch
   - branch: `perf/p0-image-fs-cluster-batch`
   - SHA: `ec5416b29ff18f578a87c4a2ae123292635f8942`
3. immutable FAT metadata cache
   - branch: `perf/p0-image-fs-fat-cache`
   - SHA: `613e73e411bba75e6670556b3114c4c48c0e4174`
4. target conflict-directory batch
   - branch: `perf/p0-image-fs-conflict-batch`
   - SHA: `595594ed966981af5f43603ae8036269cf1e6e45`
5. trusted quick-reopen read-ahead elision
   - branch: `perf/p0-trusted-quick-verify-no-readahead`
   - SHA: `4053b7a2762ab229bf705a0bd70a28404d8ec54c`

## Why transformer 5 is FINAL-SAFE material

**POTWIERDZONE from current project source:** selected-save import performs a
trusted quick reopen after a prior full image-filesystem scan. The quick reopen
reads only the first 512-byte page. With `IMAGE_READ_PAGES=32`, the synchronous
read-ahead backing request is `528 * 32 = 16896` bytes. The transformer simply
does not create a read-ahead scope for the trusted one-record verifier, while
full `MciCardImageVerifyFile` keeps the existing read-ahead scope.

Requested I/O removed from that first trusted reopen read:

- baseline: 16896 bytes
- candidate: 512 bytes
- eliminated request bytes: 16384

This is a source-level removal-of-work fact, not a real-hardware speed claim.

## Conflict batch variants

The v2 pack still builds `16`, `32`, and `64` conflict-directory variants.
Real PS2 timing selects the winner; the largest table is not assumed faster.

## Fixed controls

- `R5900_BENCH=0`
- `RAW_BULK_PAGES=16`
- `RAW_BULK_ASYNC=0`
- `IMAGE_READ_PAGES=32`
- `IMAGE_READ_ASYNC=0`
- `IMAGE_WRITE_PAGES=32`
- `IMAGE_WRITE_ASYNC=0`
- `IMAGE_WRITE_PROBE=0`
- toolchain container: `ps2dev/ps2dev:v2.0.0`
- pinned PS2SDK security source: `a13b5971ec0e39c7ba8b8559b80a4e81c8425352`

## Promotion rule

CI qualifies composition, correctness harnesses and reproducible builds. It does
not promote a performance winner. Promotion still requires correctness-clean
real-PS2 A/B with p50/p95/p99/max and whole-operation timing.
