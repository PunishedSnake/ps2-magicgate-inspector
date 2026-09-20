# PS2 Memory Card Inspector 0.4.0 "Drebin"

Drebin is the first full write-capable release of PS2 Memory Card Inspector.

It promotes the hardware-validated MagicGate/CardAuth work into a transactional
cross-region FreeMcBoot installer and adds Card Tools, recovery, persistent
settings and the production P0 optimization pass.

## Release profile

0.4.0 ships as **one production build**:

```text
-O2 -G0
-mtune=r5900 on measured hot objects
P0 synchronous production batching
```

The release intentionally does **not** publish:

- USB speed-test variants;
- async/NOWAIT candidates;
- raw batch-size A/B builds;
- Performance Lab binaries;
- synthetic R5900 benchmark builds.

The optimizations remain. The benchmark zoo does not.

## Highlights

- Full cross-region FMCB installation from a user-supplied package.
- Preflight binding of every distinct selected KELF before the first card write.
- Persistent rollback journal with card identity checking.
- Full close/reopen/read-back verification for installation writes.
- Compatibility profiles for retail CEX, real DEX, MechaPwn DEX/CEX,
  unqualified DEX-like state and PSX/DESR classification.
- Real-hardware-qualified MechaPwn DEX exception for CEX-only ENDVDPL.
- Legacy recovery-journal v1 reader and safe v1 -> v2 conversion.
- Card image export, verify and exact restore.
- Filesystem-aware image browser and save-transfer workflows.
- Persistent versioned Settings config.
- Native/480p/576p/720p/1080i display modes.
- P0 production improvements retained without release benchmark variants.
- R5900 tuning retained on measured hot objects.
- Improved Drebin logging with mass-storage ownership guards and ring-integrity
  diagnostics.

## FMCB installer

The installer discovers a complete FMCB 1.966 package recursively from USB.

Before touching the card it:

1. identifies the ROM/MechaCon/MechaPwn profile;
2. selects the appropriate cross-region manifest;
3. checks card filesystem health and free space;
4. bind-probes every distinct selected KELF source on the actual target
   hardware.

Only after those gates pass does the transaction begin.

Each destination follows:

```text
capture/backup
  -> read source
  -> KELF bind when required
  -> write
  -> close
  -> reopen
  -> full read-back verification
  -> commit
```

Any failure keeps or executes recovery state rather than silently leaving a
partially installed card.

### Cross-region policy

Drebin prepares real regional copies for the I/A/E/C system folders rather than
using classic Multi-Install filesystem crosslinks.

Early-Japanese boot files and HDD-support resources are retained in the package
model for compatibility with old boot ROMs.

### MechaPwn / DEX ENDVDPL exception

Real-hardware qualification on a MechaPwn SCPH-50000 profile showed:

- FMCB.XLF binds successfully;
- OSDSYS.XLF binds successfully;
- OSD110.XLF binds successfully;
- the 128-byte CEX ENDVDPL.XRX is rejected at `DOWNLOAD HEADER` before
  Kbit/Kc/CardAuth.

Reference FMCB already omits ENDVDPL from its DEX table.

Drebin therefore omits this CEX-only payload for:

- a real DEX ROM profile;
- a positively fingerprinted MechaPwn DEX-mode profile.

A generic DEX-like state is not enough to trigger the exception automatically.

## MagicGate and card compatibility

Drebin keeps the hardware-proven logical/physical port rule:

```text
libmc mc0/mc1 = 0/1
SECR/SIO2 MC channels = 2/3
```

Translation occurs only at the SECR boundary.

The release does not reject cards by branding or nominal capacity. Real
hardware confirmed that a non-Sony card with functional MagicGate can complete
FMCB installation and boot successfully.

Cards without functional MagicGate can still be ordinary PS2 storage but fail
the CardAuth/KELF gate as expected.

## Recovery

The v2 recovery journal records:

- captured destinations;
- backups where existing files must be restored;
- created system/config directories;
- target card identity;
- transaction state.

Recovery verifies the same card before restoring or deleting anything.

The installer can also read old v1 journals and convert them in RAM after
validating their original checksum.

## Card Tools

0.4.0 includes:

- full-card image export;
- image verification;
- exact restore;
- card/image filesystem browser;
- selected save import/export;
- PSU transfer support;
- backup-aware force format;
- hot-swap-aware card workflows.

## Persistent Settings

`SQUARE` on the Settings page writes a versioned text config:

```text
mass:/MCI/MCINSPECTOR.CFG
```

Saved values:

- video mode;
- filesystem-test profile;
- preserve-existing-CNF policy;
- installer read-back verification mode.

Save is performed through a temporary file, full read-back comparison, rename
and device sync.

## P0 production pass

The public release retains the safe P0 production choices:

- synchronous raw-card streaming;
- fixed production batch sizes;
- reduced unnecessary copies where the 0.4 architecture already supports it;
- R5900-specific fast-copy path;
- `-mtune=r5900` on measured hot objects;
- `-O2` global optimization baseline.

Experimental USB throughput variants, async fileXio candidates and Performance
Lab executables remain development-only.

## Hardware qualification

Real-hardware testing established, among other cases:

- Sony 8 MiB cards with functional MagicGate;
- a non-Sony MagicGate-capable card with successful FMCB install and boot;
- a third-party non-MagicGate card correctly rejected by CardAuth while
  remaining usable for normal storage;
- successful rollback after an intentionally failing ENDVDPL transaction;
- repaired recovery readiness after security-personality IOP rebuilds;
- cross-file logger/recovery corruption fixes around fileXio/USB ownership.

## Settings and payloads are not bundled

The project does not redistribute FreeMcBoot or Sony ROM payloads.

Provide your own complete FMCB package. See `docs/FMCB_PACKAGE.md`.

## Build and provenance

Canonical release environment:

```text
ps2dev/ps2dev:v2.0.0
```

Pinned PS2SDK security-source revision:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

The release archive contains the ELF, SHA-256, source provenance, project
license, PS2SDK AFL-2.0 text, credits and third-party notices.

## Known scope

- PSX/DESR is classified by the compatibility layer but its separate X* manifest
  and twin-sign install path are not enabled in this PS2 release.
- ROM 2.30+ native OSD-update autoboot is classified as blocked; preparing a
  card for another console/bootstrap is a separate capability.
- Unknown modchips and odd MechaCon states are resolved conservatively by real
  selected-KELF preflight rather than model-name heuristics.
