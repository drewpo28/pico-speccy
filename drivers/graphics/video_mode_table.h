#pragma once
// The video mode table.  Included by drivers/graphics/graphics.c (the firmware's
// one definition) and by tools/vga_timing_test.c, which checks the VGA half of
// every entry — so the numbers below are the ones the test reads, not a copy.
//
// See video_modes.h for the struct, the clock constants and the rule the VGA
// pixel clocks follow.
#include "video_modes.h"

#ifndef VGA_HSTX
#define VGA_HSTX 0
#endif

// EXPERIMENT (2026-09-24): horizontal blanking layout of the 720x576 modes
// ([4]-[6] @50, [13]-[15] @75), in framebuffer bytes = 2 output pixels. h_total
// stays 400 bytes (800 px) so the line rate and refresh do not move; only where the
// 40 blanking bytes go changes. A CRT behind an HDMI->VGA converter takes no audio
// in any 720-wide mode on any back-end (PIO, HSTX RAW, HSTX TMDS) while 640x480
// plays, and the island (22 bytes at the head of hsync) is the only thing laid out
// differently there. Build with -DHDMI_720_LAYOUT=n:
//   0  hs 16 / bp 16 / fp 8   the shipped layout: hsync ends inside the packet,
//                             10 control px between island and video preamble
//   1  hs 22 / bp 10 / fp 8   island wholly inside hsync, still 10 control px
//   2  hs 16 / bp 18 / fp 6   hsync as shipped, 14 control px after the island,
//                             12 before the island preamble
//   3  hs 22 / bp 12 / fp 6   island inside hsync AND 14 control px after it
//   4  hs 32 / bp 5  / fp 3   CEA-576p-like 64 px hsync (island + 20 control px
//                             inside it), 6 control px before the island
//   5..7 narrower active area (640 / 688 / 704 px) with 160 / 112 / 96 px blanking
#ifndef HDMI_720_LAYOUT
#define HDMI_720_LAYOUT 0
#endif
#if   HDMI_720_LAYOUT == 0
#define HDMI720_HS 16
#define HDMI720_BP 16
#define HDMI720_FP 8
#elif HDMI_720_LAYOUT == 1
#define HDMI720_HS 22
#define HDMI720_BP 10
#define HDMI720_FP 8
#elif HDMI_720_LAYOUT == 2
#define HDMI720_HS 16
#define HDMI720_BP 18
#define HDMI720_FP 6
#elif HDMI_720_LAYOUT == 3
#define HDMI720_HS 22
#define HDMI720_BP 12
#define HDMI720_FP 6
#elif HDMI_720_LAYOUT == 4
#define HDMI720_HS 32
#define HDMI720_BP 5
#define HDMI720_FP 3
#elif HDMI_720_LAYOUT == 5
#define HDMI720_W  320        /* 640 active px: the 640x480 blanking (160 px) in a 576-line mode */
#define HDMI720_HS 48
#define HDMI720_BP 24
#define HDMI720_FP 8
#elif HDMI_720_LAYOUT == 6
#define HDMI720_W  344        /* 688 active px, 112 px blanking */
#define HDMI720_HS 32
#define HDMI720_BP 16
#define HDMI720_FP 8
#elif HDMI_720_LAYOUT == 7
#define HDMI720_W  352        /* 704 active px, 96 px blanking */
#define HDMI720_HS 24
#define HDMI720_BP 16
#define HDMI720_FP 8
#else
#error "HDMI_720_LAYOUT must be 0..7"
#endif
// 5..7 NARROW the HDMI active area (the framebuffer stays 360 wide and its right
// edge is simply not sent) to test whether the converter needs more horizontal
// blanking than 80 px. VGA keeps its own vga_screen_width.
#ifndef HDMI720_W
#define HDMI720_W 360
#endif
_Static_assert(HDMI720_W + HDMI720_HS + HDMI720_BP + HDMI720_FP == 400, "720 line must stay 400 bytes");

static struct video_mode_t video_mode[] = {
    { // [0] 640x480 60Hz
        .v_total = 524,
        .v_active = 480,
        .freq = 60,
        .pixel_clk = 25175000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: 25.2 MHz exactly (= 126/5), which is what vga.c's truncated CLKDIV
        // already produced from the 25.175 nominal at 252/378/504.  Spelled out so
        // the HSTX backend has an exact number rather than a rounding coincidence.
        .vga_pixel_clk = 25200000
    },
    { // [1] 640x480 50Hz Pentagon 48.82Hz
        .v_total = 644,
        .v_active = 480,
        .freq = 50,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
#if VGA_HSTX
        // VGA: 21 MHz = 126/6, exact at 252/378/504 (div 12/18/24).  h_total 840
        // keeps the 25.0 kHz line rate of the 19.96 MHz mode it replaces, so
        // v_total and the vertical geometry stay put; the extra 40 px go to the
        // porches and the active area is 76.2% of the line instead of 80%.
        .vga_v_total = 512,           // 25000 / 512 = 48.828 Hz (Pentagon 48.828, exact)
        .vga_pixel_clk = 21000000,
        .vga_h_bp_bytes = 34,
        .vga_h_fp_bytes = 18
#else
        // Preserve the v1.0.6 PIO geometry for analogue monitors.
        .vga_v_total = 511,
        .vga_pixel_clk = 19894737
#endif
    },
    { // [2] 640x480 50Hz 48K 50.08Hz
        .v_total = 628,
        .v_active = 480,
        .freq = 50,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
#if VGA_HSTX
        // VGA: 21 MHz = 126/6, exact at 252/378/504 (div 12/18/24).  h_total 840
        // keeps the 25.0 kHz line rate of the 19.96 MHz mode it replaces, so
        // v_total and the vertical geometry stay put; the extra 40 px go to the
        // porches and the active area is 76.2% of the line instead of 80%.
        .vga_v_total = 499,           // 25000 / 499 = 50.100 Hz (48K 50.080)
        .vga_pixel_clk = 21000000,
        .vga_h_bp_bytes = 34,
        .vga_h_fp_bytes = 18
#else
        // Preserve the v1.0.6 PIO geometry for analogue monitors.
        .vga_v_total = 499,
        .vga_pixel_clk = 19894737
#endif
    },
    { // [3] 640x480 50Hz 128K 50.02Hz
        .v_total = 629,
        .v_active = 480,
        .freq = 50,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
#if VGA_HSTX
        // VGA: 21 MHz = 126/6, exact at 252/378/504 (div 12/18/24).  h_total 840
        // keeps the 25.0 kHz line rate of the 19.96 MHz mode it replaces, so
        // v_total and the vertical geometry stay put; the extra 40 px go to the
        // porches and the active area is 76.2% of the line instead of 80%.
        .vga_v_total = 500,           // 25000 / 500 = 50.000 Hz (128K 50.021)
        .vga_pixel_clk = 21000000,
        .vga_h_bp_bytes = 34,
        .vga_h_fp_bytes = 18
#else
        // Preserve the v1.0.6 PIO geometry for analogue monitors.
        .vga_v_total = 498,
        .vga_pixel_clk = 19894737
#endif
    },
    { // [4] 720x576 50Hz Pentagon full border — 25.2MHz pixel (sys_clk=378MHz, div=1.5)
        .v_total = 644,   // 25.2MHz/800/644 = 48.91Hz (Pentagon 48.83Hz)
        .v_active = 576,
        .freq = 50,
        .pixel_clk = 25175000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = HDMI720_W,
        .h_sync_bytes = HDMI720_HS,
        .h_bp_bytes = HDMI720_BP,
        .h_fp_bytes = HDMI720_FP,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
#if VGA_HSTX
        // VGA: 25.2 MHz = 126/5, exact at 252/378/504 (div 10/15/20).  h_total 824
        // (hs 48 + bp 40 + active 720 + fp 16, the front porch inherited from
        // h_fp_bytes) holds the 30.6 kHz line rate of the 27 MHz mode it replaces
        // within 0.3%, so v_total barely moves and the active area grows from
        // 81.8% of the line to 87.4%.
        .vga_v_total = 626,           // 30582.5 / 626 = 48.854 Hz (Pentagon 48.828)
        .vga_pixel_clk = 25200000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 20,
        .vga_h_fp_bytes = 8,   // pinned: was inherited from h_fp_bytes (the HDMI 720 line moved it)
        .vga_screen_width = 360
#else
        // Preserve the v1.0.6 PIO geometry for analogue monitors.
        .vga_v_total = 628,
        .vga_pixel_clk = 27000000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 48,
        .vga_h_fp_bytes = 8,   // pinned: was inherited from h_fp_bytes (the HDMI 720 line moved it)
        .vga_screen_width = 360
#endif
    },
    { // [5] 720x576 50Hz 48K full border — 25.2MHz pixel
        .v_total = 628,   // 25.2MHz/800/628 = 50.09Hz (48K 50.08Hz)
        .v_active = 576,
        .freq = 50,
        .pixel_clk = 25175000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = HDMI720_W,
        .h_sync_bytes = HDMI720_HS,
        .h_bp_bytes = HDMI720_BP,
        .h_fp_bytes = HDMI720_FP,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
#if VGA_HSTX
        // VGA: 25.2 MHz = 126/5, exact at 252/378/504 (div 10/15/20).  h_total 824
        // (hs 48 + bp 40 + active 720 + fp 16, the front porch inherited from
        // h_fp_bytes) holds the 30.6 kHz line rate of the 27 MHz mode it replaces
        // within 0.3%, so v_total barely moves and the active area grows from
        // 81.8% of the line to 87.4%.
        .vga_v_total = 611,           // 30582.5 / 611 = 50.053 Hz (48K 50.080)
        .vga_pixel_clk = 25200000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 20,
        .vga_h_fp_bytes = 8,   // pinned: was inherited from h_fp_bytes (the HDMI 720 line moved it)
        .vga_screen_width = 360
#else
        // Preserve the v1.0.6 PIO geometry for analogue monitors.
        .vga_v_total = 614,
        .vga_pixel_clk = 27000000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 48,
        .vga_h_fp_bytes = 8,   // pinned: was inherited from h_fp_bytes (the HDMI 720 line moved it)
        .vga_screen_width = 360
#endif
    },
    { // [6] 720x576 50Hz 128K full border — 25.2MHz pixel
        .v_total = 629,   // 25.2MHz/800/629 = 50.00Hz (128K 50.02Hz)
        .v_active = 576,
        .freq = 50,
        .pixel_clk = 25175000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = HDMI720_W,
        .h_sync_bytes = HDMI720_HS,
        .h_bp_bytes = HDMI720_BP,
        .h_fp_bytes = HDMI720_FP,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
#if VGA_HSTX
        // VGA: 25.2 MHz = 126/5, exact at 252/378/504 (div 10/15/20).  h_total 824
        // (hs 48 + bp 40 + active 720 + fp 16, the front porch inherited from
        // h_fp_bytes) holds the 30.6 kHz line rate of the 27 MHz mode it replaces
        // within 0.3%, so v_total barely moves and the active area grows from
        // 81.8% of the line to 87.4%.
        .vga_v_total = 611,           // 30582.5 / 611 = 50.053 Hz (128K 50.021)
        .vga_pixel_clk = 25200000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 20,
        .vga_h_fp_bytes = 8,   // pinned: was inherited from h_fp_bytes (the HDMI 720 line moved it)
        .vga_screen_width = 360
#else
        // Preserve the v1.0.6 PIO geometry for analogue monitors.
        .vga_v_total = 612,
        .vga_pixel_clk = 27000000,
        .vga_vsync_start = 580,
        .vga_vsync_end = 586,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 48,
        .vga_h_fp_bytes = 8,   // pinned: was inherited from h_fp_bytes (the HDMI 720 line moved it)
        .vga_screen_width = 360
#endif
    },
    { // [7] 720x480 60Hz half border
        .v_total = 524,
        .v_active = 480,
        .freq = 60,
        .pixel_clk = 25175000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
#if VGA_HSTX
        // VGA: 25.2 MHz = 126/5, exact at 252/378/504 (div 10/15/20).  h_total 824
        // (hs 48 + bp 40 + active 720 + fp 16, the front porch inherited from
        // h_fp_bytes) holds the 30.6 kHz line rate of the 27 MHz mode it replaces
        // within 0.3%, so v_total barely moves and the active area grows from
        // 81.8% of the line to 87.4%.
        .vga_v_total = 510,           // 30582.5 / 510 = 59.966 Hz (was 58.890 — the old comment's h_total 864 was really 880)
        .vga_pixel_clk = 25200000,
        .vga_vsync_start = 500,
        .vga_vsync_end = 502,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 20,
        .vga_h_fp_bytes = 8,   // pinned: was inherited from h_fp_bytes (the HDMI 720 line moved it)
        .vga_screen_width = 360
#else
        // Preserve the v1.0.6 PIO geometry for analogue monitors.
        .vga_v_total = 521,
        .vga_pixel_clk = 27000000,
        .vga_vsync_start = 500,
        .vga_vsync_end = 502,
        .vga_h_sync_bytes = 24,
        .vga_h_bp_bytes = 48,
        .vga_h_fp_bytes = 8,   // pinned: was inherited from h_fp_bytes (the HDMI 720 line moved it)
        .vga_screen_width = 360
#endif
    },
    { // [8] 720x576 60Hz full border — 25.2MHz pixel (non-standard: v_active>v_total)
        .v_total = 524,   // 25.2MHz/800/524 ≈ 60.1Hz; v_active=576>524 so all lines are active
        .v_active = 576,
        .freq = 60,
        .pixel_clk = 25175000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV,
        // VGA: 25.2 MHz exactly, as [0].  Everything else is left alone —
        // v_active 576 > v_total 524 means every line is active and the vsync
        // window at 581..586 is never reached, which is what this mode has
        // always done on VGA.
        .vga_pixel_clk = 25200000
    },

    // ---------------------------------------------------------------------
    // 37.8 MHz pixel clock (sys_clk 378 MHz, PIO divider 1.0 — no fractional
    // divider at all).  Byte-for-byte the tables above: same h_total (800 px),
    // same v_total, so the line rate is 47.25 kHz and every refresh is exactly
    // x1.5.  Each sits at its standard twin's index + VMODE_FAST_OFFSET, which
    // is what graphics_fast_mode() resolves for VIDEO::Reset().
    //
    // NOTE the refresh is the DISPLAY's, not the machine's: with V-Sync on the
    // emulated frame rate follows it, so these modes force V-Sync off (see
    // resolveConstraints in UiStage.cpp).
    // ---------------------------------------------------------------------
    { // [9] 640x480 90Hz — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/524 = 90.17Hz (x1.5 of mode [0])
        .v_total = 524,
        .v_active = 480,
        .freq = 90,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [10] 640x480 75Hz Pentagon — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/644 = 73.37Hz (x1.5 of mode [1], Pentagon 48.83 x 1.5)
        .v_total = 644,
        .v_active = 480,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [11] 640x480 75Hz 48K — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/628 = 75.24Hz (x1.5 of mode [2])
        .v_total = 628,
        .v_active = 480,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [12] 640x480 75Hz 128K — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/629 = 75.12Hz (x1.5 of mode [3])
        .v_total = 629,
        .v_active = 480,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 320,
        .h_sync_bytes = 48,
        .h_bp_bytes = 24,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [13] 720x576 75Hz Pentagon — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/644 = 73.37Hz (x1.5 of mode [4])
        .v_total = 644,
        .v_active = 576,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = HDMI720_W,
        .h_sync_bytes = HDMI720_HS,
        .h_bp_bytes = HDMI720_BP,
        .h_fp_bytes = HDMI720_FP,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [14] 720x576 75Hz 48K — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/628 = 75.24Hz (x1.5 of mode [5])
        .v_total = 628,
        .v_active = 576,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = HDMI720_W,
        .h_sync_bytes = HDMI720_HS,
        .h_bp_bytes = HDMI720_BP,
        .h_fp_bytes = HDMI720_FP,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [15] 720x576 75Hz 128K — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/629 = 75.12Hz (x1.5 of mode [6])
        .v_total = 629,
        .v_active = 576,
        .freq = 75,
        .pixel_clk = 37800000,
        .vsync_start = 581,
        .vsync_end = 586,
        .screen_width = HDMI720_W,
        .h_sync_bytes = HDMI720_HS,
        .h_bp_bytes = HDMI720_BP,
        .h_fp_bytes = HDMI720_FP,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    },
    { // [16] 720x480 90Hz — 37.8MHz pixel (sys_clk=378MHz, div=1.0)
        // 37.8MHz/800/524 = 90.17Hz (x1.5 of mode [7]), half border
        .v_total = 524,
        .v_active = 480,
        .freq = 90,
        .pixel_clk = 37800000,
        .vsync_start = 490,
        .vsync_end = 492,
        .screen_width = 360,
        .h_sync_bytes = 16,
        .h_bp_bytes = 16,
        .h_fp_bytes = 8,
        .line_bytes = 400,
        .v_offset = 0,
        .pio_clk_div = PIO_DIV_FAST,
        .tmds_mhz = TMDS_FAST_MHZ
    }
};
