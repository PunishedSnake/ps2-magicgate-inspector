# MagicGate / CardAuth findings

This document preserves the hardware findings behind Briscoe so the low-level conclusions do not disappear into development logs or get reintroduced as regressions.

## What Inspector tests

The MagicGate test is a **functional KELF-binding probe**, not a logo/vendor check and not a filesystem check.

A raw user-supplied `FMCB.XLF` is read into EE RAM while the normal Sony ROM card stack is active. The isolated PS2SDK 2.0 security session then exercises:

```text
DownloadHeader
  -> encrypted BIT block(s)
  -> SecrDownloadGetKbit
       -> Mechacon Kbit preparation
       -> card_encrypt half 0
            F2/50 -> F2/51 -> F2/52 -> F2/53
       -> card_encrypt half 1
            F2/50 -> F2/51 -> F2/52 -> F2/53
  -> SecrDownloadGetKc
       -> Mechacon Kc preparation
       -> card_encrypt half 0
       -> card_encrypt half 1
  -> optional ICVPS2 when required by the KELF
```

Nothing produced by the probe is written to the card.

## Critical port distinction

Two port-number spaces are involved:

- libmc logical memory-card ports: `0` and `1`;
- physical SIO2 channels used directly by SECRMAN CardAuth: memory cards are `2` and `3`.

The reference FreeMcBoot binding path calls:

```c
SecrDownloadFile(2 + port, slot, buffer)
```

SECRMAN CardAuth uses the supplied number directly in SIO2 packet setup. Therefore Inspector must translate:

```text
mc0 logical 0 -> SECR/SIO2 physical 2
mc1 logical 1 -> SECR/SIO2 physical 3
```

Only SECRSIF RPCs carrying a memory-card port are translated (`DOWNLOAD_HEADER`, `GET_KBIT`, `GET_KC`). Normal libmc calls remain logical 0/1.

## Why `0001D100 / FF / FF` mattered

Before the correction, both official Sony cards repeatedly produced:

```text
tr=1 stat6c=0001D100 id=FF st=FF
```

`sio2_transfer()` returning `1` only means the transfer cycle ran; it does not prove protocol success. `stat6c` showed receive-side failure and missing ACK, while `FF/FF` was consistent with no valid card response.

Once the correct physical card channels were used, those Sony cards immediately completed Kbit and Kc. A different third-party card without functional MagicGate continued to produce the same no-ACK condition **on the correct channel**, making the signature a useful negative control rather than a transport bug.

## Investigation sequence

### dev7 — BIT validation false positive

Both Sony cards reached `DownloadHeader` and the encrypted BIT block. Inspector then rejected a large plaintext BIT entry because the 0x400-byte SECRSIF block-RPC limit was incorrectly applied to every entry.

Fix: apply 0x400 only to entries actually sent to `DownloadBlock` (`flags & 2`). Large plaintext entries only advance the KELF offset.

### dev8 — first genuine Kbit failure

After fixing BIT semantics, both cards reached GET_KBIT and returned 0. This was the first tested stage that required SECRMAN `card_encrypt()` and MCMAN's callbacks.

### dev9/dev10 — isolated card-stack behavior

The temporary card stack was aligned with the security implementation. Hardware testing showed temporary MCSERV could report successful residency and still wedge the following LOADFILE RPC.

Fix: keep MCMAN resident, skip temporary MCSERV, emulate only the immediate EE-side libmc sanity query, and rebuild the normal Sony ROM X stack after the probe.

### dev11 — useful wrong command

A diagnostic tracer inserted F3 before Kbit and observed:

```text
F3: tr=1 stat6c=0001D100 id=FF st=FF
```

Source review showed F3 is not a prerequisite of real `SecrDownloadGetKbit()`. The real Kbit card transform starts at `F2/50` after Mechacon preparation.

Conclusion: post-failure command replay can perturb or misrepresent the path being diagnosed. The tracer was retired.

### dev12 — instrument the real GET_KBIT

SECRMAN was instrumented in place. Both Sony cards reported:

```text
pre=1/1
CARD ENCRYPT HALF 0
command=50
tr=1 stat6c=0001D100 id=FF st=FF
```

This proved both Mechacon Kbit halves were prepared and the failure occurred at the first genuine CardAuth command.

### dev13 — physical SIO2 port correction

Comparing the caller with the FreeMcBoot reference exposed the `2 + port` convention. After translating logical 0/1 to physical 2/3 at the SECR boundary:

- both Sony 8 MB cards completed the full probe;
- `Kbit=1` and `Kc=1`;
- normal ROM stack restoration remained functional.

### PS2SDK 2.0 SECRMAN 1.4 validation

A modern backend was then built from pinned PS2SDK 2.0 source with equivalent failed-GET_KBIT instrumentation.

It reproduced the same hardware matrix:

- Sony 8 MB: `FUNCTIONAL`;
- third-party 64 MB with functional MagicGate: `FUNCTIONAL`;
- third-party 64 MB without functional MagicGate: `NOT SUPPORTED / NO CARD AUTH ACK`.

That result allowed the project to retire the historical compatibility backend and ship 0.2.0 with PS2SDK 2.0 SECRMAN 1.4 as the single production security implementation.

## Positive and negative controls

Known positive behavior:

```text
filesystem PASS
DownloadHeader = 1
encrypted BIT blocks completed
Kbit = 1
Kc = 1
MagicGate = FUNCTIONAL
```

Known negative-control behavior:

```text
filesystem PASS
pre=1/1
F2/50
tr=1
stat6c=0001D100
id=FF
st=FF
MagicGate = NOT SUPPORTED / NO CARD AUTH ACK
```

Filesystem compatibility therefore does not imply MagicGate capability. Likewise, card capacity, Sony branding and a printed MagicGate logo are not used as trust signals.

## Result classification

`FUNCTIONAL`
: Full RAM-only KELF binding probe completed.

`NOT SUPPORTED / NO CARD AUTH ACK`
: Hardware-validated negative-control signature: both Kbit halves were prepared, but the first CardAuth command received no valid ACK/response.

`PROTOCOL ERROR / CARD AUTH`
: CardAuth was reached but failed with a different command, response ID/status, checksum or SIO2 condition. This is not automatically interpreted as no MagicGate.

`TEST INDETERMINATE / MECHACON`
: Failure occurred before a conclusive card-side result.

Other session/RPC/KELF errors
: Probe infrastructure or input failure, not a card capability verdict.

## Diagnostic record format

The instrumented PS2SDK 2.0 SECRMAN 1.4 emits this 16-byte record **only when GET_KBIT fails**:

| Byte | Meaning |
| ---: | --- |
| 0 | magic `0xD2` |
| 1 | magic `0x12` |
| 2 | stage: Mechacon half 0/1 or CardAuth half 0/1 |
| 3 | CardAuth command, or `0xFF` when none ran |
| 4 | classified low-level reason |
| 5 | transfer/callback return |
| 6 | response ID |
| 7 | response status |
| 8..11 | little-endian `stat6c` |
| 12 | first Kbit half non-zero after Mechacon preparation |
| 13 | second Kbit half non-zero after Mechacon preparation |
| 14 | first Mechacon step succeeded |
| 15 | second Mechacon step succeeded |

Successful GET_KBIT behavior is unchanged and no persistent/on-card diagnostic format is created.

## What `FUNCTIONAL` proves

A `FUNCTIONAL` result proves that the tested card/controller can complete the KELF-binding operations exercised by this probe on the tested PS2/security stack. It does not certify every possible MagicGate operation, every console revision or long-term card reliability.

The next project milestone is not deeper read-only authentication. It is a deliberately controlled **bind -> write -> close/reopen -> read-back -> verify -> rollback** experiment before any general FMCB installation mode is enabled.
