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

#define TEST_SIZE 4096

static unsigned char WriteBuffer[TEST_SIZE] __attribute__((aligned(64)));
static unsigned char ReadBuffer[TEST_SIZE] __attribute__((aligned(64)));
static sceMcTblGetDir DirEntry __attribute__((aligned(64)));

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
    int fd;
    int rc;

    *cleanup_rc = 0;
    *stage = RW_FIND_NAME;
    FillPattern();
    memset(ReadBuffer, 0, sizeof(ReadBuffer));

    rc = FindUnusedTempName(port, path);
    if (rc < 0)
        return rc;

    *stage = RW_VERIFY_CARD;
    rc = VerifySameCard(port);
    if (rc != sceMcResSucceed)
        return rc;

    /* libmc passes IOP open flags directly to XMCMAN: always use FIO_O_*. */
    *stage = RW_OPEN_WRITE;
    mcOpen(port, 0, path, FIO_O_WRONLY | FIO_O_CREAT);
    fd = McSyncResult();
    if (fd < 0)
        return fd;

    *stage = RW_WRITE;
    mcWrite(fd, WriteBuffer, sizeof(WriteBuffer));
    rc = McSyncResult();
    if (rc != (int)sizeof(WriteBuffer)) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return (rc < 0) ? rc : -1001;
    }

    *stage = RW_FLUSH;
    mcFlush(fd);
    rc = McSyncResult();
    if (rc < 0) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_CLOSE_WRITE;
    rc = CloseFile(fd);
    if (rc < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_OPEN_READ;
    mcOpen(port, 0, path, FIO_O_RDONLY);
    fd = McSyncResult();
    if (fd < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return fd;
    }

    *stage = RW_READ;
    mcRead(fd, ReadBuffer, sizeof(ReadBuffer));
    rc = McSyncResult();
    if (rc != (int)sizeof(ReadBuffer)) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return (rc < 0) ? rc : -1002;
    }

    *stage = RW_CLOSE_READ;
    rc = CloseFile(fd);
    if (rc < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    *stage = RW_COMPARE;
    if (memcmp(WriteBuffer, ReadBuffer, sizeof(WriteBuffer)) != 0) {
        *cleanup_rc = DeleteFile(port, path);
        return -1003;
    }

    *stage = RW_DELETE;
    *cleanup_rc = DeleteFile(port, path);
    if (*cleanup_rc < 0)
        return -1004;

    *stage = RW_VERIFY_DELETE;
    memset(&DirEntry, 0, sizeof(DirEntry));
    mcGetDir(port, 0, path, 0, 1, &DirEntry);
    rc = McSyncResult();
    if (rc != 0 && rc != sceMcResNoEntry)
        return -1005;

    *stage = RW_DONE;
    return 0;
}

void CardInspect(int port, CardReport *r)
{
    memset(r, 0, sizeof(*r));
    r->port = port;
    r->root_rc = -999;
    r->rw_rc = -999;
    r->cleanup_rc = 0;
    r->rw_stage = RW_NOT_RUN;
    r->health = CARD_UNKNOWN;

    mcGetInfo(port, 0, &r->type, &r->free_clusters, &r->formatted);
    r->info_rc = McSyncResult();

    if (r->info_rc == sceMcResFailAuth) {
        r->health = CARD_AUTH_FAILURE;
        return;
    }
    if (r->info_rc <= sceMcResFailDetect) {
        r->health = CARD_DETECT_FAILURE;
        return;
    }
    if (r->type == MC_TYPE_NONE) {
        r->health = CARD_NO_CARD;
        return;
    }
    if (r->info_rc == sceMcResNoFormat || !r->formatted) {
        r->health = CARD_UNFORMATTED;
        r->format_allowed = (r->type == MC_TYPE_PS2);
        return;
    }

    memset(&DirEntry, 0, sizeof(DirEntry));
    mcGetDir(port, 0, "/*", 0, 1, &DirEntry);
    r->root_rc = McSyncResult();

    if (r->root_rc == sceMcResNoFormat) {
        r->health = CARD_FILESYSTEM_BROKEN;
        r->format_allowed = (r->type == MC_TYPE_PS2);
        return;
    }
    if (r->root_rc == sceMcResFailAuth) {
        r->health = CARD_AUTH_FAILURE;
        return;
    }
    if (r->root_rc <= sceMcResFailDetect) {
        r->health = CARD_DETECT_FAILURE;
        return;
    }
    if (r->root_rc < 0 && r->root_rc != sceMcResNoEntry) {
        r->health = CARD_IO_FAILURE;
        return;
    }

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

    mcFormat(port, 0);
    rc = McSyncResult();
    if (rc == 0)
        CardInspect(port, report);
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
