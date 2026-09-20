# Release codenames

The project uses alphabetical detective/police codenames for stable release lines. The codename is a development-line label, not an indication of compatibility with a particular console model or memory card.

## Current sequence

| Version line | Codename | Focus |
| --- | --- | --- |
| `0.1.x` | **Columbo** | Standalone memory-card filesystem inspection and hardware validation |
| `0.2.x` | **Briscoe** | MagicGate/KELF capability probing, FMCB package preflight and security-backend validation |
| `0.4.x` | **Drebin** | Full card toolkit: verified FMCB transactions, raw card imaging, restore/format recovery and broader card management |

The earlier working name **Gadget** is retired for 0.4. The scope has moved well beyond an inspector-only release.

## Drebin naming scope

Drebin keeps the hardware-validated Briscoe diagnostic and CardAuth boundaries, then adds write-capable features behind explicit verification and recovery contracts. The line covers:

- filesystem diagnostics and MagicGate/CardAuth capability testing;
- cached raw FMCB KELF sources in EE RAM for repeated security transactions;
- guarded normal FreeMcBoot install/update recovery work;
- standard PCSX2 `.ps2` and OPL-style `.vmc` card images;
- verified exact image restore and backup-before-force-format behavior;
- a dedicated Card Tools surface instead of treating format as an incidental diagnostic action.

Cross-capacity migration is deliberately distinct from raw restore: a smaller image must be imported into a natively formatted larger filesystem rather than copying its smaller-card superblock verbatim.

Internal experiment numbers such as the historical dev7/dev8/dev11/dev12/dev13 sequence are preserved in the changelog and MagicGate investigation document for debugging history, but should not become permanent component names in production source.
