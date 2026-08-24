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

typedef struct MciR5900BenchReport {
    MciR5900BenchMetric copy_8k;
    MciR5900BenchMetric crc_8k;
    MciR5900BenchMetric ecc_512;
    u32 copy_iterations;
    u32 crc_iterations;
    u32 ecc_iterations;
    u32 result_hash;
} MciR5900BenchReport;

/*
 * Run a bounded, deterministic steady-state benchmark of the exact EE kernels
 * used by the image path. It performs no file/card I/O and is intended to run
 * once per application session before a long Card Tools operation.
 */
int MciR5900BenchRun(MciR5900BenchReport *report);

#endif /* MCI_R5900_BENCH_H */
