# Changelog

All notable user-visible changes to PS2 Memory Card Inspector are documented here.

The project uses semantic version numbers and detective/police release codenames. Patch releases inherit the codename of their minor release.

## [Unreleased]

### Fixed

- corrected the IOP/libmc initialization stack discovered during the first real-hardware test;
- replaced the invalid combination of ordinary `mcman/mcserv` with `mcInit(MC_TYPE_XMC)` by the coherent ROM `XMCMAN/XMCSERV` + `MC_TYPE_XMC` stack;
- changed the IOP reset argument from an empty string to the canonical `NULL` form;
- corrected memory-card open modes from POSIX/newlib `O_*` values to the IOP-native `FIO_O_*` values expected by XMCMAN;
- eliminated the resulting false `sceMcResDeniedPermit (-5)` failures on healthy writable cards;
- added visible startup diagnostics for IOP reset/sync, every ROM module, `mcInit`, `padInit` and `padPortOpen`;
- added exact R/W stage reporting and symbolic XMCMAN/MCMAN error names;
- Briscoe MagicGate probing now uses raw `FMCB.XLF` instead of an already card-bound `osdmain.elf`;
- corrected BIT block validation so the 0x400-byte SECRSIF RPC limit applies only to encrypted/downloaded blocks, not large plaintext payload entries.

### Changed

- migrated the active build from PS2DEV v1.0 to **PS2DEV 2.0.0**;
- the CI toolchain now reports GCC 15.2.0 instead of the legacy GCC 3.2.3 environment;
- removed embedded `freesio2`, `freepad`, `mcman` and `mcserv` IRXs from the normal card path;
- removed the local `DelayThread()` compatibility shim and forced compatibility header;
- the project now uses `rom0:XSIO2MAN`, `XPADMAN`, `XMCMAN` and `XMCSERV` for normal card operation;
- Briscoe uses an isolated classic SECRMAN/SECRSIF 1.3 session for MagicGate/KELF binding probes.

### Validation

- PS2DEV 2.0.0 build: **passed**;
- static ELF verification: **passed**;
- corrected XMC startup stack on real PS2 hardware: **passed**;
- full 4 KiB create/write/flush/reopen/read/compare/delete test on both populated slots: **passed**;
- a PS2 card that is refused by the FreeMcBoot installer nevertheless reports normal PS2 type/format metadata, readable root filesystem and a complete Inspector R/W `PASS`;
- raw `FMCB.XLF` `DownloadHeader` succeeds on both tested official Sony cards;
- dev7 reached a two-block BIT and successfully processed the encrypted block before exposing an Inspector-side plaintext-block validation bug;
- dev8 fixes that validation bug and awaits hardware confirmation of Kbit/Kc completion.

### Planned

- complete standalone, non-destructive MagicGate/KELF diagnostics through Kbit/Kc/ICVPS2;
- compare the FMCB-rejected regression card against a known-working FMCB card using the same raw XLF;
- continue FMCB package preflight and transactional installer work only after the MagicGate backend is hardware-validated;
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
