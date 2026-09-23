#ifdef __cplusplus
extern "C" {
#endif

#include "stdbool.h"
#include "stdio.h"
#include "stdint.h"

#ifdef TFT
#include "st7789.h"
#endif
#ifdef VGA_HDMI
#include "vga.h"
#include "hdmi.h"
#endif
#ifdef TV
#include "tv.h"
#endif
#ifdef SOFTTV
#include "tv-software.h"
#endif
#include "font6x8.h"
#include "font8x8.h"
#include "font8x16.h"

#include "video_modes.h"

enum graphics_mode_t {
    TEXTMODE_DEFAULT,
    GRAPHICSMODE_DEFAULT,
};

extern uint16_t graphics_max_tft_freq_mhz; // max SPI freq for TFT, MHz

void graphics_init();

void graphics_set_mode(enum graphics_mode_t mode);

void graphics_set_buffer(uint8_t* buffer, uint16_t width, uint16_t height);

void graphics_set_offset(int x, int y);

void graphics_set_palette(uint8_t i, uint32_t color);

void graphics_set_textbuffer(uint8_t* buffer);

void graphics_set_bgcolor(uint32_t color888);

void graphics_set_flashmode(bool flash_line, bool flash_frame);

void graphics_set_scanlines(uint8_t level);
// CRT filter aperture grille: 0=Off, 1..3 = increasingly dark. Dims the SECOND
// output pixel of every palette index (each index already owns two — HDMI via the
// conv_color TMDS pair, VGA via the packed uint16 pixel pair), so it costs nothing
// at scanout. The colour half of the filter (gamma/tint/lift) is applied upstream
// in VIDEO::applyCrtFilter() and arrives here as ordinary palette values.
void graphics_set_crt(uint8_t level);
void graphics_set_dither(bool enabled);
// HDMI clock-pair drive: false = Normal (12 mA, fast slew), true = Soft (8 mA, slow).
// Live-safe (pad registers only). No-op on outputs without an HDMI backend.
void graphics_set_hdmi_clock_drive(bool soft);
// Re-publish the current video mode's vertical timing to the active backend.
// The 50 Hz modes are per machine (v_total tuned so one display frame is one
// emulated frame), so this must run whenever VIDEO::video_mode changes — i.e.
// on every machine reset. HDMI needs it (its line ISR reads a snapshot); VGA is
// already live. No-op on the other outputs.
void graphics_update_mode_timing(void);

// Profi DS80 "packed nibble" mode — HDMI path.
// active=true: takes palette snapshot, then writes TMDS pairs for each (ink,paper) entry
//   in pair_lut[ink*16+paper] → conv_color safe slot. Also sets slot 255 = (black,black).
//   pair_lut must be VIDEO::profi_pair_lookup[0][0] (flat 256-byte array, pre-built).
//   palette16_rgb888: 16 RGB888 colors indexed by ink/paper value.
// active=false: restores snapshot. HDMI driver only; no-op on VGA/TV.
void hdmi_set_profi_ds80_mode(bool active,
                               const uint32_t *palette16_rgb888,
                               const uint8_t  *pair_lut);
// Unified DS80 active flag — set by both hdmi_set_profi_ds80_mode and vga_set_profi_ds80_mode.
extern volatile bool profi_ds80_active;

// Profi DS80 "packed nibble" mode — VGA path.
// Builds palette_vga_ds80[256]: slot → uint16_t(left_pixel, right_pixel).
// active=false: clears profi_ds80_active so ISR falls back to standard palette.
void vga_set_profi_ds80_mode(bool active,
                              const uint32_t *palette16_rgb888,
                              const uint8_t  *pair_lut);

void draw_text(const char string[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint8_t color, uint8_t bgcolor);
void draw_window(const char title[TEXTMODE_COLS + 1], uint32_t x, uint32_t y, uint32_t width, uint32_t height);

void clrScr(uint8_t color);

struct video_mode_t graphics_get_video_mode(int mode);
// Re-derive every mode's pio_clk_div for the given sys_clk (the Overclock menu
// moves it after boot).  Replaces the old graphics_set_pio_clk_div(): the divider
// is now per mode, because not every mode wants the same TMDS clock.
void graphics_set_sys_clk_mhz(unsigned mhz);

// The PIO divider `mode` would be programmed with at a sys_clk of `sys_mhz`,
// WITHOUT touching the live table: the HDMI (TMDS) one, or the VGA pixel one when
// `vga` is non-zero.  Mirrors graphics_set_sys_clk_mhz() and vga_reinit()
// respectively — VGA's 1/16 CLKDIV quantisation included — so a caller can show
// what the driver will actually program.  Returns 0 when the mode is unreachable
// at that clock (a PIO divider below 1 does not exist).
float graphics_clk_div_at(int mode, unsigned sys_mhz, int vga);

// video_mode[] index of the 37.8 MHz twin of a standard mode, or `mode` itself
// when it has none.  The table owns its own layout: callers (VIDEO::Reset) have
// no business adding VMODE_FAST_OFFSET themselves.
int graphics_fast_mode(int mode);

#ifdef __cplusplus
}
#endif
