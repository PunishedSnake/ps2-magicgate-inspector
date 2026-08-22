# Security backend profiles

Briscoe keeps the PlayStation 2 security backend selectable on purpose. The goal is to compare a known-good compatibility baseline with the maintained PS2SDK 2.0 security stack without changing the EE-side test or the corrected SIO2 port mapping between runs.

## Profiles at a glance

| Profile | SECR stack | Memory-card stack | Build status | Hardware status |
| --- | --- | --- | --- | --- |
| `fmcb13` | Pinned FreeMcBoot Installer compatibility SECRMAN/SECRSIF source | PS2SDK v1-era `freesio2`, `freepad`, `mcman` | Passing | **Validated** |
| `ps2sdk14` | PS2SDK 2.0 `secrman_special` 1.4 + matching `secrsif` | Matching PS2SDK 2.0 `freesio2`, `freepad`, `mcman` | **Passing** | Pending |

The normal application personality is not selected by this option. Outside the isolated MagicGate session, the Inspector always returns to the Sony ROM X memory-card stack.

## Shared behavior

Both security profiles intentionally share the following behavior:

- raw `FMCB.XLF` is loaded into EE RAM before the security-session reboot;
- normal libmc uses logical ports `0/1`;
- SECRSIF requests carrying a card port are translated to physical SIO2 card channels `2/3`;
- temporary MCSERV is skipped in the isolated session;
- the same compact 16-byte GET_KBIT failure record is emitted on failure only;
- no extra CardAuth commands are replayed after a failure;
- the normal ROM X card stack is rebuilt before returning to the UI;
- the KELF copy is discarded instead of being written to the memory card.

This keeps backend comparison focused on the SECR/card-stack generation rather than on unrelated EE behavior.

## `fmcb13` — hardware-validated compatibility baseline

The compatibility profile exists because it was the first stack to complete the probe on real hardware after the port bug was corrected.

### Provenance

FreeMcBoot Installer repository:

```text
https://github.com/israpps/FreeMcBoot-Installer
```

Pinned revision:

```text
ac53a47a5c6eae675cc2611c7bebe62f56c7845c
```

The profile source-builds and instruments the pinned SECRMAN source. The SECRSIF compatibility module is taken from the matching pinned installer tree. The card-side IOP modules are staged from the PS2DEV/PS2SDK v1-era environment used by that compatibility line.

### Hardware result

With the corrected port mapping:

- two official Sony 8 MB cards: `FUNCTIONAL`;
- one third-party 64 MB MagicGate-capable card: `FUNCTIONAL`;
- one third-party 64 MB card without working MagicGate: first `F2/50` returns the validated missing-ACK signature.

This profile is therefore the regression baseline for subsequent security-stack work.

### Redistribution note

The pinned FreeMcBoot compatibility source is fetched at build time rather than vendored into this repository. Its historical source provenance is documented by the upstream project, but this repository does not assert a new license over that upstream code. Before publishing binaries produced with `fmcb13`, independently verify the applicable upstream redistribution terms. See `THIRD_PARTY_NOTICES.md`.

## `ps2sdk14` — PS2SDK 2.0 SECRMAN 1.4 comparison

PS2SDK 2.0 contains `secrman_special` with IRX version **1.4** and a matching SECRSIF implementation. PS2SDK documents this module as the security manager responsible primarily for memory-card authentication and KELF binding; the cryptographic operation itself is performed by the console MechaCon.

### Pinned source

Repository:

```text
https://github.com/ps2dev/ps2sdk
```

Pinned commit used by the comparison build:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

The card stack and security stack are staged from the same PS2SDK generation:

```text
freesio2.irx
freepad.irx
mcman.irx
mcserv.irx   # embedded for common session plumbing, intentionally skipped at runtime
secrman.irx  # source-built and instrumented
secrsif.irx  # source-built, matching SECRMAN 1.4
```

### Why the same port fix still applies

PS2SDK 2.0 CardAuth directly indexes:

```text
port_ctrl1[port]
port_ctrl2[port]
regdata[0] = (port & 3) | ...
```

So the SECR caller must still provide physical memory-card channels `2/3`, not libmc's logical `0/1`.

### Instrumentation difference from stock PS2SDK

The comparison build patches only diagnostics around a failed `SecrDownloadGetKbit()` path.

Stock 1.4 normally calls a private `scePreEncryptKbit()` helper that performs the two Mechacon half-key steps. For diagnostic parity with `fmcb13`, the build-time patch expands those two private operations in the GET_KBIT function so a failure can be attributed to half 0 or half 1. The now-unused helper is removed from the temporary source tree because PS2SDK builds with `-Werror`.

Successful SECRMAN behavior is not intentionally changed. The diagnostic record replaces the Kbit response only when GET_KBIT returns failure.

### First successful CI build

The first instrumented `ps2sdk14` build initially failed because the expanded GET_KBIT path left `scePreEncryptKbit()` as an unused static function under PS2SDK's `-Werror` policy. The patcher was corrected to remove that dead helper and its forward declaration.

The corrected build completed successfully in workflow run **#109**.

Standalone ELF:

```text
size:   1,751,328 bytes
SHA256: b5c1df1c4f51b756bf6c62e5d3fc1a9a414362eab77bf3ad13cd095fc7e4723c
```

Staged PS2SDK 2.0 IOP modules:

```text
freepad.irx
1ca382e617528b179405edbcbb744c421019e9cb2bce9a7fe30f3d68c074f112

freesio2.irx
44748d1c67b22132c026dd05bb06314bcbb5318a3f12835fd388f4e2b3126986

mcman.irx
5bb7d332523add2a834374998e5dd6268c9b8a05dbff3346bff334e7d2023dd7

mcserv.irx
9f1b2ee6eb5f7c1f56ce225100824d85bc615eda3dac4f6be00b5f9f6d3c8924

instrumented secrman.irx 1.4
6dae31481db35d85b2f45f60bc82b4c8851da3de46cefc125c3cd564b760f991

matching secrsif.irx
2ca392de7b55ad70a31aa5ccbd0259182b1c6db783aea7202b1a161452d5a2db
```

The GitHub artifact ZIP digest for that run was:

```text
7e2bd64a957e349231306116efa6822a66704a536c919be64de71a1c2305f5be
```

These hashes describe the **pre-documentation comparison build** used to establish that the 1.4 profile compiles and links. Any later source/banner cleanup naturally produces a different final ELF hash.

## Selecting a profile

The Makefile accepts:

```sh
make SECR_PROFILE=fmcb13
make SECR_PROFILE=ps2sdk14
```

However, these targets expect the corresponding profile to have already been staged under `.build/`. GitHub Actions is the canonical reproducible staging path; see `docs/BUILDING.md`.

Manual workflow dispatch exposes the same profile names as a choice.

## Decision rule for the default backend

`fmcb13` remains the behavioral baseline until `ps2sdk14` has been tested against the same positive and negative card matrix.

A candidate modern default should demonstrate at minimum:

1. `FUNCTIONAL` on both official Sony 8 MB positive controls;
2. `FUNCTIONAL` on the known third-party 64 MB MagicGate-capable card;
3. a clean negative result on the known non-MagicGate 64 MB card;
4. successful normal ROM X stack restoration after each probe;
5. no regressions in raw KELF parsing, BIT handling, Kbit, or Kc stages.

Only then should the project decide whether to retire the legacy compatibility backend from normal release builds.

## Licensing advantage of the PS2SDK profile

PS2SDK 2.0 has an explicit upstream **Academic Free License 2.0** license and source provenance. From a release-engineering perspective this is materially cleaner than depending on a historical compatibility snapshot whose redistribution terms need separate verification.

That licensing clarity is one reason to prefer the maintained PS2SDK stack if hardware behavior proves equivalent.
