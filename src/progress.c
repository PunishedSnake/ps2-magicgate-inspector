/* SPDX-License-Identifier: MIT */
/*
 * Live progress adapter for the native GS frontend.
 *
 * Core diagnostic modules report short, factual progress events through
 * progress.h. This file is the only place that translates those events into
 * GUI titles, footer safety notes and tone. The actual progress bar is drawn
 * by gui.c through the same double-buffered GS renderer as the dashboards.
 */

#include "gui.h"
#include "progress.h"

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
    MciGuiTone tone;

    if (!MciGuiReady())
        return;

    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    tone = percent >= 100 ? MCI_GUI_TONE_SUCCESS : MCI_GUI_TONE_INFO;
    MciGuiRenderProgress(ProgressTitle(domain), action, detail, percent,
                         ProgressFooter(domain), tone);
}
