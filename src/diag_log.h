/* SPDX-License-Identifier: MIT */
#ifndef MCI_DIAG_LOG_H
#define MCI_DIAG_LOG_H

void MciDiagLogReset(void);
void MciDiagLogSetIoAvailable(int available);
/*
 * Temporarily prevent durable log writes while another subsystem owns mass:
 * for a streaming or transactional filesystem scope. Trace lines continue to
 * accumulate in EE RAM and are flushed after resume. Real hardware reproduced
 * USBHDFSD/fileXio cross-file corruption when DREBIN.LOG was opened/appended
 * around card-image and FMCB recovery traffic.
 */
void MciDiagLogSetMassWritePaused(int paused);

/* Durable event: when mass I/O is safe this is written immediately. Any queued
 * RAM trace is batch-flushed first so ordering remains useful after a crash. */
void MciDiagLogPrintf(const char *component, const char *format, ...);

/* Best-effort trace: always enters the bounded EE RAM ring. It is intentionally
 * not a durability boundary and therefore does not create fileXio/sync traffic
 * by itself. Use this for progress/counters/high-frequency diagnostics. */
void MciDiagLogTracePrintf(const char *component, const char *format, ...);

void MciDiagLogCheckpoint(const char *component, const char *event, int rc);
const char *MciDiagLogPath(void);

#endif /* MCI_DIAG_LOG_H */