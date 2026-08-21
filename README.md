# PS2 Memory Card Inspector

A standalone PlayStation 2 homebrew utility for inspecting memory-card health, MagicGate/KELF behavior and the readiness of user-supplied FreeMcBoot files.

> **Current development line:** v0.2.0 **Briscoe**, built with PS2DEV 2.0.0. The Columbo filesystem pipeline has passed on real PS2 hardware. Briscoe adds isolated MagicGate diagnostics and read-only FMCB package preflight before any installer write path is enabled.

## What it does today

`MC_INSPECTOR.ELF` supports both PS2 memory-card ports and can:

- query cards with `mcGetInfo()` while preserving raw XMCMAN results;
- report type, format state and free-cluster count;
- verify root-directory access;
- run a non-destructive 4 KiB write/read/compare/delete test;
- distinguish ordinary filesystem health from MagicGate/KELF capability;
- run a staged RAM-only MagicGate/KELF probe;
- scan a user-supplied FreeMcBoot package from USB;
- detect console region from `rom0:ROMVER` and resolve the normal FMCB target folder;
- offer formatting only for explicitly unformatted/no-format PS2 cards.

Briscoe dev4 does **not** install FreeMcBoot. FMCB support remains read-only until source, space, backup, KELF binding, verification and rollback behavior have been validated on hardware.

## Hardware regression lesson from dev3

Briscoe dev3 replaced the known-good Sony ROM memory-card stack with a special-SECR IOP personality for the entire application. On real hardware both populated slots then returned:

```text
mcGetInfo rc: -11 (AUTH RESET FAILED)
```

That error is produced before the full MagicGate authentication/binding test: XMCMAN could not complete its `0xF3` card-auth reset command. It was therefore a dev3 stack regression, not evidence that either card was bad.

Dev4 fixes the design rather than hiding the error.

## Runtime architecture

### Normal application personality

All ordinary card I/O uses the same Sony ROM X stack that already passed real-hardware Columbo testing:

```text
IOP reset
  -> rom0:XSIO2MAN
  -> rom0:XPADMAN
  -> rom0:XMCMAN
  -> rom0:XMCSERV
  -> mcInit(MC_TYPE_XMC)
```

Cross, Start, formatting and FMCB preflight always operate from this normal personality.

### Isolated MagicGate personality

Square first locates, reads and validates a test KELF while the normal stack is still active. The KELF stays in EE RAM. Only then does the Inspector temporarily switch IOP personality:

```text
runtime-generated IOPRP with PS2SDK SECRMAN
  -> rom0:XSIO2MAN
  -> rom0:XMCMAN
  -> rom0:XMCSERV
  -> PS2SDK SECRSIF
  -> mcInit(MC_TYPE_XMC)
  -> dedicated mcGetInfo sanity check
  -> staged KELF binding probe
```

The MagicGate page reports session setup, session `mcInit`, session `mcGetInfo`, SECR RPC and individual KELF-binding stages separately. The Inspector then **always reboots back into the normal ROM X stack**, reinitializes the controller and USB source backend, and re-reads both card slots before returning to UI.

A broken experimental MagicGate personality can therefore produce useful diagnostics without breaking ordinary card inspection for the rest of the session.

## Safety model

The ordinary R/W test creates a uniquely named temporary file (`/__MCIxx.TMP`), writes a deterministic 4096-byte pattern, reads it back, compares every byte, deletes it and verifies deletion.

The MagicGate probe modifies only an in-memory KELF copy. Bound Kbit/Kc/ICVPS2 data is not written to either card.

FMCB preflight performs source-side metadata checks only. It does not create target directories or copy, bind or write any FMCB payload. FreeMcBoot payloads are not embedded in this repository or release artifacts.

Formatting is never automatic. Triangle arms formatting; the destructive action requires **L1 + R1 + Triangle** confirmation.

## Controls

| Control | Action |
| --- | --- |
| Left / Right | Select `mc0:` or `mc1:` |
| Cross | Inspect selected card/filesystem |
| Start | Inspect both cards/filesystems |
| Square | Run isolated RAM-only MagicGate/KELF probe; always opens MG result page |
| Circle | Scan `mass:/FMCB`, `mass0:/FMCB`, `mass1:/FMCB` |
| R1 | Cycle Card / MagicGate / FMCB Preflight pages |
| Triangle | Arm format when allowed |
| L1 + R1 + Triangle | Confirm destructive format |
| Circle during format confirmation | Cancel |
| Select | Exit |

## User-supplied FMCB package

See [FMCB package layout](docs/FMCB_PACKAGE.md). Missing optional USB drivers do not make the package incomplete; missing required system/config/resource files do.

```text
INSTALL: DISABLED IN DEV4 (preflight is read-only)
```

## Build

The project targets **PS2DEV 2.0.0**. CI builds in `ps2dev/ps2dev:v2.0.0` using GCC 15.2.0.

```sh
make
```

GitHub Actions verifies the output with `file(1)`, calculates SHA-256 and publishes the ELF artifact.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
- [FMCB package](docs/FMCB_PACKAGE.md)
- [Roadmap](docs/ROADMAP.md)
- [Release codenames](docs/CODENAMES.md)
- [Changelog](CHANGELOG.md)

`main` remains the stable standalone line while Briscoe is developed and hardware-tested in a draft PR.
