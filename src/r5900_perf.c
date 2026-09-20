/* SPDX-License-Identifier: MIT */
/*
 * Minimal EE performance-counter access for Drebin's optimization lab.
 *
 * The R5900 exposes PCCR plus PCR0/PCR1 through MFPS/MTPS and MFPC/MTPC.
 * This module intentionally owns only bounded begin/end windows. Long card/USB
 * operations can exceed a 32-bit hardware counter, so callers must profile a
 * representative kernel rather than pretending a wrapped counter is precise.
 */

#include <tamtypes.h>

#include "r5900_perf.h"

#define MCI_PCCR_PC0_EXL (1u << 1)
#define MCI_PCCR_PC0_K   (1u << 2)
#define MCI_PCCR_PC0_S   (1u << 3)
#define MCI_PCCR_PC0_U   (1u << 4)
#define MCI_PCCR_PC1_EXL (1u << 11)
#define MCI_PCCR_PC1_K   (1u << 12)
#define MCI_PCCR_PC1_S   (1u << 13)
#define MCI_PCCR_PC1_U   (1u << 14)
#define MCI_PCCR_CTE     (1u << 31)

#define MCI_PCCR_ALL_MODES \
    (MCI_PCCR_PC0_EXL | MCI_PCCR_PC0_K | MCI_PCCR_PC0_S | MCI_PCCR_PC0_U | \
     MCI_PCCR_PC1_EXL | MCI_PCCR_PC1_K | MCI_PCCR_PC1_S | MCI_PCCR_PC1_U)

static u8 ActiveEvent0;
static u8 ActiveEvent1;
static int Active;

static inline void WritePccr(u32 value)
{
    __asm__ __volatile__(
        "mtps %0, 0\n\t"
        "sync.p\n\t"
        :
        : "r"(value)
        : "memory");
}

static inline void ClearPc0(void)
{
    __asm__ __volatile__(
        "mtpc $0, 0\n\t"
        "sync.p\n\t"
        :
        :
        : "memory");
}

static inline void ClearPc1(void)
{
    __asm__ __volatile__(
        "mtpc $0, 1\n\t"
        "sync.p\n\t"
        :
        :
        : "memory");
}

static inline u32 ReadPc0(void)
{
    u32 value;
    __asm__ __volatile__(
        "mfpc %0, 0\n\t"
        "sync.p\n\t"
        : "=r"(value)
        :
        : "memory");
    return value;
}

static inline u32 ReadPc1(void)
{
    u32 value;
    __asm__ __volatile__(
        "mfpc %0, 1\n\t"
        "sync.p\n\t"
        : "=r"(value)
        :
        : "memory");
    return value;
}

void MciR5900PerfDisable(void)
{
    WritePccr(0u);
    Active = 0;
}

void MciR5900PerfBegin(MciR5900PerfEvent0 event0,
                       MciR5900PerfEvent1 event1)
{
    u32 pccr;

    /* Stop before changing event selection or clearing PCR state. */
    WritePccr(0u);
    ClearPc0();
    ClearPc1();

    ActiveEvent0 = (u8)event0;
    ActiveEvent1 = (u8)event1;
    pccr = MCI_PCCR_ALL_MODES |
           (((u32)ActiveEvent0 & 0x1Fu) << 5) |
           (((u32)ActiveEvent1 & 0x1Fu) << 15) |
           MCI_PCCR_CTE;
    Active = 1;
    WritePccr(pccr);
}

void MciR5900PerfEnd(MciR5900PerfSample *sample)
{
    u32 counter0;
    u32 counter1;

    if (!Active) {
        if (sample != NULL) {
            sample->event0 = 0u;
            sample->event1 = 0u;
            sample->reserved = 0u;
            sample->counter0 = 0u;
            sample->counter1 = 0u;
        }
        return;
    }

    /* The first instruction in this function stops counting, so formatting and
     * logger work performed after the snapshot cannot contaminate the sample. */
    WritePccr(0u);
    counter0 = ReadPc0();
    counter1 = ReadPc1();
    Active = 0;

    if (sample != NULL) {
        sample->event0 = ActiveEvent0;
        sample->event1 = ActiveEvent1;
        sample->reserved = 0u;
        sample->counter0 = counter0;
        sample->counter1 = counter1;
    }
}
