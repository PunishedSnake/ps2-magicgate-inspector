# PS2 MagicGate Card Inspector & Forced FMCB Installer

Private CI/build repository for the experimental MagicGate Card Inspector and gated FMCB force-install path.

## What this repository contains

GitHub Actions clones the pinned upstream `israpps/FreeMcBoot-Installer` commit `ac53a47a5c6eae675cc2611c7bebe62f56c7845c`, applies the MGCI patch plus the non-interactive emulator harness, and builds with `ps2dev/ps2dev:v1.0`.

The resulting artifact contains:

- `standard/FMCBInstaller-MGCI.elf`
- `standard/UNC_FMCBInstaller-MGCI.elf`
- `exfat/FMCBInstaller-MGCI-EXFAT.elf`
- `exfat/UNC_FMCBInstaller-MGCI-EXFAT.elf`
- `SHA256SUMS.txt`

## Safety model

The force-install path is not a blind `MC_TYPE_PS2` bypass. On real hardware a card must pass filesystem read/write integrity, verified temporary-file cleanup, KELF loading, and the real `SecrDownloadFile()` MagicGate binding path before `full_pass` can become true.

The emulator qualification path is deliberately separate. It can validate card detection, expected rejection of an unformatted card, `mcFormat()`, read/write integrity, cleanup, KELF loading and KELF structure parsing, but it always forces `full_pass = 0`. Upstream PCSX2 therefore cannot unlock the production force-install path.

### Non-interactive emulator modes

The patched installer recognizes:

- `--mgci-pcsx2-test --mgci-mode=fresh-format --mgci-port=0`
- `--mgci-pcsx2-test --mgci-mode=formatted --mgci-port=0`
- `--mgci-pcsx2-test --mgci-mode=unformatted-reject --mgci-port=0`
- use `--mgci-port=1` for the second memory-card port

The harness writes `host:mgci-result.json` for automated runners.

## Emulator targets

For normal regression testing, use the current **PCSX2 nightly** rather than the stable channel so the test suite tracks current emulator behavior and CLI support.

A second experimental backend is planned for the archived `987123879113/pcsx2` P2IO/Python 2 fork. That fork contains MagicGate-related emulation and requires user-supplied MagicGate key material. Its results must remain distinguishable from both upstream-PCXS2 qualification and real-hardware qualification.

No BIOS dumps or MagicGate key files belong in this repository.

## Build

Push to `main` or run **Actions -> Build PS2 MagicGate Card Inspector -> Run workflow**.
