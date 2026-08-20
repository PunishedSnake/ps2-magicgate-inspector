# PS2 Memory Card Inspector

A standalone PlayStation 2 homebrew utility for inspecting memory-card health without depending on FreeMcBoot or another installer.

> **Current status:** v0.1.0 **Columbo** is build-verified with PS2SDK/PS2DEV, but has not yet been validated on real hardware. Treat the results as experimental until hardware testing is completed.

## What it does today

`MC_INSPECTOR.ELF` currently supports both PS2 memory-card ports and can:

- query the card with `mcGetInfo()` and preserve the raw MCMAN result;
- report the card type, format state and free-cluster count;
- verify access to the root directory;
- run a non-destructive 4 KiB write/read/compare test;
- verify that its temporary test file was deleted afterwards;
- distinguish common states such as healthy, full, unformatted, filesystem failure, authentication failure, detection failure and no card;
- offer manual formatting **only** when a PS2 card is reported as unformatted or the filesystem reports `sceMcResNoFormat`.

The program does **not** install FreeMcBoot and does not currently perform MagicGate/KELF qualification. Those features belong to later standalone Inspector milestones.

## Safety model

Inspection should be non-destructive. The R/W test creates a uniquely named temporary file (`/__MCIxx.TMP`), writes a deterministic 4096-byte pattern, reads it back, compares every byte, deletes the file and verifies that it is gone.

Formatting is deliberately harder to trigger than an ordinary test. It is never automatic. The user must first request formatting with **Triangle**, then confirm by holding **L1 + R1** and pressing **Triangle** again. Authentication, detection and generic I/O failures never unlock the formatter.

## Controls

| Control | Action |
| --- | --- |
| Left / Right | Select `mc0:` or `mc1:` |
| Cross | Inspect selected card |
| Start | Inspect both cards |
| Triangle | Enter format confirmation when formatting is allowed |
| L1 + R1 + Triangle | Confirm destructive format |
| Circle | Cancel format confirmation |
| Select | Exit |

## Build

The repository builds a self-contained `MC_INSPECTOR.ELF` with embedded PS2SDK IOP modules (`freesio2`, `freepad`, `mcman`, `mcserv`). CI currently uses the proven `ps2dev/ps2dev:v1.0` environment for compatibility with the toolchain used during initial bring-up.

```sh
make
```

The GitHub Actions workflow additionally verifies the output with `file(1)`, calculates SHA-256 and publishes the ELF as an Actions artifact.

## Project documentation

- [Architecture](docs/ARCHITECTURE.md) — runtime structure, card classification and safety decisions.
- [Testing](docs/TESTING.md) — hardware test matrix and what counts as a valid result.
- [Roadmap](docs/ROADMAP.md) — planned milestones and feature boundaries.
- [Release codenames](docs/CODENAMES.md) — the detective/police naming scheme.
- [Changelog](CHANGELOG.md) — user-visible changes by version.

## Repository policy

`main` is the current standalone application. The earlier FreeMcBoot patch/forced-install prototype remains available through Git history but is intentionally no longer part of the active source tree. We prefer keeping the current tree small and understandable over retaining dead experimental glue indefinitely.

## v0.1.0 — Columbo

The first standalone release is named **Columbo**: not flashy, slightly suspicious of everything, and mostly interested in asking the memory card *just one more question*.
