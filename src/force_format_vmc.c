/* SPDX-License-Identifier: MIT */

#include <libmc.h>

#include "card_image.h"
#include "force_format_vmc.h"
#include "progress.h"

static int CompleteMcCommand(int issue_rc)
{
    int result = -999;
    int sync_rc;

    if (issue_rc < 0)
        return issue_rc;
    sync_rc = mcSync(MC_WAIT, NULL, &result);
    if (sync_rc < 0)
        return sync_rc;
    if (sync_rc != 1)
        return -998;
    return result;
}

int MciCardForceFormatWithVmcBackup(int port, MciCardImageReport *report)
{
    int rc;

    if (report == NULL)
        return -1;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 1,
                      "Creating pre-format recovery image",
                      "Force format is locked until a complete .vmc logical recovery image is written, reopened and CRC-verified on USB.");
    rc = MciCardImageExport(port, MCI_CARD_IMAGE_VMC, report);
    if (rc < 0 || !report->verified)
        return rc < 0 ? rc : -30;

    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 98,
                      "Formatting memory card",
                      "The verified VMC recovery image is complete. Formatting the selected PS2 card now.");
    rc = CompleteMcCommand(mcFormat(port, 0));
    report->format_rc = rc;
    if (rc < 0) {
        report->result = MCI_CARD_IMAGE_FORMAT_ERROR;
        return rc;
    }

    report->result = MCI_CARD_IMAGE_OK;
    MciProgressUpdate(MCI_PROGRESS_CARD_TOOLS, 100,
                      "Force format complete",
                      "The card was formatted only after its verified .vmc recovery image had been secured on USB.");
    return 0;
}
