/*
 * PS2 Memory Card Inspector - ordinary memory-card diagnostics
 *
 * This module owns the normal Sony ROM X / XMCMAN filesystem side of the
 * Inspector. MagicGate is intentionally separate: a card may be a perfectly
 * healthy PS2 memory card while still lacking the CardAuth behavior required
 * for SECR/KELF binding.
 *
 * 0.4 extends the disposable integrity test without reserving hundreds of KiB
 * of EE RAM. Test files are produced and verified in deterministic 4 KiB
 * chunks, so Quick/Extended/Thorough profiles exercise 4/64/256 KiB while
 * using the same small aligned buffers and cleanup guarantees.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <io_common.h>
#include <stdio.h>
#include <string.h>

#include "card.h"
#include "progress.h"

#define TEST_CHUNK 4096u
#define TEST_DEFAULT TEST_CHUNK
#define TEST_MAX (1024u * 1024u)

static unsigned char WriteBuffer[TEST_CHUNK] __attribute__((aligned(64)));
static unsigned char ReadBuffer[TEST_CHUNK] __attribute__((aligned(64)));
static sceMcTblGetDir DirEntry __attribute__((aligned(64)));

static void CardProgress(int port, int percent,
                         const char *action, const char *detail)
{
    char line[256];

    snprintf(line, sizeof(line), "mc%d: %.240s", port,
             detail != NULL ? detail : "");
    MciProgressUpdate(MCI_PROGRESS_FILESYSTEM, percent, action, line);
}

static int McSyncResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static unsigned int NormalizeTestBytes(unsigned int bytes)
{
    if (bytes < TEST_CHUNK)
        bytes = TEST_CHUNK;
    if (bytes > TEST_MAX)
        bytes = TEST_MAX;
    bytes = (bytes + TEST_CHUNK - 1u) & ~(TEST_CHUNK - 1u);
    return bytes;
}

static void FillPattern(unsigned int absolute_offset, unsigned int size)
{
    unsigned int i;
    unsigned int state = 0x4D43494Eu ^ absolute_offset; /* "MCIN" */

    for (i = 0; i < size; i++) {
        unsigned int pos = absolute_offset + i;
        state = state * 1664525u + 1013904223u + pos;
        WriteBuffer[i] = (unsigned char)((state >> 24) ^ pos ^ (pos >> 4));
    }
}

static int CloseFile(int fd)
{
    mcClose(fd);
    return McSyncResult();
}

static int DeleteFile(int port, const char *name)
{
    mcDelete(port, 0, name);
    return McSyncResult();
}

static int FindUnusedTempName(int port, char *name)
{
    int i;
    int rc;

    for (i = 0; i < 100; i++) {
        sprintf(name, "/__MCI%02d.TMP", i);
        memset(&DirEntry, 0, sizeof(DirEntry));
        mcGetDir(port, 0, name, 0, 1, &DirEntry);
        rc = McSyncResult();
        if (rc == 0 || rc == sceMcResNoEntry)
            return 0;
        if (rc < 0)
            return rc;
    }
    return -1000;
}

static int VerifySameCard(int port)
{
    int type = 0;
    int free_clusters = 0;
    int formatted = 0;

    mcGetInfo(port, 0, &type, &free_clusters, &formatted);
    return McSyncResult();
}

static int RunReadWriteTest(int port, unsigned int test_bytes,
                            int *cleanup_rc, RwStage *stage)
{
    char path[24];
    char detail[192];
    unsigned int offset;
    unsigned int chunk;
    int fd;
    int rc;

    test_bytes = NormalizeTestBytes(test_bytes);
    *cleanup_rc = 0;
    *stage = RW_FIND_NAME;
    memset(ReadBuffer, 0, sizeof(ReadBuffer));

    CardProgress(port, 24, "Choosing a temporary test file",
                 "Searching for an unused /__MCIxx.TMP name; existing files are never overwritten.");
    rc = FindUnusedTempName(port, path);
    if (rc < 0)
        return rc;

    *stage = RW_VERIFY_CARD;
    snprintf(detail, sizeof(detail),
             "Temporary path %s selected. Re-checking the slot before creating a %u KiB test file.",
             path, test_bytes / 1024u);
    CardProgress(port, 31, "Confirming the same card is still inserted", detail);
    rc = VerifySameCard(port);
    if (rc != sceMcResSucceed)
        return rc;

    *stage = RW_OPEN_WRITE;
    snprintf(detail, sizeof(detail),
             "Creating %s for a disposable %u KiB streamed integrity test.",
             path, test_bytes / 1024u);
    CardProgress(port, 36, "Opening the temporary file for writing", detail);
    mcOpen(port, 0, path, FIO_O_WRONLY | FIO_O_CREAT);
    fd = McSyncResult();
    if (fd < 0)
        return fd;

    *stage = RW_WRITE;
    offset = 0;
    while (offset < test_bytes) {
        int percent;
        chunk = test_bytes - offset;
        if (chunk > TEST_CHUNK)
            chunk = TEST_CHUNK;
        FillPattern(offset, chunk);
        mcWrite(fd, WriteBuffer, chunk);
        rc = McSyncResult();
        if (rc != (int)chunk) {
            CloseFile(fd);
            *cleanup_rc = DeleteFile(port, path);
            return (rc < 0) ? rc : -1001;
        }
        offset += chunk;
        percent = 38 + (int)((offset * 17u) / test_bytes);
        snprintf(detail, sizeof(detail),
                 "Written %u / %u KiB in deterministic 4 KiB chunks.",
                 offset / 1024u, test_bytes / 1024u);
        CardProgress(port, percent, "Writing the integrity pattern", detail);
    }

    *stage = RW_FLUSH;
    CardProgress(port, 57, "Flushing the write",
                 "Forcing the memory-card driver to commit the complete temporary file before verification.");
    mcFlush(fd);
    rc = McSyncResult();
    if (rc < 0) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_CLOSE_WRITE;
    CardProgress(port, 61, "Closing the write handle",
                 "Finishing the write phase cleanly before reopening the file read-only.");
    rc = CloseFile(fd);
    if (rc < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_OPEN_READ;
    CardProgress(port, 65, "Reopening the temporary file",
                 "Opening the committed file read-only for an independent streamed read-back.");
    mcOpen(port, 0, path, FIO_O_RDONLY);
    fd = McSyncResult();
    if (fd < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return fd;
    }

    *stage = RW_READ;
    offset = 0;
    while (offset < test_bytes) {
        int percent;
        chunk = test_bytes - offset;
        if (chunk > TEST_CHUNK)
            chunk = TEST_CHUNK;
        memset(ReadBuffer, 0, chunk);
        mcRead(fd, ReadBuffer, chunk);
        rc = McSyncResult();
        if (rc != (int)chunk) {
            CloseFile(fd);
            *cleanup_rc = DeleteFile(port, path);
            return (rc < 0) ? rc : -1002;
        }
        *stage = RW_COMPARE;
        FillPattern(offset, chunk);
        if (memcmp(WriteBuffer, ReadBuffer, chunk) != 0) {
            CloseFile(fd);
            *cleanup_rc = DeleteFile(port, path);
            return -1003;
        }
        *stage = RW_READ;
        offset += chunk;
        percent = 67 + (int)((offset * 17u) / test_bytes);
        snprintf(detail, sizeof(detail),
                 "Verified %u / %u KiB byte-for-byte after reopen.",
                 offset / 1024u, test_bytes / 1024u);
        CardProgress(port, percent, "Reading and comparing the integrity pattern", detail);
    }

    *stage = RW_CLOSE_READ;
    CardProgress(port, 86, "Closing the read handle",
                 "All streamed chunks matched their deterministic expected data.");
    rc = CloseFile(fd);
    if (rc < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_DELETE;
    CardProgress(port, 91, "Deleting the temporary test file",
                 "The integrity check passed; removing the disposable file from the card.");
    *cleanup_rc = DeleteFile(port, path);
    if (*cleanup_rc < 0)
        return -1004;

    *stage = RW_VERIFY_DELETE;
    CardProgress(port, 96, "Verifying cleanup",
                 "Confirming that the temporary file is gone and the card is left in its original state.");
    memset(&DirEntry, 0, sizeof(DirEntry));
    mcGetDir(port, 0, path, 0, 1, &DirEntry);
    rc = McSyncResult();
    if (rc != 0 && rc != sceMcResNoEntry)
        return -1005;

    *stage = RW_DONE;
    snprintf(detail, sizeof(detail),
             "%u KiB write, flush, reopen, streamed read-back, compare and cleanup all completed.",
             test_bytes / 1024u);
    CardProgress(port, 100, "Filesystem integrity test complete", detail);
    return 0;
}

void CardInspectSized(int port, CardReport *r, unsigned int test_bytes)
{
    char detail[192];

    test_bytes = NormalizeTestBytes(test_bytes);
    memset(r, 0, sizeof(*r));
    r->port = port;
    r->root_rc = -999;
    r->rw_rc = -999;
    r->cleanup_rc = 0;
    r->rw_stage = RW_NOT_RUN;
    r->health = CARD_UNKNOWN;
    r->test_bytes = test_bytes;

    CardProgress(port, 4, "Querying memory-card metadata",
                 "Calling mcGetInfo to identify the card type, format state and free clusters.");
    mcGetInfo(port, 0, &r->type, &r->free_clusters, &r->formatted);
    r->info_rc = McSyncResult();

    if (r->info_rc == sceMcResFailAuth) {
        r->health = CARD_AUTH_FAILURE;
        CardProgress(port, 100, "Filesystem inspection stopped",
                     "mcGetInfo reported a card authentication failure.");
        return;
    }
    if (r->info_rc <= sceMcResFailDetect) {
        r->health = CARD_DETECT_FAILURE;
        CardProgress(port, 100, "Filesystem inspection stopped",
                     "The slot did not complete normal memory-card detection.");
        return;
    }
    if (r->type == MC_TYPE_NONE) {
        r->health = CARD_NO_CARD;
        CardProgress(port, 100, "No memory card detected",
                     "The selected slot is empty; no filesystem operations were attempted.");
        return;
    }
    if (r->info_rc == sceMcResNoFormat || !r->formatted) {
        r->health = CARD_UNFORMATTED;
        r->format_allowed = (r->type == MC_TYPE_PS2);
        CardProgress(port, 100, "Card detected but no formatted filesystem exists",
                     "Read/write integrity testing is skipped until the card is formatted.");
        return;
    }

    snprintf(detail, sizeof(detail),
             "PS2 card detected; formatted=%d, free clusters=%d. Checking the root directory before a %u KiB integrity test.",
             r->formatted, r->free_clusters, test_bytes / 1024u);
    CardProgress(port, 15, "Checking the root directory", detail);
    memset(&DirEntry, 0, sizeof(DirEntry));
    mcGetDir(port, 0, "/*", 0, 1, &DirEntry);
    r->root_rc = McSyncResult();

    if (r->root_rc == sceMcResNoFormat) {
        r->health = CARD_FILESYSTEM_BROKEN;
        r->format_allowed = (r->type == MC_TYPE_PS2);
        CardProgress(port, 100, "Root-directory check failed",
                     "The card reports no usable format even though metadata detection completed.");
        return;
    }
    if (r->root_rc == sceMcResFailAuth) {
        r->health = CARD_AUTH_FAILURE;
        CardProgress(port, 100, "Root-directory check failed",
                     "The normal filesystem path reported card authentication failure.");
        return;
    }
    if (r->root_rc <= sceMcResFailDetect) {
        r->health = CARD_DETECT_FAILURE;
        CardProgress(port, 100, "Root-directory check failed",
                     "The card became unavailable during filesystem inspection.");
        return;
    }
    if (r->root_rc < 0 && r->root_rc != sceMcResNoEntry) {
        r->health = CARD_IO_FAILURE;
        CardProgress(port, 100, "Root-directory check failed",
                     "The normal card filesystem returned an unexpected I/O error.");
        return;
    }

    snprintf(detail, sizeof(detail),
             "Root directory is readable. Starting the disposable %u KiB streamed write / flush / read-back test.",
             test_bytes / 1024u);
    CardProgress(port, 20, "Root directory is readable", detail);
    r->rw_rc = RunReadWriteTest(port, test_bytes, &r->cleanup_rc, &r->rw_stage);

    if (r->rw_rc == 0) {
        r->health = CARD_OK;
    } else if (r->rw_rc == sceMcResFullDevice) {
        r->health = CARD_FULL;
    } else if (r->rw_rc == sceMcResNoFormat) {
        r->health = CARD_FILESYSTEM_BROKEN;
        r->format_allowed = (r->type == MC_TYPE_PS2);
    } else if (r->rw_rc == sceMcResFailAuth) {
        r->health = CARD_AUTH_FAILURE;
    } else if (r->rw_rc <= sceMcResFailDetect) {
        r->health = CARD_DETECT_FAILURE;
    } else {
        r->health = CARD_IO_FAILURE;
    }
}

void CardInspect(int port, CardReport *report)
{
    CardInspectSized(port, report, TEST_DEFAULT);
}

int CardFormat(int port, CardReport *report)
{
    int rc;

    CardProgress(port, 10, "Formatting the memory card",
                 "The destructive mcFormat operation is running. Do not remove the card.");
    mcFormat(port, 0);
    rc = McSyncResult();
    if (rc == 0) {
        CardProgress(port, 35, "Format completed; verifying the new filesystem",
                     "Re-running the conservative 4 KiB filesystem inspection on the freshly formatted card.");
        CardInspect(port, report);
    }
    return rc;
}

const char *CardRwStageText(RwStage stage)
{
    switch (stage) {
        case RW_FIND_NAME: return "FIND TEMP NAME";
        case RW_VERIFY_CARD: return "VERIFY SAME CARD";
        case RW_OPEN_WRITE: return "OPEN-WRITE";
        case RW_WRITE: return "WRITE";
        case RW_FLUSH: return "FLUSH";
        case RW_CLOSE_WRITE: return "CLOSE-WRITE";
        case RW_OPEN_READ: return "OPEN-READ";
        case RW_READ: return "READ";
        case RW_CLOSE_READ: return "CLOSE-READ";
        case RW_COMPARE: return "COMPARE";
        case RW_DELETE: return "DELETE";
        case RW_VERIFY_DELETE: return "VERIFY DELETE";
        case RW_DONE: return "DONE";
        default: return "NOT RUN";
    }
}

const char *CardResultText(int rc)
{
    switch (rc) {
        case sceMcResSucceed: return "OK";
        case sceMcResChangedCard: return "CARD CHANGED";
        case sceMcResNoFormat: return "NO FORMAT";
        case sceMcResFullDevice: return "FULL DEVICE";
        case sceMcResNoEntry: return "NO ENTRY";
        case sceMcResDeniedPermit: return "DENIED PERMIT";
        case sceMcResNotEmpty: return "NOT EMPTY";
        case sceMcResUpLimitHandle: return "HANDLE LIMIT";
        case sceMcResFailReplace: return "REPLACE FAILED";
        case sceMcResFailResetAuth: return "AUTH RESET FAILED";
        case sceMcResFailDetect: return "DETECT FAILED";
        case sceMcResFailDetect2: return "DETECT FAILED 2";
        case sceMcResDeniedPS1Permit: return "PS1 PERMISSION DENIED";
        case sceMcResFailAuth: return "AUTH FAILED";
        default: return "OTHER";
    }
}

const char *CardHealthText(CardHealth health)
{
    switch (health) {
        case CARD_OK: return "PASS";
        case CARD_FULL: return "FULL - R/W TEST COULD NOT RUN";
        case CARD_UNFORMATTED: return "UNFORMATTED / FRESH";
        case CARD_FILESYSTEM_BROKEN: return "FILESYSTEM BROKEN / NO FORMAT";
        case CARD_IO_FAILURE: return "I/O FAILURE";
        case CARD_AUTH_FAILURE: return "CARD AUTHENTICATION FAILURE";
        case CARD_DETECT_FAILURE: return "CARD DETECTION FAILURE";
        case CARD_NO_CARD: return "NO CARD";
        default: return "UNKNOWN";
    }
}

const char *CardTypeText(int type)
{
    switch (type) {
        case MC_TYPE_NONE: return "none";
        case MC_TYPE_PSX: return "PS1";
        case MC_TYPE_PS2: return "PS2";
        case MC_TYPE_POCKET: return "PocketStation/PDA";
        default: return "non-standard/unknown";
    }
}
