# Changelog

All notable changes to PS2 Memory Card Inspector are documented here.

## [Unreleased] — v0.2.0 "Briscoe"

### MagicGate / KELF diagnostics

- Added isolated, RAM-only MagicGate/KELF probing using a raw user-supplied `FMCB.XLF` from `mass:/FMCB/SYSTEM/` (also probes `mass0:` / `mass1:`).
- Ordinary memory-card inspection always returns to the hardware-validated Sony ROM X-module stack after a security-session attempt.
- Pinned the classic FreeMcBoot Installer `SECRMAN 1.3 + SECRSIF 1.3` compatibility pair for controlled hardware testing.
- `sceMcResChangedCard (-1)` is treated as a transient post-IOP-reboot state rather than a fatal card error when valid PS2 metadata is present.
- Corrected BIT block validation: the SECRSIF `0x400` RPC limit now applies only to blocks marked for security download (`flags & 2`), not to large plaintext payload blocks.
- dev12 replaces the dev11 post-failure CardAuth replay with source-level instrumentation of the exact pinned FMCB `SecrDownloadGetKbit()` path. Successful SECRMAN behavior is unchanged; only a failed Kbit call replaces the returned 16-byte Kbit buffer with a compact diagnostic record.
- The dev12 record distinguishes the two Mechacon Kbit pre-encryption calls from first/second-half card encryption and captures the exact CardAuth command, MCMAN callback return, `stat6c`, response ID/status, checksum state, and whether each pre-encrypted half was non-zero.

### Hardware findings

- dev7: both tested official Sony cards passed filesystem inspection, `mcInit`, `mcGetInfo`, SECR RPC, `DownloadHeader`, and the encrypted BIT block. A false failure on BIT block 1 was traced to the Inspector's over-broad `0x400` size guard.
- dev8: after fixing BIT semantics, both cards again behaved identically and advanced to `GET KBIT`; both returned `Kbit = 0`. One card is rejected by the stock FMCB Installer and the other already boots FMCB, so this is not evidence of a card-specific MagicGate defect.
- `SecrDownloadGetKbit()` is the first tested stage that invokes SECRMAN `card_encrypt()` and therefore depends on MCMAN having registered compatible `mcCommand`/device-ID callbacks.
- FreeMcBoot Installer's own CI builds in `ps2dev/ps2dev:v1.0`. dev9 therefore keeps the EE application on PS2DEV 2.0 but stages the isolated MagicGate IOP personality from PS2SDK v1.0 `freesio2/freepad/mcman/mcserv` plus the pinned FMCB SECRMAN/SECRSIF 1.3 pair.
- dev10 confirmed that loading the temporary PS2SDK-v1 MCSERV is unnecessary for the isolated security probe and can wedge the next LOADFILE RPC; the session therefore keeps MCMAN active and bypasses only the temporary EE-side MCSERV/libmc sanity path.
- dev11 hardware trace returned `F3: tr=1 stat6c=0001D100 id=FF st=FF`. `stat6c` indicates RX FIFO error plus missing ACK on queue slot 0. Review of the pinned FMCB SECRMAN then showed that this F3 reset was an extra diagnostic command: the real `SecrDownloadGetKbit()` path starts directly with Mechacon pre-encryption followed by `F2/50 -> 51 -> 52 -> 53`. Because dev11 stopped at the extra F3, it did not test the actual failing CardAuth path.
- dev12 removes that false prerequisite entirely and instruments the original SECRMAN function in-place, so the next hardware result identifies the real failure without altering the command sequence.

### FMCB package preflight

- Added optional read-only `mass:` package discovery and manifest validation.
- Installation writes remain disabled while the MagicGate backend is under hardware validation.

## [0.1.1-dev] — "Columbo"

- Migrated the standalone build to PS2DEV 2.0.0.
- Switched normal card access to the Sony ROM X-module stack (`XSIO2MAN`, `XPADMAN`, `XMCMAN`, `XMCSERV`).
- Fixed memory-card test open flags by using `FIO_O_*` instead of incompatible newlib `O_*` values.
- Added stage-specific read/write diagnostics and symbolic MCMAN result names.
- Real hardware confirmed full create/write/flush/reopen/read/compare/delete/cleanup PASS on both tested cards.

## [0.1.0] — "Columbo"

- Initial standalone PS2 Memory Card Inspector skeleton.
- Added card detection, root-directory check, destructive-safe 4 KiB temporary-file read/write test, and guarded format UI.
