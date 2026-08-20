# Changelog

All notable user-visible changes to PS2 Memory Card Inspector are documented here.

The project uses semantic version numbers and detective/police release codenames. Patch releases inherit the codename of their minor release.

## [Unreleased]

### Planned

- real-hardware validation of v0.1.0 behavior;
- refinements based on captured MCMAN results from official and third-party cards;
- v0.2.0 **Briscoe** diagnostic/reporting work.

## [0.1.0] — Columbo

### Added

- standalone PS2 application producing `MC_INSPECTOR.ELF`;
- embedded `freesio2`, `freepad`, `mcman` and `mcserv` IOP modules;
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

- PS2DEV build: **passed**;
- static ELF verification: **passed**;
- real PS2 hardware validation: **pending**.
