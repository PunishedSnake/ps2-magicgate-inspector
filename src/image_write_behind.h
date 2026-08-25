#ifndef MCI_IMAGE_WRITE_BEHIND_H
#define MCI_IMAGE_WRITE_BEHIND_H

/*
 * Enable/disable the image-only fileXioWrite batching wrapper.
 *
 * Production currently uses a synchronous 32-record batch. Performance-lab
 * builds may vary the batch size and enable one-request-deep async fileXio
 * write-behind. The async path owns two EE buffers but never queues a second
 * USB/BOT write before the first one completes.
 *
 * Ownership while async is enabled:
 *   FILL -> FILEXIO/USB -> FREE
 * The producer may fill the other slot while FILEXIO/USB owns one slot.
 * fileXioClose is the mandatory completion boundary before descriptor reuse,
 * verification or any unrelated fileXio operation.
 */
void MciImageWriteBehindSetEnabled(int enabled);

#endif /* MCI_IMAGE_WRITE_BEHIND_H */
