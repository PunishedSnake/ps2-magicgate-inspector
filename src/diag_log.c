/* SPDX-License-Identifier: MIT */
/*
 * Crash-oriented Drebin diagnostic logger.
 *
 * IMPORTANT LIFECYCLE RULE:
 * The logger never owns fileXio initialization and never probes mass: from a
 * linker wrapper. Drebin deliberately replaces the IOP personality at runtime;
 * only the application knows when a newly bound fileXio client and USBHDFSD
 * have had enough time to become usable. Callers therefore explicitly attach
 * and detach the logger at safe lifecycle boundaries.
 *
 * Every durable line is still opened in append mode, written, closed and
 * best-effort synced. While mass: is detached, a bounded EE-side ring retains
 * the latest trace lines and flushes them after a later explicit attach.
 */

#define NEWLIB_PORT_AWARE

#include <delaythread.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "diag_log.h"

#define DIAG_LINE_MAX 320u
#define DIAG_PENDING_LINES 64u
#define DIAG_ATTACH_ATTEMPTS 4u
#define DIAG_ATTACH_DELAY_USEC 50000u

typedef struct MciDiagRoot {
    const char *root;
    const char *device;
} MciDiagRoot;

static const MciDiagRoot Roots[] = {
    {"mass:/", "mass:"},
    {"mass0:/", "mass0:"},
    {"mass1:/", "mass1:"}
};

static char Pending[DIAG_PENDING_LINES][DIAG_LINE_MAX];
static unsigned int PendingHead;
static unsigned int PendingCount;
static unsigned int DroppedLines;
static unsigned int Sequence;
static char LogPath[96];
static char LogDevice[16];
static int IoAvailable;
static int PathReady;
static int Initialized;
static int InWrite;

static int WriteAll(int fd, const char *text, unsigned int length)
{
    unsigned int done = 0u;

    while (done < length) {
        int rc = fileXioWrite(fd, text + done, (int)(length - done));
        if (rc <= 0)
            return rc < 0 ? rc : -1;
        done += (unsigned int)rc;
    }
    return 0;
}

static int EnsurePath(void)
{
    unsigned int i;

    if (PathReady)
        return 0;

    for (i = 0; i < sizeof(Roots) / sizeof(Roots[0]); i++) {
        char directory[64];
        iox_stat_t stat;
        int fd;
        int rc;

        fd = fileXioDopen(Roots[i].root);
        if (fd < 0)
            continue;
        fileXioDclose(fd);

        snprintf(directory, sizeof(directory), "%sMCI", Roots[i].root);
        memset(&stat, 0, sizeof(stat));
        rc = fileXioGetStat(directory, &stat);
        if (rc < 0) {
            rc = fileXioMkdir(directory, 0777);
            if (rc < 0) {
                memset(&stat, 0, sizeof(stat));
                if (fileXioGetStat(directory, &stat) < 0)
                    continue;
            }
        } else if (!FIO_S_ISDIR(stat.mode)) {
            continue;
        }

        snprintf(LogPath, sizeof(LogPath), "%sMCI/DREBIN.LOG", Roots[i].root);
        snprintf(LogDevice, sizeof(LogDevice), "%s", Roots[i].device);
        PathReady = 1;
        return 0;
    }
    return -1;
}

static int WriteRawLineNow(const char *line)
{
    unsigned int length;
    int fd;
    int rc;
    int close_rc;

    if (!IoAvailable || InWrite)
        return -1;
    if (EnsurePath() < 0)
        return -2;

    length = (unsigned int)strlen(line);
    InWrite = 1;
    fd = fileXioOpen(LogPath, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_APPEND);
    if (fd < 0) {
        InWrite = 0;
        PathReady = 0;
        return fd;
    }

    rc = WriteAll(fd, line, length);
    if (rc == 0)
        rc = WriteAll(fd, "\n", 1u);
    close_rc = fileXioClose(fd);
    if (rc == 0 && close_rc < 0)
        rc = close_rc;

    if (rc == 0) {
        (void)fileXioSync(LogDevice, 0);
        DelayThread(2000);
    }
    InWrite = 0;
    return rc;
}

static void QueueLine(const char *line)
{
    unsigned int index;

    if (PendingCount < DIAG_PENDING_LINES) {
        index = (PendingHead + PendingCount) % DIAG_PENDING_LINES;
        PendingCount++;
    } else {
        PendingHead = (PendingHead + 1u) % DIAG_PENDING_LINES;
        index = (PendingHead + PendingCount - 1u) % DIAG_PENDING_LINES;
        DroppedLines++;
    }
    snprintf(Pending[index], sizeof(Pending[index]), "%s", line);
}

static void EnsureInitialized(void)
{
    if (Initialized)
        return;
    Initialized = 1;
    Sequence = 1u;
    PendingHead = 0u;
    PendingCount = 1u;
    DroppedLines = 0u;
    LogPath[0] = '\0';
    LogDevice[0] = '\0';
    IoAvailable = 0;
    PathReady = 0;
    InWrite = 0;
    snprintf(Pending[0], sizeof(Pending[0]),
             "#%06u [SESSION] ========== Drebin diagnostic session start ==========",
             Sequence);
}

static void FlushPending(void)
{
    char line[DIAG_LINE_MAX];

    if (!IoAvailable)
        return;
    if (EnsurePath() < 0) {
        IoAvailable = 0;
        return;
    }

    if (DroppedLines > 0u) {
        snprintf(line, sizeof(line),
                 "#%06u [LOGGER] pending ring overflow; %u older line(s) dropped",
                 ++Sequence, DroppedLines);
        if (WriteRawLineNow(line) < 0) {
            IoAvailable = 0;
            PathReady = 0;
            return;
        }
        DroppedLines = 0u;
    }

    while (PendingCount > 0u) {
        unsigned int index = PendingHead;
        if (WriteRawLineNow(Pending[index]) < 0) {
            IoAvailable = 0;
            PathReady = 0;
            return;
        }
        PendingHead = (PendingHead + 1u) % DIAG_PENDING_LINES;
        PendingCount--;
    }
}

void MciDiagLogReset(void)
{
    EnsureInitialized();
    PendingHead = 0u;
    PendingCount = 0u;
    DroppedLines = 0u;
    Sequence = 0u;
    IoAvailable = 0;
    PathReady = 0;
    InWrite = 0;
    LogPath[0] = '\0';
    LogDevice[0] = '\0';
    MciDiagLogPrintf("SESSION", "========== Drebin diagnostic session start ==========");
}

void MciDiagLogSetIoAvailable(int available)
{
    unsigned int attempt;

    EnsureInitialized();

    if (!available) {
        /* Detach must be safe even if the caller is about to reset the IOP or
         * has already lost the old RPC server. Never perform USB I/O here. */
        IoAvailable = 0;
        PathReady = 0;
        LogPath[0] = '\0';
        LogDevice[0] = '\0';
        MciDiagLogPrintf("LOGGER", "mass/fileXio detached; buffering trace in EE RAM");
        return;
    }

    if (IoAvailable && PathReady)
        return;

    /* This is called only after the application has explicitly allowed USB
     * enumeration time. Failure is non-fatal: stay RAM-only and try again at
     * the next safe lifecycle boundary. */
    for (attempt = 0u; attempt < DIAG_ATTACH_ATTEMPTS; attempt++) {
        IoAvailable = 1;
        PathReady = 0;
        if (EnsurePath() == 0) {
            FlushPending();
            if (IoAvailable)
                MciDiagLogPrintf("LOGGER",
                                 "mass/fileXio attached after %u attempt(s); path=%s",
                                 attempt + 1u, LogPath);
            return;
        }
        IoAvailable = 0;
        PathReady = 0;
        if (attempt + 1u < DIAG_ATTACH_ATTEMPTS)
            DelayThread(DIAG_ATTACH_DELAY_USEC);
    }
}

void MciDiagLogPrintf(const char *component, const char *format, ...)
{
    char payload[240];
    char line[DIAG_LINE_MAX];
    va_list args;
    int rc;

    EnsureInitialized();
    if (component == NULL)
        component = "GEN";
    if (format == NULL)
        format = "";

    va_start(args, format);
    vsnprintf(payload, sizeof(payload), format, args);
    va_end(args);
    payload[sizeof(payload) - 1u] = '\0';

    snprintf(line, sizeof(line), "#%06u [%s] %s",
             ++Sequence, component, payload);
    line[sizeof(line) - 1u] = '\0';

    if (IoAvailable) {
        rc = WriteRawLineNow(line);
        if (rc == 0)
            return;
        IoAvailable = 0;
        PathReady = 0;
        LogPath[0] = '\0';
        LogDevice[0] = '\0';
    }
    QueueLine(line);
}

void MciDiagLogCheckpoint(const char *component, const char *event, int rc)
{
    MciDiagLogPrintf(component, "%s rc=%d", event != NULL ? event : "checkpoint", rc);
}

const char *MciDiagLogPath(void)
{
    EnsureInitialized();
    return PathReady ? LogPath : "mass?:/MCI/DREBIN.LOG";
}
