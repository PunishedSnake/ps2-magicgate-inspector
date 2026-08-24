# Drebin Save Transfer

Drebin's next Card Tools iteration separates full-card imaging from individual-save transfer.

Full-card imaging remains the hardware-qualified raw workflow. The new **Save Transfer** path is filesystem/container aware and is intended for moving individual saves between USB files, full memory-card images and physical cards without transplanting card geometry.

## Local card selection

Card Tools must not inherit an immutable target from the dashboard.

Every Card Tools screen owns an explicit active-card context. `L1`/`R1` switches between `mc0` and `mc1` without returning to the dashboard. The currently active slot is shown in the identity banner and in the operation summary. When Card Tools exits, its active slot becomes the dashboard selection so the two interfaces cannot disagree.

The role of the selected card depends on the operation:

- full-card backup: active card is **SOURCE**;
- single-save export: active card is **SOURCE**;
- single-save import: active card is **DESTINATION**;
- image Browse/Restore: USB image is **SOURCE**, active card is **DESTINATION**;
- exact image restore: USB image is **SOURCE**, active card is **DESTINATION**;
- force format: active card is **DESTINATION**, and the confirmation screen repeats that slot immediately before the destructive operation.

Switching destination cards inside a save/image browser must refresh free-space and conflict state and clear selections that no longer fit. It must never silently carry stale conflict information from the previous slot.

## One file picker, not "latest image"

Browse/Restore must stop depending on Drebin-generated names such as `mc0-00.vmc` or `mc0-00.ps2`.

A primitive USB file picker is sufficient and preferable:

- enumerate directories and supported files;
- show the current path;
- `UP/DOWN` moves;
- `X` opens a directory or selects a file;
- `CIRCLE` moves to parent/back;
- `L1/R1` changes the destination card where the operation has one;
- files are identified by extension plus signature/structure validation, not by a required basename.

The picker is shared by Image Browser, Exact Restore and Save Import.

## Unified format model

`src/save_transfer.h` defines a single format vocabulary. PS2 and PS1 containers may appear in the same picker, but they are not interchangeable and the inserted card type is validated before any write.

### Full PS2 images

- `.ps2` - 528-byte PCSX2 page layout;
- `.vmc` - 512-byte logical-page layout.

These remain image sources and are not treated as single-save archives.

### PS2 single-save import

Initial target set:

- `.psu` - EMS/uLaunchELF;
- `.max` / `.pws` - Action Replay MAX / MAX Drive;
- `.cbs` - CodeBreaker;
- `.sps` - SharkPort;
- `.xps` - X-Port/Xploder;
- `.psv` - PS3-exported PS2 save.

Reference implementations are taken from public PS2 tooling, especially Apollo Save Tool / CheatDevicePS2 and mymc, then adapted to Drebin's fail-closed transaction model. Proprietary containers are decoded into one internal directory/files/metadata representation before anything is written to a card.

### PS1 formats in the same picker

`.mcs` and DexDrive `.gme` are PS1 formats, not PS2 save containers. They may share the picker, but only a PS1 destination card may accept them. The same applies to PS1 `.psx` and the PS1 variant of `.psv`.

A PS2 card must reject these before creating a destination directory. Likewise a PS1 card must reject PS2 PSU/MAX/CBS/SPS/XPS payloads.

## Default single-save export

Drebin's normal PS2 single-save export format is **PSU**.

Reasons:

- uncompressed and simple;
- preserves directory/file metadata needed for faithful restoration;
- widely supported by uLaunchELF, mymc, Apollo and desktop converters;
- no CodeBreaker encryption or MAX/LZARI dependency;
- suitable as Drebin's archival/interchange format even when import supports many historical containers.

Additional export formats can be added later, but the normal one-button export should stay PSU unless a concrete interoperability reason justifies another format.

## Human-readable save names

The image browser currently exposes raw directory names such as `BASLUS-21004HIFU`. Those names remain important identifiers, but should not be the primary display label.

The first metadata source is the save's `icon.sys` file. PS2SDK documents the `mcIcon` structure and its 34-character Shift-JIS title field. Drebin should read `icon.sys` from the save directory/image, validate the `PS2D` header and decode the title for display.

Display policy:

1. decoded `icon.sys` title when valid;
2. optional later heuristics from other small save metadata files;
3. raw directory name as a guaranteed fallback.

The raw directory name is always retained in a secondary field because it is the real filesystem object used for conflict detection and restore.

## Import safety contract

Every container importer follows the same sequence:

```text
pick file
  -> identify/validate container
  -> decode into internal save package in RAM/streaming state
  -> validate destination card generation
  -> derive destination directory and required clusters
  -> check conflict/free space
  -> create transaction
  -> create directory/files
  -> restore metadata
  -> reopen every written file and byte-compare
  -> commit
```

On failure, only objects created by the current transaction are rolled back. Existing saves are not overwritten by the initial implementation. Replace/merge policy remains a separate future conflict workflow.

## Implementation sources and licensing

Useful public references include:

- Apollo Save Tool PS2 `source/import_ps2.c` for native PS2 implementations of MAX, CBS, XPS, PSU and PSV workflows;
- CheatDevicePS2 / PS2SaveUtility for MAX/CBS/PSU/XPS container definitions;
- ps2dev/mymc for documented PSU/MAX/SPS/CBS parsing and PS2 memory-card filesystem behavior;
- PS2SDK `libmc.h` for `mcIcon` / `icon.sys` layout.

Code copied or adapted into Drebin must retain compatible licensing/attribution as required. Prefer reimplementation from documented structures where this keeps Drebin's licensing and safety boundaries simple.
