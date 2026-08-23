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

## Export verification

An export is not reported as successful after the last write alone. Drebin closes the image, reopens it, reads the entire file back, validates size and layout, validates every `.ps2` ECC record, calculates CRC32 over the complete logical page stream and compares that value with the CRC captured from the physical card while dumping it.

A small `<image>.mci.txt` sidecar records geometry, format, CRC32 and verification state without modifying the standard `.ps2` or `.vmc` file itself.

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
