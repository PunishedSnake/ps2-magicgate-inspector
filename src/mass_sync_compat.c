/* SPDX-License-Identifier: MIT */
/*
 * USBHDFSD compatibility shim.
 *
 * The hardware-qualified mass: backend repeatedly returns -5 for fileXioSync
 * even when close/reopen verification succeeds. Current PS2SDK USBHDFSD is
 * built on the vfat driver whose IOMANX sync operation returns EIO, so this
 * compatibility result MUST NOT be treated as a durability or reset barrier.
 * Drebin's safety gate is close/reopen/read-back plus explicit mass: ownership.
 * Normalize only the observed mass:/mass0:/mass1: result for legacy callers.
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
