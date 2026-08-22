# PS2 Memory Card Inspector 0.2.0 "Briscoe"

Briscoe is the first stable release of PS2 Memory Card Inspector with a real-hardware-validated MagicGate/CardAuth capability probe built on the modern **PS2SDK 2.0 SECRMAN 1.4** stack.

## Highlights

- Hardware-validated RAM-only MagicGate/KELF probe.
- Modern PS2SDK 2.0 `secrman_special` 1.4 + matching SECRSIF/card modules.
- Critical logical-port -> physical-SIO2 fix: `mc0 0 -> 2`, `mc1 1 -> 3` at the SECR boundary only.
- In-path GET_KBIT diagnostics without replaying authentication commands.
- Functional distinction between ordinary filesystem compatibility and real CardAuth/MagicGate capability.
- Read-only FreeMcBoot package preflight from USB.
- Known-good Sony ROM X memory-card stack retained for ordinary card I/O.
- Normal card environment rebuilt after every isolated MagicGate attempt.

## Hardware results

| Card | Filesystem | MagicGate |
| --- | --- | --- |
| Sony 8 MB #1 | PASS | `FUNCTIONAL` |
| Sony 8 MB #2 | PASS | `FUNCTIONAL` |
| Third-party 64 MB with functional MagicGate | PASS | `FUNCTIONAL` |
| Third-party 64 MB without functional MagicGate | PASS | `NOT SUPPORTED / NO CARD AUTH ACK` |

The PS2SDK 2.0 SECRMAN 1.4 backend reproduced the same positive and negative controls used during development. This means the probe is detecting functional CardAuth/KELF behavior rather than manufacturer, capacity or a printed MagicGate logo.

## What `FUNCTIONAL` means

The raw `FMCB.XLF` supplied by the user completed the RAM-only binding path through the required SECR stages, including Kbit and Kc. The bound KELF is not written to the memory card.

## Negative-control result

The known non-MagicGate 64 MB card reaches Mechacon preparation but does not ACK the first real CardAuth command:

```text
pre=1/1
command=0x50
tr=1
stat6c=0001D100
id=FF
st=FF
```

This is reported as:

```text
NOT SUPPORTED / NO CARD AUTH ACK
```

Other CardAuth failures remain protocol errors, while Mechacon/session failures remain indeterminate rather than being mislabeled as unsupported MagicGate.

## Important implementation fix

libmc identifies memory cards as logical ports 0 and 1, but SECRMAN CardAuth consumes physical SIO2 channel numbers. Memory cards are channels 2 and 3.

Briscoe therefore keeps normal libmc traffic on logical 0/1 and translates only SECR RPCs carrying a card port:

```text
mc0 logical 0 -> SIO2 2
mc1 logical 1 -> SIO2 3
```

This was the root cause of the earlier repeated `0001D100 / FF / FF` failures on known-good Sony cards.

## Safety model

- Ordinary filesystem testing uses a temporary 4 KiB file and verifies cleanup.
- MagicGate/KELF probing is RAM-only.
- The user-supplied FreeMcBoot package is scanned read-only.
- Formatting is explicit and requires a second destructive confirmation chord.
- FreeMcBoot installation writes are **not enabled** in 0.2.0.

## FreeMcBoot package input

The MagicGate probe expects a raw, unbound file at one of:

```text
mass:/FMCB/SYSTEM/FMCB.XLF
mass0:/FMCB/SYSTEM/FMCB.XLF
mass1:/FMCB/SYSTEM/FMCB.XLF
```

An already installed `osdmain.elf` is not suitable because it is already card-bound.

## Build and provenance

The release targets `ps2dev/ps2dev:v2.0.0` and pins the PS2SDK security source to:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

CI applies `tools/patch_secrman14_diag.py` to a temporary checkout, source-builds SECRMAN 1.4 and matching SECRSIF, stages matching PS2SDK 2.0 card modules, builds the standalone ELF, records source provenance and publishes SHA-256.

## Licensing and attribution

Original PS2 Memory Card Inspector code is released under the MIT License.

PS2SDK components remain under the Academic Free License 2.0. The release package includes the PS2SDK license text, project credits, third-party notices and exact source provenance.

Sony ROM modules and user-supplied FreeMcBoot payloads are not distributed by this project.

## Known limitations / next milestone

0.2.0 is a diagnostic and preflight utility, not yet a FreeMcBoot installer.

The next milestone is deliberately narrow: bind one KELF in RAM, write one controlled target, close/reopen it, read it back in full, verify the result and roll back on failure. A general installer should only follow after that transaction has been validated on backed-up hardware.
