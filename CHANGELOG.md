# Changelog

All notable changes to PS2 Memory Card Inspector are documented here.


## [0.4.1] — 2026-09-21 — "Drebin"

### USB/FMCB hotfix

- Fixed real-hardware USB FAT corruption during FMCB installation where
  diagnostic text could appear as malformed files/directories on `mass:`.
- Extended the logger's RAM-only mass-storage ownership scope across complete
  installer revalidation and transaction execution.
- Reused preflight-bound KELFs from EE RAM so no MagicGate IOP/USB rebuild is
  needed after the recovery journal becomes active.
- Removed unnecessary PADMAN loading from the temporary MagicGate IOP
  personality.
- Fixed the false initial Drebin RAM-ring checksum warning.
- Hardware re-test confirmed FMCB installation no longer corrupts the USB
  filesystem.
- Release packaging contains one production ELF only; no benchmark variants.

## [0.4.0] — 2026-09-20 — "Drebin"

### FreeMcBoot installer

- Promoted the project from read-only FMCB preflight to a verified cross-region installer.
- Added capability-driven CEX/DEX/MechaPwn compatibility policy.
- Added real regional I/A/E/C destination copies instead of legacy Multi-Install crosslinks.
- Added early-Japan OSDSYS/OSD110 and HDD-support package handling.
- Added preflight binding of every distinct selected KELF source before the first card mutation.
- Replaced opaque installer binding with stage-aware SECR HEADER/BLOCK/Kbit/Kc/ICVPS2 diagnostics.
- Added the hardware-qualified MechaPwn DEX exception that omits CEX-only ENDVDPL after its 128-byte KELF was reproduced failing at DOWNLOAD HEADER.
- Added explicit ROM 2.30+ native-boot classification and PSX/DESR compatibility classification.

### Transaction and recovery

- Added durable USB recovery journals and card identity markers.
- Added backup, write, close/reopen and full read-back verification for each installation destination.
- Added rollback of created/replaced destinations and directories.
- Added legacy recovery-journal v1 reader with checksum validation and in-RAM conversion to v2.
- Fixed recovery discovery for recursively located FMCB packages.
- Fixed post-security-session USB/card readiness handling before rollback.
- Fixed misleading rollback success reporting and recovery UI state.

### MagicGate / compatibility

- Retained the hardware-validated logical mc0/mc1 -> physical SIO2 2/3 mapping only at the SECR boundary.
- Confirmed successful FMCB installation and boot from a non-Sony card with functional MagicGate.
- Kept non-MagicGate third-party cards as valid ordinary storage while correctly rejecting CardAuth/KELF binding.
- Added a compatibility/exception corpus covering current MechaPwn, current FMCB, historical FMCB 1.8 behavior, current PS2SDK and representative forum/GitHub edge cases.

### Card Tools

- Added full-card image export, verification and exact restore.
- Added filesystem-aware image browsing and target-conflict inspection.
- Added selective save import/export and PSU transfer support.
- Added card/image/USB pickers and backup-aware force-format workflows.
- Added hot-swap-aware operation boundaries.

### Settings and UI

- Added Native, 480p, 576p, 720p and 1080i display modes.
- Added persistent versioned `MCI/MCINSPECTOR.CFG` settings on USB.
- Persisted display mode, filesystem-test profile, CNF preservation policy and installer verification mode.
- Added temporary-file/read-back/rename/sync config saves.
- Promoted the runtime banner to stable `v0.4.0 Drebin`.

### I/O correctness and diagnostics

- Fixed cross-file DREBIN/recovery corruption by serializing durable logger access against mass-storage transaction ownership.
- Forced synchronous fileXio semantics in correctness-critical logger/installer/recovery paths.
- Deferred logger path attachment while another subsystem owns `mass:`.
- Added recovery/log metadata sync boundaries.
- Added per-line RAM-ring checksums and aligned immutable flush snapshots for Drebin diagnostics.

### P0 production pass

- Kept `-O2 -G0` as the global optimization baseline.
- Retained `-mtune=r5900` on measured hot objects.
- Retained production P0 copy/streaming paths and fixed synchronous batch sizes.
- The public release does not include USB speed-test matrices, async/NOWAIT candidates, synthetic R5900 benchmark builds or Performance Lab binaries.

### Release engineering

- Simplified stable CI to one production ELF.
- Added FMCB compatibility and Settings-config invariant checks.
- Retained pinned PS2SDK 2.0 / SECRMAN 1.4 provenance.
- Release artifact contains the ELF, SHA-256, provenance, license, credits and third-party notices.

## [0.3.0-dev] — in development

### dev5 — page-scoped diagnostics and two-way navigation

- Changed slot navigation to `UP/DOWN` so controller movement matches the vertical `mc0:` / `mc1:` list on screen.
- Added `L1` for previous result page and kept `R1` for next result page.
- Changed plain `CROSS` to run only the diagnostic represented by the current page: filesystem, MagicGate/CardAuth or FMCB preflight.
- Added `L2 + CROSS` as the explicit complete selected-slot scan: filesystem integrity -> MagicGate/CardAuth -> FMCB package preflight.
- Removed automatic filesystem integrity tests at startup; reports begin in a neutral `UNKNOWN` / `NOT RUN` state.
- Removed the implicit 4 KiB filesystem re-test from MagicGate environment restoration so a MagicGate-only request stays MagicGate-only from the user's perspective.
- Kept `SQUARE`, normal-state `CIRCLE`, and `START` reserved for future installer/write actions.
- Updated the dashboard footer, README and development plan to describe the final diagnostic input model.
- Advanced the development banner to `0.3.0-dev5`.

### dev4 — unified selected-card scan

- Simplified diagnostics to a target-centric control model.
- `LEFT/RIGHT` selects `mc0:` or `mc1:`.
- `CROSS` runs the complete selected-card read-only sequence: filesystem integrity -> MagicGate/CardAuth -> FMCB package preflight.
- The currently visible result page no longer changes what `CROSS` does.
- `SQUARE`, normal-state `CIRCLE`, and `START` are reserved for future installer/write actions.
- Destructive formatting remains a separate armed action and `CIRCLE` remains its cancel control while confirmation is active.
- Updated dashboard footer and README controls to match the unified interaction model.
- Advanced the development banner to `0.3.0-dev4`.

### dev3 — live GS progress

- Added presentation-neutral progress callbacks for filesystem, MagicGate, FMCB preflight and IOP environment restoration.
- Added a native GS progress page with a real filled GS progress bar, percentage, current operation and detailed explanation.
- Tied progress to actual synchronous operation stages rather than timer animation.
- Added word-aware status wrapping and compact sidebar result labels.
- Real-hardware testing confirmed the progress UI and existing diagnostic mechanics remained functional.

### dev2 — GUI hardware-test fixes

- Moved long result text onto a dedicated row so it cannot overlap panel headings.
- Removed the remaining runtime libdebug MagicGate restore line from the GS framebuffer path.
- Shortened the filesystem activity sentence to avoid ugly right-edge wrapping.

### dev1 — native GS frontend

- Replaced the historical libdebug text dashboard with a native 640x224 FIELD GS frontend derived from the hardware-proven `fhdb-bootstrap-manager` 0.4.0 renderer architecture.
- Added double buffering, textured MSX 8x8 font rendering, Aqua-style panels and dedicated Card / MagicGate / FMCB Preflight pages.
- Preserved a single display personality with no video-mode selector.
- Split runtime control into `app_main.c` and presentation into `gui.c`.
- Preserved the hardware-validated PS2SDK 2.0 SECRMAN 1.4 MagicGate backend and Sony ROM X normal filesystem stack.

## [0.2.0] — 2026-08-22 — "Briscoe"

### MagicGate / KELF

- Added an isolated, RAM-only MagicGate/KELF capability probe using a raw user-supplied `FMCB.XLF` from `mass:/FMCB/SYSTEM/` or numbered `mass` devices.
- Ordinary memory-card inspection always returns to the hardware-validated Sony ROM X-module stack after a security-session attempt.
- Corrected BIT handling so SECRSIF's 0x400-byte block RPC limit applies only to entries marked for security download (`flags & 2`), not to large plaintext KELF entries.
- Added source-level failed-GET_KBIT instrumentation. On failure only, SECRMAN returns a compact record that distinguishes Mechacon preparation from first/second CardAuth halves and captures command, callback result, `stat6c`, response ID/status and checksum state.
- Removed post-failure authentication replay; diagnostics now observe the real CardAuth path in place.
- Corrected the critical SECR port-numbering bug. libmc logical ports 0/1 are translated to physical SIO2 memory-card channels 2/3 only for `DOWNLOAD_HEADER`, `GET_KBIT` and `GET_KC`.
- Added user-facing MagicGate classifications: `FUNCTIONAL`, `NOT SUPPORTED / NO CARD AUTH ACK`, other CardAuth protocol errors, and indeterminate Mechacon/session failures.
- Promoted **PS2SDK 2.0 SECRMAN 1.4** with matching SECRSIF/card modules to the single production security backend.
- Retired the legacy FreeMcBoot-compatible SECRMAN 1.3 build path from release plumbing after the PS2SDK 2.0 backend reproduced the same hardware results.

### Hardware validation

- Both tested official Sony 8 MB cards complete the full RAM-only binding path with `FUNCTIONAL`, including `DownloadHeader = 1`, required encrypted BIT processing, `Kbit = 1` and `Kc = 1`.
- A third-party 64 MB card with functional MagicGate/CardAuth also completes the probe with `FUNCTIONAL`.
- A third-party 64 MB card without functional MagicGate remains usable as ordinary PS2 storage but fails the first real CardAuth command (`0x50`) with `pre=1/1`, `tr=1`, `stat6c=0001D100`, `id=FF`, `st=FF`, producing `NOT SUPPORTED / NO CARD AUTH ACK`.
- The same positive/negative matrix was reproduced with the PS2SDK 2.0 SECRMAN 1.4 backend before release.
- These controls establish that the probe tests functional CardAuth/KELF capability rather than Sony branding, card capacity or a printed MagicGate logo.

### Investigation history preserved

- dev7 isolated a false failure caused by applying the 0x400 RPC limit to large plaintext BIT entries.
- dev8 reached the genuine GET_KBIT stage on both Sony cards.
- dev9/dev10 aligned the temporary card stack and discovered that starting temporary MCSERV could wedge the following LOADFILE RPC; the final session keeps MCMAN active but skips MCSERV.
- dev11 produced `F3: tr=1 stat6c=0001D100 id=FF st=FF`, then source review showed that F3 was an injected diagnostic command and not part of the real GET_KBIT CardAuth sequence.
- dev12 instrumented the real GET_KBIT path and showed `pre=1/1` followed by failure at card-encrypt half 0, command `0x50`, proving Mechacon preparation succeeded.
- Comparing that result with the reference FreeMcBoot binding path exposed the missing `2 + port` convention.
- dev13 applied the logical 0/1 -> physical 2/3 mapping and immediately produced full PASS on known-good cards.
- The later PS2SDK 2.0 / SECRMAN 1.4 comparison reproduced both positive and negative controls, allowing the legacy compatibility backend to be removed from the stable build.

### PS2SDK 2.0 / SECRMAN 1.4

- Pinned security source to PS2SDK commit `a13b5971ec0e39c7ba8b8559b80a4e81c8425352`.
- Source-builds PS2SDK 2.0 `secrman_special` IRX 1.4 and matching SECRSIF in CI.
- Uses the matching PS2SDK 2.0 `freesio2`, `freepad` and `mcman` generation for the isolated security session.
- Added deterministic `tools/patch_secrman14_diag.py` instrumentation to the temporary PS2SDK checkout.
- Fixed the first 1.4 instrumentation build issue by removing the now-unused private `scePreEncryptKbit()` helper/prototype so PS2SDK's `-Werror` policy remains clean.
- The first successful 1.4 comparison build was workflow #109; final 0.2.0 release checksums are generated independently by the release CI after cleanup.

### FMCB package preflight

- Added read-only FMCB package discovery and manifest validation from USB.
- Added console-region mapping and expected destination-folder reporting.
- Installation writes remain disabled in 0.2.0.
- The next write-capable milestone is a controlled bind -> write -> close/reopen -> read-back -> verify -> rollback transaction, not a general installer button.

### Project maintenance and release engineering

- Cleaned development-only backend selection and obsolete tracer code from the release build.
- Updated runtime labels to stable `v0.2.0 "Briscoe"`.
- Added/updated architecture, MagicGate, testing, building, backend provenance, FMCB package and roadmap documentation.
- Added MIT licensing for original Inspector source.
- Added explicit credits and third-party notices.
- Added the PS2SDK AFL-2.0 license text to the repository and release package.
- Release CI now records project and PS2SDK source revisions, packages notices/licenses and publishes an ELF SHA-256.

## [0.1.1-dev] — "Columbo"

- Migrated the standalone build to PS2DEV 2.0.0.
- Switched normal card access to the Sony ROM X-module stack (`XSIO2MAN`, `XPADMAN`, `XMCMAN`, `XMCSERV`).
- Fixed memory-card test open flags by using `FIO_O_*` instead of incompatible newlib `O_*` values.
- Added stage-specific read/write diagnostics and symbolic MCMAN result names.
- Real hardware confirmed full create/write/flush/reopen/read/compare/delete/cleanup PASS on both tested cards.

## [0.1.0] — "Columbo"

- Initial standalone PS2 Memory Card Inspector skeleton.
- Added card detection, root-directory check, destructive-safe 4 KiB temporary-file read/write test, and guarded format UI.
