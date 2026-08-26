/* SPDX-License-Identifier: MIT */
#ifndef MCI_R5900_PERF_H
#define MCI_R5900_PERF_H

#include <tamtypes.h>

/*
 * R5900 EE performance-counter events. The two physical counters expose
 * different event sets; keep the names explicit so call sites cannot silently
 * select an event on the wrong counter.
 */
typedef enum MciR5900PerfEvent0 {
    MCI_R5900_PC0_PROCESSOR_CYCLE = 1,
    MCI_R5900_PC0_SINGLE_ISSUE = 2,
    MCI_R5900_PC0_BRANCH_ISSUED = 3,
    MCI_R5900_PC0_BTAC_MISS = 4,
    MCI_R5900_PC0_ITLB_MISS = 5,
    MCI_R5900_PC0_ICACHE_MISS = 6,
    MCI_R5900_PC0_DTLB_ACCESS = 7,
    MCI_R5900_PC0_NONBLOCKING_LOAD = 8,
    MCI_R5900_PC0_WBB_SINGLE_REQUEST = 9,
    MCI_R5900_PC0_WBB_BURST_REQUEST = 10,
    MCI_R5900_PC0_ADDRESS_BUS_BUSY = 11,
    MCI_R5900_PC0_INSTRUCTION_COMPLETED = 12,
    MCI_R5900_PC0_NON_BDS_COMPLETED = 13,
    MCI_R5900_PC0_COP2_COMPLETED = 14,
    MCI_R5900_PC0_LOAD_COMPLETED = 15
} MciR5900PerfEvent0;

typedef enum MciR5900PerfEvent1 {
    MCI_R5900_PC1_LOW_ORDER_BRANCH = 0,
    MCI_R5900_PC1_PROCESSOR_CYCLE = 1,
    MCI_R5900_PC1_DUAL_ISSUE = 2,
    MCI_R5900_PC1_BRANCH_MISPREDICT = 3,
    MCI_R5900_PC1_TLB_MISS = 4,
    MCI_R5900_PC1_DTLB_MISS = 5,
    MCI_R5900_PC1_DCACHE_MISS = 6,
    MCI_R5900_PC1_WBB_SINGLE_UNAVAILABLE = 7,
    MCI_R5900_PC1_WBB_BURST_UNAVAILABLE = 8,
    MCI_R5900_PC1_WBB_BURST_ALMOST_FULL = 9,
    MCI_R5900_PC1_WBB_BURST_FULL = 10,
    MCI_R5900_PC1_DATA_BUS_BUSY = 11,
    MCI_R5900_PC1_INSTRUCTION_COMPLETED = 12,
    MCI_R5900_PC1_NON_BDS_COMPLETED = 13,
    MCI_R5900_PC1_COP1_COMPLETED = 14,
    MCI_R5900_PC1_STORE_COMPLETED = 15
} MciR5900PerfEvent1;

typedef struct MciR5900PerfSample {
    u8 event0;
    u8 event1;
    u16 reserved;
    u32 counter0;
    u32 counter1;
} MciR5900PerfSample;

/*
 * Begin/end one bounded measurement window. Both counters count in all normal
 * execution modes, matching the established EE ps2Perf convention. Keep a
 * window comfortably below 2^32 events; this helper deliberately does not hide
 * counter wraparound behind invented 64-bit precision.
 */
void MciR5900PerfBegin(MciR5900PerfEvent0 event0,
                       MciR5900PerfEvent1 event1);
void MciR5900PerfEnd(MciR5900PerfSample *sample);
void MciR5900PerfDisable(void);

#endif /* MCI_R5900_PERF_H */
