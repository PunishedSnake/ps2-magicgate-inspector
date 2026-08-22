# User-supplied FreeMcBoot package

Briscoe does **not** ship FreeMcBoot payloads. The Inspector understands a baseline package layout supplied by the user and currently performs a strictly **read-only preflight**.

## USB layout

Place the package under one of these roots:

```text
mass:/FMCB/
mass0:/FMCB/
mass1:/FMCB/
```

The first existing root is selected.

Baseline normal-install layout:

```text
FMCB/
├── SYSTEM/
│   ├── FMCB.XLF
│   ├── ENDVDPL.XRX
│   ├── FMCB.ICN
│   └── ICON.SYS
└── SYS-CONF/
    ├── FMCB_CFG.ELF
    ├── FREEMCB.CNF
    ├── ICON.SYS
    ├── SYSCONF.ICN
    ├── USBD.IRX          (optional)
    └── USBHDFSD.IRX      (optional)
```

The Inspector does not require a specific archive name. Required files must exist and be non-empty. The package itself is not copied into this repository or bundled with project release artifacts.

## Raw KELF used by the MagicGate probe

`SYSTEM/FMCB.XLF` is also the raw, unbound KELF used by the RAM-only MagicGate capability test.

An already installed memory-card file such as:

```text
mc?:/B?EXEC-SYSTEM/osdmain.elf
```

must **not** be substituted for it. An installed `osdmain.elf` has already been card-bound, while the capability probe needs raw bind input.

## Preflight result

Press **Circle** to scan the package. The Inspector reports:

- selected source root;
- console region from `rom0:ROMVER`;
- normal FMCB target system folder (`BIEXEC-SYSTEM`, `BAEXEC-SYSTEM`, `BEEXEC-SYSTEM` or `BCEXEC-SYSTEM`);
- number of required files found and missing;
- optional files found;
- total payload bytes visible to the source backend;
- number of KELFs that will eventually require binding.

`READY (READ-ONLY PREFLIGHT)` means only that the current package contract is satisfied. It does **not** mean the card has been modified or that installation is enabled.

## Current safety boundary

There is no enabled FMCB installation transaction in Briscoe. The package scanner performs source-side discovery/stat operations and never creates FMCB directories or writes package payloads to a memory card.

The UI deliberately reports that installation is disabled.

Before a write path is allowed, these areas must be independently validated:

1. target card filesystem health;
2. functional MagicGate/KELF capability;
3. package completeness;
4. console-region mapping;
5. required free-space calculation;
6. backup and rollback state;
7. KELF bind output;
8. target write;
9. close/reopen and full read-back verification.

The MagicGate bind primitive is now hardware-validated with the `fmcb13` security backend, including positive and negative card controls. That does **not** remove the need to validate the on-card write transaction separately.

## Planned first write-capable milestone

The project should not jump directly to a general installer. The first write test should be deliberately narrow:

```text
preflight
  -> backup/rollback preparation
  -> bind one raw KELF in RAM
  -> write one controlled target
  -> close/reopen
  -> read back the entire file
  -> verify
  -> rollback on failure
```

Only after this behaves safely on backed-up hardware should the wider FMCB file set be enabled.

## Early Japanese models

Early Japanese boot-ROM update cases require explicit model/ROM handling and are intentionally outside the first normal-install transaction. They must not be inferred solely from the generic region folder mapping.

## Redistribution

FreeMcBoot files are user-supplied and retain the terms of their upstream project. PS2 Memory Card Inspector does not relicense those payloads. See the repository's `THIRD_PARTY_NOTICES.md` for project build dependencies and provenance.
