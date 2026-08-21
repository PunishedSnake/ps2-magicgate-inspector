# Testing

## Current confidence level

v0.1.0 **Columbo** is build-verified, but its first real-hardware run exposed an initialization defect before card inspection began. The current development branch fixes the protocol mismatch and is **PS2DEV 2.0.0 build-verified, hardware re-test pending**.

A green CI build proves that the source compiles and links into a valid PS2 ELF with the selected PS2DEV environment. It does not prove that every memory-card state is classified correctly on a real console.

## Startup validation

The current build prints each initialization stage. A successful startup should pass, in order:

1. SIF RPC initialization;
2. IOP reset using `SifIopReset(NULL, 0)`;
3. IOP synchronization;
4. `rom0:XSIO2MAN`;
5. `rom0:XPADMAN`;
6. `rom0:XMCMAN`;
7. `rom0:XMCSERV`;
8. `mcInit(MC_TYPE_XMC)`;
9. `padInit()` and `padPortOpen()`;
10. initial inspection of both card slots.

When reporting a startup problem, photograph or transcribe the **last visible line**. This is more useful than simply reporting that the application froze.

The original v0.1.0 hardware build reached `mcserv.irx OK` and then stalled because ordinary MCMAN/MCSERV had been paired with the XMC client protocol. That exact configuration is no longer used.

## First card-validation matrix

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
2. Confirm that initialization reaches `Initialization complete. Inspecting slots...`.
3. Confirm that the correct port is selected before running any operation.
4. Press **Cross** to inspect the selected card.
5. After a successful test, verify in another memory-card browser that no `/__MCIxx.TMP` file remains.
6. Do not test formatting on a card containing data you care about.
7. For format-path testing, use a disposable or freshly prepared PS2 card.

## What to record when reporting a problem

Please capture:

- console model;
- last startup line if initialization did not finish;
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

A photo of the Inspector screen is useful because the current build intentionally exposes raw XMCMAN values.

## Result interpretation

A `PASS` means the card completed the current filesystem integrity test. It is **not yet** a MagicGate certification result.

Likewise, failure does not automatically mean the physical card is dead. A failure can represent authentication, protocol compatibility, filesystem damage, insufficient free space or another condition that future releases may classify more precisely.

## CI expectations

Every active development branch should compile `MC_INSPECTOR.ELF` in CI. CI should:

- compile and link with PS2DEV 2.0.0;
- use the current EE compiler supplied by that environment;
- verify the artifact is a 32-bit MIPS PS2 ELF;
- calculate SHA-256;
- upload the ELF as an artifact;
- fail rather than silently publishing a missing output.

Hardware validation remains a separate release-quality signal and should be documented explicitly instead of being implied by a green build.
