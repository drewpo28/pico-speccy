#pragma once
// 4-phase PWM for the VGA resistor ladder, driven by the RP2350's HSTX.
//
// The DAC is two bits per channel — 64 colours — and the driver has always spread
// the rest over a Bayer 2x2 block (vga_bayer4), which buys 13 levels per channel at
// the price of a spatial pattern: it is why the 16 flat ZX colours are forced solid
// and why a 1-pixel detail beats against the dither. PWM buys the levels INSIDE one
// pixel instead: four sub-samples per pixel, integrated by the ladder and the
// monitor's input, so every pixel carries its own level and no neighbour is
// involved.
//
// ONE OUTPUT PIXEL IS ONE 32-BIT FIFO WORD = four phase bytes, and it lasts
// `k` clk_hstx cycles, where k = clk_hstx / pixel_clock (CSR.N_SHIFTS = k,
// CSR.SHIFT = 16).  Each cycle is DDR — every pin takes shift-register bit i on the
// rising edge and bit i+8 on the falling one — and the register right-ROTATES by 16
// between cycles, so the half-cycles walk
//
//     b0 b1 | b2 b3 | b0 b1 | b2 b3 | ...        (2k half-cycles in all)
//
// i.e. word = p0 | p1<<8 | p2<<16 | p3<<24.
//
// **k NEED NOT BE EVEN.**  An odd k simply gives the four phases UNEQUAL WEIGHTS —
// k=5 is 3,3,2,2 half-cycles out of 10 — and that is not a defect to avoid, it is
// what puts 25.2 MHz (126/5) inside reach at all, and it buys MORE levels than the
// equal-weight case rather than fewer: 3a+3b+2c+2d with a..d in 0..3 reaches 29 of
// the 31 sums 0..30 (1 and 29 are the two it cannot make), against 13 evenly spaced
// values for k=4 or 6, where every weight is the same and only the total matters.
// So the shipped table's 720-wide and 60 Hz modes (25.2 MHz, k=5) get 29 levels per
// channel and its 640x480 50 Hz set (21 MHz, k=6) gets 13 — the same count the
// Bayer block reaches today, but PER PIXEL, with no spatial pattern and no need for
// the solid / grid-snap fork.  18 MHz (k=7, 37 levels) is the lever if that ever
// matters more than the 640x480 geometry; it needs h_total 720 and a 1.33 us back
// porch.  Everything below is written in terms of the weight vector for that
// reason; there is no even-k assumption left.
//
// Host-testable: <stdint.h> only. tools/vga_pwm_test.c builds against this header,
// not a copy — re-run it after ANY change here, because a wrong phase order or a
// wrong level map is a colour cast nothing else will show.
#include <stdint.h>

#define VGA_PWM_PHASES  4                    // sub-samples per output pixel

// HSTX engine settings for the VGA path (the HDMI one differs — see hdmi_word.h).
// N_SHIFTS is per mode (= k) and lives in vga_hstx.c.
#define VGA_PWM_HSTX_SHIFT 16

// The VGA pin byte: R in bits 5-4, G in 3-2, B in 1-0, HS bit 6, VS bit 7. Same
// layout the PIO program's `out pins, 8` has always driven, which is what lets the
// sync templates and the two border machines stay exactly as they are.
#define VGA_PWM_SYNC_MASK 0xC0u

// Half-cycles phase i occupies when a pixel lasts k clk_hstx cycles: the indices
// j in [0, 2k) with j % 4 == i.
static inline void vga_pwm_weights(const int k, uint8_t w[VGA_PWM_PHASES]) {
    for (int i = 0; i < VGA_PWM_PHASES; i++)
        w[i] = (uint8_t)((2 * k - i + (VGA_PWM_PHASES - 1)) / VGA_PWM_PHASES);
}

// The largest weighted sum, i.e. every phase at the DAC's top code 3.
static inline int vga_pwm_maxsum(const int k) { return 3 * 2 * k; }

// value -> the four 2-bit sub-samples, for one k.  1 KB, built once per video mode.
typedef struct {
    int     k;
    uint8_t w[VGA_PWM_PHASES];
    uint8_t ph[256][VGA_PWM_PHASES];
} vga_pwm_tab_t;

// Exhaustive over the 256 phase combinations, per 8-bit channel value: nearest
// weighted sum first, then the flattest set of phases (a 3,0,3,0 and a 2,1,2,1 can
// reach the same sum; the flat one puts less ripple on the ladder), then a fixed
// order so the table is deterministic and the host test can pin it.
static inline void vga_pwm_build(vga_pwm_tab_t *t, const int k) {
    t->k = k;
    vga_pwm_weights(k, t->w);
    const int maxsum = vga_pwm_maxsum(k);
    for (int v = 0; v < 256; v++) {
        // Round to nearest, so 0 -> 0 and 255 -> maxsum exactly.
        const int target = (v * maxsum + 127) / 255;
        int best_err = 1 << 30, best_spread = 1 << 30;
        uint8_t best[VGA_PWM_PHASES] = { 0, 0, 0, 0 };
        for (int c = 0; c < 256; c++) {
            uint8_t p[VGA_PWM_PHASES];
            int sum = 0, lo = 3, hi = 0;
            for (int i = 0; i < VGA_PWM_PHASES; i++) {
                p[i] = (uint8_t)((c >> (2 * i)) & 3);
                sum += t->w[i] * p[i];
                if (p[i] < lo) lo = p[i];
                if (p[i] > hi) hi = p[i];
            }
            const int err = sum > target ? sum - target : target - sum;
            const int spread = hi - lo;
            if (err < best_err || (err == best_err && spread < best_spread)) {
                best_err = err; best_spread = spread;
                for (int i = 0; i < VGA_PWM_PHASES; i++) best[i] = p[i];
            }
        }
        for (int i = 0; i < VGA_PWM_PHASES; i++) t->ph[v][i] = best[i];
    }
}

// One output pixel: an RGB888 colour plus the sync bits every phase carries.
static inline uint32_t vga_pwm_word(const vga_pwm_tab_t *t,
                                    const uint32_t rgb888, const uint8_t sync) {
    const uint8_t *r = t->ph[(rgb888 >> 16) & 0xff];
    const uint8_t *g = t->ph[(rgb888 >>  8) & 0xff];
    const uint8_t *b = t->ph[ rgb888        & 0xff];
    uint32_t w = 0;
    for (int i = 0; i < VGA_PWM_PHASES; i++) {
        const uint32_t px = (uint32_t)((r[i] << 4) | (g[i] << 2) | b[i])
                          | (uint32_t)(sync & VGA_PWM_SYNC_MASK);
        w |= px << (8 * i);
    }
    return w;
}

// A pixel that is nothing but sync — the porch and sync runs, where the ladder must
// sit at black however many phases go by.  Also the shape of a "PWM off" pixel: the
// same byte in all four phases, which is what VGA_HSTX_PWM=0 builds (see vga.c) so a
// colour fault can be told from a transport fault without a logic analyser.
static inline uint32_t vga_pwm_flat_word(const uint8_t pixel_byte) {
    const uint32_t px = pixel_byte;
    return px | (px << 8) | (px << 16) | (px << 24);
}
static inline uint32_t vga_pwm_sync_word(const uint8_t sync) {
    return vga_pwm_flat_word((uint8_t)(sync & VGA_PWM_SYNC_MASK));
}

// Pin-level model of what the serializer does with one word over a whole pixel:
// the eight pins across all 2k half-cycles. Used by the host test; the driver never
// calls it. `out` must hold 2*k bytes.
static inline uint32_t vga_pwm_rotr(const uint32_t w, int n) {
    n &= 31;
    return n ? ((w >> n) | (w << (32 - n))) : w;
}

static inline void vga_pwm_pins(const uint32_t w, const int k, uint8_t *out) {
    for (int c = 0; c < k; c++) {
        const uint32_t reg = vga_pwm_rotr(w, VGA_PWM_HSTX_SHIFT * c);
        for (int half = 0; half < 2; half++) {
            uint8_t pins = 0;
            for (int i = 0; i < 8; i++) {
                const int sel = half ? (i + 8) : i;
                pins |= (uint8_t)(((reg >> sel) & 1u) << i);
            }
            out[c * 2 + half] = pins;
        }
    }
}

// ---------------------------------------------------------------------------
// Clocks.  clk_hstx is 126 MHz on every CPU clock the Overclock menu offers
// (252/2, 378/3, 504/4) — CLOCKS_CLK_HSTX_DIV_INT is a TWO-BIT field with no
// fractional part, so those are the only dividers that exist, and 126 MHz is the
// one value all three reach.  A mode's pixel clock is therefore 126 MHz / k with k
// an integer 1..32, which is exactly the rule video_mode_table.h's VGA half obeys.
#define VGA_HSTX_CLK_HZ 126000000u

// Cycles per pixel for a mode's pixel clock, or 0 when it is not reachable — the
// caller must refuse rather than run at the nearest, because a wrong video clock is
// a wrong refresh and, with V-Sync on, a wrong emulated speed.
static inline int vga_hstx_cycles(const uint32_t pixel_hz) {
    if (!pixel_hz) return 0;
    const uint32_t k = (VGA_HSTX_CLK_HZ + pixel_hz / 2) / pixel_hz;
    if (k < 1 || k > 32) return 0;
    return (k * pixel_hz == VGA_HSTX_CLK_HZ) ? (int)k : 0;
}

// The PIO path programs sm->clkdiv with (sys/pixel) in 16.16 TRUNCATED to 1/16.
// Every VGA pixel clock in the shipped table is an exact integer divide of 252, 378
// and 504 MHz, so the truncation now loses nothing — it did before, which is why
// the 19.894737 MHz modes ran 0.33% fast at 252/378 and 0.25% slow at 504.
static inline uint32_t vga_pio_clkdiv_q16(const uint32_t sys_hz, const uint32_t pixel_hz) {
    const double fdiv = (double)sys_hz / (double)pixel_hz;
    return (uint32_t)(fdiv * (double)(1 << 16)) & 0xfffff000u;
}
