/* SPDX-License-Identifier: MIT */
/*
 * Live progress adapter for the native GS frontend.
 *
 * Core diagnostic modules report short, factual progress events through
 * progress.h. This file is the only place that translates those events into
 * GUI wording, footer safety notes and the visible progress bar. The bar is
 * intentionally built from the existing GS font path, so it survives the same
 * IOP resets as the rest of the frontend and needs no second renderer state.
 */

#include <stdio.h>
#include <string.h>

#include "gui.h"
#include "progress.h"

#define PROGRESS_BAR_CELLS 48

static const char *ProgressTitle(MciProgressDomain domain)
{
    switch (domain) {
        case MCI_PROGRESS_FILESYSTEM: return "Filesystem inspection";
        case MCI_PROGRESS_MAGICGATE: return "MagicGate / KELF probe";
        case MCI_PROGRESS_FMCB: return "FMCB package preflight";
        case MCI_PROGRESS_ENVIRONMENT: return "IOP / card environment";
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
            return "Read-only package scan; no FMCB files are written to the memory card.";
        case MCI_PROGRESS_ENVIRONMENT:
            return "Please wait while the program changes or restores the IOP environment.";
        default:
            return "Please wait.";
    }
}

void MciProgressUpdate(MciProgressDomain domain,
                       int percent,
                       const char *action,
                       const char *detail)
{
    char bar[PROGRESS_BAR_CELLS + 1];
    char body[768];
    int filled;
    int i;

    if (!MciGuiReady())
        return;

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    filled = (percent * PROGRESS_BAR_CELLS + 50) / 100;
    for (i = 0; i < PROGRESS_BAR_CELLS; i++)
        bar[i] = i < filled ? '=' : '-';
    bar[PROGRESS_BAR_CELLS] = '\0';

    snprintf(body, sizeof(body),
             "%s\n"
             "%s\n\n"
             "PROGRESS  %3d%%\n"
             "[%s]",
             action != NULL && action[0] != '\0' ? action : "Working...",
             detail != NULL ? detail : "",
             percent,
             bar);

    MciGuiRenderMessage(ProgressTitle(domain), body,
                        ProgressFooter(domain),
                        percent >= 100 ? MCI_GUI_TONE_SUCCESS
                                       : MCI_GUI_TONE_INFO);
}
