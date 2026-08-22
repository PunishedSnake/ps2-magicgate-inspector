# PS2 Memory Card Inspector

PS2 Memory Card Inspector is a standalone PlayStation 2 homebrew utility for testing memory-card filesystem health, probing real MagicGate/CardAuth capability, and validating a user-supplied FreeMcBoot package before any installation write path is enabled.

## v0.2.0 "Briscoe"

Briscoe is the first release with a hardware-validated MagicGate/KELF capability probe built on the modern **PS2SDK 2.0 SECRMAN 1.4** stack.

The ordinary memory-card path remains on the Sony ROM X modules that passed real-hardware filesystem testing. MagicGate is run in a temporary isolated IOP session, the KELF is modified only in EE RAM, and the normal ROM stack is rebuilt afterwards.

### Hardware validation

| Card | Filesystem | MagicGate result |
| --- | --- | --- |
| Sony 8 MB #1 | PASS | `FUNCTIONAL` |
| Sony 8 MB #2 | PASS | `FUNCTIONAL` |
| Third-party 64 MB with functional MagicGate | PASS | `FUNCTIONAL` |
| Third-party 64 MB without functional MagicGate | PASS | `NOT SUPPORTED / NO CARD AUTH ACK` |

The same positive/negative split was reproduced with the final PS2SDK 2.0 SECRMAN 1.4 backend. The probe therefore detects functional CardAuth/KELF capability rather than Sony branding, printed logos or card capacity.

## What it does

- inspects both `mc0:` and `mc1:`;
- reports card type, formatting state and free clusters;
- verifies root-directory access;
- performs a temporary 4 KiB write/read/compare/delete filesystem test;
- runs a **RAM-only** MagicGate/KELF capability probe;
- distinguishes functional MagicGate from ordinary PS2 storage without working CardAuth;
- reports low-level CardAuth failure details when GET_KBIT fails;
- scans a user-supplied FreeMcBoot package from USB without installing it;
- detects the console region and resolves the expected FMCB destination folder;
- exposes formatting only through an explicit destructive confirmation chord.

**0.2.0 is not an FMCB installer.** FMCB package handling is read-only preflight. The next installation milestone is a controlled bind -> write -> reopen -> read-back -> verify transaction with rollback.

## MagicGate result meanings

`FUNCTIONAL` means the complete RAM-only binding path reached `DONE`, including `DownloadHeader`, required encrypted BIT blocks, `GET_KBIT`, `GET_KC`, and ICVPS2 when required.

`NOT SUPPORTED / NO CARD AUTH ACK` is the hardware-validated negative-control signature: both Mechacon Kbit halves were prepared, but the card did not ACK the first real CardAuth command (`0x50`).

`PROTOCOL ERROR / CARD AUTH` means the card reached CardAuth but failed at another command or response-validation condition.

`TEST INDETERMINATE / ...` is reserved for failures that do not prove card capability, such as Mechacon, RPC, session or malformed diagnostic failures.

## The critical port-numbering fix

libmc exposes cards as logical ports `0` and `1`, but SECRMAN CardAuth consumes physical SIO2 channel numbers. The memory-card channels are `2` and `3`.

The reference FreeMcBoot binding path enters SECRMAN with `2 + port`. Inspector initially forwarded logical 0/1 directly, which selected controller channels and produced repeated:

```text
stat6c=0001D100 id=FF st=FF
```

Briscoe keeps normal libmc traffic on 0/1 and translates only SECR requests that contain a card port:

```text
mc0 logical 0 -> SECR/SIO2 physical 2
mc1 logical 1 -> SECR/SIO2 physical 3
```

Once that was corrected, known-good cards completed Kbit/Kc while the non-MagicGate card continued to fail at the genuine first CardAuth command. See [MagicGate findings](docs/MAGICGATE.md).

## Runtime architecture

### Normal personality

```text
IOP reset
  -> rom0:XSIO2MAN
  -> rom0:XPADMAN
  -> rom0:XMCMAN
  -> rom0:XMCSERV
  -> mcInit(MC_TYPE_XMC)
```

This path handles filesystem inspection, the temporary R/W test, formatting UI and FMCB package preflight.

### Isolated MagicGate personality

Before the IOP switch, a raw user-supplied `FMCB.XLF` is read into EE RAM. The temporary security session uses:

```text
PS2SDK 2.0 SECRMAN 1.4
PS2SDK 2.0 SECRSIF
PS2SDK 2.0 freesio2 / freepad / mcman
```

Temporary MCSERV is intentionally not started because hardware testing showed that it can wedge the following LOADFILE RPC. CardAuth requires MCMAN's registered SECRMAN callbacks, so MCMAN remains active while the immediate EE-side libmc sanity query is emulated.

After the probe, Inspector rebuilds the Sony ROM X stack before returning to normal operation.

See [Architecture](docs/ARCHITECTURE.md).

## Test KELF / FMCB package

The MagicGate probe expects a raw, unbound FMCB KELF at one of:

```text
mass:/FMCB/SYSTEM/FMCB.XLF
mass0:/FMCB/SYSTEM/FMCB.XLF
mass1:/FMCB/SYSTEM/FMCB.XLF
```

An `osdmain.elf` already installed on a memory card is not suitable input because it is already card-bound.

For the wider package layout, see [FMCB package](docs/FMCB_PACKAGE.md).

## Controls

The 0.3 development UI is deliberately **target-centric**. Left/Right selects the card; Cross always runs the complete read-only diagnostic suite for that selected slot, regardless of which results tab is currently visible. This keeps Square, Circle and Start free for future installer/write actions instead of spending one button per diagnostic subsystem.

| Control | Action |
| --- | --- |
| Left / Right | Select `mc0:` or `mc1:` |
| Cross | Run full selected-card scan: filesystem -> MagicGate/CardAuth -> FMCB preflight |
| R1 | Cycle Card / MagicGate / FMCB Preflight result pages |
| Triangle | Arm format when allowed |
| L1 + R1 + Triangle | Confirm destructive format |
| Circle during format confirmation | Cancel |
| Square / Circle / Start | Reserved for future actions outside format confirmation |
| Select | Exit |

Changing the visible page does not change what Cross runs. Changing the selected slot changes the target of the next full scan; the test does not auto-start merely because the selection moved.

## Building

The release build targets **PS2DEV / PS2SDK 2.0.0**. CI pins the security source to PS2SDK commit:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

CI applies `tools/patch_secrman14_diag.py`, builds SECRMAN 1.4 and matching SECRSIF from that checkout, stages the matching PS2SDK 2.0 SIO2/PAD/MCMAN modules, then builds the standalone EE ELF.

A plain local `make` expects those staged modules under `.build/`; see [Building](docs/BUILDING.md).

## Safety

The 4 KiB filesystem test uses an unused temporary filename, writes a deterministic pattern, flushes, closes/reopens, reads and compares it, deletes it, and verifies cleanup.

The MagicGate probe operates on a RAM copy of the KELF and never writes the bound result to the card.

FMCB package preflight performs source-side validation only. Formatting is never automatic and requires **L1 + R1 + Triangle** after being armed.

## Documentation

- [Release notes](RELEASE_NOTES.md)
- [MagicGate / CardAuth findings](docs/MAGICGATE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Security backend provenance](docs/SECURITY_BACKENDS.md)
- [Hardware and regression testing](docs/TESTING.md)
- [Building and reproducibility](docs/BUILDING.md)
- [FMCB package layout](docs/FMCB_PACKAGE.md)
- [Roadmap](docs/ROADMAP.md)
- [Changelog](CHANGELOG.md)
- [Credits](CREDITS.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## License and attribution

Original PS2 Memory Card Inspector source is released under the [MIT License](LICENSE), except where a file or third-party component states otherwise.

The release builds against and embeds PS2SDK components. PS2SDK is distributed under the **Academic Free License 2.0**; its license text is included under [`licenses/PS2SDK-AFL-2.0.txt`](licenses/PS2SDK-AFL-2.0.txt). Exact source provenance and modification details are recorded in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Sony ROM modules and user-supplied FreeMcBoot payloads are not distributed by this project.

PlayStation, MagicGate and related names are trademarks of their respective owners. PS2 Memory Card Inspector is an independent homebrew project and is not affiliated with or endorsed by Sony Interactive Entertainment.