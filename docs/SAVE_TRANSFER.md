# Drebin Save Transfer

Drebin's next Card Tools iteration separates full-card imaging from individual-save transfer.

Full-card imaging remains the hardware-qualified raw workflow. The new **Save Transfer** path is filesystem/container aware and is intended for moving individual saves between USB files, full memory-card images and physical cards without transplanting card geometry.

## Current implementation status

The implementation is deliberately landing in hardware-testable slices rather than replacing every Card Tools path at once.

Implemented on the Save Transfer development branch:

- Card Tools owns a local active-card context; `L1`/`R1` can switch `mc0`/`mc1` without returning to the dashboard;
- Image Browser can also switch the destination slot and refreshes free-space/conflict state before allowing new selections;
- a bounded, non-recursive USB file picker enumerates `mass:/`, `mass0:/` and `mass1:/`, sorts directories before files and filters supported image/save extensions;
- the legacy image lookup is intercepted so Browse/Restore and Exact Restore can choose an arbitrary `.ps2` or `.vmc` file rather than requiring a Drebin-generated basename;
- the image browser has a presentation-only `icon.sys` title resolver: the real filesystem directory name remains authoritative for conflict detection and restore, while a decoded title can be shown to the user;
- the first writable single-save backend, PSU, is implemented with container validation, conflict/free-space checks, metadata restoration, byte-for-byte post-write verification and rollback of objects created by the current import;
- PSU export writes the conventional 512-byte-entry/1024-byte-padded format, closes and syncs the USB output, then reopens it and byte-compares every exported file with the source card before reporting PASS.

Not yet exposed as the final Card Tools workflow:

- the unified **Import Save** and **Export Save** menu entries;
- MAX/PWS, CBS, SPS/XPS and PSV decoders;
- PS1 MCS/GME/PSX/PSV importers;
- full Shift-JIS rendering. Since 0.4.2 the title decoder walks the 68-byte Shift-JIS title correctly and folds full-width roman text to ASCII; kana/kanji are still skipped and the raw directory identifier is used when fewer than three ASCII characters survive.

A temporary integration detail remains while the old Card Tools controller is still present: cancelling the new image picker at its root returns an explicit cancellation code, but the legacy caller currently turns every negative lookup result into its old "image not found" warning. The Card Tools v2 controller must consume cancellation separately.

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

Browse/Restore must not depend on Drebin-generated names such as `mc0-00.vmc` or `mc0-00.ps2`.

The shared USB picker:

- enumerates directories and supported files without recursively scanning the whole drive;
- shows the current path;
- `UP/DOWN` moves;
- `X` opens a directory or selects a file;
- `CIRCLE` moves to parent/back;
- the active USB root can be changed between `mass:/`, `mass0:/` and `mass1:/`;
- files are filtered by extension for navigation and are still structurally/signature validated by the consumer before any destructive action.

The picker is shared by Image Browser, Exact Restore and Save Import. Full-card image verification remains unchanged after a file is selected.

## Unified format model

`src/save_transfer.h` defines a single format vocabulary. PS2 and PS1 containers may appear in the same picker, but they are not interchangeable and the inserted card type is validated before any write.

### Full PS2 images

- `.ps2` - 528-byte PCSX2 page layout;
- `.vmc` - 512-byte logical-page layout.

These remain image sources and are not treated as single-save archives.

### PS2 single-save import (0.4.2: all implemented)

| Container | Extensions | Notes |
| --- | --- | --- |
| EMS / uLaunchELF | `.psu` | streamed from USB, no RAM copy |
| Action Replay MAX / MAX Drive | `.max`, `.pws` | header CRC32 checked and reported; LZARI; no timestamps in the format, so the card assigns the creation time |
| CodeBreaker | `.cbs` | fixed-state RC4 + zlib (vendored `tinf`) |
| SharkPort / X-Port / Xploder | `.sps`, `.xps` | byte-swapped modes handled |
| PS3 export | `.psv` (type 2) | signature checked and reported; an unsigned or edited PSV still imports |

Detection is by content (`src/save_detect.c`). The extension only picks between
variants that share one layout (MAX vs PWS, SharkPort vs X-Port), so a
GameFAQs download renamed to `save.bin` is still recognised.

### PS1 saves and cards (0.4.2)

Single saves import to a **PS1 card** through mcman's PS1 file API:

| Container | Extensions |
| --- | --- |
| MCS (PSXGameEdit, MemcardRex) | `.mcs`, `.ps1` |
| Action Replay / GameShark / Xploder / Caetla | `.psx`, `.mcb`, `.mcx`, `.pda` |
| Raw save body (no header; name from file name) | `.bin`, none, e.g. `BESLES-01234GAME` |
| PS3 export | `.psv` (type 1, signature checked and reported) |

Whole PS1 cards can be imported (every save, all-or-nothing) or converted:

| Card container | Extensions |
| --- | --- |
| Raw 128 KiB | `.mcd`, `.mcr`, `.mc`, `.bin`, `.ddf`, `.psm`, `.mci`, `.srm`, POPStarter `.VMC` |
| DexDrive | `.gme`, `.dex` |
| VGS / bleem! | `.mem`, `.vgs` |
| PSP / Vita | `.vmp` (HMAC-SHA1 signed; signature checked on import, written on export) |
| MCX | `.mcx` (AES-128-CBC encrypted + SHA-256; decrypted and checked on import) |

PS1 rules:

- PS2 cards reject PS1 containers and PS1 cards reject PS2 containers before
  any write (the hot-swap probe settles the slot for the right card type).
- Free space on a PS1 card is counted in 8 KiB blocks (mcman `findfree1`).
- **mcman clears deleted-save records on a PS1 card whenever a save is
  created** (`ps1mc_fio.c`, O_CREAT path). Undeleting such saves in another
  tool is impossible afterwards; the confirmation screen says so.
- mcman's PS1 O_CREAT would also silently replace a save with the same name,
  so the importer checks every name first and refuses conflicts.
- A card image imports all of its saves or none: names and total blocks are
  checked before the first write; any failure deletes what was created.

### Convert on USB (0.4.2)

`CONVERT SAVE ON USB` never touches a memory card. After picking a file the
user chooses the output:

| Source | Outputs |
| --- | --- |
| any PS2 save | `.psu`, `.psv` (signed for PS3) |
| any PS1 single save | `.mcs`, `.psv` (signed for PS3), `.psx` (Action Replay) |
| any PS1 card | `.mcd`, `.gme`, `.vgs`, `.vmp` (signed for PSP/Vita), `.mcx` (encrypted) |

The output is written next to the source, re-read and compared; signed
outputs are verified after signing; an existing file is never overwritten.

### Signatures and encryption

`src/save_sign.c` implements the PSV/VMP HMAC-SHA1 signature and MCX
encryption on top of `src/mci_crypto.c` (AES-128, SHA-1, HMAC-SHA1). The
outputs are byte-identical to MemcardRex (VMP, MCX, PS1 PSV) and to Apollo
Save Tool's resigner (PS1 and PS2 PSV); see `tests/fixtures/oracle`.
Signature state is informational on import: saves edited by other tools are
often unsigned, so a bad signature is reported but does not block the import.

### Not yet supported

- PocketStation execution metadata and Shift-JIS kana/kanji rendering
  (titles fold to ASCII).
- A PS2 `.psv` is signed correctly but has not yet been tested on PS3
  hardware.
- Adding a PS1 save into a POPStarter `.VMC` in place (convert to `.mcd`
  works; in-place insertion is the next step).

## Default single-save export

Drebin's normal PS2 single-save export format is **PSU**.

Reasons:

- uncompressed and simple;
- preserves directory/file metadata needed for faithful restoration;
- widely supported by uLaunchELF, mymc, Apollo and desktop converters;
- no CodeBreaker encryption or MAX/LZARI dependency;
- suitable as Drebin's archival/interchange format even when import supports many historical containers.

Additional export formats can be added later, but the normal one-button export should stay PSU unless a concrete interoperability reason justifies another format.

The current PSU backend is intentionally fail-closed. It accepts the conventional flat PS2 save-directory form and refuses to silently discard unexpected nested directory objects.

## Human-readable save names

The image browser must not expose raw directory names such as `BASLUS-21004HIFU` as the only user-facing identity when the save contains a usable title.

The first metadata source is the save's `icon.sys` file. PS2SDK documents the `mcIcon` structure and its 34-character Shift-JIS title field. Drebin reads `icon.sys` from the save directory/image, validates the `PS2D` header and decodes a representable title for display.

Display policy:

1. decoded `icon.sys` title when valid and representable;
2. optional later heuristics from other small save metadata files;
3. raw directory name as a guaranteed fallback.

The raw directory name is always retained separately because it is the real filesystem object used for conflict detection and restore. Presentation code must never rewrite the identifier used by the transaction engine.

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

The persistent diagnostic logger follows the same USB isolation rule as full-card imaging: while a save archive is held open for long-form read/write/verification, durable `DREBIN.LOG` writes are paused and trace lines remain in EE RAM until the archive descriptor is closed. This prevents recurrence of the hardware-proven USBHDFSD cross-file corruption bug.

## Implementation sources and licensing

Useful public references include:

- mymc (Ross Ridge, public domain) `ps2save.py` for PSU/MAX/SPS/CBS layouts and the CodeBreaker RC4 state; mymc++ is used only as an external test oracle (`tools/gen_save_fixtures.py`), never linked;
- Okumura's LZARI (1989, public domain) for the MAX decompressor;
- `tinf` 1.2.1 by Jørgen Ibsen (zlib licence, `src/tinf/LICENSE.txt`) for CodeBreaker inflate;
- psdevwiki and mymc++ `format_psv` for the PSV header layout;
- Martin Korth's nocash PSX specs for the PS1 card filesystem, and MemcardRex for GME/VGS/VMP header facts (no MemcardRex code is used);
- PS2SDK mcman `ps1mc_fio.c` / `mcserv.c` for PS1 card behaviour on PS2;
- Apollo Save Tool PS2 `source/import_ps2.c` for native PS2 implementations of MAX, CBS, XPS, PSU and PSV workflows;
- CheatDevicePS2 / PS2SaveUtility for MAX/CBS/PSU/XPS container definitions;
- ps2dev/mymc for documented PSU/MAX/SPS/CBS parsing and PS2 memory-card filesystem behavior;
- PS2SDK `libmc.h` for `mcIcon` / `icon.sys` layout.

Code copied or adapted into Drebin must retain compatible licensing/attribution as required. Prefer original reimplementation from documented structures and externally observable container behaviour where this keeps Drebin's licensing and safety boundaries simple.
