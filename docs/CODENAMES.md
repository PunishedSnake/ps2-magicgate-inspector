# Release codenames

PS2 Memory Card Inspector development releases use alphabetical codenames. The codename is a development-line label, not an indication of compatibility with a particular console model or memory card.

## Current sequence

| Version line | Codename | Focus |
| --- | --- | --- |
| `0.1.x` | **Columbo** | Standalone memory-card filesystem inspection and hardware validation |
| `0.2.x` | **Briscoe** | MagicGate/KELF capability probing, FMCB package preflight and security-backend validation |

Future names should continue alphabetically where practical.

## Briscoe naming scope

Briscoe covers the transition from a filesystem-only inspector to a tool that can distinguish:

- ordinary PS2 filesystem usability;
- real MagicGate/CardAuth capability;
- KELF binding readiness;
- FMCB package readiness;
- infrastructure/protocol failures that are not valid card-capability verdicts.

The line deliberately remains read-only with respect to FMCB installation until a bind/write/read-back/rollback transaction is validated on hardware.

Internal experiment numbers such as the historical dev7/dev8/dev11/dev12/dev13 sequence are preserved in the changelog and MagicGate investigation document for debugging history, but should not become permanent component names in production source.
