# Roadmap

The roadmap is staged around hardware evidence. PS2 Memory Card Inspector should not turn a working diagnostic primitive into a destructive installer faster than it can verify and roll back its writes.

## v0.2.0 — Briscoe — complete

Hardware-validated release scope:

- Sony ROM X memory-card personality retained for ordinary filesystem work;
- temporary 4 KiB filesystem write/read/compare/delete test;
- raw `FMCB.XLF` acquisition from USB into EE RAM;
- isolated MagicGate/KELF session with normal-stack restoration;
- correct BIT semantics for large plaintext entries;
- failed-GET_KBIT instrumentation on the real CardAuth path without command replay;
- correct libmc logical `0/1` -> SECR/SIO2 physical `2/3` mapping;
- PS2SDK 2.0 SECRMAN 1.4 promoted to the single production security backend;
- `FUNCTIONAL` on two Sony 8 MB cards;
- `FUNCTIONAL` on a third-party 64 MB MagicGate-capable card;
- `NOT SUPPORTED / NO CARD AUTH ACK` on a third-party 64 MB card without functional MagicGate while ordinary storage remains usable;
- read-only FMCB package preflight;
- reproducible CI packaging with source provenance, SHA-256, project license, PS2SDK AFL-2.0 text, credits and third-party notices.

The legacy SECRMAN 1.3 comparison path is retained only in history/documentation and is no longer part of the release build.

## Next milestone: controlled bind/write/read-back experiment

Do not jump directly to a full FMCB installer. The next write-capable build should perform one deliberately narrow transaction on a disposable or fully backed-up card:

```text
preflight
  -> backup/rollback state
  -> bind raw KELF in RAM
  -> write one controlled target
  -> close/reopen
  -> full read-back
  -> verify bytes/metadata
  -> rollback on any failure
```

Success criteria:

- no unrelated files touched;
- exact on-card result can be read back and verified;
- any failure leaves enough information and backup state to restore the previous card state;
- normal ROM X filesystem access still works after the transaction;
- a power-cycle test is performed only after on-card verification passes.

## Next: FMCB installation transaction

Only after the single-file write path is hardware-validated:

- calculate required target space;
- validate the complete user-supplied package;
- create system/config directories safely;
- bind required KELF payloads using the validated SECRMAN 1.4 backend;
- copy non-KELF resources;
- close/reopen and verify every written file;
- preserve or back up replaceable existing content;
- maintain rollback metadata until the whole transaction commits;
- expose clear abort/failure states instead of leaving a partially installed card.

## v0.3.x — installer hardening and recovery

Possible scope:

- explicit backup/export before modifying an existing FMCB installation;
- installation journal and rollback/recovery path;
- verification-only mode for existing installations;
- compare existing target files against the user-supplied package;
- detect already-bound KELFs and reject them as raw bind sources;
- richer free-space and filesystem-health gating;
- clearer distinction between storage health and MagicGate capability.

## Later diagnostic work

Potential additions where they provide actionable information:

- export test reports to USB;
- card-controller/MagicGate behavior database based on observed protocol results rather than branding;
- timing/retry diagnostics for borderline cards;
- deeper CardAuth `50/51/52/53` detail view;
- optional research builds comparing historical security backends;
- additional safe filesystem-integrity checks.

## Non-goals

The project does not currently aim to:

- emulate MagicGate in software;
- bypass the Mechacon or replace Sony cryptographic hardware;
- fabricate MagicGate capability on a controller that does not implement CardAuth;
- redistribute Sony ROM modules;
- bundle FreeMcBoot payloads;
- automatically format or modify a card because a diagnostic stage fails.

The target remains narrow: identify what a card and console can actually do, then make future installation operations explicit, verifiable and recoverable.
