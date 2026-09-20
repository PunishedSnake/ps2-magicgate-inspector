# Architecture

PS2 Memory Card Inspector 0.4.0 "Drebin" uses explicit runtime personalities and
ownership boundaries. The design is a result of real-hardware failures: normal
filesystem access, raw card imaging, MagicGate and USB/recovery traffic cannot
all be treated as one interchangeable I/O environment.

## Core rules

1. Ordinary filesystem work uses the normal Sony ROM X card personality.
2. Raw-page Card Tools use a separate pinned raw MCMAN/MCSERV personality.
3. MagicGate/KELF work uses the isolated PS2SDK 2.0 SECRMAN 1.4 personality.
4. Runtime switches restore the normal personality before normal UI/file work.
5. fileXio block mode is forced synchronous in correctness-critical paths.
6. Long-lived `mass:` ownership is explicit; Drebin logging cannot interleave
   durable writes with installer/recovery ownership.
7. FMCB compatibility is capability/profile-driven, then verified by real KELF
   bind preflight.
8. No FMCB destination is modified until all distinct selected KELFs pass.
9. Card mutation remains under a durable transaction until commit.

## Runtime personalities

### Normal card/filesystem personality

```text
rom0:XSIO2MAN
rom0:XPADMAN
rom0:XMCMAN
rom0:XMCSERV
mcInit(MC_TYPE_XMC)
```

Used for filesystem diagnostics, installed-file operations and normal UI state.

### Security personality

```text
PS2SDK 2.0 freesio2/freepad/mcman
PS2SDK 2.0 SECRMAN 1.4
PS2SDK 2.0 SECRSIF
temporary MCSERV intentionally skipped
```

Used for KELF HEADER/BLOCK/Kbit/Kc/ICVPS2 operations.

libmc logical ports remain 0/1. Only SECR RPCs translate them to physical
SIO2 memory-card channels 2/3.

### Raw-card personality

A separately staged non-X MCMAN/MCSERV pair is used for raw-page image work.
This is kept separate from the normal XMC contract instead of trying to make
one module generation serve incompatible APIs.

## FMCB install flow

```text
package discovery
  -> runtime compatibility profile
  -> filesystem/free-space validation
  -> bind-preflight each distinct selected KELF
  -> restore normal stack after each security probe
  -> recovery journal + card identity
  -> capture/backup destinations
  -> bind source in RAM when required
  -> write
  -> close/reopen
  -> full read-back verification
  -> commit
     or
  -> verified rollback
```

Regional alias destinations use real file copies.

## Compatibility layer

`src/fmcb_compat.c` evaluates:

- ROM version/region;
- real DEX flag;
- PSX/DESR presence;
- MechaCon state;
- positive MechaPwn fingerprint/mode.

Static policy only decides what should be attempted. Unknown or modified
hardware still has to pass actual selected-KELF preflight.

See `docs/FMCB_COMPATIBILITY_CORPUS.md`.

## Recovery

Recovery v2 tracks:

- transaction state;
- target port/card identity;
- prepared destinations;
- backups;
- created regional/system directories.

The USB journal and card marker must agree before rollback mutates the card.

Old v1 journals are parsed with their original layout/checksum and converted
only after validation.

## Logger ownership

Real hardware reproduced Drebin text landing in recovery data and recovery bytes
landing in `DREBIN.LOG` when fileXio lifetime/ownership was insufficiently
isolated.

The current rule is:

```text
subsystem acquires mass ownership
  -> durable logger writes pause
  -> subsystem closes/syncs all mass traffic
  -> logger resumes
```

The logger also forces `FXIO_WAIT`, keeps per-line RAM hashes and flushes from
an aligned immutable snapshot.

## Settings lifecycle

GUI starts in safe Native mode.

After normal USB initialization:

```text
find MCINSPECTOR.CFG
  -> parse into a temporary settings copy
  -> validate version/known values
  -> commit settings
  -> apply saved display mode
```

A malformed config cannot partially alter runtime settings.

## Production optimization profile

0.4.0 keeps:

```text
-O2 -G0
-mtune=r5900 on measured hot objects
synchronous raw/image transport
fixed P0 production batches
R5900 fast-copy path
```

Benchmark-only R5900 objects, async candidates and USB speed-test matrices are
not part of the public release.
