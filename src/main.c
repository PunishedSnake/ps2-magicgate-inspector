#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <libmc.h>
#include <libpad.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>

#define APP_VERSION "0.1.0-standalone"
#define TEST_SIZE 4096
#define SLOT_COUNT 2

extern unsigned char freesio2_irx[];
extern unsigned int size_freesio2_irx;
extern unsigned char freepad_irx[];
extern unsigned int size_freepad_irx;
extern unsigned char mcman_irx[];
extern unsigned int size_mcman_irx;
extern unsigned char mcserv_irx[];
extern unsigned int size_mcserv_irx;

typedef enum CardHealth {
    CARD_UNKNOWN = 0,
    CARD_OK,
    CARD_FULL,
    CARD_UNFORMATTED,
    CARD_FILESYSTEM_BROKEN,
    CARD_IO_FAILURE,
    CARD_AUTH_FAILURE,
    CARD_DETECT_FAILURE,
    CARD_NO_CARD
} CardHealth;

typedef struct CardReport {
    int port;
    int info_rc;
    int type;
    int free_clusters;
    int formatted;
    int root_rc;
    int rw_rc;
    int cleanup_rc;
    int format_allowed;
    CardHealth health;
} CardReport;

static unsigned char PadBuffer[256] __attribute__((aligned(64)));
static unsigned char WriteBuffer[TEST_SIZE] __attribute__((aligned(64)));
static unsigned char ReadBuffer[TEST_SIZE] __attribute__((aligned(64)));
static sceMcTblGetDir DirEntry __attribute__((aligned(64)));
static CardReport Reports[SLOT_COUNT];

static int McSyncResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static int ExecIrx(void *buffer, unsigned int size)
{
    int status = 0;
    return SifExecModuleBuffer(buffer, size, 0, NULL, &status);
}

static int InitIopAndDevices(void)
{
    int rc;

    SifInitRpc(0);
    while (!SifIopReset("", 0)) {;}
    while (!SifIopSync()) {;}

    SifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();
    sbv_patch_enable_lmb();

    rc = ExecIrx(freesio2_irx, size_freesio2_irx);
    if (rc < 0) return rc;
    rc = ExecIrx(freepad_irx, size_freepad_irx);
    if (rc < 0) return rc;
    rc = ExecIrx(mcman_irx, size_mcman_irx);
    if (rc < 0) return rc;
    rc = ExecIrx(mcserv_irx, size_mcserv_irx);
    if (rc < 0) return rc;

    SifExitIopHeap();
    SifLoadFileExit();

    rc = mcInit(MC_TYPE_XMC);
    if (rc < 0) return rc;

    rc = padInit(0);
    if (rc == 0) return -1;

    if (padPortOpen(0, 0, PadBuffer) == 0)
        return -2;

    return 0;
}

static void FillPattern(void)
{
    unsigned int i;
    unsigned int state = 0x4D43494Eu; /* MCIN */

    for (i = 0; i < sizeof(WriteBuffer); i++) {
        state = state * 1664525u + 1013904223u;
        WriteBuffer[i] = (unsigned char)((state >> 24) ^ i ^ (i >> 4));
    }
}

static int CloseFile(int fd)
{
    mcClose(fd);
    return McSyncResult();
}

static int DeleteFile(int port, const char *name)
{
    mcDelete(port, 0, name);
    return McSyncResult();
}

static int FindUnusedTempName(int port, char *name)
{
    int i;
    int rc;

    for (i = 0; i < 100; i++) {
        sprintf(name, "/__MCI%02d.TMP", i);
        memset(&DirEntry, 0, sizeof(DirEntry));
        mcGetDir(port, 0, name, 0, 1, &DirEntry);
        rc = McSyncResult();

        if (rc == 0 || rc == sceMcResNoEntry)
            return 0;

        if (rc < 0)
            return rc;

        /* A positive result means that exact filename already exists. */
    }

    return -1000;
}

static int RunReadWriteTest(int port, int *cleanup_rc)
{
    char path[24];
    int fd;
    int rc;

    *cleanup_rc = 0;
    FillPattern();
    memset(ReadBuffer, 0, sizeof(ReadBuffer));

    rc = FindUnusedTempName(port, path);
    if (rc < 0)
        return rc;

    mcOpen(port, 0, path, O_WRONLY | O_CREAT | O_TRUNC);
    fd = McSyncResult();
    if (fd < 0)
        return fd;

    mcWrite(fd, WriteBuffer, sizeof(WriteBuffer));
    rc = McSyncResult();
    if (rc != (int)sizeof(WriteBuffer)) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return (rc < 0) ? rc : -1001;
    }

    mcFlush(fd);
    rc = McSyncResult();
    if (rc < 0) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    rc = CloseFile(fd);
    if (rc < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    mcOpen(port, 0, path, O_RDONLY);
    fd = McSyncResult();
    if (fd < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return fd;
    }

    mcRead(fd, ReadBuffer, sizeof(ReadBuffer));
    rc = McSyncResult();
    if (rc != (int)sizeof(ReadBuffer)) {
        CloseFile(fd);
        *cleanup_rc = DeleteFile(port, path);
        return (rc < 0) ? rc : -1002;
    }

    rc = CloseFile(fd);
    if (rc < 0) {
        *cleanup_rc = DeleteFile(port, path);
        return rc;
    }

    if (memcmp(WriteBuffer, ReadBuffer, sizeof(WriteBuffer)) != 0) {
        *cleanup_rc = DeleteFile(port, path);
        return -1003;
    }

    *cleanup_rc = DeleteFile(port, path);
    if (*cleanup_rc < 0)
        return -1004;

    memset(&DirEntry, 0, sizeof(DirEntry));
    mcGetDir(port, 0, path, 0, 1, &DirEntry);
    rc = McSyncResult();
    if (rc != 0 && rc != sceMcResNoEntry)
        return -1005;

    return 0;
}

static void InspectCard(int port)
{
    CardReport *r = &Reports[port];

    memset(r, 0, sizeof(*r));
    r->port = port;
    r->root_rc = -999;
    r->rw_rc = -999;
    r->cleanup_rc = 0;
    r->health = CARD_UNKNOWN;

    mcGetInfo(port, 0, &r->type, &r->free_clusters, &r->formatted);
    r->info_rc = McSyncResult();

    if (r->info_rc == sceMcResFailAuth) {
        r->health = CARD_AUTH_FAILURE;
        return;
    }

    if (r->info_rc <= sceMcResFailDetect) {
        r->health = CARD_DETECT_FAILURE;
        return;
    }

    if (r->type == MC_TYPE_NONE) {
        r->health = CARD_NO_CARD;
        return;
    }

    if (r->info_rc == sceMcResNoFormat || !r->formatted) {
        r->health = CARD_UNFORMATTED;
        r->format_allowed = (r->type == MC_TYPE_PS2);
        return;
    }

    memset(&DirEntry, 0, sizeof(DirEntry));
    mcGetDir(port, 0, "/*", 0, 1, &DirEntry);
    r->root_rc = McSyncResult();

    if (r->root_rc == sceMcResNoFormat) {
        r->health = CARD_FILESYSTEM_BROKEN;
        r->format_allowed = (r->type == MC_TYPE_PS2);
        return;
    }

    if (r->root_rc == sceMcResFailAuth) {
        r->health = CARD_AUTH_FAILURE;
        return;
    }

    if (r->root_rc <= sceMcResFailDetect) {
        r->health = CARD_DETECT_FAILURE;
        return;
    }

    if (r->root_rc < 0 && r->root_rc != sceMcResNoEntry) {
        r->health = CARD_IO_FAILURE;
        return;
    }

    r->rw_rc = RunReadWriteTest(port, &r->cleanup_rc);

    if (r->rw_rc == 0) {
        r->health = CARD_OK;
    } else if (r->rw_rc == sceMcResFullDevice) {
        r->health = CARD_FULL;
    } else if (r->rw_rc == sceMcResNoFormat) {
        r->health = CARD_FILESYSTEM_BROKEN;
        r->format_allowed = (r->type == MC_TYPE_PS2);
    } else if (r->rw_rc == sceMcResFailAuth) {
        r->health = CARD_AUTH_FAILURE;
    } else if (r->rw_rc <= sceMcResFailDetect) {
        r->health = CARD_DETECT_FAILURE;
    } else {
        r->health = CARD_IO_FAILURE;
    }
}

static const char *HealthText(CardHealth health)
{
    switch (health) {
        case CARD_OK: return "PASS";
        case CARD_FULL: return "FULL - R/W TEST COULD NOT RUN";
        case CARD_UNFORMATTED: return "UNFORMATTED / FRESH";
        case CARD_FILESYSTEM_BROKEN: return "FILESYSTEM BROKEN / NO FORMAT";
        case CARD_IO_FAILURE: return "I/O FAILURE";
        case CARD_AUTH_FAILURE: return "CARD AUTHENTICATION FAILURE";
        case CARD_DETECT_FAILURE: return "CARD DETECTION FAILURE";
        case CARD_NO_CARD: return "NO CARD";
        default: return "UNKNOWN";
    }
}

static const char *TypeText(int type)
{
    switch (type) {
        case MC_TYPE_NONE: return "none";
        case MC_TYPE_PSX: return "PS1";
        case MC_TYPE_PS2: return "PS2";
        case MC_TYPE_POCKET: return "PocketStation/PDA";
        default: return "non-standard/unknown";
    }
}

static void Render(int selected, int confirm_format, int last_format_rc)
{
    CardReport *r = &Reports[selected];

    scr_clear();
    scr_printf("PS2 Memory Card Inspector v%s\n", APP_VERSION);
    scr_printf("Standalone diagnostic build - no FMCB installation code\n\n");
    scr_printf("< LEFT / RIGHT > select slot    X test    START test both\n");
    scr_printf("SELECT exit\n\n");

    scr_printf("Selected: SLOT %d (mc%d:)\n", selected + 1, selected);
    scr_printf("Health: %s\n", HealthText(r->health));
    scr_printf("mcGetInfo rc: %d\n", r->info_rc);
    scr_printf("Reported type: %d (%s)\n", r->type, TypeText(r->type));
    scr_printf("Formatted flag: %d\n", r->formatted);
    scr_printf("Free clusters: %d\n", r->free_clusters);
    scr_printf("Root directory rc: %d\n", r->root_rc);
    scr_printf("4 KiB write/read/compare/delete rc: %d\n", r->rw_rc);
    scr_printf("Cleanup rc: %d\n\n", r->cleanup_rc);

    scr_printf("Other slot: mc%d: %s\n\n", selected ^ 1, HealthText(Reports[selected ^ 1].health));

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
        scr_printf("\nLast format rc: %d\n", last_format_rc);

    scr_printf("\nRaw MCMAN errors are shown intentionally for diagnosis.\n");
}

static int FormatCard(int port)
{
    int rc;

    mcFormat(port, 0);
    rc = McSyncResult();
    if (rc == 0)
        InspectCard(port);
    return rc;
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
    scr_printf("PS2 Memory Card Inspector\nInitializing IOP and memory-card services...\n");

    init_rc = InitIopAndDevices();
    if (init_rc < 0) {
        scr_printf("\nInitialization failed: %d\n", init_rc);
        scr_printf("System halted.\n");
        SleepThread();
    }

    InspectCard(0);
    InspectCard(1);

    while (1) {
        if (dirty) {
            Render(selected, confirm_format, last_format_rc);
            dirty = 0;
        }

        pressed = ReadPadPressed(&held);

        if (pressed & PAD_SELECT)
            break;

        if (confirm_format) {
            if (pressed & PAD_CIRCLE) {
                confirm_format = 0;
                dirty = 1;
            } else if ((pressed & PAD_TRIANGLE) && (held & PAD_L1) && (held & PAD_R1)) {
                last_format_rc = FormatCard(selected);
                confirm_format = 0;
                dirty = 1;
            }
        } else {
            if (pressed & (PAD_LEFT | PAD_RIGHT)) {
                selected ^= 1;
                last_format_rc = -999;
                dirty = 1;
            }

            if (pressed & PAD_CROSS) {
                InspectCard(selected);
                dirty = 1;
            }

            if (pressed & PAD_START) {
                InspectCard(0);
                InspectCard(1);
                dirty = 1;
            }

            if ((pressed & PAD_TRIANGLE) && Reports[selected].format_allowed) {
                confirm_format = 1;
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
