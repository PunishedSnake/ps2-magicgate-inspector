/* SPDX-License-Identifier: MIT */
/*
 * Card Tools v2 currently inherits one legacy confirmation string from the
 * composition controller. Keep the destructive prompt truthful while the
 * backend has moved from PCSX2 raw recovery to the verified VMC path.
 */

#include <string.h>

#include "gui.h"

void __real_MciGuiRenderMessage(const char *title, const char *body,
                                const char *footer, MciGuiTone tone);

void __wrap_MciGuiRenderMessage(const char *title, const char *body,
                                const char *footer, MciGuiTone tone)
{
    static const char ForceVmcBody[] =
        "Force-format the selected card regardless of its current filesystem state?\n\n"
        "Before mcFormat is allowed, Drebin creates a complete OPL-style .vmc logical recovery image on USB, reopens the full image and compares its CRC with the physical card capture. If that backup or read-back verification fails, formatting is blocked.";

    if (title != NULL && strcmp(title, "FORCE FORMAT + VERIFIED BACKUP") == 0)
        body = ForceVmcBody;
    __real_MciGuiRenderMessage(title, body, footer, tone);
}
