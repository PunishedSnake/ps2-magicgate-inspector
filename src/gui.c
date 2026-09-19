/* SPDX-License-Identifier: MIT */
/*
 * PS2 Memory Card Inspector - 0.4 GUI composition layer.
 *
 * The hardware/display renderer and shared page helpers remain in
 * gui_core.inc. 0.4 keeps the FMCB preflight page in this thin composition
 * layer so its bounded single-line fields can evolve without disturbing the
 * GS backend that was already hardware-validated.
 */

/* Keep selected core renderers available as internal fallbacks while replacing
 * only the page composition and operation-screen identity banner below. The
 * include remains the same translation unit, so its static renderer helpers
 * and state stay private and directly reusable here. */
#define render_fmcb render_fmcb_base
#define MciGuiRenderDashboard MciGuiRenderDashboard_base
#define MciGuiRenderMessage MciGuiRenderMessage_base
#define MciGuiRenderProgress MciGuiRenderProgress_base
#include "gui_core.inc"
#undef MciGuiRenderProgress
#undef MciGuiRenderMessage
#undef MciGuiRenderDashboard
#undef render_fmcb

static int LastFmcbMarqueeSlot = -1;
static int LastFmcbMarqueeStatus = -1;
static int LastMagicGateMarqueeSlot = -1;
static char LastMagicGateMarqueeSource[192];
static int ActiveHeaderSlot = 0;

static qword_t *identity_banner(qword_t *q)
{
    char version[80];

    q = rect_fill(q, 12, 8, 628, 31, Theme.panel);
    q = rect_outline(q, 12, 8, 628, 31, Theme.border);
    q = text(q, 22, 14, "PS2 Memory Card Inspector", Theme.text);
    snprintf(version, sizeof(version), "v0.4.0-dev4 Drebin  mc%d", ActiveHeaderSlot);
    q = text_box(q, 462, 14, 618, 23, version, Theme.accent);
    return q;
}

static qword_t *render_fmcb(qword_t *q, int selected,
                            const CardReport cards[2],
                            const MagicGateReport magicgate[2],
                            const FmcbMassBackendStatus *mass,
                            const FmcbPackageReport packages[2])
{
    const FmcbPackageReport *r = &packages[selected];
    char line[160];
    int shown = 0;
    int i;

    if (selected != LastFmcbMarqueeSlot ||
        (int)r->status != LastFmcbMarqueeStatus) {
        MarqueeEpoch = GetTimerSystemTime();
        LastFmcbMarqueeSlot = selected;
        LastFmcbMarqueeStatus = (int)r->status;
    }

    q = slot_summary(q, selected, cards, magicgate, packages);
    q = panel_title(q, 158, 55, 628, 88,
                    "FREEMCBOOT PACKAGE / INSTALLER", package_color(r->status));
    q = text_box(q, 170, 73, 615, 81,
                 FmcbPackageStatusText(r->status), package_color(r->status));

    q = rect_fill(q, 158, 93, 628, 126, Theme.panel);
    q = rect_outline(q, 158, 93, 628, 126, Theme.border);
    snprintf(line, sizeof(line), "USB storage: %s   package data: %u KiB",
             mass != NULL && mass->available ? "READY" : "NOT AVAILABLE",
             (r->total_found_bytes + 1023u) / 1024u);
    q = text_box(q, 172, 101, 615, 109, line, Theme.text);
    if (r->source_root[0] != '\0') {
        size_t root_len = strlen(r->source_root);
        const char *root_tail = root_len > 43u
                                    ? r->source_root + root_len - 43u
                                    : r->source_root;
        snprintf(line, sizeof(line), "Found at: %s%.43s",
                 root_len > 43u ? "..." : "", root_tail);
    } else {
        snprintf(line, sizeof(line), "Package location: not found yet");
    }
    q = text_box(q, 172, 113, 615, 121, line, Theme.muted);

    q = rect_fill(q, 158, 131, 418, 202, Theme.panel);
    q = rect_outline(q, 158, 131, 418, 202, Theme.border);
    q = text(q, 172, 139, "INSTALL PLAN", Theme.muted);

    snprintf(line, sizeof(line), "Region %c -> %s",
             r->plan.region_letter ? r->plan.region_letter : '?',
             r->plan.destination_system[0]
                 ? r->plan.destination_system : "unresolved");
    q = text_box(q, 172, 152, 408, 160, line, Theme.text);

    snprintf(line, sizeof(line), "Required %d/%d  missing %d",
             r->found_required, r->plan.required_files, r->missing_required);
    q = text_box(q, 172, 164, 408, 172, line,
                 r->missing_required == 0 && r->status == FMCB_PACKAGE_READY
                     ? Theme.success : Theme.text);

    snprintf(line, sizeof(line), "Optional %d/%d   KELF %d",
             r->found_optional, r->plan.optional_files, r->plan.kelf_files);
    q = text_box(q, 172, 176, 408, 184, line, Theme.text);

    q = selected_single_line(
        q, 172, 190, 408, 198,
        r->status == FMCB_PACKAGE_READY
            ? "SQUARE  Open verified installer"
            : "Run preflight before installation",
        r->status == FMCB_PACKAGE_READY ? Theme.accent : Theme.warning);

    q = rect_fill(q, 424, 131, 628, 202, Theme.panel);
    q = rect_outline(q, 424, 131, 628, 202, Theme.border);
    q = text(q, 438, 139, "MISSING REQUIRED", Theme.muted);
    if (r->missing_required == 0) {
        q = text(q, 438, 154, "None", Theme.success);
    } else {
        for (i = 0; i < r->entry_count && shown < 4; i++) {
            const FmcbPackageFileStatus *f = &r->files[i];
            if (!f->found && (f->flags & FMCB_FILE_REQUIRED)) {
                q = text_box(q, 438, 153.0f + (float)shown * 11.0f,
                             616, 161.0f + (float)shown * 11.0f,
                             f->relative_path, Theme.warning);
                shown++;
            }
        }
    }
    return q;
}

void MciGuiRenderDashboard(int selected,
                           MciGuiPage page,
                           const CardReport cards[2],
                           const MagicGateReport magicgate[2],
                           const MagicGateIopStatus *mg_iop,
                           const FmcbMassBackendStatus *mass,
                           const FmcbPackageReport packages[2],
                           const MciSettings *settings,
                           int settings_row,
                           int last_video_rc,
                           int confirm_format,
                           int last_format_rc)
{
    packet_t *packet;
    qword_t *q;

    if (!RendererReady || cards == NULL || magicgate == NULL ||
        packages == NULL || settings == NULL)
        return;
    if (selected < 0 || selected > 1)
        selected = 0;
    ActiveHeaderSlot = selected;
    if ((unsigned int)page >= MCI_GUI_PAGE_COUNT)
        page = MCI_GUI_CARD;

    if (page != MCI_GUI_SETTINGS) {
        LastSettingsMarqueeRow = -1;
        LastSettingsMarqueeState = ~0u;
    }
    if (page != MCI_GUI_FMCB) {
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
                     sizeof(LastMagicGateMarqueeSource), "%.*s",
                     (int)sizeof(LastMagicGateMarqueeSource) - 1, source);
        }
    } else {
        LastMagicGateMarqueeSlot = -1;
        LastMagicGateMarqueeSource[0] = '\0';
    }

    q = frame_begin(&packet);
    q = shell(q, page, selected);
    if (page == MCI_GUI_MAGICGATE)
        q = render_magicgate(q, selected, cards, magicgate, mg_iop, packages);
    else if (page == MCI_GUI_FMCB)
        q = render_fmcb(q, selected, cards, magicgate, mass, packages);
    else if (page == MCI_GUI_SETTINGS)
        q = render_settings(q, settings, settings_row, last_video_rc);
    else
        q = render_card(q, selected, cards, magicgate, packages,
                        confirm_format, last_format_rc);
    q = footer(q, page, confirm_format);
    frame_end(packet, q);
}

void MciGuiRenderMessage(const char *title,
                         const char *body,
                         const char *footer_text,
                         MciGuiTone tone)
{
    packet_t *packet;
    qword_t *q;
    UiRgb accent;
    float body_bottom;

    if (!RendererReady)
        return;
    accent = tone_color(tone);
    body_bottom = footer_text != NULL && footer_text[0] != '\0' ? 196.0f : 212.0f;

    q = frame_begin(&packet);
    q = rect_fill(q, 0, 0, UI_W, UI_H, Theme.background);
    q = rect_fill(q, 0, 0, UI_W, 4, accent);
    q = identity_banner(q);
    q = text_box(q, 20, 39, 620, 48, title != NULL ? title : "Status", accent);
    q = rect_fill(q, 16, 53, 624, body_bottom, Theme.panel);
    q = rect_outline(q, 16, 53, 624, body_bottom, Theme.border);
    q = text_box(q, 30, 64, 610, body_bottom - 8.0f,
                 body != NULL ? body : "", Theme.text);
    if (footer_text != NULL && footer_text[0] != '\0') {
        q = rect_fill(q, 0, 202, UI_W, UI_H, Theme.panel_alt);
        q = text_box(q, 20, 211, 620, 220, footer_text, Theme.muted);
    }
    frame_end(packet, q);
}

void MciGuiRenderProgress(const char *title,
                          const char *action,
                          const char *detail,
                          int percent,
                          const char *footer_text,
                          MciGuiTone tone)
{
    packet_t *packet;
    qword_t *q;
    UiRgb accent;
    char pct[16];
    float inner_x0 = 32.0f;
    float inner_x1 = 608.0f;
    float fill_x;

    if (!RendererReady)
        return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    accent = tone_color(tone);
    fill_x = inner_x0 + (inner_x1 - inner_x0) * ((float)percent / 100.0f);
    snprintf(pct, sizeof(pct), "%3d%%", percent);

    q = frame_begin(&packet);
    q = rect_fill(q, 0, 0, UI_W, UI_H, Theme.background);
    q = rect_fill(q, 0, 0, UI_W, 4, accent);
    q = identity_banner(q);
    q = text_box(q, 20, 39, 620, 48, title != NULL ? title : "Working", accent);
    q = rect_fill(q, 16, 53, 624, 196, Theme.panel);
    q = rect_outline(q, 16, 53, 624, 196, Theme.border);
    q = text_box(q, 30, 65, 610, 75,
                 action != NULL ? action : "Working...", Theme.text);
    q = text_box(q, 30, 84, 610, 121, detail != NULL ? detail : "", Theme.muted);
    q = text(q, 30, 130, "PROGRESS", Theme.muted);
    q = text_box(q, 566, 130, 610, 138, pct, accent);
    q = rect_fill(q, 30, 145, 610, 164, Theme.panel_alt);
    q = rect_outline(q, 30, 145, 610, 164, Theme.border);
    if (percent > 0)
        q = rect_fill(q, inner_x0, 148, fill_x, 161, accent);
    q = text_box(q, 30, 176, 610, 187,
                 percent >= 100 ? "Operation complete; returning to the dashboard."
                                : "Working synchronously; controls resume when this step completes.",
                 percent >= 100 ? Theme.success : Theme.muted);
    if (footer_text != NULL && footer_text[0] != '\0') {
        q = rect_fill(q, 0, 202, UI_W, UI_H, Theme.panel_alt);
        q = text_box(q, 20, 211, 620, 220, footer_text, Theme.muted);
    }
    frame_end(packet, q);
}

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
                 "L1 mc0   R1 mc1   DPAD Move   X Open   CIRCLE Back", Theme.muted);
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
    snprintf(line, sizeof(line), "SOURCE  %s  %s%.47s",
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
                 "L1 mc0  R1 mc1  UP/DOWN Move  SQUARE Select  TRIANGLE Fit  X Restore  CIRCLE Back",
                 Theme.muted);
    frame_end(packet, q);
}
