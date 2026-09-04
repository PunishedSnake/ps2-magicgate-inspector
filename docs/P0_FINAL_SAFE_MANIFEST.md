# P0 final-safe composition manifest

This document pins the exact inputs used to assemble the combined P0 hardware A/B candidates. It is deliberately explicit: experiment branch names have been reused during the optimization campaign, so branch names are not treated as provenance.

## Status vocabulary

- **POTWIERDZONE (static/CI):** the cited experiment built successfully with its own fail-closed source guards and static checks.
- **CURRENT IMPLEMENTATION:** behavior of the exact repository / PS2SDK revisions pinned below.
- **HIPOTEZA DO TESTU:** performance ranking that still requires timing on a real PlayStation 2.

None of the combined candidates in this manifest is promoted to the normal production path solely from CI or PCSX2 timing.

## Composition base

`FINAL-SAFE` starts from:

- repository: `PunishedSnake/ps2-magicgate-inspector`
- base commit: `c88f50787b15bf253e11e5bc75017ac1ed4cdd79`
- base description: `CI: add non-timing direct-slot execution probe`

This base already contains the earlier qualified output-side P0 work, including the redundant export-copy removal and the direct producer -> write-behind slot ownership path. `IMAGE_WRITE_PROBE=0` is used for primary timing builds.

Production transport controls inherited from the base are kept fixed in the final-safe matrix:

- `R5900_BENCH=0`
- `RAW_BULK_PAGES=16`
- `RAW_BULK_ASYNC=0`
- `IMAGE_READ_PAGES=32`
- `IMAGE_WRITE_PAGES=32`
- `IMAGE_WRITE_ASYNC=0`
- `IMAGE_WRITE_PROBE=0`

The final-safe matrix therefore isolates the newly composed synchronous work-removal / batching changes instead of mixing them with another asynchronous transport axis.

## Pinned synchronous transforms

The workflow extracts each transformer from the exact commit listed here, then applies it to a clean worktree of the base commit. The experiment branches are fetched only so their current heads can be checked against these pins; a moved branch fails the workflow instead of silently changing the candidate.

1. Batched image-name discovery
   - branch: `perf/p0-image-discovery-batch`
   - pinned commit: `f82c536de1c872214699856df43652d509807295`
   - transformer: `tools/patch_card_image_discovery_batch.py`
   - target: `src/card_image.c`
   - timing mode: no `--probe`

2. Two-page image-filesystem cluster batching
   - branch: `perf/p0-image-fs-cluster-batch`
   - pinned commit: `ec5416b29ff18f578a87c4a2ae123292635f8942`
   - transformer: `tools/patch_card_image_fs_cluster_batch.py`
   - target: `src/card_image_fs.c`
   - timing mode: no `--probe`

3. Immutable image-filesystem FAT metadata cache
   - branch: `perf/p0-image-fs-fat-cache`
   - pinned commit: `613e73e411bba75e6670556b3114c4c48c0e4174`
   - transformer: `tools/patch_card_image_fs_meta_cache.py`
   - target: `src/card_image_fs.c`
   - timing mode: no `--probe`

4. Target-card conflict-directory batching
   - branch: `perf/p0-image-fs-conflict-batch`
   - pinned commit: `595594ed966981af5f43603ae8036269cf1e6e45`
   - transformer: `tools/patch_card_image_fs_conflict_batch.py`
   - target: `src/card_image_fs.c`
   - timing mode: `--batch-size 16`, `32`, or `64`, no `--probe`

The fixed application order is:

`discovery -> cluster batch -> FAT cache -> conflict batch`

Every transformer uses exact source anchors and fails closed if the expected source shape is not present.

## Why conflict batches remain 16 / 32 / 64

`CURRENT IMPLEMENTATION`: the pinned PS2SDK/libmc contract allows `mcGetDir` to request multiple `sceMcTblGetDir` entries in one call. The current structure is 64 bytes, therefore the three candidate tables occupy approximately 1 KiB, 2 KiB and 4 KiB of EE BSS respectively.

The R5900 D-cache is only 8 KiB. Fewer EE<->IOP round trips therefore do not automatically make the largest table fastest. Selecting 32 or 64 without real-hardware evidence would merely replace measurement with numerology.

`HIPOTEZA DO TESTU`: one of 16 / 32 / 64 will minimize total conflict-refresh latency for the representative save-list workload. The hardware A/B run decides which one can be promoted.

## Preserved original reference

The historical reference build intentionally retained during development is:

- workflow: `Build PS2 Memory Card Inspector`
- run number: `#541`
- run ID: `32707959479`
- source branch: `feat/0.4.0-dev4-drebin-browser`
- source SHA: `a81ca03dce237fbbb48c52ce39adc3319cc0fea1`
- artifact ID: `9512980209`
- GitHub artifact digest: `sha256:c3e05adc3c24861eb3144265e40999c3297822cc3924eff0ec1eb306121da250`

The final-safe workflow records the GitHub metadata for this artifact and, when Actions artifact download is authorized, bundles the exact retained archive beside the new candidates. This is `ORIGINAL-541`, not a reconstructed approximation.

## Earlier technical baseline

The first identified sequential image verification read-ahead implementation was introduced by:

- `fb419b1592ffb5e7e95490b380fb2a041f9f6989` (`Drebin: batch sequential image verification reads`)

The first identified USB image write batching implementation was introduced by:

- `aeb9eddeee371e4f90c34ca752b86ac22080190c` (`Batch card-image USB writes on sector boundaries`)

A separate pre-transport-optimization baseline may be reconstructed for engineering attribution, but `ORIGINAL-541` remains the primary user-facing historical reference because it is the exact retained ELF artifact used during the project.

## Async read experiment kept separate

The USB read-overlap experiment is intentionally not part of `FINAL-SAFE`:

- branch: `perf/p0-usb-read-overlap`
- pinned head: `eca8ec24543d65f349b74b44544f3fa3cc159331`
- merge base with the later direct-slot line: `850a4eb9bbc513787d456c8dc4d80de8c22f54bc`

That branch diverged before the later output-side direct-slot work. Its one-outstanding `fileXio` NOWAIT read pipeline, close/drain ownership barriers and telemetry are useful experiments, but calling it `FINAL` before reconciling both lines would discard part of the already-qualified output-side P0 work.

`HIPOTEZA DO TESTU`: after deterministic source reconciliation, an async-read combined candidate may reduce non-hideable USB verification/readback time. It must remain a separate A/B axis until correctness and real-console timing qualify it.

## Real-hardware qualification contract

Use the same physical PS2, SCPH/hardware revision, memory card, USB device, USB filesystem, source image, target slot and workload for all candidates. Record the PS2SDK/toolchain pins, active IRX set, direction, image format, sample count and whether the run is cold or warm.

Correctness is a hard gate before timing comparison. Require the same logical image CRC / verification result, expected output size and successful restore verification where applicable.

For timing, record at least operation total plus available `RAW-PERF`, `RAW-BULK-PERF` and `IMAGE-IO` counters. Report p50, p95, p99 and max rather than only an average. Run enough samples to make the tail meaningful; the final A/B README uses 20 runs per candidate as the minimum campaign target.

Alternate or randomize candidate order so USB/card warming and thermal/state drift do not consistently favor one ELF.
