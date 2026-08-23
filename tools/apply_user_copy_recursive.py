from pathlib import Path
import re

# CARD page: present normal-user concepts instead of libmc API names/raw rc.
p = Path('src/gui_core.inc')
s = p.read_text()
old = '''    snprintf(line, sizeof(line), "%d (%s)", r->info_rc, CardResultText(r->info_rc));
    q = value_line(q, 172, 101, "mcGetInfo", line, Theme.text);
    snprintf(line, sizeof(line), "%d (%s)", r->type, CardTypeText(r->type));
    q = value_line(q, 172, 113, "Card type", line, Theme.text);
    snprintf(line, sizeof(line), "%d    free=%d", r->formatted, r->free_clusters);
    q = value_line(q, 172, 125, "Formatted", line, Theme.text);
    snprintf(line, sizeof(line), "%d (%s)", r->root_rc, CardResultText(r->root_rc));
    q = value_line(q, 172, 137, "Root dir", line, Theme.text);
    snprintf(line, sizeof(line), "%s  rc=%d cleanup=%d",
             CardRwStageText(r->rw_stage), r->rw_rc, r->cleanup_rc);
    q = value_line(q, 172, 149, "R/W verify", line,
                   r->rw_rc == 0 ? Theme.success : Theme.text);
'''
new = '''    snprintf(line, sizeof(line), "%s",
             r->info_rc == 0 ? "Ready" : CardResultText(r->info_rc));
    q = value_line(q, 172, 101, "Card status", line,
                   r->info_rc == 0 ? Theme.success : Theme.text);
    snprintf(line, sizeof(line), "%s", CardTypeText(r->type));
    q = value_line(q, 172, 113, "Card type", line, Theme.text);
    if (r->formatted)
        snprintf(line, sizeof(line), "Formatted, %d free clusters", r->free_clusters);
    else
        snprintf(line, sizeof(line), "Not formatted");
    q = value_line(q, 172, 125, "Filesystem", line,
                   r->formatted ? Theme.success : Theme.warning);
    snprintf(line, sizeof(line), "%s",
             r->root_rc == 0 ? "Readable" : CardResultText(r->root_rc));
    q = value_line(q, 172, 137, "Root access", line,
                   r->root_rc == 0 ? Theme.success : Theme.text);
    if (r->rw_stage == RW_NOT_RUN)
        snprintf(line, sizeof(line), "Not run");
    else if (r->rw_rc == 0 && r->cleanup_rc == 0)
        snprintf(line, sizeof(line), "Passed; temporary data removed");
    else if (r->rw_rc == 0)
        snprintf(line, sizeof(line), "Passed; cleanup warning");
    else
        snprintf(line, sizeof(line), "%s - %s",
                 CardRwStageText(r->rw_stage), CardResultText(r->rw_rc));
    q = value_line(q, 172, 149, "Integrity test", line,
                   r->rw_rc == 0 ? Theme.success : Theme.text);
'''
if s.count(old) != 1:
    raise SystemExit('CARD detail block anchor mismatch')
s = s.replace(old, new, 1)
s = s.replace('"FILESYSTEM HEALTH"', '"CARD & FILESYSTEM"', 1)
oldfmt = '''            snprintf(line, sizeof(line), "Last format rc: %d (%s)",
                     last_format_rc, CardResultText(last_format_rc));'''
newfmt = '''            snprintf(line, sizeof(line), "Last format result: %s",
                     CardResultText(last_format_rc));'''
if s.count(oldfmt) != 1:
    raise SystemExit('format result anchor mismatch')
s = s.replace(oldfmt, newfmt, 1)
p.write_text(s)

# Filesystem progress copy should describe user-visible operations.
p = Path('src/card.c')
s = p.read_text()
s = s.replace('"Querying memory-card metadata"', '"Checking the memory card"')
s = s.replace('"Calling mcGetInfo to identify the card type, format state and free clusters."',
              '"Reading the card type, formatting state and available space."')
s = s.replace('"mcGetInfo reported a card authentication failure."',
              '"The card information check reported an authentication failure."')
s = s.replace('"Checking the root directory"', '"Checking the card filesystem"')
p.write_text(s)

# MagicGate raw KELF: recursively accept FMCB.XLF anywhere on USB.
p = Path('src/magicgate.c')
s = p.read_text()
if '#include "usb_search.h"' not in s:
    s = s.replace('#include "progress.h"\n', '#include "progress.h"\n#include "usb_search.h"\n', 1)
s = re.sub(r'static const char \*RawKelfCandidates\[\] = \{.*?\};\n\n', '', s, count=1, flags=re.S)
anchor = '''static int McSyncResult(void)
{'''
callback = '''static void RawKelfSearchProgress(const char *path,
                                  unsigned int directories_scanned,
                                  void *userdata)
{
    MagicGateReport *report = (MagicGateReport *)userdata;
    char detail[224];
    int percent = 2 + (int)(directories_scanned / 8u);

    if (percent > 7)
        percent = 7;
    snprintf(detail, sizeof(detail),
             "Searched %u folders; checking %.150s",
             directories_scanned, path != NULL ? path : "USB storage");
    MgProgress(report, percent, "Searching USB storage for FMCB.XLF", detail);
}

static int McSyncResult(void)
{'''
if s.count(anchor) != 1:
    raise SystemExit('MagicGate callback anchor mismatch')
s = s.replace(anchor, callback, 1)
pattern = r'static int FindRawKelfSource\(MagicGateReport \*report\)\n\{.*?\n\}\n\nstatic const char \*RawPathFromReport'
replacement = '''static int FindRawKelfSource(MagicGateReport *report)
{
    iox_stat_t stat;
    char path[MCI_USB_SEARCH_PATH_MAX];
    char detail[224];
    int rc;

    MgProgress(report, 2, "Searching USB storage for FMCB.XLF",
               "Scanning folders recursively. FMCB.XLF can be stored anywhere on the USB drive.");
    rc = MciUsbFindFmcbXlf(path, sizeof(path), 0,
                           RawKelfSearchProgress, report);
    report->source_io_rc = rc;
    if (rc < 0)
        return sceMcResNoEntry;

    memset(&stat, 0, sizeof(stat));
    rc = fileXioGetStat(path, &stat);
    report->source_io_rc = rc;
    if (rc < 0)
        return rc;
    if (stat.size < sizeof(SecrKELFHeader_t) || stat.size > MG_MAX_KELF_SIZE)
        return MG_INVALID_LAYOUT;

    report->source_port = MG_RAW_SOURCE_PORT;
    report->source_size = (int)stat.size;
    snprintf(report->source_path, sizeof(report->source_path), "RAW %s", path);
    snprintf(detail, sizeof(detail), "Found %.160s (%d bytes).",
             path, report->source_size);
    MgProgress(report, 8, "FMCB.XLF found", detail);
    return 0;
}

static const char *RawPathFromReport'''
s, n = re.subn(pattern, replacement, s, count=1, flags=re.S)
if n != 1:
    raise SystemExit('FindRawKelfSource replacement mismatch')
p.write_text(s)

# FMCB preflight: recursively find a real package by SYSTEM/FMCB.XLF and derive root.
p = Path('src/fmcb_install.c')
s = p.read_text()
if '#include "usb_search.h"' not in s:
    s = s.replace('#include "progress.h"\n', '#include "progress.h"\n#include "usb_search.h"\n', 1)
s = re.sub(r'static const char \*MassRoots\[\] = \{.*?\};\n\n', '', s, count=1, flags=re.S)
anchor = '''static int ProbeRoot(const char *root, int target_port, FmcbPackageReport *report)
{'''
callback = '''static void PackageSearchProgress(const char *path,
                                  unsigned int directories_scanned,
                                  void *userdata)
{
    char detail[224];
    int percent = 8 + (int)(directories_scanned / 10u);

    (void)userdata;
    if (percent > 15)
        percent = 15;
    snprintf(detail, sizeof(detail),
             "Searched %u folders; checking %.150s",
             directories_scanned, path != NULL ? path : "USB storage");
    MciProgressUpdate(MCI_PROGRESS_FMCB, percent,
                      "Searching USB storage for a FreeMcBoot package", detail);
}

static int ProbeRoot(const char *root, int target_port, FmcbPackageReport *report)
{'''
if s.count(anchor) != 1:
    raise SystemExit('FMCB ProbeRoot anchor mismatch')
s = s.replace(anchor, callback, 1)
pattern = r'int FmcbProbeMassPackage\(int target_port, const FmcbMassBackendStatus \*backend,\n                         FmcbPackageReport \*report\)\n\{.*?\n\}\n\nconst char \*FmcbPackageStatusText'
replacement = '''int FmcbProbeMassPackage(int target_port, const FmcbMassBackendStatus *backend,
                         FmcbPackageReport *report)
{
    char xlf_path[MCI_USB_SEARCH_PATH_MAX];
    char package_root[FMCB_SOURCE_ROOT_MAX];
    char detail[256];
    int rc;

    FmcbResetPackageReport(report, target_port);
    MciProgressUpdate(MCI_PROGRESS_FMCB, 3, "Checking USB storage",
                      "Making sure the USB filesystem is available before looking for FreeMcBoot files.");
    if (backend == NULL || !backend->available) {
        report->status = FMCB_PACKAGE_SOURCE_UNAVAILABLE;
        report->source_probe_rc = -1;
        MciProgressUpdate(MCI_PROGRESS_FMCB, 100, "USB storage is not available",
                          "Connect a readable USB drive containing the FreeMcBoot package, then run preflight again.");
        return -1;
    }

    MciProgressUpdate(MCI_PROGRESS_FMCB, 7,
                      "Searching USB storage for a FreeMcBoot package",
                      "Looking recursively for SYSTEM/FMCB.XLF. The package folder may be placed anywhere on the USB drive.");
    rc = MciUsbFindFmcbXlf(xlf_path, sizeof(xlf_path), 1,
                           PackageSearchProgress, NULL);
    report->source_probe_rc = rc;
    if (rc == 0) {
        rc = MciUsbPackageRootFromXlf(xlf_path, package_root,
                                      sizeof(package_root));
        if (rc == 0) {
            snprintf(detail, sizeof(detail),
                     "Found FreeMcBoot at %.170s. Checking the rest of the package now.",
                     package_root);
            MciProgressUpdate(MCI_PROGRESS_FMCB, 16,
                              "FreeMcBoot package found", detail);
            report->source_probe_rc = 0;
            return ProbeRoot(package_root, target_port, report);
        }
        report->source_probe_rc = rc;
    }

    report->status = FMCB_PACKAGE_NOT_FOUND;
    MciProgressUpdate(MCI_PROGRESS_FMCB, 100, "FreeMcBoot package not found",
                      "No SYSTEM/FMCB.XLF was found within the bounded recursive USB scan. A standalone FMCB.XLF can still be used for the MagicGate test.");
    return -1;
}

const char *FmcbPackageStatusText'''
s, n = re.subn(pattern, replacement, s, count=1, flags=re.S)
if n != 1:
    raise SystemExit('FmcbProbeMassPackage replacement mismatch')
p.write_text(s)

# FMCB page: hide raw probe return codes from normal users.
p = Path('src/gui.c')
s = p.read_text()
old = '''    snprintf(line, sizeof(line), "mass: %s   source: %s",
             mass != NULL && mass->available ? "AVAILABLE" : "UNAVAILABLE",
             r->source_root[0] ? r->source_root : "not resolved");
    q = text_box(q, 172, 101, 615, 109, line, Theme.text);
    snprintf(line, sizeof(line), "probe rc=%d   payload=%u bytes",
             r->source_probe_rc, r->total_found_bytes);
    q = text_box(q, 172, 113, 615, 121, line, Theme.muted);
'''
new = '''    snprintf(line, sizeof(line), "USB storage: %s   package data: %u KiB",
             mass != NULL && mass->available ? "READY" : "NOT AVAILABLE",
             (r->total_found_bytes + 1023u) / 1024u);
    q = text_box(q, 172, 101, 615, 109, line, Theme.text);
    if (r->source_root[0] != '\0') {
        size_t root_len = strlen(r->source_root);
        const char *root_tail = root_len > 43u
                                    ? r->source_root + root_len - 43u
                                    : r->source_root;
        snprintf(line, sizeof(line), "Found at: %s%s",
                 root_len > 43u ? "..." : "", root_tail);
    } else {
        snprintf(line, sizeof(line), "Package location: not found yet");
    }
    q = text_box(q, 172, 113, 615, 121, line, Theme.muted);
'''
if s.count(old) != 1:
    raise SystemExit('FMCB GUI source block anchor mismatch')
s = s.replace(old, new, 1)
p.write_text(s)
