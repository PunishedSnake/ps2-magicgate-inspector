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
- corrected the Briscoe MagicGate probe so it no longer attempts to re-bind an already-bound `B?EXEC-SYSTEM/osdmain.elf` from a memory card;
- MagicGate bind testing now requires a raw user-supplied `mass:/FMCB/SYSTEM/FMCB.XLF` (or mass0/mass1 equivalent), matching the input used by the FMCB installer before it creates `osdmain.elf`.

### Changed

- migrated the active build from PS2DEV v1.0 to **PS2DEV 2.0.0**;
- the CI toolchain now reports GCC 15.2.0 instead of the legacy GCC 3.2.3 environment;
- removed embedded `freesio2`, `freepad`, `mcman` and `mcserv` IRXs from the normal application personality;
- removed the local `DelayThread()` compatibility shim and forced compatibility header;
- the normal application uses `rom0:XSIO2MAN`, `XPADMAN`, `XMCMAN` and `XMCSERV`;
- Briscoe runs MagicGate/KELF work in an isolated security session and restores the normal card stack afterwards;
- the isolated security session uses the classic FreeMcBoot Installer `SECRMAN 1.3 + SECRSIF 1.3` compatibility pair;
- `sceMcResChangedCard (-1)` after the security-session reboot is treated as a transient state notification rather than an authentication failure.

### Validation

- PS2DEV 2.0.0 build: **passed**;
- static ELF verification: **passed**;
- corrected XMC startup stack on real PS2 hardware: **passed**;
- full 4 KiB create/write/flush/reopen/read/compare/delete test on both populated official Sony slots: **passed**;
- a PS2 card that is refused by the FreeMcBoot installer nevertheless reports normal PS2 type/format metadata, readable root filesystem and a complete Inspector R/W `PASS`;
- a second official Sony card with a working FMCB installation also passes the normal card path;
- Briscoe dev6 advanced both cards through security-session card detection and SECR RPC binding, but both returned `HEADER BIND FAILED` because the probe used the same already-bound `mc1:/BIEXEC-SYSTEM/osdmain.elf` as its input;
- that dev6 header result is explicitly **not** considered a valid MagicGate verdict;
- dev7 changes the comparison to use one identical raw `FMCB.XLF` from USB against both target cards.

### Planned

- hardware-test the raw-XLF dev7 probe against the FMCB-rejected card and the known-working FMCB card;
- continue through DownloadHeader/BIT block/Kbit/Kc/ICVPS2 classification once the raw input is validated;
- detect or at least report CEX/DEX/MechaPWN-relevant console security state where practical;
- finish read-only FMCB package preflight and only then design transactional installation writes;
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
