# PS2 Memory Card Inspector

Standalone PlayStation 2 memory-card diagnostic utility. The current development milestone deliberately contains **no FMCB installation or force-install path**; it focuses only on identifying card state, exercising the filesystem safely, and offering guarded formatting when the card is fresh/unformatted or reports a broken/no-format filesystem state.

The older FreeMcBoot/MagicGate patch prototype remains in the repository for reference while the standalone application is developed.

## Current tests

For each physical memory-card port (`mc0:` and `mc1:`), the inspector reports:

- raw `mcGetInfo()` result code;
- reported card type;
- formatted flag;
- free-cluster count as returned by MCMAN;
- root-directory access result;
- a 4 KiB temporary-file write/read/compare/delete integrity test;
- temporary-file cleanup result;
- a conservative health classification.

The write test searches for an unused `__MCIxx.TMP` filename, never overwrites an existing file, writes a deterministic 4096-byte pattern, flushes and closes it, reads it back, compares the complete buffer, deletes it, and verifies cleanup.

## Formatting policy

Formatting is **never automatic**.

The format option is unlocked only when all of the following are true:

1. the device reports itself as a PS2 memory card; and
2. the card is explicitly unformatted/fresh (`sceMcResNoFormat` / formatted flag false), or a filesystem operation reports the same no-format/broken-filesystem state.

Authentication failures, card-detection failures, generic I/O failures, a full card, and unknown/non-PS2 card types do **not** unlock formatting.

Even when formatting is available, the user must first press **Triangle** to enter the confirmation state and then hold **L1 + R1** while pressing **Triangle** again. **Circle** cancels. Formatting erases all data on the selected card.

## Controls

- **Left / Right** — select Slot 1 / Slot 2
- **Cross** — rerun tests on the selected card
- **Start** — test both card slots
- **Triangle** — arm formatting, only when the safety policy allows it
- **L1 + R1 + Triangle** — confirm destructive formatting
- **Circle** — cancel format confirmation
- **Select** — exit

## Current scope

This standalone milestone does not yet perform the real KELF `SecrDownloadFile()` MagicGate binding test. That will return later as an independent diagnostic module once the basic card-health program has been exercised on real hardware.

## Build

The CI build runs the actual compilation inside `ps2dev/ps2dev:v1.0` while GitHub checkout/artifact actions stay on the modern Ubuntu host runner.

Output:

- `MC_INSPECTOR.ELF`
- `SHA256SUMS.txt`

The ELF embeds `freesio2.irx`, `freepad.irx`, `mcman.irx`, and `mcserv.irx`, so it does not depend on an FMCB installer directory or external IRX files at runtime.
