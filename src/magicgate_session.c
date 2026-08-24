/* SPDX-License-Identifier: MIT */
/*
 * Isolated MagicGate session shim.
 *
 * v0.2.0 embeds a matching PS2SDK 2.0 SIO2/PAD/MCMAN/MCSERV set, but the
 * temporary MCSERV is intentionally not started. Real hardware showed that
 * starting it in this isolated security personality can wedge the next LOADFILE
 * RPC even though the module itself reports a successful resident result.
 *
 * CardAuth needs MCMAN's registered SECRMAN callbacks, not EE file-service I/O,
 * so MCMAN stays active while this shim emulates only the immediate EE-side
 * libmc sanity query used by the RAM-only probe. After the security transaction
 * the application rebuilds the normal Sony ROM X card stack; subsequent libmc
 * calls are real again.
 *
 * Drebin's raw image personality is the deliberate exception: PS2SDK MCSERV is
 * allowed to start there because its page-level RPCs are required for .ps2/.vmc
 * imaging. The caller enables that exception only around the MCSERV load.
 *
 * This shim deliberately emits no libdebug screen output. Once the GS frontend
 * is active, direct scr_printf() calls can contaminate one of its double-buffered
 * framebuffers. Session progress is presented by app_main.c through gui.c.
 */

#include <tamtypes.h>
#include <loadfile.h>
#include <libmc.h>

#include "magicgate_session.h"

extern unsigned char fmcb_mcserv_irx[];
extern unsigned char secrsif_irx[];

static int IsolatedNoMcserv;
static int FakeMcInitDone;
static int FakeMcPending;
static int AllowRealMcserv;

int __real_SifExecModuleBuffer(void *ptr, u32 size, u32 arg_len,
                               const char *args, int *mod_res);
int __real_mcInit(int type);
int __real_mcGetInfo(int port, int slot, int *type, int *free_clusters,
                     int *formatted);
int __real_mcSync(int mode, int *cmd, int *result);

void MciSessionAllowRealMcserv(int allowed)
{
    AllowRealMcserv = allowed != 0;
}

void MciSessionResetShim(void)
{
    /* These variables live on the EE and therefore survive an IOP reboot. */
    IsolatedNoMcserv = 0;
    FakeMcInitDone = 0;
    FakeMcPending = 0;
    AllowRealMcserv = 0;
}

static void PrimeNormalCardSlot(int port)
{
    int attempt;

    /*
     * XMCMAN loses all card-detection state when the IOP is rebuilt after the
     * temporary MagicGate personality. File operations issued immediately after
     * mcInit can therefore fail with sceMcResFailDetect (-12), even though the
     * same card passed the pre-MagicGate filesystem test. Re-run the normal
     * GetInfo handshake here. A first -1 is the expected "changed card" result;
     * the second pass settles the freshly initialized driver on the same card.
     *
     * Empty slots and genuinely failing cards are intentionally ignored here.
     * Normal callers still perform their own validation and will report them.
     */
    for (attempt = 0; attempt < 2; attempt++) {
        int card_type = MC_TYPE_NONE;
        int free_clusters = 0;
        int formatted = 0;
        int result = -999;
        int issue_rc;
        int sync_rc;

        issue_rc = __real_mcGetInfo(port, 0, &card_type, &free_clusters,
                                    &formatted);
        if (issue_rc < 0)
            return;
        sync_rc = __real_mcSync(MC_WAIT, NULL, &result);
        if (sync_rc < 0)
            return;
        if (result != sceMcResChangedCard)
            return;
    }
}

int __wrap_SifExecModuleBuffer(void *ptr, u32 size, u32 arg_len,
                               const char *args, int *mod_res)
{
    int rc;

    if (ptr == fmcb_mcserv_irx && !AllowRealMcserv) {
        if (mod_res != 0)
            *mod_res = 0;
        IsolatedNoMcserv = 1;
        FakeMcInitDone = 0;
        FakeMcPending = 0;
        return 0x7f;
    }

    rc = __real_SifExecModuleBuffer(ptr, size, arg_len, args, mod_res);
    return rc;
}

int __wrap_mcInit(int type)
{
    int rc;

    if (IsolatedNoMcserv && !FakeMcInitDone) {
        FakeMcInitDone = 1;
        return 0;
    }

    if (IsolatedNoMcserv && FakeMcInitDone) {
        IsolatedNoMcserv = 0;
        FakeMcPending = 0;
    }

    rc = __real_mcInit(type);
    if (rc >= 0 && type == MC_TYPE_XMC) {
        PrimeNormalCardSlot(0);
        PrimeNormalCardSlot(1);
    }
    return rc;
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
