# Drebin Image Browser / Selective Restore

This document defines the filesystem-level restore workflow for Drebin 0.4.x.
It is intentionally separate from raw Exact Restore.

## Two restore models

### Exact Restore

Exact Restore reproduces a complete `.ps2` or `.vmc` image onto a card page by
page. It is a raw NAND operation and therefore requires compatible destination
geometry. It is intended for disaster recovery and exact cloning.

### Image Browser / Selective Restore

Selective Restore treats a `.ps2` or `.vmc` image as a read-only PS2 filesystem.
The source card capacity does not have to match the destination card. An 8 MiB
image can restore saves to a 64 MiB card and a 64 MiB image can restore selected
saves to an 8 MiB card. The only hard capacity constraint is the amount of free
space required by the selected save directories on the destination filesystem.

No source superblock, FAT, backup block or raw card geometry is copied during a
selective restore.

## Source workflow

1. Discover `.ps2` and `.vmc` images on USB storage.
2. Let the user choose an image rather than silently assuming the newest file.
3. Verify the complete image before parsing it.
4. Parse the PS2 superblock, indirect FAT, FAT chains and root directory.
5. Enumerate top-level directories as save containers.
6. Read each save's metadata and, when present, `icon.sys` display title.
7. Calculate recursive file count, directory count, logical byte size and
   estimated destination cluster requirement.
8. Present the result in the Image Browser without writing to the card.

Future source backends may expose `mc0:` and `mc1:` through the same browser
model so card-to-card copy can share the selection and conflict UI.

## Browser UI

The Image Browser uses the same GS visual language as the main Drebin dashboard:
framed panels, cyan selection bars, status colours and a fixed footer. It must
not fall back to a plain text modal list.

Suggested layout:

```
+----------------------------------------------------------------+
| Drebin                                        v0.4.x   mc1      |
+----------------------------------------------------------------+
| CARD TOOLS | IMAGE BROWSER                                      |
+-------------------------------+--------------------------------+
| SOURCE                        | DESTINATION                    |
| mc0-03.ps2   8 MiB            | mc1:  PS2                     |
| verified     14 saves         | free 31.4 MiB                 |
+-------------------------------+--------------------------------+
| [ ] Gran Turismo 4                         1248 KiB   OK         |
| [x] Metal Gear Solid 3                      384 KiB   OK         |
| [!] Final Fantasy X                         512 KiB   EXISTS     |
| [x] Silent Hill 3                           196 KiB   OK         |
| ...                                                            |
+----------------------------------------------------------------+
| Selected 2 | Required 580 KiB | Free after restore 30.8 MiB    |
+----------------------------------------------------------------+
| UP/DOWN Move  SQUARE Select  TRIANGLE All  X Restore  O Back   |
+----------------------------------------------------------------+
```

The primary selectable unit is a whole top-level save directory. Individual
files are browseable for inspection/comparison, but restoring only arbitrary
files from a save is not the default because many games require `icon.sys`, icon
assets, metadata and multiple data files to remain together.

## Save labels

The browser should prefer a human-readable title extracted from `icon.sys`.
The raw directory name remains visible as secondary technical information, for
example:

```
Metal Gear Solid 3: Subsistence
BASLUS-21359...    384 KiB    7 files
```

If `icon.sys` cannot be parsed, the raw directory name becomes the primary
label. A malformed title must never block restoring an otherwise valid save.

## Selection and capacity rules

- `SQUARE` toggles the highlighted save.
- `TRIANGLE` selects every non-conflicting save that currently fits.
- The footer continuously reports selected save count, estimated required
  clusters and destination free clusters.
- `CROSS` opens the restore summary/confirmation only when all selected saves
  can fit.
- Insufficient free space blocks the transaction before the first card write.
- Source image size is irrelevant to destination capacity.

## Existing-save conflicts

Initial safe behaviour is fail-closed: an existing top-level destination save
is marked `EXISTS` and is not overwritten by an ordinary restore.

The intended conflict UI is per-save:

```
Existing save detected

> Keep destination
  Replace with image copy
  Compare contents
  Browse image copy
  Browse destination copy
```

`Compare contents` should show at least directory size, modification time, file
count and a file-level difference summary. A later version may calculate CRC32
for matching file paths.

`Replace with image copy` must become transactional. Before deleting or
modifying the destination save, Drebin must create a recoverable copy of that
save on USB and verify it. Only then may the destination directory be replaced.

## Selective restore transaction

1. Revalidate the source image.
2. Revalidate the destination card and filesystem.
3. Refresh conflicts.
4. Recalculate selected cluster requirement from the current destination free
   space.
5. For every selected `REPLACE` conflict, create and verify a rollback backup.
6. Create the top-level destination directory.
7. Recursively create subdirectories and stream files from image FAT chains.
8. Flush and close every output file.
9. Reopen every restored file and compare it with the source image stream.
10. Restore timestamps and supported attributes after content verification.
11. Commit the save only after its complete tree verifies.
12. If a later object fails, remove objects created by this transaction and
    restore any replaced save from the verified rollback copy.
13. Report PASS only when every selected save has verified.

## Safety boundary

Selective Restore uses normal Sony ROM X/libmc filesystem operations. It does
not enter the raw-card MCMAN/MCSERV personality and does not call
`mcEraseBlock`, `mcWritePage` or raw formatting APIs.

Raw Exact Restore and Force Format remain isolated Card Tools operations with
separate destructive confirmations.

## Card Tools information architecture

`TRIANGLE` on the main Card page opens Card Tools. The main Card page must say
this explicitly instead of displaying the obsolete `Format locked` action.

Card Tools should be presented as native Drebin panels/cards, grouped roughly as:

- **BACKUP**: Create `.ps2`, Create `.vmc`, Verify image
- **RESTORE**: Image Browser / Selective Restore, Exact Restore
- **MAINTENANCE**: Format, Force Format + verified recovery image
- **RECOVERY**: Restore the most recent verified pre-format image

Destructive entries use warning/danger styling but remain visually consistent
with the rest of the dashboard.

## Future extensions

The browser model is deliberately source-agnostic enough to support:

- `mc0:` -> `mc1:` save copy
- `mc1:` -> `mc0:` save copy
- image -> card
- card -> image/archive
- image -> image save extraction/insertion
- save metadata and icon preview
- compare/merge tooling for conflicting saves

These extensions must retain the whole-save default and transactional overwrite
policy.
