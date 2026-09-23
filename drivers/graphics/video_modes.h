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
// in every standard mode -> 31.5 kHz line rate, 31.75 us per line.
//
// TMDS_FAST_MHZ 378 = 37.8 MHz pixel: the same tables at x1.5 the pixel rate,
// i.e. 47.25 kHz / 21.16 us per line and x1.5 the refresh (90 / 75 Hz).  Only
// sys_clk 378 gives it a clean divider (1.0), so those modes are gated on
// Config::cpu_mhz == 378 — see Config::isFastVideoMode().
#define TMDS_STD_MHZ   252
#define TMDS_FAST_MHZ  378
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
// Every VGA pixel clock here is 126 MHz / k with k an integer 1..32.  That is not
// cosmetic: it is exactly the set the RP2350's HSTX serializer can produce
// (clk_hstx = clk_sys/{1,2,3,4} — CLOCKS_CLK_HSTX_DIV_INT is a TWO-BIT field with
// no fractional part — so 126 MHz at 252/378/504, then k clk_hstx cycles per
// pixel).  The old 19.894737 / 27 MHz clocks are not in that set at any CPU clock,
// which is what kept VGA on the PIO.  As a free side effect the new clocks are
// EXACT on the PIO too — 21 MHz is 252/12, 378/18, 504/24 and 25.2 MHz is
// 252/10, 378/15, 504/20 — where 19.894737 lost a whole 1/16 step to
// vga.c's truncated CLKDIV and came out 0.33% high at 252/378 and 0.25% low at 504.
//
// h_total is chosen to keep the LINE RATE of the mode it replaces, so v_total and
// the vertical geometry stay put and only the active fraction of the line moves.
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

// video_mode[] index offset of the 37.8 MHz ("fast") twin of a standard mode:
// entries [0]..[7] have their x1.5-refresh counterpart at [9]..[16].
#define VMODE_FAST_OFFSET 9
