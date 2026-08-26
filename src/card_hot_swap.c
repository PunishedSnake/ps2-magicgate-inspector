/* SPDX-License-Identifier: MIT */

#include <tamtypes.h>
#include <delaythread.h>
#include <libmc.h>
#include <stdio.h>

#include "card_hot_swap.h"
#include "diag_log.h"

static int ProbeOnce(int port, int *type_out, int *free_out, int *formatted_out)
{
    int type = MC_TYPE_NONE;
    int free_clusters = -1;
    int formatted = 0;
    int result = -999;
    int issue_rc;
    int sync_rc;

    issue_rc = mcGetInfo(port, 0, &type, &free_clusters, &formatted);
    if (issue_rc < 0)
        return issue_rc;
    sync_rc = mcSync(MC_WAIT, NULL, &result);
    if (sync_rc < 0)
        return sync_rc;
    if (sync_rc != 1)
        return -998;
    if (type_out != NULL) *type_out = type;
    if (free_out != NULL) *free_out = free_clusters;
    if (formatted_out != NULL) *formatted_out = formatted;
    return result;
}

int MciNormalCardProbeFormatted(int port, int *free_clusters)
{
    int attempt;
    int type = MC_TYPE_NONE;
    int free_value = -1;
    int formatted = 0;
    int rc = -1;
    int reinit_rc = -999;

    /* A newly inserted card commonly reports ChangedCard first and may expose
     * transitional type/format fields on that pass. Consume a few read-only
     * probes before escalating to a normal XMC client re-init. */
    for (attempt = 0; attempt < 3; attempt++) {
        rc = ProbeOnce(port, &type, &free_value, &formatted);
        MciDiagLogPrintf("CARD-HOTSWAP",
                         "probe port=mc%d attempt=%d rc=%d type=%d free=%d formatted=%d",
                         port, attempt + 1, rc, type, free_value, formatted);
        if (rc >= sceMcResNoFormat && type == MC_TYPE_PS2 && formatted) {
            if (free_clusters != NULL)
                *free_clusters = free_value;
            return 0;
        }
        /* FailDetect/transport errors describe an empty or genuinely missing
         * slot, not a changed-card transition. Do not reset libmc merely because
         * the other physical slot is empty. */
        if (rc < sceMcResNoFormat && rc != sceMcResChangedCard)
            return rc;
        DelayThread(16000);
    }

    /* Rebinding libmc is a last-resort normal-stack refresh, never used inside
     * the raw imaging personality. The mcInit wrapper primes both XMC slots. */
    reinit_rc = mcInit(MC_TYPE_XMC);
    MciDiagLogPrintf("CARD-HOTSWAP",
                     "reinit port=mc%d rc=%d after probe rc=%d type=%d formatted=%d",
                     port, reinit_rc, rc, type, formatted);
    if (reinit_rc < 0)
        return reinit_rc;

    for (attempt = 0; attempt < 3; attempt++) {
        rc = ProbeOnce(port, &type, &free_value, &formatted);
        MciDiagLogPrintf("CARD-HOTSWAP",
                         "post-reinit probe port=mc%d attempt=%d rc=%d type=%d free=%d formatted=%d",
                         port, attempt + 1, rc, type, free_value, formatted);
        if (rc >= sceMcResNoFormat && type == MC_TYPE_PS2 && formatted) {
            if (free_clusters != NULL)
                *free_clusters = free_value;
            return 0;
        }
        if (rc < sceMcResNoFormat && rc != sceMcResChangedCard)
            return rc;
        DelayThread(16000);
    }

    if (rc < sceMcResNoFormat)
        return rc;
    return formatted ? sceMcResFailDetect : sceMcResNoFormat;
}
