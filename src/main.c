/*
 * PS2 Memory Card Inspector
 * -------------------------
 * v0.2.0 Briscoe separates ordinary filesystem health from MagicGate/KELF
 * capability.  A card can pass one layer and fail the other; the UI never
 * collapses those two facts into a single vague "unsupported card" result.
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

#define APP_VERSION "0.2.0-dev2"
#define APP_CODENAME "Briscoe"
#define SLOT_COUNT 2

extern unsigned char freesio2_irx[];
extern unsigned int size_freesio2_irx;
extern unsigned char freepad_irx[];
extern unsigned int size_freepad_irx;
extern unsigned char mcman_irx[];
extern unsigned int size_mcman_irx;
extern unsigned char mcserv_irx[];
extern unsigned int size_mcserv_irx;
extern unsigned char secrman_irx[];
extern unsigned int size_secrman_irx;

static unsigned char PadBuffer[256] __attribute__((aligned(64)));
static CardReport Reports[SLOT_COUNT];
static MagicGateReport MgReports[SLOT_COUNT];
static MagicGateIopStatus MgIopStatus;

static int LoadEmbeddedModule(const unsigned char *module, unsigned int size,
                              const char *name)
{
    int rc;
    int start_rc = -999;

    scr_printf("[IOP] Loading %s...\n", name);
    rc = SifExecModuleBuffer((void *)module, size, 0, NULL, &start_rc);
    if (rc < 0)
        scr_printf("[IOP] %s FAILED: load=%d start=%d\n", name, rc, start_rc);
    else
        scr_printf("[IOP] %s OK: id=%d start=%d\n", name, rc, start_rc);

    return rc;
}

/*
 * Build the smallest possible IOPRP containing the PS2SDK special SECRMAN.
 * ioprpgen adds RESET/ROMDIR/EXTINFO itself.  Rebooting with this image is
 * important: merely loading a second SECRMAN after the ROM memory-card stack
 * is already initialized leaves the new SECRMAN without XMCMAN callbacks.
 */
static int RebootIopWithSecrman(void)
{
    struct ioprpgen_ctx ctx;
    struct ioprpgen_memwrite_ctx memctx;
    struct ioprpgen_entry entries[2];
    int image_size;
    int written;
    void *image;

    memset(entries, 0, sizeof(entries));
    entries[0].m_name = "SECRMAN";
    entries[0].m_data = secrman_irx;
    entries[0].m_data_size = size_secrman_irx;

    ioprpgen_setup_membuf(&ctx, &memctx, NULL, 0);
    image_size = ioprpgen_write_ioprp(&ctx, entries);
    if (image_size <= 0)
        return -3000;

    image = memalign(64, image_size);
    if (image == NULL)
        return -3001;

    ioprpgen_setup_membuf(&ctx, &memctx, image, image_size);
    written = ioprpgen_write_ioprp(&ctx, entries);
    if (written != image_size) {
        free(image);
        return -3002;
    }

    SifInitRpc(0);
    if (!SifIopRebootBuffer(image, image_size)) {
        free(image);
        return -3003;
    }

    while (!SifIopSync()) {;}
    free(image);
    SifInitRpc(0);
    return 0;
}

/*
 * Initialize one coherent PS2SDK stack:
 *
 *   runtime IOPRP: special SECRMAN
 *   -> freesio2 -> freepad -> XMCMAN -> XMCSERV -> SECRSIF
 *
 * PS2SDK's mcman.irx/mcserv.irx are built as XMCMAN/XMCSERV by default.  By
 * loading XMCMAN after the SECRMAN reboot, its init code registers the card
 * command and device-ID callbacks that SECRMAN needs for real MagicGate I/O.
 */
static int InitIopAndDevices(void)
{
    int rc;
    int mg_rc;

    scr_printf("[IOP 1/9] Building SECRMAN IOPRP and rebooting IOP...\n");
    rc = RebootIopWithSecrman();
    if (rc < 0) {
        scr_printf("[IOP 1/9] SECRMAN reboot FAILED: %d\n", rc);
        return rc;
    }
    scr_printf("[IOP 1/9] IOP rebooted with PS2SDK SECRMAN.\n");

    scr_printf("[IOP 2/9] Initializing loadfile/IOP heap...\n");
    rc = SifLoadFileInit();
    if (rc < 0) {
        scr_printf("[IOP 2/9] SifLoadFileInit FAILED: %d\n", rc);
        return rc;
    }
    SifInitIopHeap();
    sbv_patch_enable_lmb();

    scr_printf("[IOP 3/9] Loading PS2SDK SIO2MAN...\n");
    rc = LoadEmbeddedModule(freesio2_irx, size_freesio2_irx, "freesio2/X SIO2MAN");
    if (rc < 0) return rc;

    scr_printf("[IOP 4/9] Loading PS2SDK PADMAN...\n");
    rc = LoadEmbeddedModule(freepad_irx, size_freepad_irx, "freepad/PADMAN");
    if (rc < 0) return rc;

    scr_printf("[IOP 5/9] Loading PS2SDK XMCMAN...\n");
    rc = LoadEmbeddedModule(mcman_irx, size_mcman_irx, "mcman/XMCMAN");
    if (rc < 0) return rc;

    scr_printf("[IOP 6/9] Loading PS2SDK XMCSERV...\n");
    rc = LoadEmbeddedModule(mcserv_irx, size_mcserv_irx, "mcserv/XMCSERV");
    if (rc < 0) return rc;

    scr_printf("[IOP 7/9] Loading SECRSIF...\n");
    mg_rc = MagicGateLoadIopModules(&MgIopStatus);
    scr_printf("[SECR] secrman: load=%d start=%d\n",
               MgIopStatus.secrman_load_rc, MgIopStatus.secrman_start_rc);
    scr_printf("[SECR] secrsif: load=%d start=%d\n",
               MgIopStatus.secrsif_load_rc, MgIopStatus.secrsif_start_rc);
    if (mg_rc < 0)
        scr_printf("[SECR] WARNING: MagicGate disabled (rc=%d).\n", mg_rc);
    else
        scr_printf("[SECR] MagicGate RPC servers staged.\n");

    SifExitIopHeap();
    SifLoadFileExit();

    scr_printf("[IOP 8/9] Binding libmc to XMCSERV...\n");
    rc = mcInit(MC_TYPE_XMC);
    if (rc < 0) {
        scr_printf("[IOP 8/9] mcInit FAILED: %d\n", rc);
        return rc;
    }
    scr_printf("[IOP 8/9] mcInit OK: %d\n", rc);

    scr_printf("[IOP 9/9] Initializing controller...\n");
    rc = padInit(0);
    if (rc == 0) {
        scr_printf("[IOP 9/9] padInit FAILED.\n");
        return -1;
    }
    if (padPortOpen(0, 0, PadBuffer) == 0) {
        scr_printf("[IOP 9/9] padPortOpen FAILED.\n");
        return -2;
    }
    scr_printf("[IOP 9/9] Controller port open.\n");

    return 0;
}

static void InspectAndInvalidateMagicGate(int port)
{
    CardInspect(port, &Reports[port]);
    MagicGateResetReport(&MgReports[port], port);
}

static void RenderHeader(int selected, int view)
{
    scr_printf("PS2 Memory Card Inspector v%s - %s\n", APP_VERSION, APP_CODENAME);
    scr_printf("PS2DEV 2.0 / coherent XMCMAN + SECR stack\n\n");
    scr_printf("< LEFT/RIGHT > slot   X filesystem   SQUARE MagicGate\n");
    scr_printf("START both filesystems   R1 %s   SELECT exit\n\n",
               view ? "card view" : "MG details");
    scr_printf("Selected: SLOT %d (mc%d:)\n", selected + 1, selected);
}

static void RenderCardView(int selected, int confirm_format, int last_format_rc)
{
    CardReport *r = &Reports[selected];
    MagicGateReport *mg = &MgReports[selected];

    RenderHeader(selected, 0);
    scr_printf("Filesystem health: %s\n", CardHealthText(r->health));
    scr_printf("MagicGate/KELF:    %s\n\n", MagicGateResultText(mg->result));

    scr_printf("mcGetInfo rc: %d (%s)\n", r->info_rc, CardResultText(r->info_rc));
    scr_printf("Reported type: %d (%s)\n", r->type, CardTypeText(r->type));
    scr_printf("Formatted flag: %d\n", r->formatted);
    scr_printf("Free clusters: %d\n", r->free_clusters);
    scr_printf("Root directory rc: %d (%s)\n", r->root_rc, CardResultText(r->root_rc));
    scr_printf("R/W stage: %s\n", CardRwStageText(r->rw_stage));
    scr_printf("4 KiB R/W rc: %d (%s)\n", r->rw_rc, CardResultText(r->rw_rc));
    scr_printf("Cleanup rc: %d (%s)\n\n", r->cleanup_rc, CardResultText(r->cleanup_rc));

    scr_printf("Other slot: mc%d: FS=%s  MG=%s\n\n",
               selected ^ 1,
               CardHealthText(Reports[selected ^ 1].health),
               MagicGateResultText(MgReports[selected ^ 1].result));

    if (r->format_allowed) {
        if (!confirm_format) {
            scr_printf("TRIANGLE: format this card (destructive)\n");
        } else {
            scr_printf("*** FORMAT CONFIRMATION ***\n");
            scr_printf("Formatting erases ALL data on mc%d:.\n", selected);
            scr_printf("Hold L1 + R1, then press TRIANGLE to confirm.\n");
            scr_printf("CIRCLE cancels.\n");
        }
    } else {
        scr_printf("Formatting is locked for this state/type.\n");
    }

    if (last_format_rc != -999)
        scr_printf("Last format rc: %d (%s)\n",
                   last_format_rc, CardResultText(last_format_rc));

    if (r->type == MC_TYPE_PS2)
        scr_printf("SQUARE runs a RAM-only MagicGate/KELF bind probe.\n");
    else
        scr_printf("MagicGate probe requires a detected PS2 card.\n");
}

static void RenderMagicGateView(int selected)
{
    MagicGateReport *mg = &MgReports[selected];

    RenderHeader(selected, 1);
    scr_printf("Filesystem: %s\n", CardHealthText(Reports[selected].health));
    scr_printf("MagicGate:  %s\n", MagicGateResultText(mg->result));
    scr_printf("MG stage:   %s\n\n", MagicGateStageText(mg->stage));

    scr_printf("SECR stack: %s\n", MgIopStatus.available ? "AVAILABLE" : "UNAVAILABLE");
    scr_printf("  secrman reboot: %d / %d\n",
               MgIopStatus.secrman_load_rc, MgIopStatus.secrman_start_rc);
    scr_printf("  secrsif load/start: %d / %d\n",
               MgIopStatus.secrsif_load_rc, MgIopStatus.secrsif_start_rc);
    scr_printf("SECR RPC bind/transport rc: %d\n\n", mg->rpc_rc);

    if (mg->source_port >= 0) {
        scr_printf("KELF source: mc%d:%s\n", mg->source_port, mg->source_path);
        scr_printf("KELF size: %d bytes   source I/O rc: %d\n",
                   mg->source_size, mg->source_io_rc);
    } else {
        scr_printf("KELF source: none found\n");
        scr_printf("Looks for FMCB osdmain.elf on the other slot first.\n");
    }

    scr_printf("\nDownload header: %d   reply size: %d\n",
               mg->header_rc, mg->header_reply_size);
    scr_printf("BIT blocks: %d   encrypted: %d   completed: %d\n",
               mg->block_count, mg->encrypted_blocks, mg->blocks_completed);
    scr_printf("Failed block: %d\n", mg->failed_block);
    scr_printf("Get Kbit: %d\n", mg->kbit_rc);
    scr_printf("Get Kc:   %d\n", mg->kc_rc);
    if (mg->icvps2_required)
        scr_printf("Get ICVPS2: %d (required)\n", mg->icvps2_rc);
    else
        scr_printf("Get ICVPS2: N/A\n");

    scr_printf("\nNo KELF or card data is written by this probe.\n");
    scr_printf("SQUARE: run again   R1: return to card view\n");
}

static void Render(int selected, int view, int confirm_format, int last_format_rc)
{
    scr_clear();
    if (view)
        RenderMagicGateView(selected);
    else
        RenderCardView(selected, confirm_format, last_format_rc);
}

static u32 ReadPadPressed(u32 *held)
{
    struct padButtonStatus buttons;
    static u32 old_state = 0;
    u32 state = 0;
    u32 pressed = 0;
    int pad_state;

    pad_state = padGetState(0, 0);
    if (pad_state != PAD_STATE_STABLE && pad_state != PAD_STATE_FINDCTP1) {
        *held = old_state;
        return 0;
    }

    if (padRead(0, 0, &buttons) != 0) {
        state = 0xFFFFu ^ buttons.btns;
        pressed = state & ~old_state;
        old_state = state;
    }

    *held = state;
    return pressed;
}

int main(int argc, char *argv[])
{
    int selected = 0;
    int view = 0;
    int confirm_format = 0;
    int last_format_rc = -999;
    int init_rc;
    int dirty = 1;
    u32 held;
    u32 pressed;

    (void)argc;
    (void)argv;

    init_scr();
    scr_clear();
    scr_printf("PS2 Memory Card Inspector - Briscoe bring-up\n");
    scr_printf("Initializing coherent memory-card and SECR services...\n");

    init_rc = InitIopAndDevices();
    if (init_rc < 0) {
        scr_printf("\nInitialization failed: %d\n", init_rc);
        scr_printf("System halted.\n");
        SleepThread();
    }

    scr_printf("\nInitialization complete. Inspecting slots...\n");
    InspectAndInvalidateMagicGate(0);
    InspectAndInvalidateMagicGate(1);

    while (1) {
        if (dirty) {
            Render(selected, view, confirm_format, last_format_rc);
            dirty = 0;
        }

        pressed = ReadPadPressed(&held);

        if (pressed & PAD_SELECT)
            break;

        if (confirm_format) {
            if (pressed & PAD_CIRCLE) {
                confirm_format = 0;
                dirty = 1;
            } else if ((pressed & PAD_TRIANGLE) &&
                       (held & PAD_L1) && (held & PAD_R1)) {
                last_format_rc = CardFormat(selected, &Reports[selected]);
                MagicGateResetReport(&MgReports[selected], selected);
                confirm_format = 0;
                dirty = 1;
            }
        } else {
            if (pressed & (PAD_LEFT | PAD_RIGHT)) {
                selected ^= 1;
                last_format_rc = -999;
                dirty = 1;
            }

            if (pressed & PAD_R1) {
                view ^= 1;
                dirty = 1;
            }

            if (pressed & PAD_CROSS) {
                InspectAndInvalidateMagicGate(selected);
                view = 0;
                dirty = 1;
            }

            if (pressed & PAD_START) {
                InspectAndInvalidateMagicGate(0);
                InspectAndInvalidateMagicGate(1);
                view = 0;
                dirty = 1;
            }

            if ((pressed & PAD_SQUARE) && Reports[selected].type == MC_TYPE_PS2) {
                MagicGateProbeCard(selected, &MgReports[selected]);
                view = 1;
                dirty = 1;
            }

            if ((pressed & PAD_TRIANGLE) && Reports[selected].format_allowed) {
                confirm_format = 1;
                view = 0;
                dirty = 1;
            }
        }

        DelayThread(16000);
    }

    padPortClose(0, 0);
    padEnd();
    SifExitRpc();
    return 0;
}
