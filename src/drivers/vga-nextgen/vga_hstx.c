#include "vga_hstx.h"

#if VGA_HSTX

#include <stdio.h>
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/regs/dreq.h"
#include "vga.h"
#include "vga_pwm.h"

// The serializer's bit[i] drives GPIO 12+i and nothing else — the block is wired to
// that pin range in silicon — so this back-end only exists where the display is
// already there.  CMake refuses the combination elsewhere; this is the backstop.
#if VGA_BASE_PIN != 12
#error "VGA_HSTX needs the display on GPIO 12-19 (VGA_BASE_PIN=12)"
#endif

// Pad settings for the ladder.  4 mA is what the PIO path has always given these
// pins (pio_gpio_init leaves the reset default), and the ladder's resistor values
// were chosen around that source impedance — changing it would move every colour.
// The slew rate is the one thing that does change: a phase is one half of a
// clk_hstx cycle, 3.97 ns at 126 MHz, and a slow edge would not reach its level
// before the next phase, which turns the PWM's average into something the
// resistors, not the table, decide.
#define VGA_HSTX_DRIVE  GPIO_DRIVE_STRENGTH_4MA
#define VGA_HSTX_SLEW   GPIO_SLEW_RATE_FAST

static int s_k = 0;   // clk_hstx cycles per pixel of the running mode

volatile void *vga_hstx_fifo(void) { return (volatile void *)&hstx_fifo_hw->fifo; }
uint32_t vga_hstx_fifo_stat(void)  { return hstx_fifo_hw->stat; }
unsigned vga_hstx_dreq(void)       { return DREQ_HSTX; }
int vga_hstx_cycles_live(void)     { return s_k; }

void vga_hstx_stop(void) {
    hstx_ctrl_hw->csr = 0;
    s_k = 0;
}

// clk_hstx = 126 MHz on every CPU clock the Overclock menu offers: 252/2, 378/3,
// 504/4.  CLOCKS_CLK_HSTX_DIV_INT is a TWO-BIT field with no fractional part, so
// 1, 2, 3 and 4 are the only dividers that exist (the datasheet's "0 -> max+1" is
// what makes 4 reachable) — which is the whole reason the VGA pixel clocks had to
// move onto 126/k in the first place.  Do not "fix" this into a mask-and-clamp.
static bool vga_hstx_clock(void) {
    const uint32_t sys = (uint32_t)clock_get_hz(clk_sys);
    const uint32_t div = (sys + VGA_HSTX_CLK_HZ / 2) / VGA_HSTX_CLK_HZ;
    if (div < 1 || div > 4 || div * VGA_HSTX_CLK_HZ != sys) {
        printf("vga_hstx: clk_sys %u cannot give %u Hz with an integer 1..4 divider\n",
               (unsigned)sys, (unsigned)VGA_HSTX_CLK_HZ);
        return false;
    }
    // An ODD divider costs the PHASE WEIGHTS, not the picture.  The generator has
    // no DC50 bit (only GPOUT0-3 do), so at /3 the clock is 1/3-2/3 and the DDR
    // half-cycles alternate wide/narrow — and a phase takes every 4th half-cycle,
    // i.e. always the SAME parity, so the weights become W,N,W,N instead of equal.
    // A level whose four sub-samples are equal is still exact (that is most of
    // them); only the mixed ones skew, by about one DAC step.  Harmless here where
    // the HDMI half is not — the ladder integrates, a TMDS receiver does not.
    if (div & 1u) {
        printf("vga_hstx: clk_sys/%u is an ODD divider - clk_hstx duty is not 50%%, "
               "so the four PWM phase weights are uneven (mixed levels off by ~1 "
               "step). Prefer an even divider (clk_sys 252 or 504).\n", (unsigned)div);
    }
    clock_configure_int_divider(clk_hstx, 0,
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                                sys, div);
    return true;
}

// The engine.  SHIFT = 16 and N_SHIFTS = k, so one 32-bit word is one pixel held
// for k cycles and the register's right-ROTATE walks its two halves alternately —
// that is the datasheet's own reason for a rotate rather than a shift ("it also
// allows data to be repeated, when the product of SHIFT and N_SHIFTS is greater
// than 32"), and it is what lets a pixel last more than two cycles at all.
// CLKDIV/CLKPHASE are don't-care: no pin has BITn_CLK set, VGA carries its sync on
// ordinary data bits.
static void vga_hstx_engine(const int k) {
    hstx_ctrl_hw->csr =
          ((uint32_t)1                  << HSTX_CTRL_CSR_CLKDIV_LSB)
        | ((uint32_t)(k & 0x1f)         << HSTX_CTRL_CSR_N_SHIFTS_LSB)
        | ((uint32_t)VGA_PWM_HSTX_SHIFT << HSTX_CTRL_CSR_SHIFT_LSB)
        | HSTX_CTRL_CSR_EN_BITS;
    s_k = k;
}

bool vga_hstx_set_pixel_clk(const uint32_t pixel_hz) {
    const int k = vga_hstx_cycles(pixel_hz);
    if (!k) {
        printf("vga_hstx: pixel clock %u Hz is not 126 MHz / k (k = 1..32) - refused\n",
               (unsigned)pixel_hz);
        return false;
    }
    if (k == s_k) return true;
    // EN low resets the shift counter, so the first word after the restart starts a
    // fresh pixel; the DMA is stopped by the caller across a mode change anyway.
    hstx_ctrl_hw->csr = 0;
    vga_hstx_engine(k);
    return true;
}

bool vga_hstx_start(const uint32_t pixel_hz) {
    const int k = vga_hstx_cycles(pixel_hz);
    if (!k) {
        printf("vga_hstx: pixel clock %u Hz is not 126 MHz / k (k = 1..32) - staying on the PIO\n",
               (unsigned)pixel_hz);
        return false;
    }
    vga_hstx_stop();
    if (!vga_hstx_clock()) return false;

    // Every pin takes shift-register bit i on the rising edge of a cycle and bit
    // i+8 on the falling one: two phase bytes per cycle, four per word.  The pin
    // order is the PIO program's — `out pins, 8` with a right-shifting OSR puts
    // byte bit i on VGA_BASE_PIN + i — so the byte layout (b7 VS, b6 HS, b5-4 R,
    // b3-2 G, b1-0 B) and with it both border machines and the sync templates are
    // untouched.
    for (int i = 0; i < 8; i++)
        hstx_ctrl_hw->bit[i] = ((uint32_t)i       << HSTX_CTRL_BIT0_SEL_P_LSB)
                             | ((uint32_t)(i + 8) << HSTX_CTRL_BIT0_SEL_N_LSB);

    vga_hstx_engine(k);

    for (int i = 0; i < 8; i++) {
        const unsigned pin = (unsigned)(VGA_BASE_PIN + i);
        gpio_set_function(pin, GPIO_FUNC_HSTX);
        gpio_set_drive_strength(pin, VGA_HSTX_DRIVE);
        gpio_set_slew_rate(pin, VGA_HSTX_SLEW);
    }

    uint8_t w[VGA_PWM_PHASES];
    vga_pwm_weights(k, w);
    // NB the level SPAN is 0..maxsum; how many of those sums the four 2-bit phases
    // can actually reach is a different number (29 of 31 at k=5) — see vga_pwm.h.
    printf("vga_hstx: pixel %u Hz = clk_hstx %u / %d, phase weights %u,%u,%u,%u of %d, "
           "levels 0..%d, pins %d-%d\n",
           (unsigned)pixel_hz, (unsigned)clock_get_hz(clk_hstx), k,
           w[0], w[1], w[2], w[3], 2 * k, vga_pwm_maxsum(k),
           VGA_BASE_PIN, VGA_BASE_PIN + 7);
    return true;
}

#if HDMI_HSTX_TRACE
void vga_hstx_dump_config(void) {
    const uint32_t csr = hstx_ctrl_hw->csr;
    printf("vga_hstx: csr=%08x en=%u shift=%u n_shifts=%u fifo_stat=%08x\n",
           (unsigned)csr,
           (unsigned)((csr & HSTX_CTRL_CSR_EN_BITS) != 0),
           (unsigned)((csr & HSTX_CTRL_CSR_SHIFT_BITS)    >> HSTX_CTRL_CSR_SHIFT_LSB),
           (unsigned)((csr & HSTX_CTRL_CSR_N_SHIFTS_BITS) >> HSTX_CTRL_CSR_N_SHIFTS_LSB),
           (unsigned)hstx_fifo_hw->stat);
    for (int i = 0; i < 8; i++) {
        const uint32_t b = hstx_ctrl_hw->bit[i];
        const unsigned pin = (unsigned)(VGA_BASE_PIN + i);
        const unsigned fn = (unsigned)gpio_get_function(pin);
        printf("vga_hstx:   GP%-2u bit[%d]=%08x P=%u N=%u func=%u%s\n", pin, i, (unsigned)b,
               (unsigned)((b & HSTX_CTRL_BIT0_SEL_P_BITS) >> HSTX_CTRL_BIT0_SEL_P_LSB),
               (unsigned)((b & HSTX_CTRL_BIT0_SEL_N_BITS) >> HSTX_CTRL_BIT0_SEL_N_LSB),
               fn, fn == GPIO_FUNC_HSTX ? "" : "  <-- NOT HSTX");
    }
}
#else
void vga_hstx_dump_config(void) {}
#endif

#else
// Keeps the translation unit non-empty on every other build.
typedef int vga_hstx_not_built_t;
#endif // VGA_HSTX
