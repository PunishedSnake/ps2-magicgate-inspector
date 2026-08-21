# Roadmap

This roadmap describes intended feature boundaries, not promises of exact release dates. Hardware findings may reorder milestones.

## v0.1.0 — Columbo

**Theme:** establish the standalone Inspector.

- self-contained PS2 ELF;
- `mc0:` / `mc1:` selection;
- `mcGetInfo()` and raw MCMAN diagnostics;
- root-directory probe;
- deterministic 4 KiB write/read/compare/delete test;
- conservative health classification;
- manually confirmed format path for fresh or explicitly unformatted PS2 cards;
- PS2DEV CI build and artifact generation.

Status: **hardware-validated for the baseline filesystem pipeline on real PS2 hardware**.

## v0.2.0 — Briscoe

**Theme:** separate filesystem health, MagicGate capability and installation readiness.

In active development:

- decode and document more XMCMAN return codes;
- preserve per-stage result data;
- structured card / MagicGate / FMCB preflight views;
- coherent PS2SDK 2.0 XMCMAN + special SECRMAN stack;
- non-destructive staged MagicGate/KELF bind probe;
- report `DownloadHeader`, encrypted blocks, Kbit, Kc and ICVPS2 separately;
- optional `mass:` backend using PS2SDK iomanX/fileXio/USBD/USBHDFSD;
- read-only discovery and validation of a user-supplied `mass:/FMCB` package;
- console-region detection and installation-plan generation;
- keep FreeMcBoot payloads out of Inspector builds and releases.

Planned before enabling FMCB writes:

- required-space calculation;
- model/ROM handling, especially early Japanese consoles;
- destination collision inventory and backup plan;
- transactional copy/bind/write/read-back verification;
- explicit rollback/recovery reporting;
- hardware validation of the SECR stack and package source backend.

The FMCB commit path remains disabled until those prerequisites are proven on hardware.

## v0.3.0 — Poirot

**Theme:** identify what kind of card we are actually dealing with.

Planned research and implementation:

- richer card capability fingerprinting instead of relying only on the reported type;
- compare official, licensed and third-party cards;
- persist fingerprints from known hardware test cases;
- distinguish unusual-but-working cards from cards that fail a specific MagicGate stage;
- expand compatibility heuristics only when backed by captured hardware evidence.

## v0.4.0 — Kojak

**Theme:** controlled recovery.

Potential scope after enough real-card testing:

- more precise filesystem-damage classification;
- post-format verification pass;
- format result audit and re-query;
- warnings for suspicious capacity/type combinations;
- optional recovery-oriented diagnostics that remain separate from destructive repair.

The Inspector should never turn generic I/O failures into an automatic "format it" recommendation.

## v0.5.0 — Dale Cooper

**Theme:** make reports pleasant to use and pleasant to share.

Potential scope:

- cleaner UI while preserving raw data access;
- structured report export;
- build/version metadata in reports;
- PCSX2 regression harness for the parts that emulation can validate;
- test-case fixtures for parser/classification logic.

## v0.6.0+ — open investigation

Candidates include deeper card fingerprint databases, counterfeit/compatibility heuristics, expanded repair tooling and additional package-source backends. These should only be promoted into numbered milestones once preceding hardware evidence justifies them.

## v1.0.0 — Inspector Gadget

Reserved for the first release we are comfortable calling a stable general-purpose PS2 memory-card diagnostic utility.

A 1.0 release should require at minimum:

- broad hardware testing across official and third-party cards;
- stable detection and filesystem diagnostics;
- reliable cleanup of all temporary test data;
- well-understood formatting behavior;
- documented raw error interpretation;
- no known path where an ambiguous failure unlocks a destructive operation;
- reproducible builds and release artifacts;
- user-facing documentation sufficient for use without reading the source.

The codename is non-negotiably appropriate.
