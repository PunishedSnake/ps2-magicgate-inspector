# Oracle fixtures (third-party output)

These files were produced by **other tools** from the Drebin fixtures, so the
Drebin encoders can be checked byte-for-byte against independent
implementations. They are data only; no third-party code is in this repository.

| File | Produced by | From |
| --- | --- | --- |
| `card.vmp` | MemcardRex `SaveMemoryCard(..., vmp)` | `ps1/card.mcd` |
| `card.mcx` | MemcardRex `SaveMemoryCard(..., mcx)` | `ps1/card.mcd` |
| `ps1_save1.memcardrex.psv` | MemcardRex `SaveSingleSave(slot 1, psv)` | `ps1/card.mcd` |
| `ps2_save.apollo.psv` | Apollo Save Tool `generateHash()` (ps3-psvresigner) signing an unsigned Drebin PS2 PSV | `ps2/save.psu` |

MemcardRex and Apollo produce identical PS1 PSV and VMP signatures for the
same input, which was checked when these were generated (2026-11-09,
MemcardRex master, apollo-ps3 master). Regeneration needs .NET 8 and
libmbedtls and is not part of CI.
