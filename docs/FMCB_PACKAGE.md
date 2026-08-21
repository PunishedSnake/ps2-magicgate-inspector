# User-supplied FreeMcBoot package

Briscoe does **not** ship FreeMcBoot payloads.  The Inspector only knows the
layout of a package supplied by the user and, in v0.2.0-dev3, performs a
strictly read-only preflight.

## USB layout

Put the package under one of these roots:

- `mass:/FMCB/`
- `mass0:/FMCB/`
- `mass1:/FMCB/`

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

The Inspector does not care which official/custom FMCB archive these files
were extracted from as long as the required files are present and non-empty.
No payload is copied into this repository or its release artifacts.

## Preflight result

Press **Circle** in Briscoe dev3.  The Inspector initializes its optional
PS2SDK USB/fileXio stack and reports:

- selected source root;
- console region from `rom0:ROMVER`;
- target system folder (`BIEXEC-SYSTEM`, `BAEXEC-SYSTEM`, `BEEXEC-SYSTEM` or
  `BCEXEC-SYSTEM`);
- number of required files found/missing;
- optional files found;
- total payload bytes visible to the source backend;
- number of KELFs that will eventually need to be bound with MagicGate.

`READY (READ-ONLY PREFLIGHT)` means only that the baseline package contract is
satisfied.  It does **not** mean installation has been attempted or enabled.

## Safety boundary in dev3

There is no FMCB commit function in dev3.  The package scanner performs only
`stat`/read-side source operations and never creates directories or writes to a
memory card.  The UI intentionally prints:

```text
INSTALL: DISABLED IN DEV3 (preflight is read-only)
```

The later commit stage must not be enabled until all of these are independently
validated on hardware:

1. target card filesystem health;
2. staged MagicGate/KELF bind capability;
3. package completeness;
4. console-region mapping;
5. required free-space calculation;
6. backup/rollback and read-back verification design.

Early Japanese boot-ROM update cases are intentionally not part of the first
normal-install preflight and will require explicit model/ROM handling before a
write path is allowed.
