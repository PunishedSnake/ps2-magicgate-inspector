# PS2 Memory Card Inspector 0.4.2-rc1 "Drebin" (pre-release)

**This is a release candidate.** Everything below passes the host test suite
(with ASan/UBSan) and the PS2 build, and the signed/encrypted formats are
byte-identical to MemcardRex and Apollo Save Tool. **None of the new card
write paths have been run on real hardware yet.** Keep a backup of any card
you use with it, and please report results.

## Fixed

- **`.ps2` ECC now matches PCSX2 and mymc.** 0.4.0/0.4.1 left bit 7 set in the
  second ECC byte of about half of all 128-byte chunks. New exports are
  standard; older Drebin images still verify and restore (reported as
  `ecc=legacy-drebin-0.4`).
- **Save titles decode.** `icon.sys` titles are Shift-JIS byte strings; they
  were read as 16-bit values, so nearly every title fell back to the folder
  name.
- The title resolver now uses the same superblock rules as the image browser.

## New

- **Import almost any save container**, detected by content (the extension
  is only a hint):
  - PS2: `.psu`, `.max`/`.pws`, `.cbs`, `.sps`/`.xps`, `.psv`
  - PS1 saves: `.mcs`/`.ps1`, `.psx`/`.mcb`/`.mcx`/`.pda`, `.psv`, raw saves
  - PS1 cards: `.mcd`/`.mcr`/`.mc`/`.bin`/`.ddf`, POPStarter `.VMC`,
    `.gme`/`.dex`, `.mem`/`.vgs`, `.vmp`, encrypted `.mcx`
- **PS1 memory cards** as import targets. Card images import all of their
  saves or none.
- **Convert on USB** with a choice of output: PS2 → `.psu` / signed `.psv`;
  PS1 save → `.mcs` / signed `.psv` / `.psx`; PS1 card → `.mcd` / `.gme` /
  `.vgs` / signed `.vmp` / encrypted `.mcx`.
- **Raw PS1 saves**: the save name is shown and editable on the pad, with a
  product-code check and a guess from the file name.
- **Replace file in save**: swap one raw data file inside a PS2 save, after a
  verified `.psu` backup, through a verified temporary copy.
- SHA-256 of every image, and `.mci.txt` sidecar v2 with superblock health
  (bad blocks, backup blocks, card flags). Existing keys are unchanged.

## Things to know

- On a PS1 card, the memory-card driver clears deleted-but-recoverable save
  entries whenever a save is created. The import screen warns about this.
- A bad PSV/VMP signature or MCX checksum is reported, not fatal: edited
  saves are often unsigned.
- Files carried over from MAX saves get the card's own creation time (the
  format stores none).
- A PS3 has not yet been shown to accept a Drebin-signed PS2 `.psv`.

## Hardware checks wanted

1. Export a `.ps2`; `mymcplusplus ecc_check` is clean and `sha256` in the
   sidecar matches `sha256sum`.
2. Restore an old 0.4.1 `.ps2` image.
3. Import a GameFAQs `.max`, `.cbs`, `.sps` and `.psv`; the game loads them.
4. Import a `.mcs` and a `.gme` onto a PS1 card; a PS1 game sees the saves.
5. Name conflicts and a full PS1 card are refused before any write.
6. Replace a file inside a PS2 save; the backup appears and the game loads it.
7. Convert a `.gme` to `.mcd` and load it in POPStarter or an emulator.

## Release assets

- `MC_INSPECTOR-0.4.2-rc1-Drebin.ELF` - the program
- `MC_INSPECTOR-0.4.2-rc1-Drebin.zip` - the ELF plus third-party notices and
  license texts

Full details: `CHANGELOG.md`, `docs/SAVE_TRANSFER.md`, `docs/CARD_TOOLS.md`.
