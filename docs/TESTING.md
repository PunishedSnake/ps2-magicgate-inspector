# Hardware and regression testing

PS2 Memory Card Inspector is developed against real PlayStation 2 hardware. Emulator-only success is not considered sufficient for memory-card, SIO2 or MagicGate changes.

This document defines the current regression matrix and records which conclusions have real-hardware evidence behind them.

## General test rule

When reporting a result, record at minimum:

```text
application revision / commit
SECR_PROFILE
selected slot
card description
filesystem result
MagicGate result
Kbit/Kc values or failure stage
stat6c / id / status when a CardAuth failure is shown
normal-stack restore result
```

A MagicGate screenshot without the security profile is incomplete regression data.

## Known card matrix

The Briscoe investigation currently has four useful physical cards:

| Card | Role | Filesystem | `fmcb13` MagicGate |
| --- | --- | --- | --- |
| Official Sony 8 MB A | positive control | PASS | FUNCTIONAL |
| Official Sony 8 MB B | positive control | PASS | FUNCTIONAL |
| Third-party 64 MB, no functional MagicGate | negative control | PASS | `NOT SUPPORTED / NO CARD AUTH ACK` |
| Third-party 64 MB, MagicGate-capable | third-party positive control | PASS | FUNCTIONAL |

The last two cards are particularly useful because they prove that capacity and manufacturer branding are not proxies for actual CardAuth behavior.

## Filesystem regression test

The ordinary card path must continue to use the normal ROM X stack.

Expected sequence:

1. `mcGetInfo()` succeeds or returns a state the UI correctly classifies;
2. root directory can be queried for a healthy formatted PS2 card;
3. temporary 4 KiB file is created;
4. deterministic data is written and flushed;
5. file is reopened and read;
6. all bytes compare;
7. file is deleted;
8. deletion/cleanup is verified.

Known hardware baseline: both official Sony cards completed the full create/write/flush/reopen/read/compare/delete path.

Any change to the isolated security stack that breaks this ordinary path **after restoration** is a regression even if MagicGate itself passes.

## MagicGate positive-control test

For each positive-control card:

1. place a raw `FMCB.XLF` at `mass:/FMCB/SYSTEM/FMCB.XLF` or an accepted numbered `mass` path;
2. run the MagicGate probe;
3. confirm session setup succeeds;
4. confirm `DownloadHeader = 1`;
5. confirm the expected encrypted BIT block count completes;
6. confirm `Kbit = 1`;
7. confirm `Kc = 1`;
8. confirm final result is `FUNCTIONAL`;
9. after returning to the UI, rerun ordinary filesystem inspection to verify the ROM X stack was restored correctly.

For the currently used FMCB.XLF test input the observed BIT summary has been:

```text
BIT blocks: 2
encrypted: 1
completed: 1
```

Do not hardcode those values as a universal KELF rule; they describe the current test file.

## MagicGate negative-control test

The known third-party 64 MB card without functional MagicGate is expected to remain filesystem-readable but fail the first real Kbit CardAuth transaction.

Validated `fmcb13` signature:

```text
pre=1/1
half=0
command=0x50
tr=1
stat6c=0001D100
id=FF
status=FF
```

Expected user-facing classification:

```text
NOT SUPPORTED / NO CARD AUTH ACK
```

This negative control is important. If a new backend suddenly reports `FUNCTIONAL` on every card, the probe may have become a false positive. If it fails the Sony positive controls with the same signature, suspect backend/session/port regression before blaming the cards.

## `ps2sdk14` hardware comparison plan

The PS2SDK 2.0 SECRMAN 1.4 comparison profile has compiled and linked successfully. Hardware equivalence is still pending.

Test it in this order:

### 1. Sony 8 MB positive control A

Expected ideal result:

```text
FUNCTIONAL
Kbit=1
Kc=1
```

If this fails, capture the complete GET_KBIT diagnostic line before changing code.

### 2. Sony 8 MB positive control B

Confirms the result is not specific to the first card.

### 3. Third-party 64 MB negative control

Expected result should remain a clean first-command no-ACK or another clearly card-side rejection. A false `FUNCTIONAL` result here requires investigation.

### 4. Third-party 64 MB MagicGate-capable positive control

Expected `FUNCTIONAL`.

### 5. Post-probe filesystem regression

After each card/security test, verify the normal ROM stack and ordinary R/W path again.

Only after all of those agree with the `fmcb13` baseline should `ps2sdk14` be considered hardware-validated.

## Historical regression cases that must not return

### Large plaintext BIT entry rejected as > 0x400

Cause: the SECRSIF RPC block-size limit was incorrectly applied to every BIT entry.

Correct behavior: validate every block against KELF bounds, but apply the `0x400` RPC payload limit only when `flags & 2` means the block is actually sent to `DownloadBlock`.

### Installed `osdmain.elf` used as raw bind input

Cause: confusion between a source KELF and an already card-bound installed KELF.

Correct behavior: use a raw user-supplied `FMCB.XLF` from USB.

### Temporary MCSERV wedges LOADFILE RPC

Cause: isolated PS2SDK-v1 MCSERV could become resident yet interfere with the following module load.

Correct behavior: skip temporary MCSERV during the isolated probe, keep MCMAN active, and emulate only the immediate EE-side libmc sanity query.

### Extra F3 inserted before GET_KBIT

Cause: independent diagnostic replay assumed authentication reset was part of the actual GET_KBIT sequence.

Correct behavior: do not inject F3. The real card transform starts at `F2/50` after Mechacon preparation.

### Logical libmc port forwarded to CardAuth

Cause: EE logical `0/1` was passed directly to SECRMAN, which consumes physical SIO2 channels.

Observed failure:

```text
0001D100 / FF / FF
```

Correct behavior:

```text
0 -> 2
1 -> 3
```

at the SECR RPC boundary only.

## CI/build regression

Every security profile must at least compile in CI before hardware testing.

The first `ps2sdk14` instrumentation attempt failed compilation because the diagnostic expansion left stock `scePreEncryptKbit()` unused and PS2SDK treats warnings as errors. That was a build-patcher bug, not a SECRMAN runtime failure.

Corrected comparison build:

```text
workflow #109
ELF SHA256 b5c1df1c4f51b756bf6c62e5d3fc1a9a414362eab77bf3ad13cd095fc7e4723c
```

## FMCB preflight tests

FMCB package scanning remains read-only.

Verify:

- package root discovery on `mass:`, `mass0:` and `mass1:`;
- required file detection;
- optional file handling;
- console-region target resolution;
- no target card writes during scan;
- ordinary card functionality after USB backend shutdown/reinitialization.

## Future installer write validation

Do not jump directly from a successful MagicGate capability test to a general FMCB install button.

The first write-capable experiment should use a disposable/fully backed-up target and must implement:

1. preflight and free-space check;
2. backup of anything that would be replaced;
3. bind KELF in RAM;
4. write only the test target;
5. close and reopen it through normal filesystem APIs;
6. read back the entire file;
7. verify exact expected bound contents/metadata;
8. restore/rollback on any mismatch;
9. power-cycle boot test only after on-card verification passes.

Until this transaction is implemented and validated, the project remains a diagnostic/preflight utility rather than an FMCB installer.
