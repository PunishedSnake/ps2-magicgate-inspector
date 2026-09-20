# Hardware and regression testing

PS2 Memory Card Inspector is qualified on real PlayStation 2 hardware.
Emulator-only success is not sufficient for MagicGate, SIO2, IOP reboot,
USB/fileXio ownership or FMCB boot claims.

## Release backend

0.4.0 uses the pinned PS2SDK 2.0 SECRMAN 1.4 security backend:

```text
a13b5971ec0e39c7ba8b8559b80a4e81c8425352
```

The public build is the P0 production profile: `-O2 -G0`, R5900 tuning on
measured hot objects and synchronous production transport.

## Required regression data

Record at minimum:

```text
Inspector version / commit
ELF SHA-256
console model
ROMVER / region
Mecha version
MechaPwn fingerprint/mode if present
selected slot
card description
filesystem result
MagicGate/KELF result
selected FMCB KELF set
exact bind failure stage if any
recovery result
boot result when installation succeeds
```

For performance-specific experiments outside the stable release also record
build flags, workload, direction, buffering, sample count and latency
distribution.

## Card / filesystem controls

Known hardware classes include:

| Card | Filesystem | MagicGate |
| --- | --- | --- |
| Sony 8 MiB positive controls | PASS | FUNCTIONAL |
| Third-party MagicGate-capable card | PASS | FUNCTIONAL |
| Third-party card without functional MagicGate | PASS | NOT SUPPORTED / NO CARD AUTH ACK |

A non-Sony MagicGate-capable card has also completed FMCB installation and boot
successfully. Brand is therefore not a compatibility predicate.

## MagicGate regression

The logical/physical mapping must remain:

```text
libmc mc0/mc1: 0/1
SECR/SIO2 card channels: 2/3
```

Translation occurs only at the SECR boundary.

Positive-control KELFs must reach the required HEADER/BLOCK/Kbit/Kc/ICVPS2
stages. Negative-control cards must not suddenly become false positives.

## FMCB package/preflight regression

Before a transaction starts:

1. package discovery must resolve the complete source tree;
2. compatibility policy must select the correct CEX/DEX/MechaPwn payload class;
3. card filesystem/free-space gates must pass;
4. every **distinct selected KELF source** must bind successfully in RAM;
5. the normal card/USB personality must return after every security-session
   switch.

A failure at this stage must leave the card untouched.

## Transaction regression

For an installation that reaches write phase, verify:

1. recovery journal and card identity marker are durable;
2. pre-existing targets are captured before replacement;
3. newly created directories are tracked by ownership state;
4. each write is closed/reopened;
5. the complete destination is read back and compared;
6. recovery remains armed until the whole transaction commits;
7. a forced failure rolls back to the captured pre-install state;
8. a successful install survives a full power-cycle and boots FMCB.

## Qualified MechaPwn ENDVDPL case

Real hardware reproduced the following on a positively fingerprinted MechaPwn
DEX-like SCPH-50000 profile:

```text
FMCB.XLF      PASS
OSDSYS.XLF    PASS
OSD110.XLF    PASS
ENDVDPL.XRX   DOWNLOAD HEADER failure
```

ENDVDPL fails before Kbit/Kc/CardAuth. Reference FMCB also omits ENDVDPL on DEX.
The compatibility layer therefore omits CEX-only ENDVDPL only for real DEX or
positively fingerprinted MechaPwn DEX mode.

Generic DEX-like state is not enough.

## Recovery regression

Test both:

- normal rollback after an injected installer failure;
- startup with an existing recovery journal.

Recovery must verify the same target card before restoring/deleting anything.

Legacy v1 journals must be validated with their original checksum before
in-memory conversion to v2.

## USB/fileXio ownership regression

Historical hardware testing found cross-file corruption where Drebin logging
could land in recovery/image files.

The stable invariants are:

- correctness-critical fileXio runs in synchronous mode;
- FMCB/recovery owns `mass:` exclusively against durable logger writes;
- logger path attachment is deferred while storage ownership is held;
- mass metadata is synced before logger resume;
- logger RAM-ring checksum diagnostics stay clean.

Any recurrence of journal magic or binary recovery structures inside
`DREBIN.LOG` is a release blocker.

## Settings regression

Verify:

1. save from Settings with `SQUARE`;
2. temporary-file write and read-back succeeds;
3. restart;
4. all four settings reload;
5. saved display mode applies only after USB initialization;
6. corrupt/unsupported config leaves current/default settings intact.

## Stable CI regression

The release workflow must:

- run card/FMCB/Settings source invariants;
- build only one production ELF;
- keep `-mtune=r5900` in the production hot-object profile;
- force async transport flags to zero;
- exclude `r5900_bench` and `r5900_perf` from the public ELF;
- package SHA-256, provenance and license files.
