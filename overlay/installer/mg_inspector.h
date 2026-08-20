#ifndef MG_INSPECTOR_H
#define MG_INSPECTOR_H

/* CI source marker: changes under overlay/ intentionally trigger PS2DEV builds. */
#define MG_INSPECTOR_VERSION "0.1.0"

#define MG_TEST_NOT_RUN (-1)
#define MG_TEST_FAIL      0
#define MG_TEST_PASS      1

typedef struct MGInspectorResult {
    int port;
    int slot;

    int mc_getinfo_result;
    int card_type;
    int free_kb;
    int formatted;
    int standard_ps2_type;

    int rw_test;
    int rw_error;

    int kelf_load_test;
    int kelf_error;
    int kelf_size;

    int mg_bind_test;
    int mg_error;

    int keyblock_changed;
    int kbit_nonzero;
    int kc_nonzero;

    int full_pass;
} MGInspectorResult;

/*
 * Runs a non-destructive compatibility test against a memory card.
 * The only card-side write is a temporary 4 KiB file in the card root,
 * which is deleted before the function returns.
 *
 * The MagicGate test loads INSTALL/SYSTEM/FMCB.XLF into RAM and runs the
 * same SecrDownloadFile() path used by the normal FMCB installer. The
 * signed test buffer is never written to the card by this function.
 */
int MGInspectorRun(int port, int slot, MGInspectorResult *result);

/* Render a compact, human-readable report suitable for ShowMessageBox(). */
void MGInspectorFormatReport(const MGInspectorResult *result, char *buffer, unsigned int buffer_size);

#endif
