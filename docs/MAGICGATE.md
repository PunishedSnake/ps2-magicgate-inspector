# MagicGate / CardAuth findings

This document records the hardware findings behind the Briscoe MagicGate implementation. It exists so that the low-level conclusions do not disappear into development logs or get reintroduced as regressions later.

## What the Inspector is testing

The current MagicGate test is a **functional KELF binding probe**, not a logo/vendor check and not a filesystem check.

The raw user-supplied `FMCB.XLF` is read into EE RAM while the known-good normal memory-card stack is active. In an isolated security session the Inspector then exercises the same SECR stages needed to obtain card-bound KELF material:

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

Nothing produced by this probe is written to the card.

## The critical port distinction

There are two different port number spaces involved:

- EE/libmc logical memory-card ports: `0` and `1`;
- SIO2 physical device channels used directly by SECRMAN CardAuth: memory cards are `2` and `3`.

The reference FreeMcBoot installer calls:

```c
SecrDownloadFile(2 + port, slot, buffer)
```

SECRMAN's CardAuth code uses the supplied port directly when filling `sio2packet.port_ctrl1[port]`, `port_ctrl2[port]` and the low bits of `regdata[0]`.

Therefore the correct translation is:

```text
mc0 logical 0 -> SECR/SIO2 physical 2
mc1 logical 1 -> SECR/SIO2 physical 3
```

The Inspector performs this translation only at the SECRSIF RPC boundary for calls that carry a memory-card port (`DOWNLOAD_HEADER`, `GET_KBIT`, and `GET_KC`). Normal libmc calls remain on logical `0/1`.

## Why `0001D100 / FF / FF` mattered

Before the port correction, both official Sony cards repeatedly produced:

```text
tr=1 stat6c=0001D100 id=FF st=FF
```

The important distinction is that `sio2_transfer()` returning `1` means the transfer cycle ran; it does not mean the protocol transaction succeeded. The `stat6c` value indicated receive-side failure and missing ACK. `FF/FF` was consistent with no valid card response.

Once the logical-to-physical port mapping was fixed, the same two cards completed Kbit and Kc immediately. A third-party card without functional MagicGate continued to produce the same no-ACK pattern at the correct physical card channel, which turned the old failure signature into a useful negative-control signature rather than a transport bug.

## Development sequence and conclusions

### dev7 — BIT validation false positive

Both official Sony cards passed filesystem checks, session setup, SECR RPC binding, `DownloadHeader`, and the encrypted BIT block. The Inspector then falsely rejected a large plaintext BIT entry because the code applied SECRSIF's `0x400` RPC-payload limit to every BIT entry.

Fix: the `0x400` limit applies only to blocks actually sent through `DownloadBlock` (`flags & 2`). Large plaintext payload blocks only advance the KELF offset.

### dev8 — first real Kbit failure

After correcting BIT semantics, both cards reached `GET KBIT` and both returned `0`. This was the first stage that required the SECRMAN `card_encrypt()` path and its MCMAN callbacks.

The identical result on a card that already booted FMCB and one rejected by a stock installer made a card-specific defect unlikely.

### dev9/dev10 — matching the FMCB-era IOP personality

The isolated security environment was aligned with the PS2SDK-v1 generation used by the FreeMcBoot Installer compatibility source. Loading temporary MCSERV was found to wedge a later LOADFILE RPC on real hardware.

Fix: keep MCMAN resident, skip temporary MCSERV, emulate only the EE-side `mcInit/mcGetInfo/mcSync` sanity query used immediately before the RAM-only probe, then restore the complete normal ROM X stack afterward.

### dev11 — useful wrong command

An independent tracer inserted an `F3` authentication reset before attempting the Kbit transform and observed:

```text
F3: tr=1 stat6c=0001D100 id=FF st=FF
```

Source review then showed that `F3` is **not** a prerequisite of the real `SecrDownloadGetKbit()` path. The actual Kbit card transform starts directly at `F2/50`.

Conclusion: post-failure command replay can perturb or misrepresent the path being diagnosed. The tracer was retired.

### dev12 — instrument the real GET_KBIT

The pinned SECRMAN source was instrumented in place. On failure only, its normal 16-byte Kbit return buffer was replaced with a compact diagnostic record.

Both Sony cards reported:

```text
pre=1/1
CARD ENCRYPT HALF 0
command=50
tr=1 stat6c=0001D100 id=FF st=FF
```

This proved:

1. both Mechacon Kbit halves were prepared;
2. failure occurred in the first real card-side encryption command;
3. the problem was before `51/52/53` and was not a missing Kbit from Mechacon.

### dev13 — physical SIO2 port correction

Comparing the caller with FreeMcBoot exposed the `2 + port` convention. After mapping logical `0/1` to physical `2/3` at the SECR boundary:

- both official Sony 8 MB cards completed the full probe;
- `Kbit=1` and `Kc=1` on both;
- the security session restored the ordinary ROM stack successfully afterward.

That made the port mapping hardware-validated.

## Positive and negative controls

### Positive controls

Two official Sony 8 MB cards:

```text
filesystem PASS
DownloadHeader = 1
encrypted BIT blocks completed
Kbit = 1
Kc = 1
MagicGate = FUNCTIONAL
```

A third-party 64 MB card carrying a MagicGate marking also completed the same probe.

### Negative control

A different third-party 64 MB card remained fully usable as a PS2 filesystem but failed the first real CardAuth command:

```text
pre=1/1
F2/50
tr=1
stat6c=0001D100
id=FF
st=FF
```

Because the same transport/backend passes other cards on the same console, this is classified as:

```text
NOT SUPPORTED / NO CARD AUTH ACK
```

This distinction is central to the project: **filesystem compatibility does not imply MagicGate capability**.

## Current result classification

`FUNCTIONAL`
: Full RAM-only KELF binding probe completed.

`NOT SUPPORTED / NO CARD AUTH ACK`
: Known negative-control signature: both pre-encrypted Kbit halves exist and the first `F2/50` receives a missing-ACK/no-valid-response condition.

`PROTOCOL ERROR / CARD AUTH`
: CardAuth was reached but failed with a different command/status/ID/checksum/SIO2 condition. This should not be automatically interpreted as “no MagicGate”.

`TEST INDETERMINATE / MECHACON`
: Failure occurred while obtaining/preparing Kbit before a conclusive card-side test.

Other session/RPC/KELF errors
: Probe infrastructure or input failure. These are not card capability verdicts.

## Diagnostic record format

Both supported SECR profiles intentionally emit the same failure record through the 16-byte Kbit response **only when GET_KBIT fails**. Successful behavior is left unchanged.

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

The record exists for diagnostics, not as a public protocol or persistent on-card format.

## What this test does not prove

A `FUNCTIONAL` result proves that the tested card/controller can complete the KELF-binding operations exercised by this probe on the tested PS2 stack. It does not certify every possible MagicGate operation, every console revision, or long-term card reliability.

Likewise, a printed “MagicGate” label is not used as a trust signal. The project deliberately prefers observed protocol behavior.

## Next validation step

The modern PS2SDK 2.0 SECRMAN 1.4 backend now builds successfully with equivalent instrumentation. It should be tested against the same card matrix before replacing the hardware-validated `fmcb13` baseline.

Only after the selected backend is stable should the project move to a controlled **bind -> write -> read-back -> verify -> rollback** experiment for FMCB installation.
