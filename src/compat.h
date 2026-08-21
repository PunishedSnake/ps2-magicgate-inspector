#ifndef MC_INSPECTOR_COMPAT_H
#define MC_INSPECTOR_COMPAT_H

/*
 * PS2SDK v1.0 compatibility shims.
 *
 * This header is force-included before each source file.  Pull in the PS2SDK
 * declarations first; only then redirect calls to our diagnostic wrappers.
 * That keeps the SDK prototypes intact while still intercepting calls made by
 * the Inspector source itself.
 */
#include <tamtypes.h>
#include <iopcontrol.h>
#include <loadfile.h>

int DelayThread(int usec);
int MciSifIopReset(const char *arg, int mode);
int MciSifIopSync(void);
int MciSifExecModuleBuffer(void *ptr, u32 size, u32 arg_len, const char *args, int *mod_res);

#define SifIopReset(arg, mode) MciSifIopReset((arg), (mode))
#define SifIopSync() MciSifIopSync()
#define SifExecModuleBuffer(ptr, size, arg_len, args, mod_res) \
    MciSifExecModuleBuffer((ptr), (size), (arg_len), (args), (mod_res))

#endif
