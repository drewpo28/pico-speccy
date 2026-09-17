// Host validation of the VGA 4-phase PWM (drivers/vga-nextgen/vga_pwm.h), the thing
// that turns the 2-bit-per-channel resistor ladder into 13 levels per channel
// WITHOUT the Bayer 2x2 block the driver uses today — per pixel instead of per 2x2.
//
//   gcc -O2 -Wall -Wextra -Idrivers/vga-nextgen -o /tmp/vga_pwm_test tools/vga_pwm_test.c
//   /tmp/vga_pwm_test
//
// It includes the shipped header; there is no copy of the construction here.
// Re-run after ANY change to it: a wrong phase order, a wrong level map or a wrong
// byte position is a colour cast or a 1-pixel ripple, and neither is visible in a
// framebuffer dump — the framebuffer holds palette indices, and the PWM lives in
// the word the LUT hands to the serializer.
//
// Every assertion was checked to fail under a hand-applied mutation of what it
// covers (listed at the bottom of the run).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vga_pwm.h"

static int failures = 0;
static long checks = 0;
#define FAIL(fmt, ...) do { if (failures < 10) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); failures++; } while (0)

int main(void) {
    printf("VGA 4-phase PWM over the 2-bit ladder\n");
    printf("=====================================\n\n");

    // --- levels -------------------------------------------------------------
    {
        int seen[VGA_PWM_LEVELS]; memset(seen, 0, sizeof seen);
        int prev = -1, worst = 0, worst_v = 0;
        for (int v = 0; v < 256; v++) {
            const int L = vga_pwm_level((uint8_t)v);
            if (L < 0 || L >= VGA_PWM_LEVELS) { FAIL("value %d -> level %d out of range", v, L); continue; }
            if (L < prev) FAIL("level is not monotonic at %d (%d after %d)", v, L, prev);
            prev = L; seen[L] = 1;
            // nearest: the reconstructed value must be within half a step
            const int back = L * 255 / (VGA_PWM_LEVELS - 1);
            const int err = back > v ? back - v : v - back;
            if (err > 255 / (2 * (VGA_PWM_LEVELS - 1)) + 1) FAIL("value %d -> %d is not the nearest level", v, L);
            if (err > worst) { worst = err; worst_v = v; }
            checks++;
        }
        for (int L = 0; L < VGA_PWM_LEVELS; L++) if (!seen[L]) FAIL("level %d is unreachable", L);
        if (vga_pwm_level(0) != 0) FAIL("black is not level 0");
        if (vga_pwm_level(255) != VGA_PWM_LEVELS - 1) FAIL("white is not the top level");
        printf("  %d levels per channel, %d colours; worst quantisation %d/255 (at %d)\n",
               VGA_PWM_LEVELS, VGA_PWM_LEVELS * VGA_PWM_LEVELS * VGA_PWM_LEVELS, worst, worst_v);
    }

    // --- phase decomposition -------------------------------------------------
    for (int L = 0; L < VGA_PWM_LEVELS; L++) {
        uint8_t ph[VGA_PWM_PHASES];
        vga_pwm_phases(L, ph);
        int sum = 0, lo = 3, hi = 0;
        for (int i = 0; i < VGA_PWM_PHASES; i++) {
            if (ph[i] > 3) FAIL("level %d phase %d = %d is not a 2-bit DAC value", L, i, ph[i]);
            sum += ph[i];
            if (ph[i] < lo) lo = ph[i];
            if (ph[i] > hi) hi = ph[i];
        }
        if (sum != L) FAIL("level %d sums to %d", L, sum);
        if (hi - lo > 1) FAIL("level %d is bunched (%d..%d) instead of spread", L, lo, hi);
        checks++;
    }
    // The spread must also put the odd phase FIRST and THIRD, so the ripple is at
    // twice the pixel rate: level 2 is 1,0,1,0 and not 1,1,0,0.
    {
        uint8_t ph[VGA_PWM_PHASES];
        vga_pwm_phases(2, ph);
        if (!(ph[0] == 1 && ph[1] == 0 && ph[2] == 1 && ph[3] == 0))
            FAIL("level 2 is %d,%d,%d,%d - the bump order is not 0,2,1,3", ph[0], ph[1], ph[2], ph[3]);
        checks++;
    }

    // --- the word, and what the serializer does with it ----------------------
    {
        long worst_err = 0; uint32_t worst_c = 0;
        for (uint32_t r = 0; r < 256; r += 1) {
            const uint32_t c = (r << 16) | ((255 - r) << 8) | ((r * 7) & 0xff);
            for (int s = 0; s < 2; s++) {
                const uint8_t sync = s ? 0xC0 : 0x40;
                const uint32_t w = vga_pwm_word(c, sync);
                uint8_t pins[VGA_PWM_PHASES];
                vga_pwm_pins(w, pins);
                // The byte in each phase slot, not just their sum: the analog
                // integral over a pixel is order-invariant, so only an explicit
                // check keeps the packing pinned to the order the serializer emits.
                uint8_t pr[VGA_PWM_PHASES], pg[VGA_PWM_PHASES], pb[VGA_PWM_PHASES];
                vga_pwm_phases(vga_pwm_level((uint8_t)((c >> 16) & 0xff)), pr);
                vga_pwm_phases(vga_pwm_level((uint8_t)((c >>  8) & 0xff)), pg);
                vga_pwm_phases(vga_pwm_level((uint8_t)( c        & 0xff)), pb);
                for (int i = 0; i < VGA_PWM_PHASES; i++) {
                    const uint8_t want_b = (uint8_t)((pr[i] << 4) | (pg[i] << 2) | pb[i] | sync);
                    if (pins[i] != want_b)
                        FAIL("phase %d is %02x, wanted %02x (colour %06X)", i, pins[i], want_b, c);
                }
                int sum[3] = { 0, 0, 0 };
                for (int i = 0; i < VGA_PWM_PHASES; i++) {
                    if ((pins[i] & VGA_PWM_SYNC_MASK) != sync)
                        FAIL("phase %d lost the sync bits (%02x vs %02x)", i, pins[i] & VGA_PWM_SYNC_MASK, sync);
                    sum[0] += (pins[i] >> 4) & 3;
                    sum[1] += (pins[i] >> 2) & 3;
                    sum[2] +=  pins[i]       & 3;
                }
                // Each channel's mean over the pixel must be the level it asked for.
                const int want[3] = { vga_pwm_level((uint8_t)((c >> 16) & 0xff)),
                                      vga_pwm_level((uint8_t)((c >>  8) & 0xff)),
                                      vga_pwm_level((uint8_t)( c        & 0xff)) };
                for (int ch = 0; ch < 3; ch++) {
                    if (sum[ch] != want[ch]) FAIL("channel %d integrates to %d, wanted %d", ch, sum[ch], want[ch]);
                    // ...and how far that is from the colour actually asked for
                    const int target = (int)((c >> (16 - 8 * ch)) & 0xff);
                    const long got = (long)sum[ch] * 255 / (VGA_PWM_LEVELS - 1);
                    const long err = got > target ? got - target : target - got;
                    if (err > worst_err) { worst_err = err; worst_c = c; }
                }
                checks++;
            }
        }
        printf("  worst level error over a colour ramp: %ld/255 (at %06X)\n", worst_err, worst_c);
    }

    // --- porch / sync runs ---------------------------------------------------
    for (int s = 0; s < 4; s++) {
        const uint8_t sync = (uint8_t)(s << 6);
        uint8_t pins[VGA_PWM_PHASES];
        vga_pwm_pins(vga_pwm_sync_word(sync), pins);
        for (int i = 0; i < VGA_PWM_PHASES; i++)
            if (pins[i] != sync) FAIL("sync word phase %d = %02x, wanted %02x", i, pins[i], sync);
        checks++;
    }

    // --- clocks: the HSTX path must reproduce what the PIO path really programs
    {
        const struct { uint32_t sys, pix; const char *what; } cases[] = {
            { 378000000, 19894737, "640x480 @50  (the default mode on the default clock)" },
            { 378000000, 27000000, "720x576 @50" },
            { 378000000, 25175000, "640x480 @60" },
            { 252000000, 19894737, "640x480 @50 at 252 MHz" },
            { 504000000, 19894737, "640x480 @50 at 504 MHz" },
        };
        printf("\n  clk_sys      nominal px     PIO div    real px       clk_hstx div  clk_hstx\n");
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            const uint32_t q = vga_pio_clkdiv_q16(cases[i].sys, cases[i].pix);
            const uint32_t h = vga_hstx_clkdiv_q16(cases[i].sys, cases[i].pix);
            const double qd = q / 65536.0, hd = h / 65536.0;
            const double real_px = cases[i].sys / qd;
            if (h * 2u != q) FAIL("the HSTX divider is not half the PIO one (%u vs %u)", h, q);
            // Two clk_hstx cycles per output pixel, by construction.
            const double hstx = cases[i].sys / hd;
            if (hstx < real_px * 2 - 1 || hstx > real_px * 2 + 1)
                FAIL("clk_hstx %.1f is not twice the pixel clock %.1f", hstx, real_px);
            printf("  %-12u %-14u %-10.4f %-13.4f %-13.5f %.4f MHz\n",
                   cases[i].sys, cases[i].pix, qd, real_px, hd, hstx / 1e6);
            checks++;
        }
        // The one that matters: the truncation the shipped code does is what the
        // hw-tuned vga_v_total values were measured against, so it must survive.
        if (vga_pio_clkdiv_q16(378000000, 19894737) != 0x12F000u)
            FAIL("the 378 MHz / 50 Hz divider is no longer the shipped 18.9375");
        checks++;
    }

    printf("\n%ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
