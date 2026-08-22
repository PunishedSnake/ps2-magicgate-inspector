# Third-party notices

PS2 Memory Card Inspector contains original project code and builds against or embeds build artifacts from existing PlayStation 2 homebrew projects. Third-party components remain subject to their own copyright and license terms; the project's MIT License does not replace those terms.

## PS2DEV / PS2SDK

Upstream repository:

```text
https://github.com/ps2dev/ps2sdk
```

Security-source revision used by PS2 Memory Card Inspector 0.2.0:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

PS2SDK is distributed under the **Academic Free License version 2.0 (AFL-2.0)**. A copy of the upstream license text is included in this repository and release package as:

```text
licenses/PS2SDK-AFL-2.0.txt
```

The corresponding upstream source is available from the repository and exact commit above.

### Components used

The EE application links against PS2SDK libraries and embeds PS2SDK IOP artifacts needed by the standalone program. The isolated MagicGate session uses the PS2SDK 2.0 generation of:

```text
freesio2
freepad
mcman
mcserv     # staged/embedded but intentionally not started in the isolated session
secrman    # source-built SECRMAN 1.4 with Inspector failure instrumentation
secrsif    # source-built matching SECRSIF
```

Other PS2SDK IOP modules used by the application include the normal USB/fileXio support required for the user-supplied FMCB package source.

### Modified SECRMAN source

PS2 Memory Card Inspector does not vendor the PS2SDK SECRMAN source tree. CI checks out the pinned PS2SDK revision into a temporary directory and applies:

```text
tools/patch_secrman14_diag.py
```

The patch modifies the temporary PS2SDK SECRMAN/CardAuth source only to make a failed `SecrDownloadGetKbit()` observable. It records the real Mechacon/CardAuth failure stage and returns a compact diagnostic record through the failed Kbit reply. Successful authentication remains on the upstream code path and no authentication command is replayed for diagnosis.

This repository therefore provides:

- the exact upstream source revision;
- the complete build-time modification script;
- the upstream AFL-2.0 license text;
- reproducible CI instructions showing how the modified IRX is produced.

The CI release artifact also includes `SOURCE_PROVENANCE.txt` with the exact Inspector and PS2SDK revisions used for that build.

## Historical FreeMcBoot reference work

FreeMcBoot / FreeHdBoot installer source and later maintained FreeMcBoot-Installer revisions were used during development to understand the expected KELF-binding/CardAuth call sequence and, critically, the `2 + port` convention used when entering SECRMAN.

Reference repository used during the Briscoe investigation:

```text
https://github.com/israpps/FreeMcBoot-Installer
```

Historical comparison revision:

```text
ac53a47a5c6eae675cc2611c7bebe62f56c7845c
```

That compatibility backend is **not part of the 0.2.0 release build** and no FreeMcBoot source or installer payload is redistributed by PS2 Memory Card Inspector. Its role and historical results remain documented in `CHANGELOG.md` and `docs/MAGICGATE.md`.

The project credits SP193 and later FreeMcBoot-Installer maintainers for preserving the implementation knowledge that made this comparison possible.

## Sony ROM modules

During ordinary operation Inspector loads memory-card/controller modules directly from the user's own console ROM, including:

```text
rom0:XSIO2MAN
rom0:XPADMAN
rom0:XMCMAN
rom0:XMCSERV
```

Those Sony ROM modules are not copied into this repository and are not included in release artifacts.

## User-supplied FreeMcBoot files

A raw `FMCB.XLF` and any wider FMCB package used for testing are supplied by the user from USB storage. The project does not bundle FreeMcBoot payloads.

In 0.2.0 the KELF is used only as RAM-resident test input. The bound result is not written back to the card.

## Project license

Original PS2 Memory Card Inspector code is released under the MIT License in the repository `LICENSE` file, except where a source file or third-party component states otherwise.

The MIT License applies to the project's original code and build scripts; it does not relicense PS2SDK or any other upstream project.

## Trademarks and affiliation

PlayStation and MagicGate are trademarks of their respective owners. FreeMcBoot, FreeHdBoot, PS2SDK and related project names are used descriptively.

PS2 Memory Card Inspector is an independent homebrew project and is not affiliated with or endorsed by Sony Interactive Entertainment or the upstream projects listed above.

## Release-maintainer checklist

For each binary release:

1. retain the exact PS2SDK source revision;
2. retain the build-time SECRMAN modification script;
3. include `LICENSE`, `CREDITS.md`, this notice and the PS2SDK AFL-2.0 text;
4. include a source-provenance record pointing to the corresponding upstream source;
5. publish the final ELF SHA-256;
6. do not bundle Sony ROM modules or user-supplied FreeMcBoot payloads.

This file documents project provenance and attribution. It is not legal advice.
