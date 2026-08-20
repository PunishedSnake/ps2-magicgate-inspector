#include <errno.h>
#include <kernel.h>
#include <libmc.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/fcntl.h>

#include <libsecr-common.h>

#include "libsecr.h"
#include "main.h"
#include "mg_inspector.h"

#define MG_RW_TEST_SIZE 4096
#define MG_MAX_KELF_SIZE (8 * 1024 * 1024)

static unsigned char MgWriteBuffer[MG_RW_TEST_SIZE] ALIGNED(64);
static unsigned char MgReadBuffer[MG_RW_TEST_SIZE] ALIGNED(64);

static int BufferIsAllZero(const unsigned char *buffer, unsigned int size)
{
    unsigned int i;

    for (i = 0; i < size; i++) {
        if (buffer[i] != 0)
            return 0;
    }

    return 1;
}

static void FillTestPattern(unsigned char *buffer, unsigned int size)
{
    unsigned int i;
    unsigned int state = 0x4D474349; /* "MGCI" */

    for (i = 0; i < size; i++) {
        state = state * 1664525u + 1013904223u;
        buffer[i] = (unsigned char)((state >> 24) ^ i ^ (i >> 3));
    }
}

static int CloseMcFile(int fd)
{
    int result;

    mcClose(fd);
    mcSync(0, NULL, &result);
    return result;
}

static void DeleteTestFile(int port, int slot, const char *name)
{
    int result;

    mcDelete(port, slot, name);
    mcSync(0, NULL, &result);
}

static int FindUnusedTestFile(int port, int slot, char *name, unsigned int name_size)
{
    sceMcTblGetDir dirent;
    int i, result;

    for (i = 0; i < 100; i++) {
        snprintf(name, name_size, "__MGCI%02d.TMP", i);
        memset(&dirent, 0, sizeof(dirent));
        mcGetDir(port, slot, name, 0, 1, &dirent);
        mcSync(0, NULL, &result);

        if (result == 0)
            return 0; /* No matching entry: safe temporary filename. */
        if (result < 0)
            return result;
    }

    return -EEXIST;
}

static int RunReadWriteTest(int port, int slot)
{
    char test_file[20];
    int fd, result;

    FillTestPattern(MgWriteBuffer, sizeof(MgWriteBuffer));
    memset(MgReadBuffer, 0, sizeof(MgReadBuffer));

    result = FindUnusedTestFile(port, slot, test_file, sizeof(test_file));
    if (result < 0)
        return result;

    mcOpen(port, slot, test_file, O_WRONLY | O_CREAT | O_TRUNC);
    mcSync(0, NULL, &fd);
    if (fd < 0)
        return fd;

    mcWrite(fd, MgWriteBuffer, sizeof(MgWriteBuffer));
    mcSync(0, NULL, &result);
    if (result != (int)sizeof(MgWriteBuffer)) {
        CloseMcFile(fd);
        DeleteTestFile(port, slot, test_file);
        return (result < 0) ? result : -EIO;
    }

    result = CloseMcFile(fd);
    if (result < 0) {
        DeleteTestFile(port, slot, test_file);
        return result;
    }

    mcOpen(port, slot, test_file, O_RDONLY);
    mcSync(0, NULL, &fd);
    if (fd < 0) {
        DeleteTestFile(port, slot, test_file);
        return fd;
    }

    mcRead(fd, MgReadBuffer, sizeof(MgReadBuffer));
    mcSync(0, NULL, &result);
    if (result != (int)sizeof(MgReadBuffer)) {
        CloseMcFile(fd);
        DeleteTestFile(port, slot, test_file);
        return (result < 0) ? result : -EIO;
    }

    result = CloseMcFile(fd);
    DeleteTestFile(port, slot, test_file);
    if (result < 0)
        return result;

    return (memcmp(MgWriteBuffer, MgReadBuffer, sizeof(MgWriteBuffer)) == 0) ? 0 : -EIO;
}

static int GetKelfKeyOffset(const void *buffer, unsigned int size, unsigned int *key_offset)
{
    const SecrKELFHeader_t *header;
    unsigned int flags;
    unsigned int offset;
    unsigned int bit_table_size;

    if (buffer == NULL || key_offset == NULL || size < sizeof(SecrKELFHeader_t))
        return -EINVAL;

    header = (const SecrKELFHeader_t *)buffer;
    flags = *(const unsigned int *)&header->flags;
    offset = sizeof(SecrKELFHeader_t);

    bit_table_size = (unsigned int)header->BIT_count * sizeof(SecrBitBlockData_t);
    if (bit_table_size > size || offset > size - bit_table_size)
        return -EINVAL;
    offset += bit_table_size;

    if (flags & 1) {
        unsigned int extra;

        if (offset >= size)
            return -EINVAL;
        extra = ((const unsigned char *)buffer)[offset] + 1;
        if (extra > size || offset > size - extra)
            return -EINVAL;
        offset += extra;
    }

    if ((flags & 0xF000) == 0) {
        if (offset > size - 8)
            return -EINVAL;
        offset += 8;
    }

    if (offset > size - 32)
        return -EINVAL;

    *key_offset = offset;
    return 0;
}

static int LoadTestKelf(void **buffer_out, int *size_out)
{
    char path[320];
    char cwd[256];
    FILE *file;
    void *buffer;
    long size;
    int result;

    *buffer_out = NULL;
    *size_out = 0;

    if (getcwd(cwd, sizeof(cwd) - 1) == NULL)
        return -errno;

    if ((strlen(cwd) + strlen("INSTALL/SYSTEM/FMCB.XLF") + 2) >= sizeof(path))
        return -EINVAL;

    snprintf(path, sizeof(path), "%s%sINSTALL/SYSTEM/FMCB.XLF", cwd,
             (cwd[0] != '\0' && cwd[strlen(cwd) - 1] == '/') ? "" : "/");

    file = fopen(path, "rb");
    if (file == NULL)
        return -errno;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    rewind(file);

    if (size <= 0 || size > MG_MAX_KELF_SIZE) {
        fclose(file);
        return -EINVAL;
    }

    buffer = memalign(64, ((unsigned int)size + 63) & ~63);
    if (buffer == NULL) {
        fclose(file);
        return -ENOMEM;
    }

    result = 0;
    if (fread(buffer, 1, size, file) != (unsigned int)size) {
        free(buffer);
        buffer = NULL;
        result = -EIO;
    }

    fclose(file);

    if (result == 0) {
        *buffer_out = buffer;
        *size_out = (int)size;
    }

    return result;
}

int MGInspectorRun(int port, int slot, MGInspectorResult *out)
{
    void *kelf_buffer;
    unsigned char keyblock_before[32];
    unsigned int key_offset;
    int result;
    int rw_result;
    int kelf_result;

    if (out == NULL)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->port = port;
    out->slot = slot;
    out->rw_test = MG_TEST_NOT_RUN;
    out->kelf_load_test = MG_TEST_NOT_RUN;
    out->mg_bind_test = MG_TEST_NOT_RUN;
    out->keyblock_changed = MG_TEST_NOT_RUN;
    out->kbit_nonzero = MG_TEST_NOT_RUN;
    out->kc_nonzero = MG_TEST_NOT_RUN;

    mcGetInfo(port, slot, &out->card_type, &out->free_kb, &out->formatted);
    mcSync(0, NULL, &result);
    out->mc_getinfo_result = result;
    out->standard_ps2_type = (out->card_type == MC_TYPE_PS2);

    /* sceMcResChangedCard (-1) is still a usable response. */
    if (result < -1)
        return result;

    if (!out->formatted)
        return -EINVAL;

    rw_result = RunReadWriteTest(port, slot);
    out->rw_error = rw_result;
    out->rw_test = (rw_result == 0) ? MG_TEST_PASS : MG_TEST_FAIL;
    if (rw_result < 0)
        return rw_result;

    kelf_result = LoadTestKelf(&kelf_buffer, &out->kelf_size);
    out->kelf_error = kelf_result;
    out->kelf_load_test = (kelf_result == 0) ? MG_TEST_PASS : MG_TEST_FAIL;
    if (kelf_result < 0)
        return kelf_result;

    if (GetKelfKeyOffset(kelf_buffer, out->kelf_size, &key_offset) == 0)
        memcpy(keyblock_before, (unsigned char *)kelf_buffer + key_offset, sizeof(keyblock_before));
    else {
        key_offset = 0;
        memset(keyblock_before, 0, sizeof(keyblock_before));
    }

    /*
     * This is the decisive test: use the same card-binding operation as the
     * installer itself. 2 + port matches FreeMcBoot's SignKELF().
     */
    if (SecrDownloadFile(2 + port, slot, kelf_buffer) != NULL) {
        out->mg_bind_test = MG_TEST_PASS;
        out->mg_error = 0;

        if (key_offset != 0 && GetKelfKeyOffset(kelf_buffer, out->kelf_size, &key_offset) == 0) {
            const unsigned char *keyblock = (const unsigned char *)kelf_buffer + key_offset;

            out->keyblock_changed = (memcmp(keyblock_before, keyblock, 32) != 0) ? MG_TEST_PASS : MG_TEST_FAIL;
            out->kbit_nonzero = BufferIsAllZero(keyblock, 16) ? MG_TEST_FAIL : MG_TEST_PASS;
            out->kc_nonzero = BufferIsAllZero(keyblock + 16, 16) ? MG_TEST_FAIL : MG_TEST_PASS;
        }
    } else {
        out->mg_bind_test = MG_TEST_FAIL;
        out->mg_error = -EINVAL;
    }

    free(kelf_buffer);

    out->full_pass = (out->rw_test == MG_TEST_PASS &&
                      out->kelf_load_test == MG_TEST_PASS &&
                      out->mg_bind_test == MG_TEST_PASS);

    return out->full_pass ? 0 : -EINVAL;
}

static const char *PassFail(int value)
{
    if (value == MG_TEST_PASS)
        return "PASS";
    if (value == MG_TEST_FAIL)
        return "FAIL";
    return "N/A";
}

void MGInspectorFormatReport(const MGInspectorResult *r, char *buffer, unsigned int buffer_size)
{
    const char *verdict;

    if (buffer == NULL || buffer_size == 0)
        return;

    if (r == NULL) {
        snprintf(buffer, buffer_size, "MagicGate Inspector: no result.");
        return;
    }

    verdict = r->full_pass ? "FULL PASS - FORCE INSTALL ALLOWED" : "FAIL - FORCE INSTALL LOCKED";

    snprintf(buffer, buffer_size,
             "MagicGate Card Inspector v%s\n\n"
             "Target: mc%d: (slot %d)\n"
             "mcGetInfo result: %d\n"
             "Reported type: %d%s\n"
             "Formatted: %s\n"
             "Free space: %d KB\n\n"
             "Filesystem R/W: %s (rc=%d)\n"
             "FMCB.XLF load: %s (rc=%d)\n"
             "MagicGate KELF bind: %s (rc=%d)\n"
             "Kbit non-zero: %s\n"
             "Kc non-zero: %s\n"
             "Key block changed: %s\n\n"
             "%s",
             MG_INSPECTOR_VERSION,
             r->port,
             r->port + 1,
             r->mc_getinfo_result,
             r->card_type,
             r->standard_ps2_type ? " (standard PS2 type)" : " (NON-STANDARD TYPE)",
             r->formatted ? "yes" : "no",
             r->free_kb,
             PassFail(r->rw_test), r->rw_error,
             PassFail(r->kelf_load_test), r->kelf_error,
             PassFail(r->mg_bind_test), r->mg_error,
             PassFail(r->kbit_nonzero),
             PassFail(r->kc_nonzero),
             PassFail(r->keyblock_changed),
             verdict);
}
