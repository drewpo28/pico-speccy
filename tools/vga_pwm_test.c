// 4-phase PWM for the VGA ladder over HSTX — checks the SHIPPED header.
//
//   gcc -O2 -Wall -Wextra -Idrivers/vga-nextgen tools/vga_pwm_test.c
//       -o /tmp/vga_pwm_test  &&  /tmp/vga_pwm_test
//
// Re-run after ANY change to drivers/vga-nextgen/vga_pwm.h.  A wrong phase order,
// a wrong weight or a wrong level map is a colour cast, and there is no way to see
// one without the hardware: the picture is there, the geometry is right, and every
// counter is clean.
//
// The model this checks against is the datasheet's, not the driver's: one 32-bit
// FIFO word is four phase BYTES; the shift register right-ROTATES by 16 each cycle
// and every pin takes bit i on the rising edge and bit i+8 on the falling one, so
// a pixel lasting k cycles emits the half-cycle sequence
//     b0 b1 | b2 b3 | b0 b1 | ...          (2k of them)
// and phase i is on the wire for the number of those slots with index == i mod 4.
#include <stdio.h>
#include <string.h>
#include "vga_pwm.h"

static int failures = 0;
static long checks  = 0;
#define FAIL(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } while (0)

// The weighted sum a phase set puts on one channel, i.e. what the ladder integrates.
static int wsum(const uint8_t w[4], const uint8_t p[4]) {
    return w[0]*p[0] + w[1]*p[1] + w[2]*p[2] + w[3]*p[3];
}

int main(void) {
    // ---- 1. the weights are the half-cycle counts, by brute force --------------
    for (int k = 1; k <= 32; k++) {
        uint8_t w[4], ref[4] = { 0, 0, 0, 0 };
        vga_pwm_weights(k, w);
        for (int j = 0; j < 2 * k; j++) ref[j % 4]++;
        for (int i = 0; i < 4; i++) {
            if (w[i] != ref[i]) FAIL("k=%d weight[%d] = %u, want %u", k, i, w[i], ref[i]);
            checks++;
        }
        if (w[0] + w[1] + w[2] + w[3] != 2 * k) FAIL("k=%d weights do not sum to 2k", k);
        if (vga_pwm_maxsum(k) != 3 * 2 * k)     FAIL("k=%d maxsum", k);
        checks += 2;
    }

    // ---- 2. the level table ----------------------------------------------------
    // Every k a shipped VGA mode can ask for, plus its neighbours.
    for (int k = 3; k <= 8; k++) {
        static vga_pwm_tab_t t;
        vga_pwm_build(&t, k);
        const int maxsum = vga_pwm_maxsum(k);

        if (wsum(t.w, t.ph[0])   != 0)      FAIL("k=%d: black is not 0", k);
        if (wsum(t.w, t.ph[255]) != maxsum) FAIL("k=%d: white is not the top level", k);
        checks += 2;

        int prev = -1, worst = 0, seen[4 * 32 * 3 + 1];
        memset(seen, 0, sizeof seen);
        for (int v = 0; v < 256; v++) {
            const int s = wsum(t.w, t.ph[v]);
            // Monotonic: a brighter value may never come out darker.
            if (s < prev) FAIL("k=%d: value %d sums to %d after %d", k, v, s, prev);
            prev = s;
            // ...and as close to the ideal as the level set allows.  The coarsest
            // gap between reachable sums is max(w), so half of it is the bound.
            const int want = (v * maxsum + 127) / 255;
            const int err = s > want ? s - want : want - s;
            if (err > (t.w[0] + 1) / 2)
                FAIL("k=%d: value %d wants %d, got %d (err %d)", k, v, want, s, err);
            if (err > worst) worst = err;
            seen[s] = 1;
            // Phases are 2-bit DAC codes and nothing else.
            for (int i = 0; i < 4; i++) if (t.ph[v][i] > 3) FAIL("k=%d: phase %d out of range", k, i);
            checks += 3;
        }
        int levels = 0;
        for (int s = 0; s <= maxsum; s++) levels += seen[s];
        printf("k=%2d  weights %u,%u,%u,%u of %2d   %3d distinct levels used, worst error %d/%d\n",
               k, t.w[0], t.w[1], t.w[2], t.w[3], 2 * k, levels, worst, maxsum);
    }

    // The two the shipped table actually uses, and the claim made for the odd one.
    {
        static vga_pwm_tab_t t5, t6;
        vga_pwm_build(&t5, 5);   // 25.2 MHz — the 720-wide modes and both 60 Hz ones
        vga_pwm_build(&t6, 6);   // 21   MHz — the 640x480 50 Hz set
        int n5 = 0, n6 = 0, s5[31] = {0}, s6[37] = {0};
        for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++)
        for (int c = 0; c < 4; c++) for (int d = 0; d < 4; d++) {
            const uint8_t p[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
            s5[wsum(t5.w, p)] = 1; s6[wsum(t6.w, p)] = 1;
        }
        for (int i = 0; i <= 30; i++) n5 += s5[i];
        for (int i = 0; i <= 36; i++) n6 += s6[i];
        // k=5 is 3,3,2,2: 3a+3b+2c+2d reaches 29 of the 31 sums 0..30.  The two it
        // cannot make are 1 and 29 — both need an odd multiple of 3 that is out of
        // range (29 = 3x+2y wants x odd with y >= 7).
        if (n5 != 29) FAIL("k=5 should reach 29 of the 31 sums, reaches %d", n5);
        if (s5[1] || s5[29]) FAIL("k=5 should not be able to make 1 or 29");
        // k=6 is 3,3,3,3: 13 evenly spaced levels, every third sum.
        if (n6 != 13) FAIL("k=6 should reach 13 levels, reaches %d", n6);
        checks += 3;
    }

    // ---- 3. word -> pins, the whole pixel --------------------------------------
    {
        static vga_pwm_tab_t t;
        const int k = 5;
        vga_pwm_build(&t, k);
        const uint32_t colours[] = { 0x000000, 0xffffff, 0x123456, 0xff8000,
                                     0x00ff00, 0x0000ff, 0x808080, 0xa2a2a2 };
        for (unsigned ci = 0; ci < sizeof(colours)/sizeof(colours[0]); ci++) {
            for (int s = 0; s < 4; s++) {
                const uint8_t sync = (uint8_t)(s << 6);
                const uint32_t w = vga_pwm_word(&t, colours[ci], sync);
                uint8_t pins[64];
                vga_pwm_pins(w, k, pins);
                // Every half-cycle carries the sync bits — a monitor must not see
                // them blink at 252 MHz.
                int rsum = 0, gsum = 0, bsum = 0;
                for (int j = 0; j < 2 * k; j++) {
                    if ((pins[j] & VGA_PWM_SYNC_MASK) != sync)
                        FAIL("colour %06x: sync lost at half-cycle %d", (unsigned)colours[ci], j);
                    rsum += (pins[j] >> 4) & 3;
                    gsum += (pins[j] >> 2) & 3;
                    bsum +=  pins[j]       & 3;
                    checks++;
                }
                // ...and the integrated level is the table's weighted sum, which is
                // the one thing that decides the colour on screen.
                const uint8_t *r = t.ph[(colours[ci] >> 16) & 0xff];
                const uint8_t *g = t.ph[(colours[ci] >>  8) & 0xff];
                const uint8_t *b = t.ph[ colours[ci]        & 0xff];
                if (rsum != wsum(t.w, r) || gsum != wsum(t.w, g) || bsum != wsum(t.w, b))
                    FAIL("colour %06x: pins integrate to %d/%d/%d, table says %d/%d/%d",
                         (unsigned)colours[ci], rsum, gsum, bsum,
                         wsum(t.w, r), wsum(t.w, g), wsum(t.w, b));
                checks += 3;
            }
        }
    }

    // ---- 3b. the half-cycle ORDER, against a sequence written from the datasheet
    // rather than from the model.  The average alone cannot see this — every phase
    // permutation integrates to the same level — but the order is what decides the
    // ripple frequency, and a wrong SHIFT or a swapped P/N half is a real fault.
    {
        // word = p0 | p1<<8 | p2<<16 | p3<<24.  SHIFT = 16, so cycle c reads the
        // register rotated right by 16c: cycle 0 gives (p0 on the rising edge, p1
        // on the falling), cycle 1 gives (p2, p3), cycle 2 is back to (p0, p1).
        const uint32_t w = 0xDDCCBBAAu;                       // p0=AA p1=BB p2=CC p3=DD
        const uint8_t seq[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        for (int k = 1; k <= 8; k++) {
            uint8_t pins[16];
            vga_pwm_pins(w, k, pins);
            for (int j = 0; j < 2 * k; j++) {
                if (pins[j] != seq[j % 4])
                    FAIL("k=%d: half-cycle %d = %02x, want %02x", k, j, pins[j], seq[j % 4]);
                checks++;
            }
        }
    }

    // ---- 3c. among the phase sets that reach the same level, the table must pick
    // the FLATTEST — 2,1,2,1 rather than 3,0,3,0 — or the ladder carries a bigger
    // swing at the pixel rate for no gain in accuracy.
    for (int k = 4; k <= 6; k++) {
        static vga_pwm_tab_t t;
        vga_pwm_build(&t, k);
        const int maxsum = vga_pwm_maxsum(k);
        for (int v = 0; v < 256; v++) {
            const int want = (v * maxsum + 127) / 255;
            const int got_sum = wsum(t.w, t.ph[v]);
            const int got_err = got_sum > want ? got_sum - want : want - got_sum;
            int got_spread, best_spread = 4;
            { int lo = 3, hi = 0;
              for (int i = 0; i < 4; i++) { if (t.ph[v][i] < lo) lo = t.ph[v][i];
                                            if (t.ph[v][i] > hi) hi = t.ph[v][i]; }
              got_spread = hi - lo; }
            for (int c = 0; c < 256; c++) {
                uint8_t p[4]; int sum = 0, lo = 3, hi = 0;
                for (int i = 0; i < 4; i++) {
                    p[i] = (uint8_t)((c >> (2 * i)) & 3); sum += t.w[i] * p[i];
                    if (p[i] < lo) lo = p[i];
                    if (p[i] > hi) hi = p[i];
                }
                const int e = sum > want ? sum - want : want - sum;
                if (e < got_err) FAIL("k=%d value %d: error %d beatable with %d", k, v, got_err, e);
                if (e == got_err && hi - lo < best_spread) best_spread = hi - lo;
            }
            if (got_spread != best_spread)
                FAIL("k=%d value %d: spread %d, flattest at the same error is %d",
                     k, v, got_spread, best_spread);
            checks += 2;
        }
    }

    // ---- 4. the flat words the porches and the PWM-off build use ---------------
    for (int v = 0; v < 256; v++) {
        uint8_t pins[64];
        vga_pwm_pins(vga_pwm_flat_word((uint8_t)v), 7, pins);
        for (int j = 0; j < 14; j++) {
            if (pins[j] != (uint8_t)v) FAIL("flat word %02x: half-cycle %d = %02x", v, j, pins[j]);
            checks++;
        }
    }
    for (int s = 0; s < 4; s++) {
        const uint8_t sync = (uint8_t)(s << 6);
        uint8_t pins[8];
        vga_pwm_pins(vga_pwm_sync_word(sync), 4, pins);
        for (int j = 0; j < 8; j++) {
            if (pins[j] != sync) FAIL("sync word %02x: half-cycle %d = %02x", sync, j, pins[j]);
            checks++;
        }
    }

    // ---- 5. which pixel clocks the serializer can make -------------------------
    {
        const struct { uint32_t hz; int k; const char *what; } cases[] = {
            { 21000000, 6, "640x480 50 Hz (the shipped set)" },
            { 25200000, 5, "720-wide and both 60 Hz modes" },
            { 31500000, 4, "reachable, unused" },
            { 19894737, 0, "the OLD 640x480 clock — must be refused" },
            { 27000000, 0, "the OLD 720-wide clock — must be refused" },
            { 37800000, 0, "the 90/75 Hz set — must be refused" },
            { 0,        0, "zero" },
        };
        for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            const int k = vga_hstx_cycles(cases[i].hz);
            if (k != cases[i].k)
                FAIL("%u Hz: %d cycles/pixel, want %d (%s)",
                     (unsigned)cases[i].hz, k, cases[i].k, cases[i].what);
            if (k && (uint32_t)k * cases[i].hz != VGA_HSTX_CLK_HZ)
                FAIL("%u Hz: %d cycles does not make %u Hz", (unsigned)cases[i].hz, k, VGA_HSTX_CLK_HZ);
            checks += 2;
        }
    }

    printf("\n%ld checks, %d failures\n", checks, failures);
    return failures != 0;
}
