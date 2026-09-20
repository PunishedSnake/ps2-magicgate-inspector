# Roadmap

The roadmap is evidence-driven. 0.4.0 is the first release where the project is
not merely a diagnostic frontend but a verified write-capable memory-card tool.

## v0.4.0 — Drebin — release scope complete

Drebin includes:

- native GS UI with selectable display modes;
- card/filesystem diagnostics;
- hardware-validated MagicGate/CardAuth;
- Card Tools image export/verify/restore;
- filesystem-aware image/save browsing and transfer;
- guarded force-format path;
- cross-region FMCB 1.966 installation;
- compatibility profiles for CEX/DEX/MechaPwn and PSX/DESR classification;
- all-distinct-KELF compatibility preflight before card mutation;
- durable transaction/recovery journal;
- full read-back verification and rollback;
- persistent Settings config;
- P0 production optimization pass with R5900 tuning;
- one stable public ELF without benchmark variants.

## Post-0.4 qualification

The next work is not to add more destructive power. It is to broaden the
hardware matrix and simplify code that no longer needs development scaffolding.

Priorities:

- more retail CEX consoles across ROM revisions;
- real DEX hardware;
- more MechaPwn CEX/DEX configurations;
- ROM 1.80 / 2.10;
- early Japanese SCPH-10000/15000/18000;
- Chinese-region BCEXEC hardware;
- more third-party MagicGate controllers;
- representative modchips;
- ROM 2.20 versus 2.30 preparation/boot behavior.

## PSX/DESR

PSX/DESR is recognized by the compatibility layer, but the release does not yet
enable its separate X* payload manifest/twin-sign install path.

A future milestone should implement and qualify:

- `XFMCB.XLF`;
- `XUDNL.XRX`;
- `XENDVDPL.XRX`;
- PS2-to-PSX twin-sign key transfer;
- real DESR hardware tests.

## Future Card Tools work

Potential additions:

- richer save metadata views;
- exportable machine-readable diagnostics;
- verification-only audit of existing FMCB installations;
- compare installed files against a supplied package;
- safer batch migration between cards;
- better card-controller fingerprint database based on observed behavior rather
  than branding.

## Performance work after 0.4

P0 production choices are frozen for the stable release.

Further speed work belongs on research branches and must be justified by real
hardware measurements, including latency tails and correctness hashes.

Candidates include:

- alternate raw-card batch sizes;
- USB image read/write overlap;
- further copy elimination;
- additional R5900 hot-kernel tuning.

No future optimization should weaken transaction ownership, recovery or
verification.

## Non-goals

The project does not aim to:

- emulate MagicGate cryptography in software;
- fabricate MagicGate capability on cards that do not implement CardAuth;
- redistribute Sony ROM modules;
- redistribute FreeMcBoot payloads;
- replace real-hardware qualification with PCSX2 timing results.
