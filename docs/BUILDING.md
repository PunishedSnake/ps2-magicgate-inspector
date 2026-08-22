# Building and reproducibility

PS2 Memory Card Inspector 0.2.0 targets PS2DEV / PS2SDK 2.0 for both the EE application and the isolated MagicGate security stack.

## Canonical build

GitHub Actions is the canonical release build path. It pins the security source, stages matching IOP modules, applies the deterministic failed-GET_KBIT instrumentation, builds SECRMAN/SECRSIF from source, links the standalone ELF, computes SHA-256 and packages the required license/provenance files.

The release has one security backend: **PS2SDK 2.0 SECRMAN 1.4**.

## Toolchain

CI uses:

```text
ps2dev/ps2dev:v2.0.0
```

The validated Briscoe development builds used GCC 15.2.0 from that image.

## Pinned PS2SDK source

The security source revision is:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

CI copies the matching PS2SDK 2.0 card modules into:

```text
.build/ps2sdk2-mg/freesio2.irx
.build/ps2sdk2-mg/freepad.irx
.build/ps2sdk2-mg/mcman.irx
.build/ps2sdk2-mg/mcserv.irx
```

It checks out the pinned PS2SDK source, applies:

```text
tools/patch_secrman14_diag.py
```

and builds:

```text
iop/security/secrman -> .build/ps2sdk2-secr14/secrman.irx
iop/security/secrsif -> .build/ps2sdk2-secr14/secrsif.irx
```

The application then builds with a plain:

```sh
make
```

## Why SECRMAN is patched at build time

Inspector does not vendor a permanent fork of PS2SDK SECRMAN. CI:

1. checks out an exact upstream revision;
2. patches only the temporary source tree;
3. records CardAuth/Mechacon state along the real failed GET_KBIT path;
4. removes the now-unused private helper so PS2SDK's `-Werror` build remains clean;
5. builds the IRX and embeds it into the standalone ELF.

The patch does not provide software MagicGate keys or replace the successful authentication path. Actual security operations still use the console Mechacon and the card's CardAuth protocol.

The patch source is part of this repository so the modification is reproducible and auditable.

## Local build

A local `make` requires the same staged files under `.build/`. The Makefile intentionally fails when they are missing instead of silently using arbitrary installed IRX versions.

The safest local process is to reproduce the staging commands from `.github/workflows/build.yml` inside `ps2dev/ps2dev:v2.0.0`, then run:

```sh
make
```

Output:

```text
MC_INSPECTOR.ELF
```

For public binaries, prefer CI so the exact source revision and build environment are recorded automatically.

## Release artifact contents

The workflow packages:

```text
MC_INSPECTOR.ELF
SHA256SUMS.txt
SOURCE_PROVENANCE.txt
README.md
RELEASE_NOTES.md
LICENSE
CREDITS.md
THIRD_PARTY_NOTICES.md
licenses/PS2SDK-AFL-2.0.txt
```

`SOURCE_PROVENANCE.txt` records both the Inspector revision and the pinned PS2SDK security-source revision.

## Historical development baseline

Briscoe development used a pinned FreeMcBoot-compatible SECRMAN 1.3 stack to isolate the original CardAuth failure and discover the logical-port/physical-SIO2 mapping bug. That path was valuable as a regression baseline but is no longer part of the 0.2.0 production build.

The historical results remain in `CHANGELOG.md`, `docs/MAGICGATE.md` and repository history. Removing the 1.3 build path avoids shipping an unnecessary second backend and gives the release a single, clearly licensed PS2SDK 2.0 provenance chain.

## Release policy

A stable build must:

- build from the pinned PS2SDK source revision;
- keep the exact build-time diagnostic patch in source control;
- include project and PS2SDK license/attribution notices;
- publish a SHA-256 for the ELF;
- preserve the normal ROM X filesystem stack and isolated security-session boundary;
- keep FMCB installation writes disabled until bind/write/read-back/rollback behavior is independently hardware-validated.
