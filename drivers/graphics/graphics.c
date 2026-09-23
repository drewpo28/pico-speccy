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
#endif
#endif