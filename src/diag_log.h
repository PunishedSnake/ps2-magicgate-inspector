/* SPDX-License-Identifier: MIT */
#ifndef MCI_DIAG_LOG_H
#define MCI_DIAG_LOG_H

void MciDiagLogReset(void);
void MciDiagLogSetIoAvailable(int available);
/*
 * Temporarily prevent durable log writes while another long-lived mass: file
 * descriptor is active. Trace lines continue to accumulate in EE RAM and are
 * flushed after resume. This avoids USBHDFSD/fileXio descriptor-position
 * corruption observed when DREBIN.LOG was opened/appended/closed while a card
 * image file remained open for streaming export or verification.
 */
void MciDiagLogSetMassWritePaused(int paused);
void MciDiagLogPrintf(const char *component, const char *format, ...);
void MciDiagLogCheckpoint(const char *component, const char *event, int rc);
const char *MciDiagLogPath(void);

#endif /* MCI_DIAG_LOG_H */
