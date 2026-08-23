/* SPDX-License-Identifier: MIT */
/* Live progress adapter for the native GS frontend. */

#include <stdio.h>
#include <string.h>

#include "diag_log.h"
#include "gui.h"
#include "progress.h"

static int LastLoggedDomain = -1;
static int LastLoggedPercent = -1;
static char LastLoggedAction[96];

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
    const char *safe_action = action != NULL ? action : "";
    const char *safe_detail = detail != NULL ? detail : "";
    char normalized_detail[192];

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    /* card_image.c historically used one detail string for both formats and
     * claimed that OPL .vmc verification was checking .ps2 ECC records. Keep
     * the progress UI format-neutral here until the image engine owns separate
     * per-format status text. .ps2 ECC validation still happens in the verifier. */
    if (strcmp(safe_action, "Verifying memory-card image") == 0 &&
        strcmp(safe_detail,
               "Reading the image back and validating logical data plus .ps2 ECC records.") == 0) {
        safe_detail = "Reading the image back and validating the complete logical image stream.";
    }

    /* The image engine's old `page -> OPL .vmc` wording looked like a target
     * filename even though the text after the arrow was only a format label.
     * Make that distinction explicit until card_image.c exposes report->path to
     * the live progress renderer. */
    if (strcmp(safe_action, "Creating memory-card image") == 0) {
        const char *arrow = strstr(safe_detail, " -> ");
        if (arrow != NULL) {
            unsigned int prefix = (unsigned int)(arrow - safe_detail);
            if (prefix + strlen(arrow + 4) + 12u < sizeof(normalized_detail)) {
                snprintf(normalized_detail, sizeof(normalized_detail),
                         "%.*s | format: %s", (int)prefix, safe_detail, arrow + 4);
                safe_detail = normalized_detail;
            }
        }
    }

    /* The GS can update much more frequently than a crash log needs. Keep one
     * durable line per visible percent/action transition so raw imaging does
     * not turn USB sync traffic into the workload we are trying to diagnose. */
    if ((int)domain != LastLoggedDomain || percent != LastLoggedPercent ||
        strcmp(safe_action, LastLoggedAction) != 0) {
        MciDiagLogPrintf("PROGRESS", "domain=%s percent=%d action=%s | %s",
                         ProgressTitle(domain), percent, safe_action, safe_detail);
        LastLoggedDomain = (int)domain;
        LastLoggedPercent = percent;
        snprintf(LastLoggedAction, sizeof(LastLoggedAction), "%s", safe_action);
    }

    if (!MciGuiReady())
        return;

    MciGuiRenderProgress(ProgressTitle(domain), safe_action, safe_detail, percent,
                         ProgressFooter(domain), MCI_GUI_TONE_INFO);
}
