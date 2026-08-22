# Credits

## PS2 Memory Card Inspector

**Hifu Himejima / PunishedSnake** — project author and maintainer; feature design; real-hardware testing; filesystem, MagicGate and FreeMcBoot preflight investigation; Briscoe release validation.

## PS2DEV / PS2SDK contributors

For the maintained PlayStation 2 toolchain, SDK, EE/IOP libraries, memory-card and SIO2 support, SECRMAN/SECRSIF implementation, documentation and build environment that make this project possible.

PS2 Memory Card Inspector 0.2.0 uses PS2SDK 2.0 SECRMAN 1.4 with matching PS2SDK 2.0 components. Exact source provenance is recorded in `THIRD_PARTY_NOTICES.md`.

## FreeMcBoot / FreeHdBoot developers

For the FreeMcBoot ecosystem, installer design, KELF-binding work and years of practical PS2 memory-card research that provided the reference behavior needed to understand the CardAuth path.

**SP193** is specifically credited for the FreeMcBoot installer source lineage and for making the relevant implementation available to later homebrew work.

## israpps / FreeMcBoot-Installer maintainers

For preserving and maintaining accessible FreeMcBoot Installer source that was used as a historical comparison during the Briscoe investigation. That compatibility backend helped expose the critical `2 + port` SECRMAN calling convention before the project moved to the PS2SDK 2.0 production stack.

No FreeMcBoot payload is included in this project or release package.

## Hardware validation

Briscoe's MagicGate capability test was validated on real PlayStation 2 hardware with multiple physical cards:

- two official Sony 8 MB cards — `FUNCTIONAL`;
- a third-party 64 MB card with functional MagicGate/CardAuth — `FUNCTIONAL`;
- a third-party 64 MB card without functional MagicGate/CardAuth — ordinary filesystem PASS, security result `NOT SUPPORTED / NO CARD AUTH ACK`.

The positive and negative controls were essential for separating actual card capability from branding, capacity and Inspector-side SIO2 mistakes.

## Trademarks and project names

PlayStation and MagicGate are trademarks of their respective owners. FreeMcBoot, FreeHdBoot, PS2SDK and other project names are used descriptively and remain associated with their respective projects and contributors.

PS2 Memory Card Inspector is an independent homebrew project. No trademark or project name listed here implies endorsement or affiliation.
