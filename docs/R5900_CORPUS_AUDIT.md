# Drebin vs. MIPS/R5900 optimization research corpus

This is the implementation audit for the optimization corpus snapshot dated
2026-08-24. It separates work already present in Drebin from techniques that
still require measurement or do not match the current workload.

The source hierarchy remains the one recommended by the corpus: Sony/SCE EE
manuals and current compiler/toolchain source are primary; the corpus is the
index and synthesis layer rather than a replacement for those sources.

## Already implemented

| Corpus recommendation | Drebin state |
| --- | --- |
| Measure transactions before instruction tuning | Raw MCSERV batches 16 x 512-byte pages per SIF RPC and records RPC calls/ticks/fallbacks. |
| Prefer sequential/local access | Raw acquisition, image read-ahead and write-behind use sequential 8 KiB windows. |
| Respect 64-byte cache lines/alignment | Major raw/image working buffers are 64-byte aligned. |
| Exploit 128-bit GPR data movement where natural | `r5900_memops.S` uses native `LQ/SQ` with guarded `memcpy` fallback. |
| Remove branch-heavy scalar bit work | CRC32 and card ECC are compact table-driven kernels. |
| Keep lookup tables small relative to 8 KiB D-cache | CRC + ECC tables total 1536 bytes. |
| Keep `-G0` coherent | Project and PS2SDK build remain `-G0`. |
| Keep short-loop workaround | Toolchain reports `-mfix-r5900`; hand-written loop also keeps an explicit safe branch delay slot. |
| Use per-file policy rather than global flag folklore | Image hot objects are isolated and currently receive the R5900 tuning experiment. |
| Prove transformed math is equivalent | Host CI compares table CRC/ECC with the old reference algorithms and deterministic vectors. |
| Verify final output, not only internal state | Completed images are reopened and read back; destructive restore retains geometry and verification gates. |

## Corrected assumption from the previous pass

`gcc -Q --help=target` reporting `-mtune=mips1` is not sufficient evidence that
current PS2DEV GCC lacks R5900 scheduling knowledge. The current GCC fork has a
dedicated `gcc/config/mips/5900.md` machine description/pipeline model.

Therefore `-mtune=r5900` remains a scoped experiment, not a project-wide truth.
The next decision must be based on:

1. generated `objdump` output;
2. code/symbol size;
3. real EE performance counters;
4. unchanged result hashes and card-image verification.

## Added in this pass

### Real EE performance counters

`r5900_perf.c` provides bounded PCCR/PCR0/PCR1 windows using the R5900
`MTPS/MFPS/MTPC/MFPC` interface. It deliberately exposes 32-bit samples rather
than pretending long operations cannot wrap the hardware counters.

Three steady-state counter pairs are collected for each kernel:

```text
A: PC0 processor cycles        + PC1 dual-instruction issue
B: PC0 instruction-cache miss  + PC1 data-cache miss
C: PC0 branch issued           + PC1 branch mispredict
```

This directly covers the corpus' requested cycles/cache/branch dimensions while
keeping each measurement window short enough for a 32-bit hardware counter.

### Workload-specific benchmark

`r5900_bench.c` benchmarks exactly the current image-side kernels, not generic
million-add loops:

- aligned 8 KiB `LQ/SQ` copy;
- table CRC32 over 8 KiB;
- PCSX2/card ECC generation over 512-byte pages.

Each kernel receives an unmeasured warm-up first. The measured runs are bounded,
deterministic and return a repeatable result hash. The first raw Card Tools
session writes one `R5900-PERF` record to `DREBIN.LOG` before any long-lived
image file descriptor is opened.

### Static compiler report in CI

Every development artifact contains `R5900_STATIC_REPORT.txt` with:

- compiler version and target triple;
- reported `march`, `mtune` and `mfix-r5900` state;
- selected optimizer switches under `-O2`, `-O3` and `-Os`;
- final ELF size and largest symbols;
- production hot-object sizes;
- separately compiled `card_math.c` O2/O3/Os candidates;
- symbol sizes and disassembly for production/O3/Os math kernels;
- production R5900 memory-copy disassembly.

The first report found the following `card_math.c` text sizes:

```text
-O2   976 bytes
-O3  1392 bytes
-Os   828 bytes
```

That does not make O3 automatically worse. The CRC hot function remains 0x180
bytes in both O2 and O3 and `MciCardMathEcc128` remains 0xB8 bytes. Most O3
growth is in one-time table construction and an expanded four-chunk ECC builder.
The generated O3 hot loops also make better use of several branch delay slots
that remain `nop` in O2. `-Os`, meanwhile, shrinks CRC substantially but turns
its byte operation into repeated calls, so size alone is not a speed verdict.

### Hardware A/B compiler candidate

CI builds two hardware-test ELFs from the same revision:

```text
MC_INSPECTOR.ELF
    production O2 policy

MC_INSPECTOR-cardmath-O3.ELF
    identical build except card_math.c = -O3 -mtune=r5900
```

Both execute the same `R5900-PERF` benchmark and must produce the same result
hash. O3 is promoted only if the retail EE shows a useful cycle win without an
unacceptable cache/branch regression and normal image verification remains
bit-identical.

## Still intentionally not enabled

### Project-wide `-O3`

Rejected as a blind default. With a 16 KiB I-cache, code growth and inlining are
part of the performance equation. The CI report now gives us the static half of
the A/B test; hardware gives the other half.

### LTO

Still a separate experiment. It may improve cross-TU optimization but can also
inflate hot code. It should enter the same size/counter matrix instead of being
mixed into the current controller build.

### Manual MMI CRC

CRC32 has a serial dependency between bytes, so naive packed SIMD does not map
cleanly to the algorithm. A hand MMI version needs an actual decomposition that
beats the current compact table kernel; using MMI merely to be R5900-specific is
not an optimization.

ECC is a more plausible MMI candidate, but the current table lookup creates a
non-contiguous dependency. Hardware counters and disassembly now tell us whether
ALU/branch cost is still material before redesigning it.

### Scratchpad / DMAC double buffering

The 16 KiB scratchpad matches two 8 KiB raw batches almost suspiciously well.
It remains the strongest next pipeline experiment after the current SIF bulk
path is hardware-qualified. It is not enabled in this pass because USBHDFSD and
SIF DMA lifetimes already have hardware-discovered interaction hazards. The
profiler should establish whether EE memory/cache pressure is large enough to
justify the added DMA state machine.

### VU0/VU1

Not a current priority. Drebin's hot work is integer byte-stream processing and
I/O orchestration, not 4-float geometry or a large vector arithmetic workload.
Offloading simply because the VUs exist would add transfer/synchronization cost
without a source-backed reason to expect a win.

## Source relevance notes

The TU/e thesis *Exploring boundaries in game processing* is useful secondary
corroboration for the R5900's 2-way superscalar core, non-blocking loads,
128-bit multimedia operations, scratchpad and DMA-oriented architecture.

Liu & Cai (2009), *A Research for the Optimization of MIPS Instruction set
simulation*, studies host-side instruction-decoder organization for a MIPS
simulator. Its multi-level decode index and reported simulator speedup do not
transfer to native Drebin hot loops and are therefore not used as a basis for
this pass.

## Next hardware decision table

Once a build is run on a retail EE, compare the `R5900-PERF` line with the CI
static report:

| Observation | Next experiment |
| --- | --- |
| O3 CRC/ECC cycles lower and cache/branch cost remains sane | Promote O3 only for `card_math.o`; leave the rest O2. |
| O3 wins only cold/table setup but not steady-state math | Keep O2 production math; do not pay the extra text footprint. |
| CRC cycles high, D-cache misses high | Reconsider table/layout size before increasing unroll/table footprint. |
| ECC cycles high, good cache behavior, low dual issue | Inspect dependency chain; prototype a separately verified scheduled/MMI ECC kernel. |
| Copy cycles high relative to bytes, low dual issue | A/B a hand-scheduled/unrolled `LQ/SQ` copy kernel. |
| Copy dominated by D-cache misses | Test scratchpad/DMAC staging rather than adding arithmetic instructions. |
| Branch mispredicts are material | Restructure the offending hot loop before adding more arithmetic optimization. |
| Kernels cheap but export remains slow | Stop touching EE math and return to SIO2/SIF/USB pipeline overlap. |

That last row is important: an optimization pass is allowed to conclude that the
CPU is no longer the bottleneck.
