# Architecture

PS2 Memory Card Inspector deliberately separates ordinary memory-card I/O from experimental/security-sensitive MagicGate work. That separation is a direct result of real-hardware regressions found during Briscoe development.

## Design goals

1. Keep the ordinary filesystem inspector on a known-good card stack.
2. Treat MagicGate/KELF work as an isolated session that may fail without poisoning the rest of the application.
3. Preserve the test KELF in EE RAM across the IOP reboot.
4. Never write a bound KELF to a card during the capability probe.
5. Make low-level failures observable without replaying extra authentication commands.
6. Restore the normal ROM stack after every security-session attempt.
7. Keep security backend versions reproducible and explicitly selectable.

## High-level flow

```text
startup
  |
  v
normal ROM X card personality
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
         +--> reboot IOP with selected SECRMAN
         +--> load matching temporary SIO2/PAD/MCMAN stack
         +--> skip temporary MCSERV
         +--> load matching SECRSIF
         +--> run RAM-only KELF binding probe
         +--> discard RAM KELF
         +--> reboot/rebuild normal ROM X card personality
         +--> reopen USB/pad clients
         +--> re-inspect both memory-card slots
         |
         v
       UI
```

## Normal application personality

The normal personality uses modules from the console ROM:

```text
rom0:XSIO2MAN
rom0:XPADMAN
rom0:XMCMAN
rom0:XMCSERV
```

followed by:

```c
mcInit(MC_TYPE_XMC)
```

This is the permanent application personality because it has passed real-hardware filesystem testing on the development console and cards.

No experimental SECRMAN replacement remains resident after a MagicGate probe.

## Why the MagicGate personality is isolated

An earlier Briscoe experiment replaced the normal card personality globally with a security-oriented stack. Real hardware then returned memory-card authentication-reset failures before the actual KELF test even ran.

The architectural fix was to isolate the security stack rather than hide the error.

A security-session failure is therefore allowed to produce diagnostics, but the application must still reconstruct the normal ROM stack before returning to ordinary card operations.

## Preparing the test KELF

The raw KELF is located while the normal filesystem environment is still active:

```text
mass:/FMCB/SYSTEM/FMCB.XLF
mass0:/FMCB/SYSTEM/FMCB.XLF
mass1:/FMCB/SYSTEM/FMCB.XLF
```

The file is read into aligned EE memory and validated before any IOP reboot.

An installed `mc?:/B?EXEC-SYSTEM/osdmain.elf` is deliberately not used as test input because it is already card-bound. The probe needs a raw, unbound source KELF.

## Temporary IOPRP

The Inspector generates a minimal IOPRP image in EE RAM with the selected SECRMAN embedded. It then uses `SifIopRebootBuffer()` to start the isolated security personality.

The selected build profile determines which temporary security/card modules are embedded in the standalone ELF:

### `fmcb13`

```text
SECRMAN compatibility source from pinned FreeMcBoot Installer revision
matching SECRSIF 1.3 compatibility bridge
PS2SDK-v1-era freesio2/freepad/mcman
```

### `ps2sdk14`

```text
PS2SDK 2.0 SECRMAN 1.4
PS2SDK 2.0 SECRSIF
PS2SDK 2.0 freesio2/freepad/mcman
```

See `SECURITY_BACKENDS.md` for exact revisions and hashes.

## Why temporary MCSERV is skipped

Real-hardware testing showed that loading the temporary PS2SDK-v1 MCSERV could return resident successfully and still leave the following LOADFILE RPC stuck.

The MagicGate probe itself needs MCMAN's SECRMAN callback registration, not ordinary EE file-service traffic during the isolated phase. Therefore:

- temporary MCMAN remains active;
- temporary MCSERV is intercepted and skipped;
- the immediate EE-side `mcInit/mcGetInfo/mcSync` sanity query is emulated;
- all normal libmc behavior resumes after the ROM X environment is rebuilt.

This behavior lives in `src/magicgate_session.c`.

## Logical vs physical card ports

This is a non-obvious architectural requirement and must not be “simplified” away.

libmc exposes memory cards as logical ports:

```text
mc0 -> 0
mc1 -> 1
```

SECRMAN CardAuth uses the supplied number directly as an SIO2 channel. The physical memory-card channels are:

```text
mc0 -> 2
mc1 -> 3
```

The reference FMCB installer expresses that convention as `2 + port` when entering SECRMAN.

The Inspector keeps all normal libmc code on `0/1`, then translates only SECRSIF RPC requests containing a card port:

```text
DOWNLOAD_HEADER
GET_KBIT
GET_KC
```

The translation is implemented in `src/magicgate_diag.c` because that file already wraps the relevant SECRSIF RPC calls.

## KELF probe stages

`src/magicgate.c` performs the high-level security transaction:

1. bind SECRSIF RPC clients;
2. `DownloadHeader`;
3. parse returned BIT metadata;
4. send only BIT entries marked for security download (`flags & 2`);
5. advance over large plaintext BIT entries without forcing them through the `0x400` SECRSIF block buffer;
6. `DownloadGetKbit`;
7. `DownloadGetKc`;
8. fetch ICVPS2 when required;
9. report `DONE`/`FUNCTIONAL` only after all required stages succeed.

The SECRSIF block-size limit applies to the block RPC payload, not to every KELF BIT entry.

## Failure instrumentation

A failed GET_KBIT needs enough detail to distinguish:

- Mechacon/pre-encryption failure;
- absent MCMAN callback;
- SIO2 failure;
- response ID/status failure;
- checksum failure;
- failure in first versus second Kbit half.

Instead of replaying authentication commands after the fact, the selected SECRMAN source is instrumented at build time. On GET_KBIT failure only, it serializes a compact record into the existing 16-byte Kbit reply buffer.

`src/magicgate_diag.c` decodes and classifies that record on EE.

No diagnostic record is emitted on success, so successful SECRMAN behavior is kept as close to upstream as practical.

## MagicGate capability classification

The known negative-control signature is:

```text
stage       card half 0
command     F2/50
pre         1/1
transfer    1
stat6c      0001D100
id/status   FF/FF
```

With a backend already proven to pass other cards, that condition means the card did not provide a valid ACK to the first CardAuth command and is presented as:

```text
NOT SUPPORTED / NO CARD AUTH ACK
```

Other CardAuth failures remain protocol errors rather than being overgeneralized as “no MagicGate”.

## Environment restoration

After every MagicGate attempt:

1. the temporary KELF buffer is freed;
2. the IOP is reset;
3. normal ROM `XSIO2MAN/XPADMAN/XMCMAN/XMCSERV` are reloaded;
4. real `mcInit(MC_TYPE_XMC)` resumes;
5. controller input is reopened;
6. USB/FMCB package backend is reinitialized;
7. both card slots are inspected again.

A failure to restore the normal environment is treated as fatal and the application asks for a reset/power-cycle rather than continuing with uncertain card state.

## Installation boundary

The current architecture intentionally stops before card installation writes.

A future installer path must be a separate transaction with:

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

Passing the RAM-only MagicGate probe is a prerequisite, not permission to skip installation verification.
