# Drebin Card Tools

0.4.0 "Drebin" moves destructive and image-management operations out of the ordinary diagnostic path. `Triangle` opens Card Tools for the selected slot; a normal filesystem test never formats or raw-writes a card.

## Port-numbering contract

Card Tools uses **logical libmc/MCMAN ports only**:

```text
mc0 = 0
mc1 = 1
```

Do not convert Card Tools page reads/writes/erases to physical SIO2 channels `2/3`. The MCMAN used by Drebin owns that conversion internally when it builds the actual SIO2 transfer. Pre-shifting the port changes which MCMAN device-state entry is consulted and previously broke raw imaging on real hardware.

The superficially similar `+2` used by MagicGate/FMCB security binding is correct for a different reason: SECRMAN/CardAuth directly consumes physical SIO2 memory-card channels. That conversion belongs only at the SECR boundary.

The complete rule, failure history and code-review checklist are documented in [Memory-card port domains](MEMORY_CARD_PORT_DOMAINS.md). Shared source-level vocabulary and conversion helpers live in `src/mc_port.h`.

## Image formats

Drebin supports two interoperable raw image layouts.

### PCSX2 `.ps2`

Each PS2 NAND page is stored as 528 bytes:

```text
512 bytes logical page data
12 bytes ECC (four 3-byte ECC values, one per 128 data bytes)
4 bytes spare/reserved
```

libmc exposes corrected 512-byte page data rather than arbitrary physical spare bytes, so Drebin regenerates the standard ECC records and writes zero to the remaining four spare bytes. The resulting image follows the ordinary PCSX2 raw-card layout while remaining independent of undocumented physical spare contents.

### OPL-style `.vmc`

Each page is stored as 512 logical data bytes with no ECC/spare area. This is the compact logical-page layout commonly used for PS2 VMC files.

### Filesystem geometry is not fixed to official 8 MiB cards

The filesystem browser must not assume that every valid PS2 memory-card superblock uses the erase geometry commonly seen on an official 8 MiB card. Real-hardware qualification of a functional 64 MiB card produced this valid superblock geometry:

```text
page_len          = 512
pages_per_cluster = 2
pages_per_block   = 32
clusters_per_card = 65536
alloc_offset      = 266
alloc_end         = 65238
rootdir_cluster   = 0
backup_block1     = 4095
backup_block2     = 4094
```

An early Drebin image-browser validator incorrectly rejected `pages_per_block > 16`, causing both otherwise valid `.vmc` and `.ps2` images of this card to fail with `IMAGE INVALID (rc=-3)`. That restriction was invalid: `pages_per_block` describes physical erase geometry and the filesystem browser does not erase the source image. Browser validation therefore requires a non-zero value but does not impose the official-card `16` value as a universal upper limit.

Keep physical erase-geometry policy in the raw exact-restore/format layer. Do not move it into the filesystem parser.

## Export verification

An export is not reported as successful after the last write alone. Drebin closes the image, reopens it, reads the entire file back, validates size and layout, validates every `.ps2` ECC record, calculates CRC32 over the complete logical page stream and compares that value with the CRC captured from the physical card while dumping it.

A small `<image>.mci.txt` sidecar records geometry, format, CRC32 and verification state without modifying the standard `.ps2` or `.vmc` file itself.

## Persistent logger isolation during image I/O

Real-hardware testing on 2026-08-24 established that USBHDFSD/fileXio must not be treated as safely supporting Drebin's previous logging pattern while a card-image file descriptor remains open.

The diagnostic logger previously opened `DREBIN.LOG`, appended one progress line, closed it and synced `mass:` while the image exporter or verifier simultaneously kept another file on the same USB filesystem open. Two independent 64 MiB dumps proved that those logger writes could land inside the image stream itself.

Observed corruption included:

- literal progress/logger text appearing inside logical memory-card pages;
- a damaged VMC superblock magic string;
- corrupted `.ps2` spare/ECC bytes;
- image CRC mismatches despite the raw card-read CRC remaining stable;
- image-browser rejection caused by corruption introduced by the diagnostic path, not by the card filesystem.

The two independently captured logical streams differed on only five pages. Selecting the uncorrupted copy of each differing page reconstructed the exact raw-card CRC reported during acquisition, proving that the card reads were sound and the corruption occurred in USB image-file handling while persistent logging was active.

Drebin therefore treats a card-image stream as an exclusive `mass:` critical section:

```text
log operation start durably
  -> pause DREBIN.LOG USB writes
  -> keep trace lines in EE RAM
  -> export / verify / browse / selective import / exact restore
  -> close every image descriptor
  -> resume logger USB writes
  -> flush queued trace
  -> log operation result durably
```

Do not reintroduce per-progress durable logging while an image file is open. If persistent mid-stream crash checkpoints become necessary, the image itself must first be closed/synced before the logger touches `mass:` and then reopened explicitly; concurrent long-lived image I/O plus logger append traffic is not an accepted design.

## Exact restore

Exact restore is intentionally strict:

1. verify the source image in full;
2. probe destination card geometry through raw page RPCs;
3. reject the operation if image and destination page counts differ;
4. erase the destination block by block;
5. program each 512-byte logical page;
6. read the complete card back;
7. compare the restored card CRC32 with the source image CRC32.

This operation is for same-geometry restore. It is **not** smaller-to-larger migration.

## Force format

Force format is backup-first. Drebin must create and verify a `.ps2` recovery image on USB before `mcFormat()` is issued. If image creation or read-back verification fails, format remains blocked.

This gives a deterministic recovery path for formats performed by Drebin. Recovery of a card that was formatted earlier without a backup is a separate forensic problem and cannot be promised losslessly.

## Smaller-to-larger migration

An 8 MiB raw image must not simply be written to the first 8 MiB of a 64 MiB card: the source superblock contains the smaller geometry and would effectively transplant that filesystem geometry to the larger device.

Migration therefore remains a filesystem-aware operation:

```text
verify source image
  -> inspect destination capacity
  -> format destination natively
  -> import directory tree/files/metadata
  -> rebind destination-specific FMCB KELFs when required
  -> verify imported contents
```

This path will reuse the same raw image reader and hardware-validated MagicGate backend, but remains deliberately separate from exact restore.
