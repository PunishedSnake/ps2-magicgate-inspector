# MCINSPECTOR P0 closeout

Status: software candidate generation is complete. Real-hardware qualification is the remaining gate before any P0 timing candidate can be promoted.

## Source-of-truth state

P0 is intentionally split into immutable families:

- historical ORIGINAL-541: `a81ca03dce237fbbb48c52ce39adc3319cc0fea1`
- FINAL-SAFE v2: `445ddd31efc6f9c88f2652dcfafee6ca8ff32808`
- FINAL-SAFE v3: `94e6efb9ae132486167a5122edbff62a3ca6c490`
- FINAL-ASYNC v3: `2c97480e1a42fe46470a8669631e4ebe4813ce05`
- RAW-BULK v3: `e46f24dd643c525b0923aa8648dfb6c6b00d0740`
- warning/correctness cleanup: `f4a4e2ba37843aa7452500d22ef024d0904fd64c`
- hardware campaign pack: `f91e1682e68fc1b7d16d7a9d996fffec6816ecd4`

The complete campaign artifact is pinned by `docs/P0_HARDWARE_CAMPAIGN_V3.md`.

## POTWIERDZONE

The software-side P0 work removes or reduces work in the following paths without changing the campaign's correctness contract:

1. batched image discovery;
2. two-page image-filesystem cluster batching;
3. immutable indirect-FAT/FAT metadata caching;
4. destination conflict batching with 16/32/64 candidates;
5. trusted quick-verify read-ahead elision;
6. trusted quick-open descriptor/page-zero ownership fusion;
7. one-deep image read or image write overlap as separate candidates;
8. raw-card 8/16-page synchronous and one-deep asynchronous candidates;
9. retained read-back verification, rollback and correctness hashes.

CI success proves source composition/build consistency. It does not prove a performance winner.

## HIPOTEZY DO TESTU

The following remain hardware questions:

- conflict batch 16 vs 32 vs 64;
- SOURCE-SYNC vs ASYNC-READ vs ASYNC-WRITE;
- raw 16-page vs 8-page;
- raw synchronous vs one-deep asynchronous prefetch;
- end-to-end gain versus ORIGINAL-541 and BASE-c88f507;
- the new dominant bottleneck after the winning composition is selected.

No new runtime optimization should be added before those axes are measured. Doing so would turn the campaign back into a moving target.

## Promotion order

Use the exact Stage 0-6 procedure from `docs/P0_HARDWARE_CAMPAIGN_V3.md`.

Correctness is a hard gate before timing:

- identical logical output/correctness hash;
- verify/restore PASS;
- no new fileXio/mc/SIF error;
- no descriptor ownership anomaly;
- no hang/reset.

Then compare p50, p95, p99 and max. Average alone is not accepted.

`tools/analyze_p0_campaign.py` reads the filled stage CSVs and applies the fail-closed software side of these gates. It reports the Pareto frontier for p50/p95/p99/max and refuses to auto-promote a median-only result when tail latency trades off.

Examples:

```bash
python3 tools/analyze_p0_campaign.py STAGE2_CONFLICT.csv
python3 tools/analyze_p0_campaign.py STAGE3_ASYNC.csv --json stage3.json
python3 tools/analyze_p0_campaign.py STAGE5_HISTORICAL.csv --expected-hash DEADBEEF
```

Stages 1-4 default to at least five timing samples per candidate. Stage 5 defaults to twenty. Survivors should still be extended to the sample counts required by the hardware protocol.

## Cleanup after a winner exists

The warning/correctness cleanup is deliberately not a timing candidate. Apply it only to the exact hardware-selected winner, then rerun build/correctness smoke.

In particular, retain the recovery-path capacity fix from the cleanup branch.

## Stage 6: whole-system re-profile

After promotion, measure again instead of assuming the same bottleneck remains.

Inspect at minimum:

- raw-card RPC count and service/wait ticks;
- image read/write calls, bytes and service ticks;
- async submit/ready/wait ratios;
- memory-card service time;
- SIF/fileXio service time;
- USB/BOT/storage time;
- filesystem metadata/cache behavior;
- verification/CRC time;
- error and retry counts.

The next optimization class is chosen from measured non-hidden time.

## Interaction with the FMCB cross-region installer

`feat/fmcb-cross-region-binding` was created from the older `feat/0.4.0` line while P0 candidates were already far ahead of that base.

Do not merge a P0 winner backwards into the stale installer base.

After P0 promotion:

1. reconstruct or select the exact winning P0 runtime;
2. apply the warning/correctness cleanup;
3. replay/port the cross-region installer changes onto that winner;
4. rebuild and rerun FMCB correctness/MagicGate tests;
5. profile again because installer/security sessions and normal card/image workflows have different critical paths.

This keeps the hardware-qualified P0 transport decisions intact while carrying the newer cross-region functionality forward.
