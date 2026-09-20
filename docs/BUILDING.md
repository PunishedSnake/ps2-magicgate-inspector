# Building and reproducibility

PS2 Memory Card Inspector 0.4.0 "Drebin" targets PS2DEV / PS2SDK 2.0.

The public release is one production profile. P0 optimizations used by the normal
runtime remain enabled, while USB speed-test variants, async/NOWAIT candidates
and Performance Lab binaries are not built or published.

## Canonical release profile

```text
-O2 -G0
-mtune=r5900 on measured hot objects
RAW_BULK_PAGES=16
IMAGE_READ_PAGES=32
IMAGE_WRITE_PAGES=32
RAW_BULK_ASYNC=0
IMAGE_READ_AHEAD_ASYNC=0
IMAGE_WRITE_ASYNC=0
```

The global baseline stays at `-O2`. Drebin does not promote broad `-O3`,
`-ffast-math`, LTO or unrolling into release policy.

## Canonical CI build

GitHub Actions is the canonical release build path. It:

1. runs source invariants for card math, FMCB cross-region policy and Settings;
2. stages the matching PS2SDK 2.0 card/security personalities;
3. source-builds the pinned SECRMAN 1.4 + SECRSIF pair;
4. builds the legacy non-X raw MCMAN/MCSERV pair used by Card Tools;
5. builds one production `MC_INSPECTOR.ELF`;
6. verifies that benchmark/Performance Lab objects are absent;
7. packages SHA-256, provenance, licenses and release documentation.

CI uses:

```text
ps2dev/ps2dev:v2.0.0
```

## Pinned security source

The qualified security source revision remains:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

CI applies:

```text
tools/patch_secrman14_diag.py
```

to a temporary checkout and builds:

```text
iop/security/secrman -> .build/ps2sdk2-secr14/secrman.irx
iop/security/secrsif -> .build/ps2sdk2-secr14/secrsif.irx
```

Matching PS2SDK 2.0 card modules are staged under:

```text
.build/ps2sdk2-mg/
```

Card Tools also require a raw-page legacy MCMAN/MCSERV personality built with
XMC compatibility disabled:

```text
.build/ps2sdk2-raw/mcman.irx
.build/ps2sdk2-raw/mcserv.irx
```

## Local build

A plain local `make` expects the same staged IRX files as CI. The Makefile
fails if they are absent rather than silently selecting arbitrary installed
modules.

After staging:

```sh
make clean
make MC_INSPECTOR.ELF
```

Output:

```text
MC_INSPECTOR.ELF
```

For public binaries, prefer CI because it records the exact project revision,
PS2SDK security revision and release profile.

## Release artifact

The stable workflow packages:

```text
MC_INSPECTOR-0.4.0-Drebin.ELF
SHA256SUMS.txt
SOURCE_PROVENANCE.txt
README.md
RELEASE_NOTES.md
LICENSE
CREDITS.md
THIRD_PARTY_NOTICES.md
licenses/PS2SDK-AFL-2.0.txt
```

`SOURCE_PROVENANCE.txt` records that the artifact is the P0 production profile
with synchronous transport and no speed-test variants.

## Why the release keeps P0 but not the lab

P0 changes that are part of the normal architecture remain in 0.4.0. Examples
include production batching, fast-copy support and R5900 tuning on hot objects.

The following remain development-only:

- USB throughput A/B matrices;
- alternate raw/image batch sizes;
- async fileXio candidates;
- synthetic R5900 counter builds;
- Performance Lab executables.

This keeps the public artifact reproducible and understandable without throwing
away improvements already integrated into the production path.

## Release policy

A stable build must:

- build from the pinned PS2SDK source revision;
- keep the exact build-time SECR diagnostic patch in source control;
- keep ordinary card I/O and isolated security personalities separate;
- retain all-KELF compatibility preflight before FMCB writes;
- retain durable recovery and full read-back verification;
- retain synchronous ownership for correctness-critical mass I/O;
- publish one production ELF with SHA-256 and provenance;
- include project and PS2SDK license/attribution notices.
