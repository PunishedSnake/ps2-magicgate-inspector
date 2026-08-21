#ifndef MC_INSPECTOR_COMPAT_H
#define MC_INSPECTOR_COMPAT_H

/*
 * PS2SDK v1.0 compatibility shims.
 *
 * The standalone Inspector is often launched from an already-initialized
 * homebrew environment (uLaunchELF/FMCB).  Keep the actual IOP reset calls
 * behind wrappers so we can use the canonical NULL reset argument and print
 * progress before any potentially blocking stage.
 */
int DelayThread(int usec);

#define SifIopReset(arg, mode) MciSifIopReset((arg), (mode))
#define SifIopSync() MciSifIopSync()
#define SifExecModuleBuffer(ptr, size, arg_len, args, mod_res) \
    MciSifExecModuleBuffer((ptr), (size), (arg_len), (args), (mod_res))

#endif
