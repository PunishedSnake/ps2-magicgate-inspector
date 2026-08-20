# Architecture

## Purpose

PS2 Memory Card Inspector is a standalone EE application. It does not patch or embed itself into FreeMcBoot. The executable owns its IOP initialization, memory-card RPC setup, controller input and text UI.

The architecture is intentionally conservative: inspection and repair are separate decisions, raw MCMAN results are preserved for debugging, and destructive operations are gated behind both state checks and an explicit confirmation gesture.

## Runtime layout

The build embeds four PS2SDK IOP modules into the ELF:

- `freesio2.irx` — SIO2 transport used by pads and memory cards;
- `freepad.irx` — controller support;
- `mcman.irx` — memory-card manager;
- `mcserv.irx` — EE/IOP memory-card RPC service.

At startup `InitIopAndDevices()` resets and synchronizes the IOP, enables module-buffer loading, starts the embedded IRXs, initializes `libmc`, and opens pad port 0.

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

The raw MCMAN return codes remain visible on screen so future versions can refine classification without losing low-level diagnostic evidence.

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

## Compatibility shim

`src/compat.c` contains a local `DelayThread()` implementation because the PS2DEV v1.0 environment used for the initial build does not export the newer helper expected by later code. The shim is only used to throttle the UI loop; it is not part of any memory-card protocol timing.

## Future boundaries

MagicGate/KELF qualification, richer card identity/fingerprint data, structured logs and recovery tools should be added as standalone Inspector modules. They should not reintroduce FreeMcBoot installation behavior into this repository.
