/*
 * PS2 Memory Card Inspector - ordinary memory-card diagnostics
 *
 * This module owns the normal Sony ROM X / XMCMAN filesystem side of the
 * Inspector. MagicGate is intentionally separate: a card may be a perfectly
 * healthy PS2 memory card while still lacking the CardAuth behavior required
 * for SECR/KELF binding.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <io_common.h>
#include <stdio.h>
#include <string.h>

#include "card.h"
#include "progress.h"

#define TEST_SIZE 4096

static unsigned char WriteBuffer[TEST_SIZE] __attribute__((aligned(64)));
static unsigned char ReadBuffer[TEST_SIZE] __attribute__((aligned(64)));
static sceMcTblGetDir DirEntry __attribute__((aligned(64)));

static void CardProgress(int port, int percent,
                         const char *action, const char *detail)
{
    char line[192];

    snprintf(line, sizeof(line), "mc%d: %s", port,
             detail != NULL ? detail : "");
    MciProgressUpdate(MCI_PROGRESS_FILESYSTEM, percent, action, line);
}

static int McSyncResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static void FillPattern(void)
{
    unsigned int i;
    unsigned int state = 0x4D43494Eu; /* "MCIN" */

    for (i = 0; i < sizeof(WriteBuffer); i++) {
        state = state * 1664525u + 1013904223u;
        WriteBuffer[i] = (unsigned char)((state >> 24) ^ i ^ (i >> 4));
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

static int RunReadWriteTest(int port, int *cleanup_rc, RwStage *stage)
{
    char path[24];
    char detail[192];
    int fd;
    int rc;

    *cleanup_rc = 0;
    *stage = RW_FIND_NAME;
    FillPattern();
    memset(ReadBuffer, 0, sizeof(ReadBuffer));

    CardProgress(port, 24, "Choosing a temporary test file",
                 "Searching for an unused /__MCIxx.TMP name; existing files are never overwritten.");
    rc = FindUnusedTempName(port, path);
    if (rc < 0)
        return rc;

    *stage = RW_VERIFY_CARD;
    snprintf(detail, sizeof(detail),
             "Temporary path %s selected. Re-checking the slot before creating it.", path);
    CardProgress(port, 31, "Confirming the same card is still inserted", detail);
    rc = VerifySameCard(port);
    if (rc != sceMcResSucceed)
        return rc;

    /* libmc passes IOP open flags directly to XMCMAN: always use FIO_O_*. */
    *stage = RW_OPEN_WRITE;
    snprintf(detail, sizeof(detail),
             "Creating %s for a disposable 4 KiB integrity test.", path);
    CardProgress(port, 38, "Opening the temporary file for writing", detail);
    mcOpen(port, 0, path, FIO_O_WRONLY | FIO_O_CREAT);
    fd = McSyncResult();
    if (fd < 0)
        return fd;

    *stage = RW_WRITE;
    CardProgress(port, 45, "Writing the 4 KiB test pattern",
                 "Writing deterministic data that will be read back and compared byte-for-byte.");
    mcWrite(fd, WriteBuffer, sizeof(WriteBuffer));
    rc = McSyncResult();
    if (rc != (int)sizeof(WriteBuffer)) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return (rc < 0) ? rc : -1001;
    }

    *stage = RW_FLUSH;
    CardProgress(port, 52, "Flushing the write",
                 "Forcing the memory-card driver to commit the temporary file before verification.");
    mcFlush(fd);
    rc = McSyncResult();
    if (rc < 0) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_CLOSE_WRITE;
    CardProgress(port, 58, "Closing the write handle",
                 "Finishing the write phase cleanly before reopening the file read-only.");
    rc = CloseFile(fd);
    if (rc < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_OPEN_READ;
    CardProgress(port, 64, "Reopening the temporary file",
                 "Opening the committed test file read-only for independent read-back verification.");
    mcOpen(port, 0, path, FIO_O_RDONLY);
    fd = McSyncResult();
    if (fd < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return fd;
    }

    *stage = RW_READ;
    CardProgress(port, 71, "Reading the 4 KiB test pattern back",
                 "Reading the complete file into a separate EE buffer for comparison.");
    mcRead(fd, ReadBuffer, sizeof(ReadBuffer));
    rc = McSyncResult();
    if (rc != (int)sizeof(ReadBuffer)) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return (rc < 0) ? rc : -1002;
    }

    *stage = RW_CLOSE_READ;
    CardProgress(port, 77, "Closing the read handle",
                 "The read-back buffer is complete; the file handle is no longer needed.");
    rc = CloseFile(fd);
    if (rc < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_COMPARE;
    CardProgress(port, 84, "Comparing written and read-back data",
                 "Checking all 4096 bytes. Any mismatch is reported as a filesystem I/O failure.");
    if (memcmp(WriteBuffer, ReadBuffer, sizeof(WriteBuffer)) != 0) {
        *cleanup_rc = DeleteFile(port, path);
        return -1003;
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
    CardProgress(port, 100, "Filesystem integrity test complete",
                 "Metadata, root directory, write, flush, read-back, compare and cleanup all completed.");
    return 0;
}

void CardInspect(int port, CardReport *r)
{
    char detail[160];

    memset(r, 0, sizeof(*r));
    r->port = port;
    r->root_rc = -999;
    r->rw_rc = -999;
    r->cleanup_rc = 0;
    r->rw_stage = RW_NOT_RUN;
    r->health = CARD_UNKNOWN;

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
             "PS2 card detected; formatted=%d, free clusters=%d. Checking the root directory.",
             r->formatted, r->free_clusters);
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

    CardProgress(port, 20, "Root directory is readable",
                 "Starting the disposable 4 KiB write / flush / read-back integrity test.");
    r->rw_rc = RunReadWriteTest(port, &r->cleanup_rc, &r->rw_stage);

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

int CardFormat(int port, CardReport *report)
{
    int rc;

    CardProgress(port, 10, "Formatting the memory card",
                 "The destructive mcFormat operation is running. Do not remove the card.");
    mcFormat(port, 0);
    rc = McSyncResult();
    if (rc == 0) {
        CardProgress(port, 35, "Format completed; verifying the new filesystem",
                     "Re-running the complete filesystem inspection on the freshly formatted card.");
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
