/*
 * PS2 Memory Card Inspector - ordinary filesystem diagnostics
 *
 * This module intentionally knows nothing about the isolated SECR/MagicGate
 * personality. It runs against the normal Sony ROM X memory-card stack and
 * answers a separate question: is the card usable as a PS2 filesystem and can
 * a small temporary file survive create/write/flush/reopen/read/delete?
 *
 * MagicGate capability is reported independently so a healthy third-party card
 * without CardAuth support is not mislabeled as a broken filesystem.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <stdio.h>
#include <string.h>

#include "card.h"

#define CARD_RW_SIZE 4096

static unsigned char WriteBuffer[CARD_RW_SIZE] __attribute__((aligned(64)));
static unsigned char ReadBuffer[CARD_RW_SIZE] __attribute__((aligned(64)));

static int McSyncResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static void FillPattern(unsigned char *buffer, int size, int port)
{
    int i;
    unsigned int x = 0x4D434900u ^ (unsigned int)port;

    for (i = 0; i < size; i++) {
        x = x * 1664525u + 1013904223u;
        buffer[i] = (unsigned char)((x >> 24) ^ i ^ (port * 0x5A));
    }
}

static int OpenRoot(int port)
{
    char path[16];
    int fd;
    int rc;

    snprintf(path, sizeof(path), "mc%d:/", port);
    mcOpen(port, 0, path + 4, O_RDONLY);
    rc = McSyncResult();
    if (rc < 0)
        return rc;

    fd = rc;
    mcClose(fd);
    rc = McSyncResult();
    return rc < 0 ? rc : 0;
}

static int DeleteIfPresent(int port, const char *name)
{
    int rc;

    mcDelete(port, 0, name);
    rc = McSyncResult();
    if (rc == sceMcResNoEntry)
        return 0;
    return rc;
}

static int RunRwTest(int port, CardReport *report)
{
    char name[32];
    int fd = -1;
    int rc;
    int i;

    snprintf(name, sizeof(name), "/__MCI%d.TMP", port);
    report->rw_stage = CARD_RW_CREATE;
    report->cleanup_rc = DeleteIfPresent(port, name);
    if (report->cleanup_rc < 0)
        return report->cleanup_rc;

    FillPattern(WriteBuffer, sizeof(WriteBuffer), port);
    memset(ReadBuffer, 0, sizeof(ReadBuffer));

    mcOpen(port, 0, name, O_CREAT | O_TRUNC | O_RDWR);
    rc = McSyncResult();
    if (rc < 0)
        goto cleanup;
    fd = rc;

    report->rw_stage = CARD_RW_WRITE;
    mcWrite(fd, WriteBuffer, sizeof(WriteBuffer));
    rc = McSyncResult();
    if (rc != (int)sizeof(WriteBuffer)) {
        if (rc >= 0)
            rc = -1001;
        goto cleanup;
    }

    report->rw_stage = CARD_RW_FLUSH;
    mcFlush(fd);
    rc = McSyncResult();
    if (rc < 0)
        goto cleanup;

    mcClose(fd);
    rc = McSyncResult();
    fd = -1;
    if (rc < 0)
        goto cleanup;

    report->rw_stage = CARD_RW_REOPEN;
    mcOpen(port, 0, name, O_RDONLY);
    rc = McSyncResult();
    if (rc < 0)
        goto cleanup;
    fd = rc;

    report->rw_stage = CARD_RW_READ;
    mcRead(fd, ReadBuffer, sizeof(ReadBuffer));
    rc = McSyncResult();
    if (rc != (int)sizeof(ReadBuffer)) {
        if (rc >= 0)
            rc = -1002;
        goto cleanup;
    }

    mcClose(fd);
    rc = McSyncResult();
    fd = -1;
    if (rc < 0)
        goto cleanup;

    report->rw_stage = CARD_RW_COMPARE;
    for (i = 0; i < (int)sizeof(WriteBuffer); i++) {
        if (WriteBuffer[i] != ReadBuffer[i]) {
            rc = -1003;
            goto cleanup;
        }
    }

    report->rw_stage = CARD_RW_DELETE;
    mcDelete(port, 0, name);
    rc = McSyncResult();
    if (rc < 0)
        goto cleanup;

    report->rw_stage = CARD_RW_VERIFY_DELETE;
    mcOpen(port, 0, name, O_RDONLY);
    rc = McSyncResult();
    if (rc != sceMcResNoEntry) {
        if (rc >= 0) {
            fd = rc;
            rc = -1004;
        }
        goto cleanup;
    }

    report->cleanup_rc = 0;
    report->rw_stage = CARD_RW_DONE;
    return 0;

cleanup:
    if (fd >= 0) {
        mcClose(fd);
        McSyncResult();
    }
    report->cleanup_rc = DeleteIfPresent(port, name);
    return rc;
}

void CardInspect(int port, CardReport *report)
{
    int rc;

    memset(report, 0, sizeof(*report));
    report->port = port;
    report->info_rc = -999;
    report->root_rc = -999;
    report->rw_rc = -999;
    report->cleanup_rc = -999;
    report->rw_stage = CARD_RW_NOT_RUN;
    report->health = CARD_HEALTH_UNKNOWN;

    mcGetInfo(port, 0, &report->type, &report->free_clusters,
              &report->formatted);
    report->info_rc = McSyncResult();

    if (report->info_rc == sceMcResNoEntry || report->type == MC_TYPE_NONE) {
        report->health = CARD_HEALTH_NO_CARD;
        return;
    }

    if (report->info_rc < 0 && report->info_rc != sceMcResNoFormat) {
        report->health = CARD_HEALTH_FILESYSTEM_ERROR;
        return;
    }

    if (report->type != MC_TYPE_PS2) {
        report->health = CARD_HEALTH_NOT_PS2;
        return;
    }

    if (report->info_rc == sceMcResNoFormat || !report->formatted) {
        report->health = CARD_HEALTH_UNFORMATTED;
        report->format_allowed = 1;
        return;
    }

    report->root_rc = OpenRoot(port);
    if (report->root_rc < 0) {
        report->health = CARD_HEALTH_FILESYSTEM_ERROR;
        return;
    }

    rc = RunRwTest(port, report);
    report->rw_rc = rc;
    if (rc < 0) {
        report->health = CARD_HEALTH_RW_ERROR;
        return;
    }

    report->health = CARD_HEALTH_HEALTHY;
}

int CardFormat(int port, CardReport *report)
{
    int rc;

    if (report == NULL || !report->format_allowed ||
        report->type != MC_TYPE_PS2)
        return -2001;

    mcFormat(port, 0);
    rc = McSyncResult();
    CardInspect(port, report);
    return rc;
}

const char *CardResultText(int result)
{
    switch (result) {
        case sceMcResSucceed: return "OK";
        case sceMcResChangedCard: return "CHANGED CARD";
        case sceMcResNoFormat: return "NO FORMAT";
        case sceMcResFullDevice: return "FULL";
        case sceMcResNoEntry: return "NO ENTRY/CARD";
        case sceMcResDeniedPermit: return "DENIED";
        case -1001: return "SHORT WRITE";
        case -1002: return "SHORT READ";
        case -1003: return "DATA MISMATCH";
        case -1004: return "DELETE VERIFY FAILED";
        case -2001: return "FORMAT LOCKED";
        default: return "ERROR/UNKNOWN";
    }
}

const char *CardTypeText(int type)
{
    switch (type) {
        case MC_TYPE_NONE: return "NONE";
        case MC_TYPE_PS1: return "PS1";
        case MC_TYPE_PS2: return "PS2";
        case MC_TYPE_PDA: return "PDA";
        default: return "UNKNOWN";
    }
}

const char *CardHealthText(CardHealth health)
{
    switch (health) {
        case CARD_HEALTH_NO_CARD: return "NO CARD";
        case CARD_HEALTH_NOT_PS2: return "NOT PS2 CARD";
        case CARD_HEALTH_UNFORMATTED: return "UNFORMATTED";
        case CARD_HEALTH_FILESYSTEM_ERROR: return "FILESYSTEM ERROR";
        case CARD_HEALTH_RW_ERROR: return "R/W TEST FAILED";
        case CARD_HEALTH_HEALTHY: return "PASS";
        default: return "NOT TESTED";
    }
}

const char *CardRwStageText(CardRwStage stage)
{
    switch (stage) {
        case CARD_RW_CREATE: return "CREATE";
        case CARD_RW_WRITE: return "WRITE";
        case CARD_RW_FLUSH: return "FLUSH";
        case CARD_RW_REOPEN: return "REOPEN";
        case CARD_RW_READ: return "READ";
        case CARD_RW_COMPARE: return "COMPARE";
        case CARD_RW_DELETE: return "DELETE";
        case CARD_RW_VERIFY_DELETE: return "VERIFY DELETE";
        case CARD_RW_DONE: return "DONE";
        default: return "NOT RUN";
    }
}
