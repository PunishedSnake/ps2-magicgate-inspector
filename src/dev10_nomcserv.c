/*
 * Briscoe dev10/dev12 diagnostic shim.
 *
 * Real hardware showed the PS2SDK v1 MCSERV returning RESIDENT_END and the
 * next LOADFILE RPC never completing. The isolated security session therefore
 * skips temporary MCSERV and fakes only the EE-side libmc sanity probe.
 *
 * dev12 no longer replays card-auth commands after GET_KBIT. The temporary
 * SECRMAN 1.3 itself instruments the exact failing path and returns diagnostics
 * through the existing GET_KBIT response buffer.
 */

#include <tamtypes.h>
#include <loadfile.h>
#include <libmc.h>
#include <debug.h>

extern unsigned char fmcb_mcserv_irx[];
extern unsigned char secrsif_irx[];

static int Dev10IsolatedNoMcserv;
static int Dev10FakeMcInitDone;
static int Dev10FakeMcPending;

int __real_SifExecModuleBuffer(void *ptr, u32 size, u32 arg_len,
                               const char *args, int *mod_res);
int __real_mcInit(int type);
int __real_mcGetInfo(int port, int slot, int *type, int *free_clusters,
                     int *formatted);
int __real_mcSync(int mode, int *cmd, int *result);

int __wrap_SifExecModuleBuffer(void *ptr, u32 size, u32 arg_len,
                               const char *args, int *mod_res)
{
    int rc;

    if (ptr == fmcb_mcserv_irx) {
        scr_printf("[DEV12] Skipping temporary MCSERV v1.\n");
        if (mod_res != 0)
            *mod_res = 0;
        Dev10IsolatedNoMcserv = 1;
        Dev10FakeMcInitDone = 0;
        Dev10FakeMcPending = 0;
        return 0x7f;
    }

    if (ptr == secrsif_irx)
        scr_printf("[DEV12] Loading SECRSIF after instrumented SECRMAN...\n");

    rc = __real_SifExecModuleBuffer(ptr, size, arg_len, args, mod_res);

    if (ptr == secrsif_irx)
        scr_printf("[DEV12] SECRSIF returned rc=%d start=%d.\n",
                   rc, mod_res != 0 ? *mod_res : -999);

    return rc;
}

int __wrap_mcInit(int type)
{
    if (Dev10IsolatedNoMcserv && !Dev10FakeMcInitDone) {
        scr_printf("[DEV12] Bypassing isolated EE mcInit; MCMAN stays active.\n");
        Dev10FakeMcInitDone = 1;
        return 0;
    }

    if (Dev10IsolatedNoMcserv && Dev10FakeMcInitDone) {
        Dev10IsolatedNoMcserv = 0;
        Dev10FakeMcPending = 0;
        scr_printf("[DEV12] Restored ROM X stack: real mcInit resumes.\n");
    }

    return __real_mcInit(type);
}

int __wrap_mcGetInfo(int port, int slot, int *type, int *free_clusters,
                     int *formatted)
{
    if (Dev10IsolatedNoMcserv) {
        (void)port;
        (void)slot;
        if (type != 0)
            *type = MC_TYPE_PS2;
        if (free_clusters != 0)
            *free_clusters = 0;
        if (formatted != 0)
            *formatted = MC_FORMATTED;
        Dev10FakeMcPending = 1;
        return 0;
    }

    return __real_mcGetInfo(port, slot, type, free_clusters, formatted);
}

int __wrap_mcSync(int mode, int *cmd, int *result)
{
    if (Dev10IsolatedNoMcserv && Dev10FakeMcPending) {
        (void)mode;
        if (cmd != 0)
            *cmd = 0;
        if (result != 0)
            *result = 0;
        Dev10FakeMcPending = 0;
        return 1;
    }

    return __real_mcSync(mode, cmd, result);
}
