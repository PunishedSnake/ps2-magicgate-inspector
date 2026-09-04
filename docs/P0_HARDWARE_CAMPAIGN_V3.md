# P0 hardware campaign v3

This is the final software-side P0 qualification package before real PlayStation 2 timing decides promotion.

## Immutable upstream artifacts

### Historical baseline
- build/run: `#541` / `32707959479`
- source: `a81ca03dce237fbbb48c52ce39adc3319cc0fea1`
- artifact: `9512980209`
- digest: `sha256:c3e05adc3c24861eb3144265e40999c3297822cc3924eff0ec1eb306121da250`

### FINAL-SAFE v2, retained to isolate the v3 ownership change
- run: `33914313656`
- source: `445ddd31efc6f9c88f2652dcfafee6ca8ff32808`
- artifact: `9952574690`
- digest: `sha256:2edfd7e36cd2d056b64af286c0de58042a790d2897ed8f1db41f3c0fcddd6a09`

### FINAL-SAFE v3
- run: `33917893563`
- source: `94e6efb9ae132486167a5122edbff62a3ca6c490`
- artifact: `9953876173`
- digest: `sha256:f40f3c662761188a2b99011a5b792d3450140a5ad872140b73b29144c57a2345`

### FINAL-ASYNC v3
- run: `33917936986`
- source: `2c97480e1a42fe46470a8669631e4ebe4813ce05`
- artifact: `9953916431`
- digest: `sha256:d4fbc394b00d80e2ce8a64c4936b8767d7c6431f0068db578bbc8594b32ac8da`

### Raw-bulk v3 screening pack
- run: `33917988910`
- source: `e46f24dd643c525b0923aa8648dfb6c6b00d0740`
- artifact: `9953935212`
- digest: `sha256:d3ed6ee879ea8c1b796203ad32a5d540f1e8bb2ab1c5052aca4471a31e25166d`

### Warning/correctness cleanup, not a timing candidate
- run: `33918304643`
- source: `f4a4e2ba37843aa7452500d22ef024d0904fd64c`
- artifact: `9954015076`
- digest: `sha256:61014fdf8b481340a5786174ca5bc18403a128752dff488f674d171dbb546cdd`

### Isolated trusted quick-open fusion A/B
- run: `33915645229`
- source: `1cda25502170c875697f9f54f588045c3f0a54e9`
- artifact: `9953060485`
- digest: `sha256:d9407fc077e84a76f86413c2e2fd451f70b14b54365ed024aa933bb8850c284c`

## Campaign order

Correctness is a hard gate at every stage. Any corruption, wrong logical CRC/hash, wrong output size, failed verify/restore, new fileXio/mc/SIF error, hang or reset eliminates the candidate before timing rank.

### Stage 0: smoke and hardware correctness
One representative operation for ORIGINAL-541, FINAL-SAFE-v2 conflict32, FINAL-SAFE-v3 conflict32, FINAL-ASYNC-v3 SOURCE-SYNC/READ/WRITE conflict32 and raw16-sync from the raw screening pack.

### Stage 1: isolate quick-open fusion
Selected-save import only. Compare FINAL-SAFE-v2 conflict32 against FINAL-SAFE-v3 conflict32. Five alternating runs are enough for screening; extend to at least 20 valid runs if the delta is measurable and correctness is identical.

### Stage 2: choose conflict batch
Compare FINAL-SAFE-v3 conflict16/32/64 with five rotating screening runs each. Rank end-to-end operation latency first. Use p95/p99/max and service counters to reject a median-only win with worse tails.

### Stage 3: choose image transport overlap
For the Stage-2 conflict winner, compare the same-source FINAL-ASYNC-v3 SOURCE-SYNC, READ and WRITE builds. Start with five rotating runs each; extend surviving candidates to at least 20 valid runs. READ+WRITE dual NOWAIT is intentionally not a candidate.

### Stage 4: choose raw-card bulk geometry/overlap
For the already selected conflict/transport family, compare only four raw variants:
- 16 pages / synchronous
- 8 pages / synchronous
- 16 pages / one-deep asynchronous prefetch
- 8 pages / one-deep asynchronous prefetch

Do not benchmark the complete 12-ELF raw pack as a Cartesian product. CI contains all conflict families only so the winning conflict family does not require a rebuild.

### Stage 5: historical/project gain
Compare at least:
- exact ORIGINAL-541 artifact
- `BASE-c88f507` when available from retained packs
- winning FINAL-SAFE-v3
- winning FINAL async/raw combination if one qualifies

Use at least 20 valid runs per final survivor where practical. Report p50, p95, p99 and max. Do not report average alone.

### Stage 6: whole-system re-profile
After choosing the winner, profile the whole operation again before P1. The bottleneck may have moved from EE copies/RPC frequency into memory-card service, SIF, USB/BOT, filesystem metadata, verification math, or another serialized stage.

## Required metadata

Record for every final series:
- SCPH model and hardware revision
- memory card type/capacity
- USB device and filesystem
- exact ELF SHA-256
- PS2SDK/toolchain and active IRX hashes
- workload, image format and image size
- direction
- relevant batch/async knobs
- sample count
- correctness hash / verify result
- p50 / p95 / p99 / max
- RAW-BULK-PERF and IMAGE-IO counters where applicable
- fileXio/mc/SIF errors and any reset/hang

PCSX2 can be used for correctness and inspection, but it is not the timing arbiter for this campaign.

## Cleanup promotion

The warning/correctness cleanup is intentionally excluded from timing candidates. After a real-hardware performance winner is selected, apply the cleanup to that exact winner and rerun correctness/build smoke. It includes a real path-capacity fix: the recovery path can no longer be smaller than the 192-byte FMCB source-root producer.
