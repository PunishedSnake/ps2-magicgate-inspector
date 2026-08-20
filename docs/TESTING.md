# Testing

## Current confidence level

v0.1.0 **Columbo** is currently **build-verified, hardware-unverified**.

A green CI build proves that the source compiles and links into a valid PS2 ELF with the selected PS2DEV environment. It does not prove that every memory-card state is classified correctly on a real console.

## First hardware validation matrix

Test the following cases when practical:

| Case | Expected result | Formatting offered? |
| --- | --- | --- |
| No card inserted | `NO CARD` | No |
| Known-good official PS2 card | `PASS` | No |
| Known-good third-party PS2-compatible card | `PASS` or useful raw failure | No unless genuinely unformatted |
| Fresh/unformatted PS2 card | `UNFORMATTED / FRESH` | Yes |
| Full formatted card | `FULL - R/W TEST COULD NOT RUN` | No |
| PS1 card | Report PS1 type | No |
| Card with filesystem corruption / `sceMcResNoFormat` | Filesystem failure | Yes, only if reported as PS2 type |
| Authentication failure | `CARD AUTHENTICATION FAILURE` | No |
| Detection failure / electrically unstable card | `CARD DETECTION FAILURE` | No |

## Safe test procedure

1. Boot `MC_INSPECTOR.ELF` with a known-good card first.
2. Confirm that the correct port is selected before running any operation.
3. Press **Cross** to inspect the selected card.
4. After a successful test, verify in another memory-card browser that no `/__MCIxx.TMP` file remains.
5. Do not test formatting on a card containing data you care about.
6. For format-path testing, use a disposable or freshly prepared PS2 card.

## What to record when reporting a problem

Please capture:

- console model;
- card manufacturer and advertised capacity;
- whether the card is official Sony hardware, licensed, or third-party;
- selected port (`mc0:` / `mc1:`);
- displayed card type;
- formatted flag;
- free-cluster value;
- raw `mcGetInfo` return code;
- root-directory return code;
- 4 KiB test return code;
- cleanup return code;
- whether the PS2 Browser can normally read the same card.

A photo of the Inspector screen is useful because the current build intentionally exposes raw MCMAN values.

## Result interpretation

A `PASS` means the card completed the current filesystem integrity test. It is **not yet** a MagicGate certification result.

Likewise, failure does not automatically mean the physical card is dead. A failure can represent authentication, protocol compatibility, filesystem damage, insufficient free space or another condition that future releases may classify more precisely.

## CI expectations

Every active development branch should compile `MC_INSPECTOR.ELF` in CI. CI should:

- compile and link with PS2DEV;
- verify the artifact is a 32-bit MIPS PS2 ELF;
- calculate SHA-256;
- upload the ELF as an artifact;
- fail rather than silently publishing a missing output.

Hardware validation remains a separate release-quality signal and should be documented explicitly instead of being implied by a green build.
