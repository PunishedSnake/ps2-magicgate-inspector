/* SPDX-License-Identifier: MIT */
/*
 * PS2 Memory Card Inspector - native GS frontend, 0.4 generation
 *
 * The logical UI remains the hardware-proven 640x224 design validated during
 * 0.3 development. 0.4 ports the corrected runtime display architecture from
 * fhdb-bootstrap-manager 0.4.3: native FIELD output is the recovery baseline,
 * while 480p/576p/720p/1080i use a separately reserved VRAM area and a
 * pixel-snapped logical viewport. This avoids stretching the old framebuffer
 * by guesswork and keeps text/panels aligned in every mode.
 */

#include <kernel.h>
#include <rom0_info.h>
#include <syscallnr.h>
#include <tamtypes.h>
#include <timer.h>

#include <debug.h>
#include <dma.h>
#include <dma_registers.h>
#include <draw.h>
#include <gif_registers.h>
#include <graph.h>
#include <gs_privileged.h>
#include <gs_psm.h>
#include <packet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "ui_layout.h"
#include "video_mode.h"

#define UI_W 640
#define UI_H 224
#define UI_FRAMES 2
#define UI_CONTEXT 0
#define UI_PACKET_QWORDS 12288
#define FONT_SRC_W 8
#define FONT_SRC_H 8
#define GLYPH_W 8
#define GLYPH_H 8
#define LINE_STEP 10
#define ATLAS_W 128
#define ATLAS_H 64

#define ALT_STORAGE_W 640
/*
 * The alternate slots are allocated as PSM16 storage but are reinterpreted as
 * PSM32 framebuffers. A 640x540 PSM32 GS buffer spans 10 x 17 = 170 GS pages
 * because PSMCT32 pages are 64x32 pixels. PS2SDK graph_vram_size() uses a
 * linear byte count and rounds 640x540 to only 169 pages. Reserving 1080 PSM16
 * lines therefore places the second 1080i buffer's final GS page on top of the
 * font atlas. 1088 PSM16 lines advance the allocator by the required 170 pages
 * per slot while keeping all other alternate modes inside the same reservation.
 */
#define ALT_STORAGE_H 1088
#define ALT_STORAGE_PSM GS_PSM_16
#define GS_PAGE_WORDS 2048u
#define PSMCT32_PAGE_W 64u
#define PSMCT32_PAGE_H 32u
/* Accessibility-first marquee. Only the selected setting can animate.
 * Scroll a whole glyph every 240 ms, keep the beginning readable briefly,
 * hold the fully visible end for two seconds, then snap directly back to the
 * beginning instead of making the sentence travel through a carousel gap. */
#define MARQUEE_STEP_MS 240u
#define MARQUEE_START_HOLD_MS 1500u
#define MARQUEE_END_HOLD_MS 2000u

#define VSYNC_TIMEOUT_MS 250u
#define GIF_TIMEOUT_MS 250u
#define GIF_CHCR (*(volatile u32 *)0x1000A000)
#define NATIVE_PMODE 0x000000000000FF62ULL
#define NATIVE_DISPFB2 0x0000000000001400ULL
#define NATIVE_DISPLAY2 0x001BF9FF0983227CULL

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

typedef struct MciVideoSpec {
    MciVideoMode id;
    int interlace;
    int graph_mode;
    int frame_mode;
    int flicker_filter;
    int screen_x;
    int screen_y;
    unsigned int psm;
    int filtered_presentation;
    int explicit_display;
    int crt_mode;
    int display_x;
    int display_y;
    unsigned int magh;
    unsigned int magv;
    unsigned int display_width;
    unsigned int display_height;
} MciVideoSpec;

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

/* Timings/display windows are intentionally kept in lockstep with the
 * hardware-tested fhdb-bootstrap-manager 0.4.3 implementation. */
static const MciVideoSpec VideoSpecs[MCI_VIDEO_MODE_COUNT] = {
    {MCI_VIDEO_NATIVE,
     GRAPH_MODE_INTERLACED, GRAPH_MODE_AUTO, GRAPH_MODE_FIELD, GRAPH_ENABLE,
     0, 0, GS_PSM_32, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {MCI_VIDEO_480P,
     GRAPH_MODE_NONINTERLACED, GRAPH_MODE_HDTV_480P, GRAPH_MODE_FRAME,
     GRAPH_DISABLE, 0, 0, GS_PSM_32, 0, 0,
     0x50, 232, 35, 1, 0, 1440, 448},
    {MCI_VIDEO_576P,
     GRAPH_MODE_NONINTERLACED, GRAPH_MODE_HDTV_576P, GRAPH_MODE_FRAME,
     GRAPH_DISABLE, 0, 0, GS_PSM_32, 0, 1,
     0x53, 255, 44, 1, 0, 1440, 576},
    {MCI_VIDEO_720P,
     GRAPH_MODE_NONINTERLACED, GRAPH_MODE_HDTV_720P, GRAPH_MODE_FRAME,
     GRAPH_DISABLE, 0, 0, GS_PSM_32, 0, 1,
     0x52, 306, 24, 1, 0, 1280, 720},
    {MCI_VIDEO_1080I,
     GRAPH_MODE_INTERLACED, GRAPH_MODE_HDTV_1080I, GRAPH_MODE_FRAME,
     GRAPH_DISABLE, 0, 0, GS_PSM_32, 0, 1,
     0x51, 236, 47, 2, 0, 1920, 1080}
};

static framebuffer_t NativeFrames[UI_FRAMES];
static framebuffer_t AltFrames[UI_FRAMES];
static framebuffer_t *ActiveFrames = NativeFrames;
static unsigned int ActiveFrameCount = UI_FRAMES;
static zbuffer_t Zbuffer;
static texbuffer_t FontTexture;
static clutbuffer_t NoClut;
static lod_t FontLod;
static blend_t AlphaBlend;
static packet_t *RenderPacket;
static packet_t *BootstrapPacket;
static packet_t *ClearPacket;
static unsigned int DrawFrame = 1;
static u32 FontAtlas[ATLAS_W * ATLAS_H] __attribute__((aligned(64)));
static int RendererReady;
static int Blending = -1;
static int DrawStateDirty;
static MciVideoMode CurrentVideoMode = MCI_VIDEO_NATIVE;
static MciUiLayout Layout;
static int FilteredPresentation = 1;
static u64 MarqueeEpoch;
static int MarqueeNeeded;
static int LastSettingsMarqueeRow = -1;
static unsigned int LastSettingsMarqueeState = ~0u;

static int wait_gif_idle(unsigned int timeout_ms)
{
    u64 start = GetTimerSystemTime();
    u64 timeout = MSec2TimerBusClock(timeout_ms);

    while ((GIF_CHCR & 0x100u) != 0u) {
        if (GetTimerSystemTime() - start >= timeout)
            return -1;
    }
    return 0;
}

static int wait_finish(unsigned int timeout_ms)
{
    u64 start = GetTimerSystemTime();
    u64 timeout = MSec2TimerBusClock(timeout_ms);

    while ((*GS_REG_CSR & 2u) == 0u) {
        if (GetTimerSystemTime() - start >= timeout)
            return -1;
    }
    *GS_REG_CSR = 2u;
    return 0;
}

static int wait_vsync(unsigned int timeout_ms)
{
    u64 start = GetTimerSystemTime();
    u64 timeout = MSec2TimerBusClock(timeout_ms);

    *GS_REG_CSR = 8u;
    while ((*GS_REG_CSR & 8u) == 0u) {
        if (GetTimerSystemTime() - start >= timeout)
            return -1;
    }
    return 0;
}

static int submit_and_wait(packet_t *packet, qword_t *q)
{
    int rc;

    if (wait_gif_idle(GIF_TIMEOUT_MS) < 0)
        return -1;
    *GS_REG_CSR = 2u;
    __asm__ __volatile__("sync.l; sync.p");
    rc = dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data,
                                 (int)(q - packet->data), 0, 0);
    __asm__ __volatile__("sync.l; sync.p");
    if (rc < 0 || wait_gif_idle(GIF_TIMEOUT_MS) < 0 ||
        wait_finish(GIF_TIMEOUT_MS) < 0)
        return -1;
    return 0;
}

static void reset_gif_path(void)
{
    GIF_CHCR = 0u;
    GIF_REG_CTRL = GIF_SET_CTRL(1, 0);
    __asm__ __volatile__("sync.l; sync.p");
    GIF_REG_CTRL = GIF_SET_CTRL(0, 0);
    *DMA_REG_STAT = DMA_SET_STAT(1u << DMA_CHANNEL_GIF, 0, 0, 0, 0, 0, 0);
    (void)dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
}

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

    rect.v0.x = MciUiSnapX(&Layout, x0);
    rect.v0.y = MciUiSnapY(&Layout, y0);
    rect.v0.z = 1;
    rect.v1.x = MciUiSnapX(&Layout, x1);
    rect.v1.y = MciUiSnapY(&Layout, y1);
    rect.v1.z = 1;
    color_set(&rect.color, rgb);
    blend_select(0);
    return draw_rect_filled(q, UI_CONTEXT, &rect);
}

static qword_t *rect_outline(qword_t *q, float x0, float y0,
                             float x1, float y1, UiRgb rgb)
{
    rect_t rect;

    rect.v0.x = MciUiSnapX(&Layout, x0);
    rect.v0.y = MciUiSnapY(&Layout, y0);
    rect.v0.z = 1;
    rect.v1.x = MciUiSnapX(&Layout, x1);
    rect.v1.y = MciUiSnapY(&Layout, y1);
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
    unsigned int px;
    unsigned int py;
    unsigned int pw;
    unsigned int ph;

    if (ch >= 128u)
        ch = '?';
    gx = ((unsigned int)ch & 15u) * FONT_SRC_W;
    gy = ((unsigned int)ch >> 4) * FONT_SRC_H;
    MciUiTextCell(&Layout, x, y, &px, &py, &pw, &ph);

    g.v0.x = (float)px;
    g.v0.y = (float)py;
    g.v0.z = 2;
    g.v1.x = (float)(px + pw);
    g.v1.y = (float)(py + ph);
    g.v1.z = 2;
    g.t0.u = (float)gx;
    g.t0.v = (float)gy;
    g.t1.u = (float)(gx + FONT_SRC_W);
    g.t1.v = (float)(gy + FONT_SRC_H);
    g.color = *color;

    blend_select(1);
    return draw_rect_textured(q, UI_CONTEXT, &g);
}

static qword_t *text_marquee_box(qword_t *q, float x, float y,
                                 float max_x, const char *value, UiRgb rgb)
{
    size_t length = strcspn(value, "\r\n");
    unsigned int cells = max_x > x ? (unsigned int)((max_x - x) / GLYPH_W) : 0u;
    unsigned int max_offset;
    unsigned int offset;
    unsigned int i;
    u64 elapsed;
    u64 step_ticks;
    u64 start_hold_ticks;
    u64 end_hold_ticks;
    u64 scroll_ticks;
    u64 cycle_ticks;
    u64 phase;
    color_t color;

    if (cells == 0u || length == 0u)
        return q;
    if (length <= cells)
        return q;

    color_set(&color, rgb);
    MarqueeNeeded = 1;
    max_offset = (unsigned int)length - cells;
    elapsed = GetTimerSystemTime() - MarqueeEpoch;
    step_ticks = MSec2TimerBusClock(MARQUEE_STEP_MS);
    start_hold_ticks = MSec2TimerBusClock(MARQUEE_START_HOLD_MS);
    end_hold_ticks = MSec2TimerBusClock(MARQUEE_END_HOLD_MS);
    scroll_ticks = (u64)max_offset * step_ticks;
    cycle_ticks = start_hold_ticks + scroll_ticks + end_hold_ticks;
    phase = cycle_ticks != 0u ? elapsed % cycle_ticks : 0u;

    if (phase < start_hold_ticks) {
        offset = 0u;
    } else if (phase < start_hold_ticks + scroll_ticks) {
        offset = (unsigned int)((phase - start_hold_ticks) / step_ticks);
        if (offset > max_offset)
            offset = max_offset;
    } else {
        offset = max_offset;
    }

    for (i = 0; i < cells; i++) {
        unsigned int pos = offset + i;
        unsigned char ch = pos < length ? (unsigned char)value[pos] : ' ';
        q = glyph(q, x + (float)(i * GLYPH_W), y, ch, &color);
    }
    return q;
}

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

static qword_t *selected_single_line(qword_t *q, float x, float y,
                                     float max_x, float max_y,
                                     const char *value, UiRgb rgb)
{
    size_t length;
    unsigned int cells;

    if (value == NULL)
        return q;
    length = strcspn(value, "\r\n");
    cells = max_x > x ? (unsigned int)((max_x - x) / GLYPH_W) : 0u;
    if (length > cells)
        return text_marquee_box(q, x, y, max_x, value, rgb);
    return text_box(q, x, y, max_x, max_y, value, rgb);
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

static int allocate_pair(framebuffer_t pair[UI_FRAMES],
                         unsigned int width, unsigned int height,
                         unsigned int psm, int require_zero)
{
    unsigned int i;

    for (i = 0; i < UI_FRAMES; i++) {
        int address = graph_vram_allocate(width, height, psm, GRAPH_ALIGN_PAGE);
        if (address < 0 || (require_zero && i == 0u && address != 0))
            return -1;
        pair[i].address = (unsigned int)address;
        pair[i].width = width;
        pair[i].height = height;
        pair[i].psm = psm;
        pair[i].mask = 0;
    }
    return 0;
}

static int apply_layout(const MciVideoSpec *spec)
{
    const MciVideoGeometry *g = MciVideoModeGeometry(spec->id);

    if (g == NULL)
        return -1;
    if (MciUiLayoutConfigure(&Layout, g->surface_width, g->surface_height,
                             g->frame_width, g->frame_height,
                             g->viewport_x, g->viewport_y,
                             g->viewport_width, g->viewport_height) < 0)
        return -2;
    FilteredPresentation = spec->filtered_presentation;
    return 0;
}

static void configure_alt_frames(const MciVideoSpec *spec)
{
    const MciVideoGeometry *g = MciVideoModeGeometry(spec->id);
    unsigned int i;

    for (i = 0; i < UI_FRAMES; i++) {
        AltFrames[i].width = g->frame_width;
        AltFrames[i].height = g->frame_height;
        AltFrames[i].psm = spec->psm;
        AltFrames[i].mask = 0;
    }
}

static unsigned int psmct32_page_footprint_words(unsigned int width,
                                                  unsigned int height)
{
    unsigned int pages_x = (width + PSMCT32_PAGE_W - 1u) / PSMCT32_PAGE_W;
    unsigned int pages_y = (height + PSMCT32_PAGE_H - 1u) / PSMCT32_PAGE_H;

    return pages_x * pages_y * GS_PAGE_WORDS;
}

static int environment_setup(void)
{
    packet_t *packet;
    qword_t *q;
    unsigned int i;
    int texture_address;

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    graph_vram_clear();
    if (allocate_pair(NativeFrames, UI_W, UI_H, GS_PSM_32, 1) < 0)
        return -1;
    /* Reserve a contiguous block large enough for every 0.4.3 display layout.
     * The bases stay fixed; per-mode stride/height/PSM are changed in place. */
    if (allocate_pair(AltFrames, ALT_STORAGE_W, ALT_STORAGE_H,
                      ALT_STORAGE_PSM, 0) < 0)
        return -2;

    texture_address = graph_vram_allocate(ATLAS_W, ATLAS_H, GS_PSM_32,
                                          GRAPH_ALIGN_BLOCK);
    if (texture_address < 0)
        return -3;
    {
        const MciVideoGeometry *g1080 = MciVideoModeGeometry(MCI_VIDEO_1080I);
        unsigned int frame_words;

        if (g1080 == NULL)
            return -3;
        frame_words = psmct32_page_footprint_words(g1080->frame_width,
                                                   g1080->frame_height);
        /* Fail closed if a future geometry/allocation change can alias either
         * 1080i framebuffer with its neighbour or with the font texture. */
        if (AltFrames[1].address < AltFrames[0].address + frame_words ||
            (unsigned int)texture_address < AltFrames[1].address + frame_words)
            return -3;
    }
    FontTexture.address = (unsigned int)texture_address;

    Zbuffer.enable = DRAW_DISABLE;
    Zbuffer.method = ZTEST_METHOD_ALLPASS;
    Zbuffer.address = 0;
    Zbuffer.zsm = GS_ZBUF_32;
    Zbuffer.mask = 1;

    packet = packet_init(256, PACKET_NORMAL);
    if (packet == NULL)
        return -4;
    q = packet->data;
    for (i = 0; i < UI_FRAMES; i++) {
        q = draw_setup_environment(q, UI_CONTEXT, &NativeFrames[i], &Zbuffer);
        q = draw_clear(q, UI_CONTEXT, 0, 0, UI_W, UI_H, 0, 0, 0);
    }
    for (i = 0; i < UI_FRAMES; i++) {
        q = draw_setup_environment(q, UI_CONTEXT, &AltFrames[i], &Zbuffer);
        q = draw_clear(q, UI_CONTEXT, 0, 0,
                       ALT_STORAGE_W, ALT_STORAGE_H, 0, 0, 0);
    }

    ActiveFrames = NativeFrames;
    ActiveFrameCount = UI_FRAMES;
    DrawFrame = 1u;
    if (apply_layout(&VideoSpecs[MCI_VIDEO_NATIVE]) < 0) {
        packet_free(packet);
        return -5;
    }
    q = draw_setup_environment(q, UI_CONTEXT, &ActiveFrames[DrawFrame], &Zbuffer);
    q = draw_primitive_xyoffset(q, UI_CONTEXT, 2048.0f, 2048.0f);
    q = draw_scissor_area(q, UI_CONTEXT, 0, UI_W - 1, 0, UI_H - 1);
    q = draw_finish(q);
    if (submit_and_wait(packet, q) < 0) {
        packet_free(packet);
        return -6;
    }
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
    if (submit_and_wait(packet, q) < 0) {
        packet_free(packet);
        return -2;
    }
    packet_free(packet);
    return 0;
}

static void present_framebuffer(const framebuffer_t *frame)
{
    if (FilteredPresentation)
        graph_set_framebuffer_filtered(frame->address, frame->width,
                                       frame->psm, 0, 0);
    else
        graph_set_framebuffer(1, frame->address, frame->width,
                              frame->psm, 0, 0);
}

static int clear_active_frames(framebuffer_t pair[UI_FRAMES],
                               unsigned int count)
{
    qword_t *q;
    unsigned int i;

    if (ClearPacket == NULL || count == 0u || count > UI_FRAMES)
        return -1;
    packet_reset(ClearPacket);
    q = ClearPacket->data;
    for (i = 0; i < count; i++) {
        q = draw_setup_environment(q, UI_CONTEXT, &pair[i], &Zbuffer);
        q = draw_clear(q, UI_CONTEXT, 0, 0,
                       pair[i].width, pair[i].height, 0, 0, 0);
    }
    q = draw_finish(q);
    return submit_and_wait(ClearPacket, q);
}

static int rom_version_number(void)
{
    char romname[16];

    memset(romname, 0, sizeof(romname));
    GetRomName(romname);
    return (int)strtol(romname, NULL, 10);
}

static int setup_legacy_576p(void)
{
    u64 gcont = ((u64)GetGsVParam() & 1u) << 25;

    if (graph_set_mode(GRAPH_MODE_NONINTERLACED, GRAPH_MODE_HDTV_480P,
                       GRAPH_MODE_FRAME, GRAPH_DISABLE) < 0)
        return -1;
    *GS_REG_SMODE1 = 0x00000017404B0504ULL | gcont;
    *GS_REG_SYNCH1 = 0x000402E02003C827ULL;
    *GS_REG_SYNCH2 = 0x000000000019CA67ULL;
    *GS_REG_SYNCHV = 0x00A9000002700005ULL;
    *GS_REG_SMODE2 = 0u;
    *GS_REG_SRFSH = 4u;
    *GS_REG_SMODE1 = 0x0000001740490504ULL | gcont;
    __asm__ __volatile__("sync.l; sync.p");
    return 0;
}

static void program_explicit_display(const MciVideoSpec *spec)
{
    int dx = spec->display_x;
    int dy = spec->display_y;
    u64 display;

    if (GetSyscallHandler(__NR__GetGsDxDyOffset) != NULL) {
        int ox, oy, ignored_w, ignored_h;
        _GetGsDxDyOffset(spec->crt_mode, &ox, &oy, &ignored_w, &ignored_h);
        dx += ox;
        dy += oy;
    }
    if (spec->id == MCI_VIDEO_1080I)
        *GS_REG_SMODE2 = GS_SET_SMODE2(1, 1, 0);
    display = GS_SET_DISPLAY(dx, dy, spec->magh, spec->magv,
                             spec->display_width - 1u,
                             spec->display_height - 1u);
    *GS_REG_DISPLAY1 = display;
    *GS_REG_DISPLAY2 = display;
}

static int bootstrap_native(void)
{
    qword_t *q;

    graph_disable_output();
    *GS_REG_CSR = 0x200u;
    GsPutIMR(0xff00u);
    SetGsCrt(GRAPH_MODE_INTERLACED, graph_get_region(), GRAPH_MODE_FIELD);
    __asm__ __volatile__("sync.l; sync.p");

    ActiveFrames = NativeFrames;
    ActiveFrameCount = UI_FRAMES;
    DrawFrame = 1u;
    if (apply_layout(&VideoSpecs[MCI_VIDEO_NATIVE]) < 0)
        return -1;

    *GS_REG_DISPFB1 = 0u;
    *GS_REG_DISPFB2 = NATIVE_DISPFB2;
    *GS_REG_DISPLAY1 = 0u;
    *GS_REG_DISPLAY2 = NATIVE_DISPLAY2;
    *GS_REG_BGCOLOR = 0u;

    packet_reset(BootstrapPacket);
    q = BootstrapPacket->data;
    q = draw_setup_environment(q, UI_CONTEXT, &NativeFrames[DrawFrame], &Zbuffer);
    q = draw_primitive_xyoffset(q, UI_CONTEXT, 2048.0f, 2048.0f);
    q = draw_scissor_area(q, UI_CONTEXT, 0, UI_W - 1, 0, UI_H - 1);
    q = draw_texture_sampling(q, UI_CONTEXT, &FontLod);
    q = draw_texturebuffer(q, UI_CONTEXT, &FontTexture, &NoClut);
    q = draw_alpha_blending(q, UI_CONTEXT, &AlphaBlend);
    q = draw_finish(q);
    if (submit_and_wait(BootstrapPacket, q) < 0)
        return -2;
    *GS_REG_PMODE = NATIVE_PMODE;
    __asm__ __volatile__("sync.l; sync.p");
    graph_enable_output();
    Blending = -1;
    DrawStateDirty = 0;
    CurrentVideoMode = MCI_VIDEO_NATIVE;
    return 0;
}

static int restore_native(int hard)
{
    int rc;

    if (hard || wait_gif_idle(GIF_TIMEOUT_MS) < 0)
        reset_gif_path();
    rc = bootstrap_native();
    if (rc < 0 && !hard) {
        reset_gif_path();
        rc = bootstrap_native();
    }
    return rc;
}

int MciGuiApplyVideoMode(MciVideoMode mode)
{
    const MciVideoSpec *spec;
    const MciVideoGeometry *g;
    int rc;

    if (!RendererReady)
        return -1;
    if ((unsigned int)mode >= MCI_VIDEO_MODE_COUNT)
        return -2;
    if (mode == CurrentVideoMode)
        return 0;
    if (wait_gif_idle(GIF_TIMEOUT_MS) < 0)
        return -3;
    if (wait_vsync(VSYNC_TIMEOUT_MS) < 0)
        return -4;
    if (mode == MCI_VIDEO_NATIVE)
        return restore_native(0);

    spec = &VideoSpecs[(unsigned int)mode];
    g = MciVideoModeGeometry(mode);
    if (g == NULL)
        return -5;

    graph_disable_output();
    if (mode == MCI_VIDEO_576P && rom_version_number() < 220)
        rc = setup_legacy_576p();
    else
        rc = graph_set_mode(spec->interlace, spec->graph_mode,
                            spec->frame_mode, spec->flicker_filter);
    if (rc < 0) {
        (void)restore_native(1);
        return -6;
    }

    if (spec->explicit_display) {
        program_explicit_display(spec);
    } else if (graph_set_screen(spec->screen_x, spec->screen_y,
                                g->surface_width, g->surface_height) < 0) {
        (void)restore_native(1);
        return -7;
    }

    configure_alt_frames(spec);
    if (clear_active_frames(AltFrames, g->frame_count) < 0) {
        (void)restore_native(1);
        return -8;
    }
    ActiveFrames = AltFrames;
    ActiveFrameCount = g->frame_count;
    if (apply_layout(spec) < 0) {
        (void)restore_native(1);
        return -9;
    }
    CurrentVideoMode = mode;
    DrawFrame = 0u;
    graph_set_bgcolor(0, 0, 0);
    present_framebuffer(&AltFrames[ActiveFrameCount > 1u ? 1u : 0u]);
    Blending = -1;
    DrawStateDirty = 1;
    graph_enable_output();
    if (wait_vsync(VSYNC_TIMEOUT_MS) < 0) {
        (void)restore_native(1);
        return -10;
    }
    return 0;
}

MciVideoMode MciGuiCurrentVideoMode(void)
{
    return CurrentVideoMode;
}

static qword_t *frame_begin(packet_t **packet_out)
{
    qword_t *q;

    MarqueeNeeded = 0;
    if (wait_gif_idle(GIF_TIMEOUT_MS) < 0)
        reset_gif_path();
    packet_reset(RenderPacket);
    q = RenderPacket->data;
    if (DrawStateDirty) {
        q = draw_setup_environment(q, UI_CONTEXT, &ActiveFrames[DrawFrame], &Zbuffer);
        q = draw_primitive_xyoffset(q, UI_CONTEXT, 2048.0f, 2048.0f);
        q = draw_scissor_area(q, UI_CONTEXT, 0,
                              Layout.active_width - 1u,
                              0, Layout.active_height - 1u);
        q = draw_texture_sampling(q, UI_CONTEXT, &FontLod);
        q = draw_texturebuffer(q, UI_CONTEXT, &FontTexture, &NoClut);
        q = draw_alpha_blending(q, UI_CONTEXT, &AlphaBlend);
        DrawStateDirty = 0;
    } else {
        q = draw_framebuffer(q, UI_CONTEXT, &ActiveFrames[DrawFrame]);
    }
    q = draw_clear(q, UI_CONTEXT, 0, 0,
                   Layout.active_width, Layout.active_height, 0, 0, 0);
    *packet_out = RenderPacket;
    return q;
}

static void frame_end(packet_t *packet, qword_t *q)
{
    unsigned int complete = DrawFrame;

    if (ActiveFrameCount == 1u && wait_vsync(VSYNC_TIMEOUT_MS) < 0) {
        (void)restore_native(1);
        return;
    }
    q = draw_finish(q);
    if (submit_and_wait(packet, q) < 0) {
        (void)restore_native(1);
        return;
    }
    if (ActiveFrameCount > 1u && wait_vsync(VSYNC_TIMEOUT_MS) < 0) {
        (void)restore_native(1);
        return;
    }
    if (CurrentVideoMode == MCI_VIDEO_1080I &&
        wait_vsync(VSYNC_TIMEOUT_MS) < 0) {
        (void)restore_native(1);
        return;
    }
    present_framebuffer(&ActiveFrames[complete]);
    DrawFrame = (DrawFrame + 1u) % ActiveFrameCount;
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
        "CARD", "MAGICGATE", "FMCB PREFLIGHT", "SETTINGS"
    };
    static const float tab_x[MCI_GUI_PAGE_COUNT + 1] = {
        14.0f, 126.0f, 270.0f, 472.0f, 626.0f
    };
    char version[80];
    unsigned int i;

    q = rect_fill(q, 0, 0, UI_W, UI_H, Theme.background);
    q = rect_fill(q, 0, 0, UI_W, 4, Theme.accent);
    q = rect_fill(q, 12, 8, 628, 31, Theme.panel);
    q = rect_outline(q, 12, 8, 628, 31, Theme.border);
    q = text(q, 22, 14, "PS2 Memory Card Inspector", Theme.text);
    snprintf(version, sizeof(version), "v0.4.0-dev3 Drebin  mc%d", selected);
    q = text_box(q, 462, 14, 618, 23, version, Theme.accent);

    for (i = 0; i < MCI_GUI_PAGE_COUNT; i++) {
        UiRgb bg = i == (unsigned int)page ? Theme.panel_alt : Theme.panel;
        UiRgb fg = i == (unsigned int)page ? Theme.accent : Theme.muted;
        q = rect_fill(q, tab_x[i], 35, tab_x[i + 1] - 4, 51, bg);
        if (i == (unsigned int)page)
            q = rect_fill(q, tab_x[i], 49, tab_x[i + 1] - 4, 51, Theme.accent);
        q = text_box(q, tab_x[i] + 7, 40, tab_x[i + 1] - 10, 48,
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
    q = text_box(q, x0 + 12, y0 + 6, x1 - 8, y0 + 14, title, Theme.muted);
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
    q = panel_title(q, 158, 55, 628, 88, "CARD & FILESYSTEM",
                    card_health_color(r->health));
    q = text_box(q, 170, 73, 615, 81, CardHealthText(r->health),
                 card_health_color(r->health));

    q = rect_fill(q, 158, 93, 628, 169, Theme.panel);
    q = rect_outline(q, 158, 93, 628, 169, Theme.border);
    snprintf(line, sizeof(line), "%s",
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

    if (confirm_format) {
        q = rect_fill(q, 158, 174, 628, 202, Theme.panel_alt);
        q = rect_outline(q, 158, 174, 628, 202, Theme.danger);
        q = text(q, 172, 180, "FORMAT CONFIRMATION - ALL DATA WILL BE ERASED", Theme.danger);
        q = text(q, 172, 191, "Hold L1 + R1 and press TRIANGLE. CIRCLE cancels.", Theme.warning);
    } else {
        q = rect_fill(q, 158, 174, 628, 202, Theme.panel);
        q = rect_outline(q, 158, 174, 628, 202, Theme.border);
        if (r->format_allowed)
            q = text(q, 172, 181, "TRIANGLE  Arm destructive format", Theme.warning);
        else
            q = text(q, 172, 181, "Format locked for this card state", Theme.muted);
        if (last_format_rc != -999) {
            snprintf(line, sizeof(line), "Last format result: %s",
                     CardResultText(last_format_rc));
            q = text_box(q, 172, 191, 615, 199, line,
                         last_format_rc == 0 ? Theme.success : Theme.danger);
        } else {
            q = text(q, 172, 191, "Filesystem and MagicGate are evaluated independently.", Theme.muted);
        }
    }
    return q;
}

static const char *magicgate_user_step(const MagicGateReport *mg)
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
{
    const MagicGateReport *mg = &magicgate[selected];
    char line[256];

    q = slot_summary(q, selected, cards, magicgate, packages);
    q = panel_title(q, 158, 55, 628, 88, "MAGICGATE / KELF CAPABILITY", mg_color(mg->result));
    q = text_box(q, 170, 73, 615, 81, MagicGateResultText(mg->result), mg_color(mg->result));

    q = rect_fill(q, 158, 93, 628, 123, Theme.panel);
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

    q = rect_fill(q, 158, 128, 389, 202, Theme.panel);
    q = rect_outline(q, 158, 128, 389, 202, Theme.border);
    q = text(q, 172, 136, "SECURITY SESSION", Theme.muted);
    snprintf(line, sizeof(line), "setup %d   mcInit %d", mg->session_setup_rc, mg->session_mcinit_rc);
    q = text(q, 172, 149, line, Theme.text);
    snprintf(line, sizeof(line), "mcInfo %d  type %d fmt %d", mg->session_mcinfo_rc,
             mg->session_type, mg->session_formatted);
    q = text(q, 172, 160, line, Theme.text);
    snprintf(line, sizeof(line), "restore %d   RPC %d", mg->restore_rc, mg->rpc_rc);
    q = text(q, 172, 171, line, mg->restore_rc == 0 ? Theme.success : Theme.text);
    if (mg_iop != NULL) {
        snprintf(line, sizeof(line), "SECRSIF %d/%d", mg_iop->secrsif_load_rc, mg_iop->secrsif_start_rc);
        q = text(q, 172, 182, line, Theme.text);
    }
    q = text(q, 172, 193, "Backend: PS2SDK 2.0 SECRMAN 1.4", Theme.accent);

    q = rect_fill(q, 395, 128, 628, 202, Theme.panel);
    q = rect_outline(q, 395, 128, 628, 202, Theme.border);
    q = text(q, 409, 136, "BIND PIPELINE", Theme.muted);
    snprintf(line, sizeof(line), "Header %d  reply %d", mg->header_rc, mg->header_reply_size);
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
    q = text(q, 409, 193, "Probe is RAM-only", Theme.muted);
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
    q = panel_title(q, 158, 55, 628, 88, "FREEMCBOOT PACKAGE / INSTALLER", package_color(r->status));
    q = text_box(q, 170, 73, 615, 81, FmcbPackageStatusText(r->status), package_color(r->status));

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
             r->missing_required == 0 && r->status == FMCB_PACKAGE_READY ? Theme.success : Theme.text);
    snprintf(line, sizeof(line), "Optional %d/%d   KELF %d",
             r->found_optional, r->plan.optional_files, r->plan.kelf_files);
    q = text(q, 172, 176, line, Theme.text);
    q = text(q, 172, 190,
             r->status == FMCB_PACKAGE_READY ? "SQUARE  Open verified installer" : "Run preflight before installation",
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

static qword_t *settings_row(qword_t *q, int row, int selected,
                             const char *name, const char *value,
                             const char *hint)
{
    float y0 = 70.0f + (float)row * 30.0f;
    UiRgb bg = row == selected ? Theme.panel_alt : Theme.panel;
    UiRgb border = row == selected ? Theme.accent : Theme.border;

    q = rect_fill(q, 24, y0, 616, y0 + 26, bg);
    q = rect_outline(q, 24, y0, 616, y0 + 26, border);
    if (row == selected)
        q = rect_fill(q, 24, y0, 29, y0 + 26, Theme.accent);
    q = text_box(q, 38, y0 + 4, 310, y0 + 12, name, Theme.text);
    if (row == selected) {
        q = selected_single_line(q, 322, y0 + 4, 604, y0 + 12,
                                 value, Theme.accent);
        q = selected_single_line(q, 38, y0 + 15, 604, y0 + 23,
                                 hint, Theme.muted);
    } else {
        q = text_box(q, 322, y0 + 4, 604, y0 + 12, value, Theme.accent);
        q = text_box(q, 38, y0 + 15, 604, y0 + 23, hint, Theme.muted);
    }
    return q;
}

static qword_t *render_settings(qword_t *q,
                                const MciSettings *settings,
                                int selected_row, int last_video_rc)
{
    char video[96];
    unsigned int state;

    state = ((unsigned int)settings->video_mode & 0xffu) |
            (((unsigned int)settings->fs_profile & 0xffu) << 8) |
            ((settings->preserve_existing_cnfs ? 1u : 0u) << 16) |
            (((unsigned int)settings->install_verify_mode & 0xffu) << 17);
    if (selected_row != LastSettingsMarqueeRow ||
        state != LastSettingsMarqueeState) {
        MarqueeEpoch = GetTimerSystemTime();
        LastSettingsMarqueeRow = selected_row;
        LastSettingsMarqueeState = state;
    }

    q = panel_title(q, 12, 55, 628, 69, "SETTINGS", Theme.accent);
    snprintf(video, sizeof(video), "%s%s",
             MciVideoModeName(settings->video_mode),
             settings->video_mode == MciGuiCurrentVideoMode() ? "  [ACTIVE]" : "  [PENDING]");
    q = settings_row(q, 0, selected_row, "Display mode", video,
                     "LEFT/RIGHT chooses a mode; X applies it. Native is the recovery fallback.");
    q = settings_row(q, 1, selected_row, "Filesystem test",
                     MciFsTestProfileName(settings->fs_profile),
                     "Quick is the 0.3 baseline; extended profiles increase disposable verification traffic.");
    q = settings_row(q, 2, selected_row, "Preserve existing CNF",
                     settings->preserve_existing_cnfs ? "YES" : "NO",
                     "Installer can keep an existing FREEMCB.CNF instead of replacing user configuration.");
    q = settings_row(q, 3, selected_row, "Install read-back verify",
                     MciInstallVerifyModeName(settings->install_verify_mode),
                     settings->install_verify_mode == MCI_INSTALL_VERIFY_ENFORCED
                         ? "Every selected installer file is reopened and byte-compared before commit."
                         : settings->install_verify_mode == MCI_INSTALL_VERIFY_REQUIRED
                               ? "Required files are reopened and compared; optional payloads skip the second read."
                               : "Read-back comparison is disabled; durable USB rollback protection remains active.");

    (void)last_video_rc;
    return q;
}

static qword_t *footer(qword_t *q, MciGuiPage page, int confirm_format)
{
    const char *line;

    q = rect_fill(q, 0, 205, UI_W, UI_H, Theme.panel_alt);
    if (confirm_format) {
        line = "L1+R1+TRIANGLE Confirm format        CIRCLE Cancel";
    } else if (page == MCI_GUI_SETTINGS) {
        line = "UP/DOWN Setting   LEFT/RIGHT Change   X Apply   L1/R1 Page   SELECT Exit";
    } else if (page == MCI_GUI_FMCB) {
        line = "UP/DOWN Slot   X Preflight   SQUARE Installer   L2+X Full scan   L1/R1 Page";
    } else {
        line = "UP/DOWN Slot   X Test   L2+X Full scan   L1/R1 Page   SELECT Exit";
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

    BootstrapPacket = packet_init(96, PACKET_NORMAL);
    ClearPacket = packet_init(256, PACKET_NORMAL);
    RenderPacket = packet_init(UI_PACKET_QWORDS, PACKET_NORMAL);
    if (BootstrapPacket == NULL || ClearPacket == NULL || RenderPacket == NULL)
        return -4;

    DrawStateDirty = 0;
    CurrentVideoMode = MCI_VIDEO_NATIVE;
    MarqueeEpoch = GetTimerSystemTime();
    MarqueeNeeded = 0;
    RendererReady = 1;
    MciGuiRenderMessage("Starting 0.4.0",
                        "Graphics Synthesizer frontend ready.\n"
                        "Settings can switch among the display modes carried forward from the hardware-tested 0.4.3 HDD-manager backend.",
                        NULL, MCI_GUI_TONE_INFO);
    return 0;
}

int MciGuiReady(void)
{
    return RendererReady;
}

int MciGuiNeedsAnimation(void)
{
    return MarqueeNeeded;
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
    if ((unsigned int)page >= MCI_GUI_PAGE_COUNT)
        page = MCI_GUI_CARD;
    if (page != MCI_GUI_SETTINGS) {
        LastSettingsMarqueeRow = -1;
        LastSettingsMarqueeState = ~0u;
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
    q = rect_fill(q, 12, 8, 628, 31, Theme.panel);
    q = rect_outline(q, 12, 8, 628, 31, Theme.border);
    q = text(q, 22, 14, "PS2 Memory Card Inspector  v0.4.0-dev3 Drebin", Theme.text);
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
    q = rect_fill(q, 12, 8, 628, 31, Theme.panel);
    q = rect_outline(q, 12, 8, 628, 31, Theme.border);
    q = text(q, 22, 14, "PS2 Memory Card Inspector  v0.4.0-dev3 Drebin", Theme.text);
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
