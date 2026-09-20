# User-supplied FreeMcBoot package

PS2 Memory Card Inspector does **not** redistribute FreeMcBoot payloads. The
cross-region installer consumes a complete user-supplied FMCB package and keeps
all card writes behind filesystem, MagicGate and recovery preconditions.

## Why this installer exists

The reference FMCB installer can report `Failed to bind MagicGate` on hardware
where the Inspector's isolated PS2SDK 2.0 SECRMAN 1.4 personality has already
completed CardAuth and KELF binding successfully. The Inspector therefore does
not import the reference installer's security-session lifecycle. It reuses the
hardware-validated path:

```text
logical mc0/mc1
  -> isolated PS2SDK 2.0 SECRMAN 1.4 + matching SIO2/MCMAN
  -> translate 0/1 to physical SIO2 2/3 only at the SECRMAN/CardAuth boundary
  -> instrumented SECRSIF header/block/Kbit/Kc/ICVPS2 bind in EE RAM
  -> restore Sony ROM X card stack
  -> write, close, reopen and verify through ordinary libmc
```

Raw libmc/MCSERV/MCMAN calls continue to use logical ports 0/1. The +2 mapping
is deliberately confined to the SECR boundary. See `MAGICGATE.md` and
`MEMORY_CARD_PORT_DOMAINS.md`.

## USB layout

The package can be below `mass:/FMCB`, `mass0:/FMCB`, `mass1:/FMCB`, or a
recursively discovered directory containing `SYSTEM/FMCB.XLF`.

A cross-region package must contain at least:

```text
FMCB/
├── SYSTEM/
│   ├── FMCB.XLF
│   ├── ENDVDPL.XRX
│   ├── OSDSYS.XLF
│   ├── OSD110.XLF
│   ├── DEV9.IRX
│   ├── ATAD.IRX
│   ├── HDDLOAD.IRX
│   ├── FMCB.ICN
│   ├── BIICON.SYS
│   ├── BEICON.SYS
│   ├── BAICON.SYS
│   └── BCICON.SYS
└── SYS-CONF/
    ├── FMCB_CFG.ELF
    ├── FREEMCB.CNF
    ├── ICON.SYS
    ├── SYSCONF.ICN
    ├── USBD.IRX
    └── USBHDFSD.IRX
```

`ENDVDPL.XRX` follows the reference installer and remains required in the
source package so one package can serve CEX and DEX-like targets. It is selected
for normal retail/CEX profiles, but omitted for a real DEX ROM and for a
positively fingerprinted MechaPwn DEX-mode profile. Real-hardware qualification
on SCPH-50000 showed the 128-byte ENDVDPL KELF being rejected at SECR download
header while FMCB.XLF/OSDSYS.XLF/OSD110.XLF bound normally. A merely DEX-like
MechaCon without the MechaPwn fingerprint does not trigger this omission.

## Cross-region destination set

The transaction follows the reference FMCB cross-region alias set without
legacy multi-install filesystem crosslinks:

```text
BIEXEC-SYSTEM/osd130.elf
BIEXEC-SYSTEM/osdmain.elf
BEEXEC-SYSTEM/osd130.elf
BEEXEC-SYSTEM/osdmain.elf
BAEXEC-SYSTEM/osd120.elf
BAEXEC-SYSTEM/osd130.elf
BAEXEC-SYSTEM/osdmain.elf
BCEXEC-SYSTEM/osdmain.elf

BIEXEC-SYSTEM/osdsys.elf
BIEXEC-SYSTEM/osd110.elf
```

The final two paths are the early Japanese ROM update payloads. The installer
also installs the reference DEV9/ATAD/HDDLOAD files in `BIEXEC-SYSTEM`, the
region-specific icon resources in all four system folders, and the common
`SYS-CONF` payload.

This is a **real-copy cross-region install**. It intentionally does not use the
old Multi Install crosslink trick.

## Transaction and recovery model

Before the first destination is changed, the installer:

1. re-runs card filesystem verification;
2. re-runs the hardware MagicGate/CardAuth test;
3. re-probes the complete USB package and active ROMVER/MechaCon policy;
4. simulates free-space use including all four regional directories;
5. creates a dual-slot checksummed recovery journal on USB;
6. arms the target card with the transaction identity marker.

For each destination it then:

1. persists the old destination, or its absence, into the USB recovery journal;
2. reads the raw source into EE RAM;
3. binds KELFs through the isolated, hardware-validated SECRMAN personality;
4. restores the normal Sony ROM X filesystem stack;
5. writes and flushes the destination;
6. closes and reopens it;
7. performs the configured read-back verification.

Recovery version 2 records all four `BI/BE/BA/BCEXEC-SYSTEM` directories plus
`SYS-CONF`, so an interrupted cross-region transaction can restore replaced
files and remove only directories created by that transaction.

## MechaPwn policy

The MechaPwn project requires a cross-region FMCB setup when region changes can
move the OSDSYS update lookup, most notably on Deckard units. This installer
therefore always prepares the complete I/A/E/C destination set. It does not
downgrade to a one-region install merely because the console currently reports
one stable region.

A ROMVER/MechaCon transition that is internally inconsistent is still rejected.
The user should reboot into a settled state before installation.

## Raw KELF rule

`SYSTEM/FMCB.XLF` must be the raw, unbound installer source. An already
installed `mc?:/B?EXEC-SYSTEM/osdmain.elf` is card-bound and is not a valid
replacement source.

## Redistribution

FreeMcBoot files are user-supplied and retain the terms of their upstream
project. PS2 Memory Card Inspector does not relicense or bundle those payloads.
See `THIRD_PARTY_NOTICES.md` for build dependencies and provenance.
