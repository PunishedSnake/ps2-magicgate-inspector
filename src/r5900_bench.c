/* SPDX-License-Identifier: MIT */
/*
 * Deterministic real-EE benchmark for Drebin's current image hot kernels.
 *
 * The benchmark is intentionally small enough to run once before a Card Tools
 * operation without turning the utility into a benchmark demo. It measures
 * steady-state code after one warm-up and keeps each counter window bounded.
 */

#include <tamtypes.h>
#include <string.h>

#include "card_math.h"
#include "r5900_bench.h"
#include "r5900_memops.h"
#include "r5900_perf.h"

#define MCI_BENCH_BYTES 8192u
#define MCI_BENCH_PAGE_BYTES 512u
#define MCI_BENCH_COPY_ITERATIONS 256u
#define MCI_BENCH_CRC_ITERATIONS 64u
#define MCI_BENCH_ECC_ITERATIONS 1024u

static unsigned char BenchSource[MCI_BENCH_BYTES] __attribute__((aligned(64)));
static unsigned char BenchDest[MCI_BENCH_BYTES] __attribute__((aligned(64)));
static volatile u32 BenchSink;

static u32 MixHash(u32 hash, u32 value)
{
    hash ^= value;
    hash *= 16777619u;
    return hash;
}

static void PrepareSource(void)
{
    u32 state = 0x5900D3E1u;
    unsigned int i;

    for (i = 0u; i < sizeof(BenchSource); i++) {
        state = state * 1664525u + 1013904223u;
        BenchSource[i] = (unsigned char)(state >> 24);
    }
    memset(BenchDest, 0, sizeof(BenchDest));
}

static __attribute__((noinline)) u32 RunCopyKernel(void)
{
    unsigned int i;
    u32 hash = 2166136261u;

    for (i = 0u; i < MCI_BENCH_COPY_ITERATIONS; i++)
        MciFastCopy(BenchDest, BenchSource, sizeof(BenchSource));

    hash = MixHash(hash, ((const u32 *)BenchDest)[0]);
    hash = MixHash(hash, ((const u32 *)BenchDest)[511]);
    hash = MixHash(hash, ((const u32 *)BenchDest)[1023]);
    hash = MixHash(hash, ((const u32 *)BenchDest)[2047]);
    BenchSink ^= hash;
    return hash;
}

static __attribute__((noinline)) u32 RunCrcKernel(void)
{
    unsigned int i;
    u32 crc = 0u;

    for (i = 0u; i < MCI_BENCH_CRC_ITERATIONS; i++)
        crc = MciCardMathCrc32Update(crc, BenchSource, sizeof(BenchSource));

    BenchSink ^= crc;
    return crc;
}

static __attribute__((noinline)) u32 RunEccKernel(void)
{
    unsigned char spare[16] __attribute__((aligned(16)));
    unsigned int i;
    u32 hash = 2166136261u;

    for (i = 0u; i < MCI_BENCH_ECC_ITERATIONS; i++) {
        const unsigned char *page = BenchSource +
            ((i & 15u) * MCI_BENCH_PAGE_BYTES);
        MciCardMathBuildSpare(page, spare);
    }

    hash = MixHash(hash, (u32)spare[0] | ((u32)spare[1] << 8) |
                         ((u32)spare[2] << 16) | ((u32)spare[3] << 24));
    hash = MixHash(hash, (u32)spare[8] | ((u32)spare[9] << 8) |
                         ((u32)spare[10] << 16) | ((u32)spare[11] << 24));
    BenchSink ^= hash;
    return hash;
}

typedef u32 (*MciBenchKernel)(void);

static u32 MeasureKernel(MciBenchKernel kernel, MciR5900BenchMetric *metric)
{
    MciR5900PerfSample sample;
    u32 result_a;
    u32 result_b;

    /* Pair A quantifies scheduling efficiency: elapsed processor cycles and
     * how often the EE actually issued two instructions together. */
    MciR5900PerfBegin(MCI_R5900_PC0_PROCESSOR_CYCLE,
                      MCI_R5900_PC1_DUAL_ISSUE);
    result_a = kernel();
    MciR5900PerfEnd(&sample);
    metric->cycles = sample.counter0;
    metric->dual_issues = sample.counter1;

    /* Pair B is a second steady-state execution of the same deterministic
     * kernel. Code/tables are already warm on purpose: these are hot-loop cache
     * misses, not cold-start misses from entering Card Tools. */
    MciR5900PerfBegin(MCI_R5900_PC0_ICACHE_MISS,
                      MCI_R5900_PC1_DCACHE_MISS);
    result_b = kernel();
    MciR5900PerfEnd(&sample);
    metric->icache_misses = sample.counter0;
    metric->dcache_misses = sample.counter1;

    return MixHash(result_a, result_b);
}

int MciR5900BenchRun(MciR5900BenchReport *report)
{
    u32 hash = 2166136261u;

    if (report == NULL)
        return -1;

    memset(report, 0, sizeof(*report));
    BenchSink = 0u;
    PrepareSource();

    /* Warm the exact code and lookup tables before the measured steady-state
     * passes. The benchmark exists to compare compiler/kernel variants, not to
     * measure the one-time card_math table constructor. */
    (void)RunCopyKernel();
    (void)RunCrcKernel();
    (void)RunEccKernel();

    hash = MixHash(hash, MeasureKernel(RunCopyKernel, &report->copy_8k));
    hash = MixHash(hash, MeasureKernel(RunCrcKernel, &report->crc_8k));
    hash = MixHash(hash, MeasureKernel(RunEccKernel, &report->ecc_512));

    report->copy_iterations = MCI_BENCH_COPY_ITERATIONS;
    report->crc_iterations = MCI_BENCH_CRC_ITERATIONS;
    report->ecc_iterations = MCI_BENCH_ECC_ITERATIONS;
    report->result_hash = MixHash(hash, BenchSink);

    MciR5900PerfDisable();
    return 0;
}
