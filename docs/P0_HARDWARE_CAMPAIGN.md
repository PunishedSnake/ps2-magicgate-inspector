# P0 real-hardware campaign

This is the execution plan for selecting the P0 winner on a real PlayStation 2. It combines the exact retained historical reference, the mature synchronous base, the composed FINAL-SAFE variants, and the same-source async read/write candidates without mixing performance claims across different source trees.

## Evidence boundary

**POTWIERDZONE (static/CI):** the source compositions, ownership guards, exact artifact provenance, PS2SDK/IRX staging, and candidate ELF builds are reproducible in CI.

**HIPOTEZA DO TESTU:** which conflict batch is fastest, whether read overlap helps, whether write overlap helps, and the final end-to-end percentage improvement. These require correctness-clean real-console data.

PCSX2 may be used for correctness/debugging but is not the timing/cache/DMA/FIFO/USB/SIF arbiter.

## Exact artifact inputs

### Historical reference

- workflow build: `#541`
- run ID: `32707959479`
- source SHA: `a81ca03dce237fbbb48c52ce39adc3319cc0fea1`
- artifact ID: `9512980209`
- digest: `sha256:c3e05adc3c24861eb3144265e40999c3297822cc3924eff0ec1eb306121da250`

This is the retained `ORIGINAL-541` artifact used during the project, not a source reconstruction.

### FINAL-SAFE pack

- run ID: `33910509140`
- source SHA: `54b1e6e8a3bbe79652390198bec4917e18738471`
- artifact ID: `9951188483`
- digest: `sha256:66a61f2c8f27576dfa93e227ea96a3ab53e07b67eb63a9518fda7f24334515bc`

Contains:

- `BASE-c88f507.ELF`
- `FINAL-SAFE-conflict16.ELF`
- `FINAL-SAFE-conflict32.ELF`
- `FINAL-SAFE-conflict64.ELF`
- exact retained ORIGINAL-541 archive
- source diffs, transformer pins, IRX hashes, results template, and protocol.

### Reconciled FINAL-ASYNC pack

- run ID: `33911363852`
- source SHA: `82a29948fb911a644b4671bb92e65634292bf2d2`
- artifact ID: `9951530075`
- digest: `sha256:fbd77011ba3350ef7390494919e284b1ce4e1c7e42a8577ef5d58cd16cc0e00c`

For each conflict batch 16 / 32 / 64 it contains same-source:

- `FINAL-ASYNC-SOURCE-SYNC`
- `FINAL-ASYNC-READ`
- `FINAL-ASYNC-WRITE`

There is deliberately no simultaneous read+write NOWAIT candidate because the pinned current fileXio client exposes one global async ownership/completion state.

## Fixed hardware metadata

Record for every campaign:

- SCPH model and hardware revision;
- PS2SDK commit and toolchain/container identity;
- active IRX set and hashes;
- physical memory-card model/capacity and slot;
- USB device/controller identification where available;
- USB filesystem;
- source image format (`.vmc` or `.ps2`) and exact byte size;
- operation direction;
- cold/warm policy;
- restart/reboot policy;
- sample count;
- build SHA / candidate name;
- correctness hash and verify result.

Keep these fixed inside an A/B group. If one changes, record a new group rather than quietly mixing samples.

## Correctness gate

A timing sample is invalid for ranking if any of the following occurs:

- logical CRC / correctness hash differs from the baseline;
- output size differs;
- full verification fails;
- selective restore or post-write verification fails;
- new fileXio, memory-card, SIF, filesystem, ownership, reset, hang, or corruption symptom appears.

Do not average failures into the timing dataset. Count them separately as correctness/deadline failures.

## Stage 0: hardware smoke test

Run one representative operation with:

1. exact `ORIGINAL-541` artifact;
2. `BASE-c88f507`;
3. FINAL-SAFE conflict16;
4. FINAL-SAFE conflict32;
5. FINAL-SAFE conflict64;
6. same-source sync conflict32;
7. async-read conflict32;
8. async-write conflict32.

The purpose is only to catch hardware-only correctness problems before spending time on statistics.

## Stage 1: choose conflict-directory batch

Use only the three FINAL-SAFE variants so async transport is not another changing variable.

Run five valid screening repetitions each. Rotate order:

1. 16, 32, 64
2. 64, 32, 16
3. 32, 64, 16
4. 16, 64, 32
5. 64, 16, 32

Primary ranking is end-to-end operation time. Use service counters to explain the result, not to replace it.

Do not assume 64 wins merely because it issues fewer EE/IOP control transactions. `sceMcTblGetDir` entries are 64 bytes, so the 16 / 32 / 64 tables are about 1 / 2 / 4 KiB against an 8 KiB R5900 D-cache.

If two variants are very close at five runs, retain both for the longer sample rather than declaring a winner from noise.

## Stage 2: choose async policy

Take the winning conflict batch from Stage 1 and compare only its same-source family:

- SYNC: `IMAGE_READ_ASYNC=0`, `IMAGE_WRITE_ASYNC=0`
- READ_ASYNC: `IMAGE_READ_ASYNC=1`, `IMAGE_WRITE_ASYNC=0`
- WRITE_ASYNC: `IMAGE_READ_ASYNC=0`, `IMAGE_WRITE_ASYNC=1`

Start with five valid runs each and rotate S/R/W order. If correctness remains clean and an async mode is competitive, extend surviving candidates to at least 20 valid runs each.

For read overlap record:

- end-to-end operation time;
- `async_submits`;
- `async_ready`;
- `async_waits`;
- `async_wait_ticks`;
- read service p50 / p95 / p99 / max.

For write overlap record the analogous write-side counters.

A ready hit demonstrates hidden request latency but does not by itself prove a useful user-visible improvement. A high wait count with little end-to-end gain suggests the consumer reaches the dependency point before enough independent work has elapsed.

## Stage 3: project-level comparison

Once conflict batch and async policy are selected, compare:

- exact ORIGINAL-541;
- BASE-c88f507;
- winning FINAL-SAFE;
- winning same-source async candidate, if an async mode qualified.

For the final report target at least 20 valid runs for each candidate where practical.

Report:

- p50;
- p95;
- p99;
- max;
- correctness/deadline failures;
- relevant `RAW-PERF`, `RAW-BULK-PERF`, and `IMAGE-IO` counters.

An arithmetic mean may be included as supplementary information but must not be the only statistic.

## Stage 4: whole-system re-profile

After selecting the winner, profile again rather than immediately stacking another optimization on top. P0 has already removed copies, reduced transactions, batched metadata and introduced optional overlap. The critical path may have moved to:

- physical memory-card service time;
- USB/BOT transfer time;
- SIF/RPC latency;
- filesystem metadata;
- CRC/ECC/verification work;
- cache pressure;
- another serialized control boundary.

The next optimization must follow that new bottleneck.

## Promotion rule

Promote the smallest correctness-clean change that wins on real hardware. If synchronous FINAL-SAFE wins, do not promote async merely because it is more sophisticated. If async wins only at the cost of worse p99/max or intermittent correctness, keep the synchronous path.

After promotion, retain the losing candidates and raw logs long enough to reproduce the decision and update the PS2 optimization research notes with the real-hardware evidence.
