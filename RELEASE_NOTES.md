# PS2 Memory Card Inspector 0.4.1 "Drebin"

0.4.1 is a focused bugfix release for the 0.4.0 Drebin line.

## Fixed

- Prevented FMCB installation from corrupting the USB FAT filesystem with
  fragments of diagnostic text.
- Extended exclusive `mass:` ownership across the complete FMCB
  revalidation/installation attempt, including MagicGate IOP handoffs.
- Retained each successfully preflight-bound KELF in EE RAM and reused it
  during the transaction, so the installer no longer repeatedly rebuilds the
  security IOP/USB stack after the recovery journal has become active.
- Kept PADMAN out of the temporary MagicGate security personality because the
  CardAuth path does not need a controller server.
- Fixed the false initial `RAM ring corruption at slot=0` diagnostic by
  initializing the checksum for the pre-populated session line.
- Clarified that `fileXioSync()` on the current PS2SDK USBHDFSD/vfat stack is
  not a real durability barrier when the driver returns the observed EIO path.

## Hardware validation

The corrected installer was re-tested on real PlayStation 2 hardware and no
longer damaged the USB filesystem during FMCB installation.

## Release asset

This maintenance release intentionally publishes only:

```text
MC_INSPECTOR-0.4.1-Drebin.ELF
```

No benchmark, Performance Lab, async/NOWAIT, raw-batch A/B or alternate
transport binaries are included.
