/* SPDX-License-Identifier: MIT */
/*
 * Persistent recovery support for the 0.4 FMCB installer.
 *
 * A memory-card update cannot be power-loss atomic, so the transaction stores
 * every replaced destination on the same USB device as the installation
 * package before touching the card. Journal metadata is written into two
 * alternating checksummed slots. A torn write can therefore fall back to the
 * previous valid sequence instead of destroying the only recovery record.
 */

#define NEWLIB_PORT_AWARE

#include <tamtypes.h>
#include <libmc.h>
#include <fileXio_rpc.h>
#include <io_common.h>
#include <delaythread.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fmcb_recovery.h"
#include "progress.h"

#define RECOVERY_MAGIC 0x4D434952u /* MCIR */
#define RECOVERY_VERSION 1u
#define RECOVERY_CHUNK 4096u
#define RECOVERY_DIR "MCI-RECOVERY"

#define JOURNAL_STATE_ACTIVE 1u
#define JOURNAL_STATE_ROLLING_BACK 2u
#define JOURNAL_STATE_COMMITTED 3u

typedef struct RecoveryEntry {
    u32 manifest_index;
    u32 prepared;
    u32 existed;
    u32 backup_size;
    u32 backup_checksum;
    char destination[FMCB_PATH_MAX];
    char backup_name[24];
} RecoveryEntry;

typedef struct RecoveryJournal {
    u32 magic;
    u32 version;
    u32 struct_size;
    u32 sequence;
    u32 state;
    s32 target_port;
    u32 entry_count;
    u32 created_system_dir;
    u32 created_sysconf_dir;
    u32 checksum;
    char system_dir[48];
    RecoveryEntry entries[FMCB_MAX_PACKAGE_ENTRIES];
} RecoveryJournal;

static const char *KnownRoots[] = {
    "mass:/FMCB",
    "mass0:/FMCB",
    "mass1:/FMCB"
};

static int McResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static int CloseCardFile(int fd)
{
    mcClose(fd);
    return McResult();
}

static u32 FnvUpdate(u32 hash, const unsigned char *data, unsigned int size)
{
    unsigned int i;
    for (i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static u32 JournalChecksum(const RecoveryJournal *journal)
{
    RecoveryJournal copy;

    copy = *journal;
    copy.checksum = 0;
    return FnvUpdate(2166136261u, (const unsigned char *)&copy,
                     sizeof(copy));
}

static void BuildPath(char *out, unsigned int size,
                      const char *root, const char *name)
{
    snprintf(out, size, "%s/%s", root, name);
}

static void BestEffortMassSync(const char *source_root)
{
    char device[16];
    const char *colon;
    unsigned int length;

    if (source_root == NULL)
        return;
    colon = strchr(source_root, ':');
    if (colon == NULL)
        return;
    length = (unsigned int)(colon - source_root) + 1u;
    if (length >= sizeof(device))
        return;
    memcpy(device, source_root, length);
    device[length] = '\0';
    (void)fileXioSync(device, 0);
    DelayThread(10000);
}

static int ReadExactFile(const char *path, void *buffer, unsigned int size)
{
    unsigned char *p = (unsigned char *)buffer;
    unsigned char trailing;
    unsigned int done = 0;
    int fd;
    int rc;

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;
    while (done < size) {
        rc = fileXioRead(fd, p + done, (int)(size - done));
        if (rc <= 0) {
            fileXioClose(fd);
            return rc < 0 ? rc : -5100;
        }
        done += (unsigned int)rc;
    }
    rc = fileXioRead(fd, &trailing, 1);
    fileXioClose(fd);
    return rc == 0 ? 0 : (rc < 0 ? rc : -5101);
}

static int WriteExactFile(const char *path, const void *buffer,
                          unsigned int size, const char *source_root)
{
    const unsigned char *p = (const unsigned char *)buffer;
    unsigned int done = 0;
    int fd;
    int rc;

    (void)fileXioRemove(path);
    fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT);
    if (fd < 0)
        return fd;
    while (done < size) {
        rc = fileXioWrite(fd, p + done, (int)(size - done));
        if (rc <= 0) {
            fileXioClose(fd);
            return rc < 0 ? rc : -5102;
        }
        done += (unsigned int)rc;
    }
    rc = fileXioClose(fd);
    if (rc < 0)
        return rc;
    BestEffortMassSync(source_root);
    return 0;
}

static int JournalValid(const RecoveryJournal *journal)
{
    if (journal->magic != RECOVERY_MAGIC ||
        journal->version != RECOVERY_VERSION ||
        journal->struct_size != (u32)sizeof(*journal) ||
        journal->entry_count > FMCB_MAX_PACKAGE_ENTRIES ||
        journal->target_port < 0 || journal->target_port > 1 ||
        (journal->state != JOURNAL_STATE_ACTIVE &&
         journal->state != JOURNAL_STATE_ROLLING_BACK &&
         journal->state != JOURNAL_STATE_COMMITTED))
        return 0;
    return journal->checksum == JournalChecksum(journal);
}

static int ReadJournalSlot(const char *root, int slot,
                           RecoveryJournal *journal)
{
    char path[FMCB_RECOVERY_PATH_MAX + 32];
    iox_stat_t stat;
    int rc;

    snprintf(path, sizeof(path), "%s/journal%d.bin", root, slot);
    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    if (rc < 0)
        return rc;
    if (stat.size != sizeof(*journal))
        return -5110;
    memset(journal, 0, sizeof(*journal));
    rc = ReadExactFile(path, journal, sizeof(*journal));
    if (rc < 0)
        return rc;
    return JournalValid(journal) ? 0 : -5111;
}

static int LoadLatestJournal(const char *root, RecoveryJournal *journal,
                             int *valid_slots, int *present_slots)
{
    RecoveryJournal a;
    RecoveryJournal b;
    char path[FMCB_RECOVERY_PATH_MAX + 32];
    iox_stat_t stat;
    int a_rc;
    int b_rc;
    int present = 0;
    int valid = 0;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    BuildPath(path, sizeof(path), root, "journal0.bin");
    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(path, &stat) >= 0)
        present++;
    BuildPath(path, sizeof(path), root, "journal1.bin");
    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(path, &stat) >= 0)
        present++;

    a_rc = ReadJournalSlot(root, 0, &a);
    b_rc = ReadJournalSlot(root, 1, &b);
    if (a_rc == 0)
        valid++;
    if (b_rc == 0)
        valid++;

    if (present_slots != NULL)
        *present_slots = present;
    if (valid_slots != NULL)
        *valid_slots = valid;
    if (valid == 0)
        return present > 0 ? -5112 : -ENOENT;

    if (a_rc == 0 && b_rc == 0)
        *journal = a.sequence >= b.sequence ? a : b;
    else if (a_rc == 0)
        *journal = a;
    else
        *journal = b;
    return 0;
}

static int SaveJournal(const char *root, const char *source_root,
                       RecoveryJournal *journal)
{
    char path[FMCB_RECOVERY_PATH_MAX + 32];
    RecoveryJournal verify;
    int slot;
    int rc;

    journal->magic = RECOVERY_MAGIC;
    journal->version = RECOVERY_VERSION;
    journal->struct_size = sizeof(*journal);
    journal->sequence++;
    journal->checksum = JournalChecksum(journal);
    slot = (int)(journal->sequence & 1u);
    snprintf(path, sizeof(path), "%s/journal%d.bin", root, slot);

    rc = WriteExactFile(path, journal, sizeof(*journal), source_root);
    if (rc < 0)
        return rc;
    memset(&verify, 0, sizeof(verify));
    rc = ReadJournalSlot(root, slot, &verify);
    if (rc < 0 || verify.sequence != journal->sequence)
        return rc < 0 ? rc : -5113;
    return 0;
}

static int EnsureRecoveryDirectory(const char *root)
{
    iox_stat_t stat;
    int rc;

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(root, &stat);
    if (rc >= 0)
        return 0;
    rc = fileXioMkdir(root, 0777);
    if (rc < 0) {
        memset(&stat, 0, sizeof(stat));
        if (fileXioGetStat(root, &stat) < 0)
            return rc;
    }
    return 0;
}

static int BackupPath(const char *root, unsigned int manifest_index,
                      int temporary, char *path, unsigned int path_size,
                      char *name, unsigned int name_size)
{
    char local[24];

    snprintf(local, sizeof(local), temporary ? "b%02u.tmp" : "b%02u.bin",
             manifest_index);
    if (name != NULL && name_size > 0)
        snprintf(name, name_size, "%s", local);
    BuildPath(path, path_size, root, local);
    return 0;
}

static int MassFileChecksum(const char *path, unsigned int expected_size,
                            u32 *checksum)
{
    unsigned char buffer[RECOVERY_CHUNK] __attribute__((aligned(64)));
    iox_stat_t stat;
    unsigned int total = 0;
    u32 hash = 2166136261u;
    int fd;
    int rc;

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    if (rc < 0)
        return rc;
    if (stat.size != expected_size)
        return -5120;

    fd = fileXioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return fd;
    while (total < expected_size) {
        unsigned int chunk = expected_size - total;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);
        rc = fileXioRead(fd, buffer, (int)chunk);
        if (rc != (int)chunk) {
            fileXioClose(fd);
            return rc < 0 ? rc : -5121;
        }
        hash = FnvUpdate(hash, buffer, chunk);
        total += chunk;
    }
    fileXioClose(fd);
    if (checksum != NULL)
        *checksum = hash;
    return 0;
}

static int CaptureCardFileToMass(int port, const char *card_path,
                                 const char *temp_path,
                                 const char *final_path,
                                 const char *source_root,
                                 unsigned int size, u32 *checksum)
{
    unsigned char buffer[RECOVERY_CHUNK] __attribute__((aligned(64)));
    unsigned int total = 0;
    u32 hash = 2166136261u;
    u32 verify_hash = 0;
    int card_fd;
    int mass_fd;
    int rc;

    mcOpen(port, 0, card_path, FIO_O_RDONLY);
    card_fd = McResult();
    if (card_fd < 0)
        return card_fd;

    (void)fileXioRemove(temp_path);
    mass_fd = fileXioOpen(temp_path, FIO_O_WRONLY | FIO_O_CREAT);
    if (mass_fd < 0) {
        CloseCardFile(card_fd);
        return mass_fd;
    }

    while (total < size) {
        unsigned int chunk = size - total;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);
        mcRead(card_fd, buffer, chunk);
        rc = McResult();
        if (rc != (int)chunk) {
            fileXioClose(mass_fd);
            CloseCardFile(card_fd);
            (void)fileXioRemove(temp_path);
            return rc < 0 ? rc : -5122;
        }
        rc = fileXioWrite(mass_fd, buffer, (int)chunk);
        if (rc != (int)chunk) {
            fileXioClose(mass_fd);
            CloseCardFile(card_fd);
            (void)fileXioRemove(temp_path);
            return rc < 0 ? rc : -5123;
        }
        hash = FnvUpdate(hash, buffer, chunk);
        total += chunk;
    }

    rc = CloseCardFile(card_fd);
    if (rc < 0) {
        fileXioClose(mass_fd);
        (void)fileXioRemove(temp_path);
        return rc;
    }
    rc = fileXioClose(mass_fd);
    if (rc < 0) {
        (void)fileXioRemove(temp_path);
        return rc;
    }
    BestEffortMassSync(source_root);

    rc = MassFileChecksum(temp_path, size, &verify_hash);
    if (rc < 0 || verify_hash != hash) {
        (void)fileXioRemove(temp_path);
        return rc < 0 ? rc : -5124;
    }

    (void)fileXioRemove(final_path);
    rc = fileXioRename(temp_path, final_path);
    if (rc < 0) {
        (void)fileXioRemove(temp_path);
        return rc;
    }
    BestEffortMassSync(source_root);
    if (checksum != NULL)
        *checksum = hash;
    return 0;
}

static int DeleteCardTarget(int port, const char *path)
{
    int rc;
    mcDelete(port, 0, path);
    rc = McResult();
    if (rc == sceMcResNoEntry)
        return 0;
    return rc;
}

static int RestoreMassBackupToCard(int port, const char *mass_path,
                                   const RecoveryEntry *entry)
{
    unsigned char buffer[RECOVERY_CHUNK] __attribute__((aligned(64)));
    unsigned char card_buffer[RECOVERY_CHUNK] __attribute__((aligned(64)));
    unsigned int total = 0;
    u32 hash = 2166136261u;
    int mass_fd;
    int card_fd;
    int rc;

    rc = MassFileChecksum(mass_path, entry->backup_size, &hash);
    if (rc < 0 || hash != entry->backup_checksum)
        return rc < 0 ? rc : -5130;

    rc = DeleteCardTarget(port, entry->destination);
    if (rc < 0)
        return rc;
    mcOpen(port, 0, entry->destination, FIO_O_WRONLY | FIO_O_CREAT);
    card_fd = McResult();
    if (card_fd < 0)
        return card_fd;
    mass_fd = fileXioOpen(mass_path, FIO_O_RDONLY);
    if (mass_fd < 0) {
        CloseCardFile(card_fd);
        return mass_fd;
    }

    while (total < entry->backup_size) {
        unsigned int chunk = entry->backup_size - total;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);
        rc = fileXioRead(mass_fd, buffer, (int)chunk);
        if (rc != (int)chunk) {
            fileXioClose(mass_fd);
            CloseCardFile(card_fd);
            return rc < 0 ? rc : -5131;
        }
        mcWrite(card_fd, buffer, chunk);
        rc = McResult();
        if (rc != (int)chunk) {
            fileXioClose(mass_fd);
            CloseCardFile(card_fd);
            return rc < 0 ? rc : -5132;
        }
        total += chunk;
    }
    fileXioClose(mass_fd);
    mcFlush(card_fd);
    rc = McResult();
    if (rc < 0) {
        CloseCardFile(card_fd);
        return rc;
    }
    rc = CloseCardFile(card_fd);
    if (rc < 0)
        return rc;

    /* Verify the restored card against the persistent backup byte-for-byte. */
    mcOpen(port, 0, entry->destination, FIO_O_RDONLY);
    card_fd = McResult();
    if (card_fd < 0)
        return card_fd;
    mass_fd = fileXioOpen(mass_path, FIO_O_RDONLY);
    if (mass_fd < 0) {
        CloseCardFile(card_fd);
        return mass_fd;
    }
    total = 0;
    while (total < entry->backup_size) {
        unsigned int chunk = entry->backup_size - total;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);
        rc = fileXioRead(mass_fd, buffer, (int)chunk);
        if (rc != (int)chunk) {
            fileXioClose(mass_fd);
            CloseCardFile(card_fd);
            return rc < 0 ? rc : -5133;
        }
        mcRead(card_fd, card_buffer, chunk);
        rc = McResult();
        if (rc != (int)chunk || memcmp(buffer, card_buffer, chunk) != 0) {
            fileXioClose(mass_fd);
            CloseCardFile(card_fd);
            return rc < 0 ? rc : -5134;
        }
        total += chunk;
    }
    fileXioClose(mass_fd);
    return CloseCardFile(card_fd);
}

static void FillStatus(FmcbRecoveryStatus *status, const char *source_root,
                       const char *recovery_root,
                       const RecoveryJournal *journal)
{
    memset(status, 0, sizeof(*status));
    status->present = 1;
    status->valid = 1;
    status->target_port = journal->target_port;
    status->prepared_files = (int)journal->entry_count;
    status->sequence = journal->sequence;
    status->state = journal->state == JOURNAL_STATE_ROLLING_BACK
                        ? FMCB_RECOVERY_ROLLING_BACK
                        : FMCB_RECOVERY_ACTIVE;
    snprintf(status->source_root, sizeof(status->source_root), "%s",
             source_root);
    snprintf(status->recovery_root, sizeof(status->recovery_root), "%s",
             recovery_root);
}

static int RemoveRecoveryFiles(const FmcbRecoveryStatus *status,
                               const RecoveryJournal *journal)
{
    char path[FMCB_RECOVERY_PATH_MAX + 32];
    unsigned int i;

    for (i = 0; i < journal->entry_count; i++) {
        if (journal->entries[i].backup_name[0] != '\0') {
            BuildPath(path, sizeof(path), status->recovery_root,
                      journal->entries[i].backup_name);
            (void)fileXioRemove(path);
        }
        snprintf(path, sizeof(path), "%s/b%02u.tmp", status->recovery_root,
                 journal->entries[i].manifest_index);
        (void)fileXioRemove(path);
    }
    BuildPath(path, sizeof(path), status->recovery_root, "journal0.bin");
    (void)fileXioRemove(path);
    BuildPath(path, sizeof(path), status->recovery_root, "journal1.bin");
    (void)fileXioRemove(path);
    BestEffortMassSync(status->source_root);
    (void)fileXioRmdir(status->recovery_root);
    return 0;
}

int FmcbRecoveryProbe(const FmcbMassBackendStatus *backend,
                      FmcbRecoveryStatus *status)
{
    unsigned int i;

    if (status == NULL)
        return -1;
    memset(status, 0, sizeof(*status));
    status->target_port = -1;
    status->state = FMCB_RECOVERY_NONE;
    status->probe_rc = -ENOENT;
    if (backend == NULL || !backend->available) {
        status->probe_rc = -2;
        return -2;
    }

    for (i = 0; i < sizeof(KnownRoots) / sizeof(KnownRoots[0]); i++) {
        RecoveryJournal journal;
        FmcbRecoveryStatus found;
        char root[FMCB_RECOVERY_PATH_MAX];
        int valid_slots = 0;
        int present_slots = 0;
        int rc;

        snprintf(root, sizeof(root), "%s/%s", KnownRoots[i], RECOVERY_DIR);
        memset(&journal, 0, sizeof(journal));
        rc = LoadLatestJournal(root, &journal, &valid_slots, &present_slots);
        if (rc == 0) {
            FillStatus(&found, KnownRoots[i], root, &journal);
            if (journal.state == JOURNAL_STATE_COMMITTED) {
                /* All card writes were already verified before this state was
                 * published. A power cut during cleanup must not turn a good
                 * installation into an apparent rollback request. */
                RemoveRecoveryFiles(&found, &journal);
                continue;
            }
            *status = found;
            status->probe_rc = 0;
            return 0;
        }
        if (present_slots > 0 && valid_slots == 0) {
            status->present = 1;
            status->valid = 0;
            status->state = FMCB_RECOVERY_CORRUPT;
            status->probe_rc = rc;
            snprintf(status->source_root, sizeof(status->source_root), "%s",
                     KnownRoots[i]);
            snprintf(status->recovery_root, sizeof(status->recovery_root), "%s",
                     root);
            return rc;
        }
    }
    return -ENOENT;
}

int FmcbRecoveryBegin(const FmcbPackageReport *package,
                      FmcbRecoveryStatus *status)
{
    RecoveryJournal existing;
    RecoveryJournal journal;
    char root[FMCB_RECOVERY_PATH_MAX];
    int valid_slots = 0;
    int present_slots = 0;
    int rc;

    if (package == NULL || status == NULL || package->source_root[0] == '\0')
        return -1;
    snprintf(root, sizeof(root), "%s/%s", package->source_root, RECOVERY_DIR);

    memset(&existing, 0, sizeof(existing));
    rc = LoadLatestJournal(root, &existing, &valid_slots, &present_slots);
    if (rc == 0 || present_slots > 0)
        return -5140; /* Explicit recovery/discard is required first. */

    rc = EnsureRecoveryDirectory(root);
    if (rc < 0)
        return rc;

    memset(&journal, 0, sizeof(journal));
    journal.magic = RECOVERY_MAGIC;
    journal.version = RECOVERY_VERSION;
    journal.struct_size = sizeof(journal);
    journal.sequence = 0;
    journal.state = JOURNAL_STATE_ACTIVE;
    journal.target_port = package->plan.target_port;
    rc = SaveJournal(root, package->source_root, &journal);
    if (rc < 0)
        return rc;

    FillStatus(status, package->source_root, root, &journal);
    return 0;
}

int FmcbRecoveryCaptureTarget(FmcbRecoveryStatus *status,
                              int target_port,
                              int manifest_index,
                              const char *destination,
                              int *existed,
                              unsigned int *size)
{
    RecoveryJournal journal;
    RecoveryEntry *entry;
    sceMcTblGetDir info __attribute__((aligned(64)));
    char temp_path[FMCB_RECOVERY_PATH_MAX + 32];
    char final_path[FMCB_RECOVERY_PATH_MAX + 32];
    u32 checksum = 2166136261u;
    int rc;

    if (status == NULL || !status->valid || destination == NULL ||
        target_port != status->target_port || manifest_index < 0 ||
        manifest_index >= FMCB_MAX_PACKAGE_ENTRIES)
        return -1;
    rc = LoadLatestJournal(status->recovery_root, &journal, NULL, NULL);
    if (rc < 0 || journal.state != JOURNAL_STATE_ACTIVE)
        return rc < 0 ? rc : -5142;
    if (journal.entry_count >= FMCB_MAX_PACKAGE_ENTRIES)
        return -5141;

    memset(&info, 0, sizeof(info));
    mcGetDir(target_port, 0, destination, 0, 1, &info);
    rc = McResult();
    if (rc < 0 && rc != sceMcResNoEntry)
        return rc;

    entry = &journal.entries[journal.entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->manifest_index = (u32)manifest_index;
    entry->existed = rc > 0 ? 1u : 0u;
    entry->backup_size = entry->existed ? info.FileSizeByte : 0u;
    snprintf(entry->destination, sizeof(entry->destination), "%s", destination);

    BackupPath(status->recovery_root, (unsigned int)manifest_index, 1,
               temp_path, sizeof(temp_path), NULL, 0);
    BackupPath(status->recovery_root, (unsigned int)manifest_index, 0,
               final_path, sizeof(final_path), entry->backup_name,
               sizeof(entry->backup_name));

    if (entry->existed && entry->backup_size > 0u) {
        MciProgressUpdate(MCI_PROGRESS_FMCB, 0,
                          "Persisting destination recovery backup",
                          "Copying the original target to USB and verifying it before the card can be modified.");
        rc = CaptureCardFileToMass(target_port, destination, temp_path,
                                   final_path, status->source_root,
                                   entry->backup_size, &checksum);
        if (rc < 0)
            return rc;
        entry->backup_checksum = checksum;
    } else if (entry->existed) {
        entry->backup_checksum = 2166136261u;
    }

    entry->prepared = 1;
    journal.entry_count++;
    rc = SaveJournal(status->recovery_root, status->source_root, &journal);
    if (rc < 0)
        return rc;

    status->prepared_files = (int)journal.entry_count;
    status->sequence = journal.sequence;
    if (existed != NULL)
        *existed = entry->existed != 0u;
    if (size != NULL)
        *size = entry->backup_size;
    return 0;
}

int FmcbRecoveryRecordDirectories(FmcbRecoveryStatus *status,
                                  const char *system_dir,
                                  int created_system_dir,
                                  int created_sysconf_dir)
{
    RecoveryJournal journal;
    int rc;

    if (status == NULL || !status->valid)
        return -1;
    rc = LoadLatestJournal(status->recovery_root, &journal, NULL, NULL);
    if (rc < 0 || journal.state != JOURNAL_STATE_ACTIVE)
        return rc < 0 ? rc : -5142;
    journal.created_system_dir = created_system_dir ? 1u : 0u;
    journal.created_sysconf_dir = created_sysconf_dir ? 1u : 0u;
    if (system_dir != NULL)
        snprintf(journal.system_dir, sizeof(journal.system_dir), "%s",
                 system_dir);
    rc = SaveJournal(status->recovery_root, status->source_root, &journal);
    if (rc == 0)
        status->sequence = journal.sequence;
    return rc;
}

int FmcbRecoveryRun(FmcbRecoveryStatus *status, int *rollback_rc)
{
    RecoveryJournal journal;
    int i;
    int first_error = 0;
    int rc;

    if (rollback_rc != NULL)
        *rollback_rc = 0;
    if (status == NULL || !status->present || !status->valid)
        return -1;

    rc = LoadLatestJournal(status->recovery_root, &journal, NULL, NULL);
    if (rc < 0)
        return rc;
    if (journal.state == JOURNAL_STATE_COMMITTED) {
        RemoveRecoveryFiles(status, &journal);
        memset(status, 0, sizeof(*status));
        status->target_port = -1;
        status->state = FMCB_RECOVERY_NONE;
        return 0;
    }
    journal.state = JOURNAL_STATE_ROLLING_BACK;
    rc = SaveJournal(status->recovery_root, status->source_root, &journal);
    if (rc < 0)
        return rc;
    status->state = FMCB_RECOVERY_ROLLING_BACK;
    status->sequence = journal.sequence;

    MciProgressUpdate(MCI_PROGRESS_FMCB, 3,
                      "Recovering an interrupted FMCB transaction",
                      "Restoring every destination with a durable USB backup. The operation is idempotent and can be re-run after another interruption.");

    for (i = (int)journal.entry_count - 1; i >= 0; i--) {
        const RecoveryEntry *entry = &journal.entries[i];
        char backup_path[FMCB_RECOVERY_PATH_MAX + 32];
        char detail[224];
        int percent = 8 + ((int)journal.entry_count - 1 - i) * 82 /
                           (journal.entry_count > 0 ? (int)journal.entry_count : 1);

        if (!entry->prepared)
            continue;
        snprintf(detail, sizeof(detail),
                 "Restoring mc%d:%s from transaction backup state.",
                 status->target_port, entry->destination);
        MciProgressUpdate(MCI_PROGRESS_FMCB, percent,
                          "Restoring original destination", detail);

        if (!entry->existed) {
            rc = DeleteCardTarget(status->target_port, entry->destination);
        } else if (entry->backup_size == 0u) {
            rc = DeleteCardTarget(status->target_port, entry->destination);
            if (rc >= 0) {
                int fd;
                mcOpen(status->target_port, 0, entry->destination,
                       FIO_O_WRONLY | FIO_O_CREAT);
                fd = McResult();
                rc = fd < 0 ? fd : CloseCardFile(fd);
            }
        } else {
            BuildPath(backup_path, sizeof(backup_path), status->recovery_root,
                      entry->backup_name);
            rc = RestoreMassBackupToCard(status->target_port, backup_path,
                                         entry);
        }
        if (rc < 0 && first_error == 0)
            first_error = rc;
    }

    if (first_error == 0 && journal.created_sysconf_dir) {
        mcDelete(status->target_port, 0, "/SYS-CONF");
        rc = McResult();
        if (rc < 0 && rc != sceMcResNoEntry && rc != sceMcResNotEmpty)
            first_error = rc;
    }
    if (first_error == 0 && journal.created_system_dir &&
        journal.system_dir[0] != '\0') {
        mcDelete(status->target_port, 0, journal.system_dir);
        rc = McResult();
        if (rc < 0 && rc != sceMcResNoEntry && rc != sceMcResNotEmpty)
            first_error = rc;
    }

    if (rollback_rc != NULL)
        *rollback_rc = first_error;
    if (first_error < 0)
        return first_error;

    MciProgressUpdate(MCI_PROGRESS_FMCB, 96,
                      "Recovery verified",
                      "Every prepared destination has been restored or removed according to its original state. Cleaning the USB journal now.");
    RemoveRecoveryFiles(status, &journal);
    memset(status, 0, sizeof(*status));
    status->target_port = -1;
    status->state = FMCB_RECOVERY_NONE;
    MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                      "FMCB recovery complete",
                      "The memory card has been restored to the transaction's captured pre-install state.");
    return 0;
}

int FmcbRecoveryFinish(FmcbRecoveryStatus *status)
{
    RecoveryJournal journal;
    int rc;

    if (status == NULL || !status->valid)
        return -1;
    rc = LoadLatestJournal(status->recovery_root, &journal, NULL, NULL);
    if (rc < 0)
        return rc;

    /* Publish COMMITTED before deleting anything. If power disappears during
     * cleanup, the next probe knows the card already passed every read-back
     * comparison and can finish USB cleanup without rolling the install back. */
    journal.state = JOURNAL_STATE_COMMITTED;
    rc = SaveJournal(status->recovery_root, status->source_root, &journal);
    if (rc < 0)
        return rc;

    RemoveRecoveryFiles(status, &journal);
    memset(status, 0, sizeof(*status));
    status->target_port = -1;
    status->state = FMCB_RECOVERY_NONE;
    return 0;
}

const char *FmcbRecoveryStateText(FmcbRecoveryState state)
{
    switch (state) {
        case FMCB_RECOVERY_ACTIVE: return "INCOMPLETE TRANSACTION";
        case FMCB_RECOVERY_ROLLING_BACK: return "ROLLBACK INCOMPLETE";
        case FMCB_RECOVERY_CORRUPT: return "RECOVERY JOURNAL CORRUPT";
        default: return "NONE";
    }
}
