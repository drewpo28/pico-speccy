#pragma once
// Video mode timing: the struct and the clock constants both backends are written
// against.  Split out of graphics.h so the TABLE (video_mode_table.h) can be built
// on a host — tools/vga_timing_test.c checks the shipped numbers rather than a copy
// of them, which is the only way a timing typo can be caught without a monitor.
//
// Nothing here may depend on the SDK: <stdint.h> only.
#include <stdint.h>

// PIO clock divider must be integer or half-integer (n/2) for a clean TMDS pixel
// clock.  TMDS bit clock = pixel_clock * 10; the HDMI PIO program is 10 instructions
// (one bit per lane per cycle, clock pair as side-set), so SM clock = TMDS rate.
//
// TMDS_STD_MHZ 252 = 25.2 MHz pixel, and it is the same at EVERY CPU clock:
// 252/1.0, 378/1.5, 504/2.0 all land on it, which is why the timing table does
// not depend on the Overclock setting.  h_total is 800 px (line_bytes 400 x 2)
// in the 640-wide modes -> 31.5 kHz line rate, 31.75 us per line; the 720-wide
// ones use an 832-px line (416 bytes, 30.29 kHz) — see HDMI720_* in
// video_mode_table.h for why.
//
// TMDS_FAST_MHZ 378 = 37.8 MHz pixel: the same tables at x1.5 the pixel rate,
// i.e. 47.25 kHz / 21.16 us per line and x1.5 the refresh (90 / 75 Hz).  Only
// sys_clk 378 gives it a clean divider (1.0), so those modes are gated on
// Config::cpu_mhz == 378 — see Config::isFastVideoMode().
#define TMDS_STD_MHZ   252
#define TMDS_FAST_MHZ  378
// TMDS_540_MHZ 270 = 27 MHz pixel: the standard set re-timed for sys_clk 540 MHz,
// where 25.2 MHz would need a 2.143 divider. 540 / 270 = 2.0 exactly. Same active
// areas, wider lines / more lines so every refresh stays where it was.
// TMDS_XGA_MHZ 540 = 54 MHz pixel: 1024x768 x4 (divider 1.0 at 540 only).
#define TMDS_540_MHZ   270
#define TMDS_XGA_MHZ   540
#ifndef CPU_MHZ
// Host builds (tools/vga_timing_test.c) have no board define; the value only seeds
// pio_clk_div, which graphics_set_sys_clk_mhz() re-derives at boot anyway.
#define CPU_MHZ 378
#endif
#define PIO_DIV        ((float)CPU_MHZ / (float)TMDS_STD_MHZ)
// Clamped at >= 1.0: a build whose boot clock is below 378 MHz (ZERO2) cannot
// reach this TMDS rate at all, and sm_config_set_clkdiv() asserts on a divider
// under 1. Such a board can still select these modes after raising the CPU clock
// in the menu — graphics_set_sys_clk_mhz() then recomputes the whole table.
#define PIO_DIV_FAST   (CPU_MHZ < TMDS_FAST_MHZ ? 1.0f : (float)CPU_MHZ / (float)TMDS_FAST_MHZ)

// ---------------------------------------------------------------------------
// The VGA half of the table, and why its pixel clocks are what they are.
//
// A VGA "byte" in the h_* fields is TWO output pixels (the HDMI fields' unit);
// vga.c doubles them and adds vga_screen_width * 2 for the active area, so
//     h_total = (hs + bp + screen_width + fp) * 2
// and a field left at 0 INHERITS the HDMI one — including vga_h_fp_bytes, which
// is why the 720-wide modes have a 16 px front porch they never spell out.
//
// For VGA_HSTX builds, every standard VGA pixel clock is 126 MHz / k with k
// an integer 1..32. That is not cosmetic: it is exactly the set the RP2350's HSTX serializer can produce
// (clk_hstx = clk_sys/{1,2,3,4} — CLOCKS_CLK_HSTX_DIV_INT is a TWO-BIT field with
// no fractional part — so 126 MHz at 252/378/504, then k clk_hstx cycles per
// pixel).  The old 19.894737 / 27 MHz clocks are not in that set at any CPU clock,
// which is what kept VGA on the PIO.  As a free side effect the new clocks are
// EXACT on the PIO too — 21 MHz is 252/12, 378/18, 504/24 and 25.2 MHz is
// 252/10, 378/15, 504/20 — where 19.894737 lost a whole 1/16 step to
// vga.c's truncated CLKDIV and came out 0.33% high at 252/378 and 0.25% low at 504.
//
// PIO builds retain the v1.0.6 VGA timings: changing the back porch and active
// fraction can shift/crop the picture on monitors with stored analogue geometry.
// HSTX h_total is chosen to keep the LINE RATE of the mode it replaces, so
// v_total barely changes and only the active fraction of the line moves.
// tools/vga_timing_test.c pins all of it.
// ---------------------------------------------------------------------------

struct video_mode_t {
  int v_total;
  int v_active;
  int freq;
  int pixel_clk;
  int vsync_start;
  int vsync_end;
  int screen_width;
  int h_sync_bytes;
  int h_bp_bytes;
  int h_fp_bytes;
  int line_bytes;
  int v_offset;
  float pio_clk_div; // PIO divider = sys_clk / TMDS_clk, must be integer or half-integer (n/2)
  // TMDS bit clock this mode is built for, MHz (0 = the 252 MHz default, i.e.
  // 25.2 MHz pixel).  graphics_set_sys_clk_mhz() re-derives pio_clk_div from it
  // whenever sys_clk moves, so a mode that wants a different pixel clock (the
  // 90/75 Hz set: 378 MHz TMDS = 37.8 MHz pixel) keeps it at every CPU clock.
  int tmds_mhz;
  // Non-zero = the x4 scanout (1024x768): each framebuffer byte is put into the line
  // buffer TWICE (= 4 output pixels, the PIO converter doubles each byte already),
  // each framebuffer row is shown on 4 lines (a vertical map), and the source is
  // the centre 256x192 of the 320x240 framebuffer: x from x4_offset bytes, rows
  // from v_offset. screen_width is then the LINE-BUFFER width (512), not the fb's.
  int x4_offset;
  // Non-zero = the x3 scanout (1024x768): the HDMI converter runs one byte per
  // output pixel (each fb pixel is put in the line buffer three times, every control
  // byte twice), the whole 320x240 framebuffer is shown as 960x720 centred, each fb
  // row on 3 lines. The h_* / line_bytes fields stay in the usual 2-px units (the
  // DMA moves 2 * line_bytes bytes); screen_width is the 512 of the 1024-px area.
  int x3;            // 0, or the x3 line layout (hdmi.c hdmi_x3_layouts[x3 - 1])
  // Non-zero = the x2 "window" scanout (800x600 at 504): the fb row (screen_width
  // minus 2 * x2_pad bytes wide) is put x2_pad bytes into the active line with
  // background either side, and the rows are centred vertically by a line map.
  int x2_pad;
  // VGA-only overrides for fields above. If 0/zero, VGA uses the main fields.
  // HDMI never reads these — its timing is unaffected.
  int vga_v_total;
  int vga_v_active;
  int vga_pixel_clk;
  int vga_vsync_start;
  int vga_vsync_end;
  int vga_h_sync_bytes;
  int vga_h_bp_bytes;
  int vga_h_fp_bytes;
  int vga_screen_width;
};

// Longest line any mode in the table asks for, in output pixels.  vga.c sizes its
// four line templates to this (they are re-rendered in place on a mode switch, so
// they must hold the widest one) and vga_reinit() refuses a layout above it; the
// widest shipped mode is 880 (PIO 720-wide modes).  Kept here rather than
// in the driver so tools/vga_timing_test.c pins the SHIPPED bound instead of a
// copy of it — raise it here the moment a mode needs a longer line, because under
// PWM every pixel costs four bytes and this is four buffers.
#define VGA_MAX_LINE_SIZE 896
// ...except the VGA twins of the big HDMI modes ([28]..[39], VESA 60 Hz: 1024x768 at 63 MHz,
// 1304 px; 800x600 at 42 MHz, 1108 px). vga.c sizes the templates to the BOOT mode's
// line, so only a board that boots into one of those pays the longer buffers; the
// standard set keeps the 896 bound above. A big mode always runs narrow (no PWM).
#define VGA_MAX_LINE_SIZE_BIG 1344

// video_mode[] index offset of the 37.8 MHz ("fast") twin of a standard mode:
// entries [0]..[7] have their x1.5-refresh counterpart at [9]..[16].
#define VMODE_FAST_OFFSET 9
// ...and the 27 MHz (sys_clk 540) twin of [0]..[8] at index + VMODE_540_OFFSET,
// then the 1024x768 x4 mode (540 only) at VMODE_XGA.
#define VMODE_540_OFFSET 17
#define VMODE_XGA        26
#define VMODE_XGA3       27
// The big modes at ~50 Hz per machine, one entry per (clock, scale) — see
// graphics_big_mode() for which clock picks which. Shared:
#define VMODE_XGA4_504   28   // 1024x768 x4, 50.4 MHz (HSTX: also at 252)
#define VMODE_XGA3_504   29   // 1024x768 x3
#define VMODE_SVGA3_378  30   // 800x600 x3, 37.8 MHz
#define VMODE_SVGA2_378  31   // 800x600 x2, 37.8 MHz
#define VMODE_HD3_504    32   // 1280x720 x3 16:9, 50.4 MHz (HSTX: also at 252)
#define VMODE_HD3_540    33   // ...54 MHz
#define VMODE_SD3_504    34   // 1440x576 x3 16:9 (360x288 fb), 50.4 MHz (HSTX: also at 252)
#define VMODE_SD3_540    35   // ...54 MHz = CEA 1440x576p50
#if defined(HDMI_HSTX) && HDMI_HSTX == 2
#define VMODE_XGA4_378   36   // 1024x768 x4 / x3, 75.6 MHz (clk_hstx 378 /1)
#define VMODE_XGA3_378   37
#define VMODE_SVGA3_H504 38   // 800x600 x3 / x2, 50.4 MHz (clk_hstx 252: 252 /1, 504 /2)
#define VMODE_SVGA2_H504 39
#define VMODE_SVGA3_H540 40   // 800x600 x3 / x2, 54 MHz (clk_hstx 270 = 540 /2)
#define VMODE_SVGA2_H540 41
#define VMODE_HD3_378    42   // 1280x720 x3, 75.6 MHz, 1980-px line
#define VMODE_HD60_378   43   // 1280x720 @60 x3, 75.6 MHz, 1680-px line
#define VMODE_SD60_504   44   // 1440x480 @60 x3 (360x240 fb), 50.4 MHz (also at 252)
#define VMODE_SD60_540   45   // ...54 MHz = CEA 1440x480p60
#else
#define VMODE_SVGA3_504  36   // 800x600 x3 / x2, 33.6 MHz (PIO 1.5)
#define VMODE_SVGA2_504  37
#define VMODE_SVGA3_540  38   // 800x600 x3 / x2, 36 MHz (PIO 1.5)
#define VMODE_SVGA2_540  39
#define VMODE_SD60_504   40   // 1440x480 @60 x3 (360x240 fb), 50.4 MHz
#define VMODE_SD60_540   41   // ...54 MHz = CEA 1440x480p60
#endif
