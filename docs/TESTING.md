# Testing

## Current confidence level

The current Columbo development line is **PS2DEV 2.0.0 build-verified and has now passed its first complete real-hardware card tests**.

The corrected ROM X-module stack initializes successfully on real hardware, both tested memory-card slots are detected, and the full filesystem integrity path can complete with `PASS`.

A green CI build still proves only that the source compiles and links into a valid PS2 ELF. Real-hardware evidence remains a separate quality signal, and not every memory-card state has been exercised yet.

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

The original v0.1.0 hardware build reached `mcserv.irx OK` and then stalled because ordinary MCMAN/MCSERV had been paired with the XMC client protocol. That exact configuration is no longer used.

## Confirmed real-hardware result

A real PS2 test of `0.1.1-dev — Columbo` completed successfully on both populated slots.

One of the tested PS2 memory cards is specifically useful as a regression case because the FreeMcBoot installer refuses to install to it despite otherwise normal card operation. Inspector reported for that card:

- `mcGetInfo rc: 0`;
- reported type: `2 (PS2)`;
- formatted flag: `1`;
- free clusters: `7998`;
- root-directory rc: `0`;
- R/W stage: `DONE`;
- 4 KiB write/read/compare/delete rc: `0`;
- cleanup rc: `0`;
- final health: `PASS`.

The second populated slot also completed with `PASS`.

This proves that the FMCB-rejected card can be detected as a PS2 memory card, traverse its filesystem, create a file, write and flush data, close/reopen it, read the data back byte-for-byte, and delete the temporary file successfully.

It does **not** yet prove that the card passes MagicGate/KELF binding. That distinction is the next important diagnostic target.

## First card-validation matrix

Test the following cases when practical:

| Case | Expected result | Formatting offered? | Status |
| --- | --- | --- | --- |
| No card inserted | `NO CARD` | No | Pending |
| Known-good PS2 card | `PASS` | No | **Confirmed** |
| FMCB-rejected but otherwise readable PS2 card | `PASS` or precise low-level failure | No | **Confirmed filesystem/RW PASS** |
| Known-good third-party PS2-compatible card | `PASS` or useful raw failure | No unless genuinely unformatted | Pending |
| Fresh/unformatted PS2 card | `UNFORMATTED / FRESH` | Yes | Pending |
| Full formatted card | `FULL - R/W TEST COULD NOT RUN` | No | Pending |
| PS1 card | Report PS1 type | No | Pending |
| Card with filesystem corruption / `sceMcResNoFormat` | Filesystem failure | Yes, only if reported as PS2 type | Pending |
| Authentication failure | `CARD AUTHENTICATION FAILURE` | No | Pending |
| Detection failure / electrically unstable card | `CARD DETECTION FAILURE` | No | Pending |

## Safe test procedure

1. Boot the current development `MC_INSPECTOR.ELF` with a known-good card first.
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
- whether the card is official/licensed/third-party;
- selected port (`mc0:` / `mc1:`);
- displayed card type;
- formatted flag;
- free-cluster value;
- raw `mcGetInfo` return code;
- root-directory return code;
- R/W stage;
- 4 KiB test return code;
- cleanup return code;
- whether the PS2 Browser can normally read the same card.

A photo of the Inspector screen is useful because the current build intentionally exposes raw XMCMAN values.

## Result interpretation

A `PASS` means the card completed the current filesystem integrity test. It is **not yet** a MagicGate certification result.

For the FMCB-rejected regression card, filesystem/RW failure is now ruled out as the reason for installer refusal. The remaining interesting area is the MagicGate/KELF binding path or installer-specific compatibility logic.

Likewise, failure does not automatically mean the physical card is dead. A failure can represent authentication, protocol compatibility, filesystem damage, insufficient free space or another condition that future releases may classify more precisely.

## CI expectations

Every active development branch should compile `MC_INSPECTOR.ELF` in CI. CI should:

- compile and link with PS2DEV 2.0.0;
- use the current EE compiler supplied by that environment;
- verify the artifact is a 32-bit MIPS PS2 ELF;
- calculate SHA-256;
- upload the ELF as an artifact;
- fail rather than silently publishing a missing output.
