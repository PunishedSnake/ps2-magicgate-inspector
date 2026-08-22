# Architecture

PS2 Memory Card Inspector 0.2.0 "Briscoe" separates ordinary memory-card I/O from MagicGate/KELF work. That separation is a direct result of real-hardware testing: the normal filesystem path and the security path have different module requirements, and allowing one experimental IOP personality to own the entire application caused regressions.

## Design rules

1. Ordinary card I/O stays on the Sony ROM X stack that passed hardware tests.
2. MagicGate runs as an isolated session that may fail without poisoning the rest of the application.
3. The raw test KELF is loaded into EE RAM before the security-session reboot.
4. The capability probe never writes a bound KELF to the memory card.
5. GET_KBIT failures are instrumented in place; authentication commands are never replayed for diagnosis.
6. The normal ROM stack is rebuilt after every MagicGate attempt.
7. The release uses one reproducible modern security backend: PS2SDK 2.0 SECRMAN 1.4 plus matching PS2SDK 2.0 modules.

## High-level flow

```text
startup
  |
  v
normal Sony ROM X card personality
  |
  +--> filesystem inspection
  +--> temporary 4 KiB R/W test
  +--> guarded format UI
  +--> FMCB package preflight
  |
  +--> MagicGate requested
         |
         +--> locate/read/validate raw FMCB.XLF into EE RAM
         +--> close normal pad/USB clients
         +--> reboot IOP with source-built PS2SDK SECRMAN 1.4
         +--> load PS2SDK 2.0 SIO2/PAD/MCMAN
         +--> intentionally skip temporary MCSERV
         +--> load PS2SDK 2.0 SECRSIF
         +--> run RAM-only KELF binding probe
         +--> discard RAM KELF
         +--> rebuild normal Sony ROM X card personality
         +--> reopen USB/pad clients
         +--> re-inspect both card slots
         |
         v
       UI
```

## Normal application personality

Ordinary filesystem operations use the console's ROM modules:

```text
rom0:XSIO2MAN
rom0:XPADMAN
rom0:XMCMAN
rom0:XMCSERV
```

followed by `mcInit(MC_TYPE_XMC)`.

This path is intentionally independent from the embedded PS2SDK security stack. It passed create/write/flush/reopen/read/compare/delete tests on real PS2 hardware and is restored after every security-session attempt.

## Production MagicGate personality

The 0.2.0 release uses the PS2SDK 2.0 generation exclusively:

```text
PS2SDK 2.0 SECRMAN 1.4
PS2SDK 2.0 SECRSIF
PS2SDK 2.0 freesio2
PS2SDK 2.0 freepad
PS2SDK 2.0 mcman
```

CI pins PS2SDK source revision:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

`tools/patch_secrman14_diag.py` is applied to a temporary checkout before SECRMAN is built. The patch adds failed-GET_KBIT observability but does not replace the successful upstream path. Exact provenance and licensing are recorded in `THIRD_PARTY_NOTICES.md`.

## Why temporary MCSERV is skipped

Hardware testing showed that starting the temporary MCSERV can report success and still leave the following LOADFILE RPC wedged.

The security probe needs MCMAN's SECRMAN callbacks, not ordinary EE-side file-service traffic during the isolated phase. Therefore:

- temporary MCMAN remains active;
- temporary MCSERV is intercepted and not started;
- the immediate EE-side `mcInit/mcGetInfo/mcSync` sanity sequence is emulated;
- all normal libmc behavior resumes after the Sony ROM X environment is rebuilt.

This behavior is isolated in `src/magicgate_session.c`.

## Logical vs physical memory-card ports

This rule is easy to miss and must not be simplified away.

libmc exposes logical ports:

```text
mc0 -> 0
mc1 -> 1
```

SECRMAN CardAuth consumes the supplied number as an SIO2 channel index. The physical memory-card channels are:

```text
mc0 -> 2
mc1 -> 3
```

The reference FreeMcBoot binding path enters SECRMAN with `2 + port`. Inspector therefore keeps ordinary libmc code on 0/1 and translates only SECRSIF requests that carry a card port:

```text
DOWNLOAD_HEADER
GET_KBIT
GET_KC
```

That bridge is implemented in `src/magicgate_diag.c`.

The original 0/1 bug was the root cause of repeated `stat6c=0001D100 id=FF st=FF` failures on otherwise working Sony cards: CardAuth was being sent to controller channels instead of memory-card channels.

## KELF probe stages

`src/magicgate.c` performs the high-level transaction:

1. bind SECRSIF RPC clients;
2. `DownloadHeader`;
3. parse returned BIT metadata;
4. send only BIT entries marked for security download (`flags & 2`);
5. advance over large plaintext BIT entries without forcing them through the 0x400-byte SECRSIF block RPC;
6. `DownloadGetKbit`;
7. `DownloadGetKc`;
8. fetch ICVPS2 when required;
9. report `DONE` / `FUNCTIONAL` only after every required stage succeeds.

The 0x400-byte limit belongs to the block RPC payload; it is not a blanket size limit for every BIT entry.

## Failed-GET_KBIT diagnostic record

The instrumented SECRMAN 1.4 emits a compact 16-byte record only when GET_KBIT fails. It distinguishes:

- first or second Mechacon pre-encryption half;
- first or second card-encryption half;
- CardAuth command 0x50/0x51/0x52/0x53;
- MCMAN callback result;
- SIO2 `stat6c`;
- card response ID/status;
- checksum state.

The record is placed in the failed Kbit reply buffer, which is otherwise unusable. `src/magicgate_diag.c` decodes it on EE. No diagnostic data is emitted on successful GET_KBIT.

## Result classification

Hardware validation established three important outcomes:

```text
FUNCTIONAL
```

The full RAM-only KELF path completed through Kbit and Kc.

```text
NOT SUPPORTED / NO CARD AUTH ACK
```

The known negative-control signature was observed on a card without functional MagicGate:

```text
stage       card half 0
command     0x50
pre         1/1
transfer    1
stat6c      0001D100
id/status   FF/FF
```

Because the same PS2SDK 2.0 backend passes known-good cards, this signature now means the card did not ACK the first CardAuth command rather than that Inspector selected the wrong SIO2 channel.

Other CardAuth failures remain `PROTOCOL ERROR / CARD AUTH`; Mechacon or infrastructure failures remain indeterminate instead of being mislabeled as unsupported MagicGate.

## Hardware validation matrix for 0.2.0

| Card | Filesystem | MagicGate result |
| --- | --- | --- |
| Sony 8 MB #1 | PASS | FUNCTIONAL |
| Sony 8 MB #2 | PASS | FUNCTIONAL |
| Third-party 64 MB, MagicGate-capable | PASS | FUNCTIONAL |
| Third-party 64 MB, no functional MagicGate | PASS | NOT SUPPORTED / NO CARD AUTH ACK |

The same positive results were reproduced with the PS2SDK 2.0 SECRMAN 1.4 backend before release. This proves the probe is testing functional CardAuth/KELF capability rather than Sony branding or card capacity.

## Environment restoration

After every MagicGate attempt:

1. the temporary KELF buffer is freed;
2. the IOP is reset;
3. Sony ROM `XSIO2MAN/XPADMAN/XMCMAN/XMCSERV` are reloaded;
4. real `mcInit(MC_TYPE_XMC)` resumes;
5. controller input is reopened;
6. the USB/FMCB package backend is reinitialized;
7. both card slots are inspected again.

A failure to restore the normal environment is fatal; Inspector asks for a reset/power-cycle instead of continuing with uncertain card state.

## Installation boundary

0.2.0 is not an FMCB installer. Package inspection is read-only and the MagicGate probe is RAM-only.

A future installation transaction must still add and validate:

```text
preflight
  -> backup/rollback preparation
  -> bind in RAM
  -> target write
  -> close/reopen
  -> read-back
  -> byte/metadata verification
  -> rollback on failure
```

Passing the MagicGate capability probe is a prerequisite for that work, not permission to skip write verification.
