/* SPDX-License-Identifier: MIT */
/*
 * Page-level memory-card personality for Drebin Card Tools.
 *
 * Sony ROM XMCSERV intentionally does not expose libmc's legacy raw-page RPCs.
 * Imaging therefore runs in a short-lived PS2SDK 2.0 MCMAN/MCSERV personality,
 * with USBHDFSD loaded in the same IOP so raw pages can stream directly to or
 * from standard host image files. app_main.c restores the normal Sony ROM X
 * environment immediately after the operation.
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
#include <string.h>

#include "card_raw_session.h"
#include "magicgate_session.h"
#include "progress.h"

extern unsigned char fmcb_freesio2_irx[];
extern unsigned int size_fmcb_freesio2_irx;
extern unsigned char fmcb_freepad_irx[];
extern unsigned int size_fmcb_freepad_irx;
extern unsigned char fmcb_mcman_irx[];
extern unsigned int size_fmcb_mcman_irx;
extern unsigned char fmcb_mcserv_irx[];
extern unsigned int size_fmcb_mcserv_irx;
extern unsigned char iomanX_irx[];
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];
extern unsigned int size_fileXio_irx;
extern unsigned char usbd_irx[];
extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[];
extern unsigned int size_usbhdfsd_irx;

static int ExecEmbedded(const unsigned char *data, unsigned int size)
{
    int start_rc = -999;
    return SifExecModuleBuffer((void *)data, size, 0, NULL, &start_rc);
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
    status->filexio_init_rc = -999;
}

int MciRawCardSessionStart(MciRawCardSessionStatus *status)
{
    int rc;

    if (status == NULL)
        return -1;
    MciRawCardSessionReset(status);

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 2,
                      "Entering raw card mode",
                      "Resetting the IOP before loading the page-level PS2SDK memory-card stack.");
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
                      "Loading PS2SDK SIO2/MCMAN",
                      "Starting the matched PS2SDK 2.0 transport and memory-card manager.");
    rc = ExecEmbedded(fmcb_freesio2_irx, size_fmcb_freesio2_irx);
    status->sio2_rc = rc;
    if (rc < 0) goto out;
    rc = ExecEmbedded(fmcb_freepad_irx, size_fmcb_freepad_irx);
    status->pad_rc = rc;
    if (rc < 0) goto out;
    rc = ExecEmbedded(fmcb_mcman_irx, size_fmcb_mcman_irx);
    status->mcman_rc = rc;
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 38,
                      "Starting page-level MCSERV",
                      "Temporarily enabling the real PS2SDK MCSERV so libmc raw-page RPCs are available.");
    MciSessionAllowRealMcserv(1);
    rc = ExecEmbedded(fmcb_mcserv_irx, size_fmcb_mcserv_irx);
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

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 66,
                      "Binding raw card and USB clients",
                      "Connecting libmc in legacy page-RPC mode and fileXio to the freshly loaded services.");
    status->mcinit_rc = mcInit(MC_TYPE_MC);
    if (status->mcinit_rc < 0)
        return status->mcinit_rc;
    status->filexio_init_rc = fileXioInit();
    if (status->filexio_init_rc < 0)
        return status->filexio_init_rc;

    DelayThread(350000);
    status->ready = 1;
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100,
                      "Raw card mode ready",
                      "Page-level card RPCs and USB image I/O are active.");
    return 0;
}

void MciRawCardSessionStop(MciRawCardSessionStatus *status)
{
    if (status != NULL && status->ready)
        fileXioExit();
    if (status != NULL)
        status->ready = 0;
    MciSessionAllowRealMcserv(0);
}
