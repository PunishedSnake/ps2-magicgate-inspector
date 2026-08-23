from pathlib import Path
import re


def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise SystemExit(f"{label}: expected one anchor, found {text.count(old)}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# USB discovery: bounded recursion, readiness wait and verified package cache.
# ---------------------------------------------------------------------------
Path("src/usb_search.h").write_text(r'''#ifndef MCI_USB_SEARCH_H
#define MCI_USB_SEARCH_H

#define MCI_USB_SEARCH_PATH_MAX 192
#define MCI_USB_SEARCH_MAX_DEPTH 8
#define MCI_USB_SEARCH_MAX_DIRS 384

typedef void (*MciUsbSearchProgress)(const char *path,
                                     unsigned int directories_scanned,
                                     void *userdata);

/* Wait until at least one mass: root can actually be opened. This is used
 * after an IOP rebuild, when USBHDFSD may be resident slightly before the FAT
 * volume is ready for directory walking. */
int MciUsbWaitForStorage(unsigned int attempts, unsigned int delay_usec);

/* Recursively search mass:/, mass0:/ and mass1:/ for FMCB.XLF.
 * If require_system_parent is non-zero, only .../SYSTEM/FMCB.XLF matches.
 * The walk is bounded by depth and directory-count limits. Dot-prefixed
 * directories are ignored because they are not valid FMCB package locations
 * and some USB/FAT combinations expose transient pseudo entries there. */
int MciUsbFindFmcbXlf(char *out_path, unsigned int out_size,
                      int require_system_parent,
                      MciUsbSearchProgress progress, void *userdata);

/* Convert .../SYSTEM/FMCB.XLF into the package root used by the manifest. */
int MciUsbPackageRootFromXlf(const char *xlf_path,
                             char *out_root, unsigned int out_size);

/* In-process cache for a package root that has already passed the complete
 * FMCB manifest preflight. The cache is never populated from a lone XLF. A
 * read verifies that SYSTEM/FMCB.XLF still exists; normal preflight then
 * rechecks the entire manifest without repeating the recursive tree walk. */
int MciUsbGetVerifiedPackageRoot(char *out_root, unsigned int out_size);
int MciUsbRememberVerifiedPackageRoot(const char *root);
void MciUsbClearVerifiedPackageRoot(void);

#endif /* MCI_USB_SEARCH_H */
''')

Path("src/usb_search.c").write_text(r'''/* SPDX-License-Identifier: MIT */
/* Bounded recursive FMCB source discovery for USB mass storage. */

#define NEWLIB_PORT_AWARE

#include <delaythread.h>
#include <fileXio_rpc.h>
#include <iox_stat.h>
#include <stdio.h>
#include <string.h>

#include "usb_search.h"

static const char *const SearchRoots[] = {
    "mass:/",
    "mass0:/",
    "mass1:/"
};

static char VerifiedPackageRoot[MCI_USB_SEARCH_PATH_MAX];

typedef struct SearchState {
    const char *filename;
    int require_system_parent;
    char *out_path;
    unsigned int out_size;
    unsigned int dirs_scanned;
    MciUsbSearchProgress progress;
    void *userdata;
} SearchState;

static int ascii_lower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static int name_equal_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *last_component(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static int parent_is_system(const char *directory)
{
    return name_equal_ci(last_component(directory), "SYSTEM");
}

static int entry_name_safe(const char *name, int is_directory)
{
    const unsigned char *p = (const unsigned char *)name;

    if (name == NULL || name[0] == '\0')
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;

    /* FMCB package discovery has no reason to enter hidden/pseudo directories.
     * In particular, this prevents malformed USBHDFSD/FAT directory aliases
     * such as repeated `.as/.as/...` from turning into a fake recursive tree. */
    if (is_directory && name[0] == '.')
        return 0;

    while (*p != '\0') {
        if (*p < 0x20u || *p == '/' || *p == '\\' || *p == ':')
            return 0;
        p++;
    }
    return 1;
}

static int join_path(const char *directory, const char *name,
                     char *out, unsigned int out_size)
{
    unsigned int len;
    int written;

    if (directory == NULL || name == NULL || out == NULL || out_size == 0)
        return -1;
    len = (unsigned int)strlen(directory);
    written = snprintf(out, out_size, "%s%s%s", directory,
                       len > 0 && directory[len - 1] == '/' ? "" : "/", name);
    return (written >= 0 && (unsigned int)written < out_size) ? 0 : -2;
}

int MciUsbWaitForStorage(unsigned int attempts, unsigned int delay_usec)
{
    unsigned int attempt;
    unsigned int i;

    if (attempts == 0u)
        attempts = 1u;
    for (attempt = 0; attempt < attempts; attempt++) {
        for (i = 0; i < sizeof(SearchRoots) / sizeof(SearchRoots[0]); i++) {
            int fd = fileXioDopen(SearchRoots[i]);
            if (fd >= 0) {
                fileXioDclose(fd);
                return 0;
            }
        }
        if (attempt + 1u < attempts && delay_usec > 0u)
            DelayThread(delay_usec);
    }
    return -1;
}

static int search_directory(const char *directory, unsigned int depth,
                            SearchState *state)
{
    iox_dirent_t entry;
    char child[MCI_USB_SEARCH_PATH_MAX];
    int fd;
    int rc;

    if (depth > MCI_USB_SEARCH_MAX_DEPTH ||
        state->dirs_scanned >= MCI_USB_SEARCH_MAX_DIRS)
        return 0;

    fd = fileXioDopen(directory);
    if (fd < 0)
        return 0;

    state->dirs_scanned++;
    if (state->progress != NULL &&
        (state->dirs_scanned == 1u || (state->dirs_scanned & 7u) == 0u))
        state->progress(directory, state->dirs_scanned, state->userdata);

    for (;;) {
        int is_directory;
        int is_regular;

        memset(&entry, 0, sizeof(entry));
        rc = fileXioDread(fd, &entry);
        if (rc <= 0)
            break;
        entry.name[sizeof(entry.name) - 1] = '\0';
        is_directory = FIO_S_ISDIR(entry.stat.mode);
        is_regular = FIO_S_ISREG(entry.stat.mode);
        if (!entry_name_safe(entry.name, is_directory))
            continue;
        if (join_path(directory, entry.name, child, sizeof(child)) < 0)
            continue;

        if (is_regular && name_equal_ci(entry.name, state->filename)) {
            if (!state->require_system_parent || parent_is_system(directory)) {
                snprintf(state->out_path, state->out_size, "%s", child);
                fileXioDclose(fd);
                return 1;
            }
        } else if (is_directory && depth < MCI_USB_SEARCH_MAX_DEPTH &&
                   state->dirs_scanned < MCI_USB_SEARCH_MAX_DIRS) {
            rc = search_directory(child, depth + 1u, state);
            if (rc == 1) {
                fileXioDclose(fd);
                return 1;
            }
        }
    }

    fileXioDclose(fd);
    return 0;
}

int MciUsbFindFmcbXlf(char *out_path, unsigned int out_size,
                      int require_system_parent,
                      MciUsbSearchProgress progress, void *userdata)
{
    SearchState state;
    unsigned int i;

    if (out_path == NULL || out_size < 16u)
        return -1;
    out_path[0] = '\0';

    memset(&state, 0, sizeof(state));
    state.filename = "FMCB.XLF";
    state.require_system_parent = require_system_parent;
    state.out_path = out_path;
    state.out_size = out_size;
    state.progress = progress;
    state.userdata = userdata;

    for (i = 0; i < sizeof(SearchRoots) / sizeof(SearchRoots[0]); i++) {
        if (search_directory(SearchRoots[i], 0, &state) == 1)
            return 0;
        if (state.dirs_scanned >= MCI_USB_SEARCH_MAX_DIRS)
            break;
    }
    return -2;
}

int MciUsbPackageRootFromXlf(const char *xlf_path,
                             char *out_root, unsigned int out_size)
{
    char temp[MCI_USB_SEARCH_PATH_MAX];
    char *slash;
    char *system_component;

    if (xlf_path == NULL || out_root == NULL || out_size == 0)
        return -1;
    if (strlen(xlf_path) >= sizeof(temp))
        return -2;
    snprintf(temp, sizeof(temp), "%s", xlf_path);

    slash = strrchr(temp, '/');
    if (slash == NULL || !name_equal_ci(slash + 1, "FMCB.XLF"))
        return -3;
    *slash = '\0';
    system_component = (char *)last_component(temp);
    if (!name_equal_ci(system_component, "SYSTEM"))
        return -4;

    slash = strrchr(temp, '/');
    if (slash == NULL)
        return -5;
    *slash = '\0';
    if (temp[0] == '\0')
        return -6;
    if (snprintf(out_root, out_size, "%s", temp) < 0 ||
        strlen(temp) >= out_size)
        return -7;
    return 0;
}

void MciUsbClearVerifiedPackageRoot(void)
{
    VerifiedPackageRoot[0] = '\0';
}

int MciUsbRememberVerifiedPackageRoot(const char *root)
{
    if (root == NULL || root[0] == '\0' ||
        strlen(root) >= sizeof(VerifiedPackageRoot))
        return -1;
    snprintf(VerifiedPackageRoot, sizeof(VerifiedPackageRoot), "%s", root);
    return 0;
}

int MciUsbGetVerifiedPackageRoot(char *out_root, unsigned int out_size)
{
    iox_stat_t stat;
    char xlf[MCI_USB_SEARCH_PATH_MAX];

    if (out_root == NULL || out_size == 0 || VerifiedPackageRoot[0] == '\0')
        return -1;
    if (join_path(VerifiedPackageRoot, "SYSTEM/FMCB.XLF", xlf,
                  sizeof(xlf)) < 0) {
        MciUsbClearVerifiedPackageRoot();
        return -2;
    }
    memset(&stat, 0, sizeof(stat));
    if (fileXioGetStat(xlf, &stat) < 0 || !FIO_S_ISREG(stat.mode) ||
        stat.size == 0u) {
        MciUsbClearVerifiedPackageRoot();
        return -3;
    }
    if (strlen(VerifiedPackageRoot) >= out_size)
        return -4;
    snprintf(out_root, out_size, "%s", VerifiedPackageRoot);
    return 0;
}
''')


# ---------------------------------------------------------------------------
# FMCB preflight: profile console first, wait for storage, prefer verified cache.
# ---------------------------------------------------------------------------
p = Path("src/fmcb_install.c")
s = p.read_text()
pattern = re.compile(r'int FmcbProbeMassPackage\(int target_port, const FmcbMassBackendStatus \*backend,\n                         FmcbPackageReport \*report\)\n\{.*?\n\}\n\nconst char \*FmcbPackageStatusText', re.S)
replacement = r'''int FmcbProbeMassPackage(int target_port, const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report)
{
    MciConsoleProfile console;
    char xlf_path[MCI_USB_SEARCH_PATH_MAX];
    char package_root[FMCB_SOURCE_ROOT_MAX];
    char detail[256];
    int profile_rc = -1;
    int rc;
    int attempt;

    FmcbResetPackageReport(report, target_port);

    /* Resolve the console before package discovery so the dashboard never
     * reports `Region ?` merely because USB enumeration or a package search
     * failed. A freshly rebuilt IOP can need one short retry before ROM/CDVD
     * services are completely settled. */
    MciProgressUpdate(MCI_PROGRESS_FMCB, 3, "Identifying this PS2",
                      "Reading the active system region and console security profile before looking for FreeMcBoot files.");
    for (attempt = 0; attempt < 3; attempt++) {
        profile_rc = MciConsoleProfileProbe(&console);
        if (profile_rc == 0)
            break;
        if (attempt < 2)
            DelayThread(100000);
    }
    FmcbBuildInstallPlan(target_port, &console, &report->plan);
    report->entry_count = FmcbPackageEntryCount();

    if (profile_rc < 0 || console.mg_folder_region == '?' || console.is_psx) {
        report->status = FMCB_PACKAGE_UNSUPPORTED_CONSOLE;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "Console region could not be resolved",
                          "The active PS2 system region is still unavailable, so installation remains blocked. No card writes were attempted.");
        return -1;
    }
    if (console.region_mismatch) {
        report->status = FMCB_PACKAGE_REGION_AMBIGUOUS;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "MechaPwn region transition is not settled",
                          "The detected Deckard DEX-like policy expects the A system-update region, but active ROMVER has not converged to it. Reboot the console before installing.");
        return -1;
    }
    if (console.cross_region_required) {
        report->status = FMCB_PACKAGE_CROSS_REGION_REQUIRED;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100,
                          "Deckard MechaPwn CEX needs cross-region FMCB",
                          "A one-region install could stop booting after a later MechaPwn CEX region change. This build blocks writes until the verified transaction covers every regional destination.");
        return -1;
    }

    MciProgressUpdate(MCI_PROGRESS_FMCB, 6, "Checking USB storage",
                      "Waiting for the USB filesystem to become readable. This can take a moment after a MagicGate security-session reboot.");
    if (backend == NULL || !backend->available ||
        MciUsbWaitForStorage(20u, 100000u) < 0) {
        report->status = FMCB_PACKAGE_SOURCE_UNAVAILABLE;
        report->source_probe_rc = -1;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100, "USB storage is not available",
                          "Connect a readable USB drive containing the FreeMcBoot package, then run preflight again.");
        return -1;
    }

    /* A cache entry exists only after the complete manifest has passed once.
     * Reuse its root to avoid a full tree walk, but re-run ProbeRoot so moved,
     * deleted or modified package files are still caught before installation. */
    if (MciUsbGetVerifiedPackageRoot(package_root, sizeof(package_root)) == 0) {
        snprintf(detail, sizeof(detail),
                 "Using the previously verified package at %.170s and rechecking its files.",
                 package_root);
        MciProgressUpdate(MCI_PROGRESS_FMCB, 10,
                          "Using cached FreeMcBoot package", detail);
        report->source_probe_rc = 0;
        rc = ProbeRoot(package_root, target_port, report);
        if (rc == 0 && report->status == FMCB_PACKAGE_READY)
            return 0;
        MciUsbClearVerifiedPackageRoot();
        FmcbResetPackageReport(report, target_port);
        FmcbBuildInstallPlan(target_port, &console, &report->plan);
        report->entry_count = FmcbPackageEntryCount();
    }

    MciProgressUpdate(MCI_PROGRESS_FMCB, 9,
                      "Searching USB storage for a FreeMcBoot package",
                      "Looking recursively for SYSTEM/FMCB.XLF. The package folder may be placed anywhere in visible USB folders.");
    rc = MciUsbFindFmcbXlf(xlf_path, sizeof(xlf_path), 1,
                           PackageSearchProgress, NULL);
    report->source_probe_rc = rc;
    if (rc == 0) {
        rc = MciUsbPackageRootFromXlf(xlf_path, package_root,
                                      sizeof(package_root));
        if (rc == 0) {
            snprintf(detail, sizeof(detail),
                     "Found FreeMcBoot at %.170s. Checking the complete installer package now.",
                     package_root);
            MciProgressUpdate(MCI_PROGRESS_FMCB, 16,
                              "FreeMcBoot package found", detail);
            report->source_probe_rc = 0;
            rc = ProbeRoot(package_root, target_port, report);
            if (rc == 0 && report->status == FMCB_PACKAGE_READY)
                (void)MciUsbRememberVerifiedPackageRoot(package_root);
            return rc;
        }
        report->source_probe_rc = rc;
    }

    report->status = FMCB_PACKAGE_NOT_FOUND;
    MciProgressUpdate(MCI_PROGRESS_FMCB, 100, "FreeMcBoot package not found",
                      "No complete-package anchor SYSTEM/FMCB.XLF was found in visible USB folders. A standalone FMCB.XLF can still be used for the MagicGate test.");
    return -1;
}

const char *FmcbPackageStatusText'''
s, n = pattern.subn(replacement, s, count=1)
if n != 1:
    raise SystemExit("FmcbProbeMassPackage replacement failed")
p.write_text(s)


# ---------------------------------------------------------------------------
# MagicGate: prefer verified package cache, otherwise bounded recursive search.
# ---------------------------------------------------------------------------
p = Path("src/magicgate.c")
s = p.read_text()
pattern = re.compile(r'static int FindRawKelfSource\(MagicGateReport \*report\)\n\{.*?\n\}\n\nstatic const char \*RawPathFromReport', re.S)
replacement = r'''static int UseRawKelfPath(MagicGateReport *report, const char *path,
                          const char *source_kind)
{
    iox_stat_t stat;
    char detail[224];
    int rc;

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    report->source_io_rc = rc;
    if (rc < 0)
        return rc;
    if (!FIO_S_ISREG(stat.mode) || stat.size < sizeof(SecrKELFHeader_t) ||
        stat.size > MG_MAX_KELF_SIZE)
        return MG_INVALID_LAYOUT;

    report->source_port = MG_RAW_SOURCE_PORT;
    report->source_size = (int)stat.size;
    snprintf(report->source_path, sizeof(report->source_path), "%s", path);
    snprintf(detail, sizeof(detail), "%s: %.150s (%d KiB).",
             source_kind, path, (report->source_size + 1023) / 1024);
    MgProgress(report, 8, "FMCB.XLF ready", detail);
    return 0;
}

static int FindRawKelfSource(MagicGateReport *report)
{
    char package_root[MCI_USB_SEARCH_PATH_MAX];
    char path[MCI_USB_SEARCH_PATH_MAX];
    int rc;

    /* A complete FMCB package that already passed preflight is the preferred
     * source. This avoids recursively walking the USB tree on every scan. */
    if (MciUsbGetVerifiedPackageRoot(package_root, sizeof(package_root)) == 0) {
        int written = snprintf(path, sizeof(path), "%s/SYSTEM/FMCB.XLF",
                               package_root);
        if (written > 0 && (unsigned int)written < sizeof(path)) {
            rc = UseRawKelfPath(report, path, "Using verified installer package");
            if (rc == 0)
                return 0;
        }
        MciUsbClearVerifiedPackageRoot();
    }

    MgProgress(report, 2, "Looking for FMCB.XLF",
               "No verified package path is cached, so visible USB folders are being searched once.");
    if (MciUsbWaitForStorage(20u, 100000u) < 0)
        return sceMcResNoEntry;
    rc = MciUsbFindFmcbXlf(path, sizeof(path), 0,
                           RawKelfSearchProgress, report);
    report->source_io_rc = rc;
    if (rc < 0)
        return sceMcResNoEntry;
    return UseRawKelfPath(report, path, "Found on USB storage");
}

static const char *RawPathFromReport'''
s, n = pattern.subn(replacement, s, count=1)
if n != 1:
    raise SystemExit("FindRawKelfSource replacement failed")
p.write_text(s)


# ---------------------------------------------------------------------------
# Full scan: preflight while USB is stable, then MagicGate uses cached package.
# ---------------------------------------------------------------------------
p = Path("src/app_main.c")
s = p.read_text()
old = r'''    snprintf(detail, sizeof(detail),
             "Running the complete read-only sequence for mc%d using %s, then MagicGate/CardAuth and FMCB package preflight.",
             target_port, MciFsTestProfileName(Settings.fs_profile));
    MciGuiRenderMessage("Full card scan", detail, NULL, MCI_GUI_TONE_INFO);
    MagicGateResetReport(&MgReports[target_port], target_port);
    FmcbResetPackageReport(&FmcbReports[target_port], target_port);
    CardInspectSized(target_port, &Reports[target_port], CurrentFsTestBytes());
    if (Reports[target_port].type == MC_TYPE_PS2) {
        (void)RunMagicGateSession(target_port);
    } else {
        MagicGateResetReport(&MgReports[target_port], target_port);
        MgReports[target_port].result = MG_RESULT_TARGET_NOT_PS2;
    }
    (void)FmcbProbeMassPackage(target_port, &FmcbMassStatus,
                               &FmcbReports[target_port]);
    (void)RefreshRecoveryStatus();
'''
new = r'''    snprintf(detail, sizeof(detail),
             "Running the complete read-only sequence for mc%d using %s, then FMCB package preflight and MagicGate/CardAuth.",
             target_port, MciFsTestProfileName(Settings.fs_profile));
    MciGuiRenderMessage("Full card scan", detail, NULL, MCI_GUI_TONE_INFO);
    MagicGateResetReport(&MgReports[target_port], target_port);
    FmcbResetPackageReport(&FmcbReports[target_port], target_port);
    CardInspectSized(target_port, &Reports[target_port], CurrentFsTestBytes());

    /* Discover and fully validate the installer while the normal USB stack is
     * already stable. A successful preflight caches the package root, so the
     * following MagicGate test can open its FMCB.XLF directly rather than
     * searching again after an IOP transition. */
    (void)FmcbProbeMassPackage(target_port, &FmcbMassStatus,
                               &FmcbReports[target_port]);
    if (Reports[target_port].type == MC_TYPE_PS2) {
        (void)RunMagicGateSession(target_port);
    } else {
        MagicGateResetReport(&MgReports[target_port], target_port);
        MgReports[target_port].result = MG_RESULT_TARGET_NOT_PS2;
    }
    (void)RefreshRecoveryStatus();
'''
s = replace_once(s, old, new, "RunSelectedFullScan")
s = s.replace('"v0.4.0-dev1  mc%d"', '"v0.4.0-dev2  mc%d"')
s = s.replace('PS2 Memory Card Inspector 0.4.0-dev1\\n\\n',
              'PS2 Memory Card Inspector 0.4.0-dev2\\n\\n')
p.write_text(s)


# ---------------------------------------------------------------------------
# GUI core: friendly MagicGate step/source; remove redundant display rc strip.
# ---------------------------------------------------------------------------
p = Path("src/gui_core.inc")
s = p.read_text()
anchor = r'''static qword_t *render_magicgate(qword_t *q, int selected,
                                 const CardReport cards[2],
                                 const MagicGateReport magicgate[2],
                                 const MagicGateIopStatus *mg_iop,
                                 const FmcbPackageReport packages[2])
{'''
helper = r'''static const char *magicgate_user_step(const MagicGateReport *mg)
{
    const char *result;

    if (mg == NULL || mg->result == MG_RESULT_NOT_RUN)
        return "Not tested yet";
    if (mg->result == MG_RESULT_PASS)
        return "MagicGate authentication completed";

    result = MagicGateResultText(mg->result);
    if (mg->stage == MG_STAGE_GET_KBIT &&
        strncmp(result, "NOT SUPPORTED", 13) == 0)
        return "Card did not answer MagicGate authentication";

    switch (mg->stage) {
        case MG_STAGE_FIND_KELF: return "Finding the FMCB test file";
        case MG_STAGE_READ_KELF: return "Reading the FMCB test file";
        case MG_STAGE_VALIDATE_KELF: return "Checking the FMCB test file";
        case MG_STAGE_SESSION_SETUP: return "Starting the isolated security test";
        case MG_STAGE_SESSION_CARD_CHECK: return "Checking the selected memory card";
        case MG_STAGE_BIND_RPC: return "Preparing the MagicGate security service";
        case MG_STAGE_DOWNLOAD_HEADER:
        case MG_STAGE_DOWNLOAD_BLOCKS: return "Sending protected test data";
        case MG_STAGE_GET_KBIT: return "Authenticating the memory card";
        case MG_STAGE_GET_KC: return "Verifying the card security key";
        case MG_STAGE_GET_ICVPS2: return "Checking the final KELF signature";
        case MG_STAGE_DONE: return "MagicGate test finished";
        default: return "Waiting to start";
    }
}

static qword_t *render_magicgate(qword_t *q, int selected,
                                 const CardReport cards[2],
                                 const MagicGateReport magicgate[2],
                                 const MagicGateIopStatus *mg_iop,
                                 const FmcbPackageReport packages[2])
{'''
s = replace_once(s, anchor, helper, "magicgate helper anchor")
s = s.replace('    char line[160];\n\n    q = slot_summary(q, selected, cards, magicgate, packages);',
              '    char line[256];\n\n    q = slot_summary(q, selected, cards, magicgate, packages);', 1)
old = r'''    q = rect_fill(q, 158, 93, 628, 123, Theme.panel);
    q = rect_outline(q, 158, 93, 628, 123, Theme.border);
    q = text(q, 172, 100, "Stage", Theme.muted);
    q = text_box(q, 236, 100, 615, 108, MagicGateStageText(mg->stage), Theme.text);
    if (mg->source_path[0] != '\0') {
        snprintf(line, sizeof(line), "%s  (%d bytes)", mg->source_path, mg->source_size);
        q = text_box(q, 172, 111, 615, 119, line, Theme.muted);
    } else {
        q = text(q, 172, 111, "KELF source: not prepared", Theme.muted);
    }
'''
new = r'''    q = rect_fill(q, 158, 93, 628, 123, Theme.panel);
    q = rect_outline(q, 158, 93, 628, 123, Theme.border);
    q = text(q, 172, 100, "Test step", Theme.muted);
    q = text_box(q, 252, 100, 615, 108, magicgate_user_step(mg), Theme.text);
    q = text(q, 172, 111, "Source", Theme.muted);
    if (mg->source_path[0] != '\0') {
        const char *path = strncmp(mg->source_path, "RAW ", 4) == 0
                               ? mg->source_path + 4 : mg->source_path;
        snprintf(line, sizeof(line), "%s  (%d KiB)", path,
                 (mg->source_size + 1023) / 1024);
        q = selected_single_line(q, 236, 111, 615, 119, line, Theme.muted);
    } else {
        q = text_box(q, 236, 111, 615, 119, "No FMCB.XLF selected", Theme.muted);
    }
'''
s = replace_once(s, old, new, "magicgate summary block")

# Settings page no longer burns a strip on an internal return code/current mode.
s = s.replace('    char footer[96];\n', '')
old = r'''    q = rect_fill(q, 24, 190, 616, 202, Theme.panel_alt);
    q = rect_outline(q, 24, 190, 616, 202, Theme.border);
    if (last_video_rc == -999)
        snprintf(footer, sizeof(footer), "Current output: %s", MciVideoModeName(MciGuiCurrentVideoMode()));
    else
        snprintf(footer, sizeof(footer), "Last display switch rc=%d   current=%s",
                 last_video_rc, MciVideoModeName(MciGuiCurrentVideoMode()));
    q = text_box(q, 34, 193, 606, 201, footer,
                 last_video_rc < 0 && last_video_rc != -999 ? Theme.danger : Theme.muted);
    return q;
'''
new = r'''    (void)last_video_rc;
    return q;
'''
s = replace_once(s, old, new, "settings status strip")
s = s.replace('"v0.4.0-dev1  mc%d"', '"v0.4.0-dev2  mc%d"')
p.write_text(s)


# ---------------------------------------------------------------------------
# GUI composition: give the MagicGate source marquee its own reset state.
# ---------------------------------------------------------------------------
p = Path("src/gui.c")
s = p.read_text()
s = replace_once(
    s,
    'static int LastFmcbMarqueeSlot = -1;\nstatic int LastFmcbMarqueeStatus = -1;\nstatic int ActiveHeaderSlot = 0;\n',
    'static int LastFmcbMarqueeSlot = -1;\nstatic int LastFmcbMarqueeStatus = -1;\nstatic int LastMagicGateMarqueeSlot = -1;\nstatic char LastMagicGateMarqueeSource[192];\nstatic int ActiveHeaderSlot = 0;\n',
    'GUI marquee state')
s = s.replace('"v0.4.0-dev1  mc%d"', '"v0.4.0-dev2  mc%d"')
old = r'''    if (page != MCI_GUI_FMCB) {
        LastFmcbMarqueeSlot = -1;
        LastFmcbMarqueeStatus = -1;
    }

    q = frame_begin(&packet);
'''
new = r'''    if (page != MCI_GUI_FMCB) {
        LastFmcbMarqueeSlot = -1;
        LastFmcbMarqueeStatus = -1;
    }
    if (page == MCI_GUI_MAGICGATE) {
        const char *source = magicgate[selected].source_path;
        if (selected != LastMagicGateMarqueeSlot ||
            strcmp(source, LastMagicGateMarqueeSource) != 0) {
            MarqueeEpoch = GetTimerSystemTime();
            LastMagicGateMarqueeSlot = selected;
            snprintf(LastMagicGateMarqueeSource,
                     sizeof(LastMagicGateMarqueeSource), "%s", source);
        }
    } else {
        LastMagicGateMarqueeSlot = -1;
        LastMagicGateMarqueeSource[0] = '\0';
    }

    q = frame_begin(&packet);
'''
s = replace_once(s, old, new, "MagicGate marquee dashboard hook")
p.write_text(s)


# Basic static sanity checks before CI gets the final word.
for name in ["src/usb_search.c", "src/usb_search.h", "src/fmcb_install.c",
             "src/magicgate.c", "src/app_main.c", "src/gui_core.inc", "src/gui.c"]:
    text = Path(name).read_text()
    if "v0.4.0-dev1" in text and name in ("src/app_main.c", "src/gui_core.inc", "src/gui.c"):
        raise SystemExit(f"stale dev1 label in {name}")

print("0.4 hardware-feedback patch applied")
