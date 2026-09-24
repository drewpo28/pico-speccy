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
    if (csr & HSTX_CTRL_CSR_EXPAND_EN_BITS)
        printf("hdmi_hstx: expand_tmds=%08x expand_shift=%08x\n",
               (unsigned)hstx_ctrl_hw->expand_tmds, (unsigned)hstx_ctrl_hw->expand_shift);
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

uint32_t hdmi_hstx_div_at(unsigned tmds_mhz, uint32_t sys_hz) {
    return hstx_div_for(tmds_mhz, sys_hz);
}

uint32_t hdmi_hstx_pixel_hz(unsigned tmds_mhz) {
    const uint32_t sys = (uint32_t)clock_get_hz(clk_sys);
    return (sys / hstx_div_for(tmds_mhz, sys)) / HDMI_HSTX_CLKDIV;
}

void hdmi_hstx_stop(void) {
    hstx_ctrl_hw->csr = 0;
}

bool hdmi_hstx_start(unsigned tmds_mhz, bool expander) {
    if (!tmds_mhz) tmds_mhz = 252;

    // The 37.8 MHz "fast" modes ask for clk_hstx 189 MHz.  That is above the
    // datasheet's 150 but well inside what the part does — see the note below.
    const uint32_t want = hstx_want_hz(tmds_mhz);
    const uint32_t sys  = (uint32_t)clock_get_hz(clk_sys);
    const uint32_t div  = hstx_div_for(tmds_mhz, sys);
    const bool exact = (div * want == sys);
    if (!exact) {
        printf("hdmi_hstx: clk_sys %u cannot give %u Hz exactly - using /%u (%u Hz)\n",
               (unsigned)sys, (unsigned)want, (unsigned)div, (unsigned)(sys / div));
    }
    // 150 MHz is the figure the datasheet quotes, NOT where the silicon stops:
    // debug/HSTX drives its 720p modes at a 74.25 MHz pixel, i.e. clk_hstx 371.25
    // MHz and 742 Mbps per pin.  So this is a note, not a refusal — the 37.8 MHz
    // "fast" modes want 189 MHz and are offered again because of it.
    if (want > 150000000u) {
        printf("hdmi_hstx: clk_hstx %u Hz is above the datasheet's 150 MHz "
               "(%u Mbps per pin) - known to run, watch for artefacts\n",
               (unsigned)want, (unsigned)(want / 500000u));
    }
    if (div > HSTX_DIV_MAX) {
        printf("hdmi_hstx: clk_sys/%u is past the 2-bit divider (max %u) - clamping, "
               "the video clock will be WRONG\n", (unsigned)div, HSTX_DIV_MAX);
    }
    // FACT, not a diagnosis: an odd divider does not give a 50% duty cycle and
    // clk_hstx has no DC50 bit to correct it (only the four GPOUT generators have
    // one - see clocks.h), and the serializer is DDR, so the two half-bits of a
    // cycle are then unequal.  Whether that is what breaks clk_sys 378 on m2p2
    // (hw 2026-09-23: 378 = /3 artefacts, 504 = /4 clean) is NOT established - the
    // line ISR's own budget is the other candidate.  Logged so a capture says
    // which divider was in force.
    if (div & 1u) {
        printf("hdmi_hstx: clk_sys/%u is an ODD divider - clk_hstx duty is not 50%% "
               "(no DC50 on this generator), so the DDR half-bits are unequal\n",
               (unsigned)div);
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

    // The command expander, when asked for: TMDS-mode words are one XRGB8888 pixel
    // each — lane 0 (blue) takes bits 7..0, lane 1 (green) 15..8, lane 2 (red)
    // 23..16, eight bits per lane (NBITS is bits-minus-one; ROT is the right-rotate
    // that brings the lane's byte down to 7..0). One shift per encoded word and one
    // per raw word, i.e. one output pixel per FIFO word in either mode. Identical to
    // quakegeneric's DVI_HSTX_MODE_XRGB8888 with pixel repetition 1.
    if (expander) {
        hstx_ctrl_hw->expand_tmds =
              (7u  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB) | (16u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB)
            | (7u  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB) | (8u  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB)
            | (7u  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB) | (0u  << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB);
        hstx_ctrl_hw->expand_shift =
              (1u << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB) | (0u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB)
            | (1u << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB) | (0u << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB);
    }

    // Serial engine: ten bits per lane shifted two at a time, N_SHIFTS pops the next
    // (expanded or raw) word after five cycles; CLKDIV/CLKPHASE put one clock period
    // on the pair per pixel, starting high to match the PIO program's side-set (see
    // HDMI_HSTX_CLKPHASE). Without the expander every FIFO word is one raw 30-bit
    // pixel — our own TMDS symbols, the index stream and the PIO address converter.
    hstx_ctrl_hw->csr =
          ((uint32_t)HDMI_HSTX_CLKDIV   << HSTX_CTRL_CSR_CLKDIV_LSB)
        | ((uint32_t)HDMI_HSTX_CLKPHASE << HSTX_CTRL_CSR_CLKPHASE_LSB)
        | ((uint32_t)HDMI_HSTX_N_SHIFTS << HSTX_CTRL_CSR_N_SHIFTS_LSB)
        | ((uint32_t)HDMI_HSTX_SHIFT    << HSTX_CTRL_CSR_SHIFT_LSB)
        | (expander ? HSTX_CTRL_CSR_EXPAND_EN_BITS : 0u)
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
    printf("hdmi_hstx: %s, TMDS %u MHz, clk_hstx %u Hz (clk_sys/%u), pixel %u Hz, pins %d-%d%s\n",
           expander ? "command expander + TMDS encoder" : "raw words",
           tmds_mhz, (unsigned)clock_get_hz(clk_hstx), (unsigned)div,
           (unsigned)live, HDMI_BASE_PIN, HDMI_BASE_PIN + 7,
           (live == want_px) ? "" : "  <-- DISAGREES with the audio path's arithmetic");
    return exact;
}

#else
// Keeps the translation unit non-empty on every other build.
typedef int hdmi_hstx_not_built_t;
#endif // HDMI_HSTX
