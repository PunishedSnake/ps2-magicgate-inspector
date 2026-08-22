/* SPDX-License-Identifier: MIT */
/*
 * Card-side identity marker for persistent FMCB recovery.
 *
 * A USB journal contains paths, not a cryptographic memory-card identity. To
 * stop a user from accidentally replaying an old journal onto a different card
 * in the same slot, the installer writes one tiny transaction token to the
 * target before any FMCB destination is modified. The matching token is also
 * stored beside the USB journal. Recovery from a later boot must prove both
 * copies match before restoring anything.
 *
 * The three lifecycle wrappers at the bottom keep marker ordering attached to
 * the journal API itself: begin -> arm card, successful finish -> clear marker,
 * successful rollback -> clear marker. This is the same linker-wrapper pattern
 * already used by the MagicGate session and keeps the transaction core boring.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <timer.h>
#include <stdio.h>
#include <string.h>

#include "fmcb_recovery.h"

#define MARKER_MAGIC 0x4D434954u /* MCIT */
#define MARKER_FILE "card-token.bin"

typedef struct RecoveryCardMarker {
    u32 magic;
    u32 token;
    s32 target_port;
    u32 checksum;
} RecoveryCardMarker;

static int McResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static u32 MarkerChecksum(const RecoveryCardMarker *marker)
{
    return marker->magic ^ marker->token ^ (u32)marker->target_port ^
           0xA5C34F21u;
}

static int MarkerValid(const RecoveryCardMarker *marker)
{
    return marker->magic == MARKER_MAGIC &&
           marker->target_port >= 0 && marker->target_port <= 1 &&
           marker->token != 0u &&
           marker->checksum == MarkerChecksum(marker);
}

static void TokenPath(const FmcbRecoveryStatus *status,
                      char *path, unsigned int path_size)
{
    snprintf(path, path_size, "%s/%s", status->recovery_root, MARKER_FILE);
}

static int WriteMassMarker(const FmcbRecoveryStatus *status,
                           const RecoveryCardMarker *marker)
{
    RecoveryCardMarker verify;
    char path[FMCB_RECOVERY_PATH_MAX + 32];
    int fd;
    int rc;

    TokenPath(status, path, sizeof(path));
    (void)fileXioRemove(path);
    fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT);
    if (fd < 0)
        return fd;
    rc = fileXioWrite(fd, marker, sizeof(*marker));
    if (rc != (int)sizeof(*marker)) {
        fileXioClose(fd);
        return rc < 0 ? rc : -5160;
    }
    rc = fileXioClose(fd);
    if (rc < 0)
        return rc;

    memset(&verify, 0, sizeof(verify));
    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;
    rc = fileXioRead(fd, &verify, sizeof(verify));
    fileXioClose(fd);
    if (rc != (int)sizeof(verify) ||
        memcmp(&verify, marker, sizeof(verify)) != 0)
        return rc < 0 ? rc : -5161;
    return 0;
}

static int ReadMassMarker(const FmcbRecoveryStatus *status,
                          RecoveryCardMarker *marker)
{
    char path[FMCB_RECOVERY_PATH_MAX + 32];
    int fd;
    int rc;

    TokenPath(status, path, sizeof(path));
    memset(marker, 0, sizeof(*marker));
    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;
    rc = fileXioRead(fd, marker, sizeof(*marker));
    fileXioClose(fd);
    if (rc != (int)sizeof(*marker))
        return rc < 0 ? rc : -5162;
    return MarkerValid(marker) ? 0 : -5163;
}

static int MarkerAlreadyExists(int target_port)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int rc;

    memset(&info, 0, sizeof(info));
    mcGetDir(target_port, 0, FMCB_RECOVERY_CARD_MARKER, 0, 1, &info);
    rc = McResult();
    if (rc == sceMcResNoEntry || rc == 0)
        return 0;
    if (rc < 0)
        return rc;
    return 1;
}

static int WriteCardMarker(int target_port,
                           const RecoveryCardMarker *marker)
{
    RecoveryCardMarker verify __attribute__((aligned(64)));
    int fd;
    int rc;

    mcOpen(target_port, 0, FMCB_RECOVERY_CARD_MARKER,
           FIO_O_WRONLY | FIO_O_CREAT);
    fd = McResult();
    if (fd < 0)
        return fd;
    mcWrite(fd, marker, sizeof(*marker));
    rc = McResult();
    if (rc != (int)sizeof(*marker)) {
        mcClose(fd);
        (void)McResult();
        return rc < 0 ? rc : -5164;
    }
    mcFlush(fd);
    rc = McResult();
    if (rc < 0) {
        mcClose(fd);
        (void)McResult();
        return rc;
    }
    mcClose(fd);
    rc = McResult();
    if (rc < 0)
        return rc;

    memset(&verify, 0, sizeof(verify));
    mcOpen(target_port, 0, FMCB_RECOVERY_CARD_MARKER, FIO_O_RDONLY);
    fd = McResult();
    if (fd < 0)
        return fd;
    mcRead(fd, &verify, sizeof(verify));
    rc = McResult();
    mcClose(fd);
    (void)McResult();
    if (rc != (int)sizeof(verify) ||
        memcmp(&verify, marker, sizeof(verify)) != 0)
        return rc < 0 ? rc : -5165;
    return 0;
}

static int ReadCardMarker(int target_port, RecoveryCardMarker *marker)
{
    int fd;
    int rc;

    memset(marker, 0, sizeof(*marker));
    mcOpen(target_port, 0, FMCB_RECOVERY_CARD_MARKER, FIO_O_RDONLY);
    fd = McResult();
    if (fd < 0)
        return fd;
    mcRead(fd, marker, sizeof(*marker));
    rc = McResult();
    mcClose(fd);
    (void)McResult();
    if (rc != (int)sizeof(*marker))
        return rc < 0 ? rc : -5166;
    return MarkerValid(marker) ? 0 : -5167;
}

int FmcbRecoveryArmCard(FmcbRecoveryStatus *status, int target_port)
{
    RecoveryCardMarker marker;
    u64 now;
    int rc;

    if (status == NULL || !status->valid ||
        target_port != status->target_port)
        return -1;

    rc = MarkerAlreadyExists(target_port);
    if (rc != 0)
        return rc > 0 ? -5168 : rc;

    now = GetTimerSystemTime();
    memset(&marker, 0, sizeof(marker));
    marker.magic = MARKER_MAGIC;
    marker.token = (u32)now ^ (u32)(now >> 32) ^
                   0x4D434900u ^ ((u32)target_port << 28) ^ status->sequence;
    if (marker.token == 0u)
        marker.token = 0x4D434901u;
    marker.target_port = target_port;
    marker.checksum = MarkerChecksum(&marker);

    /* USB token first. A power cut before the card marker is written has not
     * modified any FMCB destination and leaves enough evidence to distinguish
     * an unarmed transaction from a wrong-card recovery attempt. */
    rc = WriteMassMarker(status, &marker);
    if (rc < 0)
        return rc;
    rc = WriteCardMarker(target_port, &marker);
    if (rc < 0)
        return rc;
    status->marker_token = marker.token;
    return 0;
}

int FmcbRecoveryCheckCard(FmcbRecoveryStatus *status, int target_port)
{
    RecoveryCardMarker usb_marker;
    RecoveryCardMarker card_marker;
    int rc;

    if (status == NULL || !status->valid ||
        target_port != status->target_port)
        return -1;
    rc = ReadMassMarker(status, &usb_marker);
    if (rc < 0)
        return rc;
    rc = ReadCardMarker(target_port, &card_marker);
    if (rc < 0)
        return rc;
    if (memcmp(&usb_marker, &card_marker, sizeof(usb_marker)) != 0)
        return -5169;
    status->marker_token = usb_marker.token;
    return 0;
}

int FmcbRecoveryClearCardMarker(const FmcbRecoveryStatus *status,
                                int target_port)
{
    RecoveryCardMarker usb_marker;
    RecoveryCardMarker card_marker;
    char token_path[FMCB_RECOVERY_PATH_MAX + 32];
    int rc;

    if (status == NULL || target_port < 0 || target_port > 1)
        return -1;

    /* Never delete an unrelated file merely because it uses our marker name. */
    rc = ReadMassMarker(status, &usb_marker);
    if (rc == 0 && ReadCardMarker(target_port, &card_marker) == 0 &&
        memcmp(&usb_marker, &card_marker, sizeof(usb_marker)) == 0) {
        mcDelete(target_port, 0, FMCB_RECOVERY_CARD_MARKER);
        rc = McResult();
        if (rc < 0 && rc != sceMcResNoEntry)
            return rc;
    }

    TokenPath(status, token_path, sizeof(token_path));
    (void)fileXioRemove(token_path);
    (void)fileXioRmdir(status->recovery_root);
    return 0;
}

/* Journal lifecycle wrappers. The core recovery implementation deliberately
 * knows nothing about the card marker; linking these wrappers makes the safety
 * invariant unavoidable for all current installer callers. */
int __real_FmcbRecoveryBegin(const FmcbPackageReport *package,
                             FmcbRecoveryStatus *status);
int __real_FmcbRecoveryRun(FmcbRecoveryStatus *status, int *rollback_rc);
int __real_FmcbRecoveryFinish(FmcbRecoveryStatus *status);

int __wrap_FmcbRecoveryBegin(const FmcbPackageReport *package,
                             FmcbRecoveryStatus *status)
{
    FmcbRecoveryStatus saved;
    int rollback_rc = 0;
    int rc;

    rc = __real_FmcbRecoveryBegin(package, status);
    if (rc < 0)
        return rc;
    saved = *status;
    rc = FmcbRecoveryArmCard(status, status->target_port);
    if (rc < 0) {
        /* No FMCB destination has been touched yet. Remove the empty journal
         * and any partially-created marker artifacts so the failure is clean. */
        (void)__real_FmcbRecoveryRun(status, &rollback_rc);
        (void)FmcbRecoveryClearCardMarker(&saved, saved.target_port);
        return rc;
    }
    return 0;
}

int __wrap_FmcbRecoveryRun(FmcbRecoveryStatus *status, int *rollback_rc)
{
    FmcbRecoveryStatus saved;
    int rc;

    if (status == NULL)
        return -1;
    saved = *status;
    rc = __real_FmcbRecoveryRun(status, rollback_rc);
    if (rc == 0)
        (void)FmcbRecoveryClearCardMarker(&saved, saved.target_port);
    return rc;
}

int __wrap_FmcbRecoveryFinish(FmcbRecoveryStatus *status)
{
    FmcbRecoveryStatus saved;
    int rc;

    if (status == NULL)
        return -1;
    saved = *status;
    rc = __real_FmcbRecoveryFinish(status);
    if (rc == 0)
        (void)FmcbRecoveryClearCardMarker(&saved, saved.target_port);
    return rc;
}
