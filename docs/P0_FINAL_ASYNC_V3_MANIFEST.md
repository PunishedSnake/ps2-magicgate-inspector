# P0 FINAL-ASYNC v3 manifest

This document pins the asynchronous P0 v3 source composition used for real-PS2 A/B testing.

## Immutable inputs

- reconciled async source: `c0710d7bcda5a86a24d96a6156539610ec77dfa0`
- qualified async-read origin: `eca8ec24543d65f349b74b44544f3fa3cc159331`
- PS2SDK source/security personality: `a13b5971ec0e39c7ba8b8559b80a4e81c8425352`
- discovery transformer: `f82c536de1c872214699856df43652d509807295`
- cluster transformer: `ec5416b29ff18f578a87c4a2ae123292635f8942`
- FAT metadata-cache transformer: `613e73e411bba75e6670556b3114c4c48c0e4174`
- destination-conflict transformer: `595594ed966981af5f43603ae8036269cf1e6e45`
- trusted quick-verify read-ahead elision transformer: `4053b7a2762ab229bf705a0bd70a28404d8ec54c`
- trusted quick-open ownership fusion transformer: `59500a2aa9866f1bbac3662236deeab67ee6f36a`

## Build families

For each conflict batch 16/32/64, CI builds the same composed source in three transport modes:

1. `SOURCE-SYNC`: read async=0, write async=0
2. `READ`: read async=1, write async=0
3. `WRITE`: read async=0, write async=1

`read async=1` plus `write async=1` is intentionally rejected. Current pinned fileXio behavior exposes global block/completion ownership and this project permits exactly one one-deep asynchronous owner.

## v3 delta

v3 adds the same trusted quick-open ownership fusion qualified by FINAL-SAFE v3. The selected-save trusted path no longer closes the verified image and immediately opens/reads page zero again before streaming.

The normal full verification path and the async read/write state machines are not merged into one dual-NOWAIT pipeline.

## Correctness gate

Before performance ranking on real hardware require:

- identical logical output and correctness hash;
- successful verify/restore;
- no new fileXio/mc/SIF errors;
- no descriptor ownership anomaly, hang or reset.

Then record p50/p95/p99/max, not average alone.
