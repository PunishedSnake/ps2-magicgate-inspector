# PS2 Memory Card Inspector

A standalone PlayStation 2 homebrew utility for inspecting memory-card health, MagicGate/KELF behavior and (in Briscoe) the readiness of user-supplied FreeMcBoot files.

> **Current development line:** v0.2.0 **Briscoe**, built with PS2DEV 2.0.0. The Columbo filesystem pipeline has already passed on real PS2 hardware; Briscoe is adding a coherent PS2SDK SECR stack and read-only FMCB package preflight before any installer write path is enabled.

## What it does today

`MC_INSPECTOR.ELF` supports both PS2 memory-card ports and can:

- query the card with `mcGetInfo()` and preserve the raw XMCMAN result;
- report the card type, format state and free-cluster count;
- verify access to the root directory;
- run a non-destructive 4 KiB write/read/compare/delete test;
- verify that its temporary test file was deleted afterwards;
- distinguish filesystem health from MagicGate/KELF capability;
- run a staged, RAM-only MagicGate/KELF bind probe and report the exact SECR stage;
- scan a user-supplied FreeMcBoot package from USB and report whether its baseline normal-install files are complete;
- detect the console region from `rom0:ROMVER` and resolve the normal FMCB target system folder;
- offer manual formatting **only** when a PS2 card is reported as explicitly unformatted/no-format.

Briscoe dev3 still does **not** install FreeMcBoot. The FMCB path is intentionally read-only until MagicGate, source, space, backup and rollback behavior have all been validated on hardware.

## Safety model

Inspection should be non-destructive. The R/W test creates a uniquely named temporary file (`/__MCIxx.TMP`), writes a deterministic 4096-byte pattern, reads it back, compares every byte, deletes the file and verifies that it is gone.

The MagicGate probe modifies only an in-memory copy of a KELF. Bound Kbit/Kc/ICVPS2 data is not written back to either card.

The FMCB preflight performs source-side metadata checks only. It does not create target directories or copy/bind/write any FMCB payload. FreeMcBoot payloads are not embedded in this repository or release artifacts.

Formatting is deliberately harder to trigger than an ordinary test. It is never automatic. The user must first request formatting with **Triangle**, then confirm by holding **L1 + R1** and pressing **Triangle** again. Authentication, detection and generic I/O failures never unlock the formatter.

## Runtime stack

Briscoe uses one coherent PS2SDK 2.0 stack rather than mixing ROM and homebrew memory-card/security modules.

A tiny IOPRP is generated in EE RAM with PS2SDK `ioprpgen` and contains the special open-source `SECRMAN`. After the IOP reboot, the ELF loads PS2SDK:

- `freesio2.irx` / SIO2MAN;
- `freepad.irx` / PADMAN;
- `mcman.irx` built as XMCMAN;
- `mcserv.irx` built as XMCSERV;
- `secrsif.irx`.

This ordering lets XMCMAN register its MagicGate callbacks with the resident SECRMAN before the EE-side staged SECR RPC probe is used.

For optional FMCB package discovery, dev3 also embeds PS2SDK `iomanX`, `fileXio`, `USBD` and `USBHDFSD`. Failure of this optional USB stack does not disable ordinary card inspection or MagicGate diagnostics.

## Controls

| Control | Action |
| --- | --- |
| Left / Right | Select `mc0:` or `mc1:` |
| Cross | Inspect selected card/filesystem |
| Start | Inspect both cards/filesystems |
| Square | Run RAM-only staged MagicGate/KELF probe |
| Circle | Scan `mass:/FMCB`, `mass0:/FMCB`, `mass1:/FMCB` |
| R1 | Cycle Card / MagicGate / FMCB Preflight views |
| Triangle | Enter format confirmation when formatting is allowed |
| L1 + R1 + Triangle | Confirm destructive format |
| Circle during format confirmation | Cancel format confirmation |
| Select | Exit |

## User-supplied FMCB package

See [FMCB package layout](docs/FMCB_PACKAGE.md). Briscoe expects a baseline normal-install tree below an `FMCB` folder on USB. Missing optional USB drivers do not make the package incomplete; missing required system/config/resource files do.

The dev3 UI explicitly reports:

```text
INSTALL: DISABLED IN DEV3 (preflight is read-only)
```

## Build

The project targets **PS2DEV 2.0.0**. CI builds in `ps2dev/ps2dev:v2.0.0`; the current toolchain reports GCC 15.2.0.

```sh
make
```

The GitHub Actions workflow verifies the output with `file(1)`, calculates SHA-256 and publishes the ELF as an Actions artifact.

## Project documentation

- [Architecture](docs/ARCHITECTURE.md) — runtime structure, card classification and safety decisions.
- [Testing](docs/TESTING.md) — hardware test matrix and what counts as a valid result.
- [FMCB package](docs/FMCB_PACKAGE.md) — user-supplied package contract and dev3 safety boundary.
- [Roadmap](docs/ROADMAP.md) — planned milestones and feature boundaries.
- [Release codenames](docs/CODENAMES.md) — the detective/police naming scheme.
- [Changelog](CHANGELOG.md) — user-visible changes by version.

## Repository policy

`main` remains the stable standalone line while Briscoe is developed and hardware-tested in a draft PR. The earlier FreeMcBoot patch/forced-install prototype remains available through Git history but is intentionally not part of the active implementation.

## v0.1.0 — Columbo

The first standalone release is named **Columbo**: not flashy, slightly suspicious of everything, and mostly interested in asking the memory card *just one more question*.
