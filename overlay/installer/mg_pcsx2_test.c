#include <errno.h>
#include <libmc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_inspector.h"
#include "mg_pcsx2_test.h"

typedef enum MGPCSX2Mode {
    MG_PCSX2_FRESH_FORMAT = 0,
    MG_PCSX2_FORMATTED = 1,
    MG_PCSX2_UNFORMATTED_REJECT = 2
} MGPCSX2Mode;

static int SyncCall(void)
{
    int result = 0;
    mcSync(0, NULL, &result);
    return result;
}

static const char *TestName(int v)
{
    if (v == MG_TEST_PASS) return "PASS";
    if (v == MG_TEST_FAIL) return "FAIL";
    if (v == MG_TEST_HARDWARE_ONLY) return "HARDWARE_ONLY";
    return "NOT_RUN";
}

static MGPCSX2Mode ParseMode(int argc, char *argv[])
{
    int i;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--mgci-mode=formatted") == 0)
            return MG_PCSX2_FORMATTED;
        if (strcmp(argv[i], "--mgci-mode=unformatted-reject") == 0)
            return MG_PCSX2_UNFORMATTED_REJECT;
    }
    return MG_PCSX2_FRESH_FORMAT;
}

static int ParsePort(int argc, char *argv[])
{
    int i;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--mgci-port=1") == 0)
            return 1;
    }
    return 0;
}

int MGPCSX2TestRequested(int argc, char *argv[])
{
    int i;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--mgci-pcsx2-test") == 0)
            return 1;
    }
    return 0;
}

static int GetInfo(int port, int slot, int *type, int *free_kb, int *formatted)
{
    int result;
    mcGetInfo(port, slot, type, free_kb, formatted);
    result = SyncCall();
    return result;
}

static int WriteResultJson(const char *phase, const char *status, int rc, const MGInspectorResult *r)
{
    FILE *f = fopen("host:mgci-result.json", "wb");
    if (f == NULL)
        return -errno;

    fprintf(f,
        "{\n"
        "  \"status\": \"%s\",\n"
        "  \"phase\": \"%s\",\n"
        "  \"return_code\": %d,\n"
        "  \"port\": %d,\n"
        "  \"slot\": %d,\n"
        "  \"mc_getinfo_result\": %d,\n"
        "  \"card_type\": %d,\n"
        "  \"formatted\": %d,\n"
        "  \"free_kb\": %d,\n"
        "  \"rw_test\": \"%s\",\n"
        "  \"cleanup_test\": \"%s\",\n"
        "  \"kelf_load_test\": \"%s\",\n"
        "  \"magicgate_bind\": \"%s\",\n"
        "  \"kbit\": \"%s\",\n"
        "  \"kc\": \"%s\",\n"
        "  \"keyblock_changed\": \"%s\",\n"
        "  \"emulator_qualification_pass\": %s,\n"
        "  \"full_pass\": %s\n"
        "}\n",
        status, phase, rc, r->port, r->slot, r->mc_getinfo_result, r->card_type,
        r->formatted, r->free_kb, TestName(r->rw_test), TestName(r->cleanup_test),
        TestName(r->kelf_load_test), TestName(r->mg_bind_test), TestName(r->kbit_nonzero),
        TestName(r->kc_nonzero), TestName(r->keyblock_changed),
        r->emulator_qualification_pass ? "true" : "false",
        r->full_pass ? "true" : "false");

    fclose(f);
    return 0;
}

int MGPCSX2Run(int argc, char *argv[])
{
    MGPCSX2Mode mode = ParseMode(argc, argv);
    MGInspectorResult result;
    int port = ParsePort(argc, argv);
    int slot = 0;
    int type = 0, free_kb = 0, formatted = 0;
    int rc;

    memset(&result, 0, sizeof(result));

    rc = GetInfo(port, slot, &type, &free_kb, &formatted);
    if (rc < -1) {
        result.port = port;
        result.slot = slot;
        result.mc_getinfo_result = rc;
        result.card_type = type;
        result.free_kb = free_kb;
        result.formatted = formatted;
        WriteResultJson("mc_getinfo", "fail", rc, &result);
        return 10;
    }

    if (mode == MG_PCSX2_UNFORMATTED_REJECT) {
        rc = MGInspectorRunEmulator(port, slot, &result);
        WriteResultJson("unformatted-reject", (!formatted && rc == -EINVAL && !result.emulator_qualification_pass && !result.full_pass) ? "pass" : "fail", rc, &result);
        return (!formatted && rc == -EINVAL && !result.emulator_qualification_pass && !result.full_pass) ? 0 : 11;
    }

    if (mode == MG_PCSX2_FRESH_FORMAT) {
        MGInspectorResult reject_result;
        memset(&reject_result, 0, sizeof(reject_result));

        if (formatted) {
            reject_result.port = port;
            reject_result.slot = slot;
            reject_result.mc_getinfo_result = rc;
            reject_result.card_type = type;
            reject_result.free_kb = free_kb;
            reject_result.formatted = formatted;
            WriteResultJson("expected-fresh-but-formatted", "fail", -EINVAL, &reject_result);
            return 12;
        }

        rc = MGInspectorRunEmulator(port, slot, &reject_result);
        if (rc != -EINVAL || reject_result.emulator_qualification_pass || reject_result.full_pass) {
            WriteResultJson("fresh-reject-failed", "fail", rc, &reject_result);
            return 13;
        }

        mcFormat(port, slot);
        rc = SyncCall();
        if (rc < 0) {
            reject_result.mc_getinfo_result = rc;
            WriteResultJson("format-failed", "fail", rc, &reject_result);
            return 14;
        }

        rc = GetInfo(port, slot, &type, &free_kb, &formatted);
        if (rc < -1 || !formatted) {
            reject_result.mc_getinfo_result = rc;
            reject_result.card_type = type;
            reject_result.free_kb = free_kb;
            reject_result.formatted = formatted;
            WriteResultJson("format-verification-failed", "fail", rc, &reject_result);
            return 15;
        }
    } else if (!formatted) {
        result.port = port;
        result.slot = slot;
        result.mc_getinfo_result = rc;
        result.card_type = type;
        result.free_kb = free_kb;
        result.formatted = formatted;
        WriteResultJson("expected-formatted", "fail", -EINVAL, &result);
        return 16;
    }

    rc = MGInspectorRunEmulator(port, slot, &result);
    WriteResultJson("qualified", (rc == 0 && result.emulator_qualification_pass && !result.full_pass) ? "pass" : "fail", rc, &result);

    if (rc != 0)
        return 20;
    if (!result.emulator_qualification_pass)
        return 21;
    if (result.full_pass)
        return 22;
    if (result.rw_test != MG_TEST_PASS || result.cleanup_test != MG_TEST_PASS || result.kelf_load_test != MG_TEST_PASS)
        return 23;
    if (result.mg_bind_test != MG_TEST_HARDWARE_ONLY ||
        result.kbit_nonzero != MG_TEST_HARDWARE_ONLY ||
        result.kc_nonzero != MG_TEST_HARDWARE_ONLY ||
        result.keyblock_changed != MG_TEST_HARDWARE_ONLY)
        return 24;

    return 0;
}
