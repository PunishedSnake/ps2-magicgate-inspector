/* SPDX-License-Identifier: MIT */
#ifndef MCI_DIAG_LOG_H
#define MCI_DIAG_LOG_H

void MciDiagLogReset(void);
void MciDiagLogSetIoAvailable(int available);
void MciDiagLogPrintf(const char *component, const char *format, ...);
void MciDiagLogCheckpoint(const char *component, const char *event, int rc);
const char *MciDiagLogPath(void);

#endif /* MCI_DIAG_LOG_H */
