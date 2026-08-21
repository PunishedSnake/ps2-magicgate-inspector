# Architecture

## Purpose

PS2 Memory Card Inspector is a standalone EE application. It does not patch or embed itself into FreeMcBoot. The executable owns its IOP reset, memory-card RPC setup, controller input and text UI.

The architecture is intentionally conservative: inspection and repair are separate decisions, raw XMCMAN results are preserved for debugging, and destructive operations are gated behind both state checks and an explicit confirmation gesture.

## Runtime layout

The current build targets PS2DEV 2.0.0 and uses the Sony X-module stack from ROM rather than embedding replacement IRXs:

- `rom0:XSIO2MAN` — SIO2 transport used by pads and memory cards;
- `rom0:XPADMAN` — controller service;
- `rom0:XMCMAN` — memory-card manager;
- `rom0:XMCSERV` — EE/IOP memory-card RPC service.

At startup `InitIopAndDevices()`:

1. initializes SIF RPC;
2. resets the IOP with `SifIopReset(NULL, 0)`;
3. waits for IOP synchronization;
4. initializes LOADFILE;
5. loads the four ROM X modules in dependency order;
6. binds `libmc` with `mcInit(MC_TYPE_XMC)`;
7. initializes `libpad` and opens controller port 0.

Every startup stage is printed to the screen. If initialization stalls or fails on hardware, the last visible stage identifies the affected subsystem.

### Why the X stack matters

The first standalone development build loaded ordinary PS2SDK `mcman.irx`/`mcserv.irx` modules but called `mcInit(MC_TYPE_XMC)`. That combined different memory-card RPC protocols. On real hardware the IRXs themselves loaded successfully, but `libmc` then waited for an XMC server that was not present.

The current design keeps the client and server protocols coherent: `XMCMAN/XMCSERV` are always paired with `MC_TYPE_XMC`.

## Card inspection pipeline

`InspectCard(port)` is deliberately ordered from least invasive to most invasive:

1. **`mcGetInfo()`**
   - captures the raw return code;
   - records card type, formatted flag and free-cluster count;
   - classifies authentication and detection failures before considering the type field.
2. **Root-directory probe**
   - calls `mcGetDir("/*")`;
   - confirms that the filesystem can be traversed;
   - treats `sceMcResNoFormat` as a filesystem/format condition rather than a generic I/O failure.
3. **4 KiB integrity test**
   - finds a free `/__MCIxx.TMP` filename;
   - writes a deterministic 4096-byte pattern;
   - flushes and closes the file;
   - reopens and reads it;
   - compares all bytes;
   - deletes the file;
   - verifies that the file is absent.

A card is only reported as `PASS` when the full R/W/compare/delete path succeeds.

## Health model

The current health states are intentionally coarse and human-readable:

- `CARD_OK`
- `CARD_FULL`
- `CARD_UNFORMATTED`
- `CARD_FILESYSTEM_BROKEN`
- `CARD_IO_FAILURE`
- `CARD_AUTH_FAILURE`
- `CARD_DETECT_FAILURE`
- `CARD_NO_CARD`

The raw XMCMAN return codes remain visible on screen so future versions can refine classification without losing low-level diagnostic evidence.

## Why formatting is separate

Formatting is never an automatic recovery step. A failing R/W operation does not imply that reformatting is safe or useful.

The formatter is only exposed when:

- the reported device type is a PS2 memory card; and
- the card is explicitly unformatted, or filesystem probing returns `sceMcResNoFormat`.

Authentication errors, detection errors, arbitrary I/O errors and unknown device types do not unlock formatting.

The UI then requires a second destructive-action confirmation: hold `L1 + R1` and press `Triangle`.

## Temporary-file rules

The Inspector must never overwrite a user's existing file. `FindUnusedTempName()` searches `/__MCI00.TMP` through `/__MCI99.TMP`. A positive `mcGetDir()` result means a candidate already exists. `0` or `sceMcResNoEntry` means the exact candidate does not exist and may be used.

If all candidates exist, the test aborts instead of reusing one.

## Toolchain

The active build target is **PS2DEV 2.0.0**. The migration removed the local `DelayThread()` compatibility shim, forced include header, embedded IRX conversion rules and old module-buffer interception layer. The application now builds directly against the current PS2SDK interfaces supplied by that environment.

## Future boundaries

MagicGate/KELF qualification, richer card identity/fingerprint data, structured logs and recovery tools should be added as standalone Inspector modules. They should not reintroduce FreeMcBoot installation behavior into this repository.
