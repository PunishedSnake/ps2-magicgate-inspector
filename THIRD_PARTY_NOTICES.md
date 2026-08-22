# Third-party notices

PS2 Memory Card Inspector contains original project code and also builds against or embeds build artifacts from existing PlayStation 2 homebrew projects. Third-party components remain subject to their own copyright and license terms; the project's MIT License does not replace those terms.

This file records the provenance used by the Briscoe development line. It is intended to make later release packaging auditable, not to grant rights that an upstream project has not granted.

## PS2DEV / PS2SDK

Upstream repository:

```text
https://github.com/ps2dev/ps2sdk
```

Briscoe's PS2SDK 2.0 security comparison pins:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

PS2SDK is distributed under the **Academic Free License version 2.0 (AFL-2.0)**. The upstream license text is available in the PS2SDK repository:

```text
https://github.com/ps2dev/ps2sdk/blob/v2.0.0/LICENSE
```

Depending on the build profile, PS2 Memory Card Inspector links against PS2SDK EE libraries and embeds PS2SDK IOP modules such as memory-card/SIO2 components and the SECRMAN/SECRSIF security stack.

The `ps2sdk14` profile source-builds an instrumented temporary copy of PS2SDK 2.0 SECRMAN 1.4. The build-time patch is maintained in this repository as `tools/patch_secrman14_diag.py`; the original upstream source is not vendored here. The patch adds failure diagnostics and preserves the upstream source revision in the build documentation.

The full corresponding upstream source remains available from the pinned PS2SDK revision above.

## FreeMcBoot Installer compatibility source

Upstream repository:

```text
https://github.com/israpps/FreeMcBoot-Installer
```

Pinned compatibility revision used by `fmcb13`:

```text
ac53a47a5c6eae675cc2611c7bebe62f56c7845c
```

The upstream project identifies its lineage as FreeMcBoot installer source and explicitly credits SP193 for preserving/releasing installer source code.

The `fmcb13` profile fetches that source **at build time** and applies `tools/patch_secrman13_diag.py` to create a diagnostic development SECRMAN. The FreeMcBoot source itself is not vendored into this repository and no FreeMcBoot installation payload is included in this repository.

### Redistribution caution

At the pinned revision, this project has not identified a clear top-level software license covering the complete FreeMcBoot Installer source tree. Therefore this repository does **not** assert that the project's MIT License applies to that upstream code or to binaries produced from it.

Before publishing a binary release built with the `fmcb13` profile, verify the applicable upstream redistribution terms independently. Until that is resolved, `fmcb13` should be treated primarily as the hardware-validated development/regression baseline.

The `ps2sdk14` profile has clearer release provenance because the corresponding PS2SDK source is explicitly covered by AFL-2.0, subject to that license's requirements.

## Sony ROM modules

The Inspector loads memory-card/controller modules directly from the user's console ROM during normal operation, for example:

```text
rom0:XSIO2MAN
rom0:XPADMAN
rom0:XMCMAN
rom0:XMCSERV
```

Those ROM modules are **not** copied into this repository or distributed inside release artifacts by this project.

## User-supplied FreeMcBoot files

A raw `FMCB.XLF` and wider FMCB package used for testing are supplied by the user from USB storage. The project does not bundle FreeMcBoot payload files.

The Inspector currently uses the raw KELF only as RAM-resident test input. It does not redistribute it or write the bound result back to the card.

## Trademarks and affiliation

PlayStation and MagicGate are trademarks of their respective owners. FreeMcBoot and other homebrew project names are used descriptively.

PS2 Memory Card Inspector is an independent homebrew project and is not affiliated with or endorsed by Sony Interactive Entertainment or the upstream projects listed above.

## Release-maintainer checklist

Before publishing a binary release:

1. record the selected `SECR_PROFILE`;
2. record exact upstream commit hashes;
3. retain applicable upstream copyright/license notices;
4. provide a source location for corresponding modified third-party source as required by its license;
5. verify the redistribution terms of any compatibility component whose license is unclear;
6. do not bundle Sony ROM modules or user-supplied FMCB payloads.

This notice is not legal advice. It documents the project's current provenance and the release questions that must not be lost during development.
