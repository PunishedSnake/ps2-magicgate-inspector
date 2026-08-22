# Changelog

All notable changes to PS2 Memory Card Inspector are documented here.

## [Unreleased] — v0.2.0 "Briscoe"

### MagicGate / KELF diagnostics

- Added isolated, RAM-only MagicGate/KELF probing using a raw user-supplied `FMCB.XLF` from `mass:/FMCB/SYSTEM/` (also probes `mass0:` / `mass1:`).
- Ordinary memory-card inspection always returns to the hardware-validated Sony ROM X-module stack after a security-session attempt.
- Pinned the classic FreeMcBoot Installer `SECRMAN/SECRSIF` compatibility line as the initial controlled hardware baseline.
- `sceMcResChangedCard (-1)` is treated as a transient post-IOP-reboot state rather than a fatal card error when valid PS2 metadata is present.
- Corrected BIT block validation: the SECRSIF `0x400` RPC limit applies only to blocks marked for security download (`flags & 2`), not to large plaintext payload blocks.
- Source-level GET_KBIT instrumentation preserves successful SECRMAN behavior and, on failure only, returns a compact diagnostic record through the existing 16-byte Kbit reply. The record distinguishes Mechacon preparation from first/second-half CardAuth and captures the exact command, callback return, `stat6c`, response ID/status and checksum state.
- Corrected SECR port numbering at the SECRSIF RPC boundary. The reference FreeMcBoot Installer calls `SecrDownloadFile(2 + port, slot, ...)`, because CardAuth consumes physical SIO2 memory-card channels 2/3 directly. Inspector had passed logical libmc ports 0/1, which targeted controller channels and produced missing-ACK failures. HEADER, GET_KBIT and GET_KC now map logical 0/1 to physical 2/3 while normal libmc access remains unchanged.
- Production cleanup removes the obsolete command-replay tracer and dev-numbered runtime shims. Failed GET_KBIT calls are diagnosed in-path rather than by replaying CardAuth commands after the failure.
- User-facing MagicGate classification now distinguishes `FUNCTIONAL`, the validated `NOT SUPPORTED / NO CARD AUTH ACK` signature, other CardAuth protocol errors and indeterminate Mechacon/session failures.
- Build plumbing supports two explicitly selectable security profiles: `fmcb13` (hardware-validated compatibility baseline) and `ps2sdk14` (PS2SDK 2.0 SECRMAN 1.4 + matching SECRSIF/card stack). Both profiles share the corrected EE-side port mapping and diagnostic record format.
- Added dedicated documentation for MagicGate findings, security backend provenance, reproducible builds, hardware regression testing and the staged installer roadmap.

### Hardware findings

- dev7: both tested official Sony cards passed filesystem inspection, `mcInit`, `mcGetInfo`, SECR RPC, `DownloadHeader`, and the encrypted BIT block. A false failure on BIT block 1 was traced to the Inspector's over-broad `0x400` size guard.
- dev8: after fixing BIT semantics, both cards again behaved identically and advanced to `GET KBIT`; both returned `Kbit = 0`. One card is rejected by the stock FMCB Installer and one already boots FMCB, so this was not evidence of a card-specific MagicGate defect.
- `SecrDownloadGetKbit()` is the first tested stage that invokes SECRMAN `card_encrypt()` and therefore depends on MCMAN having registered compatible `mcCommand`/device-ID callbacks.
- dev9 aligned the isolated card stack with the PS2SDK-v1 generation used by the FreeMcBoot compatibility source.
- dev10 confirmed that loading the temporary PS2SDK-v1 MCSERV can wedge the next LOADFILE RPC. The isolated session therefore keeps MCMAN active, skips temporary MCSERV and emulates only the immediate EE-side libmc sanity query.
- dev11 hardware trace returned `F3: tr=1 stat6c=0001D100 id=FF st=FF`. `stat6c` indicates RX FIFO error plus missing ACK. Source review showed that this F3 reset was an extra diagnostic command: the real `SecrDownloadGetKbit()` path starts directly with Mechacon pre-encryption followed by `F2/50 -> 51 -> 52 -> 53`. The replay tracer was retired.
- dev12 instrumented the original SECRMAN function in-place. Both tested official Sony cards produced the same real failure: `pre=1/1`, then `CARD ENCRYPT HALF 0`, command `0x50`, `tr=1 stat6c=0001D100 id=FF st=FF`. This proved both Mechacon Kbit halves were prepared successfully and isolated the failure to the first real CardAuth command.
- Comparing that result with the reference FMCB installer exposed the caller bug: FMCB signs with `SecrDownloadFile(2 + port, slot, ...)`, while Inspector used `port` directly. Since CardAuth consumes the supplied SIO2 port number, Inspector's 0/1 selected controller channels instead of memory-card channels 2/3.
- dev13 hardware validation: after applying the correct 0/1 -> 2/3 mapping, both tested official Sony 8 MB cards completed the entire RAM-only MagicGate probe with `PASS`, including `DownloadHeader = 1`, the encrypted BIT block, `Kbit = 1` and `Kc = 1`.
- A third-party 64 MB card without functional MagicGate remains filesystem-readable but fails the real CardAuth path at the first `F2/50` with `pre=1/1`, `tr=1 stat6c=0001D100 id=FF st=FF`, i.e. RX error + missing ACK. With the corrected backend already proven on other cards, this is the project's negative-control signature.
- A separate third-party 64 MB card marked as MagicGate-capable completes the same probe with `PASS`. Capacity and Sony branding are therefore not the deciding factors; the probe detects whether the card/controller actually implements the required CardAuth behavior sufficiently to bind a KELF.
- Taken together, the positive and negative controls establish the RAM-only KELF probe as a practical functional MagicGate capability test. A printed MagicGate logo alone is not treated as proof; successful protocol completion is.

### PS2SDK 2.0 / SECRMAN 1.4 comparison

- Added `ps2sdk14`, a source-built comparison profile pinned to PS2SDK commit `a13b5971ec0e39c7ba8b8559b80a4e81c8425352`.
- The profile uses PS2SDK 2.0 `secrman_special` IRX 1.4, matching SECRSIF and matching PS2SDK 2.0 `freesio2/freepad/mcman` modules.
- Added equivalent GET_KBIT failure instrumentation for SECRMAN 1.4 so 1.3 and 1.4 hardware results can be compared with the same record format.
- The first 1.4 CI attempt exposed a build-patcher issue: expanding the two Mechacon half-key calls left stock `scePreEncryptKbit()` unused, and PS2SDK correctly rejected the warning under `-Werror`. The patcher now removes that dead temporary helper and its prototype.
- Workflow run **#109** successfully built the complete `ps2sdk14` standalone ELF. Pre-documentation comparison build: size `1,751,328` bytes, SHA-256 `b5c1df1c4f51b756bf6c62e5d3fc1a9a414362eab77bf3ad13cd095fc7e4723c`.
- The successful run produced instrumented SECRMAN 1.4 SHA-256 `6dae31481db35d85b2f45f60bc82b4c8851da3de46cefc125c3cd564b760f991` and matching SECRSIF SHA-256 `2ca392de7b55ad70a31aa5ccbd0259182b1c6db783aea7202b1a161452d5a2db`.
- `ps2sdk14` is therefore **build-validated but not yet hardware-validated**. The next step is to run it against the same Sony and third-party positive/negative controls used for `fmcb13`.

### FMCB package preflight

- Added optional read-only `mass:` package discovery and manifest validation.
- Installation writes remain disabled while the write/install transaction itself is awaiting hardware validation; the underlying RAM-only MagicGate/KELF bind primitive is hardware-validated with the `fmcb13` baseline.
- The planned first write-capable milestone is a controlled bind -> write -> close/reopen -> read-back -> verify -> rollback transaction, not a general installer button.

### Project maintenance

- Added an MIT license for original PS2 Memory Card Inspector source.
- Added third-party provenance/redistribution notes for PS2SDK and the FreeMcBoot compatibility profile.
- Added explicit project/upstream credits.
- Removed obsolete development tracer sources from the active architecture.

## [0.1.1-dev] — "Columbo"

- Migrated the standalone build to PS2DEV 2.0.0.
- Switched normal card access to the Sony ROM X-module stack (`XSIO2MAN`, `XPADMAN`, `XMCMAN`, `XMCSERV`).
- Fixed memory-card test open flags by using `FIO_O_*` instead of incompatible newlib `O_*` values.
- Added stage-specific read/write diagnostics and symbolic MCMAN result names.
- Real hardware confirmed full create/write/flush/reopen/read/compare/delete/cleanup PASS on both tested cards.

## [0.1.0] — "Columbo"

- Initial standalone PS2 Memory Card Inspector skeleton.
- Added card detection, root-directory check, destructive-safe 4 KiB temporary-file read/write test, and guarded format UI.
