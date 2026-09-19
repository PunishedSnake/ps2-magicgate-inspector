# P0 + FMCB cross-region integrated hardware test

Target: real PlayStation 2. PCSX2 may be used for UI/correctness inspection, but
it is not the arbiter for MagicGate, SIF/card timing or boot behavior.

This test validates the integrated branch as one program:

```text
P0 card/image/runtime paths
        +
hardware-validated MagicGate personality
        +
cross-region FMCB transaction
        +
persistent rollback/read-back verification
```

## Required metadata

Record:

- SCPH and hardware revision;
- MechaPwn mode/version and current ROMVER;
- card vendor/capacity and slot;
- USB device/filesystem;
- exact ELF SHA-256;
- PS2SDK security commit and IRX hashes from the artifact;
- FMCB package source/version;
- install verify mode;
- correctness hashes and any error/result codes.

## Stage A: integrated P0 smoke before installer writes

1. Boot the integrated ELF.
2. Confirm both card slots and USB backend enumerate.
3. Run normal card inspection on the target card.
4. Create one managed image with the normal P0 path.
5. Verify the image and retain its correctness hash.
6. Exercise the image browser/selective-read path far enough to prove the P0
   image filesystem still indexes the image correctly.
7. Confirm no new fileXio/mc/SIF error, hang or reset.

A failure here is a P0 integration regression and blocks FMCB testing.

## Stage B: MagicGate control

Run the normal Inspector MagicGate/KELF capability probe on the same card.

Required:

```text
CardAuth: PASS
KELF bind: PASS
normal Sony ROM X stack restored afterwards
```

Use the original Sony card first because it reproduced `Failed to bind
MagicGate` in the reference FMCB installers despite working MagicGate.

Then repeat the probe on the compatible third-party MagicGate card.

## Stage C: cross-region package preflight

Use a complete user-supplied FMCB package as documented in `FMCB_PACKAGE.md`.

The integrated manifest contains 28 destinations. For a normal retail/CEX ROM
path all 28 are selected and 11 are KELFs. A real DEX ROM intentionally skips
the CEX-only ENDVDPL entry, leaving 27 selected and 10 KELFs.

Preflight must report READY before installation is armed.

The boot alias set must include:

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

## Stage D: full install with enforced read-back

For the first hardware qualification use:

```text
Install read-back verify = ENFORCED
Preserve existing CNF    = as required by the test card
```

Do not remove the card or USB storage during the transaction.

For every selected destination the transaction must:

1. identify the previous target using P0's fail-closed inventory;
2. persist rollback state to the USB journal;
3. load the raw source into EE RAM;
4. bind each KELF through the isolated PS2SDK 2.0 SECRMAN 1.4 personality;
5. restore the Sony ROM X card stack;
6. write the destination;
7. close and reopen it;
8. compare the complete committed destination against the bound/source image.

Any failure must report the exact stage and target together with inventory,
backup, bind, write, verify, recovery and rollback result codes.

A bind failure on this branch is therefore materially different from the old
reference-installer `Failed to bind MagicGate`: it occurs inside the security
path already validated by the standalone Inspector probe.

## Stage E: reboot/boot validation

After a PASS/VERIFIED install:

1. exit cleanly;
2. power-cycle/reboot the real console;
3. boot with the installed card in the tested slot;
4. verify that FMCB/OSDSYS launch succeeds;
5. launch at least one configured ELF entry;
6. return/reboot again to prove the boot is repeatable.

For the MechaPwn SCPH-50000 this primarily validates that the all-region KELF
set is compatible with the modified MechaCon state. Cross-console/region
portability requires additional consoles and is a separate matrix.

## Stage F: P0 smoke after installation

Boot the integrated Inspector again and repeat:

- card inspection;
- MagicGate probe;
- image creation/verification or equivalent read-only P0 path;
- package preflight.

The correctness hash for an unchanged source image must still match. New
fileXio/mc/SIF errors, hangs or resets are regressions even if FMCB itself boots.

## Recovery policy

Do not deliberately power-cut the first qualification card merely to prove a
point. The persistent journal is a safety path, not a carnival attraction.

If a real failure leaves an ACTIVE/ROLLING_BACK journal:

1. preserve the USB recovery directory;
2. do not begin another install;
3. run the built-in recovery path;
4. record rollback rc and all destination diagnostics;
5. verify the card contents after recovery.

## Pass criteria

The integrated candidate passes the first whole-system qualification only when:

- P0 pre-install smoke passes;
- standalone MagicGate/CardAuth + KELF bind passes;
- cross-region preflight is READY;
- every selected install destination commits under ENFORCED read-back;
- no recovery journal remains after successful commit;
- real-console reboot launches FMCB;
- P0 post-install smoke passes;
- no new fileXio/mc/SIF error, descriptor ownership anomaly, hang or reset is observed.

Performance ranking is not part of this first integrated correctness test.
After correctness, rerun the P0 whole-system profile because the dominant
non-hidden cost may have moved.
