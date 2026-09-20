# FMCB compatibility and exception corpus

Status: current engineering corpus for the PS2 Memory Card Inspector cross-region
installer.

This document exists so compatibility behavior is not accumulated as folklore or
one-off `if (SCPH == ...)` branches. The installer is driven by observed runtime
capabilities and by explicit source-backed policy. When a static rule and a real
hardware capability probe disagree, the installer must fail closed or use a
narrow, already-qualified exception.

## Evidence order used here

For this corpus the practical order is:

1. current upstream source;
2. real-hardware reproduction;
3. older upstream source;
4. maintainer/developer documentation;
5. GitHub issues and forum reproductions;
6. historical anecdotes.

Forum/model reports are evidence for a failure class, not permission to encode a
model-number heuristic.

Each important claim below is labelled:

- **POTWIERDZONE**: current source or real-hardware reproduction;
- **CURRENT IMPLEMENTATION**: behavior of a named current commit;
- **HISTORYCZNE**: old installer/source/forum behavior;
- **INFERENCJA**: engineering conclusion from supported facts;
- **HIPOTEZA DO TESTU**: requires more real-hardware qualification.

## Pinned source snapshots

| Source | Snapshot | Role |
| --- | --- | --- |
| MechaResearch/MechaPwn | `2dc01450e37cdfbdb17f1cb66ca9cc2795677f4b` (2026-05-17) | current MechaPwn NVM/DEX/CEX policy |
| israpps/FreeMcBoot-Installer | `ac53a47a5c6eae675cc2611c7bebe62f56c7845c` (2025-03-28) | reference FMCB 1.9xx installer |
| NathanNeurotic/FMCB-Installer-Via-Any-Device | `52b73dd6054472c4e1cbe92ed740ba350dd7a7bf` (2026-07-28) | modernized current-toolchain installer fork |
| ps2dev/ps2sdk | `d317f8f0a2a413db38c5ef2b46ba927996983e0a` (2026-09-17) | current public SECR/fileXio behavior |
| TnA-Plastic/FreeMcBoot | `2e4eef9a1ec5612cde1121d185996fd08fecb930` (2021-07-10) | reconstructed vanilla FMCB 1.8b source |
| ps2homebrew/OSD-Initialization-Libraries | `0af6484eb7883b9fc4bb0ba92f0d19a528ac9106` (2025-03-17) | OSD/boot certification research |

Canonical URLs:

- https://github.com/MechaResearch/MechaPwn/commit/2dc01450e37cdfbdb17f1cb66ca9cc2795677f4b
- https://github.com/israpps/FreeMcBoot-Installer/commit/ac53a47a5c6eae675cc2611c7bebe62f56c7845c
- https://github.com/NathanNeurotic/FMCB-Installer-Via-Any-Device/commit/52b73dd6054472c4e1cbe92ed740ba350dd7a7bf
- https://github.com/ps2dev/ps2sdk/commit/d317f8f0a2a413db38c5ef2b46ba927996983e0a
- https://github.com/TnA-Plastic/FreeMcBoot/commit/2e4eef9a1ec5612cde1121d185996fd08fecb930
- https://github.com/ps2homebrew/OSD-Initialization-Libraries/commit/0af6484eb7883b9fc4bb0ba92f0d19a528ac9106

## Runtime inputs that are authoritative enough for policy

The installer must prefer these inputs over the chassis/model sticker:

1. numeric ROMVER plus ROMVER region;
2. ROMVER DEX byte;
3. presence of `rom0:PSXVER`;
4. read-only MechaCon version/state;
5. positive MechaPwn NVM fingerprint;
6. MechaPwn DEX-mode versus CEX-region-override state;
7. card type/filesystem health;
8. actual MagicGate/KELF bind result for every distinct selected KELF source.

SCPH model/datecode is useful diagnostic metadata, but it is not a sufficient
policy key. A known counterexample is the SCPH-9000x family: native FMCB behavior
is determined by ROM 2.20 versus 2.30, and chassis/datecode reports do not uniquely
identify the ROM behavior.

## Current reference FMCB install classes

### Retail/CEX PS2

**CURRENT IMPLEMENTATION, reference 1.9xx:** `PS2SysFiles` contains:

- `SYSTEM/FMCB.XLF -> B?EXEC-SYSTEM/osdmain.elf`
- `SYSTEM/ENDVDPL.XRX -> SYS-CONF/endvdpl.irx`

Therefore ENDVDPL is part of the CEX payload class.

### Real DEX PS2

**CURRENT IMPLEMENTATION, reference 1.9xx:** `DEXSysFiles` contains FMCB.XLF
but does not contain ENDVDPL.

The reference installer identifies a real DEX from `ROMVER[5] == 'D'`.

### PSX/DESR

**CURRENT IMPLEMENTATION, reference 1.9xx:** PSX is detected from
`rom0:PSXVER` and uses a distinct manifest:

- `XFMCB.XLF -> xosdmain.elf`
- `XUDNL.XRX -> xosdmain.irx`
- `XENDVDPL.XRX -> xendvdpl.irx`

Cross-PSX installation uses twin-signing: the installer signs a PS2 KELF, extracts
Kbit/Kc and installs those keys into the PSX KELF rather than pretending the PSX
payload is an ordinary PS2 KELF.

**CURRENT PROJECT POLICY:** PSX/DESR is classified explicitly but remains blocked
from the present PS2 cross-region transaction until its separate manifest and
twin-sign path are integrated.

## Early Japanese boot-ROM exceptions

**CURRENT IMPLEMENTATION, reference 1.9xx:**

- ROM 1.00J requires the specialized `OSDSYS.XLF -> BIEXEC-SYSTEM/osdsys.elf`;
- ROM 1.01J uses the specialized `OSD110.XLF -> BIEXEC-SYSTEM/osd110.elf`;
- early Japanese PCMCIA machines also require DEV9/ATAD/HDDLOAD support;
- pre-1.30 ROMs use versioned `osdXXX.elf` lookup;
- ROM 1.00/1.01 receive special treatment instead of blindly applying the
  generic rounding rule.

The cross-region manifest retains both early-Japan KELFs and the BI HDD support
files even when the host machine is newer, because the produced card is intended
to cover all I/A/E/C targets.

**HISTORYCZNE:** forum tables sometimes disagree about exact early-Japan
filenames. Current reference source wins over those tables.

## ROM 2.30 native-boot exception

**CURRENT IMPLEMENTATION, reference 1.9xx:** `IsUnsupportedModel()` classifies
`ROMVER >= 0x230` as unsupported for native FMCB installation.

**POTWIERDZONE by multiple maintainer reports:** early SCPH-9000x units with ROM
2.20 can use the OSD-update FMCB boot path, while ROM 2.30 disables it.

Forum references:

- https://www.psx-place.com/threads/freemcboot-question.36834/
- https://www.psx-place.com/threads/fmcb-fhdb-v1-9-series-release-thread.13413/
- https://www.psx-place.com/threads/mechapwn-by-mecharesearch.33498/page-2

**CURRENT PROJECT POLICY:** record this as a native-boot capability
(`ROM 2.30+ NATIVE FMCB BOOT BLOCKED`), not as a blanket refusal to prepare the
card. A ROM 2.30 host may still prepare media for another console or use another
bootstrap such as DEV1/OpenTuna/PS2BBL. Destination safety and native autoboot
capability are separate questions.

## Rare ROM warning class

**CURRENT IMPLEMENTATION, reference 1.9xx:** `IsRareModel()` explicitly calls
out ROM 1.80 and 2.10.

The current compatibility policy records these as a warning class. There is not
enough source evidence to invent extra payload rules solely from the rare-ROM
flag.

## MechaPwn exceptions

### Cross-region requirement

**POTWIERDZONE by current MechaPwn source/documentation:** changing the effective
region can move the OSD update lookup. Deckard behavior is especially explicit:
DEX policy uses the A region, while CEX region override can change the region
selected by the console.

Therefore a card intended to survive MechaPwn region changes must not be a
single-region install.

### Positive MechaPwn fingerprint

Current project probing reads the distinctive MechaPwn NVM seed from words
227..231 and combines it with the MechaCon DEX-like signal. This is stronger than
assuming that every DEX-like MechaCon is MechaPwn.

The installer distinguishes:

- real DEX;
- positively fingerprinted MechaPwn DEX mode;
- positively fingerprinted MechaPwn CEX region override;
- unqualified DEX-like state.

### ENDVDPL on MechaPwn DEX

**POTWIERDZONE, real hardware, 2026-09-20:** SCPH-50000, ROM 0170J, Mecha 5.00,
positive MechaPwn signature, DEX-like mode.

On the same card/session stack:

- eight FMCB.XLF destination binds succeeded;
- OSDSYS.XLF succeeded;
- OSD110.XLF succeeded;
- 128-byte ENDVDPL.XRX failed before CardAuth:
  `rc=-4713 stage=DOWNLOAD HEADER header=0 reply=0 blocks=0 kbit=-999 kc=-999`;
- automatic rollback restored the complete pre-install state.

This is a KELF-class/profile incompatibility, not a generic memory-card
MagicGate failure.

**CURRENT PROJECT POLICY:** omit CEX-only ENDVDPL only for:

1. real DEX; or
2. positive MechaPwn fingerprint plus MechaPwn DEX mode.

A generic/unidentified DEX-like MechaCon retains ENDVDPL and must pass the
actual KELF bind preflight. We do not generalize one hardware reproduction to all
odd MechaCon states.

### DVD-player caveats

**CURRENT/HISTORICAL evidence:** MechaPwn DEX/Force-Unlock and region changes have
multiple documented DVD-player side effects. Reports include DVD-Video becoming
unavailable and memory-card DVD-player updates behaving differently after a DEX
conversion.

Representative issue/forum references:

- https://github.com/MechaResearch/MechaPwn/issues/25
- https://github.com/MechaResearch/MechaPwn/issues/30
- https://github.com/MechaResearch/MechaPwn/issues/34
- https://github.com/MechaResearch/MechaPwn/issues/47
- https://github.com/MechaResearch/MechaPwn/issues/181
- https://www.psx-place.com/threads/corrupted-fmcb-after-using-mechapwn.37144/

These reports are not converted into broad automatic skip rules. They justify
keeping DVD-player KELFs as a distinct compatibility class and probing the exact
selected payload.

## Modchip exception class

A modchip is an external runtime modifier. Its marketing name/version is not a
stable enough contract for automatic installer policy.

**HISTORYCZNE:** a modchipped SCPH-30003 with Magic III was reported to complete
cross-model installation while cross-region/multi failed with `failed to bind
MagicGate`:

https://www.psx-place.com/threads/scph-10000-none-of-these-exploits-worked-for-me.45108/

Other maintainer reports describe some newer FMCB builds conflicting with some
modchips while older FMCB payloads work. MechaPwn has also collected chip-specific
behavior reports.

**INFERENCJA:** this is evidence for probing actual behavior, not for an
`if (MagicIII)` or `if (Modbo)` table.

**CURRENT PROJECT POLICY:** unknown modchip interference is handled by:

- no card write until all selected distinct KELFs bind successfully;
- stage-specific SECR failure reporting;
- fail-closed recovery/identity checks;
- preserving the exact source KELF name and stage in diagnostics.

## Old FMCB 1.8b lessons

**HISTORYCZNE, source:** FMCB 1.8b carries a hand-implemented MagicGate/MCID path
with direct SIO2 commands and explicit Kbit/Kc operations. Its changelog records
fixes for Chinese clone-card MCID behavior, early Japanese machines, slot
handling, x0006-era compatibility and modchip/OSDSYS interactions.

Those historical fixes identify useful failure classes:

- card identity/authentication;
- port/slot addressing;
- special boot-ROM update names;
- external boot/modchip interference.

They are not a reason to copy the old low-level MCID implementation into the
current installer. The current project uses the hardware-qualified SECR path and
stage diagnostics instead.

## Multi-install filesystem exception

**HISTORYCZNE/CURRENT reference behavior:** classic Multi Install saves space by
crosslinking filesystem entries. Current cross-region mode instead creates real
copies for the regional destinations.

Forum discussion:
https://www.psx-place.com/threads/questions-about-fmcb-installation-removal.40967/

**CURRENT PROJECT POLICY:** never use the old crosslink trick. The installer
uses real copies plus a durable rollback journal. Compatibility must not depend
on intentionally unusual MCFS topology.

## Current PS2SDK/toolchain lessons

**CURRENT IMPLEMENTATION, PS2SDK master at the pinned snapshot:**

- SECR is naturally stageable into header, block, Kbit, Kc and ICVPS2 RPCs;
- fileXio block mode/completion state is global on EE;
- NOWAIT operations return acceptance rather than a normal synchronous result.

The installer therefore keeps stage-specific KELF diagnostics and forces
`FXIO_WAIT` in correctness-critical installer/recovery/logger paths.

Production MagicGate binding remains on the pinned PS2SDK 2.0 SECRMAN 1.4
personality because that exact backend is already qualified on real hardware.
Current master is research/reference until an A/B hardware migration is done.

## Modern installer-fork lessons

The 2026 `FMCB-Installer-Via-Any-Device` fork demonstrates another class of
compatibility problems: updating the toolchain and storage stack exposed latent
file-mode/close/lifecycle bugs while broadening source-device support.

**INFERENCJA:** source transport and security policy must remain separate.
Whether a payload came from USB, MC, HDD, MMCE or MX4SIO must not silently change
which KELF profile is selected.

## Universal installer architecture

The installer follows this decision model:

1. **Probe runtime profile.** Read ROMVER/PSXVER, MechaCon state and MechaPwn
   fingerprint without writing NVM.
2. **Build a candidate manifest.** Current source-backed policy selects CEX,
   DEX, MechaPwn and early-Japan payload classes.
3. **Classify native boot separately.** ROM 2.30+ is recorded as unable to use
   native OSD-update autoboot, without confusing that with card-writing safety.
4. **Verify package contents.**
5. **Verify the card filesystem.**
6. **Bind-probe every distinct selected KELF source in RAM.** Duplicate
   destinations sharing one source are probed once.
7. **Require normal stack + recovery storage to return after every security
   personality switch.**
8. **Only after every probe passes, arm the durable recovery transaction and
   perform destination writes.**
9. **Write/close/reopen/verify.**
10. **Commit or rollback completely.**

The critical design rule is:

> Static compatibility knowledge chooses what should be attempted; real hardware
> decides whether the chosen KELF set is actually usable before the first card
> mutation.

This turns unknown modchips, clone cards and future MechaPwn/security-profile
differences into a preflight result instead of post-write archaeology.

## Distinct selected-KELF preflight

As of the compatibility-policy integration, the pre-install gate deduplicates by
source path.

A normal cross-region CEX plan currently probes:

1. `SYSTEM/FMCB.XLF`;
2. `SYSTEM/OSDSYS.XLF`;
3. `SYSTEM/OSD110.XLF`;
4. `SYSTEM/ENDVDPL.XRX`.

A real DEX or qualified MechaPwn DEX plan probes the first three because
ENDVDPL is not selected.

Any failure aborts before `FmcbRecoveryBegin()` and before a destination is
created, replaced or deleted.

## Known compatibility matrix

| Profile / condition | Static policy | Hardware gate | Current status |
| --- | --- | --- | --- |
| Retail CEX | include ENDVDPL | probe all four distinct KELFs | supported path |
| Real DEX ROMVER | omit CEX-only ENDVDPL | probe selected KELFs | reference-source confirmed |
| Positive MechaPwn DEX | omit CEX-only ENDVDPL | probe selected KELFs | real-hardware confirmed on SCPH-50000/ROM0170J/Mecha5.00 |
| Positive MechaPwn CEX override | retain CEX payload; cross-region card | probe selected KELFs | source-backed, broader hardware matrix pending |
| Unqualified DEX-like MechaCon | conservative CEX payload | probe decides | intentionally no guess |
| ROM 2.30+ | native FMCB autoboot blocked | card preparation may still proceed for other bootstrap/target | reference-source confirmed |
| ROM 1.80 / 2.10 | warning | normal capability probes | reference installer marks rare |
| Early Japan <=1.20 | retain special BI update + HDD-support payloads | probe selected early-Japan KELFs | reference-source confirmed |
| PSX/DESR | separate X* manifest + twin-sign required | future dedicated qualification | classified, not yet enabled |
| Unknown modchip | no chip-name rule | selected-KELF preflight | behavior-driven |
| Third-party/clone MC | no brand-name rule | filesystem + actual MagicGate/KELF probe | behavior-driven |
| Classic Multi Install | never generate crosslinks | n/a | intentionally unsupported |

## Open hardware matrix

These are **HIPOTEZY DO TESTU**, not current truths:

- qualified MechaPwn CEX region overrides across Dragon and Deckard;
- real DEX hardware using the current pinned SECR personality;
- ROM 1.80 and 2.10 with full selected-KELF preflight;
- SCPH-10000/15000/18000 with current recovery transaction;
- Chinese-region hardware and BCEXEC paths;
- PSX/DESR twin-sign installation;
- representative modchips with their boot interception both enabled and disabled;
- original Sony 8 MiB cards and several third-party cards that pass/fail different
  MagicGate stages;
- ROM 2.20 versus 2.30 preparation/boot behavior while keeping preparation and
  native autoboot as separate results.

Every new reproduction should record at minimum ROMVER, Mecha version, NVM
fingerprint state, card type/capacity, target slot, selected KELF set, exact bind
stage/result, installer commit and whether native OSD autoboot was attempted.

## Rule-promotion policy

A new exception is promoted from diagnostic evidence to automatic policy only
when one of the following holds:

1. current authoritative source explicitly defines the split; or
2. multiple real-hardware reproductions establish the split and it has a clear
   capability predicate; or
3. one real-hardware reproduction matches an already-existing authoritative
   source distinction, as with MechaPwn DEX + reference DEX ENDVDPL omission.

Otherwise the behavior remains a warning/candidate and the actual KELF preflight
decides.

This prevents the compatibility layer from becoming a fossilized collection of
forum anecdotes that future maintainers are afraid to touch.
