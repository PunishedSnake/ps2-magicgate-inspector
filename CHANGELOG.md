# Changelog

All notable changes to PS2 Memory Card Inspector are documented here.

## [Unreleased] — v0.2.0 "Briscoe"

### MagicGate / KELF diagnostics

- Added isolated, RAM-only MagicGate/KELF probing using a raw user-supplied `FMCB.XLF` from `mass:/FMCB/SYSTEM/` (also probes `mass0:` / `mass1:`).
- Ordinary memory-card inspection always returns to the hardware-validated Sony ROM X-module stack after a security-session attempt.
- Pinned the classic FreeMcBoot Installer `SECRMAN 1.3 + SECRSIF 1.3` compatibility pair for controlled hardware testing.
- `sceMcResChangedCard (-1)` is treated as a transient post-IOP-reboot state rather than a fatal card error when valid PS2 metadata is present.
- Corrected BIT block validation: the SECRSIF `0x400` RPC limit now applies only to blocks marked for security download (`flags & 2`), not to large plaintext payload blocks.
- Source-level GET_KBIT instrumentation preserves successful SECRMAN behavior and, on failure only, returns a compact diagnostic record through the existing 16-byte Kbit reply. The record distinguishes Mechacon preparation from first/second-half CardAuth and captures the exact command, callback return, `stat6c`, response ID/status and checksum state.
- Corrected SECR port numbering at the SECRSIF RPC boundary. The reference FreeMcBoot Installer calls `SecrDownloadFile(2 + port, slot, ...)`, because CardAuth consumes physical SIO2 memory-card channels 2/3 directly. Inspector had passed logical libmc ports 0/1, which targeted controller channels and produced missing-ACK failures. HEADER, GET_KBIT and GET_KC now map logical 0/1 to physical 2/3 while normal libmc access remains unchanged.
- Production cleanup removes the obsolete dev11 replay tracer and dev-numbered runtime shims. The retained MagicGate diagnostic bridge classifies the validated first-command `F2/50` RX/no-ACK signature as `NOT SUPPORTED / NO CARD AUTH ACK`, while Mechacon failures remain indeterminate and other CardAuth failures are reported as protocol errors.
- Build plumbing now supports two explicitly selectable security profiles: `fmcb13` (the hardware-validated FMCB SECRMAN/SECRSIF 1.3 + PS2SDK-v1 card stack) and `ps2sdk14` (an experimental source-built PS2SDK 2.0 SECRMAN 1.4 + SECRSIF + matching PS2SDK 2.0 card stack). Both profiles use identical EE-side port mapping and the same diagnostic record format for direct comparison. The 1.4 profile has been prepared but not built or hardware-tested yet.

### Hardware findings

- dev7: both tested official Sony cards passed filesystem inspection, `mcInit`, `mcGetInfo`, SECR RPC, `DownloadHeader`, and the encrypted BIT block. A false failure on BIT block 1 was traced to the Inspector's over-broad `0x400` size guard.
- dev8: after fixing BIT semantics, both cards again behaved identically and advanced to `GET KBIT`; both returned `Kbit = 0`. One card is rejected by the stock FMCB Installer and one already boots FMCB, so this is not evidence of a card-specific MagicGate defect.
- `SecrDownloadGetKbit()` is the first tested stage that invokes SECRMAN `card_encrypt()` and therefore depends on MCMAN having registered compatible `mcCommand`/device-ID callbacks.
- FreeMcBoot Installer's own CI builds in `ps2dev/ps2dev:v1.0`. dev9 therefore keeps the EE application on PS2DEV 2.0 but stages the isolated MagicGate IOP personality from PS2SDK v1.0 `freesio2/freepad/mcman/mcserv` plus the pinned FMCB SECRMAN/SECRSIF 1.3 pair.
- dev10 confirmed that loading the temporary PS2SDK-v1 MCSERV is unnecessary for the isolated security probe and can wedge the next LOADFILE RPC; the session therefore keeps MCMAN active and bypasses only the temporary EE-side MCSERV/libmc sanity path.
- dev11 hardware trace returned `F3: tr=1 stat6c=0001D100 id=FF st=FF`. `stat6c` indicates RX FIFO error plus missing ACK on queue slot 0. Review of the pinned FMCB SECRMAN then showed that this F3 reset was an extra diagnostic command: the real `SecrDownloadGetKbit()` path starts directly with Mechacon pre-encryption followed by `F2/50 -> 51 -> 52 -> 53`. Because dev11 stopped at the extra F3, it did not test the actual failing CardAuth path.
- dev12 removes that false prerequisite entirely and instruments the original SECRMAN function in-place. Both tested official Sony cards produced the same real failure: `pre=1/1`, then `CARD ENCRYPT HALF 0`, command `0x50`, `tr=1 stat6c=0001D100 id=FF st=FF`. This proves both Mechacon Kbit halves were prepared successfully and the first real CardAuth command was sent to a non-responding SIO2 channel.
- Comparing that result with the reference FMCB installer exposed the caller bug: FMCB signs with `SecrDownloadFile(2 + port, slot, ...)`, while Inspector used `port` directly. Since CardAuth writes `port_ctrl1[port]` and `(port & 3)` into SIO2 regdata, Inspector's 0/1 selected controller channels instead of memory-card channels 2/3. dev13 applies the correct mapping.
- dev13 hardware validation: both tested official Sony 8 MB cards now complete the entire RAM-only MagicGate probe with `PASS`, including `DownloadHeader = 1`, the encrypted BIT block, `Kbit = 1` and `Kc = 1`.
- A third-party 64 MB card without MagicGate support remains filesystem-readable but fails the real CardAuth path at the first `F2/50` with `pre=1/1`, `tr=1 stat6c=0001D100 id=FF st=FF`, i.e. RX error + missing ACK. This is the expected negative control: Mechacon preparation succeeds, but the card does not answer the MagicGate CardAuth command.
- A separate third-party 64 MB card marked as MagicGate-capable completes the same probe with `PASS`. Capacity and Sony branding are therefore not the deciding factors; the probe is detecting whether the card/controller actually implements the required MagicGate/CardAuth protocol sufficiently to bind a KELF.
- Taken together, the positive and negative controls validate the corrected SECR port mapping and establish the RAM-only KELF probe as a practical functional MagicGate capability test. A printed MagicGate logo alone is not treated as proof; successful protocol completion is.

### FMCB package preflight

- Added optional read-only `mass:` package discovery and manifest validation.
- Installation writes remain disabled while the write/install path itself is still awaiting hardware validation; the underlying MagicGate/KELF bind primitive is now hardware-validated by positive and negative card controls.

## [0.1.1-dev] — "Columbo"

- Migrated the standalone build to PS2DEV 2.0.0.
- Switched normal card access to the Sony ROM X-module stack (`XSIO2MAN`, `XPADMAN`, `XMCMAN`, `XMCSERV`).
- Fixed memory-card test open flags by using `FIO_O_*` instead of incompatible newlib `O_*` values.
- Added stage-specific read/write diagnostics and symbolic MCMAN result names.
- Real hardware confirmed full create/write/flush/reopen/read/compare/delete/cleanup PASS on both tested cards.

## [0.1.0] — "Columbo"

- Initial standalone PS2 Memory Card Inspector skeleton.
- Added card detection, root-directory check, destructive-safe 4 KiB temporary-file read/write test, and guarded format UI.
