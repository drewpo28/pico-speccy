// VGA timing check for the SHIPPED mode table.
//
//   gcc -DVGA_HSTX=1 -O2 -Wall -Wextra -I drivers/graphics tools/vga_timing_test.c -lm
//       -o /tmp/vga_timing_test  &&  /tmp/vga_timing_test
//
// Also build without -DVGA_HSTX=1 to check the legacy PIO geometry.
// Re-run after ANY change to drivers/graphics/video_mode_table.h.  A VGA timing
// typo does not misbehave by degrees: the monitor either loses sync or the
// emulator, which paces one guest frame per display frame under V-Sync, runs at
// the wrong speed with pitched-up sound.  Neither is visible in a code review and
// the second is not obvious on a screen either.
//
// What it pins, per mode, computed the way vga.c computes it (a VGA h_* "byte" is
// two output pixels and a zero field INHERITS the HDMI one — including
// vga_h_fp_bytes, which is how the 720-wide modes carry a 16 px front porch they
// never spell out):
//
//   1. on HSTX builds, the pixel clock is 126 MHz / k, k an integer 1..32 — the set the RP2350
//      HSTX serializer can produce (CLOCKS_CLK_HSTX_DIV_INT is a two-bit field);
//   2. for those HSTX clocks, the PIO gets that clock EXACTLY at 252, 378 and 504 MHz, through vga.c's
//      truncate-and-mask-to-1/16 CLKDIV;
//   3. line_size and shift_picture are multiples of 4 (the line DMA is 32-bit and
//      the ISR's background fill writes through a uint32*) and line_size fits
//      VGA_MAX_LINE_SIZE;
//   4. on HSTX builds, the refresh is within 0.25% of the machine the mode is for — that IS the
//      emulated frame rate when V-Sync is on;
//   5. sync and porch widths stay inside what an analogue monitor needs.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "video_mode_table.h"


static int fails = 0;
static void chk(int ok, const char *mode, const char *what, const char *detail) {
    if (!ok) { printf("FAIL %-16s %s: %s\n", mode, what, detail); fails++; }
}

// vga.c: fdiv = clk_sys / pixel_clk; CLKDIV = (uint32)(fdiv * 65536) & 0xfffff000
static double pio_pixel_hz(double sys_hz, double px_hz) {
    const double fdiv = sys_hz / px_hz;
    const unsigned d32 = (unsigned)(fdiv * 65536.0) & 0xfffff000u;
    return d32 ? sys_hz / (d32 / 65536.0) : 0.0;
}

typedef struct { const char *name; double refresh; int vga_used; } target_t;

// The machine each 50 Hz variant exists for.  48K/Pentagon run a 3.5 MHz Z80,
// the 128K a 3.5469 MHz one; the frame is statesInFrame T-states.
#define HZ_PENTAGON (3500000.0 / 71680.0)   /* 48.828125 */
#define HZ_48K      (3500000.0 / 69888.0)   /* 50.08013  */
#define HZ_128K     (3546900.0 / 70908.0)   /* 50.02115  */

static const target_t targets[] = {
    { "[0] 640x480 60",  60.0,        1 },
    { "[1] 640 Pent",    HZ_PENTAGON, 1 },
    { "[2] 640 48K",     HZ_48K,      1 },
    { "[3] 640 128K",    HZ_128K,     1 },
    { "[4] 720 Pent",    HZ_PENTAGON, 1 },
    { "[5] 720 48K",     HZ_48K,      1 },
    { "[6] 720 128K",    HZ_128K,     1 },
    { "[7] 720x480 60",  60.0,        1 },
    // [8] is the known odd one: v_active 576 > v_total 524, so every line is
    // active and the vsync window is never reached.  Its clock and layout are
    // checked; its refresh and vertical rules are not, because it has none.
    { "[8] 720x576 60",  60.0,        0 },
};
#define NTARGETS ((int)(sizeof(targets)/sizeof(targets[0])))

int main(void) {
    printf("%-16s %8s %2s %5s %-22s %8s %5s %9s %8s %6s\n",
           "mode", "px MHz", "k", "h_tot", "hs/bp/fp px", "kHz", "v_tot",
           "Hz", "target", "err");
    for (int i = 0; i < NTARGETS; i++) {
        const struct video_mode_t *m = &video_mode[i];
        const target_t *t = &targets[i];
        char d[160];

        const int px = m->vga_pixel_clk ? m->vga_pixel_clk : m->pixel_clk;
        const int hs = (m->vga_h_sync_bytes ? m->vga_h_sync_bytes : m->h_sync_bytes) * 2;
        const int bp = (m->vga_h_bp_bytes   ? m->vga_h_bp_bytes   : m->h_bp_bytes)   * 2;
        const int fp = (m->vga_h_fp_bytes   ? m->vga_h_fp_bytes   : m->h_fp_bytes)   * 2;
        const int sw = (m->vga_screen_width ? m->vga_screen_width : m->screen_width);
        const int active = sw * 2;
        const int h_total = hs + bp + active + fp;
        const int shift   = hs + bp;
        const int v_total = m->vga_v_total ? m->vga_v_total : m->v_total;
        const int v_act   = m->vga_v_active ? m->vga_v_active : m->v_active;
        const int vs_end  = m->vga_vsync_end ? m->vga_vsync_end : m->vsync_end;
        const double line_hz = (double)px / h_total;
        const double hz = line_hz / v_total;

#if !VGA_HSTX
        // Regression: HSTX clock constraints must not change PIO monitor geometry.
        static const int legacy[][4] = {
            {25200000, 800, 144, 524},
            {19894737, 800, 144, 511},
            {19894737, 800, 144, 499},
            {19894737, 800, 144, 498},
            {27000000, 880, 144, 628},
            {27000000, 880, 144, 614},
            {27000000, 880, 144, 612},
            {27000000, 880, 144, 521},
            {25200000, 800,  64, 524},
        };
        snprintf(d, sizeof d, "pixel %d, line %d, active offset %d, vertical %d",
                 px, h_total, shift, v_total);
        chk(px == legacy[i][0] && h_total == legacy[i][1] &&
            shift == legacy[i][2] && v_total == legacy[i][3],
            t->name, "PIO compatibility", d);
#endif

        // 1. HSTX-reachable: pixel = 126 MHz / k, k integer in 1..32.
        const double kf = 126000000.0 / px;
        const int k = (int)(kf + 0.5);
        snprintf(d, sizeof d, "126 MHz / %g is not an integer 1..32", kf);
        if (VGA_HSTX) chk(fabs(kf - k) < 1e-9 && k >= 1 && k <= 32, t->name, "HSTX clock", d);

        // 2. the PIO reproduces it exactly at every CPU clock we offer.
        for (int c = 0; c < 3; c++) {
            const double sys = (double)(unsigned[]){252, 378, 504}[c] * 1e6;
            const double got = pio_pixel_hz(sys, px);
            snprintf(d, sizeof d, "at %.0f MHz the PIO gives %.4f MHz, not %.4f",
                     sys / 1e6, got / 1e6, px / 1e6);
            if (VGA_HSTX) chk(fabs(got - px) < 1.0, t->name, "PIO clock", d);
        }

        // 3. DMA / ISR alignment.
        snprintf(d, sizeof d, "line_size %d, shift %d, max %d", h_total, shift, VGA_MAX_LINE_SIZE);
        chk(h_total % 4 == 0 && shift % 4 == 0 && h_total <= VGA_MAX_LINE_SIZE,
            t->name, "alignment", d);

        // 4. refresh.
        const double err = 100.0 * (hz / t->refresh - 1.0);
        if (t->vga_used) {
            snprintf(d, sizeof d, "%.3f Hz against %.3f (%+.3f%%)", hz, t->refresh, err);
            if (VGA_HSTX) chk(fabs(err) < 0.25, t->name, "refresh", d);

            // 5. vertical: the sync window must fit, and a monitor needs blanking.
            snprintf(d, sizeof d, "v_total %d, v_active %d, vsync_end %d", v_total, v_act, vs_end);
            chk(v_total > vs_end && v_total >= v_act + 8, t->name, "vertical", d);
        }

        // 5b. horizontal: sync and back porch, in microseconds.  The floor is what
        // this firmware has always shipped and a monitor has always accepted —
        // mode [8] has a 32 px sync at 25.2 MHz = 1.27 us — not a VESA minimum.
        const double us_hs = 1e6 * hs / px, us_bp = 1e6 * bp / px;
        snprintf(d, sizeof d, "hsync %.2f us, back porch %.2f us", us_hs, us_bp);
        chk(us_hs >= 1.20 && us_bp >= 1.20, t->name, "horizontal", d);

        printf("%-16s %8.4f %2d %5d %5d/%3d/%3d %10.3f %5d %9.3f %8.3f %+6.2f%%\n",
               t->name, px / 1e6, k, h_total, hs, bp, fp, line_hz / 1e3, v_total,
               hz, t->refresh, err);
    }

    // The 37.8 MHz "fast" set has no VGA override and is NOT HSTX-reachable
    // (126/37.8 = 3.333); it stays a PIO-only mode.  Assert that it really has no
    // vga_pixel_clk, so nobody "fixes" it into a VGA-HSTX build by accident.
    for (int i = VMODE_FAST_OFFSET; i < (int)(sizeof(video_mode)/sizeof(video_mode[0])); i++) {
        char d[80];
        snprintf(d, sizeof d, "mode %d has a vga_pixel_clk", i);
        chk(video_mode[i].vga_pixel_clk == 0, "fast set", "no VGA override", d);
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASS", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
