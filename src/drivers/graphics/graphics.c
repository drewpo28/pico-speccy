#include "graphics.h"
#include <string.h>
#include "pico.h"

uint16_t graphics_max_tft_freq_mhz = 126;

#include "video_mode_table.h"

/**
void draw_text(const char string[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor) {
if (!text_buffer) return;
    uint8_t* t_buf = text_buffer + TEXTMODE_COLS * 2 * y + 2 * x;
    for (int xi = TEXTMODE_COLS * 2; xi--;) {
        if (!*string) break;
        *t_buf++ = *string++;
        *t_buf++ = bgcolor << 4 | color & 0xF;
    }
}
*/
void draw_window(const char title[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    char line[width + 1];
    memset(line, 0, sizeof line);
    width--;
    height--;
    // Рисуем рамки

    memset(line, 0xCD, width); // ═══


    line[0] = 0xC9; // ╔
    line[width] = 0xBB; // ╗
    draw_text(line, x, y, 11, 1);

    line[0] = 0xC8; // ╚
    line[width] = 0xBC; //  ╝
    draw_text(line, x, height + y, 11, 1);

    memset(line, ' ', width);
    line[0] = line[width] = 0xBA;

    for (int i = 1; i < height; i++) {
        draw_text(line, x, y + i, 11, 1);
    }

    snprintf(line, width - 1, " %s ", title);
    draw_text(line, x + (width - strlen(line)) / 2, y, 14, 3);
}

struct video_mode_t __not_in_flash_func(graphics_get_video_mode)(int mode)
{
    return video_mode[mode];
}

int graphics_fast_mode(int mode)
{
    const int n = (int)(sizeof(video_mode)/sizeof(video_mode[0]));
    const int fast = mode + VMODE_FAST_OFFSET;
    if (mode < 0 || fast >= n) return mode;
    // Only answer for a real twin: index 8 (720x576@60) has none, and a caller
    // must never be handed an unrelated mode because the table grew.
    return video_mode[fast].tmds_mhz == TMDS_FAST_MHZ ? fast : mode;
}

int graphics_540_mode(int mode)
{
    const int n = (int)(sizeof(video_mode)/sizeof(video_mode[0]));
    const int t = mode + VMODE_540_OFFSET;
    if (mode < 0 || mode >= VMODE_FAST_OFFSET || t >= n) return mode;
    return video_mode[t].tmds_mhz == TMDS_540_MHZ ? t : mode;
}

int graphics_xga_mode(int x3) { return x3 ? VMODE_XGA3 : VMODE_XGA; }

// The "big" modes (Config::VM_1024x768_59 = 8, ..._59X3 = 9, VM_800x600_X3 = 10,
// VM_800x600_X2 = 11): the table index for this sys_clk, or -1 when the clock cannot
// run it. One entry per (clock, scale): the machine's line count is written into it
// by graphics_big_mode_fit(). `klass` is kept for the callers and unused here.
int graphics_big_mode(int vm, unsigned mhz, int klass)
{
    (void)klass;
    switch (vm) {
        case 8:
        case 9: {
            const int x3 = (vm == 9);
            if (mhz == 540) return x3 ? VMODE_XGA3 : VMODE_XGA;
            if (mhz == 504) return x3 ? VMODE_XGA3_504 : VMODE_XGA4_504;
#if defined(HDMI_HSTX) && HDMI_HSTX == 2
            if (mhz == 252) return x3 ? VMODE_XGA3_504 : VMODE_XGA4_504;   // clk_hstx 252 /1
            if (mhz == 378) return x3 ? VMODE_XGA3_378 : VMODE_XGA4_378;
#endif
            return -1;
        }
        case 10:
        case 11: {
            const int x2 = (vm == 11);
            if (mhz == 378) return x2 ? VMODE_SVGA2_378 : VMODE_SVGA3_378;
#if defined(HDMI_HSTX) && HDMI_HSTX == 2
            if (mhz == 252 || mhz == 504) return x2 ? VMODE_SVGA2_H504 : VMODE_SVGA3_H504;
            if (mhz == 540) return x2 ? VMODE_SVGA2_H540 : VMODE_SVGA3_H540;
#else
            if (mhz == 504) return x2 ? VMODE_SVGA2_504 : VMODE_SVGA3_504;
            if (mhz == 540) return x2 ? VMODE_SVGA2_540 : VMODE_SVGA3_540;
#endif
            return -1;
        }
        case 13:
            if (mhz == 504) return VMODE_HD3_504;
            if (mhz == 540) return VMODE_HD3_540;
#if defined(HDMI_HSTX) && HDMI_HSTX == 2
            if (mhz == 252) return VMODE_HD3_504;   // clk_hstx 252 /1
            if (mhz == 378) return VMODE_HD3_378;
#endif
            return -1;
        case 14:
            if (mhz == 504) return VMODE_SD3_504;
            if (mhz == 540) return VMODE_SD3_540;
#if defined(HDMI_HSTX) && HDMI_HSTX == 2
            if (mhz == 252) return VMODE_SD3_504;
#endif
            return -1;
        case 15:
#if defined(HDMI_HSTX) && HDMI_HSTX == 2
            return mhz == 378 ? VMODE_HD60_378 : -1;
#else
            return -1;
#endif
        case 16:
            if (mhz == 504) return VMODE_SD60_504;
            if (mhz == 540) return VMODE_SD60_540;
#if defined(HDMI_HSTX) && HDMI_HSTX == 2
            if (mhz == 252) return VMODE_SD60_504;
#endif
            return -1;
        default: return -1;
    }
}

// Write the machine's line count into big-mode entry `mode` (klass 0 Pentagon,
// 1 48K-class, 2 128K): v_total + 1 = pixel / (line * frame rate), rounded. Only the
// 50 Hz entries ([28] on); the 1024x768 @540 59 Hz ones keep theirs. Call it on the
// entry about to be used, before graphics_update_mode_timing() publishes it.
void graphics_big_mode_fit(int mode, int klass)
{
    const int n = (int)(sizeof(video_mode)/sizeof(video_mode[0]));
    if (mode < VMODE_XGA4_504 || mode >= n) return;
    if (video_mode[mode].freq != 50) return;   // the 60 Hz entries keep their line count
    static const uint32_t frame_t[3] = { 71680, 69888, 70908 };      // T-states per frame
    static const uint32_t cpu_hz[3]  = { 3500000, 3500000, 3546900 };
    if (klass < 0 || klass > 2) klass = 0;
    const uint64_t ht = (uint64_t)video_mode[mode].line_bytes * 2u;
    // lines = pixel * frame_t / (ht * cpu_hz), rounded
    const uint64_t num = (uint64_t)video_mode[mode].pixel_clk * frame_t[klass];
    const uint64_t den = ht * cpu_hz[klass];
    int lines = (int)((num + den / 2) / den);
    // A machine whose frame is shorter than the picture can hold
    // gets the fewest lines that still carry vertical blanking: slightly slow, not broken.
    if (lines < video_mode[mode].vsync_end + 6) lines = video_mode[mode].vsync_end + 6;
    video_mode[mode].v_total = lines - 1;
}

float graphics_clk_div_at(int mode, unsigned sys_mhz, int vga)
{
    const int n = (int)(sizeof(video_mode)/sizeof(video_mode[0]));
    if (mode < 0 || mode >= n || !sys_mhz) return 0.0f;
    if (vga) {
        // vga_reinit(): clk_sys / pixel_clk, written to CLKDIV as 16.8 with the low
        // 12 bits masked off — i.e. the programmed divider is quantised to 1/16.
        const int pix = video_mode[mode].vga_pixel_clk ? video_mode[mode].vga_pixel_clk
                                                       : video_mode[mode].pixel_clk;
        if (pix <= 0) return 0.0f;
        const double fdiv = (double)sys_mhz * 1000000.0 / (double)pix;
        const uint32_t d32 = (uint32_t)(fdiv * 65536.0) & 0xfffff000u;
        return (float)d32 / 65536.0f;
    }
    // HDMI: the same formula graphics_set_sys_clk_mhz() stores, clamp and all —
    // except that "below 1" is reported as unreachable instead of clamped to 1.0,
    // which would claim a mode runs when it would silently run at the wrong rate.
    const int tmds = video_mode[mode].tmds_mhz ? video_mode[mode].tmds_mhz : TMDS_STD_MHZ;
    if ((float)sys_mhz < (float)tmds) return 0.0f;
    return (float)sys_mhz / (float)tmds;
}

// The two clock fields the menu needs to label an HSTX row, without handing it the
// whole struct (UiTree.cpp deliberately does not include graphics.h).  Both answer
// for the resolved table index, i.e. after vmGraphicsIndex()/graphics_fast_mode().
unsigned graphics_mode_tmds_mhz(int mode)
{
    if (mode < 0 || mode >= (int)(sizeof(video_mode)/sizeof(video_mode[0]))) return TMDS_STD_MHZ;
    return video_mode[mode].tmds_mhz ? (unsigned)video_mode[mode].tmds_mhz : TMDS_STD_MHZ;
}

uint32_t graphics_mode_vga_pixel_hz(int mode)
{
    if (mode < 0 || mode >= (int)(sizeof(video_mode)/sizeof(video_mode[0]))) return 0;
    // A zero vga_* field INHERITS the HDMI one — the same rule vga_reinit() follows.
    return video_mode[mode].vga_pixel_clk ? (uint32_t)video_mode[mode].vga_pixel_clk
                                          : (uint32_t)video_mode[mode].pixel_clk;
}

void graphics_set_sys_clk_mhz(unsigned mhz)
{
    if (!mhz) return;
    for (int i = 0; i < sizeof(video_mode)/sizeof(video_mode[0]); i++) {
        const int tmds = video_mode[i].tmds_mhz ? video_mode[i].tmds_mhz : TMDS_STD_MHZ;
        float div = (float)mhz / (float)tmds;
        // A PIO divider below 1 does not exist; a mode asking for more TMDS than
        // sys_clk can give is simply unreachable at this clock and is gated off in
        // the menu (Config::isFastVideoMode).  Clamp so a stale pick cannot hand
        // pio_sm_init() an illegal divider.
        if (div < 1.0f) div = 1.0f;
        video_mode[i].pio_clk_div = div;
    }
}

#ifdef VGA_HDMI
extern void hdmi_set_scanlines(uint8_t level);
extern void vga_set_scanlines(uint8_t level);
void graphics_set_scanlines(uint8_t level) {
    hdmi_set_scanlines(level);
    vga_set_scanlines(level);
}
extern void hdmi_set_crt(uint8_t level);
extern void vga_set_crt(uint8_t level);
void graphics_set_crt(uint8_t level) {
    // Each backend rebuilds only its own tables from its own colour cache, so the
    // order does not matter and neither can clobber the other's state.
    hdmi_set_crt(level);
    vga_set_crt(level);
}
extern void hdmi_set_dither(bool enabled);
void graphics_set_dither(bool enabled) {
    hdmi_set_dither(enabled);
}
extern void hdmi_set_clock_drive(bool soft);
void graphics_set_hdmi_clock_drive(bool soft) {
    hdmi_set_clock_drive(soft);
}
// Only the HDMI backend caches the mode: its line ISR reads a snapshot taken in
// hdmi_init(). The VGA ISR fetches graphics_get_video_mode(get_video_mode())
// every line, and the 50 Hz variants of one resolution differ only in
// (vga_)v_total, so VGA already follows a machine switch live.
void graphics_update_mode_timing(void) {
    hdmi_update_mode_timing();
}
extern bool SELECT_VGA;
extern void vga_set_vmap(const uint16_t *map, int n);
extern const uint16_t *vga_vmap_latched(void);
extern uint32_t vga_frame_count(void);
bool graphics_vmap_supported(void) { return true; }
void graphics_set_vmap(const uint16_t *map, int n) {
    if (SELECT_VGA) vga_set_vmap(map, n); else hdmi_set_vmap(map, n);
}
const uint16_t *graphics_vmap_latched(void) {
    return SELECT_VGA ? vga_vmap_latched() : hdmi_vmap_latched();
}
uint32_t graphics_frame_count(void) {
    return SELECT_VGA ? vga_frame_count() : hdmi_frame_count();
}
#else
void graphics_set_scanlines(uint8_t level) {
    (void)level;
}
// TFT/ILI9341, TV and SOFTTV targets do not link vga.c/hdmi.c — the grille has no
// output pair to use there, so it degrades to the colour half of the filter only.
void graphics_set_crt(uint8_t level) {
    (void)level;
}
#ifdef HDMI
extern void hdmi_set_dither(bool enabled);
void graphics_set_dither(bool enabled) {
    hdmi_set_dither(enabled);
}
extern void hdmi_set_clock_drive(bool soft);
void graphics_set_hdmi_clock_drive(bool soft) {
    hdmi_set_clock_drive(soft);
}
void graphics_update_mode_timing(void) {
    hdmi_update_mode_timing();
}
extern void hdmi_set_vmap(const uint16_t *map, int n);
extern const uint16_t *hdmi_vmap_latched(void);
extern uint32_t hdmi_frame_count(void);
bool graphics_vmap_supported(void) { return true; }
void graphics_set_vmap(const uint16_t *map, int n) { hdmi_set_vmap(map, n); }
const uint16_t *graphics_vmap_latched(void) { return hdmi_vmap_latched(); }
uint32_t graphics_frame_count(void) { return hdmi_frame_count(); }
#else
void graphics_set_dither(bool enabled) {
    (void)enabled;
}
void graphics_set_hdmi_clock_drive(bool soft) {
    (void)soft;
}
// No cached mode timing on TV/SOFTTV/TFT: those drivers have their own,
// output-fixed frame rate.
void graphics_update_mode_timing(void) {
}
bool graphics_vmap_supported(void) { return false; }
void graphics_set_vmap(const uint16_t *map, int n) { (void)map; (void)n; }
const uint16_t *graphics_vmap_latched(void) { return 0; }
uint32_t graphics_frame_count(void) { return 0; }
#endif
#endif