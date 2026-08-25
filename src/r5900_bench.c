/* SPDX-License-Identifier: MIT */
/*
 * Deterministic real-EE benchmark for Drebin's current image hot kernels.
 *
 * This file is compiled into Performance Lab builds only. Production image
 * operations use whole-operation telemetry instead of perturbing cache state
 * with a synthetic benchmark before real work.
 */

#include <tamtypes.h>
#include <string.h>

#include "card_math.h"
#include "r5900_bench.h"
#include "r5900_memops.h"
#include "r5900_perf.h"

#define MCI_BENCH_BYTES 8192u
#define MCI_BENCH_PAGE_BYTES 512u
#define MCI_BENCH_RAW_BYTES 528u
#define MCI_BENCH_COPY_ITERATIONS 256u
#define MCI_BENCH_CRC_ITERATIONS 64u
#define MCI_BENCH_ECC_ITERATIONS 1024u
#define MCI_BENCH_LATENCY_SAMPLES 32u
#define MCI_BENCH_SMALL_COPY_ITERATIONS 128u
#define MCI_BENCH_8K_COPY_ITERATIONS 8u

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

static inline void CopyCompilerBarrier(void)
{
    /* Keep every repeated copy observable to the optimizer without adding a
     * memory access to the timed loop. Otherwise a sufficiently clever compiler
     * is allowed to collapse identical copies whose destination is only read at
     * the end, which would benchmark dead-code elimination rather than memcpy. */
    __asm__ volatile("" : : "r"(BenchSource), "r"(BenchDest) : "memory");
}

static u32 HashCopiedRange(unsigned int bytes)
{
    const u32 *words = (const u32 *)BenchDest;
    unsigned int word_count = bytes / sizeof(u32);
    u32 hash = 2166136261u;

    hash = MixHash(hash, words[0]);
    hash = MixHash(hash, words[word_count / 2u]);
    hash = MixHash(hash, words[word_count - 1u]);
    BenchSink ^= hash;
    return hash;
}

static __attribute__((noinline)) u32 RunFastCopy512(void)
{
    unsigned int i;
    for (i = 0u; i < MCI_BENCH_SMALL_COPY_ITERATIONS; i++) {
        MciFastCopy(BenchDest, BenchSource, MCI_BENCH_PAGE_BYTES);
        CopyCompilerBarrier();
    }
    return HashCopiedRange(MCI_BENCH_PAGE_BYTES);
}

static __attribute__((noinline)) u32 RunLibcCopy512(void)
{
    unsigned int i;
    for (i = 0u; i < MCI_BENCH_SMALL_COPY_ITERATIONS; i++) {
        memcpy(BenchDest, BenchSource, MCI_BENCH_PAGE_BYTES);
        CopyCompilerBarrier();
    }
    return HashCopiedRange(MCI_BENCH_PAGE_BYTES);
}

static __attribute__((noinline)) u32 RunFastCopy528(void)
{
    unsigned int i;
    for (i = 0u; i < MCI_BENCH_SMALL_COPY_ITERATIONS; i++) {
        MciFastCopy(BenchDest, BenchSource, MCI_BENCH_RAW_BYTES);
        CopyCompilerBarrier();
    }
    return HashCopiedRange(MCI_BENCH_RAW_BYTES);
}

static __attribute__((noinline)) u32 RunLibcCopy528(void)
{
    unsigned int i;
    for (i = 0u; i < MCI_BENCH_SMALL_COPY_ITERATIONS; i++) {
        memcpy(BenchDest, BenchSource, MCI_BENCH_RAW_BYTES);
        CopyCompilerBarrier();
    }
    return HashCopiedRange(MCI_BENCH_RAW_BYTES);
}

static __attribute__((noinline)) u32 RunFastCopy8kLatency(void)
{
    unsigned int i;
    for (i = 0u; i < MCI_BENCH_8K_COPY_ITERATIONS; i++) {
        MciFastCopy(BenchDest, BenchSource, MCI_BENCH_BYTES);
        CopyCompilerBarrier();
    }
    return HashCopiedRange(MCI_BENCH_BYTES);
}

static __attribute__((noinline)) u32 RunLibcCopy8kLatency(void)
{
    unsigned int i;
    for (i = 0u; i < MCI_BENCH_8K_COPY_ITERATIONS; i++) {
        memcpy(BenchDest, BenchSource, MCI_BENCH_BYTES);
        CopyCompilerBarrier();
    }
    return HashCopiedRange(MCI_BENCH_BYTES);
}

/* Counter-rich historical 8 KiB copy window retained for scheduler/cache A/B. */
static __attribute__((noinline)) u32 RunCopyKernel(void)
{
    unsigned int i;
    u32 hash = 2166136261u;

    for (i = 0u; i < MCI_BENCH_COPY_ITERATIONS; i++) {
        MciFastCopy(BenchDest, BenchSource, sizeof(BenchSource));
        CopyCompilerBarrier();
    }

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

static void SortU32(u32 *values, unsigned int count)
{
    unsigned int i;

    for (i = 1u; i < count; i++) {
        u32 value = values[i];
        unsigned int j = i;
        while (j > 0u && values[j - 1u] > value) {
            values[j] = values[j - 1u];
            j--;
        }
        values[j] = value;
    }
}

static u32 Percentile(const u32 *sorted, unsigned int count,
                      unsigned int percentile)
{
    unsigned int rank;

    if (count == 0u)
        return 0u;
    rank = (count * percentile + 99u) / 100u;
    if (rank == 0u)
        rank = 1u;
    if (rank > count)
        rank = count;
    return sorted[rank - 1u];
}

static u32 MeasureCycleDistribution(MciBenchKernel kernel,
                                    MciR5900Latency *latency)
{
    MciR5900PerfSample sample;
    u32 cycles[MCI_BENCH_LATENCY_SAMPLES];
    u32 hash = 2166136261u;
    unsigned int i;

    /* One untimed warm pass gives both candidates the same steady-state start. */
    (void)kernel();
    for (i = 0u; i < MCI_BENCH_LATENCY_SAMPLES; i++) {
        u32 result;
        MciR5900PerfBegin(MCI_R5900_PC0_PROCESSOR_CYCLE,
                          MCI_R5900_PC1_DUAL_ISSUE);
        result = kernel();
        MciR5900PerfEnd(&sample);
        cycles[i] = sample.counter0;
        hash = MixHash(hash, result);
    }

    SortU32(cycles, MCI_BENCH_LATENCY_SAMPLES);
    latency->p50 = Percentile(cycles, MCI_BENCH_LATENCY_SAMPLES, 50u);
    latency->p95 = Percentile(cycles, MCI_BENCH_LATENCY_SAMPLES, 95u);
    latency->p99 = Percentile(cycles, MCI_BENCH_LATENCY_SAMPLES, 99u);
    latency->max = cycles[MCI_BENCH_LATENCY_SAMPLES - 1u];
    return hash;
}

static u32 MeasureKernel(MciBenchKernel kernel, MciR5900BenchMetric *metric)
{
    MciR5900PerfSample sample;
    u32 result_a;
    u32 result_b;
    u32 result_c;

    /* Pair A quantifies scheduling efficiency: elapsed processor cycles and
     * how often the EE actually issued two instructions together. */
    MciR5900PerfBegin(MCI_R5900_PC0_PROCESSOR_CYCLE,
                      MCI_R5900_PC1_DUAL_ISSUE);
    result_a = kernel();
    MciR5900PerfEnd(&sample);
    metric->cycles = sample.counter0;
    metric->dual_issues = sample.counter1;

    /* Pair B measures steady-state hot-loop cache misses. */
    MciR5900PerfBegin(MCI_R5900_PC0_ICACHE_MISS,
                      MCI_R5900_PC1_DCACHE_MISS);
    result_b = kernel();
    MciR5900PerfEnd(&sample);
    metric->icache_misses = sample.counter0;
    metric->dcache_misses = sample.counter1;

    /* Pair C distinguishes control-flow cost from cache/arithmetic cost. */
    MciR5900PerfBegin(MCI_R5900_PC0_BRANCH_ISSUED,
                      MCI_R5900_PC1_BRANCH_MISPREDICT);
    result_c = kernel();
    MciR5900PerfEnd(&sample);
    metric->branches = sample.counter0;
    metric->branch_mispredicts = sample.counter1;

    return MixHash(MixHash(result_a, result_b), result_c);
}

static int CopyMatches(unsigned int bytes)
{
    return memcmp(BenchDest, BenchSource, bytes) == 0;
}

int MciR5900BenchRun(MciR5900BenchReport *report)
{
    u32 hash = 2166136261u;

    if (report == NULL)
        return -1;

    memset(report, 0, sizeof(*report));
    BenchSink = 0u;
    PrepareSource();

    /* Existing counter-rich hot-kernel measurements. */
    (void)RunCopyKernel();
    (void)RunCrcKernel();
    (void)RunEccKernel();
    hash = MixHash(hash, MeasureKernel(RunCopyKernel, &report->copy_8k));
    hash = MixHash(hash, MeasureKernel(RunCrcKernel, &report->crc_8k));
    hash = MixHash(hash, MeasureKernel(RunEccKernel, &report->ecc_512));

    /* Direct native-vs-compiler copy A/B at actual Drebin transfer sizes. */
    hash = MixHash(hash, MeasureCycleDistribution(RunFastCopy512,
                                                   &report->fast_copy_512));
    if (!CopyMatches(MCI_BENCH_PAGE_BYTES))
        return -2;
    hash = MixHash(hash, MeasureCycleDistribution(RunLibcCopy512,
                                                   &report->libc_copy_512));
    if (!CopyMatches(MCI_BENCH_PAGE_BYTES))
        return -3;

    hash = MixHash(hash, MeasureCycleDistribution(RunFastCopy528,
                                                   &report->fast_copy_528));
    if (!CopyMatches(MCI_BENCH_RAW_BYTES))
        return -4;
    hash = MixHash(hash, MeasureCycleDistribution(RunLibcCopy528,
                                                   &report->libc_copy_528));
    if (!CopyMatches(MCI_BENCH_RAW_BYTES))
        return -5;

    hash = MixHash(hash, MeasureCycleDistribution(RunFastCopy8kLatency,
                                                   &report->fast_copy_8k));
    if (!CopyMatches(MCI_BENCH_BYTES))
        return -6;
    hash = MixHash(hash, MeasureCycleDistribution(RunLibcCopy8kLatency,
                                                   &report->libc_copy_8k));
    if (!CopyMatches(MCI_BENCH_BYTES))
        return -7;

    report->copy_iterations = MCI_BENCH_COPY_ITERATIONS;
    report->crc_iterations = MCI_BENCH_CRC_ITERATIONS;
    report->ecc_iterations = MCI_BENCH_ECC_ITERATIONS;
    report->latency_samples = MCI_BENCH_LATENCY_SAMPLES;
    report->result_hash = MixHash(hash, BenchSink);

    MciR5900PerfDisable();
    return 0;
}
