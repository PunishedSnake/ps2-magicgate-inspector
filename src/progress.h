#ifndef MCI_PROGRESS_H
#define MCI_PROGRESS_H

/*
 * Runtime progress events are deliberately presentation-neutral at the call
 * sites. Hardware/IO modules report what they are doing and the adapter in
 * progress.c turns that into the current GS frontend. This keeps card,
 * MagicGate and FMCB code from owning layout or drawing policy.
 */
typedef enum MciProgressDomain {
    MCI_PROGRESS_FILESYSTEM = 0,
    MCI_PROGRESS_MAGICGATE,
    MCI_PROGRESS_FMCB,
    MCI_PROGRESS_ENVIRONMENT
} MciProgressDomain;

/* percent is clamped to 0..100. action and detail are rendered immediately. */
void MciProgressUpdate(MciProgressDomain domain,
                       int percent,
                       const char *action,
                       const char *detail);

#endif /* MCI_PROGRESS_H */
