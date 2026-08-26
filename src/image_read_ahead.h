/* SPDX-License-Identifier: MIT */
#ifndef MCI_IMAGE_READ_AHEAD_H
#define MCI_IMAGE_READ_AHEAD_H

/*
 * Enable only around image code that performs strictly sequential 512/528-byte
 * fileXioRead calls and closes the fd before disabling the mode. The wrapper
 * batches those tiny reads into larger USBHDFSD transfers. Do not enable this
 * around the filesystem browser's seek-heavy cluster reader.
 */
void MciImageReadAheadSetEnabled(int enabled);

/*
 * Completion boundary for the optional one-request-deep NOWAIT prefetch. The
 * fileXio client owns one global async completion state, so callers that are
 * about to close a sequential image fd must drain the read pipeline first.
 * Synchronous production builds compile this to a no-op returning zero.
 */
int MciImageReadAheadDrain(void);

#endif /* MCI_IMAGE_READ_AHEAD_H */
