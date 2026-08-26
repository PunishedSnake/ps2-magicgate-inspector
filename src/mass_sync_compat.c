/* SPDX-License-Identifier: MIT */
/*
 * USBHDFSD compatibility shim.
 *
 * The hardware-qualified mass: backend repeatedly returns -5 for fileXioSync
 * even when close/reopen verification succeeds. Drebin's safety gate is the
 * actual reopen/read-back comparison, not support for this optional device
 * operation. Normalize only that observed mass:/mass0:/mass1: result.
 */

#define NEWLIB_PORT_AWARE

#include <fileXio_rpc.h>
#include <string.h>

int __real_fileXioSync(const char *device, int flag);

static int IsMassDevice(const char *device)
{
    return device != NULL &&
           (strcmp(device, "mass:") == 0 ||
            strcmp(device, "mass0:") == 0 ||
            strcmp(device, "mass1:") == 0);
}

int __wrap_fileXioSync(const char *device, int flag)
{
    int rc = __real_fileXioSync(device, flag);

    if (rc == -5 && IsMassDevice(device))
        return 0;
    return rc;
}
