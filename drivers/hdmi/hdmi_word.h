#pragma once
// TMDS word packing for the two HDMI back-ends — the PIO one that ships today and
// the HSTX one — plus the board pin/lane map both of them are expressed against.
//
// It sits in its own header for the same reason tmds_pair.h does: so the host
// validator builds against the SHIPPED code rather than a copy of it.
//
//   gcc -O2 -Wall -Idrivers/hdmi -o /tmp/hdmi_hstx_test tools/hdmi_hstx_test.c
//   /tmp/hdmi_hstx_test
//
// Re-run that after ANY change here. What it proves is not visible in a picture:
// that the HSTX raw word plus its bit[] pad map drives every one of the 8 pins with
// the SAME bit stream the PIO word plus `out pins,6` + side-set drives today. A
// wrong lane row or a wrong INV is a swapped colour or a dead channel on one board
// and invisible on another (see hdmi_pinmap_t::swap_rg).
//
// Depends on <stdint.h> only — no SDK, no hdmi.h. The firmware builds its own map
// from hdmi.h's macros through HDMI_PINMAP_INIT; the test uses literals.
#include <stdint.h>

// ---------------------------------------------------------------------------
// Channel numbering used throughout: ch0 = blue, and it is the one that carries
// the H/V sync levels in the control periods; ch1 = green; ch2 = red. That is the
// TMDS/DVI convention, and it is NOT the argument order of the driver's historical
// get_ser_diff_data(R, G, B) — which is (ch2, ch1, ch0).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Which back-end this build drives, and therefore what one output pixel is in the
// palette table: a 64-bit differential word for the PIO program, or a 32-bit raw
// word for HSTX. Everything above the wire — the ISR, the palette semantics, the
// Data Islands, DS80, the CRT grille — is written against hdmi_word_t and does not
// care which.
//
// The SLOT is 16 bytes either way and that is not ours to choose: the PIO address
// converter reconstructs the read address as (page << 12) | (byte << 4), so a
// palette index owns 16 bytes whatever fits in them. Two 64-bit words fill it; two
// 32-bit words use the first half and leave the tail unread (the DMA fetches two
// words per index, not the whole slot). Index a slot through HDMI_SLOT_IX so the
// two cases cannot be confused — `slot * 2 + px` is only right for one of them.
// ---------------------------------------------------------------------------
#ifndef HDMI_HSTX
#define HDMI_HSTX 0
#endif
// HDMI_HSTX is 0 (PIO), 1 (HSTX raw: our symbols, the index stream, the PIO address
// converter) or 2 (HSTX with the command expander and the hardware TMDS encoder:
// the line is a command list, pixels are XRGB8888 colours, no converter). Both HSTX
// forms share the 32-bit raw word for the control and Data-Island symbols; only the
// second stops using it for PIXELS — see hdmi_tmds_line.h.
#define HDMI_EXPANDER (HDMI_HSTX >= 2)

// Flip D0-7 and D9 of all three symbols with a single XOR over the whole word —
// hdmi_write_pair's HDMI_TMDS_BALANCED_PAIR==0 pairing, the A/B escape hatch.
// The PIO constant is the shipped one and also sets word bits 30 and 31, which are
// the autopull gap: the program pulls 30 bits at a time, so those two never reach
// a pin. Left exactly as it is, and the host test compares the two back-ends on
// the PINS for that reason.
#define HDMI_PAIR_FLIP_MASK_PIO  0x3F03FFFFFFFFFFFFull
#define HDMI_PAIR_FLIP_MASK_HSTX 0x2FFBFEFFu

#if HDMI_HSTX
typedef uint32_t hdmi_word_t;
#define HDMI_SLOT_WORDS 4
#define HDMI_PAIR_FLIP_MASK HDMI_PAIR_FLIP_MASK_HSTX
#else
typedef uint64_t hdmi_word_t;
#define HDMI_SLOT_WORDS 2
#define HDMI_PAIR_FLIP_MASK HDMI_PAIR_FLIP_MASK_PIO
#endif

#define HDMI_SLOT_BYTES 16
_Static_assert(HDMI_SLOT_WORDS * sizeof(hdmi_word_t) == HDMI_SLOT_BYTES,
               "the PIO converter's (byte << 4) fixes the palette slot at 16 bytes");

// 32-bit DMA transfers per palette index: the two output pixels, whatever a pixel
// is. The pal_conv channel's transfer count, and the one number that has to move
// with the word size.
#define HDMI_DMA_WORDS_PER_INDEX (2 * (int)sizeof(hdmi_word_t) / 4)

// Word index of pixel `px` (0 = left, 1 = right) of palette slot `slot`.
#define HDMI_SLOT_IX(slot, px) ((slot) * HDMI_SLOT_WORDS + (px))

// Word index of Data-Island character `c` (0..31 = 16 slots x 2 pixels) in a
// destination whose slot stride is `sw` words: HDMI_SLOT_WORDS in the palette
// table, HDMI_BLOB_SW in a standalone blob, which stays packed.
#define HDMI_BLOB_SW 2
#define HDMI_CHAR_AT(sw, c) (((c) >> 1) * (sw) + ((c) & 1))

typedef struct {
    uint8_t data_off;    // first data pin, as an offset from HDMI_BASE_PIN
    uint8_t clk_off;     // first clock pin, same
    uint8_t rgb_notbgr;  // field order inside the PIO's 6-bit group
    uint8_t invert;      // differential pairs are wired N/P instead of P/N
    uint8_t swap_rg;     // the board swaps ch1 and ch2 onto each other's pin pair
} hdmi_pinmap_t;

// The board this firmware was compiled for. Expands hdmi.h's macros, so there is
// one definition of the pinout and this header does not repeat it.
#ifdef PICO_PC
#define HDMI_PINMAP_SWAP_RG 1
#else
#define HDMI_PINMAP_SWAP_RG 0
#endif
#define HDMI_PINMAP_INIT {                                  \
    (uint8_t)(beginHDMI_PIN_data - HDMI_BASE_PIN),          \
    (uint8_t)(beginHDMI_PIN_clk  - HDMI_BASE_PIN),          \
    (uint8_t)(HDMI_PIN_RGB_notBGR),                         \
    (uint8_t)(HDMI_PIN_invert_diffpairs),                   \
    (uint8_t)(HDMI_PINMAP_SWAP_RG) }

// Where a channel's pin PAIR sits inside the PIO's 6-bit output group.
//
// The group is (R<<4)|(G<<2)|(B<<0), or the reverse when the board is BGR, and a
// board may additionally hand ch2 to the G position and ch1 to the R position —
// that is PICO_PC, whose HDMI connector wires the two data pairs the other way
// round. Both halves used to live as #ifdefs inside get_ser_diff_data() and
// hdmi_ser_one_arg(), which is why the swap had to be written out twice.
static inline int hdmi_d6_shift(const hdmi_pinmap_t *m, const int ch) {
    const int r = m->rgb_notbgr ? 4 : 0;
    const int g = 2;
    const int b = m->rgb_notbgr ? 0 : 4;
    if (ch == 0) return b;
    if (m->swap_rg) return (ch == 1) ? r : g;
    return (ch == 1) ? g : r;
}

// HSTX bit[] index (= GPIO - HDMI_BASE_PIN) of a channel's pin pair. The pair's
// FIRST pin; its partner is +1 and carries the same bits with INV flipped.
static inline int hdmi_lane_bit(const hdmi_pinmap_t *m, const int ch) {
    return m->data_off + hdmi_d6_shift(m, ch);
}

// ---------------------------------------------------------------------------
// PIO back-end: one 64-bit word per output pixel.
//
// Consumed by the 10-instruction program (`out pins, 6` x10 with the clock pair as
// a 2-bit side-set) at 30-bit autopull, so the word is two 30-bit halves with a
// 2-bit gap at bits 30..31. The OSR shifts right, i.e. the LOW 6 bits go out first,
// and the loop below fills from the top — so the FIRST thing on the wire is the
// last iteration, symbol bit 0. LSB first, which is what TMDS wants.
// ---------------------------------------------------------------------------
static inline uint64_t hdmi_pack3_pio(const hdmi_pinmap_t *m,
                                      const uint16_t ch2, const uint16_t ch1, const uint16_t ch0) {
    const uint16_t sym[3] = { ch0, ch1, ch2 };
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;   // the gap between the two autopull halves
        uint8_t d6 = 0;
        for (int ch = 0; ch < 3; ch++) {
            uint8_t b = (uint8_t)((sym[ch] >> (9 - i)) & 1);
            uint8_t pair = (uint8_t)(b | ((b ^ 1) << 1));   // true + complement
            if (m->invert) pair ^= 0x3;
            d6 |= (uint8_t)(pair << hdmi_d6_shift(m, ch));
        }
        out64 |= d6;
    }
    return out64;
}

// One channel's contribution to the same word, so three ORs make a whole one. The
// TERC4 look-up tables are built this way: 16 entries per channel instead of 4096
// combinations (see hdmi_build_terc_luts).
static inline uint64_t hdmi_pack1_pio(const hdmi_pinmap_t *m, const uint16_t data, const int ch) {
    const int shift = hdmi_d6_shift(m, ch);
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;
        uint8_t b = (uint8_t)((data >> (9 - i)) & 1);
        uint8_t pair = (uint8_t)(b | ((b ^ 1) << 1));
        if (m->invert) pair ^= 0x3;
        out64 |= (uint64_t)((uint8_t)(pair << shift));
    }
    return out64;
}

// ---------------------------------------------------------------------------
// HSTX back-end: one 32-bit word per output pixel, three 10-bit symbols.
//
// BOARD-INDEPENDENT, and that is the point of the port: the pair order and the
// P/N inversion move out of the word and into hstx_ctrl_hw->bit[], so the palette
// LUT a board holds no longer depends on how its connector is wired.
// ---------------------------------------------------------------------------
#define HDMI_HSTX_SHIFT     2   // bits shifted out of the register per clk_hstx cycle
#define HDMI_HSTX_N_SHIFTS  5   // ...cycles before the next FIFO word is popped
#define HDMI_HSTX_CLKDIV    5   // generated-clock period, in clk_hstx cycles = one pixel

// CLKPHASE counts HALF clk_hstx periods, and "0 means the clock is initially LOW,
// the first rising edge after CLKDIV/2 cycles" (datasheet). The PIO program starts
// a pixel with the clock pair's second pin HIGH (side-set 2 on the first five
// instructions), so to put the same waveform on the same wire the generated clock
// has to start HIGH — half a generated period early, which is CLKDIV half-cycles.
// Setting this to 0 inverts the clock pair against everything that ships today;
// the host test follows the constant, so an A/B still has to agree with the PIO.
#define HDMI_HSTX_CLKPHASE  HDMI_HSTX_CLKDIV

// Bit-times per pixel, and how many of them the generated clock is high for.
#define HDMI_HSTX_BITS_PER_PIXEL (HDMI_HSTX_CLKDIV * HDMI_HSTX_SHIFT)
#define HDMI_HSTX_CLK_HALF       (HDMI_HSTX_BITS_PER_PIXEL / 2)

static inline int hdmi_hstx_clk_level(const int t) {
#if HDMI_HSTX_CLKPHASE == HDMI_HSTX_CLKDIV
    return t < HDMI_HSTX_CLK_HALF;    // starts high, like the PIO's side-set 2
#else
    return t >= HDMI_HSTX_CLK_HALF;   // CLKPHASE 0: starts low
#endif
}

// Same regime note as the VGA path: the shift register right-ROTATES, and only
// while SHIFT * N_SHIFTS stays inside 32 bits is that the same as a shift — which
// is what the pin model below assumes. 2 x 5 = 10, comfortably inside.
_Static_assert(HDMI_HSTX_SHIFT * HDMI_HSTX_N_SHIFTS <= 32,
               "the rotate would wrap: symbols would repeat instead of advancing");

static inline uint32_t hdmi_pack3_hstx(const uint16_t ch2, const uint16_t ch1, const uint16_t ch0) {
    return ((uint32_t)(ch0 & 0x3FF))
         | ((uint32_t)(ch1 & 0x3FF) << 10)
         | ((uint32_t)(ch2 & 0x3FF) << 20);
}

static inline uint32_t hdmi_pack1_hstx(const uint16_t data, const int ch) {
    return (uint32_t)(data & 0x3FF) << (ch * 10);
}

// The word this build actually stores. The board pin map is unused by the HSTX
// form on purpose — that is the property the host test checks.
static inline hdmi_word_t hdmi_pack3(const hdmi_pinmap_t *m,
                                     const uint16_t ch2, const uint16_t ch1, const uint16_t ch0) {
#if HDMI_HSTX
    (void)m;
    return hdmi_pack3_hstx(ch2, ch1, ch0);
#else
    return hdmi_pack3_pio(m, ch2, ch1, ch0);
#endif
}

static inline hdmi_word_t hdmi_pack1(const hdmi_pinmap_t *m, const uint16_t data, const int ch) {
#if HDMI_HSTX
    (void)m;
    return hdmi_pack1_hstx(data, ch);
#else
    return hdmi_pack1_pio(m, data, ch);
#endif
}

// The pad map the backend programs. sel_p / sel_n are bit positions in the shift
// register, taken on the two halves of a clk_hstx cycle (DDR); inv is the pin's
// own inversion; clk replaces the data path with the generated output clock.
typedef struct { uint8_t used, clk, inv, sel_p, sel_n; } hdmi_hstx_bit_t;

static inline void hdmi_hstx_bits(const hdmi_pinmap_t *m, hdmi_hstx_bit_t bits[8]) {
    for (int i = 0; i < 8; i++) { bits[i].used = 0; bits[i].clk = 0; bits[i].inv = 0;
                                  bits[i].sel_p = 0; bits[i].sel_n = 0; }
    // Clock pair. The PIO program side-sets 2 (0b10) for the first five bit-times
    // and 1 (0b01) for the last five, i.e. the pair's SECOND pin is the one that is
    // high first — so it takes the clock as generated and its partner inverts it.
    // Board-independent: the side-set values are the program's, not the board's.
    bits[m->clk_off + 1].used = 1; bits[m->clk_off + 1].clk = 1;
    bits[m->clk_off + 0].used = 1; bits[m->clk_off + 0].clk = 1; bits[m->clk_off + 0].inv = 1;
    for (int ch = 0; ch < 3; ch++) {
        const int off = hdmi_lane_bit(m, ch);
        for (int half = 0; half < 2; half++) {
            hdmi_hstx_bit_t *b = &bits[off + half];
            b->used  = 1;
            b->sel_p = (uint8_t)(ch * 10);
            b->sel_n = (uint8_t)(ch * 10 + 1);
            // Without board inversion the pair's first pin carries the true level,
            // exactly as the PIO's `b | (~b << 1)` puts it in the group's low bit.
            b->inv   = (uint8_t)(half == 0 ? m->invert : !m->invert);
        }
    }
}

// ---------------------------------------------------------------------------
// Pin-level models of both back-ends — what each of the 8 pins of the block does
// over the 10 bit-times of one output pixel. out[t] is a mask, bit i = the pin at
// GPIO (HDMI_BASE_PIN + i). Used by the host test; the firmware never calls these.
// ---------------------------------------------------------------------------
static inline void hdmi_pins_pio(const hdmi_pinmap_t *m, const uint64_t w, uint8_t out[10]) {
    for (int t = 0; t < 10; t++) {
        const int shift = t * 6 + (t >= 5 ? 2 : 0);   // mind the autopull gap
        const uint8_t d6 = (uint8_t)((w >> shift) & 0x3F);
        uint8_t pins = (uint8_t)(d6 << m->data_off);
        pins |= (uint8_t)((t < 5 ? 2u : 1u) << m->clk_off);   // side-set 2 then 1
        out[t] = pins;
    }
}

static inline void hdmi_pins_hstx(const hdmi_hstx_bit_t bits[8], const uint32_t w, uint8_t out[10]) {
    for (int t = 0; t < 10; t++) {
        const int cycle = t / HDMI_HSTX_SHIFT;     // the register shifts by 2 per cycle
        const int half  = t % HDMI_HSTX_SHIFT;     // ...and each cycle is DDR
        const uint32_t reg = w >> (HDMI_HSTX_SHIFT * cycle);
        uint8_t pins = 0;
        for (int i = 0; i < 8; i++) {
            if (!bits[i].used) continue;
            int level;
            if (bits[i].clk) {
                level = hdmi_hstx_clk_level(t);
            } else {
                const int sel = half ? bits[i].sel_n : bits[i].sel_p;
                level = (int)((reg >> sel) & 1u);
            }
            if (bits[i].inv) level ^= 1;
            pins |= (uint8_t)(level << i);
        }
        out[t] = pins;
    }
}
