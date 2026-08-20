#ifndef MG_INSPECTOR_H
#define MG_INSPECTOR_H

#define MG_INSPECTOR_VERSION "0.2.0"

#define MG_TEST_NOT_RUN      (-1)
#define MG_TEST_FAIL           0
#define MG_TEST_PASS           1
#define MG_TEST_HARDWARE_ONLY  2

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

    int cleanup_test;
    int cleanup_error;

    int kelf_load_test;
    int kelf_error;
    int kelf_size;

    int mg_bind_test;
    int mg_error;

    int keyblock_changed;
    int kbit_nonzero;
    int kc_nonzero;

    int emulator_qualification_pass;
    int full_pass;
} MGInspectorResult;

/* Production qualification. MagicGate binding is mandatory. */
int MGInspectorRun(int port, int slot, MGInspectorResult *result);

/*
 * Emulator qualification. This exercises all emulatable card/filesystem/KELF
 * behavior but can never set full_pass or unlock the production force-install
 * path. MagicGate-only fields are marked MG_TEST_HARDWARE_ONLY.
 */
int MGInspectorRunEmulator(int port, int slot, MGInspectorResult *result);

void MGInspectorFormatReport(const MGInspectorResult *result, char *buffer, unsigned int buffer_size);

#endif
