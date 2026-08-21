/* Compatibility and early-boot diagnostics for the old PS2DEV v1.0 toolchain. */

/* compat.h is force-included by the Makefile.  Undefine its interception
 * macros here so this translation unit can call the real PS2SDK functions. */
#undef SifIopReset
#undef SifIopSync
#undef SifExecModuleBuffer

#include <tamtypes.h>
#include <iopcontrol.h>
#include <loadfile.h>
#include <debug.h>

extern unsigned char freesio2_irx[];
extern unsigned char freepad_irx[];
extern unsigned char mcman_irx[];
extern unsigned char mcserv_irx[];

int DelayThread(int usec)
{
    volatile unsigned int loops;

    if (usec <= 0)
        return 0;

    /* UI throttling only. No memory-card protocol timing depends on this. */
    loops = (unsigned int)usec * 24u;
    while (loops-- != 0u)
        __asm__ __volatile__("nop");

    return 0;
}

/*
 * The original standalone build passed an empty string to SifIopReset().
 * PS2SDK itself consistently uses NULL for a normal reset.  On real hardware
 * an empty argument string can leave the reboot path waiting indefinitely.
 * Ignore the caller's string and always use the canonical NULL form here.
 */
int MciSifIopReset(const char *arg, int mode)
{
    static int announced = 0;
    static int completed = 0;
    int rc;

    (void)arg;

    if (!announced) {
        scr_printf("[IOP 1/6] Requesting IOP reset (NULL args)...\n");
        announced = 1;
    }

    rc = SifIopReset(NULL, mode);
    if (rc && !completed) {
        scr_printf("[IOP 1/6] Reset request accepted.\n");
        completed = 1;
    }

    return rc;
}

int MciSifIopSync(void)
{
    static int announced = 0;
    static int completed = 0;
    int rc;

    if (!announced) {
        scr_printf("[IOP 2/6] Waiting for IOP sync...\n");
        announced = 1;
    }

    rc = SifIopSync();
    if (rc && !completed) {
        scr_printf("[IOP 2/6] IOP synchronized.\n");
        completed = 1;
    }

    return rc;
}

static const char *IrxName(void *ptr)
{
    if (ptr == freesio2_irx) return "freesio2.irx";
    if (ptr == freepad_irx)  return "freepad.irx";
    if (ptr == mcman_irx)    return "mcman.irx";
    if (ptr == mcserv_irx)   return "mcserv.irx";
    return "unknown IRX";
}

int MciSifExecModuleBuffer(void *ptr, u32 size, u32 arg_len, const char *args, int *mod_res)
{
    const char *name = IrxName(ptr);
    int rc;

    scr_printf("[IOP] Loading %s (%u bytes)...\n", name, size);
    rc = SifExecModuleBuffer(ptr, size, arg_len, args, mod_res);

    if (rc < 0)
        scr_printf("[IOP] %s FAILED: rc=%d\n", name, rc);
    else if (mod_res != NULL)
        scr_printf("[IOP] %s OK: id=%d start=%d\n", name, rc, *mod_res);
    else
        scr_printf("[IOP] %s OK: id=%d\n", name, rc);

    return rc;
}
