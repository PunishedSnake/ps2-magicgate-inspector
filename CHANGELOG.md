# Changelog

All notable user-visible changes to PS2 Memory Card Inspector are documented here.

The project uses semantic version numbers and detective/police release codenames. Patch releases inherit the codename of their minor release.

## [Unreleased]

### Fixed

- corrected the IOP/libmc initialization stack discovered during the first real-hardware test;
- replaced the invalid combination of ordinary `mcman/mcserv` with `mcInit(MC_TYPE_XMC)` by the coherent ROM `XMCMAN/XMCSERV` + `MC_TYPE_XMC` stack;
- changed the IOP reset argument from an empty string to the canonical `NULL` form;
- added visible startup diagnostics for IOP reset/sync, every ROM module, `mcInit`, `padInit` and `padPortOpen`.

### Changed

- migrated the active build from PS2DEV v1.0 to **PS2DEV 2.0.0**;
- the CI toolchain now reports GCC 15.2.0 instead of the legacy GCC 3.2.3 environment;
- removed embedded `freesio2`, `freepad`, `mcman` and `mcserv` IRXs from the ELF;
- removed the local `DelayThread()` compatibility shim and forced compatibility header;
- the project now uses `rom0:XSIO2MAN`, `XPADMAN`, `XMCMAN` and `XMCSERV`.

### Validation

- PS2DEV 2.0.0 build: **passed**;
- static ELF verification: **passed**;
- corrected XMC stack on real PS2 hardware: **pending re-test**.

### Planned

- real-hardware re-validation of the corrected initialization path;
- refinements based on captured XMCMAN results from official and third-party cards;
- v0.2.0 **Briscoe** diagnostic/reporting work.

## [0.1.0] — Columbo

### Added

- standalone PS2 application producing `MC_INSPECTOR.ELF`;
- support for inspecting `mc0:` and `mc1:`;
- raw `mcGetInfo()` status, card type, format flag and free-cluster display;
- root-directory filesystem probe;
- deterministic 4 KiB write/read/compare integrity test;
- unique temporary filename selection that never intentionally overwrites an existing file;
- mandatory temporary-file deletion and post-delete verification;
- health classification for healthy, full, unformatted, filesystem, I/O, authentication, detection and no-card states;
- manual formatting for explicitly unformatted/no-format PS2 cards;
- destructive format confirmation requiring `L1 + R1 + Triangle`;
- PS2DEV CI build, ELF verification, SHA-256 generation and artifact upload;
- standalone architecture, testing, roadmap and release-codename documentation.

### Changed

- project direction moved from a FreeMcBoot installer patch to an independent PS2 diagnostic utility;
- old FMCB force-install and PCSX2 installer-harness code removed from the active source tree;
- authentication and detection errors are classified before `MC_TYPE_NONE` so useful failure information is not masked;
- exact temporary-file lookup accepts both zero results and `sceMcResNoEntry` as an unused candidate.

### Safety

- formatting is never automatic;
- generic I/O, authentication, detection and unknown-device failures do not unlock formatting;
- v0.1.0 does not install FreeMcBoot;
- v0.1.0 does not claim to certify MagicGate support.

### Validation

- initial PS2DEV build: **passed**;
- static ELF verification: **passed**;
- first real PS2 test: **failed during initialization due to the mixed MCMAN/XMC RPC stack**; corrected in the Unreleased development line.
