/* SPDX-License-Identifier: MIT */
/*
 * PS2 Memory Card Inspector - native GS frontend
 *
 * This renderer is intentionally based on the proven 640x224 frontend used by
 * fhdb-bootstrap-manager 0.4.0. The important hardware lesson is preserved:
 * author the UI directly in FIELD coordinates instead of drawing at 448 lines
 * and applying fractional Y transforms to an 8x8 bitmap font.
 *
 * Unlike the HDD manager, Memory Card Inspector 0.3.0 has exactly one display
 * personality. init_scr() performs the known-good CRT/read-circuit bootstrap;
 * this module then owns application rendering through libdraw/GIF DMA. There
 * is no mode switcher, alternate framebuffer reservation, or resolution menu.
 */

#include <kernel.h>
#include <tamtypes.h>
#include <debug.h>
#include <dma.h>
#include <draw.h>
#include <graph.h>
#include <gs_psm.h>
#include <packet.h>

#include <stdio.h>
#include <string.h>

#include "gui.h"

#define UI_W 640
#define UI_H 224
#define UI_FRAMES 2
#define UI_CONTEXT 0
#define UI_PACKET_QWORDS 8192
#define FONT_SRC_W 8
#define FONT_SRC_H 8
#define GLYPH_W 8
#define GLYPH_H 8
#define LINE_STEP 10
#define ATLAS_W 128
#define ATLAS_H 64

extern const u8 msx[];

typedef struct UiRgb {
    unsigned char r;
    unsigned char g;
    unsigned char b;
} UiRgb;

typedef struct UiPalette {
    UiRgb background;
    UiRgb panel;
    UiRgb panel_alt;
    UiRgb border;
    UiRgb text;
    UiRgb muted;
    UiRgb accent;
    UiRgb accent_soft;
    UiRgb success;
    UiRgb warning;
    UiRgb danger;
    UiRgb disabled;
} UiPalette;

/* Aqua is the default visual language carried over from the 0.4.0 HDD UI. */
static const UiPalette Theme = {
    {  5,  12,  18},
    { 12,  25,  33},
    { 18,  36,  47},
    { 45,  82,  98},
    {235, 246, 248},
    {139, 171, 181},
    { 35, 205, 194},
    { 20,  70,  76},
    { 72, 207, 128},
    {242, 181,  58},
    {232,  82,  91},
    { 52,  62,  68}
};

static framebuffer_t Frames[UI_FRAMES];
static zbuffer_t Zbuffer;
static texbuffer_t FontTexture;
static clutbuffer_t NoClut;
static lod_t FontLod;
static blend_t AlphaBlend;
static packet_t *RenderPacket;
static unsigned int DrawFrame = 1;
static u32 FontAtlas[ATLAS_W * ATLAS_H] __attribute__((aligned(64)));
static int RendererReady;
static int Blending = -1;

static void color_set(color_t *color, UiRgb rgb)
{
    color->r = rgb.r;
    color->g = rgb.g;
    color->b = rgb.b;
    color->a = 0x80;
    color->q = 1.0f;
}

static void blend_select(int enabled)
{
    if (Blending == enabled)
        return;
    if (enabled)
        draw_enable_blending();
    else
        draw_disable_blending();
    Blending = enabled;
}

static qword_t *rect_fill(qword_t *q, float x0, float y0,
                          float x1, float y1, UiRgb rgb)
{
    rect_t rect;

    rect.v0.x = x0;
    rect.v0.y = y0;
    rect.v0.z = 1;
    rect.v1.x = x1;
    rect.v1.y = y1;
    rect.v1.z = 1;
    color_set(&rect.color, rgb);
    blend_select(0);
    return draw_rect_filled(q, UI_CONTEXT, &rect);
}

static qword_t *rect_outline(qword_t *q, float x0, float y0,
                             float x1, float y1, UiRgb rgb)
{
    rect_t rect;

    rect.v0.x = x0;
    rect.v0.y = y0;
    rect.v0.z = 1;
    rect.v1.x = x1;
    rect.v1.y = y1;
    rect.v1.z = 1;
    color_set(&rect.color, rgb);
    blend_select(0);
    return draw_rect_outline(q, UI_CONTEXT, &rect);
}

static qword_t *glyph(qword_t *q, float x, float y,
                      unsigned char ch, const color_t *color)
{
    texrect_t g;
    unsigned int gx;
    unsigned int gy;

    if (ch >= 128u)
        ch = '?';
    gx = ((unsigned int)ch & 15u) * FONT_SRC_W;
    gy = ((unsigned int)ch >> 4) * FONT_SRC_H;

    g.v0.x = x;
    g.v0.y = y;
    g.v0.z = 2;
    g.v1.x = x + GLYPH_W;
    g.v1.y = y + GLYPH_H;
    g.v1.z = 2;
    g.t0.u = (float)gx;
    g.t0.v = (float)gy;
    g.t1.u = (float)(gx + FONT_SRC_W);
    g.t1.v = (float)(gy + FONT_SRC_H);
    g.color = *color;

    blend_select(1);
    return draw_rect_textured(q, UI_CONTEXT, &g);
}

/*
 * Word-aware wrapping keeps status prose readable. Long tokens still fall back
 * to character wrapping, but ordinary words move intact to the next line.
 */
static qword_t *text_box(qword_t *q, float x, float y,
                         float max_x, float max_y,
                         const char *value, UiRgb rgb)
{
    const char *p = value;
    float cx = x;
    float cy = y;
    color_t color;

    if (p == NULL)
        return q;
    color_set(&color, rgb);

    while (*p != '\0' && cy + GLYPH_H <= max_y) {
        unsigned char ch = (unsigned char)*p;

        if (ch == '\r') {
            p++;
            continue;
        }
        if (ch == '\n') {
            p++;
            cx = x;
            cy += LINE_STEP;
            continue;
        }

        if (ch != ' ' && ch != '\t') {
            const char *scan = p;
            int word_chars = 0;

            while (*scan != '\0' && *scan != ' ' && *scan != '\t' &&
                   *scan != '\r' && *scan != '\n') {
                word_chars++;
                scan++;
            }

            if (cx > x && cx + (float)(word_chars * GLYPH_W) > max_x) {
                cx = x;
                cy += LINE_STEP;
                if (cy + GLYPH_H > max_y)
                    break;
            }

            while (word_chars-- > 0 && cy + GLYPH_H <= max_y) {
                if (cx + GLYPH_W > max_x) {
                    cx = x;
                    cy += LINE_STEP;
                    if (cy + GLYPH_H > max_y)
                        break;
                }
                q = glyph(q, cx, cy, (unsigned char)*p++, &color);
                cx += GLYPH_W;
            }
            continue;
        }

        p++;
        if (cx + GLYPH_W > max_x) {
            cx = x;
            cy += LINE_STEP;
            if (cy + GLYPH_H > max_y)
                break;
        }
        if (cx == x)
            continue;
        q = glyph(q, cx, cy, ' ', &color);
        cx += GLYPH_W;
    }
    return q;
}

static qword_t *text(qword_t *q, float x, float y,
                     const char *value, UiRgb rgb)
{
    return text_box(q, x, y, UI_W - 12.0f, UI_H - 3.0f, value, rgb);
}

static void font_build(void)
{
    unsigned int ch;

    memset(FontAtlas, 0, sizeof(FontAtlas));
    for (ch = 0; ch < 128u; ch++) {
        unsigned int gx = (ch & 15u) * FONT_SRC_W;
        unsigned int gy = (ch >> 4) * FONT_SRC_H;
        unsigned int row;

        for (row = 0; row < FONT_SRC_H; row++) {
            unsigned char bits = msx[ch * FONT_SRC_H + row];
            unsigned int col;

            for (col = 0; col < FONT_SRC_W; col++) {
                if ((bits & (0x80u >> col)) != 0u)
                    FontAtlas[(gy + row) * ATLAS_W + gx + col] = 0x80ffffffu;
            }
        }
    }
    FlushCache(0);
}

static int frame_allocate(void)
{
    unsigned int i;

    graph_vram_clear();
    for (i = 0; i < UI_FRAMES; i++) {
        int address = graph_vram_allocate(UI_W, UI_H, GS_PSM_32,
                                          GRAPH_ALIGN_PAGE);
        if (address < 0 || (i == 0u && address != 0))
            return -1;
        Frames[i].address = (unsigned int)address;
        Frames[i].width = UI_W;
        Frames[i].height = UI_H;
        Frames[i].psm = GS_PSM_32;
        Frames[i].mask = 0;
    }
    return 0;
}

static int environment_setup(void)
{
    packet_t *packet;
    qword_t *q;
    unsigned int i;
    int texture_address;

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    if (frame_allocate() < 0)
        return -1;

    texture_address = graph_vram_allocate(ATLAS_W, ATLAS_H, GS_PSM_32,
                                          GRAPH_ALIGN_BLOCK);
    if (texture_address < 0)
        return -2;
    FontTexture.address = (unsigned int)texture_address;

    Zbuffer.enable = DRAW_DISABLE;
    Zbuffer.method = ZTEST_METHOD_ALLPASS;
    Zbuffer.address = 0;
    Zbuffer.zsm = GS_ZBUF_32;
    Zbuffer.mask = 1;

    packet = packet_init(128, PACKET_NORMAL);
    if (packet == NULL)
        return -3;
    q = packet->data;
    for (i = 0; i < UI_FRAMES; i++) {
        q = draw_setup_environment(q, UI_CONTEXT, &Frames[i], &Zbuffer);
        q = draw_clear(q, UI_CONTEXT, 0, 0, UI_W, UI_H, 0, 0, 0);
    }
    q = draw_setup_environment(q, UI_CONTEXT, &Frames[DrawFrame], &Zbuffer);
    q = draw_primitive_xyoffset(q, UI_CONTEXT, 2048.0f, 2048.0f);
    q = draw_scissor_area(q, UI_CONTEXT, 0, UI_W - 1, 0, UI_H - 1);
    q = draw_finish(q);
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data,
                            (int)(q - packet->data), 0, 0);
    draw_wait_finish();
    packet_free(packet);
    return 0;
}

static int font_upload(void)
{
    packet_t *packet;
    qword_t *q;

    font_build();
    packet = packet_init(4096, PACKET_NORMAL);
    if (packet == NULL)
        return -1;
    q = packet->data;
    q = draw_texture_transfer(q, FontAtlas, ATLAS_W, ATLAS_H, GS_PSM_32,
                              FontTexture.address, ATLAS_W);
    q = draw_texture_flush(q);
    dma_wait_fast();
    dma_channel_send_chain(DMA_CHANNEL_GIF, packet->data,
                           (int)(q - packet->data), 0, 0);
    dma_wait_fast();
    packet_free(packet);
    return 0;
}

static int texture_setup(void)
{
    packet_t *packet;
    qword_t *q;

    FontTexture.width = ATLAS_W;
    FontTexture.psm = GS_PSM_32;
    FontTexture.info.width = draw_log2(ATLAS_W);
    FontTexture.info.height = draw_log2(ATLAS_H);
    FontTexture.info.components = TEXTURE_COMPONENTS_RGBA;
    FontTexture.info.function = TEXTURE_FUNCTION_MODULATE;

    memset(&NoClut, 0, sizeof(NoClut));
    NoClut.storage_mode = CLUT_STORAGE_MODE1;
    NoClut.load_method = CLUT_NO_LOAD;

    memset(&FontLod, 0, sizeof(FontLod));
    FontLod.calculation = LOD_USE_K;
    FontLod.max_level = 0;
    FontLod.mag_filter = LOD_MAG_NEAREST;
    FontLod.min_filter = LOD_MIN_NEAREST;

    AlphaBlend.color1 = BLEND_COLOR_SOURCE;
    AlphaBlend.color2 = BLEND_COLOR_DEST;
    AlphaBlend.alpha = BLEND_ALPHA_SOURCE;
    AlphaBlend.color3 = BLEND_COLOR_DEST;
    AlphaBlend.fixed_alpha = 0x80;

    packet = packet_init(64, PACKET_NORMAL);
    if (packet == NULL)
        return -1;
    q = packet->data;
    q = draw_texture_sampling(q, UI_CONTEXT, &FontLod);
    q = draw_texturebuffer(q, UI_CONTEXT, &FontTexture, &NoClut);
    q = draw_alpha_blending(q, UI_CONTEXT, &AlphaBlend);
    q = draw_finish(q);
    dma_wait_fast();
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data,
                            (int)(q - packet->data), 0, 0);
    draw_wait_finish();
    packet_free(packet);
    return 0;
}

static qword_t *frame_begin(packet_t **packet_out)
{
    qword_t *q;

    dma_wait_fast();
    q = RenderPacket->data;
    q = draw_framebuffer(q, UI_CONTEXT, &Frames[DrawFrame]);
    *packet_out = RenderPacket;
    return q;
}

static void frame_end(packet_t *packet, qword_t *q)
{
    unsigned int complete = DrawFrame;

    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data,
                            (int)(q - packet->data), 0, 0);
    draw_wait_finish();
    graph_wait_vsync();
    graph_set_framebuffer_filtered(Frames[complete].address,
                                   Frames[complete].width,
                                   Frames[complete].psm, 0, 0);
    DrawFrame ^= 1u;
}

static UiRgb tone_color(MciGuiTone tone)
{
    switch (tone) {
        case MCI_GUI_TONE_SUCCESS: return Theme.success;
        case MCI_GUI_TONE_WARNING: return Theme.warning;
        case MCI_GUI_TONE_DANGER: return Theme.danger;
        case MCI_GUI_TONE_INFO:
        default: return Theme.accent;
    }
}

static UiRgb card_health_color(CardHealth health)
{
    switch (health) {
        case CARD_OK: return Theme.success;
        case CARD_FULL:
        case CARD_UNFORMATTED: return Theme.warning;
        case CARD_UNKNOWN:
        case CARD_NO_CARD: return Theme.muted;
        default: return Theme.danger;
    }
}

static UiRgb mg_color(MagicGateResult result)
{
    switch (result) {
        case MG_RESULT_PASS: return Theme.success;
        case MG_RESULT_NOT_RUN: return Theme.muted;
        case MG_RESULT_TARGET_NOT_PS2:
        case MG_RESULT_NO_TEST_KELF: return Theme.warning;
        default: return Theme.danger;
    }
}

static UiRgb package_color(FmcbPackageStatus status)
{
    switch (status) {
        case FMCB_PACKAGE_READY: return Theme.success;
        case FMCB_PACKAGE_NOT_SCANNED: return Theme.muted;
        case FMCB_PACKAGE_INCOMPLETE:
        case FMCB_PACKAGE_NOT_FOUND: return Theme.warning;
        default: return Theme.danger;
    }
}

static const char *card_short_status(CardHealth health)
{
    switch (health) {
        case CARD_OK: return "PASS";
        case CARD_FULL: return "FULL";
        case CARD_UNFORMATTED: return "NO FORMAT";
        case CARD_FILESYSTEM_BROKEN: return "FS BROKEN";
        case CARD_IO_FAILURE: return "I/O FAIL";
        case CARD_AUTH_FAILURE: return "AUTH FAIL";
        case CARD_DETECT_FAILURE: return "DETECT FAIL";
        case CARD_NO_CARD: return "NO CARD";
        default: return "UNKNOWN";
    }
}

static const char *mg_short_status(MagicGateResult result)
{
    const char *full = MagicGateResultText(result);

    if (strcmp(full, "FUNCTIONAL") == 0)
        return "FUNCTIONAL";
    if (strncmp(full, "NOT SUPPORTED", 13) == 0)
        return "NO MG ACK";
    if (strncmp(full, "PROTOCOL ERROR", 14) == 0)
        return "PROTOCOL";
    if (strncmp(full, "TEST INDETERMINATE", 18) == 0)
        return "INDETERM.";

    switch (result) {
        case MG_RESULT_NOT_RUN: return "NOT RUN";
        case MG_RESULT_NO_TEST_KELF: return "NO KELF";
        case MG_RESULT_TARGET_NOT_PS2: return "NOT PS2";
        case MG_RESULT_KBIT_FAILED: return "KBIT FAIL";
        case MG_RESULT_KC_FAILED: return "KC FAIL";
        case MG_RESULT_RPC_UNAVAILABLE: return "RPC FAIL";
        default: return "FAILED";
    }
}

static const char *package_short_status(FmcbPackageStatus status)
{
    switch (status) {
        case FMCB_PACKAGE_READY: return "READY";
        case FMCB_PACKAGE_INCOMPLETE: return "MISSING";
        case FMCB_PACKAGE_NOT_FOUND: return "NO PKG";
        case FMCB_PACKAGE_SOURCE_UNAVAILABLE: return "NO SOURCE";
        case FMCB_PACKAGE_UNSUPPORTED_CONSOLE: return "REGION ?";
        default: return "NOT SCAN";
    }
}

static qword_t *shell(qword_t *q, MciGuiPage page, int selected)
{
    static const char *const tabs[MCI_GUI_PAGE_COUNT] = {
        "CARD", "MAGICGATE", "FMCB PREFLIGHT"
    };
    char version[64];
    unsigned int i;
    float tab_x[4] = {14.0f, 146.0f, 322.0f, 626.0f};

    q = rect_fill(q, 0, 0, UI_W, UI_H, Theme.background);
    q = rect_fill(q, 0, 0, UI_W, 4, Theme.accent);
    q = rect_fill(q, 12, 8, 628, 31, Theme.panel);
    q = rect_outline(q, 12, 8, 628, 31, Theme.border);
    q = text(q, 22, 14, "PS2 Memory Card Inspector", Theme.text);
    snprintf(version, sizeof(version), "v0.3.0-dev3  mc%d", selected);
    q = text_box(q, 470, 14, 618, 23, version, Theme.accent);

    for (i = 0; i < MCI_GUI_PAGE_COUNT; i++) {
        UiRgb bg = i == (unsigned int)page ? Theme.panel_alt : Theme.panel;
        UiRgb fg = i == (unsigned int)page ? Theme.accent : Theme.muted;
        q = rect_fill(q, tab_x[i], 35, tab_x[i + 1] - 4, 51, bg);
        if (i == (unsigned int)page)
            q = rect_fill(q, tab_x[i], 49, tab_x[i + 1] - 4, 51,
                          Theme.accent);
        q = text_box(q, tab_x[i] + 8, 40, tab_x[i + 1] - 12, 48,
                     tabs[i], fg);
    }
    return q;
}

static qword_t *slot_summary(qword_t *q, int selected,
                             const CardReport cards[2],
                             const MagicGateReport magicgate[2],
                             const FmcbPackageReport packages[2])
{
    int i;

    q = rect_fill(q, 12, 55, 151, 202, Theme.panel);
    q = rect_outline(q, 12, 55, 151, 202, Theme.border);
    q = text(q, 22, 62, "MEMORY CARDS", Theme.muted);

    for (i = 0; i < 2; i++) {
        float y0 = 77.0f + (float)i * 57.0f;
        UiRgb border = i == selected ? Theme.accent : Theme.border;
        char line[48];

        q = rect_fill(q, 20, y0, 143, y0 + 49, Theme.panel_alt);
        q = rect_outline(q, 20, y0, 143, y0 + 49, border);
        if (i == selected)
            q = rect_fill(q, 20, y0, 24, y0 + 49, Theme.accent);
        snprintf(line, sizeof(line), "SLOT %d   mc%d:", i + 1, i);
        q = text_box(q, 30, y0 + 5, 137, y0 + 13, line, Theme.text);

        q = text(q, 30, y0 + 18, "FS", Theme.muted);
        q = text_box(q, 54, y0 + 18, 141, y0 + 26,
                     card_short_status(cards[i].health),
                     card_health_color(cards[i].health));
        q = text(q, 30, y0 + 29, "MG", Theme.muted);
        q = text_box(q, 54, y0 + 29, 141, y0 + 37,
                     mg_short_status(magicgate[i].result),
                     mg_color(magicgate[i].result));
        q = text(q, 30, y0 + 40, "PKG", Theme.muted);
        q = text_box(q, 62, y0 + 40, 141, y0 + 48,
                     package_short_status(packages[i].status),
                     package_color(packages[i].status));
    }
    return q;
}

static qword_t *panel_title(qword_t *q, float x0, float y0,
                            float x1, float y1,
                            const char *title, UiRgb accent)
{
    q = rect_fill(q, x0, y0, x1, y1, Theme.panel);
    q = rect_outline(q, x0, y0, x1, y1, Theme.border);
    q = rect_fill(q, x0, y0, x0 + 4, y1, accent);
    q = text_box(q, x0 + 12, y0 + 6, x1 - 8, y0 + 14,
                 title, Theme.muted);
    return q;
}

static qword_t *value_line(qword_t *q, float x, float y,
                           const char *label, const char *value, UiRgb value_color)
{
    q = text(q, x, y, label, Theme.muted);
    q = text_box(q, x + 112, y, 615, y + 8, value, value_color);
    return q;
}

static qword_t *render_card(qword_t *q, int selected,
                            const CardReport cards[2],
                            const MagicGateReport magicgate[2],
                            const FmcbPackageReport packages[2],
                            int confirm_format, int last_format_rc)
{
    const CardReport *r = &cards[selected];
    char line[128];

    q = slot_summary(q, selected, cards, magicgate, packages);
    q = panel_title(q, 158, 55, 628, 88, "FILESYSTEM HEALTH",
                    card_health_color(r->health));
    q = text_box(q, 170, 73, 615, 81, CardHealthText(r->health),
                 card_health_color(r->health));

    q = rect_fill(q, 158, 93, 628, 169, Theme.panel);
    q = rect_outline(q, 158, 93, 628, 169, Theme.border);
    snprintf(line, sizeof(line), "%d (%s)", r->info_rc,
             CardResultText(r->info_rc));
    q = value_line(q, 172, 101, "mcGetInfo", line, Theme.text);
    snprintf(line, sizeof(line), "%d (%s)", r->type, CardTypeText(r->type));
    q = value_line(q, 172, 113, "Card type", line, Theme.text);
    snprintf(line, sizeof(line), "%d    free=%d", r->formatted, r->free_clusters);
    q = value_line(q, 172, 125, "Formatted", line, Theme.text);
    snprintf(line, sizeof(line), "%d (%s)", r->root_rc,
             CardResultText(r->root_rc));
    q = value_line(q, 172, 137, "Root dir", line, Theme.text);
    snprintf(line, sizeof(line), "%s  rc=%d cleanup=%d",
             CardRwStageText(r->rw_stage), r->rw_rc, r->cleanup_rc);
    q = value_line(q, 172, 149, "4 KiB R/W", line,
                   r->rw_rc == 0 ? Theme.success : Theme.text);

    if (confirm_format) {
        q = rect_fill(q, 158, 174, 628, 202, Theme.panel_alt);
        q = rect_outline(q, 158, 174, 628, 202, Theme.danger);
        q = text(q, 172, 180, "FORMAT CONFIRMATION - ALL DATA WILL BE ERASED",
                 Theme.danger);
        q = text(q, 172, 191, "Hold L1 + R1 and press TRIANGLE. CIRCLE cancels.",
                 Theme.warning);
    } else {
        q = rect_fill(q, 158, 174, 628, 202, Theme.panel);
        q = rect_outline(q, 158, 174, 628, 202, Theme.border);
        if (r->format_allowed)
            q = text(q, 172, 181, "TRIANGLE  Arm destructive format", Theme.warning);
        else
            q = text(q, 172, 181, "Format locked for this card state", Theme.muted);
        if (last_format_rc != -999) {
            snprintf(line, sizeof(line), "Last format rc: %d (%s)",
                     last_format_rc, CardResultText(last_format_rc));
            q = text_box(q, 172, 191, 615, 199, line,
                         last_format_rc == 0 ? Theme.success : Theme.danger);
        } else {
            q = text(q, 172, 191,
                     "Filesystem and MagicGate are evaluated independently.",
                     Theme.muted);
        }
    }
    return q;
}

static qword_t *render_magicgate(qword_t *q, int selected,
                                 const CardReport cards[2],
                                 const MagicGateReport magicgate[2],
                                 const MagicGateIopStatus *mg_iop,
                                 const FmcbPackageReport packages[2])
{
    const MagicGateReport *mg = &magicgate[selected];
    char line[160];

    q = slot_summary(q, selected, cards, magicgate, packages);
    q = panel_title(q, 158, 55, 628, 88, "MAGICGATE / KELF CAPABILITY",
                    mg_color(mg->result));
    q = text_box(q, 170, 73, 615, 81, MagicGateResultText(mg->result),
                 mg_color(mg->result));

    q = rect_fill(q, 158, 93, 628, 123, Theme.panel);
    q = rect_outline(q, 158, 93, 628, 123, Theme.border);
    q = text(q, 172, 100, "Stage", Theme.muted);
    q = text_box(q, 236, 100, 615, 108, MagicGateStageText(mg->stage), Theme.text);
    if (mg->source_path[0] != '\0') {
        snprintf(line, sizeof(line), "%s  (%d bytes)", mg->source_path, mg->source_size);
        q = text_box(q, 172, 111, 615, 119, line, Theme.muted);
    } else {
        q = text(q, 172, 111, "KELF source: not prepared", Theme.muted);
    }

    q = rect_fill(q, 158, 128, 389, 202, Theme.panel);
    q = rect_outline(q, 158, 128, 389, 202, Theme.border);
    q = text(q, 172, 136, "SECURITY SESSION", Theme.muted);
    snprintf(line, sizeof(line), "setup %d   mcInit %d", mg->session_setup_rc,
             mg->session_mcinit_rc);
    q = text(q, 172, 149, line, Theme.text);
    snprintf(line, sizeof(line), "mcInfo %d  type %d fmt %d", mg->session_mcinfo_rc,
             mg->session_type, mg->session_formatted);
    q = text(q, 172, 160, line, Theme.text);
    snprintf(line, sizeof(line), "restore %d   RPC %d", mg->restore_rc, mg->rpc_rc);
    q = text(q, 172, 171, line,
             mg->restore_rc == 0 ? Theme.success : Theme.text);
    if (mg_iop != NULL) {
        snprintf(line, sizeof(line), "SECRSIF %d/%d", mg_iop->secrsif_load_rc,
                 mg_iop->secrsif_start_rc);
        q = text(q, 172, 182, line, Theme.text);
    }
    q = text(q, 172, 193, "Backend: PS2SDK 2.0 SECRMAN 1.4", Theme.accent);

    q = rect_fill(q, 395, 128, 628, 202, Theme.panel);
    q = rect_outline(q, 395, 128, 628, 202, Theme.border);
    q = text(q, 409, 136, "BIND PIPELINE", Theme.muted);
    snprintf(line, sizeof(line), "Header %d  reply %d", mg->header_rc,
             mg->header_reply_size);
    q = text(q, 409, 149, line, Theme.text);
    snprintf(line, sizeof(line), "BIT %d  enc %d  done %d", mg->block_count,
             mg->encrypted_blocks, mg->blocks_completed);
    q = text(q, 409, 160, line, Theme.text);
    snprintf(line, sizeof(line), "Kbit %d   Kc %d", mg->kbit_rc, mg->kc_rc);
    q = text(q, 409, 171, line,
             mg->kbit_rc == 1 && mg->kc_rc == 1 ? Theme.success : Theme.text);
    if (mg->icvps2_required) {
        snprintf(line, sizeof(line), "ICVPS2 %d", mg->icvps2_rc);
        q = text(q, 409, 182, line, Theme.text);
    } else {
        q = text(q, 409, 182, "ICVPS2 N/A", Theme.muted);
    }
    q = text(q, 409, 193, "RAM-only; no KELF is written", Theme.muted);
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

    q = slot_summary(q, selected, cards, magicgate, packages);
    q = panel_title(q, 158, 55, 628, 88, "FREEMCBOOT PACKAGE PREFLIGHT",
                    package_color(r->status));
    q = text_box(q, 170, 73, 615, 81, FmcbPackageStatusText(r->status),
                 package_color(r->status));

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
             r->plan.destination_system[0] ? r->plan.destination_system : "unresolved");
    q = text_box(q, 172, 152, 408, 160, line, Theme.text);
    snprintf(line, sizeof(line), "Required %d/%d  missing %d",
             r->found_required, r->plan.required_files, r->missing_required);
    q = text(q, 172, 164, line,
             r->missing_required == 0 && r->status == FMCB_PACKAGE_READY
                 ? Theme.success : Theme.text);
    snprintf(line, sizeof(line), "Optional %d/%d   KELF %d",
             r->found_optional, r->plan.optional_files, r->plan.kelf_files);
    q = text(q, 172, 176, line, Theme.text);
    q = text(q, 172, 190, "0.3 write path: NOT ENABLED YET", Theme.warning);

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

static qword_t *footer(qword_t *q, MciGuiPage page, int confirm_format)
{
    const char *line;

    q = rect_fill(q, 0, 205, UI_W, UI_H, Theme.panel_alt);
    if (confirm_format) {
        line = "L1+R1+TRIANGLE Confirm format        CIRCLE Cancel";
    } else if (page == MCI_GUI_MAGICGATE) {
        line = "LEFT/RIGHT Slot   SQUARE Probe   R1 Next page   SELECT Exit";
    } else if (page == MCI_GUI_FMCB) {
        line = "LEFT/RIGHT Slot   CIRCLE Scan   R1 Next page   SELECT Exit";
    } else {
        line = "LEFT/RIGHT Slot   X Test   START Both   R1 Next page   SELECT Exit";
    }
    q = text_box(q, 18, 211, 622, 220, line, Theme.muted);
    return q;
}

int MciGuiInit(void)
{
    if (RendererReady)
        return 0;
    if (environment_setup() < 0)
        return -1;
    if (font_upload() < 0)
        return -2;
    if (texture_setup() < 0)
        return -3;

    RenderPacket = packet_init(UI_PACKET_QWORDS, PACKET_NORMAL);
    if (RenderPacket == NULL)
        return -4;
    RendererReady = 1;
    MciGuiRenderMessage("Starting 0.3.0",
                        "Native Graphics Synthesizer frontend ready.\n"
                        "Using the hardware-proven 640x224 layout; no video mode selector.",
                        NULL, MCI_GUI_TONE_INFO);
    return 0;
}

int MciGuiReady(void)
{
    return RendererReady;
}

void MciGuiRenderDashboard(int selected,
                           MciGuiPage page,
                           const CardReport cards[2],
                           const MagicGateReport magicgate[2],
                           const MagicGateIopStatus *mg_iop,
                           const FmcbMassBackendStatus *mass,
                           const FmcbPackageReport packages[2],
                           int confirm_format,
                           int last_format_rc)
{
    packet_t *packet;
    qword_t *q;

    if (!RendererReady || cards == NULL || magicgate == NULL || packages == NULL)
        return;
    if (selected < 0 || selected > 1)
        selected = 0;
    if ((unsigned int)page >= MCI_GUI_PAGE_COUNT)
        page = MCI_GUI_CARD;

    q = frame_begin(&packet);
    q = shell(q, page, selected);
    if (page == MCI_GUI_MAGICGATE)
        q = render_magicgate(q, selected, cards, magicgate, mg_iop, packages);
    else if (page == MCI_GUI_FMCB)
        q = render_fmcb(q, selected, cards, magicgate, mass, packages);
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
    q = rect_fill(q, 12, 8, 628, 31, Theme.panel);
    q = rect_outline(q, 12, 8, 628, 31, Theme.border);
    q = text(q, 22, 14, "PS2 Memory Card Inspector  v0.3.0-dev3", Theme.text);
    q = text_box(q, 20, 39, 620, 48,
                 title != NULL ? title : "Status", accent);
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
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    accent = tone_color(tone);
    fill_x = inner_x0 + (inner_x1 - inner_x0) * ((float)percent / 100.0f);
    snprintf(pct, sizeof(pct), "%3d%%", percent);

    q = frame_begin(&packet);
    q = rect_fill(q, 0, 0, UI_W, UI_H, Theme.background);
    q = rect_fill(q, 0, 0, UI_W, 4, accent);
    q = rect_fill(q, 12, 8, 628, 31, Theme.panel);
    q = rect_outline(q, 12, 8, 628, 31, Theme.border);
    q = text(q, 22, 14, "PS2 Memory Card Inspector  v0.3.0-dev3", Theme.text);

    q = text_box(q, 20, 39, 620, 48,
                 title != NULL ? title : "Working", accent);
    q = rect_fill(q, 16, 53, 624, 196, Theme.panel);
    q = rect_outline(q, 16, 53, 624, 196, Theme.border);

    q = text_box(q, 30, 65, 610, 75,
                 action != NULL ? action : "Working...", Theme.text);
    q = text_box(q, 30, 84, 610, 121,
                 detail != NULL ? detail : "", Theme.muted);

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

void MciGuiRenderFatal(const char *title, const char *body, int code)
{
    char message[512];

    snprintf(message, sizeof(message), "%s\n\nRaw error code: %d",
             body != NULL ? body : "Fatal error", code);
    MciGuiRenderMessage(title != NULL ? title : "Fatal error",
                        message,
                        "Reset or power-cycle the console.",
                        MCI_GUI_TONE_DANGER);
}
