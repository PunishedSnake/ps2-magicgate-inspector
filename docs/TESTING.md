# Hardware and regression testing

PS2 Memory Card Inspector is validated on real PlayStation 2 hardware. Emulator-only success is not sufficient for memory-card, SIO2 or MagicGate changes.

## Release backend

0.2.0 uses the PS2SDK 2.0 SECRMAN 1.4 backend built from pinned PS2SDK commit:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

The final release candidate must continue to reproduce the hardware matrix below after any code/documentation cleanup that changes the ELF.

## Hardware matrix

| Card | Role | Filesystem | MagicGate |
| --- | --- | --- | --- |
| Sony 8 MB A | positive control | PASS | `FUNCTIONAL` |
| Sony 8 MB B | positive control | PASS | `FUNCTIONAL` |
| Third-party 64 MB, functional MagicGate | third-party positive control | PASS | `FUNCTIONAL` |
| Third-party 64 MB, no functional MagicGate | negative control | PASS | `NOT SUPPORTED / NO CARD AUTH ACK` |

The PS2SDK 2.0 SECRMAN 1.4 build reproduced the same positive/negative behavior that was previously established with the compatibility baseline. That closes the backend-comparison milestone.

## Minimum data for a regression report

Record:

```text
Inspector version / commit
ELF SHA-256
selected slot
card description
filesystem result
MagicGate result
Kbit/Kc values or failure stage
stat6c / id / status when shown
normal-stack restore result
```

Because 0.2.0 has one production backend, a separate profile name is no longer required, but the exact ELF checksum still matters.

## Filesystem regression test

The ordinary card path must remain on the Sony ROM X stack.

Expected sequence:

1. `mcGetInfo()` and error classification work;
2. root-directory query works;
3. Inspector chooses an unused temporary filename;
4. the card is rechecked before writing;
5. 4096 deterministic bytes are written and flushed;
6. the file is closed, reopened and read;
7. all bytes compare;
8. the file is deleted;
9. deletion is verified.

Known baseline: both official Sony cards passed the full create/write/flush/reopen/read/compare/delete path.

Any MagicGate change that breaks this ordinary path after restoration is a regression even if the security probe itself passes.

## Positive-control MagicGate test

For a known-good card:

1. place raw `FMCB.XLF` at an accepted `mass:` path;
2. run the Square/MagicGate probe;
3. confirm session setup succeeds;
4. confirm `DownloadHeader = 1`;
5. confirm required encrypted BIT blocks complete;
6. confirm `Kbit = 1`;
7. confirm `Kc = 1`;
8. confirm final result is `FUNCTIONAL` and stage is `DONE`;
9. rerun ordinary filesystem inspection afterwards to confirm normal-stack restoration.

For the FMCB.XLF used during development the observed BIT summary was:

```text
BIT blocks: 2
encrypted: 1
completed: 1
```

Those values describe that test file and must not be hardcoded as a universal KELF rule.

## Negative-control MagicGate test

The known third-party 64 MB card without functional MagicGate remains usable as ordinary PS2 storage but fails the first real Kbit CardAuth transaction.

Validated signature:

```text
pre=1/1
half=0
command=0x50
tr=1
stat6c=0001D100
id=FF
status=FF
```

Expected user-facing result:

```text
NOT SUPPORTED / NO CARD AUTH ACK
```

This negative control prevents false-positive regressions. A backend that suddenly returns `FUNCTIONAL` for every card is suspect; a build that makes the Sony controls fail with the same no-ACK signature is also suspect.

## Historical regressions that must not return

### Applying the 0x400 limit to every BIT entry

Wrong: reject any BIT entry larger than 0x400.

Correct: every entry must fit inside the KELF, but only entries marked `flags & 2` are sent through the 0x400-byte SECRSIF block RPC.

### Using installed `osdmain.elf` as raw input

Wrong: use an already card-bound installed KELF.

Correct: use a raw user-supplied `FMCB.XLF` from USB.

### Starting temporary MCSERV

Observed: temporary MCSERV could report successful residency and still wedge the following LOADFILE RPC.

Correct release behavior: keep temporary MCMAN active, skip temporary MCSERV, emulate only the immediate EE-side libmc sanity call, then rebuild the real ROM X stack after the probe.

### Injecting an extra F3 before GET_KBIT

Wrong: replay an F3 reset as a diagnostic prerequisite.

Correct: the real GET_KBIT CardAuth path begins after Mechacon preparation with `F2/50 -> 51 -> 52 -> 53`. Diagnostics must instrument that path in place.

### Forwarding logical libmc ports to CardAuth

Wrong:

```text
mc0 -> 0
mc1 -> 1
```

at the SECRMAN boundary.

Correct:

```text
mc0 logical 0 -> SIO2 2
mc1 logical 1 -> SIO2 3
```

only for SECR RPCs carrying a card port.

The incorrect mapping produced the repeated `0001D100 / FF / FF` signature on known-good cards.

## CI regression

The release workflow must successfully:

- stage PS2SDK 2.0 SIO2/PAD/MCMAN modules;
- check out the pinned PS2SDK source;
- apply `tools/patch_secrman14_diag.py`;
- build SECRMAN 1.4 and matching SECRSIF with PS2SDK's warning-as-error policy;
- link `MC_INSPECTOR.ELF`;
- package license/provenance files;
- compute and publish SHA-256.

The first 1.4 instrumentation attempt failed because expanding GET_KBIT left stock `scePreEncryptKbit()` unused under `-Werror`. The patcher now removes that helper and its forward declaration from the temporary source tree.

## FMCB preflight regression

FMCB package scanning remains read-only. Verify:

- package discovery on `mass:`, `mass0:` and `mass1:`;
- required/optional file handling;
- region-to-target-folder resolution;
- no target-card writes during scan;
- ordinary card functionality after USB backend reinitialization.

## Future write-path validation

A successful MagicGate capability test does not authorize a general FMCB install button.

The first write-capable experiment must use a disposable or fully backed-up target and implement:

1. preflight and free-space check;
2. backup of replaced files;
3. KELF bind in RAM;
4. write only the intended target;
5. close/reopen through normal filesystem APIs;
6. read back the entire result;
7. verify exact expected contents/metadata;
8. rollback on any mismatch;
9. power-cycle boot testing only after on-card verification passes.

Until that transaction is implemented and validated, Inspector remains a diagnostic/preflight utility rather than an FMCB installer.
