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
static int ActiveHeaderSlot = 0;

static qword_t *identity_banner(qword_t *q)
{
    char version[80];

    q = rect_fill(q, 12, 8, 628, 31, Theme.panel);
    q = rect_outline(q, 12, 8, 628, 31, Theme.border);
    q = text(q, 22, 14, "PS2 Memory Card Inspector", Theme.text);
    snprintf(version, sizeof(version), "v0.4.0-dev1  mc%d", ActiveHeaderSlot);
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

    /* The install action is the only passive marquee on this page. Reset it
     * whenever the card or preflight state changes so the user always gets the
     * readable beginning before the text starts moving. */
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
    snprintf(line, sizeof(line), "mass: %s   source: %s",
             mass != NULL && mass->available ? "AVAILABLE" : "UNAVAILABLE",
             r->source_root[0] ? r->source_root : "not resolved");
    q = text_box(q, 172, 101, 615, 109, line, Theme.text);
    snprintf(line, sizeof(line), "probe rc=%d   payload=%u bytes",
             r->source_probe_rc, r->total_found_bytes);
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

    /* Never let the action hint paint into the adjacent panel. If it does not
     * fit, use the same accessibility-oriented bounded marquee as SETTINGS:
     * start hold -> 240 ms/glyph -> 2 s end hold -> instant reset. */
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
