/* SPDX-License-Identifier: MIT */
/*
 * PS2 Memory Card Inspector
 * -------------------------
 * Briscoe keeps ordinary memory-card I/O on the Sony ROM X stack that passed
 * real-hardware validation and runs MagicGate/KELF work in a temporary,
 * isolated IOP personality.
 *
 * v0.2.0 uses the PS2SDK 2.0 security stack: SECRMAN 1.4, matching SECRSIF and
 * the matching PS2SDK 2.0 SIO2/PAD/MCMAN generation. The EE bridge translates
 * logical libmc ports 0/1 to physical SIO2 memory-card channels 2/3 only at the
 * SECR RPC boundary. The RAM-only probe never writes the bound KELF to a card,
 * and the normal ROM X stack is rebuilt after every security-session attempt.
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

#define APP_VERSION "0.2.0"
#define APP_CODENAME "Briscoe"
#define SLOT_COUNT 2
#define VIEW_CARD 0
#define VIEW_MAGICGATE 1
#define VIEW_FMCB 2
#define VIEW_COUNT 3

#define MG_PROFILE_NAME "PS2SDK 2.0 SECRMAN 1.4"
#define MG_STACK_SHORT "PS2SDK2/SECR1.4"
#define MG_SIO2_LABEL "MG/PS2SDK2 SIO2MAN"
#define MG_PAD_LABEL "MG/PS2SDK2 PADMAN"
#define MG_MCMAN_LABEL "MG/PS2SDK2 MCMAN"
#define MG_MCSERV_LABEL "MG/PS2SDK2 MCSERV"

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
 * The normal application personality deliberately uses the same Sony ROM
 * XSIO2MAN/XPADMAN/XMCMAN/XMCSERV arrangement that passed hardware testing.
 * Temporary MagicGate modules never replace this stack permanently.
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

    scr_printf("[NORMAL 5/6] Binding libmc...\n");
    rc = mcInit(MC_TYPE_XMC);
    if (rc < 0)
        return rc;

    scr_printf("[NORMAL 6/6] Initializing controller...\n");
    rc = padInit(0);
    if (rc == 0)
        return -1;
    if (padPortOpen(0, 0, PadBuffer) == 0) {
        padEnd();
        return -2;
    }
    PadActive = 1;
    return 0;
}

static void ShutdownNormalClients(void)
{
    FmcbShutdownMassBackend(&FmcbMassStatus);
    if (PadActive) {
        padPortClose(0, 0);
        padEnd();
        PadActive = 0;
    }
}

/* Generate a minimal IOPRP containing SECRMAN 1.4 in EE RAM. */
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
 * Isolated MagicGate security personality.
 *
 * SECRMAN's GET_KBIT card_encrypt path depends on MCMAN registering its
 * mcCommand and device-ID handlers, so the temporary session uses a matched
 * PS2SDK 2.0 SIO2/PAD/MCMAN generation alongside SECRMAN 1.4 and SECRSIF.
 *
 * Temporary MCSERV is presented to the wrapper but intentionally not started:
 * real hardware showed that doing so can wedge the following LOADFILE RPC.
 * MCMAN remains resident, which is sufficient for the CardAuth path.
 */
static int InitMagicGateSession(MagicGateReport *report)
{
    int rc;
    int mg_rc;

    report->stage = MG_STAGE_SESSION_SETUP;
    rc = RebootIopWithSecrman();
    report->session_setup_rc = rc;
    if (rc < 0)
        return rc;

    rc = SifLoadFileInit();
    if (rc < 0) {
        report->session_setup_rc = rc;
        return rc;
    }
    SifInitIopHeap();
    sbv_patch_enable_lmb();

    rc = LoadEmbeddedModule(fmcb_freesio2_irx, size_fmcb_freesio2_irx,
                            MG_SIO2_LABEL);
    if (rc < 0) goto out;
    rc = LoadEmbeddedModule(fmcb_freepad_irx, size_fmcb_freepad_irx,
                            MG_PAD_LABEL);
    if (rc < 0) goto out;
    rc = LoadEmbeddedModule(fmcb_mcman_irx, size_fmcb_mcman_irx,
                            MG_MCMAN_LABEL);
    if (rc < 0) goto out;
    rc = LoadEmbeddedModule(fmcb_mcserv_irx, size_fmcb_mcserv_irx,
                            MG_MCSERV_LABEL);
    if (rc < 0) goto out;

    mg_rc = MagicGateLoadIopModules(&MgIopStatus);
    if (mg_rc < 0) {
        rc = mg_rc;
        goto out;
    }

    rc = 0;

out:
    SifExitIopHeap();
    SifLoadFileExit();
    report->session_setup_rc = rc;
    if (rc < 0)
        return rc;

    rc = mcInit(MC_TYPE_XMC);
    report->session_mcinit_rc = rc;
    return rc;
}

static int RestoreNormalEnvironment(void)
{
    int rc;
    int fmcb_rc;

    scr_printf("[RESTORE] Returning to normal ROM X stack...\n");
    rc = InitNormalCardStack();
    if (rc < 0)
        return rc;

    fmcb_rc = FmcbInitMassBackend(&FmcbMassStatus);
    if (fmcb_rc < 0)
        scr_printf("[RESTORE] mass: unavailable: %d (continuing)\n", fmcb_rc);

    CardInspect(0, &Reports[0]);
    CardInspect(1, &Reports[1]);
    return 0;
}

static int RunMagicGateSession(int target_port)
{
    MagicGateKelfBuffer kelf;
    MagicGateReport *report = &MgReports[target_port];
    int rc;
    int restore_rc;

    MagicGateResetKelfBuffer(&kelf);
    rc = MagicGatePrepareKelf(target_port, &kelf, report);
    if (rc < 0)
        return rc;

    scr_clear();
    scr_printf("PS2 Memory Card Inspector - %s MagicGate session\n\n",
               APP_CODENAME);
    scr_printf("KELF prepared from %s (%d bytes)\n",
               kelf.source_path, kelf.size);
    scr_printf("Switching to isolated %s stack...\n", MG_PROFILE_NAME);

    ShutdownNormalClients();

    rc = InitMagicGateSession(report);
    if (rc < 0) {
        report->result = MG_RESULT_SESSION_SETUP_FAILED;
        scr_printf("[MG] Session setup failed: %d\n", rc);
    } else {
        scr_printf("[MG] Session ready. Checking target card...\n");
        MagicGateProbePrepared(target_port, &kelf, report);
    }

    MagicGateReleaseKelf(&kelf);

    restore_rc = RestoreNormalEnvironment();
    report->restore_rc = restore_rc;
    if (restore_rc < 0) {
        scr_printf("\nFATAL: normal card stack restore failed: %d\n", restore_rc);
        scr_printf("Power-cycle or reset the console.\n");
        SleepThread();
    }

    return (report->result == MG_RESULT_PASS) ? 0 : -1;
}

static void InspectAndInvalidateMagicGate(int port)
{
    CardInspect(port, &Reports[port]);
    MagicGateResetReport(&MgReports[port], port);
}

static void RenderHeader(int selected, int view)
{
    const char *page = "CARD";

    if (view == VIEW_MAGICGATE)
        page = "MAGICGATE";
    else if (view == VIEW_FMCB)
        page = "FMCB PREFLIGHT";

    scr_printf("PS2 Memory Card Inspector v%s - %s\n", APP_VERSION, APP_CODENAME);
    scr_printf("PS2DEV 2.0 EE / ROM normal + %s / %s\n\n",
               MG_STACK_SHORT, page);
    scr_printf("< LEFT/RIGHT > slot   X filesystem   SQUARE MagicGate\n");
    scr_printf("CIRCLE FMCB scan   R1 next page   START test both   SELECT exit\n\n");
    scr_printf("Selected: SLOT %d (mc%d:)\n", selected + 1, selected);
}

static void RenderCardView(int selected, int confirm_format, int last_format_rc)
{
    CardReport *r = &Reports[selected];
    MagicGateReport *mg = &MgReports[selected];

    RenderHeader(selected, VIEW_CARD);
    scr_printf("Filesystem health: %s\n", CardHealthText(r->health));
    scr_printf("MagicGate/KELF:    %s\n", MagicGateResultText(mg->result));
    scr_printf("FMCB package:      %s\n\n",
               FmcbPackageStatusText(FmcbReports[selected].status));

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
}

static void RenderMagicGateView(int selected)
{
    MagicGateReport *mg = &MgReports[selected];

    RenderHeader(selected, VIEW_MAGICGATE);
    scr_printf("Normal filesystem: %s\n", CardHealthText(Reports[selected].health));
    scr_printf("MagicGate result:  %s\n", MagicGateResultText(mg->result));
    scr_printf("MG stage:          %s\n\n", MagicGateStageText(mg->stage));

    if (mg->source_path[0] != '\0') {
        if (mg->source_port >= 0 && mg->source_port < 2)
            scr_printf("KELF: mc%d:%s (%d bytes)\n",
                       mg->source_port, mg->source_path, mg->source_size);
        else
            scr_printf("KELF: %s (%d bytes)\n",
                       mg->source_path, mg->source_size);
    } else {
        scr_printf("KELF: none found\n");
    }

    scr_printf("Session setup rc:  %d\n", mg->session_setup_rc);
    scr_printf("Session mcInit rc: %d\n", mg->session_mcinit_rc);
    scr_printf("Session mcGetInfo: %d  type=%d fmt=%d free=%d\n",
               mg->session_mcinfo_rc, mg->session_type,
               mg->session_formatted, mg->session_free_clusters);
    scr_printf("Normal restore rc: %d\n\n", mg->restore_rc);

    scr_printf("SECRSIF load/start: %d / %d\n",
               MgIopStatus.secrsif_load_rc, MgIopStatus.secrsif_start_rc);
    scr_printf("SECR RPC rc: %d\n", mg->rpc_rc);
    scr_printf("Download header: %d   reply size: %d\n",
               mg->header_rc, mg->header_reply_size);
    scr_printf("BIT blocks: %d   encrypted: %d   completed: %d\n",
               mg->block_count, mg->encrypted_blocks, mg->blocks_completed);
    scr_printf("Failed block: %d   Kbit: %d   Kc: %d\n",
               mg->failed_block, mg->kbit_rc, mg->kc_rc);
    if (mg->icvps2_required)
        scr_printf("ICVPS2: %d (required)\n", mg->icvps2_rc);
    else
        scr_printf("ICVPS2: N/A\n");

    scr_printf("\nProbe is RAM-only; normal card stack is restored afterwards.\n");
}

static void RenderFmcbView(int selected)
{
    FmcbPackageReport *report = &FmcbReports[selected];
    int i;
    int shown_missing = 0;

    RenderHeader(selected, VIEW_FMCB);
    scr_printf("Filesystem:   %s\n", CardHealthText(Reports[selected].health));
    scr_printf("MagicGate:    %s\n", MagicGateResultText(MgReports[selected].result));
    scr_printf("mass backend: %s\n\n", FmcbMassStatus.available ? "AVAILABLE" : "UNAVAILABLE");

    scr_printf("Package: %s\n", FmcbPackageStatusText(report->status));
    scr_printf("Source:  %s\n",
               report->source_root[0] ? report->source_root : "mass:/FMCB (not resolved)");
    scr_printf("Source probe rc: %d\n", report->source_probe_rc);

    if (report->status != FMCB_PACKAGE_NOT_SCANNED &&
        report->status != FMCB_PACKAGE_SOURCE_UNAVAILABLE &&
        report->status != FMCB_PACKAGE_NOT_FOUND) {
        scr_printf("ROMVER region: %c   FMCB region: %c\n",
                   report->plan.romver_region ? report->plan.romver_region : '?',
                   report->plan.region_letter ? report->plan.region_letter : '?');
        scr_printf("Target system: %s\n", report->plan.destination_system);
        scr_printf("Required: %d/%d   missing: %d   optional: %d/%d\n",
                   report->found_required, report->plan.required_files,
                   report->missing_required, report->found_optional,
                   report->plan.optional_files);
        scr_printf("Found payload bytes: %u\n", report->total_found_bytes);
        scr_printf("KELFs requiring bind: %d\n\n", report->plan.kelf_files);

        if (report->missing_required > 0) {
            scr_printf("Missing required files:\n");
            for (i = 0; i < report->entry_count && shown_missing < 4; i++) {
                FmcbPackageFileStatus *file = &report->files[i];
                if (!file->found && (file->flags & FMCB_FILE_REQUIRED)) {
                    scr_printf("  - %s (rc=%d)\n", file->relative_path, file->stat_rc);
                    shown_missing++;
                }
            }
            if (report->missing_required > shown_missing)
                scr_printf("  ...and %d more\n", report->missing_required - shown_missing);
        }
    }

    scr_printf("\nCIRCLE: rescan user package\n");
    scr_printf("INSTALL: NOT ENABLED IN 0.2.0 (preflight is read-only)\n");
}

static void Render(int selected, int view, int confirm_format, int last_format_rc)
{
    scr_clear();
    if (view == VIEW_MAGICGATE)
        RenderMagicGateView(selected);
    else if (view == VIEW_FMCB)
        RenderFmcbView(selected);
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
    int view = VIEW_CARD;
    int confirm_format = 0;
    int last_format_rc = -999;
    int init_rc;
    int fmcb_rc;
    int dirty = 1;
    u32 held;
    u32 pressed;

    (void)argc;
    (void)argv;

    init_scr();
    scr_clear();
    scr_printf("PS2 Memory Card Inspector v%s - %s\n", APP_VERSION, APP_CODENAME);
    scr_printf("Security backend: %s\n", MG_PROFILE_NAME);
    scr_printf("Initializing hardware-validated normal ROM X stack...\n");

    init_rc = InitNormalCardStack();
    if (init_rc < 0) {
        scr_printf("\nInitialization failed: %d\n", init_rc);
        SleepThread();
    }

    scr_printf("\nInitialization complete. Inspecting slots...\n");
    InspectAndInvalidateMagicGate(0);
    InspectAndInvalidateMagicGate(1);
    FmcbResetPackageReport(&FmcbReports[0], 0);
    FmcbResetPackageReport(&FmcbReports[1], 1);

    scr_printf("\n[FMCB] Initializing optional mass: source...\n");
    fmcb_rc = FmcbInitMassBackend(&FmcbMassStatus);
    if (fmcb_rc < 0)
        scr_printf("[FMCB] mass: unavailable: %d (Inspector continues)\n", fmcb_rc);

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
                FmcbResetPackageReport(&FmcbReports[selected], selected);
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
                view = (view + 1) % VIEW_COUNT;
                dirty = 1;
            }

            if (pressed & PAD_CROSS) {
                InspectAndInvalidateMagicGate(selected);
                view = VIEW_CARD;
                dirty = 1;
            }

            if (pressed & PAD_START) {
                InspectAndInvalidateMagicGate(0);
                InspectAndInvalidateMagicGate(1);
                view = VIEW_CARD;
                dirty = 1;
            }

            if (pressed & PAD_SQUARE) {
                if (Reports[selected].type != MC_TYPE_PS2) {
                    MagicGateResetReport(&MgReports[selected], selected);
                    MgReports[selected].result = MG_RESULT_TARGET_NOT_PS2;
                } else {
                    RunMagicGateSession(selected);
                }
                view = VIEW_MAGICGATE;
                dirty = 1;
            }

            if (pressed & PAD_CIRCLE) {
                FmcbProbeMassPackage(selected, &FmcbMassStatus,
                                     &FmcbReports[selected]);
                view = VIEW_FMCB;
                dirty = 1;
            }

            if ((pressed & PAD_TRIANGLE) && Reports[selected].format_allowed) {
                confirm_format = 1;
                view = VIEW_CARD;
                dirty = 1;
            }
        }

        DelayThread(16000);
    }

    ShutdownNormalClients();
    SifExitRpc();
    return 0;
}
