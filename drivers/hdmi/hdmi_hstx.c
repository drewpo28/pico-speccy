#include "hdmi_hstx.h"

#if HDMI_HSTX

#include <stdio.h>
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hdmi.h"
#include "hdmi_word.h"

// This board's pin/lane map, from hdmi.h's macros — the same one hdmi.c packs
// words against, and the same one tools/hdmi_hstx_test.c proves equivalent to the
// PIO path pin by pin.
static const hdmi_pinmap_t hstx_pins = HDMI_PINMAP_INIT;

#if HDMI_HSTX_TRACE
uint32_t hdmi_hstx_fifo_stat(void) {
    return hstx_fifo_hw->stat;
}

void hdmi_hstx_dump_config(void) {
    const uint32_t csr = hstx_ctrl_hw->csr;
    printf("hdmi_hstx: csr=%08x en=%u shift=%u n_shifts=%u clkdiv=%u clkphase=%u expand=%u\n",
           (unsigned)csr,
           (unsigned)((csr & HSTX_CTRL_CSR_EN_BITS) != 0),
           (unsigned)((csr & HSTX_CTRL_CSR_SHIFT_BITS)    >> HSTX_CTRL_CSR_SHIFT_LSB),
           (unsigned)((csr & HSTX_CTRL_CSR_N_SHIFTS_BITS) >> HSTX_CTRL_CSR_N_SHIFTS_LSB),
           (unsigned)((csr & HSTX_CTRL_CSR_CLKDIV_BITS)   >> HSTX_CTRL_CSR_CLKDIV_LSB),
           (unsigned)((csr & HSTX_CTRL_CSR_CLKPHASE_BITS) >> HSTX_CTRL_CSR_CLKPHASE_LSB),
           (unsigned)((csr & HSTX_CTRL_CSR_EXPAND_EN_BITS) != 0));
    for (int i = 0; i < 8; i++) {
        const uint32_t b = hstx_ctrl_hw->bit[i];
        const uint pin = (uint)(HDMI_BASE_PIN + i);
        const unsigned fn = (unsigned)gpio_get_function(pin);
        if (b & HSTX_CTRL_BIT0_CLK_BITS) {
            printf("hdmi_hstx:   GP%-2u bit[%d]=%08x CLK%s        func=%u%s\n",
                   pin, i, (unsigned)b, (b & HSTX_CTRL_BIT0_INV_BITS) ? "|INV" : "    ",
                   fn, fn == GPIO_FUNC_HSTX ? "" : "  <-- NOT HSTX");
        } else {
            const unsigned sp = (unsigned)((b & HSTX_CTRL_BIT0_SEL_P_BITS) >> HSTX_CTRL_BIT0_SEL_P_LSB);
            const unsigned sn = (unsigned)((b & HSTX_CTRL_BIT0_SEL_N_BITS) >> HSTX_CTRL_BIT0_SEL_N_LSB);
            printf("hdmi_hstx:   GP%-2u bit[%d]=%08x lane%u %s%s func=%u%s\n",
                   pin, i, (unsigned)b, sp / 10,
                   (b & HSTX_CTRL_BIT0_INV_BITS) ? "N" : "P",
                   (sn == sp + 1) ? "    " : " ??? ", fn,
                   fn == GPIO_FUNC_HSTX ? "" : "  <-- NOT HSTX");
        }
    }
}
#endif // HDMI_HSTX_TRACE

volatile void *hdmi_hstx_fifo(void) {
    return (volatile void *)&hstx_fifo_hw->fifo;
}

// Two bits leave every pin per clk_hstx cycle (the shift register advances by
// HDMI_HSTX_SHIFT and each cycle is DDR), so the serial clock is half the TMDS bit
// rate: 126 MHz for the 25.2 MHz pixel every standard mode uses.
static uint32_t hstx_want_hz(unsigned tmds_mhz) {
    return (uint32_t)(tmds_mhz ? tmds_mhz : 252) * (1000000u / HDMI_HSTX_SHIFT);
}

// clk_hstx's divider is NOT the 16.16 int+frac every other clock in the block has:
// CLOCKS_CLK_HSTX_DIV_INT is TWO BITS (0x00030000) and there is no FRAC field at
// all, so the only dividers that exist are 1, 2, 3 and 4 — the datasheet's "0 ->
// max+1" is what makes 4 reachable, and it is why writing a plain 4 works: the 2-bit
// field takes the low bits, 4 lands as 0, and the hardware reads that back as 4.
// Do not "fix" that into a mask-and-clamp.
//
// It is also why the 25.2 MHz pixel clock is so comfortable here (126 MHz from
// 252/2, 378/3, 504/4) and why the VGA path cannot use the serializer at all: its
// 19.96 and 27 MHz pixel clocks are not clk_sys/{1..4}/N for any N (see the plan
// doc). The PIO has an 8-bit fractional divider and absorbs any ratio; HSTX does not.
#define HSTX_DIV_MAX 4
static uint32_t hstx_div_for(unsigned tmds_mhz, uint32_t sys) {
    const uint32_t want = hstx_want_hz(tmds_mhz);
    uint32_t div = (sys + want / 2) / want;
    if (div == 0) div = 1;
    return div;
}

uint32_t hdmi_hstx_pixel_hz(unsigned tmds_mhz) {
    const uint32_t sys = (uint32_t)clock_get_hz(clk_sys);
    return (sys / hstx_div_for(tmds_mhz, sys)) / HDMI_HSTX_CLKDIV;
}

void hdmi_hstx_stop(void) {
    hstx_ctrl_hw->csr = 0;
}

bool hdmi_hstx_start(unsigned tmds_mhz) {
    if (!tmds_mhz) tmds_mhz = 252;

    // The datasheet's ceiling is 150 MHz / 300 Mbps per pin — the 37.8 MHz "fast"
    // modes ask for 189 MHz and are out of spec, which is why they are gated off.
    const uint32_t want = hstx_want_hz(tmds_mhz);
    const uint32_t sys  = (uint32_t)clock_get_hz(clk_sys);
    const uint32_t div  = hstx_div_for(tmds_mhz, sys);
    const bool exact = (div * want == sys);
    if (!exact) {
        printf("hdmi_hstx: clk_sys %u cannot give %u Hz exactly - using /%u (%u Hz)\n",
               (unsigned)sys, (unsigned)want, (unsigned)div, (unsigned)(sys / div));
    }
    if (want > 150000000u) {
        printf("hdmi_hstx: %u MHz TMDS wants clk_hstx %u Hz, past the 150 MHz rating\n",
               tmds_mhz, (unsigned)want);
    }
    if (div > HSTX_DIV_MAX) {
        printf("hdmi_hstx: clk_sys/%u is past the 2-bit divider (max %u) - clamping, "
               "the video clock will be WRONG\n", (unsigned)div, HSTX_DIV_MAX);
    }

    hdmi_hstx_stop();
    // Integer divider on purpose: a fractional one jitters the video clock, and
    // 126 MHz divides 252 / 378 / 504 exactly, i.e. every CPU clock the Overclock
    // menu offers.
    clock_configure_int_divider(clk_hstx, 0,
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                                sys, div);

    // Per-pin lane map. Built by the shipped helper rather than written out here,
    // so the table the host test checks IS the table the hardware gets.
    hdmi_hstx_bit_t bits[8];
    hdmi_hstx_bits(&hstx_pins, bits);
    for (int i = 0; i < 8; i++) {
        uint32_t v = 0;
        if (bits[i].clk)       v |= HSTX_CTRL_BIT0_CLK_BITS;
        else if (bits[i].used) v |= ((uint32_t)bits[i].sel_p << HSTX_CTRL_BIT0_SEL_P_LSB)
                                  | ((uint32_t)bits[i].sel_n << HSTX_CTRL_BIT0_SEL_N_LSB);
        if (bits[i].inv)       v |= HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[i] = v;
    }

    // Raw mode: no command expander, one FIFO word per output pixel, ten bits per
    // lane shifted two at a time. N_SHIFTS pops the next word after five cycles;
    // CLKDIV/CLKPHASE put one clock period on the pair per pixel, starting high to
    // match the PIO program's side-set (see HDMI_HSTX_CLKPHASE).
    hstx_ctrl_hw->csr =
          ((uint32_t)HDMI_HSTX_CLKDIV   << HSTX_CTRL_CSR_CLKDIV_LSB)
        | ((uint32_t)HDMI_HSTX_CLKPHASE << HSTX_CTRL_CSR_CLKPHASE_LSB)
        | ((uint32_t)HDMI_HSTX_N_SHIFTS << HSTX_CTRL_CSR_N_SHIFTS_LSB)
        | ((uint32_t)HDMI_HSTX_SHIFT    << HSTX_CTRL_CSR_SHIFT_LSB)
        | HSTX_CTRL_CSR_EN_BITS;

    // Pads. The data pairs take the same 12 mA / fast slew the PIO path gives them;
    // the clock pair's drive is the user's Video > HDMI > Clock drive setting and is
    // applied by hdmi.c (pad registers only, independent of the function select).
    for (int i = 0; i < 8; i++) {
        const uint pin = (uint)(HDMI_BASE_PIN + i);
        gpio_set_function(pin, GPIO_FUNC_HSTX);
        if (i == (int)hstx_pins.clk_off || i == (int)hstx_pins.clk_off + 1) continue;
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }

    // The live clock beside the arithmetic one: the audio path uses the arithmetic
    // form (it runs before this does), so the two must agree or the ACR is wrong.
    const uint32_t live = (uint32_t)clock_get_hz(clk_hstx) / HDMI_HSTX_CLKDIV;
    const uint32_t want_px = hdmi_hstx_pixel_hz(tmds_mhz);
    printf("hdmi_hstx: TMDS %u MHz, clk_hstx %u Hz (clk_sys/%u), pixel %u Hz, pins %d-%d%s\n",
           tmds_mhz, (unsigned)clock_get_hz(clk_hstx), (unsigned)div,
           (unsigned)live, HDMI_BASE_PIN, HDMI_BASE_PIN + 7,
           (live == want_px) ? "" : "  <-- DISAGREES with the audio path's arithmetic");
    return exact;
}

#else
// Keeps the translation unit non-empty on every other build.
typedef int hdmi_hstx_not_built_t;
#endif // HDMI_HSTX
