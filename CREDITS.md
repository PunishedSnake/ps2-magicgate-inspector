# Credits

## PS2 Memory Card Inspector

**Hifu Himejima / PunishedSnake**

Project author, maintainer, hardware testing, feature design and Briscoe MagicGate investigation.

## PS2 homebrew foundations

This project exists because of the work done by the wider PlayStation 2 homebrew community over many years.

### PS2DEV / PS2SDK contributors

For the PS2 toolchain, SDK, EE/IOP libraries, memory-card stack, SIO2 support, security modules, documentation and the maintained PS2SDK development environment used to build this project.

The modern `ps2sdk14` backend is based on PS2SDK 2.0's SECRMAN 1.4 / SECRSIF implementation and matching memory-card modules.

### FreeMcBoot / FreeHdBoot developers

For the original FreeMcBoot ecosystem, installer design and the practical KELF-binding work that made later investigation possible.

**SP193** is specifically credited for the FreeMcBoot installer source lineage and the work preserved by later maintainers.

### israpps / FreeMcBoot-Installer maintainers

For keeping the FreeMcBoot Installer source and compatibility builds available. The `fmcb13` regression profile pins a revision of that repository so the hardware-validated compatibility behavior can be reproduced.

## Hardware validation

Briscoe's MagicGate work was validated on real PlayStation 2 hardware with multiple memory cards, including:

- two official Sony 8 MB cards;
- a third-party 64 MB card without functional MagicGate/CardAuth support;
- a separate third-party 64 MB card that successfully completes the MagicGate/KELF probe.

Those positive and negative controls were essential to distinguishing actual card capability from filesystem compatibility and from an Inspector-side SIO2 port-numbering bug.

## Trademarks

PlayStation, MagicGate, FreeMcBoot and other names belong to their respective owners or projects. Their appearance here is descriptive and does not imply endorsement or affiliation.
