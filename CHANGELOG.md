# Changelog

All notable changes to PS2 Memory Card Inspector are documented here.

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
