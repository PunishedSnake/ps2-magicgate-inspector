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

Status: **build-verified, hardware-unverified**.

## v0.2.0 — Briscoe

**Theme:** better evidence before drawing conclusions.

Planned:

- decode and document more MCMAN return codes;
- preserve per-stage timing and result data;
- improve handling of full or nearly full cards so read-only diagnostics can still run;
- add a structured on-screen detail view;
- export a compact diagnostic report to mass storage when available;
- add hardware regression cases discovered during Columbo testing.

No MagicGate qualification yet unless hardware work proves the API path stable enough to move forward safely.

## v0.3.0 — Poirot

**Theme:** identify what kind of card we are actually dealing with.

Planned research and implementation:

- richer card capability fingerprinting instead of relying only on the reported type;
- distinguish filesystem compatibility from authentication/MagicGate capability;
- compare behavior of official, licensed and third-party cards;
- introduce a non-destructive standalone MagicGate probe if a clean test primitive can be established;
- keep any KELF test data legally and technically separate from copyrighted FMCB payloads.

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

Candidates include deeper MagicGate/KELF diagnostics, card fingerprint databases, counterfeit/compatibility heuristics and expanded repair tooling. These should only be promoted into numbered milestones once the preceding hardware evidence justifies them.

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
