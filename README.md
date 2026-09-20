# PS2 Memory Card Inspector

PS2 Memory Card Inspector is a standalone PlayStation 2 homebrew utility for
memory-card diagnostics, MagicGate/CardAuth qualification, card imaging/save
transfer, and verified cross-region FreeMcBoot installation.

## v0.4.0 "Drebin"

Drebin is the first full write-capable release.

The 0.4.0 release combines the hardware-validated MagicGate work from Briscoe
with a transactional FMCB installer, recovery journal, Card Tools, persistent
settings and the production P0 optimization pass.

There is **one public 0.4.0 build**. Development-only USB speed-test variants,
async candidates and Performance Lab binaries are not part of the release.

### Release build profile

```text
-O2 -G0
-mtune=r5900 on measured hot objects
P0 synchronous production batching
no USB benchmark matrix
no async/NOWAIT release variants
no Performance Lab binaries
```

The project deliberately keeps `-O2` as the global baseline. R5900 tuning and
the proven P0 paths are retained without turning the public release into an
optimization experiment.

## Major features

### Memory-card diagnostics

- inspect `mc0:` and `mc1:`;
- card type, formatting state and free-space reporting;
- temporary write/read/compare/delete filesystem test;
- guarded formatting and destructive-operation confirmations;
- hot-swap detection and explicit target revalidation.

### MagicGate / CardAuth

- real KELF/CardAuth capability testing on hardware;
- isolated PS2SDK 2.0 SECRMAN 1.4 security personality;
- correct logical `mc0/mc1` -> physical SIO2 `2/3` translation only at
  the SECR boundary;
- stage-specific HEADER/BLOCK/Kbit/Kc/ICVPS2 diagnostics;
- known-good and negative-control behavior verified on real hardware;
- no Sony-brand heuristic: cards are judged by actual capability.

### FreeMcBoot 1.966 cross-region installer

Drebin can install a user-supplied FMCB package as a real cross-region
installation for the supported PS2 path.

The installer:

1. detects the runtime ROM/MechaCon/MechaPwn compatibility profile;
2. discovers and validates the complete FMCB package;
3. checks card filesystem health and free space;
4. bind-probes every **distinct selected KELF source** before the first card
   mutation;
5. creates a durable recovery journal and card identity marker;
6. backs up any replaced destination;
7. binds KELFs in EE RAM through the validated SECR path;
8. writes, closes, reopens and fully verifies every destination;
9. commits only after the complete transaction succeeds;
10. rolls back on failure.

Regional destinations cover the normal I/A/E/C system folders. The installer
uses real file copies rather than the historical FMCB Multi-Install crosslink
trick.

Compatibility policy is capability-driven rather than model-number folklore.
For example, the CEX-only 128-byte `ENDVDPL.XRX` is omitted for real DEX and
for a positively fingerprinted MechaPwn DEX-mode profile after real-hardware
qualification showed that this profile rejects ENDVDPL at the SECR HEADER stage
while ordinary FMCB/OSDSYS KELFs bind successfully.

See:

- [FMCB package and install contract](docs/FMCB_PACKAGE.md)
- [FMCB compatibility and exception corpus](docs/FMCB_COMPATIBILITY_CORPUS.md)
- [P0/FMCB hardware qualification](docs/P0_FMCB_INTEGRATION_TEST.md)

### Recovery

Interrupted or failed FMCB transactions are recoverable from a persistent
journal stored beside the source package on USB.

The journal records captured destinations, created directories and transaction
identity. Recovery verifies that the same target card is present before
restoring or deleting anything.

Legacy recovery-journal v1 files are read and converted safely in RAM before
use; current transactions use v2.

### Card Tools

Drebin also contains the 0.4 Card Tools work:

- full memory-card image export;
- image verification;
- exact-image restore;
- image browser/filesystem inspection;
- selective save import/export;
- PSU-oriented save-transfer support;
- force-format workflow with backup/recovery safeguards;
- USB and card file pickers.

Destructive operations remain explicit and verification-oriented.

### Persistent Settings

Settings can be saved with **Square** on the Settings page.

The versioned text config is stored as:

```text
mass:/MCI/MCINSPECTOR.CFG
```

with `mass0:` / `mass1:` fallback.

Saved values currently include:

- display mode;
- filesystem-test profile;
- preserve-existing-CNF policy;
- FMCB read-back verification mode.

The GUI always starts in safe Native mode first. Saved display mode is applied
only after USB initialization and successful config validation.

See [Settings config](docs/SETTINGS_CONFIG.md).

## Hardware validation highlights

The project has been tested on real PlayStation 2 hardware.

Confirmed results include:

- official Sony 8 MiB cards passing filesystem and MagicGate/CardAuth;
- a third-party card with functional MagicGate passing CardAuth;
- a third-party card without functional MagicGate remaining usable as storage
  while correctly failing CardAuth;
- successful FMCB installation and boot from a non-Sony MagicGate-capable card;
- automatic rollback restoring the pre-install state after a deliberately
  incompatible ENDVDPL bind attempt;
- MechaPwn DEX-like behavior qualified separately from normal retail/CEX policy.

PCSX2 remains useful for correctness/debugging, but subtle CardAuth, IOP,
USB/fileXio and timing claims are qualified on real hardware.

## Required FMCB package

The project does **not** redistribute FreeMcBoot payloads.

Provide a complete FMCB 1.966 installer package on USB. The installer searches
recursively for `SYSTEM/FMCB.XLF` and validates the required companion files
before allowing installation.

See [docs/FMCB_PACKAGE.md](docs/FMCB_PACKAGE.md) for the exact package layout.

## Controls

Controls are page-sensitive. The footer always shows the currently relevant
actions.

Common controls include:

| Control | Action |
| --- | --- |
| Up / Down | Select card / item |
| Left / Right | Change the selected setting or browser value |
| L1 / R1 | Previous / next page where applicable |
| Cross | Run / apply / confirm the current non-destructive action |
| Square | Page-specific secondary action; on Settings saves CFG |
| Triangle | Arm or enter selected destructive/installer action where shown |
| Circle | Cancel / back |
| Select | Exit |

Destructive confirmations require an additional explicit chord where the UI
states one.

## Building

The canonical release environment is:

```text
ps2dev/ps2dev:v2.0.0
```

The security stack is pinned to PS2SDK source commit:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

The public 0.4.0 build is a single P0 production ELF:

```text
MC_INSPECTOR-0.4.0-Drebin.ELF
```

No USB speed-test or Performance Lab variants are published.

See [Building and reproducibility](docs/BUILDING.md).

## Safety model

Drebin treats card writes as transactions rather than optimistic copies.

Important properties:

- source package validation precedes destination mutation;
- every distinct selected KELF is compatibility-probed first;
- existing files are captured before replacement;
- writes are closed/reopened and read back in full;
- recovery state persists until commit;
- card identity is checked before rollback;
- logger/USB ownership is serialized around correctness-critical mass-storage
  transactions;
- formatting and restore actions require explicit confirmation.

A failed bind or write is not treated as permission to continue.

## Documentation

- [Release notes](RELEASE_NOTES.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Hardware and regression testing](docs/TESTING.md)
- [Building and reproducibility](docs/BUILDING.md)
- [MagicGate / CardAuth findings](docs/MAGICGATE.md)
- [FMCB package layout](docs/FMCB_PACKAGE.md)
- [FMCB compatibility corpus](docs/FMCB_COMPATIBILITY_CORPUS.md)
- [Settings config](docs/SETTINGS_CONFIG.md)
- [Roadmap](docs/ROADMAP.md)
- [Changelog](CHANGELOG.md)
- [Credits](CREDITS.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## License and attribution

Original PS2 Memory Card Inspector source is released under the
[MIT License](LICENSE), except where a file or third-party component states
otherwise.

PS2SDK components remain under the Academic Free License 2.0. The release
package includes the PS2SDK license text, credits, third-party notices,
checksums and source provenance.

Sony ROM modules and user-supplied FreeMcBoot payloads are not distributed by
this project.

PlayStation, MagicGate and related names are trademarks of their respective
owners. PS2 Memory Card Inspector is an independent homebrew project and is not
affiliated with or endorsed by Sony Interactive Entertainment.
