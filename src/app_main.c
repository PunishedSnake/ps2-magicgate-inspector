/* SPDX-License-Identifier: MIT */
/*
 * PS2 Memory Card Inspector 0.4.0 development controller.
 *
 * 0.4 retains the hardware-tested diagnostic boundaries from 0.3 while adding
 * configurable GS output, deeper filesystem profiles and a guarded normal
 * FreeMcBoot install transaction. Normal card I/O always uses Sony ROM X;
 * KELF binding always runs in the isolated PS2SDK 2.0 SECRMAN 1.4 personality.
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
#include <libsecr.h>
#include <debug.h>
#include <sbv_patches.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "card.h"
#include "magicgate.h"
#include "fmcb_install.h"
#include "fmcb_transaction.h"
#include "fmcb_recovery.h"
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
static FmcbInstallReport InstallReports[SLOT_COUNT];
static FmcbRecoveryStatus RecoveryStatus;
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
                      "Generating an in-memory IOPRP containing the instrumented PS2SDK 2.0 SECRMAN 1.4 module.");
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
                      "Initializing LOADFILE, the IOP heap and load-module-buffer patch for embedded IRX modules.");
    rc = SifLoadFileInit();
    if (rc < 0) {
        report->session_setup_rc = rc;
        return rc;
    }
    SifInitIopHeap();
    sbv_patch_enable_lmb();

    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 36, "Loading PS2SDK 2.0 SIO2MAN",
                      "Starting the matching SIO2 transport used by MCMAN and SECRMAN CardAuth callbacks.");
    rc = LoadEmbeddedModule(fmcb_freesio2_irx, size_fmcb_freesio2_irx);
    if (rc < 0) goto out;
    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 38, "Loading PS2SDK 2.0 PADMAN",
                      "Keeping the isolated module generation internally consistent while the normal controller client is stopped.");
    rc = LoadEmbeddedModule(fmcb_freepad_irx, size_fmcb_freepad_irx);
    if (rc < 0) goto out;
    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 40, "Loading PS2SDK 2.0 MCMAN",
                      "Registering the memory-card side used by SECRMAN for direct CardAuth command callbacks.");
    rc = LoadEmbeddedModule(fmcb_mcman_irx, size_fmcb_mcman_irx);
    if (rc < 0) goto out;
    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 41, "Applying the isolated MCSERV policy",
                      "The embedded MCSERV load is intercepted; MCMAN remains resident without starting the EE file-service server.");
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
                      "The temporary MCSERV is absent, so the compatibility shim supplies only the sanity-query view needed by the security path.");
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
                      "The normal card stack is ready. Existing results are preserved; no integrity test runs unless explicitly requested.");
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
                          "The security session ended, but the ROM X card stack could not be reconstructed safely.",
                          restore_rc);
        SleepThread();
    }
    MciProgressUpdate(MCI_PROGRESS_MAGICGATE, 100,
                      "MagicGate probe and environment restore complete",
                      "The isolated SECRMAN session has ended and the program is returning to the results dashboard.");
    return report->result == MG_RESULT_PASS ? 0 : -1;
}

/* Installer KELF binding uses the same isolated hardware-validated personality
 * as the probe, but calls PS2SDK libsecr's complete SecrDownloadFile path so
 * Kbit/Kc/ICVPS2 are written into the in-RAM KELF before normal-stack restore. */
static int BindKelfForInstaller(int target_port, unsigned char *buffer,
                                unsigned int size, void *userdata)
{
    MagicGateReport scratch;
    void *bound;
    int rc;
    int restore_rc;

    (void)size;
    (void)userdata;
    MagicGateResetReport(&scratch, target_port);
    MciProgressUpdate(MCI_PROGRESS_FMCB, 35,
                      "Entering the KELF binding personality",
                      "The source is already in EE RAM. Normal clients are closing before the isolated SECRMAN 1.4 session.");
    ShutdownNormalClients();
    rc = InitMagicGateSession(&scratch);
    if (rc >= 0) {
        rc = SecrInit();
        if (rc >= 0) {
            bound = SecrDownloadFile(target_port, 0, buffer);
            SecrDeinit();
            rc = (bound == buffer) ? 0 : -4700;
        }
    }

    restore_rc = RestoreNormalEnvironment();
    if (restore_rc < 0) {
        MciGuiRenderFatal("Installer environment restore failed",
                          "KELF binding ended but the normal Sony ROM X environment could not be restored. No destination write is safe.",
                          restore_rc);
        SleepThread();
    }
    if (rc < 0)
        return rc;
    return restore_rc;
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
    FmcbInstallResetReport(&InstallReports[port], port);
}

static unsigned int CurrentFsTestBytes(void)
{
    return MciFsTestProfileBytes(Settings.fs_profile);
}

static int RefreshRecoveryStatus(void)
{
    return FmcbRecoveryProbe(&FmcbMassStatus, &RecoveryStatus);
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
            (void)RefreshRecoveryStatus();
            break;
        case MCI_GUI_SETTINGS:
            break;
        case MCI_GUI_CARD:
        default:
            CardInspectSized(target_port, &Reports[target_port], CurrentFsTestBytes());
            break;
    }
}

static void RunSelectedFullScan(int target_port)
{
    char detail[192];

    snprintf(detail, sizeof(detail),
             "Running the complete read-only sequence for mc%d using %s, then MagicGate/CardAuth and FMCB package preflight.",
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
    }
    (void)FmcbProbeMassPackage(target_port, &FmcbMassStatus,
                               &FmcbReports[target_port]);
    (void)RefreshRecoveryStatus();
}

static int RevalidateInstallerPreconditions(int target_port, char *reason,
                                            unsigned int reason_size)
{
    (void)RefreshRecoveryStatus();
    if (RecoveryStatus.present) {
        snprintf(reason, reason_size,
                 "Persistent FMCB recovery state exists (%s). Recover that transaction before starting another install.",
                 FmcbRecoveryStateText(RecoveryStatus.state));
        return -4;
    }

    CardInspectSized(target_port, &Reports[target_port], CurrentFsTestBytes());
    if (Reports[target_port].health != CARD_OK) {
        snprintf(reason, reason_size, "Filesystem verification is not PASS (%s).",
                 CardHealthText(Reports[target_port].health));
        return -1;
    }
    if (RunMagicGateSession(target_port) < 0 ||
        MgReports[target_port].result != MG_RESULT_PASS) {
        snprintf(reason, reason_size, "MagicGate/CardAuth is not FUNCTIONAL (%s).",
                 MagicGateResultText(MgReports[target_port].result));
        return -2;
    }
    if (FmcbProbeMassPackage(target_port, &FmcbMassStatus,
                             &FmcbReports[target_port]) < 0 ||
        FmcbReports[target_port].status != FMCB_PACKAGE_READY) {
        snprintf(reason, reason_size, "FMCB package preflight is not READY (%s).",
                 FmcbPackageStatusText(FmcbReports[target_port].status));
        return -3;
    }
    return 0;
}

static int RunVerifiedInstaller(int target_port)
{
    FmcbInstallOptions options;
    FmcbInstallReport *report = &InstallReports[target_port];
    char reason[224];
    int rc;

    MciGuiRenderMessage("Revalidating before installation",
                        "The selected card, MagicGate capability, active ROMVER/MechaCon policy and USB package are re-tested immediately before the first destination write.",
                        NULL, MCI_GUI_TONE_WARNING);
    rc = RevalidateInstallerPreconditions(target_port, reason, sizeof(reason));
    if (rc < 0) {
        FmcbInstallResetReport(report, target_port);
        report->result = FMCB_INSTALL_RESULT_REJECTED;
        MciGuiRenderMessage("FMCB installation rejected", reason,
                            "CROSS or CIRCLE returns to the dashboard.",
                            MCI_GUI_TONE_DANGER);
        return -1;
    }

    options.preserve_existing_cnfs = Settings.preserve_existing_cnfs;
    options.verify_every_file = 1;
    rc = FmcbInstallNormalTransactional(target_port,
                                        &FmcbReports[target_port],
                                        &options,
                                        BindKelfForInstaller, NULL,
                                        &RecoveryStatus, report);
    if (rc == 0) {
        char result[360];
        snprintf(result, sizeof(result),
                 "Normal FMCB installation completed on mc%d. %d/%d selected entries committed or intentionally preserved. Every write passed full read-back comparison. Space check: free=%d, payload=%u, reclaimable=%u, reserve=%u clusters. Persistent recovery state was committed and removed.",
                 target_port, report->files_committed, report->files_total,
                 report->free_clusters, report->payload_clusters,
                 report->reclaimable_clusters, report->reserve_clusters);
        MciGuiRenderMessage("FMCB install PASS / VERIFIED", result,
                            "CROSS or CIRCLE returns to the dashboard.",
                            MCI_GUI_TONE_SUCCESS);
    } else {
        char result[420];
        snprintf(result, sizeof(result),
                 "Install failed at %s: %s. Files committed before failure: %d/%d. space rc=%d, recovery rc=%d, rollback rc=%d. If recovery remains present, do not start another install; restore the recorded transaction first.",
                 FmcbInstallStageText(report->stage),
                 FmcbInstallResultText(report->result),
                 report->files_committed, report->files_total,
                 report->space_rc, report->recovery_rc,
                 report->rollback_rc);
        (void)RefreshRecoveryStatus();
        MciGuiRenderMessage("FMCB install failed", result,
                            "CROSS or CIRCLE returns to the dashboard.",
                            report->result == FMCB_INSTALL_RESULT_ROLLBACK_FAILED ||
                            RecoveryStatus.state == FMCB_RECOVERY_CORRUPT
                                ? MCI_GUI_TONE_DANGER : MCI_GUI_TONE_WARNING);
    }
    return rc;
}

static int RunPendingRecovery(void)
{
    char result[320];
    int target;
    int rollback_rc = 0;
    int rc;

    (void)RefreshRecoveryStatus();
    if (!RecoveryStatus.present || !RecoveryStatus.valid) {
        MciGuiRenderMessage("FMCB recovery unavailable",
                            RecoveryStatus.present
                                ? "A recovery directory exists, but neither checksummed journal slot is valid. Automatic card writes are blocked; preserve the USB contents for manual inspection."
                                : "No incomplete FMCB transaction is currently recorded on the connected package device.",
                            "CROSS or CIRCLE returns to the dashboard.",
                            RecoveryStatus.present ? MCI_GUI_TONE_DANGER
                                                   : MCI_GUI_TONE_INFO);
        return -1;
    }

    target = RecoveryStatus.target_port;
    MciGuiRenderMessage("Recovering FMCB transaction",
                        "The persistent journal is being replayed in reverse. Only destinations captured before the interrupted install are restored, and each restored backup is verified against USB.",
                        NULL, MCI_GUI_TONE_WARNING);
    rc = FmcbRecoveryRun(&RecoveryStatus, &rollback_rc);
    if (rc == 0) {
        if (target >= 0 && target < SLOT_COUNT)
            ResetSlotReports(target);
        snprintf(result, sizeof(result),
                 "Recovery completed for mc%d. Every prepared destination was restored to its captured pre-install state and the USB recovery journal was removed.",
                 target);
        MciGuiRenderMessage("FMCB recovery PASS", result,
                            "CROSS or CIRCLE returns to the dashboard.",
                            MCI_GUI_TONE_SUCCESS);
    } else {
        snprintf(result, sizeof(result),
                 "Recovery stopped with rc=%d (rollback rc=%d). The persistent journal has been retained. Do not install over it and do not move the journal to another card.",
                 rc, rollback_rc);
        MciGuiRenderMessage("FMCB recovery incomplete", result,
                            "CROSS or CIRCLE returns to the dashboard.",
                            MCI_GUI_TONE_DANGER);
    }
    return rc;
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
        if (value < 0) value = (int)MCI_VIDEO_MODE_COUNT - 1;
        if ((unsigned int)value >= MCI_VIDEO_MODE_COUNT) value = 0;
        Settings.video_mode = (MciVideoMode)value;
    } else if (row == 1) {
        int value = (int)Settings.fs_profile + direction;
        if (value < 0) value = (int)MCI_FS_TEST_PROFILE_COUNT - 1;
        if (value >= (int)MCI_FS_TEST_PROFILE_COUNT) value = 0;
        Settings.fs_profile = (MciFsTestProfile)value;
    } else if (row == 2) {
        Settings.preserve_existing_cnfs ^= 1;
    }
    /* Per-file read-back verification is intentionally not user-disableable. */
}

int main(int argc, char *argv[])
{
    int selected = 0;
    MciGuiPage page = MCI_GUI_CARD;
    int settings_row = 0;
    int last_video_rc = -999;
    int confirm_format = 0;
    int confirm_install = 0;
    int confirm_recovery = 0;
    int install_result_modal = 0;
    int last_format_rc = -999;
    int init_rc;
    int fmcb_rc;
    int dirty = 1;
    u32 held;
    u32 pressed;

    (void)argc;
    (void)argv;
    MciSettingsDefaults(&Settings);
    memset(&RecoveryStatus, 0, sizeof(RecoveryStatus));
    RecoveryStatus.target_port = -1;

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
                          "The normal memory-card environment could not be initialized.", init_rc);
        SleepThread();
    }

    ResetSlotReports(0);
    ResetSlotReports(1);
    fmcb_rc = FmcbInitMassBackend(&FmcbMassStatus);
    (void)fmcb_rc;
    (void)RefreshRecoveryStatus();
    if (RecoveryStatus.present)
        page = MCI_GUI_FMCB;

    while (1) {
        if (dirty) {
            RenderDashboard(selected, page, settings_row, last_video_rc,
                            confirm_format, last_format_rc);
            dirty = 0;
        }

        pressed = ReadPadPressed(&held);

        if (install_result_modal) {
            if (pressed & (PAD_CROSS | PAD_CIRCLE)) {
                install_result_modal = 0;
                dirty = 1;
            }
            DelayThread(16000);
            continue;
        }

        if (confirm_recovery) {
            if (pressed & PAD_CIRCLE) {
                confirm_recovery = 0;
                dirty = 1;
            } else if ((pressed & PAD_TRIANGLE) &&
                       (held & PAD_L1) && (held & PAD_R1)) {
                confirm_recovery = 0;
                (void)RunPendingRecovery();
                install_result_modal = 1;
            }
            DelayThread(16000);
            continue;
        }

        if (confirm_install) {
            if (pressed & PAD_CIRCLE) {
                confirm_install = 0;
                dirty = 1;
            } else if ((pressed & PAD_SQUARE) &&
                       (held & PAD_L1) && (held & PAD_R1)) {
                confirm_install = 0;
                (void)RunVerifiedInstaller(selected);
                install_result_modal = 1;
            }
            DelayThread(16000);
            continue;
        }

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
                    settings_row = settings_row == 0 ? SETTINGS_ROW_COUNT - 1 : settings_row - 1;
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

            if ((pressed & PAD_SQUARE) && page == MCI_GUI_FMCB) {
                (void)RefreshRecoveryStatus();
                if (RecoveryStatus.present) {
                    if (!RecoveryStatus.valid) {
                        MciGuiRenderMessage("Recovery journal requires inspection",
                                            "An FMCB recovery directory exists, but neither checksummed journal slot is valid. A new installation is blocked so the evidence is not overwritten.",
                                            "CROSS or CIRCLE returns to the dashboard.",
                                            MCI_GUI_TONE_DANGER);
                        install_result_modal = 1;
                    } else {
                        char message[360];
                        snprintf(message, sizeof(message),
                                 "Recover the interrupted FMCB transaction recorded for mc%d?\n\nState: %s\nPrepared destinations: %d\nUSB root: %s\n\nRecovery validates the card transaction marker, restores every captured destination in reverse order, verifies restored files, then removes the journal.",
                                 RecoveryStatus.target_port,
                                 FmcbRecoveryStateText(RecoveryStatus.state),
                                 RecoveryStatus.prepared_files,
                                 RecoveryStatus.source_root);
                        MciGuiRenderMessage("FMCB RECOVERY CONFIRMATION", message,
                                            "Hold L1 + R1 and press TRIANGLE to recover. CIRCLE cancels.",
                                            MCI_GUI_TONE_DANGER);
                        confirm_recovery = 1;
                    }
                } else if (FmcbReports[selected].status != FMCB_PACKAGE_READY) {
                    MciGuiRenderMessage("Installer locked",
                                        "Run FMCB Preflight with CROSS first. The normal installer is armed only for a package that resolves every required source and destination.",
                                        "CROSS or CIRCLE returns to the dashboard.",
                                        MCI_GUI_TONE_WARNING);
                    install_result_modal = 1;
                } else {
                    const FmcbInstallPlan *plan = &FmcbReports[selected].plan;
                    const char *compact;
                    char message[640];

                    if (plan->compact_unlock_active)
                        compact = "Real DEX profile: compact reference manifest ACTIVE; ENDVDPL is omitted.";
                    else if (plan->compact_unlock_candidate)
                        compact = "DEX-like/region-unlocked MechaCon detected: compact manifest candidate found, but the CEX payload is retained until hardware validation proves ENDVDPL can be omitted safely.";
                    else
                        compact = "Retail region policy: normal CEX manifest.";

                    snprintf(message, sizeof(message),
                             "Install normal FreeMcBoot to mc%d:\n\nTarget: %s/%s\nROMVER: %04X%c  MechaCon: %u.%02u\nPolicy: %s\n%s\n\nThe card, MagicGate and package will be revalidated. Space is simulated before writes. Every replaced target is persisted and verified on USB, every card write is read back, and an interrupted transaction can be recovered on the marked target card.",
                             selected, plan->destination_system,
                             plan->destination_osd, plan->rom_version,
                             plan->romver_region,
                             plan->console.mecha_major,
                             plan->console.mecha_minor,
                             MciConsoleRegionPolicyText(&plan->console),
                             compact);
                    MciGuiRenderMessage("FMCB INSTALL CONFIRMATION", message,
                                        "Hold L1 + R1 and press SQUARE to install. CIRCLE cancels.",
                                        MCI_GUI_TONE_DANGER);
                    confirm_install = 1;
                }
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

    MciGuiRenderMessage("Exiting", "Closing controller and USB clients.",
                        NULL, MCI_GUI_TONE_INFO);
    ShutdownNormalClients();
    SifExitRpc();
    return 0;
}
