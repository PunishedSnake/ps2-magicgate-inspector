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

/*
 * Optional producer-facing zero-extra-copy path.
 *
 * Reserve returns 1 and exposes the exact next record location in the current
 * FILL slot when write-behind owns this fd/stride. It returns 0 when batching is
 * disabled or the request is not a 512/528-byte image record, allowing callers
 * to retain the ordinary fileXioWrite fallback. A negative value is an I/O or
 * pipeline error.
 *
 * The returned pointer is owned by the producer only until the next
 * write-behind operation. Commit validates that the pointer is still the next
 * uncommitted record, advances ownership to the batch, updates ordinary logical
 * write accounting, and flushes when the batch is full. No length/ownership is
 * changed by Reserve alone, so abandoning an uncommitted pointer is safe.
 */
int MciImageWriteBehindReserve(int fd, int size, void **buffer);
int MciImageWriteBehindCommit(int fd, void *buffer, int size);

#endif /* MCI_IMAGE_WRITE_BEHIND_H */
