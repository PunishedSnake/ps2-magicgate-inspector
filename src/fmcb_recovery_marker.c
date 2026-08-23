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
 * Linker wrappers attach the marker invariant to the journal API itself:
 * begin -> arm card, rollback -> verify card before restoring, finish -> verify
 * card before publishing success, and probe -> clean only a matching residual
 * marker left by a power cut after COMMITTED was published.
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

typedef struct ResidualMarkerRoot {
    const char *source_root;
    const char *recovery_root;
} ResidualMarkerRoot;

static const ResidualMarkerRoot ResidualRoots[] = {
    {"mass:/FMCB",  "mass:/FMCB/MCI-RECOVERY"},
    {"mass0:/FMCB", "mass0:/FMCB/MCI-RECOVERY"},
    {"mass1:/FMCB", "mass1:/FMCB/MCI-RECOVERY"}
};

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

    /* Evidence is removed only after both copies have been read and proven
     * identical. A wrong/missing card must never consume the USB token that is
     * needed to identify the original transaction target later. */
    rc = ReadMassMarker(status, &usb_marker);
    if (rc < 0)
        return rc;
    rc = ReadCardMarker(target_port, &card_marker);
    if (rc < 0)
        return rc;
    if (memcmp(&usb_marker, &card_marker, sizeof(usb_marker)) != 0)
        return -5169;

    mcDelete(target_port, 0, FMCB_RECOVERY_CARD_MARKER);
    rc = McResult();
    if (rc < 0 && rc != sceMcResNoEntry)
        return rc;

    TokenPath(status, token_path, sizeof(token_path));
    rc = fileXioRemove(token_path);
    if (rc < 0)
        return rc;
    (void)fileXioRmdir(status->recovery_root);
    return 0;
}

static void DiscardFailedArmArtifacts(const FmcbRecoveryStatus *status)
{
    char token_path[FMCB_RECOVERY_PATH_MAX + 32];

    if (status == NULL || status->target_port < 0 || status->target_port > 1)
        return;
    /* MarkerAlreadyExists() passed before these artifacts could have been
     * created, so anything at our marker path after a later arm failure belongs
     * to this aborted attempt rather than to an older transaction. */
    mcDelete(status->target_port, 0, FMCB_RECOVERY_CARD_MARKER);
    (void)McResult();
    TokenPath(status, token_path, sizeof(token_path));
    (void)fileXioRemove(token_path);
    (void)fileXioRmdir(status->recovery_root);
}

static int ReconcileResidualMarkers(FmcbRecoveryStatus *status)
{
    unsigned int i;

    for (i = 0; i < sizeof(ResidualRoots) / sizeof(ResidualRoots[0]); i++) {
        FmcbRecoveryStatus residual;
        RecoveryCardMarker usb_marker;
        RecoveryCardMarker card_marker;
        int rc;

        memset(&residual, 0, sizeof(residual));
        residual.target_port = -1;
        snprintf(residual.source_root, sizeof(residual.source_root), "%s",
                 ResidualRoots[i].source_root);
        snprintf(residual.recovery_root, sizeof(residual.recovery_root), "%s",
                 ResidualRoots[i].recovery_root);

        rc = ReadMassMarker(&residual, &usb_marker);
        if (rc < 0)
            continue;

        residual.present = 1;
        residual.valid = 1;
        residual.target_port = usb_marker.target_port;
        residual.marker_token = usb_marker.token;
        residual.probe_rc = 0;

        rc = ReadCardMarker(residual.target_port, &card_marker);
        if (rc == 0 &&
            memcmp(&usb_marker, &card_marker, sizeof(usb_marker)) == 0) {
            rc = FmcbRecoveryClearCardMarker(&residual,
                                             residual.target_port);
            if (rc == 0)
                continue;
        }

        /* A residual token without its matching card is deliberately sticky.
         * This most commonly means COMMITTED cleanup was interrupted and the
         * original card is not currently present. Block a new install rather
         * than overwriting the only identity evidence. */
        residual.valid = 0;
        residual.state = FMCB_RECOVERY_CORRUPT;
        residual.probe_rc = rc < 0 ? rc : -5169;
        *status = residual;
        return residual.probe_rc;
    }
    return 0;
}

/* Journal lifecycle wrappers. The core recovery implementation deliberately
 * knows nothing about the card marker; linking these wrappers makes the safety
 * invariant unavoidable for all current installer callers. */
int __real_FmcbRecoveryProbe(const FmcbMassBackendStatus *backend,
                             FmcbRecoveryStatus *status);
int __real_FmcbRecoveryBegin(const FmcbPackageReport *package,
                             FmcbRecoveryStatus *status);
int __real_FmcbRecoveryRun(FmcbRecoveryStatus *status, int *rollback_rc);
int __real_FmcbRecoveryFinish(FmcbRecoveryStatus *status);

int __wrap_FmcbRecoveryProbe(const FmcbMassBackendStatus *backend,
                             FmcbRecoveryStatus *status)
{
    int rc;
    int residual_rc;

    rc = __real_FmcbRecoveryProbe(backend, status);
    if (status == NULL || status->present)
        return rc;

    /* The real probe intentionally treats COMMITTED as cleanup-only and may
     * remove its journals. card-token.bin survives that cleanup, allowing this
     * wrapper to remove the card marker only after proving the original card is
     * still present. If not, the residual token becomes a blocking condition. */
    residual_rc = ReconcileResidualMarkers(status);
    return residual_rc < 0 ? residual_rc : rc;
}

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
        /* No FMCB destination has been touched yet. Remove the empty journal.
         * If arm failed after confirming the marker path was initially absent,
         * also discard any partial marker/token produced by this attempt. */
        (void)__real_FmcbRecoveryRun(status, &rollback_rc);
        if (rc != -5168)
            DiscardFailedArmArtifacts(&saved);
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

    /* This check is deliberately before the first real rollback operation.
     * Slot number alone is not card identity; both USB and card tokens must
     * agree before any destination from the journal can be restored/deleted. */
    rc = FmcbRecoveryCheckCard(status, status->target_port);
    if (rc < 0)
        return rc;

    saved = *status;
    rc = __real_FmcbRecoveryRun(status, rollback_rc);
    if (rc == 0) {
        int marker_rc = FmcbRecoveryClearCardMarker(&saved,
                                                    saved.target_port);
        if (marker_rc < 0)
            return marker_rc;
    }
    return rc;
}

int __wrap_FmcbRecoveryFinish(FmcbRecoveryStatus *status)
{
    FmcbRecoveryStatus saved;
    int rc;

    if (status == NULL)
        return -1;

    /* Verify that the card being declared committed is still the card that was
     * armed at transaction start. The real finish removes journals/backups but
     * deliberately leaves card-token.bin for the post-commit marker cleanup. */
    rc = FmcbRecoveryCheckCard(status, status->target_port);
    if (rc < 0)
        return rc;

    saved = *status;
    rc = __real_FmcbRecoveryFinish(status);
    if (rc == 0) {
        int marker_rc = FmcbRecoveryClearCardMarker(&saved,
                                                    saved.target_port);
        if (marker_rc < 0)
            return marker_rc;
    }
    return rc;
}
