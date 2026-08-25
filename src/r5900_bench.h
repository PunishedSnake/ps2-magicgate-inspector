/* SPDX-License-Identifier: MIT */
#ifndef MCI_R5900_BENCH_H
#define MCI_R5900_BENCH_H

#include <tamtypes.h>

typedef struct MciR5900BenchMetric {
    u32 cycles;
    u32 dual_issues;
    u32 icache_misses;
    u32 dcache_misses;
    u32 branches;
    u32 branch_mispredicts;
} MciR5900BenchMetric;

typedef struct MciR5900Latency {
    u32 p50;
    u32 p95;
    u32 p99;
    u32 max;
} MciR5900Latency;

typedef struct MciR5900BenchReport {
    /* Existing counter-rich steady-state kernels. */
    MciR5900BenchMetric copy_8k;
    MciR5900BenchMetric crc_8k;
    MciR5900BenchMetric ecc_512;

    /*
     * Copy A/B distributions use the exact sizes Drebin moves in production:
     * 512-byte VMC/card pages, 528-byte PCSX2 records and the historical 8 KiB
     * streaming batch. `fast` is MciFastCopy/LQ-SQ when its contract applies;
     * `libc` is the compiler's normal memcpy expression at the same constant
     * size. Results are processor-cycle distributions over repeated bounded
     * samples, not a single lucky counter read.
     */
    MciR5900Latency fast_copy_512;
    MciR5900Latency libc_copy_512;
    MciR5900Latency fast_copy_528;
    MciR5900Latency libc_copy_528;
    MciR5900Latency fast_copy_8k;
    MciR5900Latency libc_copy_8k;

    u32 copy_iterations;
    u32 crc_iterations;
    u32 ecc_iterations;
    u32 latency_samples;
    u32 result_hash;
} MciR5900BenchReport;

/*
 * Run a bounded, deterministic steady-state benchmark of the exact EE kernels
 * used by the image path. It performs no file/card I/O and is compiled into
 * Performance Lab builds only; ordinary backup/restore does not run it.
 */
int MciR5900BenchRun(MciR5900BenchReport *report);

#endif /* MCI_R5900_BENCH_H */
