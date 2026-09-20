# P0 reconciled async hardware matrix manifest

This document pins the exact source lineage and CI result for the combined P0 async hardware candidates. It complements `docs/P0_FINAL_SAFE_MANIFEST.md`; the synchronous final-safe matrix remains the conservative baseline and the async variants remain real-hardware A/B candidates.

## Status vocabulary

- **POTWIERDZONE (static/CI):** source identity, composition guards, build correctness, and artifact generation proved by the cited CI run.
- **CURRENT IMPLEMENTATION:** behavior of the exact source / PS2SDK revisions pinned here.
- **HIPOTEZA DO TESTU:** performance ranking that still requires a real PlayStation 2.

No async candidate is promoted to the normal production path solely because it builds or because PCSX2 appears faster.

## Reconciled lineage

The reconciliation deliberately keeps the later output-side ownership work from:

- safe composition base: `c88f50787b15bf253e11e5bc75017ac1ed4cdd79`
- redundant export-page copy removal: `f02f4a4e5b54e6063009f0e21c78402627c35a75`
- write-slot ownership API: `d7027ca4d492d1456fa0e72b9713b8193f9efd16`
- write-slot Reserve/Commit implementation: `70014c41b8deb97493a294f1d0659fd9b2c7d82a`
- producer direct-to-slot conversion: `1c8cf940c977c2103d79a715087cd1d9518fabb0`

The older qualified read-overlap experiment was:

- branch: `perf/p0-usb-read-overlap`
- qualified head: `eca8ec24543d65f349b74b44544f3fa3cc159331`
- merge base with the later direct-slot line: `850a4eb9bbc513787d456c8dc4d80de8c22f54bc`

Copying that branch wholesale would have discarded later write-side P0 work, so only its qualified read-side implementation was transplanted.

## Qualified read-side blobs transplanted verbatim

The following blobs on `perf/p0-final-async` are intentionally identical to the qualified `eca8ec2` experiment:

- `src/image_read_ahead.c`
  - blob: `a0960222c52848c2d7570d95f9eff0bd9b97f131`
- `src/image_read_ahead.h`
  - blob: `a3e3c4298b1ebadeeffee2f4e8bbfc927acb02f7`
- `src/image_quick_verify.c`
  - blob: `658b29a3bbdbab21fadfddc8db2cf08e6e3424be`
- `tools/analyze_drebin_perf.py`
  - blob: `e429febb29c16ebe6d70edcf77eb875cebf08cea`

Transplant commit:

- `6bb35f58c3bf09117fc67ecd73c3d91c9acaa766`
  - `P0: transplant qualified async read-side implementation`

## Minimal reconciliation changes

### Build controls

The later direct-slot Makefile was extended rather than replaced:

- `IMAGE_READ_PAGES ?= 32`
- `IMAGE_READ_ASYNC ?= 0`
- async-read macro is scoped to `image_read_ahead.o`
- `image_write_behind.o` sees the read-async macro only so the close/drain boundary can be compiled in when required
- existing write-side `IMAGE_WRITE_PAGES`, `IMAGE_WRITE_ASYNC`, Reserve/Commit, and probe controls are preserved

Build reconciliation commit:

- `44876c7dde68a0fb6d6eba3550e1617b8485a682`
  - `P0: reconcile async read controls with direct-slot build`

### Shared fileXio close boundary

`tools/patch_final_async_close_bridge.py` changes only the two exact anchors required to reconcile global fileXio ownership with the later write-behind implementation:

1. conditional read-ahead drain dependency;
2. `__wrap_fileXioClose` ordering.

In an async-read build the wrapper drains any outstanding speculative read, restores `FXIO_WAIT`, then retains the existing write-drain / close behavior. The transformer is fail-closed if either expected source anchor changes.

Bridge commit:

- `02aa83b5151511cf34407d7c2b5153d855cc7cbe`
  - `P0: add fail-closed async read close bridge`

## CURRENT IMPLEMENTATION: one fileXio async owner

The pinned PS2SDK fileXio client exposes one global block mode and one global async completion state. Therefore the reconciled Makefile explicitly rejects:

`IMAGE_READ_ASYNC=1 IMAGE_WRITE_ASYNC=1`

The CI matrix proves this rejection before building hardware candidates.

This is not a claim that USB hardware cannot overlap work. It is a software ownership constraint of the current fileXio client contract. The supported experiments keep at most one outstanding NOWAIT operation and overlap that request with independent EE work.

## Read-ahead ownership contract

Async read-ahead uses two buffers with at most one next refill outstanding:

1. first refill is synchronous and becomes the current consumer-owned buffer;
2. the next refill is submitted into the other buffer;
3. EE consumes records from the current buffer;
4. at the dependency point the next refill is polled, then waited only if incomplete;
5. ownership swaps and the freed buffer becomes the next producer target.

`MCI_IMAGE_READ_AHEAD_PAGES=16` is rejected for async mode. `16 * 528 = 8448` bytes is not divisible by the 512-byte VMC logical stride, so that refill geometry can split a VMC record across ownership buffers. Async candidates use 32 pages, where 16896 bytes is exactly both 32 x 528 and 33 x 512.

The read-ahead scope is a strictly sequential stream contract. The fd and logical stride are expected to remain stable for that scope. Arbitrary lseek / stream switching belongs outside the enabled read-ahead interval.

Rare partial refills are handled explicitly: the current tail is joined with already-completed next-buffer data first, then any remaining bytes are read synchronously. Completion errors fail the logical record instead of converting an optimization failure into a partial successful record.

## Synchronous transforms composed into every candidate

All three async modes additionally receive the exact same pinned synchronous P0 transformations used by FINAL-SAFE:

1. image discovery batching
   - `f82c536de1c872214699856df43652d509807295`
2. two-page image-FS cluster batching
   - `ec5416b29ff18f578a87c4a2ae123292635f8942`
3. immutable FAT metadata cache
   - `613e73e411bba75e6670556b3114c4c48c0e4174`
4. target conflict-directory batching
   - `595594ed966981af5f43603ae8036269cf1e6e45`

Conflict batch remains an independent 16 / 32 / 64 axis until real hardware selects it.

## Exact same-source hardware matrix

For each conflict batch 16 / 32 / 64 the workflow builds three ELFs from the same composed source tree:

### `FINAL-ASYNC-SOURCE-SYNC-conflictN.ELF`

- `IMAGE_READ_ASYNC=0`
- `IMAGE_WRITE_ASYNC=0`

This is the correct control for judging the async effect without comparing against different source composition.

### `FINAL-ASYNC-READ-conflictN.ELF`

- `IMAGE_READ_ASYNC=1`
- `IMAGE_WRITE_ASYNC=0`

Exactly one next sequential fileXio read may be outstanding while EE consumes the current read buffer.

### `FINAL-ASYNC-WRITE-conflictN.ELF`

- `IMAGE_READ_ASYNC=0`
- `IMAGE_WRITE_ASYNC=1`

Exactly one fileXio write may be outstanding while the producer fills the other write slot. The later direct producer -> Reserve/Commit path remains present.

There is deliberately no dual-async ELF.

## CI qualification

The first matrix run correctly failed because the workflow's `make clean` did not force CFLAG-dependent objects to rebuild, causing nominal sync/async variants to reuse the same ELF. The final byte-identity guard detected this and rejected the run rather than publishing a false A/B set.

The workflow was corrected to explicitly remove:

- `MC_INSPECTOR.ELF`
- `src/*.o`
- root `*.o`
- generated `*_irx.c`

before every CFLAG variant.

Clean-rebuild fix:

- `82a29948fb911a644b4671bb92e65634292bf2d2`
  - `CI: force clean rebuild for async CFLAG variants`

Final successful CI:

- workflow: `P0 Final Async Hardware A-B`
- run ID: `33911363852`
- build source SHA: `82a29948fb911a644b4671bb92e65634292bf2d2`
- result: `success`

The successful run proved:

- exact qualified read-side blob identity;
- later direct-slot Reserve/Commit presence;
- all synchronous transformer branch heads still match their pinned SHAs;
- card-math equivalence harness passes;
- pinned PS2SDK / IRX personalities build;
- 16 / 32 / 64 composed source trees pass all fail-closed anchors;
- dual read+write NOWAIT is rejected;
- all 9 hardware candidate ELFs rebuild successfully;
- async-read and async-write ELFs are not byte-identical to their same-source sync controls;
- the complete provenance / A-B pack uploads successfully.

## Final async artifact

- artifact ID: `9951530075`
- name: `PS2-MCI-P0-final-async-82a29948fb911a644b4671bb92e65634292bf2d2`
- size: `9,709,316` bytes
- digest: `sha256:fbd77011ba3350ef7390494919e284b1ce4e1c7e42a8577ef5d58cd16cc0e00c`
- expires: `2026-10-04T19:30:36Z`

## Fixed controls for the real-hardware campaign

Unless a specific sub-test says otherwise, keep fixed:

- `R5900_BENCH=0`
- `RAW_BULK_PAGES=16`
- `RAW_BULK_ASYNC=0`
- `IMAGE_READ_PAGES=32`
- `IMAGE_WRITE_PAGES=32`
- `IMAGE_WRITE_PROBE=0`
- same physical PS2 / SCPH revision
- same active IRX set
- same memory card and slot
- same USB device and filesystem
- same image format and byte size
- same transfer direction and workload
- same cold / warm policy

Correctness is a hard gate. Require the same correctness hash / logical CRC, expected file size, verification result, and no new I/O / SIF / memory-card errors before ranking performance.

For each representative workload target at least 20 valid runs per candidate and report p50, p95, p99 and max for end-to-end operation time and available `RAW-PERF`, `RAW-BULK-PERF`, and `IMAGE-IO` service counters.

### Async-read evidence

Compare against the corresponding `FINAL-ASYNC-SOURCE-SYNC` candidate:

- end-to-end operation time;
- `async_submits`;
- `async_ready`;
- `async_waits`;
- `async_wait_ticks`;
- read service p50 / p95 / p99 / max.

### Async-write evidence

Compare against the corresponding `FINAL-ASYNC-SOURCE-SYNC` candidate:

- end-to-end operation time;
- write-side `async_submits`;
- `async_ready`;
- `async_waits`;
- `async_wait_ticks`;
- write service p50 / p95 / p99 / max.

A large ready-hit count is evidence that latency was hidden, not by itself evidence of a useful user-visible speedup. A large wait count with little end-to-end reduction suggests the overlap window is too small or the consumer reaches the dependency point too early.

## Promotion rule

**HIPOTEZA DO TESTU:** async read or async write may reduce non-hideable time on the critical path. The fastest conflict batch may be 16, 32, or 64. Neither question is settled by CI.

Only a correctness-clean real-PS2 winner may be promoted. After promotion, profile the whole system again because the dominant bottleneck may have moved to card I/O, USB, SIF, filesystem metadata, verification math, or another serialized stage.
