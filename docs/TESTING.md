# Testing

## Current confidence level

The current Columbo/Briscoe development line is **PS2DEV 2.0.0 build-verified and has passed complete real-hardware filesystem tests**.

The corrected ROM X-module stack initializes successfully on real hardware, both tested memory-card slots are detected, and the full filesystem integrity path can complete with `PASS`.

A green CI build still proves only that the source compiles and links into a valid PS2 ELF. Real-hardware evidence remains a separate quality signal.

## Startup validation

The normal application personality should pass, in order:

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

## Confirmed real-hardware filesystem result

A real PS2 test completed successfully on both populated slots.

One tested official Sony 8 MB card is specifically useful as a regression case because the FreeMcBoot installer refuses to install to it despite otherwise normal card operation. Inspector reported for that card:

- `mcGetInfo rc: 0`;
- reported type: `2 (PS2)`;
- formatted flag: `1`;
- free clusters: `7998`;
- root-directory rc: `0`;
- R/W stage: `DONE`;
- 4 KiB write/read/compare/delete rc: `0`;
- cleanup rc: `0`;
- final health: `PASS`.

The second populated official Sony card, which already contains a working FMCB installation, also completes ordinary filesystem testing with `PASS`.

This proves that the FMCB-rejected card can be detected as a PS2 memory card, traverse its filesystem, create a file, write and flush data, close/reopen it, read the data back byte-for-byte, and delete the temporary file successfully.

## Briscoe MagicGate history

### dev3
Rejected. The always-on experimental security/card stack caused `sceMcResFailResetAuth (-11)` during ordinary card access.

### dev4
Normal card access was restored by isolating the experimental SECR personality. The isolated session still produced `sceMcResFailResetAuth (-11)` before reaching KELF RPC.

### dev5
Switching from PS2SDK SECRMAN/SECRSIF 1.4 to the classic FreeMcBoot Installer 1.3 pair changed the isolated-session card result from `-11` to `sceMcResChangedCard (-1)` while returning valid PS2 metadata.

### dev6
`CHANGED CARD` was treated as a transient post-reboot notification. Both official Sony cards advanced to `DOWNLOAD HEADER` and both returned `HEADER BIND FAILED`.

This does **not** distinguish the cards. dev4-dev6 incorrectly used the already-installed `mc1:/BIEXEC-SYSTEM/osdmain.elf` as input to a second bind. An installed `osdmain.elf` is already a card-bound KELF. The FreeMcBoot Installer instead takes the raw package source `SYSTEM/FMCB.XLF`, binds it for the target card, and only then writes the result as `B?EXEC-SYSTEM/osdmain.elf`.

Therefore the dev6 `HEADER BIND FAILED` result is not a valid MagicGate verdict on either card.

### dev7
MagicGate/KELF probing accepts only a raw user-supplied FMCB package KELF:

- `mass:/FMCB/SYSTEM/FMCB.XLF`
- `mass0:/FMCB/SYSTEM/FMCB.XLF`
- `mass1:/FMCB/SYSTEM/FMCB.XLF`

Installed memory-card KELFs are no longer valid probe inputs. If no raw `FMCB.XLF` is present, the probe must stop before the isolated SECR session and report `RAW FMCB.XLF REQUIRED`.

The dev7 A/B hardware test must use the exact same raw `FMCB.XLF` against both target cards. Only then is a difference in `DOWNLOAD HEADER`, block transfer, Kbit, Kc or ICVPS2 meaningful.

## MechaPWN / DEX note

Console security mode can matter independently of the card. FreeMcBoot Installer documents that a CEX SECRMAN cannot authenticate cards correctly on a DEX and uses a custom security module with DEX-aware handling. MechaPWN can place supported consoles into a DEX-like configuration, so this remains a legitimate variable to record during MagicGate testing.

It is **not** yet proven to be the cause of the observed dev6 header failures because dev6 used an invalid already-bound KELF input.

## First card-validation matrix

| Case | Expected result | Formatting offered? | Status |
| --- | --- | --- | --- |
| No card inserted | `NO CARD` | No | Pending |
| Known-good PS2 card | `PASS` | No | **Confirmed** |
| FMCB-rejected but otherwise readable PS2 card | filesystem `PASS`; MG separately classified | No | **Filesystem/RW confirmed** |
| Known-good third-party PS2-compatible card | `PASS` or useful raw failure | No unless genuinely unformatted | Pending |
| Fresh/unformatted PS2 card | `UNFORMATTED / FRESH` | Yes | Pending |
| Full formatted card | `FULL - R/W TEST COULD NOT RUN` | No | Pending |
| PS1 card | Report PS1 type | No | Pending |
| Card with filesystem corruption / `sceMcResNoFormat` | Filesystem failure | Yes, only if reported as PS2 type | Pending |
| Authentication failure | `CARD AUTHENTICATION FAILURE` | No | Pending |
| Detection failure / electrically unstable card | `CARD DETECTION FAILURE` | No | Pending |

## Safe test procedure

1. Boot the current development `MC_INSPECTOR.ELF` with a known-good card first.
2. Confirm normal filesystem inspection still reaches `PASS`.
3. For MagicGate dev7, place a raw FMCB package at `mass:/FMCB/` (or mass0/mass1) and ensure `SYSTEM/FMCB.XLF` exists.
4. Use the same USB package for every target card comparison.
5. Press **Square** to run the RAM-only MagicGate probe.
6. Record the raw source path, session `mcGetInfo`, SECR RPC result, header result, block counts, Kbit and Kc.
7. After the session, confirm normal card inspection still works.
8. Do not test formatting on a card containing data you care about.

## Safety

The MagicGate probe is RAM-only. Bound output is not written to either memory card. FMCB support remains read-only preflight only; installation writes are disabled.

## CI expectations

Every active development branch should compile `MC_INSPECTOR.ELF` in CI. CI should:

- compile and link with PS2DEV 2.0.0;
- use the current EE compiler supplied by that environment;
- verify the artifact is a 32-bit MIPS PS2 ELF;
- calculate SHA-256;
- upload the ELF as an artifact;
- fail rather than silently publishing a missing output.
