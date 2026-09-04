# P0 FINAL-SAFE v3 manifest

This document pins the sixth synchronous P0 composition used for real-PS2 qualification.

## Immutable inputs

- base runtime: `c88f50787b15bf253e11e5bc75017ac1ed4cdd79`
- PS2SDK source/security personality: `a13b5971ec0e39c7ba8b8559b80a4e81c8425352`
- image discovery batch transformer: `f82c536de1c872214699856df43652d509807295`
- image cluster batch transformer: `ec5416b29ff18f578a87c4a2ae123292635f8942`
- image FAT metadata cache transformer: `613e73e411bba75e6670556b3114c4c48c0e4174`
- target conflict batch transformer: `595594ed966981af5f43603ae8036269cf1e6e45`
- trusted quick-verify read-ahead elision transformer: `4053b7a2762ab229bf705a0bd70a28404d8ec54c`
- trusted quick-open ownership fusion transformer: `59500a2aa9866f1bbac3662236deeab67ee6f36a`

## Composition order

1. batched image discovery
2. two-page filesystem cluster batching
3. immutable indirect-FAT/FAT metadata cache
4. destination conflict batching, built as 16/32/64 candidates
5. trusted quick-reopen read-ahead elision
6. trusted quick-open ownership fusion

The order is intentional and is verified by fail-closed source anchors in CI.

## What v3 changes relative to v2

The selected-save import path previously performed a trusted quick reopen, validated page zero, closed that descriptor, then immediately reopened the same image and read page zero again before streaming the selected save.

v3 transfers ownership of the already verified descriptor and page-zero data directly into `OpenImage()`. The following contracts remain intact:

- preverify `fileXioSync` remains at the diagnostic/service boundary;
- image size and known-format checks remain;
- page-zero geometry/superblock validation remains;
- full scan/full verification behavior remains unchanged;
- logger mass-write pause remains depth-counted;
- helper errors close any descriptor they opened;
- successful ownership passes to `MciFsImage`, which closes it on normal completion or rollback.

This is confirmed source/build work removal, not a claimed real-hardware speedup.

## Fixed synchronous transport for primary v3 qualification

- `R5900_BENCH=0`
- `RAW_BULK_PAGES=16`
- `RAW_BULK_ASYNC=0`
- `IMAGE_READ_PAGES=32`
- `IMAGE_READ_ASYNC=0`
- `IMAGE_WRITE_PAGES=32`
- `IMAGE_WRITE_ASYNC=0`
- `IMAGE_WRITE_PROBE=0`

These remain fixed so FINAL-SAFE v3 changes only the synchronous software/data-path composition. Async transport remains a separate test axis.

## Hardware gate

For each surviving conflict batch candidate record at least:

- SCPH / hardware revision
- memory card and USB device
- PS2SDK/toolchain and active IRX hashes
- exact ELF SHA-256
- workload and image format/size
- p50 / p95 / p99 / max
- correctness hash
- verify/restore result
- fileXio / image-I/O errors

A candidate is not promoted on CI success alone.
