/* SPDX-License-Identifier: MIT */
/*
 * Page-level memory-card personality for Drebin Card Tools.
 *
 * Sony ROM XMCSERV intentionally does not expose libmc's legacy raw-page RPCs.
 * Modern PS2SDK's default MCSERV build is also XMC-compatible, which makes
 * libmc identify it as MC_TYPE_XMC and reject mcReadPage/mcWritePage by design.
 * Imaging therefore uses a separately built legacy PS2SDK MCMAN/MCSERV pair
 * with XMC compatibility disabled, plus USBHDFSD in the same temporary IOP.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <delaythread.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <sbv_patches.h>
#include <timer.h>
#include <string.h>

#include "card_raw_session.h"
#include "diag_log.h"
#include "magicgate_session.h"
#include "progress.h"
#include "raw_bulk_read.h"
#include "usb_search.h"

extern unsigned char fmcb_freesio2_irx[];
extern unsigned int size_fmcb_freesio2_irx;
extern unsigned char fmcb_freepad_irx[];
extern unsigned int size_fmcb_freepad_irx;
extern unsigned char raw_mcman_irx[];
extern unsigned int size_raw_mcman_irx;
extern unsigned char raw_mcserv_irx[];
extern unsigned int size_raw_mcserv_irx;
extern unsigned char iomanX_irx[];
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];
extern unsigned int size_fileXio_irx;
extern unsigned char usbd_irx[];
extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[];
extern unsigned int size_usbhdfsd_irx;

static u64 RawSessionStartTicks;
static u64 RawSessionReadyTicks;

static int ExecEmbedded(const unsigned char *data, unsigned int size)
{
    int start_rc = -999;
    int module_id = SifExecModuleBuffer((void *)data, size, 0, NULL, &start_rc);

    if (module_id < 0)
        return module_id;
    /* IRX resident/no-resident return conventions are module-specific. The
     * module id is the reliable load result; client binding below proves the
     * service actually became usable. */
    return module_id;
}

static u64 TicksToUsec(u64 ticks)
{
    u32 seconds = 0u;
    u32 useconds = 0u;

    TimerBusClock2USec(ticks, &seconds, &useconds);
    return (u64)seconds * 1000000u + useconds;
}

void MciRawCardSessionReset(MciRawCardSessionStatus *status)
{
    memset(status, 0, sizeof(*status));
    status->sio2_rc = -999;
    status->pad_rc = -999;
    status->mcman_rc = -999;
    status->mcserv_rc = -999;
    status->iomanx_rc = -999;
    status->filexio_module_rc = -999;
    status->usbd_rc = -999;
    status->usbhdfsd_rc = -999;
    status->mcinit_rc = -999;
    status->mcinfo_issue_rc = -999;
    status->mcinfo_sync_rc = -999;
    status->mcinfo_result = -999;
    status->card_type = MC_TYPE_NONE;
    status->free_clusters = -1;
    status->formatted = 0;
    status->filexio_init_rc = -999;
}

int MciRawCardSessionStart(MciRawCardSessionStatus *status)
{
    int mcinfo_result = -999;
    int type = MC_TYPE_NONE;
    int free_clusters = -1;
    int formatted = 0;
    int rc;

    if (status == NULL)
        return -1;
    RawSessionStartTicks = GetTimerSystemTime();
    RawSessionReadyTicks = 0u;
    MciRawCardSessionReset(status);

    /* The caller normally detached the normal mass backend already. Repeat the
     * transition here defensively so no progress callback can ever try to use a
     * stale fileXio client across the IOP reset below. Detach itself is RAM-only. */
    MciDiagLogSetIoAvailable(0);

    /* MagicGate's fake-MCSERV state lives on the EE and survives an IOP reset.
     * Clear it before the real raw personality so mcInit cannot be swallowed. */
    MciSessionResetShim();

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 2,
                      "Entering raw card mode",
                      "Resetting the IOP before loading the legacy page-level PS2SDK memory-card stack.");
    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {;}
    while (!SifIopSync()) {;}
    SifInitRpc(0);

    rc = SifLoadFileInit();
    if (rc < 0)
        return rc;
    SifInitIopHeap();
    sbv_patch_enable_lmb();

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 10,
                      "Loading raw-card I/O services",
                      "Starting iomanX and fileXio before the memory-card and USB drivers.");
    rc = ExecEmbedded(iomanX_irx, size_iomanX_irx);
    status->iomanx_rc = rc;
    if (rc < 0) goto out;
    rc = ExecEmbedded(fileXio_irx, size_fileXio_irx);
    status->filexio_module_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 24,
                      "Loading legacy SIO2/MCMAN",
                      "Starting PS2SDK SIO2 plus the dedicated non-XMC MCMAN build required by raw page RPCs.");
    rc = ExecEmbedded(fmcb_freesio2_irx, size_fmcb_freesio2_irx);
    status->sio2_rc = rc;
    if (rc < 0) goto out;
    rc = ExecEmbedded(fmcb_freepad_irx, size_fmcb_freepad_irx);
    status->pad_rc = rc;
    if (rc < 0) goto out;
    rc = ExecEmbedded(raw_mcman_irx, size_raw_mcman_irx);
    status->mcman_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 38,
                      "Starting legacy page-level MCSERV",
                      "Loading a non-XMC MCSERV so libmc selects MC_TYPE_MC and permits erase/read/write page commands.");
    MciSessionAllowRealMcserv(1);
    rc = ExecEmbedded(raw_mcserv_irx, size_raw_mcserv_irx);
    MciSessionAllowRealMcserv(0);
    status->mcserv_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 50,
                      "Loading USB storage",
                      "Starting USBD and USBHDFSD in the same raw-card IOP personality.");
    rc = ExecEmbedded(usbd_irx, size_usbd_irx);
    status->usbd_rc = rc;
    if (rc < 0) goto out;
    rc = ExecEmbedded(usbhdfsd_irx, size_usbhdfsd_irx);
    status->usbhdfsd_rc = rc;

out:
    MciSessionAllowRealMcserv(0);
    SifExitIopHeap();
    SifLoadFileExit();
    if (rc < 0)
        return rc;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 64,
                      "Binding raw card client",
                      "Binding libmc to the legacy MCSERV and forcing one card-detection pass before any raw page read.");
    status->mcinit_rc = mcInit(MC_TYPE_MC);
    if (status->mcinit_rc < 0)
        return status->mcinit_rc;

    status->mcinfo_issue_rc = mcGetInfo(0, 0, &type, &free_clusters, &formatted);
    /* This first probe proves that a real legacy MCSERV RPC is live. The image
     * engine performs another probe on the actually selected port before the
     * first raw page read, so mc1 gets independent card-state initialization. */
    if (status->mcinfo_issue_rc < 0)
        return status->mcinfo_issue_rc;
    status->mcinfo_sync_rc = mcSync(MC_WAIT, NULL, &mcinfo_result);
    status->mcinfo_result = mcinfo_result;
    status->card_type = type;
    status->free_clusters = free_clusters;
    status->formatted = formatted;
    if (status->mcinfo_sync_rc < 0)
        return status->mcinfo_sync_rc;

    status->filexio_init_rc = fileXioInit();
    if (status->filexio_init_rc < 0)
        return status->filexio_init_rc;

    /* USBHDFSD enumeration is asynchronous, but a fixed sleep pays its full
     * latency even for devices that are already ready and still cannot prove a
     * slow device actually mounted. Use the existing bounded readiness probe:
     * one short grace interval, then return as soon as any mass root opens. */
    DelayThread(20000);
    rc = MciUsbWaitForStorage(8u, 50000u);
    MciDiagLogPrintf("RAW", "USB readiness probe rc=%d after fileXio init", rc);
    if (rc < 0)
        return rc;

    MciDiagLogSetIoAvailable(1);
    status->ready = 1;
    RawSessionReadyTicks = GetTimerSystemTime();
    MciDiagLogPrintf(
        "RAW-PERF",
        "session ready setup_ticks=%llu setup_us=%llu",
        (unsigned long long)(RawSessionReadyTicks - RawSessionStartTicks),
        (unsigned long long)TicksToUsec(RawSessionReadyTicks - RawSessionStartTicks));
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100,
                      "Raw card mode ready",
                      "Legacy page-level card RPCs and USB image I/O are active.");
    return 0;
}

void MciRawCardSessionStop(MciRawCardSessionStatus *status)
{
    u64 stop_ticks;
    u64 setup_ticks;
    u64 active_ticks;
    u64 total_ticks;
    int drain_rc;

    /* A NOWAIT raw prefetch owns both an EE receive buffer and the current IOP
     * MCSERV personality until RPC END arrives. Rebooting/tearing down fileXio
     * underneath that ownership would turn a performance optimization into a
     * lifecycle race. Submit early, wait late, but still wait before destruction. */
    drain_rc = MciRawBulkReadDrain();
    MciDiagLogPrintf("RAW-BULK", "pipeline drain before raw teardown rc=%d",
                     drain_rc);
    MciRawBulkReadLogStats("pre-teardown");

    stop_ticks = GetTimerSystemTime();
    setup_ticks = RawSessionReadyTicks >= RawSessionStartTicks
                      ? RawSessionReadyTicks - RawSessionStartTicks
                      : 0u;
    active_ticks = RawSessionReadyTicks != 0u && stop_ticks >= RawSessionReadyTicks
                       ? stop_ticks - RawSessionReadyTicks
                       : 0u;
    total_ticks = RawSessionStartTicks != 0u && stop_ticks >= RawSessionStartTicks
                      ? stop_ticks - RawSessionStartTicks
                      : 0u;
    MciDiagLogPrintf(
        "RAW-PERF",
        "session stop setup_ticks=%llu setup_us=%llu active_ticks=%llu active_us=%llu total_ticks=%llu total_us=%llu",
        (unsigned long long)setup_ticks,
        (unsigned long long)TicksToUsec(setup_ticks),
        (unsigned long long)active_ticks,
        (unsigned long long)TicksToUsec(active_ticks),
        (unsigned long long)total_ticks,
        (unsigned long long)TicksToUsec(total_ticks));

    /* Detach before fileXioExit. This only changes EE logger state and queues a
     * marker, so it is safe even if the raw operation already damaged the RPC. */
    MciDiagLogSetIoAvailable(0);
    if (status != NULL && status->ready)
        fileXioExit();
    if (status != NULL)
        status->ready = 0;
    RawSessionStartTicks = 0u;
    RawSessionReadyTicks = 0u;
    MciSessionAllowRealMcserv(0);
    MciSessionResetShim();
}
