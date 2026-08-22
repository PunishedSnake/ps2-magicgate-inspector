# Roadmap

The roadmap is intentionally staged around hardware evidence. PS2 Memory Card Inspector should not turn a working diagnostic primitive into a destructive installer faster than it can verify and roll back its writes.

## v0.2.0 — Briscoe

### Completed / hardware-validated

- Normal Sony ROM X memory-card personality retained for ordinary filesystem work.
- Non-destructive 4 KiB filesystem read/write/compare/delete test.
- Raw `FMCB.XLF` acquisition from USB into EE RAM.
- Isolated MagicGate/KELF security session with normal-stack restoration afterward.
- Correct BIT semantics: large plaintext blocks no longer inherit the SECRSIF `0x400` RPC limit.
- Source-level GET_KBIT failure instrumentation without post-failure CardAuth replay.
- Correct logical libmc `0/1` -> physical SECR/SIO2 `2/3` card-port mapping.
- Full MagicGate/KELF probe PASS on two official Sony 8 MB cards using the `fmcb13` baseline.
- Full PASS on a third-party 64 MB MagicGate-capable card.
- Clean first-command no-ACK failure on a third-party 64 MB non-MagicGate card while its filesystem remains usable.
- User-facing capability classification that separates `FUNCTIONAL`, unsupported/no-ACK, protocol errors and indeterminate infrastructure failures.
- Read-only FMCB package preflight.

### Completed / build-validated, hardware pending

- Selectable `ps2sdk14` profile using PS2SDK 2.0 SECRMAN 1.4, matching SECRSIF and matching PS2SDK 2.0 memory-card stack.
- Equivalent GET_KBIT failure record format for direct comparison with `fmcb13`.
- Reproducible CI build of the instrumented 1.4 profile.

### Next: validate PS2SDK SECRMAN 1.4 on hardware

Run `ps2sdk14` against the same four-card matrix:

1. official Sony 8 MB positive control A;
2. official Sony 8 MB positive control B;
3. third-party 64 MB negative control;
4. third-party 64 MB MagicGate-capable positive control.

The modern backend should not become the default until it reproduces both the positive and negative behavior of the validated compatibility baseline and restores the normal ROM card stack correctly after every probe.

### Next: controlled bind/write/read-back experiment

Once a default security backend is selected, add a deliberately narrow write experiment rather than a full installer.

Required transaction:

```text
preflight
  -> backup/rollback state
  -> bind raw KELF in RAM
  -> write one controlled target
  -> close/reopen
  -> full read-back
  -> verify
  -> rollback on any failure
```

This stage should be tested only with a fully backed-up/disposable card until failure recovery is proven.

### Next: FMCB installation transaction

Only after the single-file write path is hardware-validated:

- calculate required target space;
- validate complete user-supplied package;
- create target system/config directories safely;
- bind all required KELF payloads using the validated backend;
- copy non-KELF resources;
- verify every written file after reopening it;
- preserve or back up replaceable existing content;
- maintain rollback metadata until the entire transaction commits;
- provide clear abort/failure state instead of leaving a partially installed card.

### Release cleanup before Briscoe stable

- settle the default SECR backend;
- remove development-only backend-selection behavior from ordinary release builds unless keeping it is useful for an advanced diagnostics mode;
- strip stale development labels from UI;
- keep low-level diagnostics available on the MagicGate detail page or debug build;
- generate clean release checksums;
- include `LICENSE`, credits and third-party notices in release packaging;
- ensure release provenance records the exact PS2SDK/upstream revisions.

## v0.3.x — installer hardening / recovery

Possible scope after the first safe installer transaction exists:

- explicit backup/export before modifying an existing FMCB installation;
- installation journal and rollback/recovery path;
- verification-only mode for an existing installation;
- compare existing target files against the user-supplied package;
- detect already-bound KELFs and avoid treating them as raw sources;
- richer free-space and filesystem-health gating;
- clearer distinction between card storage health and MagicGate capability.

## Later diagnostic work

Potential future additions, only where they provide actionable information:

- persistent test report export to USB;
- card-controller/MagicGate behavior database based on observed protocol results rather than branding;
- timing/retry diagnostics for borderline cards;
- deeper protocol detail view for CardAuth `50/51/52/53` failures;
- optional comparison of multiple security backends in one test session;
- additional safe filesystem integrity checks.

## Non-goals

The project does **not** currently aim to:

- emulate MagicGate in software;
- bypass the MechaCon or replace Sony's cryptographic hardware;
- fabricate MagicGate capability on a card whose controller does not implement CardAuth;
- redistribute Sony ROM modules;
- bundle FreeMcBoot payloads;
- automatically format or modify a card simply because a diagnostic stage fails.

The useful target is narrower: identify what a card and console can actually do, then make any future installation operation explicit, verifiable and recoverable.
