#pragma once
// 4-phase PWM for the VGA resistor ladder, driven by the RP2350's HSTX.
//
// The DAC is two bits per channel — 64 colours — and the driver has always spread
// the rest over a Bayer 2x2 block (vga_bayer4), which buys 13 levels per channel at
// the price of a spatial pattern: it is why the 16 flat ZX colours are forced solid
// and why a 1-pixel detail beats against the dither. PWM buys the same 13 levels
// INSIDE one pixel instead: four sub-samples per pixel, integrated by the ladder and
// the monitor's input, so every pixel carries its own level and no neighbour is
// involved.
//
// One output pixel is one 32-bit FIFO word = four phase bytes. The HSTX shift
// register right-ROTATES by 16 each cycle and every pin takes bit i on the rising
// edge and bit i+8 on the falling one, so:
//
//   cycle 0 -> phase 0 = byte 0, phase 1 = byte 1
//   cycle 1 -> phase 2 = byte 2, phase 3 = byte 3   (the register is rotated by 16)
//
// i.e. word = p0 | p1<<8 | p2<<16 | p3<<24, and a pixel lasts two clk_hstx cycles.
//
// Host-testable: <stdint.h> only. tools/vga_pwm_test.c builds against this header,
// not a copy — re-run it after ANY change here, because a wrong phase order or a
// wrong level map is a colour cast nothing else will show.
#include <stdint.h>

#define VGA_PWM_PHASES  4                    // sub-samples per output pixel
#define VGA_PWM_LEVELS  (3 * VGA_PWM_PHASES + 1)   // 0..12 -> 13 levels per channel

// HSTX engine settings for the VGA path (the HDMI one differs — see hdmi_word.h).
#define VGA_PWM_HSTX_SHIFT    16
#define VGA_PWM_HSTX_N_SHIFTS 2

// The VGA pin byte: R in bits 5-4, G in 3-2, B in 1-0, HS bit 6, VS bit 7. Same
// layout the PIO program's `out pins, 8` has always driven, which is what lets the
// sync templates and the two border machines stay exactly as they are.
#define VGA_PWM_SYNC_MASK 0xC0u

// 8-bit channel value -> level. Nearest of the 13, rather than the top nibble: the
// 16-entry table this replaces quantised twice over.
static inline int vga_pwm_level(const uint8_t v) {
    return (v * (VGA_PWM_LEVELS - 1) + 127) / 255;
}

// Level -> the four 2-bit sub-samples. Each phase is floor(level/4), with level%4 of
// them bumped by one in the order 0, 2, 1, 3 — spread rather than bunched, so the
// ripple sits at twice the pixel rate instead of at the pixel rate.
static const uint8_t vga_pwm_bump_order[VGA_PWM_PHASES] = { 0, 2, 1, 3 };

static inline void vga_pwm_phases(int level, uint8_t ph[VGA_PWM_PHASES]) {
    if (level < 0) level = 0;
    if (level > VGA_PWM_LEVELS - 1) level = VGA_PWM_LEVELS - 1;
    const int base = level / VGA_PWM_PHASES;
    const int bump = level % VGA_PWM_PHASES;
    for (int i = 0; i < VGA_PWM_PHASES; i++) ph[i] = (uint8_t)base;
    for (int i = 0; i < bump; i++) ph[vga_pwm_bump_order[i]]++;
}

// One output pixel: an RGB888 colour plus the sync bits that phase carries.
static inline uint32_t vga_pwm_word(const uint32_t rgb888, const uint8_t sync) {
    uint8_t r[VGA_PWM_PHASES], g[VGA_PWM_PHASES], b[VGA_PWM_PHASES];
    vga_pwm_phases(vga_pwm_level((uint8_t)((rgb888 >> 16) & 0xff)), r);
    vga_pwm_phases(vga_pwm_level((uint8_t)((rgb888 >>  8) & 0xff)), g);
    vga_pwm_phases(vga_pwm_level((uint8_t)( rgb888        & 0xff)), b);
    uint32_t w = 0;
    for (int i = 0; i < VGA_PWM_PHASES; i++) {
        const uint32_t px = (uint32_t)((r[i] << 4) | (g[i] << 2) | b[i])
                          | (uint32_t)(sync & VGA_PWM_SYNC_MASK);
        w |= px << (8 * i);
    }
    return w;
}

// A phase byte that is nothing but sync — the porch and sync runs, where the ladder
// must sit at black however many phases go by.
static inline uint32_t vga_pwm_sync_word(const uint8_t sync) {
    const uint32_t px = (uint32_t)(sync & VGA_PWM_SYNC_MASK);
    return px | (px << 8) | (px << 16) | (px << 24);
}

// The register right-ROTATES, which is what lets N_SHIFTS repeat data when
// SHIFT * N_SHIFTS exceeds 32 (the datasheet says so explicitly). Inside 32 a
// rotate and a shift are indistinguishable, and these settings stay inside it — if
// that ever changes, the model below has to be a real rotate and so does anything
// reasoning about which bits a cycle reads.
_Static_assert(VGA_PWM_HSTX_SHIFT * VGA_PWM_HSTX_N_SHIFTS <= 32,
               "phases would repeat: the rotate wraps and the packing above is wrong");

// Pin-level model of what the serializer does with one word: the eight pins over
// the four phases. Every pin takes bit i on the rising edge of a clk_hstx cycle and
// bit i+8 on the falling one, and the register right-rotates by 16 between the two
// cycles. Used by the host test; the driver never calls it.
static inline uint32_t vga_pwm_rotr(const uint32_t w, int k) {
    k &= 31;
    return k ? ((w >> k) | (w << (32 - k))) : w;
}

static inline void vga_pwm_pins(const uint32_t w, uint8_t out[VGA_PWM_PHASES]) {
    for (int c = 0; c < VGA_PWM_HSTX_N_SHIFTS; c++) {
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
// Clocks. The PIO path programs sm->clkdiv with (sys/pixel) in 16.16 TRUNCATED to
// 1/16, and that truncation is load-bearing: at 378 MHz the 50 Hz modes' nominal
// 19894737 Hz comes out as a divider of 18.9375 and a pixel clock of 19.9604 MHz,
// which is what the empirically tuned vga_v_total values (511 / 499) were measured
// against. The HSTX path must reproduce THAT clock, not the nominal one, or every
// 50 Hz VGA mode moves by 0.33% and the tuned refresh goes with it.
//
// Returns the divider in 16.16, exactly as vga.c computes it.
static inline uint32_t vga_pio_clkdiv_q16(const uint32_t sys_hz, const uint32_t pixel_hz) {
    const double fdiv = (double)sys_hz / (double)pixel_hz;
    return (uint32_t)(fdiv * (double)(1 << 16)) & 0xfffff000u;
}

// clk_hstx for that pixel clock: two cycles per output pixel, so half the divider.
// In 16.16; the clocks block takes an integer+fractional divider of at least this
// precision, so halving an already-quantised value is exact.
static inline uint32_t vga_hstx_clkdiv_q16(const uint32_t sys_hz, const uint32_t pixel_hz) {
    return vga_pio_clkdiv_q16(sys_hz, pixel_hz) / VGA_PWM_HSTX_N_SHIFTS;
}
