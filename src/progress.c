/* SPDX-License-Identifier: MIT */
/* Live progress adapter for the native GS frontend. */

#include "gui.h"
#include "progress.h"

static const char *ProgressTitle(MciProgressDomain domain)
{
    switch (domain) {
        case MCI_PROGRESS_FILESYSTEM: return "Filesystem inspection";
        case MCI_PROGRESS_MAGICGATE: return "MagicGate / KELF probe";
        case MCI_PROGRESS_FMCB: return "FMCB package / installer";
        case MCI_PROGRESS_ENVIRONMENT: return "IOP / card environment";
        case MCI_PROGRESS_CARD_TOOLS: return "Memory Card Tools";
        default: return "Working";
    }
}

static const char *ProgressFooter(MciProgressDomain domain)
{
    switch (domain) {
        case MCI_PROGRESS_FILESYSTEM:
            return "Do not remove the memory card while the temporary test is active.";
        case MCI_PROGRESS_MAGICGATE:
            return "Do not remove the memory card or USB source during the security probe.";
        case MCI_PROGRESS_FMCB:
            return "Do not remove the memory card or USB source during an installer transaction.";
        case MCI_PROGRESS_ENVIRONMENT:
            return "Please wait while the program changes or restores the IOP environment.";
        case MCI_PROGRESS_CARD_TOOLS:
            return "Do not remove the selected card or USB storage while imaging or restoring.";
        default:
            return "Please wait.";
    }
}

void MciProgressUpdate(MciProgressDomain domain,
                       int percent,
                       const char *action,
                       const char *detail)
{
    if (!MciGuiReady())
        return;

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    MciGuiRenderProgress(ProgressTitle(domain), action, detail, percent,
                         ProgressFooter(domain), MCI_GUI_TONE_INFO);
}
