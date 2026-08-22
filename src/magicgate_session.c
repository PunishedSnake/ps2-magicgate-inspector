/*
 * Briscoe isolated MagicGate session shim.
 *
 * Real hardware showed that loading the temporary MCSERV before SECRSIF can
 * wedge the next LOADFILE RPC. The isolated security personality therefore
 * keeps MCMAN active, skips temporary MCSERV, and fakes only the EE-side libmc
 * sanity query used immediately before the RAM-only KELF probe.
 *
 * The normal ROM X memory-card stack is restored afterwards and all subsequent
 * libmc calls are real again.
 */

#include <tamtypes.h>
#include <loadfile.h>
#include <libmc.h>
#include <debug.h>

extern unsigned char fmcb_mcserv_irx[];
extern unsigned char secrsif_irx[];

static int IsolatedNoMcserv;
static int FakeMcInitDone;
static int FakeMcPending;

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
        scr_printf("[MG] Skipping temporary MCSERV; MCMAN remains resident.\n");
        if (mod_res != 0)
            *mod_res = 0;
        IsolatedNoMcserv = 1;
        FakeMcInitDone = 0;
        FakeMcPending = 0;
        return 0x7f;
    }

    if (ptr == secrsif_irx)
        scr_printf("[MG] Loading SECRSIF bridge...\n");

    rc = __real_SifExecModuleBuffer(ptr, size, arg_len, args, mod_res);

    if (ptr == secrsif_irx)
        scr_printf("[MG] SECRSIF returned rc=%d start=%d.\n",
                   rc, mod_res != 0 ? *mod_res : -999);

    return rc;
}

int __wrap_mcInit(int type)
{
    if (IsolatedNoMcserv && !FakeMcInitDone) {
        scr_printf("[MG] Bypassing isolated EE mcInit; MCMAN stays active.\n");
        FakeMcInitDone = 1;
        return 0;
    }

    if (IsolatedNoMcserv && FakeMcInitDone) {
        IsolatedNoMcserv = 0;
        FakeMcPending = 0;
        scr_printf("[MG] Normal ROM X stack restored; real mcInit resumes.\n");
    }

    return __real_mcInit(type);
}

int __wrap_mcGetInfo(int port, int slot, int *type, int *free_clusters,
                     int *formatted)
{
    if (IsolatedNoMcserv) {
        (void)port;
        (void)slot;
        if (type != 0)
            *type = MC_TYPE_PS2;
        if (free_clusters != 0)
            *free_clusters = 0;
        if (formatted != 0)
            *formatted = MC_FORMATTED;
        FakeMcPending = 1;
        return 0;
    }

    return __real_mcGetInfo(port, slot, type, free_clusters, formatted);
}

int __wrap_mcSync(int mode, int *cmd, int *result)
{
    if (IsolatedNoMcserv && FakeMcPending) {
        (void)mode;
        if (cmd != 0)
            *cmd = 0;
        if (result != 0)
            *result = 0;
        FakeMcPending = 0;
        return 1;
    }

    return __real_mcSync(mode, cmd, result);
}
