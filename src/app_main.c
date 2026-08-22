/* SPDX-License-Identifier: MIT */
/*
 * PS2 Memory Card Inspector 0.4.0 development controller.
 *
 * The 0.3 line established the tested navigation and diagnostic boundaries.
 * 0.4 retains them while adding a Settings page, selectable filesystem stress
 * depth and runtime display modes adapted from fhdb-bootstrap-manager 0.4.3.
 *
 * Navigation remains orthogonal to I/O: UP/DOWN selects a card on diagnostic
 * pages, L1/R1 selects a page, CROSS runs only that page's diagnostic, and
 * L2+CROSS runs the complete read-only diagnostic sequence. Settings consumes
 * UP/DOWN/LEFT/RIGHT locally and never starts card I/O merely by being edited.
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "card.h"
#include "magicgate.h"
#include "fmcb_install.h"
#include "gui.h"
#include "progress.h"
#include "settings.h"

#define SLOT_COUNT 2
#define SETTINGS_ROW_COUNT 4

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
static MciSettings Settings;
static int PadActive;

static int LoadRomModule(const char *path)
{
    return SifLoadModule(path, 0, NULL);
}

static int LoadEmbeddedModule(void *data, unsigned int size)
{
    int start_rc = -999;
    return SifExecModuleBuffer(data, size, 0, NULL, &start_rc);
}

static int InitNormalCardStack(void)
{
    int rc;

    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {;}
    while (!SifIopSync()) {;}
    SifInitRpc(0);

    rc = SifLoadFileInit();
    if (rc < 0)
        return rc;

    rc = LoadRomModule("rom0:XSIO2MAN");
    if (rc < 0) goto out_loadfile;
    rc = LoadRomModule("rom0:XPADMAN");
    if (rc < 0) goto out_loadfile;
    rc = LoadRomModule("rom0:XMCMAN");
    if (rc < 0) goto out_loadfile;
    rc = LoadRomModule("rom0:XMCSERV");

out_loadfile:
    SifLoadFileExit();
    if (rc < 0)
        return rc;

    rc = mcInit(MC_TYPE_XMC);
    if (rc < 0)
        return rc;

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

static int RebootIopWithSecrman(void)
{
    struct ioprpgen_ctx ctx;
    struct ioprpgen_memwrite_ctx memctx;
    struct ioprpgen_entry entries[2];
    int image_size;
    int written;
    void *image;

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 29,
                      "Building the isolated SECRMAN IOP image",
                      "Generating a minimal in-memory IOPRP containing the instrumented PS2SDK 2.0 SECRMAN 1.4 module.");
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

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 31,
                      "Rebooting the IOP into the security personality",
                      "The normal Sony ROM X clients are stopped; SECRMAN 1.4 is becoming the resident security backend.");
    SifInitRpc(0);
    if (!SifIopRebootBuffer(image, image_size)) {
        free(image);
        return -3003;
    }
    while (!SifIopSync()) {;}
    free(image);
    SifInitRpc(0);

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 33,
                      "Security IOP reboot complete",
                      "The IOP is synchronized. Loading the matching SIO2, PAD and MCMAN generation next.");
    return 0;
}

static int InitMagicGateSession(MagicGateReport *report)
{
    int rc;
    int mg_rc;

    report->stage = MG_STAGE_SESSION_SETUP;
    rc = RebootIopWithSecrman();
    report->session_setup_rc = rc;
    if (rc < 0)
        return rc;

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 34,
                      "Preparing module loading in the security IOP",
                      "Initializing LOADFILE, the IOP heap and the load-module-buffer patch required for embedded IRX modules.");
    rc = SifLoadFileInit();
    if (rc < 0) {
        report->session_setup_rc = rc;
        return rc;
    }
    SifInitIopHeap();
    sbv_patch_enable_lmb();

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 36,
                      "Loading PS2SDK 2.0 SIO2MAN",
                      "Starting the matching SIO2 transport used by MCMAN and SECRMAN CardAuth callbacks.");
    rc = LoadEmbeddedModule(fmcb_freesio2_irx, size_fmcb_freesio2_irx);
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 38,
                      "Loading PS2SDK 2.0 PADMAN",
                      "Keeping the isolated module generation internally consistent while the normal controller client is stopped.");
    rc = LoadEmbeddedModule(fmcb_freepad_irx, size_fmcb_freepad_irx);
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 40,
                      "Loading PS2SDK 2.0 MCMAN",
                      "Registering the memory-card side used by SECRMAN for direct CardAuth command callbacks.");
    rc = LoadEmbeddedModule(fmcb_mcman_irx, size_fmcb_mcman_irx);
    if (rc < 0) goto out;

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 41,
                      "Applying the isolated MCSERV policy",
                      "The embedded MCSERV load is intentionally intercepted; MCMAN stays resident without starting the EE file-service server.");
    rc = LoadEmbeddedModule(fmcb_mcserv_irx, size_fmcb_mcserv_irx);
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

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 45,
                      "Initializing the temporary EE card view",
                      "The temporary MCSERV is absent, so the compatibility shim supplies only the sanity-query view needed by the RAM-only probe.");
    rc = mcInit(MC_TYPE_XMC);
    report->session_mcinit_rc = rc;
    return rc;
}

static int RestoreNormalEnvironment(void)
{
    int rc;
    int fmcb_rc;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 5,
                      "Rebuilding the Sony ROM X card stack",
                      "Resetting the IOP and restoring XSIO2MAN, XPADMAN, XMCMAN and XMCSERV for ordinary filesystem I/O.");
    rc = InitNormalCardStack();
    if (rc < 0)
        return rc;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 18,
                      "Normal memory-card services are back",
                      "Real libmc and controller clients are active again. Reconnecting the optional USB package backend.");
    fmcb_rc = FmcbInitMassBackend(&FmcbMassStatus);
    (void)fmcb_rc;

    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 92,
                      "Restoring the dashboard without implicit card tests",
                      "The normal card stack is ready. Existing filesystem results are preserved; no integrity test is run unless the user explicitly requests it.");
    MciProgressUpdate(MCI_PROGRESS_ENVIRONMENT, 100,
                      "Normal environment restored",
                      "Sony ROM X card services, controller input and optional mass: access have been rebuilt successfully.");
    return 0;
}

static int RunMagicGateSession(int target_port)
{
    MagicGateKelfBuffer kelf;
    MagicGateReport *report = &MgReports[target_port];
    char detail[192];
    int rc;
    int restore_rc;

    MagicGateResetKelfBuffer(&kelf);
    rc = MagicGatePrepareKelf(target_port, &kelf, report);
    if (rc < 0)
        return rc;

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 27,
                      "Closing normal clients before the security reboot",
                      "Closing controller and mass: clients cleanly; the raw KELF remains safely buffered in EE RAM.");
    ShutdownNormalClients();

    rc = InitMagicGateSession(report);
    if (rc < 0)
        report->result = MG_RESULT_SESSION_SETUP_FAILED;
    else
        MagicGateProbePrepared(target_port, &kelf, report);

    snprintf(detail, sizeof(detail),
             "Security result so far: %s. Releasing the RAM KELF and restoring the normal Sony ROM X environment.",
             MagicGateResultText(report->result));
    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 93,
                      "Security transaction finished", detail);
    MagicGateReleaseKelf(&kelf);

    restore_rc = RestoreNormalEnvironment();
    report->restore_rc = restore_rc;
    if (restore_rc < 0) {
        MciGuiRenderFatal("Normal stack restore failed",
                          "The security session ended, but the known-good ROM X card stack could not be reconstructed safely.",
                          restore_rc);
        SleepThread();
    }

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 100,
                      "MagicGate probe and environment restore complete",
                      "The isolated SECRMAN session has ended and the program is returning to the results dashboard.");
    return report->result == MG_RESULT_PASS ? 0 : -1;
}

static void ResetCardReport(int port)
{
    CardReport *report = &Reports[port];

    memset(report, 0, sizeof(*report));
    report->port = port;
    report->info_rc = -999;
    report->root_rc = -999;
    report->rw_rc = -999;
    report->cleanup_rc = 0;
    report->rw_stage = RW_NOT_RUN;
    report->health = CARD_UNKNOWN;
}

static void ResetSlotReports(int port)
{
    ResetCardReport(port);
    MagicGateResetReport(&MgReports[port], port);
    FmcbResetPackageReport(&FmcbReports[port], port);
}

static unsigned int CurrentFsTestBytes(void)
{
    return MciFsTestProfileBytes(Settings.fs_profile);
}

static void RunSelectedPageTest(int target_port, MciGuiPage page)
{
    switch (page) {
        case MCI_GUI_MAGICGATE:
            (void)RunMagicGateSession(target_port);
            break;
        case MCI_GUI_FMCB:
            (void)FmcbProbeMassPackage(target_port, &FmcbMassStatus,
                                       &FmcbReports[target_port]);
            break;
        case MCI_GUI_SETTINGS:
            break;
        case MCI_GUI_CARD:
        default:
            CardInspectSized(target_port, &Reports[target_port],
                             CurrentFsTestBytes());
            break;
    }
}

static void RunSelectedFullScan(int target_port)
{
    char detail[192];

    snprintf(detail, sizeof(detail),
             "Running the complete read-only diagnostic sequence for mc%d using the %s filesystem profile, then MagicGate/CardAuth and FMCB package preflight.",
             target_port, MciFsTestProfileName(Settings.fs_profile));
    MciGuiRenderMessage("Full card scan", detail, NULL, MCI_GUI_TONE_INFO);

    MagicGateResetReport(&MgReports[target_port], target_port);
    FmcbResetPackageReport(&FmcbReports[target_port], target_port);
    CardInspectSized(target_port, &Reports[target_port], CurrentFsTestBytes());

    if (Reports[target_port].type == MC_TYPE_PS2) {
        (void)RunMagicGateSession(target_port);
    } else {
        MagicGateResetReport(&MgReports[target_port], target_port);
        MgReports[target_port].result = MG_RESULT_TARGET_NOT_PS2;
        MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 100,
                          "MagicGate probe skipped",
                          "The selected slot does not currently report a PS2 memory card, so no CardAuth transaction is attempted.");
    }

    (void)FmcbProbeMassPackage(target_port, &FmcbMassStatus,
                               &FmcbReports[target_port]);
}

static u32 ReadPadPressed(u32 *held)
{
    struct padButtonStatus buttons;
    static u32 old_state;
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

static void RenderDashboard(int selected, MciGuiPage page,
                            int settings_row, int last_video_rc,
                            int confirm_format, int last_format_rc)
{
    MciGuiRenderDashboard(selected, page, Reports, MgReports, &MgIopStatus,
                          &FmcbMassStatus, FmcbReports, &Settings,
                          settings_row, last_video_rc,
                          confirm_format, last_format_rc);
}

static void ChangeSetting(int row, int direction)
{
    if (row == 0) {
        int value = (int)Settings.video_mode + direction;
        if (value < 0)
            value = (int)MCI_VIDEO_MODE_COUNT - 1;
        if ((unsigned int)value >= MCI_VIDEO_MODE_COUNT)
            value = 0;
        Settings.video_mode = (MciVideoMode)value;
    } else if (row == 1) {
        int value = (int)Settings.fs_profile + direction;
        if (value < 0)
            value = (int)MCI_FS_TEST_PROFILE_COUNT - 1;
        if (value >= (int)MCI_FS_TEST_PROFILE_COUNT)
            value = 0;
        Settings.fs_profile = (MciFsTestProfile)value;
    } else if (row == 2) {
        Settings.preserve_existing_cnfs ^= 1;
    }
    /* Row 3 is intentionally not toggleable. Full installer verification is
     * a safety invariant, not an attractive checkbox for impatient humans. */
}

int main(int argc, char *argv[])
{
    int selected = 0;
    MciGuiPage page = MCI_GUI_CARD;
    int settings_row = 0;
    int last_video_rc = -999;
    int confirm_format = 0;
    int last_format_rc = -999;
    int init_rc;
    int fmcb_rc;
    int dirty = 1;
    u32 held;
    u32 pressed;

    (void)argc;
    (void)argv;

    MciSettingsDefaults(&Settings);

    init_scr();
    if (MciGuiInit() < 0) {
        scr_clear();
        scr_printf("PS2 Memory Card Inspector 0.4.0-dev1\n\n");
        scr_printf("GS frontend initialization failed.\n");
        SleepThread();
    }

    MciGuiRenderMessage("Starting",
                        "Initializing the hardware-validated Sony ROM X memory-card stack.",
                        NULL, MCI_GUI_TONE_INFO);
    init_rc = InitNormalCardStack();
    if (init_rc < 0) {
        MciGuiRenderFatal("Initialization failed",
                          "The normal memory-card environment could not be initialized.",
                          init_rc);
        SleepThread();
    }

    ResetSlotReports(0);
    ResetSlotReports(1);
    fmcb_rc = FmcbInitMassBackend(&FmcbMassStatus);
    (void)fmcb_rc;

    while (1) {
        if (dirty) {
            RenderDashboard(selected, page, settings_row, last_video_rc,
                            confirm_format, last_format_rc);
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
            if (page == MCI_GUI_SETTINGS) {
                if (pressed & PAD_UP) {
                    settings_row = settings_row == 0
                                       ? SETTINGS_ROW_COUNT - 1
                                       : settings_row - 1;
                    dirty = 1;
                } else if (pressed & PAD_DOWN) {
                    settings_row = (settings_row + 1) % SETTINGS_ROW_COUNT;
                    dirty = 1;
                }
                if (pressed & PAD_LEFT) {
                    ChangeSetting(settings_row, -1);
                    dirty = 1;
                } else if (pressed & PAD_RIGHT) {
                    ChangeSetting(settings_row, 1);
                    dirty = 1;
                }
            } else if (pressed & (PAD_UP | PAD_DOWN)) {
                selected ^= 1;
                last_format_rc = -999;
                dirty = 1;
            }

            if (pressed & PAD_L1) {
                page = page == MCI_GUI_CARD
                           ? (MciGuiPage)(MCI_GUI_PAGE_COUNT - 1)
                           : (MciGuiPage)((unsigned int)page - 1u);
                dirty = 1;
            } else if (pressed & PAD_R1) {
                page = (MciGuiPage)(((unsigned int)page + 1u) % MCI_GUI_PAGE_COUNT);
                dirty = 1;
            }

            if (pressed & PAD_CROSS) {
                if (page == MCI_GUI_SETTINGS) {
                    if (settings_row == 0) {
                        last_video_rc = MciGuiApplyVideoMode(Settings.video_mode);
                        if (last_video_rc < 0)
                            Settings.video_mode = MciGuiCurrentVideoMode();
                    }
                } else if (held & PAD_L2) {
                    RunSelectedFullScan(selected);
                } else {
                    RunSelectedPageTest(selected, page);
                }
                dirty = 1;
            }

            if ((pressed & PAD_TRIANGLE) &&
                page != MCI_GUI_SETTINGS && Reports[selected].format_allowed) {
                confirm_format = 1;
                page = MCI_GUI_CARD;
                dirty = 1;
            }
        }

        DelayThread(16000);
    }

    MciGuiRenderMessage("Exiting",
                        "Closing controller and USB clients.",
                        NULL, MCI_GUI_TONE_INFO);
    ShutdownNormalClients();
    SifExitRpc();
    return 0;
}
