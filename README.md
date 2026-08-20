# PS2 MagicGate Card Inspector & Forced FMCB Installer

Private CI/build repository for the experimental MagicGate Card Inspector and gated FMCB force-install path.

## What this repository contains

This repository intentionally stores only the MGCI patch and build glue. GitHub Actions clones the pinned upstream `israpps/FreeMcBoot-Installer` commit `ac53a47a5c6eae675cc2611c7bebe62f56c7845c`, applies the patch, and builds with the same `ps2dev/ps2dev:v1.0` container used by upstream.

The resulting artifact contains:

- `standard/FMCBInstaller-MGCI.elf`
- `standard/UNC_FMCBInstaller-MGCI.elf`
- `exfat/FMCBInstaller-MGCI-EXFAT.elf`
- `exfat/UNC_FMCBInstaller-MGCI-EXFAT.elf`
- `SHA256SUMS.txt`

## Safety model

The force-install path is not a blind `MC_TYPE_PS2` bypass. A card must first pass the Inspector's read/write integrity test and a real MagicGate KELF binding operation. The normal FMCB installation path then signs the real KELF again; MagicGate failures are still fatal.

## Build

Push to `main` or run **Actions -> Build PS2 MagicGate Card Inspector -> Run workflow**.
