/*
 * PS2 Memory Card Inspector
 * -------------------------
 * Briscoe dev12 keeps the hardware-validated ordinary memory-card stack as the
 * permanent application personality. MagicGate/KELF work runs in an isolated
 * IOP session using the same PS2SDK v1-era card stack expected by the classic
 * FMCB SECRMAN 1.3 modules, then the normal ROM X stack is restored.
 *
 * The temporary dev12 SECRMAN is source-built from the pinned FMCB 1.3 code and
 * instruments only failed GET_KBIT calls. Successful behavior is unchanged.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopcontrol_special.h>
#include <iopheap.h>
#include <ioprpgen.h>
#include <loadfile.h>
#include <delaythread.h>
#include <libmc.h>
#include <libpad.h>
#include <debug.h>
#include <sbv_patches.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card.h"
#include "magicgate.h"
#include "fmcb_install.h"

#define APP_VERSION "0.2.0-dev12"
#define APP_CODENAME "Briscoe"
#define SLOT_COUNT 2
#define VIEW_CARD 0
#define VIEW_MAGICGATE 1
#define VIEW_FMCB 2
#define VIEW_COUNT 3

extern unsigned char secrman_irx[];
extern unsigned int size_secrman_irx;
extern unsigned char fmcb_freesio2_irx[];
extern unsigned int size_fmcb_freesio2_irx;
extern unsigned char fmcb_freepad_irx[];
extern unsigned int size_fmcb_freepad_irx;
extern unsigned char fmcb_mcman_irx[];
extern unsigned int size_fmcb_mcman_irx;
extern unsigned char fmcb_mcserv_irx[];
extern unsigned int size_fmcb_mcserv_irx;

static unsigned char PadBuffer[256] __attribute__((aligned(64)));
static CardReport Reports[SLOT_COUNT];
static MagicGateReport MgReports[SLOT_COUNT];
static MagicGateIopStatus MgIopStatus;
static FmcbMassBackendStatus FmcbMassStatus;
static FmcbPackageReport FmcbReports[SLOT_COUNT];
static int PadActive;

static int LoadRomModule(const char *path, const char *name)
{
    int rc;

    scr_printf("[IOP] Loading %s...\n", name);
    rc = SifLoadModule(path, 0, NULL);
    if (rc < 0)
        scr_printf("[IOP] %s FAILED: rc=%d\n", name, rc);
    else
        scr_printf("[IOP] %s OK: id=%d\n", name, rc);
    return rc;
}

static int LoadEmbeddedModule(void *data, unsigned int size, const char *name)
{
    int start_rc = -999;
    int rc;

    scr_printf("[IOP] Loading %s (%u bytes)...\n", name, size);
    rc = SifExecModuleBuffer(data, size, 0, NULL, &start_rc);
    if (rc < 0)
        scr_printf("[IOP] %s FAILED: rc=%d start=%d\n", name, rc, start_rc);
    else
        scr_printf("[IOP] %s OK: id=%d start=%d\n", name, rc, start_rc);
    return rc;
}

/*
 * The normal application personality is deliberately the same ROM X-module
 * arrangement that passed real-hardware Columbo testing. Experimental SECR
 * code is never allowed to replace it permanently.
 */
static int InitNormalCardStack(void)
{
    int rc;

    scr_printf("[NORMAL 1/6] Resetting IOP...\n");
    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {;}
    while (!SifIopSync()) {;}
    SifInitRpc(0);

    scr_printf("[NORMAL 2/6] Initializing loadfile...\n");
    rc = SifLoadFileInit();
    if (rc < 0)
        return rc;

    scr_printf("[NORMAL 3/6] Loading ROM XSIO2MAN/XPADMAN...\n");
    rc = LoadRomModule("rom0:XSIO2MAN", "XSIO2MAN");
    if (rc < 0) goto out_loadfile;
    rc = LoadRomModule("rom0:XPADMAN", "XPADMAN");
    if (rc < 0) goto out_loadfile;

    scr_printf("[NORMAL 4/6] Loading ROM XMCMAN/XMCSERV...\n");
    rc = LoadRomModule("rom0:XMCMAN", "XMCMAN");
    if (rc < 0) goto out_loadfile;
    rc = LoadRomModule("rom0:XMCSERV", "XMCSERV");
    if (rc < 0) goto out_loadfile;

out_loadfile:
    SifLoadFileExit();
    if (rc < 0)
        return rc;

    scr_printf("[NORMAL 5/6] Initializing libmc...\n");
    rc = mcInit(MC_TYPE_XMC);
    if (rc < 0)
        return rc;

    scr_printf("[NORMAL 6/6] Initializing pad...\n");
    rc = padInit(0);
    if (rc == 0)
        return -1;
    rc = padPortOpen(0, 0, PadBuffer);
    if (rc == 0)
        return -1;

    PadActive = 1;
    return 0;
}

static void ShutdownPadForIopReset(void)
{
    if (PadActive) {
        padPortClose(0, 0);
        padEnd();
        PadActive = 0;
    }
}

static int RestoreNormalCardStack(void)
{
    int rc;

    ShutdownPadForIopReset();
    rc = InitNormalCardStack();
    return rc;
}

static int InitMagicGateCardStack(void)
{
    int rc;

    ShutdownPadForIopReset();

    scr_printf("[MG 1/8] Resetting IOP...\n");
    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {;}
    while (!SifIopSync()) {;}
    SifInitRpc(0);

    scr_printf("[MG 2/8] Initializing loadfile...\n");
    rc = SifLoadFileInit();
    if (rc < 0)
        return rc;

    scr_printf("[MG 3/8] Loading FMCB-v1 FREESIO2/FREEPAD...\n");
    rc = LoadEmbeddedModule(fmcb_freesio2_irx, size_fmcb_freesio2_irx, "FREESIO2-v1");
    if (rc < 0) goto out_loadfile;
    rc = LoadEmbeddedModule(fmcb_freepad_irx, size_fmcb_freepad_irx, "FREEPAD-v1");
    if (rc < 0) goto out_loadfile;

    scr_printf("[MG 4/8] Loading instrumented FMCB SECRMAN 1.3...\n");
    rc = LoadEmbeddedModule(secrman_irx, size_secrman_irx, "SECRMAN-1.3-dev12");
    if (rc < 0) goto out_loadfile;

    scr_printf("[MG 5/8] Loading FMCB-v1 MCMAN...\n");
    rc = LoadEmbeddedModule(fmcb_mcman_irx, size_fmcb_mcman_irx, "MCMAN-v1");
    if (rc < 0) goto out_loadfile;

    scr_printf("[MG 6/8] Loading FMCB-v1 MCSERV compatibility shim...\n");
    rc = LoadEmbeddedModule(fmcb_mcserv_irx, size_fmcb_mcserv_irx, "MCSERV-v1");
    if (rc < 0) goto out_loadfile;

    scr_printf("[MG 7/8] Loading SECRSIF 1.3...\n");
    rc = LoadEmbeddedModule(secrsif_irx, size_secrsif_irx, "SECRSIF-1.3");
    if (rc < 0) goto out_loadfile;

out_loadfile:
    SifLoadFileExit();
    if (rc < 0)
        return rc;

    scr_printf("[MG 8/8] Initializing temporary libmc/pad...\n");
    rc = mcInit(MC_TYPE_MC);
    if (rc < 0)
        return rc;
    rc = padInit(0);
    if (rc == 0)
        return -1;
    rc = padPortOpen(0, 0, PadBuffer);
    if (rc == 0)
        return -1;
    PadActive = 1;

    return 0;
}

static void UpdateAllReports(void)
{
    int i;
    for (i = 0; i < SLOT_COUNT; i++)
        CardInspect(i, 0, &Reports[i]);
}

static void ClearMagicGateReports(void)
{
    int i;
    for (i = 0; i < SLOT_COUNT; i++)
        MagicGateReportReset(&MgReports[i]);
    memset(&MgIopStatus, 0, sizeof(MgIopStatus));
}

static void ClearFmcbReports(void)
{
    int i;
    for (i = 0; i < SLOT_COUNT; i++)
        FmcbPackageReportReset(&FmcbReports[i]);
}

static void RunMagicGateProbe(int slot)
{
    int restore_rc;

    ClearMagicGateReports();
    MgIopStatus.setup_rc = InitMagicGateCardStack();
    if (MgIopStatus.setup_rc >= 0) {
        MagicGateProbeSlot(slot, 0, &MgReports[slot]);
    }

    restore_rc = RestoreNormalCardStack();
    MgIopStatus.restore_rc = restore_rc;
    if (restore_rc >= 0)
        UpdateAllReports();
}

static void RunMagicGateProbeBoth(void)
{
    int i;
    int restore_rc;

    ClearMagicGateReports();
    MgIopStatus.setup_rc = InitMagicGateCardStack();
    if (MgIopStatus.setup_rc >= 0) {
        for (i = 0; i < SLOT_COUNT; i++)
            MagicGateProbeSlot(i, 0, &MgReports[i]);
    }

    restore_rc = RestoreNormalCardStack();
    MgIopStatus.restore_rc = restore_rc;
    if (restore_rc >= 0)
        UpdateAllReports();
}

static int ReadPadButtons(unsigned int *buttons)
{
    struct padButtonStatus pad;
    int state;

    state = padGetState(0, 0);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
        return -1;
    if (padRead(0, 0, &pad) == 0)
        return -1;

    *buttons = 0xFFFF ^ pad.btns;
    return 0;
}

static void DrawHeader(int selected, int view)
{
    scr_clear();
    scr_printf("PS2 Memory Card Inspector v%s - %s\n", APP_VERSION, APP_CODENAME);
    scr_printf("PS2DEV 2.0 EE / ROM normal + FMCB-v1 MG / MAGICGATE\n\n");
    scr_printf("< LEFT/RIGHT > slot   X filesystem   SQUARE MagicGate\n");
    scr_printf("CIRCLE FMCB scan   R1 next page   START test both   SELECT exit\n\n");
    scr_printf("Selected: SLOT %d (mc%d:)\n", selected + 1, selected);
    (void)view;
}

static void DrawCardView(int selected)
{
    const CardReport *r = &Reports[selected];
    DrawHeader(selected, VIEW_CARD);
    CardDrawReport(r);
}

static void DrawMagicGateView(int selected)
{
    DrawHeader(selected, VIEW_MAGICGATE);
    MagicGateDrawReport(&MgReports[selected], &MgIopStatus);
}

static void DrawFmcbView(int selected)
{
    DrawHeader(selected, VIEW_FMCB);
    FmcbPackageDrawReport(&FmcbReports[selected], &FmcbMassStatus);
}

int main(int argc, char *argv[])
{
    int selected = 0;
    int view = VIEW_CARD;
    int running = 1;
    unsigned int old_buttons = 0;
    unsigned int buttons = 0;
    unsigned int pressed;
    int rc;

    (void)argc;
    (void)argv;

    init_scr();
    scr_printf("PS2 Memory Card Inspector v%s - %s\n", APP_VERSION, APP_CODENAME);
    scr_printf("Starting hardware-safe normal card stack...\n");

    rc = InitNormalCardStack();
    if (rc < 0) {
        scr_printf("Fatal: normal card stack init failed rc=%d\n", rc);
        SleepThread();
        return 1;
    }

    FmcbMassStatus.init_rc = FmcbMassBackendInit();
    UpdateAllReports();
    ClearMagicGateReports();
    ClearFmcbReports();

    while (running) {
        if (view == VIEW_CARD)
            DrawCardView(selected);
        else if (view == VIEW_MAGICGATE)
            DrawMagicGateView(selected);
        else
            DrawFmcbView(selected);

        if (ReadPadButtons(&buttons) >= 0) {
            pressed = buttons & ~old_buttons;
            old_buttons = buttons;

            if (pressed & PAD_LEFT) {
                selected = (selected + SLOT_COUNT - 1) % SLOT_COUNT;
            } else if (pressed & PAD_RIGHT) {
                selected = (selected + 1) % SLOT_COUNT;
            } else if (pressed & PAD_R1) {
                view = (view + 1) % VIEW_COUNT;
            } else if (pressed & PAD_CROSS) {
                CardRunIntegrityTest(selected, 0, &Reports[selected]);
                view = VIEW_CARD;
            } else if (pressed & PAD_SQUARE) {
                RunMagicGateProbe(selected);
                view = VIEW_MAGICGATE;
            } else if (pressed & PAD_CIRCLE) {
                FmcbPackageScan(selected, &FmcbReports[selected], &FmcbMassStatus);
                view = VIEW_FMCB;
            } else if (pressed & PAD_START) {
                RunMagicGateProbeBoth();
                view = VIEW_MAGICGATE;
            } else if (pressed & PAD_SELECT) {
                running = 0;
            }
        }

        DelayThread(16000);
    }

    ShutdownPadForIopReset();
    return 0;
}
