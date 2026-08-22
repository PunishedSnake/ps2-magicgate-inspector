# Building and reproducibility

PS2 Memory Card Inspector targets the PS2DEV toolchain and PS2SDK 2.0 for the EE application. Briscoe also needs a deliberately matched IOP security/card profile for the isolated MagicGate session.

## Recommended build path

GitHub Actions is the canonical reproducible build path because it stages the exact IOP modules, pins upstream source revisions, applies the diagnostic patch, and then links the standalone ELF.

The workflow exposes two security profiles:

```text
fmcb13
ps2sdk14
```

Use **Actions -> Build PS2 Memory Card Inspector -> Run workflow** and select the desired `secr_profile`.

During the 1.4 comparison phase, pushes to the Briscoe feature branch intentionally exercise `ps2sdk14`; manual dispatch can still build either profile explicitly.

## EE toolchain

The standalone application is built in:

```text
ps2dev/ps2dev:v2.0.0
```

The successful 1.4 comparison build reported:

```text
mips64r5900el-ps2-elf-gcc (GCC) 15.2.0
```

The produced ELF is statically linked and keeps debug information during development.

## `ps2sdk14` staging

The workflow pins PS2SDK source to:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

Inside the PS2DEV 2.0 container it stages:

```text
.build/ps2sdk2-mg/freesio2.irx
.build/ps2sdk2-mg/freepad.irx
.build/ps2sdk2-mg/mcman.irx
.build/ps2sdk2-mg/mcserv.irx
```

It then checks out the pinned PS2SDK source, applies:

```text
tools/patch_secrman14_diag.py
```

and source-builds:

```text
iop/security/secrman -> .build/ps2sdk2-secr14/secrman.irx
iop/security/secrsif -> .build/ps2sdk2-secr14/secrsif.irx
```

The final EE build is equivalent to:

```sh
make SECR_PROFILE=ps2sdk14
```

after those files exist.

## `fmcb13` staging

The compatibility baseline pins FreeMcBoot Installer source to:

```text
ac53a47a5c6eae675cc2611c7bebe62f56c7845c
```

The workflow stages the PS2SDK-v1-era card modules under:

```text
.build/fmcb-ps2sdk-v1/
```

and applies:

```text
tools/patch_secrman13_diag.py
```

to the pinned SECRMAN source before building the temporary diagnostic module.

The final EE build uses:

```sh
make SECR_PROFILE=fmcb13
```

The compatibility profile is retained as the hardware-validated regression baseline. See `THIRD_PARTY_NOTICES.md` before using this profile in public binary distribution.

## Why the diagnostic patches are build-time patches

The project does not vendor a forked copy of upstream SECRMAN source. Instead, CI:

1. pins an upstream revision;
2. fetches it into a temporary build directory;
3. applies a small deterministic instrumentation patch;
4. builds the temporary IRX;
5. embeds only the resulting build artifact into the standalone development ELF.

This keeps provenance explicit and makes it easier to compare against upstream changes.

The patch is intentionally limited to failure diagnostics. It does not add a software MagicGate implementation or cryptographic keys. The actual security operations continue to use the PS2 MechaCon and the card's CardAuth protocol.

## Local build

A local checkout can run `make`, but a selected profile must be staged first. The Makefile deliberately fails with a clear message if the required `.build/` IRX files are absent.

For example, after staging the PS2SDK 2.0 profile exactly as CI does:

```sh
make SECR_PROFILE=ps2sdk14
```

Output:

```text
MC_INSPECTOR.ELF
```

For reproducible public test builds, prefer CI rather than manually copying arbitrary IRX versions into `.build/`.

## Artifact verification

CI performs:

```sh
file artifacts/MC_INSPECTOR.ELF
sha256sum artifacts/MC_INSPECTOR.ELF
```

and includes:

```text
MC_INSPECTOR.ELF
SHA256SUMS.txt
SECR_PROFILE.txt
```

in the uploaded artifact.

Always record the selected security profile alongside a hardware result. A screenshot showing `FUNCTIONAL` is not a useful regression data point if the backend revision is unknown.

## Known successful `ps2sdk14` comparison build

Workflow run:

```text
#109
```

ELF:

```text
size    1,751,328 bytes
SHA256  b5c1df1c4f51b756bf6c62e5d3fc1a9a414362eab77bf3ad13cd095fc7e4723c
```

Instrumented SECRMAN 1.4:

```text
SHA256  6dae31481db35d85b2f45f60bc82b4c8851da3de46cefc125c3cd564b760f991
```

Matching SECRSIF:

```text
SHA256  2ca392de7b55ad70a31aa5ccbd0259182b1c6db783aea7202b1a161452d5a2db
```

This confirms the profile compiles and links. It does **not** by itself establish hardware equivalence with `fmcb13`.

## Release-build policy

Before a public stable release:

- select one default hardware-validated security backend;
- preserve an explicit source revision for every embedded third-party module;
- include relevant license and attribution notices;
- produce checksums from a clean CI run;
- distinguish development/debug builds from release builds;
- do not enable FMCB card writes until write/read-back/rollback testing is complete.
