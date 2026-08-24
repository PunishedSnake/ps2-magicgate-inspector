#!/usr/bin/env python3
from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"pattern not found in {path}: {old[:100]!r}")
    p.write_text(text.replace(old, new, 1))


def append_once(path, marker, block):
    p = Path(path)
    text = p.read_text()
    if marker in text:
        return
    p.write_text(text + block)


# ---------------------------------------------------------------------------
# Raw image engine: preserve immediate libmc errors and prime selected port.
# ---------------------------------------------------------------------------
replace_once(
    "src/card_image.c",
'''static int McResult(void)
{
    int result = -999;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static int ReadPage(int port, u32 page, unsigned char data[IMAGE_PAGE_DATA])
{
    int rc;
    mcReadPage(port, 0, (int)page, data);
    rc = McResult();
    return rc;
}

static int WritePage(int port, u32 page, const unsigned char data[IMAGE_PAGE_DATA])
{
    int rc;
    mcWritePage(port, 0, (int)page, (void *)data);
    rc = McResult();
    return rc;
}

static int EraseBlock(int port, u32 block)
{
    int rc;
    mcEraseBlock(port, 0, (int)block, -1);
    rc = McResult();
    return rc;
}
''',
'''static int CompleteMcCommand(int issue_rc)
{
    int result = -999;
    int sync_rc;

    /* libmc can reject a command before an RPC is queued. Never call mcSync in
     * that case: doing so used to turn the useful immediate -1 into our -999
     * sentinel and produced the misleading CARD GEOMETRY ERROR seen on dev3. */
    if (issue_rc < 0)
        return issue_rc;
    sync_rc = mcSync(MC_WAIT, NULL, &result);
    if (sync_rc < 0)
        return sync_rc;
    if (sync_rc != 1)
        return -998;
    return result;
}

static int ReadPage(int port, u32 page, unsigned char data[IMAGE_PAGE_DATA])
{
    return CompleteMcCommand(mcReadPage(port, 0, (int)page, data));
}

static int WritePage(int port, u32 page, const unsigned char data[IMAGE_PAGE_DATA])
{
    return CompleteMcCommand(mcWritePage(port, 0, (int)page, (void *)data));
}

static int EraseBlock(int port, u32 block)
{
    return CompleteMcCommand(mcEraseBlock(port, 0, (int)block, -1));
}
''')

replace_once(
    "src/card_image.c",
'''    unsigned char page[IMAGE_PAGE_DATA] __attribute__((aligned(64)));
    unsigned int i;
    int rc;

    if (geometry == NULL)
        return -1;
    memset(geometry, 0, sizeof(*geometry));

    rc = ReadPage(port, 0u, page);
''',
'''    unsigned char page[IMAGE_PAGE_DATA] __attribute__((aligned(64)));
    unsigned int i;
    int type = MC_TYPE_NONE;
    int free_clusters = -1;
    int formatted = 0;
    int rc;

    if (geometry == NULL)
        return -1;
    memset(geometry, 0, sizeof(*geometry));

    /* Legacy MCMAN keeps per-port card type state. Probe the actual selected
     * slot before the first raw-page RPC; mc1 must not inherit mc0's state. A
     * changed-card (-1) or no-format (-2) result is valid if type says PS2. */
    rc = CompleteMcCommand(mcGetInfo(port, 0, &type, &free_clusters, &formatted));
    if (rc < -2)
        return rc;
    if (type != MC_TYPE_PS2)
        return sceMcResFailDetect;

    rc = ReadPage(port, 0u, page);
''')

replace_once(
    "src/card_image.c",
'''    mcFormat(port, 0);
    rc = McResult();
''',
'''    rc = CompleteMcCommand(mcFormat(port, 0));
''')

replace_once(
    "src/card_image.c",
'''    rc = MciCardImageProbeGeometry(port, &geometry);
    if (rc < 0) {
        report->result = MCI_CARD_IMAGE_GEOMETRY_ERROR;
        return rc;
    }
''',
'''    rc = MciCardImageProbeGeometry(port, &geometry);
    if (rc < 0) {
        report->result = rc <= -10 ? MCI_CARD_IMAGE_NO_CARD
                                   : MCI_CARD_IMAGE_GEOMETRY_ERROR;
        report->verify_rc = rc;
        return rc;
    }
''')

# ---------------------------------------------------------------------------
# FMCB inventory: exact lookup -> parent enumeration -> direct read-only open.
# ---------------------------------------------------------------------------
replace_once(
    "src/fmcb_transaction.h",
'''    int existed;
    int backup_rc;
''',
'''    int existed;
    int inventory_exact_rc;
    int inventory_parent_rc;
    int inventory_open_rc;
    int backup_rc;
''')

replace_once(
    "src/fmcb_transaction.c",
'''        report->files[i].backup_rc = -999;
        report->files[i].bind_rc = -999;
''',
'''        report->files[i].inventory_exact_rc = -999;
        report->files[i].inventory_parent_rc = -999;
        report->files[i].inventory_open_rc = -999;
        report->files[i].backup_rc = -999;
        report->files[i].bind_rc = -999;
''')

replace_once(
    "src/fmcb_transaction.c",
'''static int InventoryTargetFromParent(int port, const char *path,
                                     int *exists, unsigned int *size)
{
    sceMcTblGetDir entries[64] __attribute__((aligned(64)));
    char pattern[FMCB_PATH_MAX];
    const char *slash = strrchr(path, '/');
    const char *name;
    int prefix_len;
    int count;
    int i;

    if (slash == NULL || slash == path || slash[1] == '\\0')
        return -4620;
    name = slash + 1;
    prefix_len = (int)(slash - path);
    if (snprintf(pattern, sizeof(pattern), "%.*s/*", prefix_len, path) < 0 ||
        strlen(pattern) >= sizeof(pattern))
        return -4621;

    memset(entries, 0, sizeof(entries));
    mcGetDir(port, 0, pattern, 0, 64, entries);
    count = McResult();
    if (count == sceMcResNoEntry || count == 0)
        return 0;
    if (count < 0)
        return count;
    if (count > 64)
        count = 64;
    for (i = 0; i < count; i++) {
        if (NameEqualCi(entries[i].EntryName, name)) {
            *exists = 1;
            *size = entries[i].FileSizeByte;
            return 0;
        }
    }
    return 0;
}

static int InventoryTarget(int port, const char *path,
                           int *exists, unsigned int *size)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int exact_rc;
    int fallback_rc;

    *exists = 0;
    *size = 0;
    memset(&info, 0, sizeof(info));
    mcGetDir(port, 0, path, 0, 1, &info);
    exact_rc = McResult();
    if (exact_rc == sceMcResNoEntry || exact_rc == 0)
        return 0;
    if (exact_rc > 0) {
        *exists = 1;
        *size = info.FileSizeByte;
        return 0;
    }

    fallback_rc = InventoryTargetFromParent(port, path, exists, size);
    if (fallback_rc == 0)
        return 0;
    return exact_rc;
}
''',
'''static int CompleteInventoryCommand(int issue_rc)
{
    if (issue_rc < 0)
        return issue_rc;
    return McResult();
}

/* Return 1 when found, 0 when the parent was enumerated and the name is absent,
 * or a negative mc/lib error if the parent itself cannot be enumerated. */
static int InventoryTargetFromParent(int port, const char *path,
                                     int *exists, unsigned int *size)
{
    sceMcTblGetDir entries[64] __attribute__((aligned(64)));
    char pattern[FMCB_PATH_MAX];
    const char *slash = strrchr(path, '/');
    const char *name;
    int prefix_len;
    int count;
    int i;

    if (slash == NULL || slash == path || slash[1] == '\\0')
        return -4620;
    name = slash + 1;
    prefix_len = (int)(slash - path);
    if (snprintf(pattern, sizeof(pattern), "%.*s/*", prefix_len, path) < 0 ||
        strlen(pattern) >= sizeof(pattern))
        return -4621;

    memset(entries, 0, sizeof(entries));
    count = CompleteInventoryCommand(mcGetDir(port, 0, pattern, 0, 64, entries));
    if (count == sceMcResNoEntry || count == 0)
        return 0;
    if (count < 0)
        return count;
    if (count > 64)
        count = 64;
    for (i = 0; i < count; i++) {
        if (NameEqualCi((const char *)entries[i].EntryName, name)) {
            *exists = 1;
            *size = entries[i].FileSizeByte;
            return 1;
        }
    }
    return 0;
}

/* Protected system directories may reject mcGetDir while their files are still
 * perfectly readable. A direct read-only open is both less invasive and a
 * stronger precondition for rollback: if it succeeds, we know the old target
 * can actually be captured before replacement. */
static int InventoryTargetFromOpen(int port, const char *path,
                                   int *exists, unsigned int *size)
{
    int fd;
    int end;
    int close_rc;

    fd = CompleteInventoryCommand(mcOpen(port, 0, path, FIO_O_RDONLY));
    if (fd == sceMcResNoEntry)
        return 0;
    if (fd < 0)
        return fd;

    end = CompleteInventoryCommand(mcSeek(fd, 0, SEEK_END));
    close_rc = CloseCardFile(fd);
    if (end < 0)
        return end;
    if (close_rc < 0)
        return close_rc;
    *exists = 1;
    *size = (unsigned int)end;
    return 1;
}

static int InventoryTarget(int port, const char *path,
                           int *exists, unsigned int *size,
                           int *exact_out, int *parent_out, int *open_out)
{
    sceMcTblGetDir info __attribute__((aligned(64)));
    int exact_rc;
    int parent_rc;
    int open_rc;

    *exists = 0;
    *size = 0;
    *exact_out = -999;
    *parent_out = -999;
    *open_out = -999;

    memset(&info, 0, sizeof(info));
    exact_rc = CompleteInventoryCommand(mcGetDir(port, 0, path, 0, 1, &info));
    *exact_out = exact_rc;
    if (exact_rc == sceMcResNoEntry || exact_rc == 0)
        return 0;
    if (exact_rc > 0) {
        *exists = 1;
        *size = info.FileSizeByte;
        return 0;
    }

    parent_rc = InventoryTargetFromParent(port, path, exists, size);
    *parent_out = parent_rc;
    if (parent_rc >= 0)
        return 0;

    open_rc = InventoryTargetFromOpen(port, path, exists, size);
    *open_out = open_rc;
    if (open_rc >= 0)
        return 0;

    /* Fail closed. We only call an inaccessible target absent if one of the
     * read-only probes has actually proved absence. */
    return open_rc;
}
''')

replace_once(
    "src/fmcb_transaction.c",
'''        rc = InventoryTarget(target_port, file->destination, &file->existed,
                             &file->previous_size);
''',
'''        rc = InventoryTarget(target_port, file->destination, &file->existed,
                             &file->previous_size,
                             &file->inventory_exact_rc,
                             &file->inventory_parent_rc,
                             &file->inventory_open_rc);
''')

# ---------------------------------------------------------------------------
# Selective restore: verify every imported file by reopening and comparing it.
# ---------------------------------------------------------------------------
replace_once(
    "src/card_image_fs.c",
'''static int ImportFile(MciImportTxn *txn, const MciFsDirEntry *entry,
                      const char *path)
{
''',
'''static int VerifyImportedFile(MciImportTxn *txn, const MciFsDirEntry *entry,
                              const char *path)
{
    unsigned char source[FS_MAX_CLUSTER_BYTES] __attribute__((aligned(64)));
    unsigned char target[FS_MAX_CLUSTER_BYTES] __attribute__((aligned(64)));
    u32 current = entry->cluster;
    u32 remaining = entry->length;
    u32 next;
    int fd;
    int rc;

    mcOpen(txn->port, 0, path, FIO_O_RDONLY);
    fd = McResult();
    if (fd < 0)
        return fd;

    while (remaining > 0u) {
        u32 chunk = remaining > txn->image->cluster_bytes
                        ? txn->image->cluster_bytes : remaining;
        if (current == FS_FAT_END || current >= txn->image->sb.alloc_end) {
            mcClose(fd); (void)McResult();
            return -30;
        }
        rc = ReadCluster(txn->image, txn->image->sb.alloc_offset + current,
                         source);
        if (rc < 0) {
            mcClose(fd); (void)McResult();
            return rc;
        }
        mcRead(fd, target, (int)chunk);
        rc = McResult();
        if (rc != (int)chunk || memcmp(source, target, chunk) != 0) {
            mcClose(fd); (void)McResult();
            return rc < 0 ? rc : -31;
        }
        remaining -= chunk;
        if (remaining > 0u) {
            rc = NextRelativeCluster(txn->image, current, &next);
            if (rc != 0) {
                mcClose(fd); (void)McResult();
                return -32;
            }
            current = next;
        }
    }
    mcRead(fd, target, 1);
    rc = McResult();
    if (rc != 0) {
        mcClose(fd); (void)McResult();
        return rc < 0 ? rc : -33;
    }
    mcClose(fd);
    return McResult();
}

static int ImportFile(MciImportTxn *txn, const MciFsDirEntry *entry,
                      const char *path)
{
''')

replace_once(
    "src/card_image_fs.c",
'''    if (rc < 0)
        return rc;
    rc = SetMetadata(txn->port, path, entry);
''',
'''    if (rc < 0)
        return rc;
    rc = VerifyImportedFile(txn, entry, path);
    if (rc < 0)
        return rc;
    rc = SetMetadata(txn->port, path, entry);
''')

# ---------------------------------------------------------------------------
# GUI API and native renderers for Card Tools and Image Browser.
# ---------------------------------------------------------------------------
replace_once(
    "src/gui.h",
'''#include "card.h"
#include "magicgate.h"
''',
'''#include "card.h"
#include "card_image_fs.h"
#include "magicgate.h"
''')

replace_once(
    "src/gui.h",
'''void MciGuiRenderProgress(const char *title,
                          const char *action,
                          const char *detail,
                          int percent,
                          const char *footer,
                          MciGuiTone tone);

void MciGuiRenderFatal''',
'''void MciGuiRenderProgress(const char *title,
                          const char *action,
                          const char *detail,
                          int percent,
                          const char *footer,
                          MciGuiTone tone);

void MciGuiRenderCardTools(int selected, int selected_item);
void MciGuiRenderImageBrowser(int target_port,
                              const MciImageSaveList *list,
                              int selected_row,
                              int first_row,
                              int free_clusters);

void MciGuiRenderFatal''')

replace_once(
    "src/gui.c",
'''    snprintf(version, sizeof(version), "v0.4.0-dev3 Drebin  mc%d", ActiveHeaderSlot);
''',
'''    snprintf(version, sizeof(version), "v0.4.0-dev4 Drebin  mc%d", ActiveHeaderSlot);
''')

# Add renderers after progress renderer, before end of file. Marker is unique.
append_once(
    "src/gui.c",
    "void MciGuiRenderCardTools(int selected, int selected_item)",
'''

void MciGuiRenderCardTools(int selected, int selected_item)
{
    static const char *const titles[8] = {
        "BACKUP  PCSX2 .ps2", "BACKUP  OPL .vmc",
        "BROWSE / RESTORE .ps2", "BROWSE / RESTORE .vmc",
        "EXACT RESTORE .ps2", "EXACT RESTORE .vmc",
        "FORCE FORMAT + BACKUP", "RETURN"
    };
    static const char *const hints[8] = {
        "Create a full verified raw image with regenerated ECC.",
        "Create a full verified 512-byte-page VMC image.",
        "Browse saves in the latest .ps2 image and selectively restore them.",
        "Browse saves in the latest .vmc image and selectively restore them.",
        "Raw page-for-page restore. Destination geometry must match exactly.",
        "Raw page-for-page restore. Destination geometry must match exactly.",
        "Create and verify a .ps2 recovery image before formatting the card.",
        "Return to the Card dashboard."
    };
    packet_t *packet;
    qword_t *q;
    int i;

    if (!RendererReady)
        return;
    if (selected_item < 0 || selected_item >= 8)
        selected_item = 0;
    ActiveHeaderSlot = selected;

    q = frame_begin(&packet);
    q = rect_fill(q, 0, 0, UI_W, UI_H, Theme.background);
    q = rect_fill(q, 0, 0, UI_W, 4, Theme.accent);
    q = identity_banner(q);
    q = text(q, 18, 39, "CARD TOOLS", Theme.accent);
    q = text_box(q, 144, 39, 620, 48,
                 "Backup, selective restore, exact recovery and maintenance",
                 Theme.muted);

    for (i = 0; i < 8; i++) {
        int col = i & 1;
        int row = i >> 1;
        float x0 = col ? 322.0f : 16.0f;
        float x1 = col ? 624.0f : 318.0f;
        float y0 = 54.0f + (float)row * 35.0f;
        float y1 = y0 + 31.0f;
        UiRgb border = i == selected_item ? Theme.accent : Theme.border;
        UiRgb title_color = (i == 4 || i == 5 || i == 6)
                                ? Theme.warning : Theme.text;

        q = rect_fill(q, x0, y0, x1, y1,
                      i == selected_item ? Theme.panel_alt : Theme.panel);
        q = rect_outline(q, x0, y0, x1, y1, border);
        if (i == selected_item)
            q = rect_fill(q, x0, y0, x0 + 5, y1, Theme.accent);
        q = text_box(q, x0 + 12, y0 + 5, x1 - 8, y0 + 13,
                     titles[i], title_color);
        q = text_box(q, x0 + 12, y0 + 17, x1 - 8, y0 + 26,
                     hints[i], Theme.muted);
    }

    q = rect_fill(q, 0, 205, UI_W, UI_H, Theme.panel_alt);
    q = text_box(q, 18, 211, 622, 220,
                 "DPAD Move   X Open   CIRCLE Back", Theme.muted);
    frame_end(packet, q);
}

void MciGuiRenderImageBrowser(int target_port,
                              const MciImageSaveList *list,
                              int selected_row,
                              int first_row,
                              int free_clusters)
{
    packet_t *packet;
    qword_t *q;
    char line[192];
    const char *tail;
    int visible = 7;
    int selected_count = 0;
    u32 selected_clusters = 0u;
    int i;

    if (!RendererReady || list == NULL)
        return;
    ActiveHeaderSlot = target_port;
    if (first_row < 0)
        first_row = 0;

    for (i = 0; i < list->save_count; i++) {
        if (list->saves[i].selected) {
            selected_count++;
            selected_clusters += list->saves[i].required_clusters;
        }
    }

    q = frame_begin(&packet);
    q = rect_fill(q, 0, 0, UI_W, UI_H, Theme.background);
    q = rect_fill(q, 0, 0, UI_W, 4, Theme.accent);
    q = identity_banner(q);
    q = text(q, 18, 39, "IMAGE BROWSER / SELECTIVE RESTORE", Theme.accent);

    q = rect_fill(q, 16, 52, 624, 79, Theme.panel);
    q = rect_outline(q, 16, 52, 624, 79, Theme.border);
    tail = strlen(list->path) > 47u ? list->path + strlen(list->path) - 47u
                                   : list->path;
    snprintf(line, sizeof(line), "SOURCE  %s  %s%s",
             MciCardImageFormatName(list->format),
             strlen(list->path) > 47u ? "..." : "", tail);
    q = text_box(q, 28, 58, 612, 66, line, Theme.text);
    snprintf(line, sizeof(line),
             "DESTINATION  mc%d:    image saves %d    free clusters %d",
             target_port, list->save_count, free_clusters);
    q = text_box(q, 28, 69, 612, 77, line,
                 free_clusters >= 0 ? Theme.success : Theme.warning);

    q = rect_fill(q, 16, 83, 624, 177, Theme.panel);
    q = rect_outline(q, 16, 83, 624, 177, Theme.border);
    for (i = 0; i < visible; i++) {
        int index = first_row + i;
        float y0 = 86.0f + (float)i * 13.0f;
        const MciImageSaveEntry *save;
        char box[5];
        char size_text[32];
        const char *status;
        UiRgb status_color;

        if (index >= list->save_count)
            break;
        save = &list->saves[index];
        snprintf(box, sizeof(box), "[%c]", save->selected ? 'X' : ' ');
        snprintf(size_text, sizeof(size_text), "%u KiB",
                 (save->total_bytes + 1023u) / 1024u);
        status = save->conflict ? "EXISTS" : "OK";
        status_color = save->conflict ? Theme.warning : Theme.success;

        if (index == selected_row) {
            q = rect_fill(q, 19, y0 - 1, 621, y0 + 11, Theme.panel_alt);
            q = rect_fill(q, 19, y0 - 1, 23, y0 + 11, Theme.accent);
        }
        q = text(q, 29, y0 + 1, box,
                 save->conflict ? Theme.disabled : Theme.accent);
        q = text_box(q, 61, y0 + 1, 372, y0 + 9, save->name, Theme.text);
        q = text_box(q, 390, y0 + 1, 480, y0 + 9, size_text, Theme.muted);
        q = text_box(q, 530, y0 + 1, 610, y0 + 9, status, status_color);
    }

    q = rect_fill(q, 16, 181, 624, 201, Theme.panel_alt);
    q = rect_outline(q, 16, 181, 624, 201, Theme.border);
    snprintf(line, sizeof(line),
             "Selected %d   required %u clusters   destination free %d",
             selected_count, selected_clusters, free_clusters);
    q = text_box(q, 28, 188, 612, 196, line,
                 free_clusters >= 0 && selected_clusters <= (u32)free_clusters
                     ? Theme.accent : Theme.warning);

    q = rect_fill(q, 0, 205, UI_W, UI_H, Theme.panel_alt);
    q = text_box(q, 18, 211, 622, 220,
                 "UP/DOWN Move   SQUARE Select   TRIANGLE Fit all   X Restore   CIRCLE Back",
                 Theme.muted);
    frame_end(packet, q);
}
''')

# Main dashboard: Card Tools is the Triangle action now, not inline format.
replace_once(
    "src/gui_core.inc",
'''        if (r->format_allowed)
            q = text(q, 172, 181, "TRIANGLE  Arm destructive format", Theme.warning);
        else
            q = text(q, 172, 181, "Format locked for this card state", Theme.muted);
        if (last_format_rc != -999) {
''',
'''        q = text(q, 172, 181, "TRIANGLE  Open Card Tools", Theme.accent);
        if (last_format_rc != -999) {
''')

replace_once(
    "src/gui_core.inc",
'''        } else {
            q = text(q, 172, 191, "Filesystem and MagicGate are evaluated independently.", Theme.muted);
        }
''',
'''        } else {
            q = text(q, 172, 191,
                     "Backup, image browser, restore and format maintenance.",
                     Theme.muted);
        }
''')

replace_once(
    "src/gui_core.inc",
'''        line = "UP/DOWN Slot   X Preflight   SQUARE Installer   L2+X Full scan   L1/R1 Page";
    } else {
        line = "UP/DOWN Slot   X Test   L2+X Full scan   L1/R1 Page   SELECT Exit";
''',
'''        line = "UP/DOWN Slot  X Preflight  SQUARE Installer  TRIANGLE Tools  L1/R1 Page";
    } else {
        line = "UP/DOWN Slot  X Test  L2+X Full  TRIANGLE Tools  L1/R1 Page";
''')

# Identity strings in included fallback renderer.
p = Path("src/gui_core.inc")
s = p.read_text().replace("v0.4.0-dev3 Drebin", "v0.4.0-dev4 Drebin")
p.write_text(s)

# ---------------------------------------------------------------------------
# Application: wire native Card Tools and a real multi-select image browser.
# ---------------------------------------------------------------------------
replace_once(
    "src/app_main.c",
'''#include "card_image.h"
#include "card_raw_session.h"
''',
'''#include "card_image.h"
#include "card_image_fs.h"
#include "card_raw_session.h"
''')

replace_once(
    "src/app_main.c",
'''        snprintf(result, sizeof(result),
                 "Install failed at %s: %s. Target: %s. Files committed before failure: %d/%d. space rc=%d, recovery rc=%d, rollback rc=%d. If recovery remains present, do not start another install; restore the recorded transaction first.",
                 FmcbInstallStageText(report->stage),
                 FmcbInstallResultText(report->result), failed_target,
                 report->files_committed, report->files_total,
                 report->space_rc, report->recovery_rc,
                 report->rollback_rc);
''',
'''        const FmcbInstallFileReport *failed_file =
            (report->current_file >= 0 && report->current_file < FMCB_TX_MAX_FILES)
                ? &report->files[report->current_file] : NULL;
        snprintf(result, sizeof(result),
                 "Install failed at %s: %s. Target: %s. Files committed: %d/%d. inventory exact=%d parent=%d open=%d. space rc=%d, recovery rc=%d, rollback rc=%d. No new install should start while recovery state is present.",
                 FmcbInstallStageText(report->stage),
                 FmcbInstallResultText(report->result), failed_target,
                 report->files_committed, report->files_total,
                 failed_file ? failed_file->inventory_exact_rc : -999,
                 failed_file ? failed_file->inventory_parent_rc : -999,
                 failed_file ? failed_file->inventory_open_rc : -999,
                 report->space_rc, report->recovery_rc,
                 report->rollback_rc);
''')

# More useful imaging failure diagnostics before the first page.
replace_once(
    "src/app_main.c",
'''    snprintf(message, sizeof(message),
             "%s on mc%d\\n\\nResult: %s (rc=%d)\\nPath: %s\\nPages: %u/%u\\nCRC32: %08X\\nVerified: %s%s",
             MciCardImageFormatName(report->format), report->port,
             MciCardImageResultText(report->result), rc,
             report->path[0] != '\\0' ? report->path : "n/a",
             report->pages_done, report->pages_total,
             report->logical_crc32, report->verified ? "YES" : "NO",
             destructive ? "\\n\\nThe card was modified only after the source/safety image passed verification." : "");
''',
'''    snprintf(message, sizeof(message),
             "%s on mc%d\\n\\nResult: %s (rc=%d)\\nPath: %s\\nPages: %u/%u\\nCRC32: %08X\\nVerified: %s\\nRaw stack: mcInit=%d mcInfo(issue/sync/result)=%d/%d/%d%s",
             MciCardImageFormatName(report->format), report->port,
             MciCardImageResultText(report->result), rc,
             report->path[0] != '\\0' ? report->path : "n/a",
             report->pages_done, report->pages_total,
             report->logical_crc32, report->verified ? "YES" : "NO",
             RawCardStatus.mcinit_rc, RawCardStatus.mcinfo_issue_rc,
             RawCardStatus.mcinfo_sync_rc, RawCardStatus.mcinfo_result,
             destructive ? "\\n\\nThe card was modified only after source/recovery verification." : "");
''')

old_menu = '''static void RunCardToolsMenu(int port)
{
    enum { CARD_TOOLS_ROWS = 8 };
    int row = 0;
    u32 held;
    u32 pressed;
    char body[900];

    for (;;) {
        snprintf(body, sizeof(body),
                 "Selected card: mc%d\\n\\n"
                 "%c Create verified PCSX2 .ps2 image\\n"
                 "%c Create verified OPL .vmc image\\n"
                 "%c Verify latest .ps2 image\\n"
                 "%c Verify latest .vmc image\\n"
                 "%c Exact restore latest .ps2 image\\n"
                 "%c Exact restore latest .vmc image\\n"
                 "%c Force format + verified .ps2 backup\\n"
                 "%c Return\\n\\n"
                 "Exports and restores use page-level PS2SDK MCMAN/MCSERV. Exact restore rejects different card geometry; smaller-to-larger migration remains a separate filesystem-aware operation.",
                 port,
                 row == 0 ? '>' : ' ', row == 1 ? '>' : ' ',
                 row == 2 ? '>' : ' ', row == 3 ? '>' : ' ',
                 row == 4 ? '>' : ' ', row == 5 ? '>' : ' ',
                 row == 6 ? '>' : ' ', row == 7 ? '>' : ' ');
        MciGuiRenderMessage("CARD TOOLS - Drebin", body,
                            "UP/DOWN selects. CROSS runs. CIRCLE returns.",
                            row >= 4 && row <= 6 ? MCI_GUI_TONE_WARNING : MCI_GUI_TONE_INFO);

        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE)
            return;
        if (pressed & PAD_UP) {
            row = row == 0 ? CARD_TOOLS_ROWS - 1 : row - 1;
            continue;
        }
        if (pressed & PAD_DOWN) {
            row = (row + 1) % CARD_TOOLS_ROWS;
            continue;
        }
        if (!(pressed & PAD_CROSS))
            continue;

        switch (row) {
            case 0: RunCardImageExportAction(port, MCI_CARD_IMAGE_PS2); break;
            case 1: RunCardImageExportAction(port, MCI_CARD_IMAGE_VMC); break;
            case 2: RunCardImageVerifyLatest(port, MCI_CARD_IMAGE_PS2); break;
            case 3: RunCardImageVerifyLatest(port, MCI_CARD_IMAGE_VMC); break;
            case 4: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_PS2); break;
            case 5: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_VMC); break;
            case 6: RunForceFormatWithBackup(port); break;
            default: return;
        }
    }
}
'''

new_menu = '''static int CurrentFreeClusters(int port)
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
    if (type != MC_TYPE_PS2 || !formatted || result < -2)
        return result < -2 ? result : -1;
    return free_clusters;
}

static u32 SelectedImageClusters(const MciImageSaveList *list)
{
    u32 total = 0u;
    int i;
    for (i = 0; i < list->save_count; i++)
        if (list->saves[i].selected)
            total += list->saves[i].required_clusters;
    return total;
}

static void ShowSelectiveRestoreResult(const MciImageImportReport *report, int rc)
{
    char message[520];
    snprintf(message, sizeof(message),
             "Result: %s (rc=%d)\\n\\nSelected saves: %d\\nRestored saves: %d\\nFiles: %u  directories: %u\\nBytes verified: %u\\nRequired clusters: %u  target free before: %d\\nFailed path: %s\\nRollback rc: %d",
             MciImageFsResultText(report->result), rc,
             report->selected_saves, report->restored_saves,
             report->files_written, report->directories_written,
             report->bytes_written, report->required_clusters,
             report->target_free_clusters,
             report->failed_path[0] ? report->failed_path : "none",
             report->rollback_rc);
    MciGuiRenderMessage("Selective restore", message,
                        "CROSS or CIRCLE returns to Card Tools.",
                        rc == 0 ? MCI_GUI_TONE_SUCCESS : MCI_GUI_TONE_DANGER);
    WaitForModalDismiss();
}

static void RunSelectiveRestoreLatest(int port, MciCardImageFormat format)
{
    MciImageSaveList list;
    MciImageImportReport report;
    char path[MCI_CARD_IMAGE_PATH_MAX];
    int row = 0;
    int first = 0;
    int free_clusters;
    int rc;
    u32 held;
    u32 pressed;

    /* Until the source picker lands, use the newest Drebin image for either
     * source slot. This already permits mc0-image -> mc1 and vice versa. */
    rc = MciCardImageFindLatest(port, format, path, sizeof(path));
    if (rc < 0)
        rc = MciCardImageFindLatest(port ^ 1, format, path, sizeof(path));
    if (rc < 0) {
        MciGuiRenderMessage("Image Browser",
                            "No Drebin image of this format was found in mass:/MCI. Create a backup first or place a compatible image in the managed image set.",
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_WARNING);
        WaitForModalDismiss();
        return;
    }

    rc = MciImageFsScan(path, format, &list);
    if (rc < 0 || list.save_count <= 0) {
        char message[280];
        snprintf(message, sizeof(message),
                 "The image could not be indexed as a PS2 save filesystem. Result: %s (rc=%d).",
                 MciImageFsResultText(list.result), rc);
        MciGuiRenderMessage("Image Browser", message,
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_DANGER);
        WaitForModalDismiss();
        return;
    }

    rc = MciImageFsRefreshTargetConflicts(port, &list);
    if (rc < 0) {
        MciGuiRenderMessage("Image Browser",
                            "The destination card could not be checked for existing save-directory conflicts.",
                            "CROSS or CIRCLE returns to Card Tools.",
                            MCI_GUI_TONE_DANGER);
        WaitForModalDismiss();
        return;
    }
    free_clusters = CurrentFreeClusters(port);

    for (;;) {
        MciGuiRenderImageBrowser(port, &list, row, first, free_clusters);
        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE)
            return;
        if (pressed & PAD_UP) {
            row = row == 0 ? list.save_count - 1 : row - 1;
        } else if (pressed & PAD_DOWN) {
            row = (row + 1) % list.save_count;
        }
        if (row < first)
            first = row;
        else if (row >= first + 7)
            first = row - 6;

        if ((pressed & PAD_SQUARE) && !list.saves[row].conflict) {
            if (list.saves[row].selected) {
                list.saves[row].selected = 0;
            } else if (free_clusters >= 0 &&
                       SelectedImageClusters(&list) + list.saves[row].required_clusters <=
                           (u32)free_clusters) {
                list.saves[row].selected = 1;
            }
        }

        if (pressed & PAD_TRIANGLE) {
            u32 remaining = free_clusters > 0 ? (u32)free_clusters : 0u;
            int i;
            for (i = 0; i < list.save_count; i++) {
                list.saves[i].selected = 0;
                if (!list.saves[i].conflict &&
                    list.saves[i].required_clusters <= remaining) {
                    list.saves[i].selected = 1;
                    remaining -= list.saves[i].required_clusters;
                }
            }
        }

        if ((pressed & PAD_CROSS) && SelectedImageClusters(&list) > 0u) {
            rc = MciImageFsImportSelected(port, &list, &report);
            if (rc == 0)
                ResetSlotReports(port);
            ShowSelectiveRestoreResult(&report, rc);
            return;
        }
    }
}

static void RunCardToolsMenu(int port)
{
    int item = 0;
    u32 held;
    u32 pressed;

    for (;;) {
        MciGuiRenderCardTools(port, item);
        for (;;) {
            pressed = ReadPadPressed(&held);
            if (pressed != 0u)
                break;
            DelayThread(16000);
        }
        if (pressed & PAD_CIRCLE)
            return;
        if (pressed & PAD_LEFT)
            item = (item & ~1) | ((item & 1) ^ 1);
        else if (pressed & PAD_RIGHT)
            item = (item & ~1) | ((item & 1) ^ 1);
        if (pressed & PAD_UP)
            item = item < 2 ? item + 6 : item - 2;
        else if (pressed & PAD_DOWN)
            item = item >= 6 ? item - 6 : item + 2;
        if (!(pressed & PAD_CROSS))
            continue;

        switch (item) {
            case 0: RunCardImageExportAction(port, MCI_CARD_IMAGE_PS2); break;
            case 1: RunCardImageExportAction(port, MCI_CARD_IMAGE_VMC); break;
            case 2: RunSelectiveRestoreLatest(port, MCI_CARD_IMAGE_PS2); break;
            case 3: RunSelectiveRestoreLatest(port, MCI_CARD_IMAGE_VMC); break;
            case 4: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_PS2); break;
            case 5: RunCardImageRestoreLatest(port, MCI_CARD_IMAGE_VMC); break;
            case 6: RunForceFormatWithBackup(port); break;
            default: return;
        }
    }
}
'''
replace_once("src/app_main.c", old_menu, new_menu)

# Version in controller diagnostics.
p = Path("src/app_main.c")
s = p.read_text().replace("0.4.0-dev3 Drebin", "0.4.0-dev4 Drebin")
p.write_text(s)

print("Drebin hardware blocker fixes integrated.")
