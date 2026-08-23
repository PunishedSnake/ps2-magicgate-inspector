# MechaPwn-aware FMCB planning

The 0.4 installer treats the console's *active* system-update policy as hardware state, not as a sticker-region assumption.

## Read-only evidence

`console_profile.c` gathers four independent signals without writing MechaCon/NVM state:

1. `rom0:ROMVER` for the OSDSYS system-update region and boot-ROM revision.
2. MechaCon SCMD `0x03/00` for the MechaCon generation and DEX-like revision bit.
3. On newer units, SCMD `0x36` for regional parameters as diagnostics.
4. MechaCon NVM words `227..231` through read-only SCMD `0x0A`. MechaPwn writes the distinctive `MechaPwn\0 EC` key seed there when its region patch is active. Matching this seed is treated as a direct MechaPwn-region-patch fingerprint, not merely inferred from a DEX-like MechaCon revision.

The installer never issues the NVM write command used by MechaPwn.

## Policy matrix

| Runtime profile | 0.4 normal-install policy |
| --- | --- |
| Stock retail CEX | Install only the active ROMVER region. |
| Real DEX ROM | Use the reference DEX compact manifest; `ENDVDPL` is omitted. |
| Pre-Deckard MechaPwn, DEX-like | Active ROMVER region remains the target. Mark as an unlock-aware compact candidate, but retain the CEX `ENDVDPL` payload until hardware tests prove omission safe. |
| Pre-Deckard MechaPwn, CEX | Active ROMVER region remains the target; no all-region assumption is made. |
| Deckard MechaPwn, DEX-like | Expect active `A` region after reboot. A non-`A` ROMVER while the DEX-like patch is detected is treated as a transitional/ambiguous state and installation is blocked. |
| Deckard MechaPwn, CEX | A one-region install is intentionally blocked. MechaPwn can move the system-update region under CEX, so a replacement installer must use a verified cross-region transaction if the card is expected to survive later region changes. |
| Unknown DEX-like state without MechaPwn fingerprint | Treat as region-unlocked capability, but do not claim it is MechaPwn. |

This follows the MechaPwn project's own warning that Deckard users should have cross-region FMCB before changing regions. The important distinction is that the existing reference installer solves this by duplicating/cross-linking region targets; the 0.4 transaction engine will not emulate that behavior until rollback, space accounting and read-back verification cover every regional destination.

## Compact-install rule

"Region unlocked" and "safe to omit a file" are deliberately separate decisions. At present:

- real DEX omission of `ENDVDPL` is enabled because it matches the reference FMCB installer;
- MechaPwn/DEX-like omission is only reported as a potential saving;
- Deckard CEX region switching requires cross-region coverage rather than a smaller one-region install.

That means the planner may discover that a console *could* support a smaller policy without silently turning a hardware inference into a destructive write decision.
