/* SPDX-License-Identifier: MIT */
/*
 * Save Transfer/Card Tools v2 composition layer.
 *
 * Keep the already hardware-qualified GUI implementation intact and extend it
 * in the same translation unit so the private GS primitives/theme remain
 * reusable without exporting a second renderer API surface.
 */

#include "gui.c"

void MciGuiRenderCardToolsV2(int selected, int selected_item)
{
    static const char *const titles[8] = {
        "BACKUP  PCSX2 .ps2", "BACKUP  OPL .vmc",
        "BROWSE / RESTORE IMAGE", "EXACT RESTORE IMAGE",
        "IMPORT SINGLE SAVE", "EXPORT SAVE TO .psu",
        "FORCE FORMAT + BACKUP", "RETURN"
    };
    static const char *const hints[8] = {
        "Create a full verified raw image with regenerated ECC.",
        "Create a full verified 512-byte-page VMC image.",
        "Pick any compatible .ps2/.vmc file and restore selected saves.",
        "Pick an image for destructive same-geometry page restore.",
        "Pick a supported historical save container and import it.",
        "Choose one save on the active card and export interoperable PSU.",
        "Create and verify a .ps2 recovery image before formatting.",
        "Return to the Card dashboard."
    };
    packet_t *packet;
    qword_t *q;
    char context[96];
    int i;

    if (!RendererReady)
        return;
    if (selected < 0 || selected > 1)
        selected = 0;
    if (selected_item < 0 || selected_item >= 8)
        selected_item = 0;
    ActiveHeaderSlot = selected;

    q = frame_begin(&packet);
    q = rect_fill(q, 0, 0, UI_W, UI_H, Theme.background);
    q = rect_fill(q, 0, 0, UI_W, 4, Theme.accent);
    q = identity_banner(q);
    q = text(q, 18, 39, "CARD TOOLS / SAVE TRANSFER", Theme.accent);
    snprintf(context, sizeof(context),
             "active mc%d   L1 mc0   R1 mc1", selected);
    q = text_box(q, 355, 39, 620, 48, context, Theme.muted);

    for (i = 0; i < 8; i++) {
        int col = i & 1;
        int row = i >> 1;
        float x0 = col ? 322.0f : 16.0f;
        float x1 = col ? 624.0f : 318.0f;
        float y0 = 54.0f + (float)row * 35.0f;
        float y1 = y0 + 31.0f;
        UiRgb border = i == selected_item ? Theme.accent : Theme.border;
        UiRgb title_color = (i == 3 || i == 6)
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
                 "DPAD Move   X Open   L1 mc0   R1 mc1   CIRCLE Back",
                 Theme.muted);
    frame_end(packet, q);
}
