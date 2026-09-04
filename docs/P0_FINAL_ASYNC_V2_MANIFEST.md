# P0 FINAL-ASYNC v2 composition manifest

This pack reconciles the qualified read/write async candidates with the five
synchronous FINAL-SAFE v2 transforms. It preserves the later direct producer ->
write-slot Reserve/Commit ownership model.

## Pinned reconciled async source

- source SHA: `c0710d7bcda5a86a24d96a6156539610ec77dfa0`
- qualified async-read origin: `eca8ec24543d65f349b74b44544f3fa3cc159331`
- toolchain container: `ps2dev/ps2dev:v2.0.0`
- PS2SDK security source: `a13b5971ec0e39c7ba8b8559b80a4e81c8425352`

The pinned source contains the reconciled read-side implementation, build guard
for dual NOWAIT and later write-slot ownership work. The fileXio close bridge is
still applied as an exact fail-closed transformer to materialized test trees.

## Synchronous transforms composed into every candidate

1. discovery batch `f82c536de1c872214699856df43652d509807295`
2. cluster batch `ec5416b29ff18f578a87c4a2ae123292635f8942`
3. immutable FAT cache `613e73e411bba75e6670556b3114c4c48c0e4174`
4. conflict batch `595594ed966981af5f43603ae8036269cf1e6e45`
5. trusted quick-reopen read-ahead elision `4053b7a2762ab229bf705a0bd70a28404d8ec54c`

## Candidate families

For each conflict batch 16 / 32 / 64:

- `FINAL-ASYNC-V2-SOURCE-SYNC`: read async 0, write async 0
- `FINAL-ASYNC-V2-READ`: read async 1, write async 0
- `FINAL-ASYNC-V2-WRITE`: read async 0, write async 1

There is deliberately no build with read async 1 and write async 1.

**CURRENT IMPLEMENTATION:** the pinned fileXio client exposes one global block
mode/completion state. Dual NOWAIT ownership therefore remains rejected at build
time.

## Promotion rule

Only real-PS2 end-to-end timing can select async. `async_ready` is evidence that
latency was hidden; it is not itself a user-visible performance result. Record
p50/p95/p99/max and correctness for the whole operation.
