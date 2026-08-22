# PS2 Memory Card Inspector

PS2 Memory Card Inspector is a standalone PlayStation 2 homebrew utility for testing memory-card filesystem health, probing real MagicGate/CardAuth capability, and validating a user-supplied FreeMcBoot package before any installation write path is enabled.

The current development line is **v0.2.0 "Briscoe"**. The ordinary filesystem path and the `fmcb13` MagicGate backend have been validated on real PS2 hardware. A second `ps2sdk14` backend, built entirely from the PS2SDK 2.0 security/card stack, is available for controlled comparison and has passed CI compilation; hardware validation is the next step.

## Current status

The Inspector can currently:

- inspect both `mc0:` and `mc1:`;
- report memory-card type, formatting state and free clusters;
- verify root-directory access;
- perform a temporary 4 KiB write/read/compare/delete filesystem test;
- run a **RAM-only** KELF/MagicGate capability probe;
- distinguish a functional MagicGate implementation from a card that simply behaves as ordinary PS2 storage;
- classify CardAuth failures with low-level SIO2 information;
- scan a user-supplied FMCB package on USB without writing it to the card;
- detect the console region and resolve the expected FMCB destination folder;
- guard destructive formatting behind an explicit confirmation chord.

**FMCB installation writes are still disabled.** The next installer milestone is a controlled bind -> write -> read-back -> verify experiment with rollback before any general installation mode is enabled.

## Hardware-validated MagicGate results

The current probe has both positive and negative controls:

| Card | Filesystem | MagicGate/KELF | Observed result |
| --- | --- | --- | --- |
| Official Sony 8 MB, card A | PASS | PASS | `DownloadHeader=1`, encrypted BIT block complete, `Kbit=1`, `Kc=1` |
| Official Sony 8 MB, card B | PASS | PASS | Same complete path |
| Third-party 64 MB without working MagicGate | PASS | FAIL | First real CardAuth command `F2/50`: `pre=1/1`, `tr=1`, `stat6c=0001D100`, `id=FF`, `st=FF` |
| Third-party 64 MB marked MagicGate-capable | PASS | PASS | Complete KELF bind probe |

This is important: **Sony branding and 8 MB capacity are not what the test detects.** A third-party 64 MB card can pass when its controller actually implements the required MagicGate/CardAuth protocol, while another 64 MB card can remain perfectly usable as storage and fail authentication.

A printed MagicGate logo is therefore treated as a claim, not proof. Protocol completion is the proof.

## Result meanings

The user-facing result is intentionally separated from ordinary filesystem health:

- `FUNCTIONAL` — the complete RAM-only KELF binding probe passed.
- `NOT SUPPORTED / NO CARD AUTH ACK` — the validated negative-control signature: Mechacon prepared both Kbit halves, but the card did not ACK the first `F2/50` CardAuth command.
- `PROTOCOL ERROR / CARD AUTH` — the card reached CardAuth but failed at another command, response-ID/status, checksum, or SIO2 condition.
- `TEST INDETERMINATE / ...` — the failure occurred outside a conclusive card capability result, for example in the Mechacon, RPC/session setup, or an unavailable diagnostic path.

See [MagicGate findings](docs/MAGICGATE.md) for the protocol sequence and the investigation history.

## The port-numbering bug that mattered

The most important Briscoe fix was not a different MagicGate implementation. It was using the correct SIO2 channel.

The EE-side libmc API identifies cards as logical ports `0` and `1`, but SECRMAN CardAuth consumes the physical SIO2 memory-card channels. The reference FreeMcBoot installer signs with:

```c
SecrDownloadFile(2 + port, slot, buffer)
```

The Inspector originally forwarded logical `0/1` into SECRMAN. That selected controller channels instead of memory-card channels and produced the repeatable failure:

```text
tr=1 stat6c=0001D100 id=FF st=FF
```

The corrected bridge keeps normal libmc traffic on `0/1` and maps only SECR RPCs carrying a card port:

```text
mc0 logical 0 -> SECR/SIO2 physical 2
mc1 logical 1 -> SECR/SIO2 physical 3
```

After this correction both official Sony 8 MB cards immediately completed `Kbit` and `Kc`, while the non-MagicGate 64 MB card continued to fail at the real first CardAuth command. That positive/negative split is what validates the fix.

## Runtime architecture

### Normal application personality

Ordinary card inspection uses the Sony ROM X-module stack that has already passed real-hardware filesystem testing:

```text
IOP reset
  -> rom0:XSIO2MAN
  -> rom0:XPADMAN
  -> rom0:XMCMAN
  -> rom0:XMCSERV
  -> mcInit(MC_TYPE_XMC)
```

Filesystem tests, formatting UI and FMCB package preflight run from this normal personality.

### Isolated MagicGate personality

Before switching IOP personalities, the Inspector reads a raw user-supplied `FMCB.XLF` into EE RAM. The security session then reboots into a temporary stack containing the selected SECRMAN/SECRSIF and matching memory-card modules. The KELF is modified only in RAM.

Temporary MCSERV is deliberately not allowed to become part of the isolated probe. Real-hardware testing showed that loading it can wedge the following LOADFILE RPC, so MCMAN remains active while the one EE-side libmc sanity query used by the probe is emulated. After the probe, the complete normal ROM X stack is rebuilt before the UI resumes.

See [Architecture](docs/ARCHITECTURE.md).

## Security backend profiles

Briscoe deliberately keeps two reproducible security profiles while the modern stack is evaluated:

| Profile | SECRMAN / SECRSIF | Card stack | Status |
| --- | --- | --- | --- |
| `fmcb13` | Pinned FreeMcBoot Installer compatibility source | PS2SDK v1-era `freesio2/freepad/mcman` | **Hardware validated** |
| `ps2sdk14` | PS2SDK 2.0 `secrman_special` 1.4 + matching SECRSIF | PS2SDK 2.0 `freesio2/freepad/mcman` | **Build validated; hardware test pending** |

Both profiles use the same logical-to-physical port correction and the same compact GET_KBIT failure-record format, so the hardware results are directly comparable.

The `ps2sdk14` profile is built from PS2SDK commit `a13b5971ec0e39c7ba8b8559b80a4e81c8425352`. The first successful instrumented 1.4 CI build was workflow run **#109**; its standalone ELF SHA-256 was:

```text
b5c1df1c4f51b756bf6c62e5d3fc1a9a414362eab77bf3ad13cd095fc7e4723c
```

That checksum identifies the first successful comparison build before the later UI/comment/documentation cleanup. Test reports should always quote the exact artifact they ran.

See [Security backends](docs/SECURITY_BACKENDS.md) and [Building](docs/BUILDING.md).

## Safety model

The filesystem test chooses an unused temporary filename, verifies the card again before writing, writes a deterministic 4096-byte pattern, flushes, closes/reopens, reads and compares it, deletes it, and verifies cleanup.

The MagicGate probe:

- accepts a raw user-supplied `FMCB.XLF` from USB;
- keeps it in EE RAM;
- performs SECRMAN/KELF operations on that RAM copy;
- does **not** write the bound KELF to a memory card;
- restores the normal card stack after the isolated session.

FMCB preflight validates source-side package structure only. It does not create installation directories or copy/bind payloads to the target card.

Formatting is never automatic. The destructive action requires a second confirmation using **L1 + R1 + Triangle**.

## Controls

| Control | Action |
| --- | --- |
| Left / Right | Select `mc0:` or `mc1:` |
| Cross | Inspect selected filesystem |
| Start | Inspect both filesystems |
| Square | Run isolated RAM-only MagicGate/KELF probe |
| Circle | Scan the FMCB package on `mass:` / `mass0:` / `mass1:` |
| R1 | Cycle Card / MagicGate / FMCB Preflight pages |
| Triangle | Arm format when the current card state allows it |
| L1 + R1 + Triangle | Confirm destructive format |
| Circle during format confirmation | Cancel |
| Select | Exit |

## Test KELF / FMCB package

The MagicGate probe expects a raw, unbound FMCB KELF at one of:

```text
mass:/FMCB/SYSTEM/FMCB.XLF
mass0:/FMCB/SYSTEM/FMCB.XLF
mass1:/FMCB/SYSTEM/FMCB.XLF
```

An `osdmain.elf` already installed on a memory card is **not** suitable input: it is already card-bound.

For the wider package preflight layout, see [FMCB package](docs/FMCB_PACKAGE.md).

## Building

The EE application targets **PS2DEV / PS2SDK 2.0**. The canonical reproducible builds are the GitHub Actions profiles because each workflow stages the exact matching IOP modules before linking the standalone ELF.

The build system accepts:

```text
SECR_PROFILE=fmcb13
SECR_PROFILE=ps2sdk14
```

A plain local `make SECR_PROFILE=...` expects the matching staged files under `.build/`; see [Building](docs/BUILDING.md) for the exact layout and pinned upstream revisions.

## Documentation

- [MagicGate / CardAuth findings](docs/MAGICGATE.md)
- [Security backend profiles](docs/SECURITY_BACKENDS.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Hardware and regression testing](docs/TESTING.md)
- [Building and reproducibility](docs/BUILDING.md)
- [FMCB package layout](docs/FMCB_PACKAGE.md)
- [Roadmap](docs/ROADMAP.md)
- [Release codenames](docs/CODENAMES.md)
- [Changelog](CHANGELOG.md)
- [Credits](CREDITS.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## License and attribution

Original PS2 Memory Card Inspector source in this repository is released under the [MIT License](LICENSE), except where a file or imported/build-time component states otherwise.

The project builds against and, depending on the selected profile, embeds or derives build artifacts from third-party PS2 homebrew projects. Those components retain their own licenses and copyright notices. In particular, PS2SDK is distributed under the **Academic Free License 2.0**.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before redistributing binaries. The legacy `fmcb13` compatibility profile uses pinned FreeMcBoot Installer source at build time; its upstream provenance is documented separately and its redistribution terms should be verified independently before using that profile for a public binary release.

PlayStation, MagicGate and related names are trademarks of their respective owners. This project is an independent homebrew utility and is not affiliated with or endorsed by Sony Interactive Entertainment.
