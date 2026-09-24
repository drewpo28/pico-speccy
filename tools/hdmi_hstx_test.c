// Host validation of the HSTX HDMI back-end against the PIO one that ships today.
// It includes the shipped header directly — the only copies of code here are the
// two ORACLES (the current get_ser_diff_data / hdmi_ser_one_arg bodies), which
// exist precisely so a refactor of them can be caught.
//
//   gcc -O2 -Wall -Wextra -Idrivers/hdmi -o /tmp/hdmi_hstx_test tools/hdmi_hstx_test.c
//   /tmp/hdmi_hstx_test
//   gcc -O2 -Wall -Wextra -DHDMI_HSTX=1 -Idrivers/hdmi -o /tmp/hdmi_hstx_test1 tools/hdmi_hstx_test.c
//   /tmp/hdmi_hstx_test1
//
// BOTH invocations matter: the second is the only thing that checks the palette
// LAYOUT the HSTX word size implies (16-byte slots holding two 32-bit words, and
// Data-Island characters no longer contiguous), which is a different question from
// the word arithmetic and is what HDMI_SLOT_IX / HDMI_CHAR_AT exist for.
//
// Three things are proved, per board:
//
//  1. REGRESSION — hdmi_word.h's PIO packers produce the byte-identical 64-bit word
//     the driver builds today, for every channel value. Until the HSTX backend is
//     switched on this is the whole safety of the refactor.
//  2. COMPOSITION — pack1(ch0) | pack1(ch1) | pack1(ch2) == pack3, which is what
//     the TERC4 look-up tables rely on (hdmi_build_terc_luts / hdmi_pack_blob).
//  3. EQUIVALENCE — the HSTX raw word driven through hstx_ctrl_hw->bit[] puts the
//     SAME bit stream on each of the 8 pins, over all 10 bit-times of a pixel, as
//     the PIO word driven through `out pins,6` + the 2-bit side-set clock. This is
//     the one that cannot be seen in a picture until a lane or an INV is wrong.
//
// Every assertion here was checked to FAIL under a hand-applied mutation of the
// code it covers (18 of them: the lane swap, the pair inversion, the clock
// polarity and its two halves, the DDR edge order, the autopull gap on both sides,
// the BGR field order, the channel shift, both pair-flip masks, the slot stride and
// the Data-Island character stride). A suite that cannot fail is worth nothing
// here: a wrong lane is a swapped colour on one board and invisible on another.
//
// The input space is covered exhaustively rather than sampled: the packers are
// separable (each channel contributes to disjoint bit fields), so sweeping every
// 10-bit value on one channel against fixed others, for each channel, leaves
// nothing untested. Real palette / control / TERC4 / guard-band symbols are run on
// top of that so a failure names something recognisable.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hdmi_word.h"
#include "tmds_pair.h"

static int failures = 0;
static long checks  = 0;

#define FAIL(fmt, ...) do { \
    if (failures < 10) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    failures++; } while (0)

// ---------------------------------------------------------------------------
// Oracles: the bodies that are in drivers/hdmi/hdmi.c today, with the three
// compile-time board choices (#ifdef PICO_PC, HDMI_PIN_invert_diffpairs,
// HDMI_PIN_RGB_notBGR) turned into arguments so one copy covers every board.
// ---------------------------------------------------------------------------
static uint64_t ref_get_ser_diff_data(int swap_rg, int invert, int rgb_notbgr,
                                      uint16_t dataR, uint16_t dataG, uint16_t dataB) {
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;
        uint8_t bR, bG;
        if (swap_rg) {
            bG = (dataR >> (9 - i)) & 1;
            bR = (dataG >> (9 - i)) & 1;
        } else {
            bR = (dataR >> (9 - i)) & 1;
            bG = (dataG >> (9 - i)) & 1;
        }
        uint8_t bB = (dataB >> (9 - i)) & 1;

        bR |= (bR ^ 1) << 1;
        bG |= (bG ^ 1) << 1;
        bB |= (bB ^ 1) << 1;

        if (invert) { bR ^= 0x3; bG ^= 0x3; bB ^= 0x3; }

        uint8_t d6;
        if (rgb_notbgr) d6 = (bR << 4) | (bG << 2) | (bB << 0);
        else            d6 = (bB << 4) | (bG << 2) | (bR << 0);
        out64 |= d6;
    }
    return out64;
}

static uint64_t ref_hdmi_ser_one_arg(int swap_rg, int invert, int rgb_notbgr,
                                     uint16_t data, int arg) {
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;
        uint8_t bit = (data >> (9 - i)) & 1;
        uint8_t b2 = bit | ((bit ^ 1) << 1);
        if (invert) b2 ^= 0x3;
        int wire;
        if (swap_rg) wire = (arg == 1) ? 1 : (arg == 2) ? 0 : 2;
        else         wire = (arg == 1) ? 0 : (arg == 2) ? 1 : 2;
        int shift;
        if (rgb_notbgr) shift = (wire == 0) ? 4 : (wire == 1) ? 2 : 0;
        else            shift = (wire == 2) ? 4 : (wire == 1) ? 2 : 0;
        out64 |= (uint64_t)(b2 << shift);
    }
    return out64;
}

// arg 1/2/3 of the oracle are ch2/ch1/ch0 of the header (see its call sites).
static int arg_of_ch(int ch) { return ch == 2 ? 1 : ch == 1 ? 2 : 3; }

// ---------------------------------------------------------------------------
// Boards. data_off / clk_off are derived from hdmi.h exactly as HDMI_PINMAP_INIT
// does: base+2 data, base+0 clock everywhere but ZERO2, which is base+0 data,
// base+6 clock, BGR and not inverted.
// ---------------------------------------------------------------------------
typedef struct { const char *name; hdmi_pinmap_t map; int hstx; const char *note; } board_t;
static const board_t boards[] = {
    { "PCp2  (PICO_PC)", { 2, 0, 1, 1, 1 }, 1, "GPIO 12-19, lanes 1/2 swapped" },
    { "m2p2  (MURM2)",   { 2, 0, 1, 1, 0 }, 1, "GPIO 12-19" },
    { "m1p2  (MURM1)",   { 2, 0, 1, 1, 0 }, 0, "GPIO 6-13  - no HSTX" },
    { "z0p2  (ZERO2)",   { 0, 6, 0, 0, 0 }, 0, "GPIO 32-39 - no HSTX, BGR, non-inverted" },
};
#define NBOARDS ((int)(sizeof boards / sizeof boards[0]))

// ---------------------------------------------------------------------------
// Symbols worth naming in a failure. Every 10-bit value is covered by the sweep
// below regardless; these make the output readable.
// ---------------------------------------------------------------------------
static const uint16_t ctrl[4]  = { 0x354, 0x0AB, 0x154, 0x2AB };   // CTL 00/01/10/11
static const uint16_t guard[2] = { 0x133, 0x2CC };                 // DI / video guard bands
static const uint16_t terc4[16] = {
    0x29C, 0x263, 0x2E4, 0x2E2, 0x171, 0x11E, 0x18E, 0x13C,
    0x2CC, 0x139, 0x19C, 0x2C4, 0x1C4, 0x1B1, 0x18C, 0x1E1,
};

static uint32_t rngstate = 0x1234567u;
static uint32_t rng(void) {
    rngstate ^= rngstate << 13; rngstate ^= rngstate >> 17; rngstate ^= rngstate << 5;
    return rngstate;
}

// ---------------------------------------------------------------------------
static void check_triple(const board_t *b, uint16_t ch2, uint16_t ch1, uint16_t ch0,
                         const char *what) {
    const hdmi_pinmap_t *m = &b->map;

    // 1. regression against the shipped body
    const uint64_t got = hdmi_pack3_pio(m, ch2, ch1, ch0);
    const uint64_t ref = ref_get_ser_diff_data(m->swap_rg, m->invert, m->rgb_notbgr, ch2, ch1, ch0);
    if (got != ref)
        FAIL("%s %s pack3_pio %016llx != shipped %016llx (ch2=%03x ch1=%03x ch0=%03x)",
             b->name, what, (unsigned long long)got, (unsigned long long)ref, ch2, ch1, ch0);

    // the two autopull halves must stay clean of each other
    if (got & 0xC0000000ull)
        FAIL("%s %s pack3_pio set the autopull gap bits (%016llx)", b->name, what,
             (unsigned long long)got);

    // 2. the OR composition the TERC4 tables depend on
    uint64_t or3 = 0;
    for (int ch = 0; ch < 3; ch++) or3 |= hdmi_pack1_pio(m, ch == 0 ? ch0 : ch == 1 ? ch1 : ch2, ch);
    if (or3 != got)
        FAIL("%s %s pack1_pio OR %016llx != pack3_pio %016llx", b->name, what,
             (unsigned long long)or3, (unsigned long long)got);

    // 3. pin-level equivalence, PIO vs HSTX
    hdmi_hstx_bit_t bits[8];
    hdmi_hstx_bits(m, bits);
    uint8_t pio[10], hstx[10];
    hdmi_pins_pio(m, got, pio);
    hdmi_pins_hstx(bits, hdmi_pack3_hstx(ch2, ch1, ch0), hstx);
    for (int t = 0; t < 10; t++)
        if (pio[t] != hstx[t])
            FAIL("%s %s bit-time %d: PIO pins %02x != HSTX pins %02x (ch2=%03x ch1=%03x ch0=%03x)",
                 b->name, what, t, pio[t], hstx[t], ch2, ch1, ch0);

    // 4. the legacy pairing's one-XOR twin. Both masks are checked in either build:
    //    each must be exactly "flip D0-7 and D9 of all three symbols" ON THE PINS,
    //    which is what hdmi_write_pair's HDMI_TMDS_BALANCED_PAIR==0 branch means by
    //    it. On the pins, because the PIO constant also touches the two autopull
    //    bits nothing ever shifts out.
    {
        const uint16_t f2 = ch2 ^ 0x2FF, f1 = ch1 ^ 0x2FF, f0 = ch0 ^ 0x2FF;
        uint8_t want[10], via_mask[10];
        hdmi_pins_pio(m, hdmi_pack3_pio(m, f2, f1, f0), want);
        hdmi_pins_pio(m, hdmi_pack3_pio(m, ch2, ch1, ch0) ^ HDMI_PAIR_FLIP_MASK_PIO, via_mask);
        if (memcmp(want, via_mask, sizeof want))
            FAIL("%s %s PIO pair-flip mask is not D0-7+D9 on the pins", b->name, what);
        hdmi_pins_hstx(bits, hdmi_pack3_hstx(f2, f1, f0), want);
        hdmi_pins_hstx(bits, hdmi_pack3_hstx(ch2, ch1, ch0) ^ HDMI_PAIR_FLIP_MASK_HSTX, via_mask);
        if (memcmp(want, via_mask, sizeof want))
            FAIL("%s %s HSTX pair-flip mask is not D0-7+D9 on the pins", b->name, what);
    }
    checks++;
}

static void check_one_channel(const board_t *b, uint16_t v, int ch) {
    const hdmi_pinmap_t *m = &b->map;
    const uint64_t got = hdmi_pack1_pio(m, v, ch);
    const uint64_t ref = ref_hdmi_ser_one_arg(m->swap_rg, m->invert, m->rgb_notbgr, v, arg_of_ch(ch));
    if (got != ref)
        FAIL("%s pack1_pio(ch%d, %03x) %016llx != shipped %016llx",
             b->name, ch, v, (unsigned long long)got, (unsigned long long)ref);
    if (hdmi_pack1_hstx(v, ch) != ((uint32_t)v << (ch * 10)))
        FAIL("%s pack1_hstx(ch%d, %03x) is not the plain field", b->name, ch, v);
    checks++;
}

// The palette slot is 16 bytes whatever the word is (the PIO address converter's
// (byte << 4)), so the index macros have to bend, not the layout. A Data-Island
// character is pixel c&1 of slot base+(c>>1) — contiguous only while two words
// fill a slot.
static void check_layout(void) {
    const size_t ws = sizeof(hdmi_word_t);
    printf("Palette layout for a %u-bit word:\n", (unsigned)(ws * 8));
    if (HDMI_SLOT_WORDS * ws != HDMI_SLOT_BYTES) FAIL("slot is not %d bytes", HDMI_SLOT_BYTES);
    for (int slot = 0; slot < 256; slot++)
        for (int px = 0; px < 2; px++) {
            const size_t off = (size_t)HDMI_SLOT_IX(slot, px) * ws;
            if (off != (size_t)slot * HDMI_SLOT_BYTES + (size_t)px * ws)
                FAIL("slot %d pixel %d lands at byte %u", slot, px, (unsigned)off);
        }
    for (int c = 0; c < 32; c++) {
        if (HDMI_CHAR_AT(HDMI_BLOB_SW, c) != c)
            FAIL("blob character %d is not packed", c);
        if (HDMI_CHAR_AT(HDMI_SLOT_WORDS, c) != HDMI_SLOT_IX(c >> 1, c & 1))
            FAIL("island character %d does not land on its slot pixel", c);
    }
    // The 256 palette slots must still fit the 4 KB page the converter can address.
    if ((size_t)HDMI_SLOT_IX(255, 1) * ws >= 4096) FAIL("the palette page overflows 4 KB");
    printf("  slot stride %d words / %d bytes, island characters %s\n",
           HDMI_SLOT_WORDS, HDMI_SLOT_BYTES,
           HDMI_SLOT_WORDS == 2 ? "contiguous" : "strided (two of every four words)");
    printf("  page A palette occupies %u of the 4096 addressable bytes\n\n",
           (unsigned)((size_t)HDMI_SLOT_IX(256, 0) * ws));
    checks++;
}

// A Data Island is 32 characters = 16 palette slots x 2 pixels, and the blob it
// comes from is PACKED (two words per slot, nothing wasted). Copying it into the
// palette table is therefore a STRIDED copy whenever a slot holds more than two
// words — which is exactly the HSTX case and the one piece of genuinely new logic
// in the driver. Get it wrong by copying flat and the island covers eight slots
// instead of sixteen, the rest stay stale, the BCH fails and the sink mutes with
// the picture still perfect. This checks the placement rule end to end, including
// that nothing outside the two used words of each slot is touched.
static void check_island_placement(void) {
    enum { BASE = 222, NSLOTS = 16, NCHARS = 32, PAGE = 4096 / (int)sizeof(hdmi_word_t) };
    static hdmi_word_t page[PAGE];
    static uint8_t written[PAGE];
    hdmi_word_t blob[NCHARS];
    for (int c = 0; c < NCHARS; c++) blob[c] = (hdmi_word_t)(0xA0000u + c);
    memset(page, 0, sizeof page);
    memset(written, 0, sizeof written);

    // The placement the driver uses: the blob is packed, the destination strided.
    for (int i = 0; i < NSLOTS; i++)
        for (int px = 0; px < 2; px++) {
            const int at = HDMI_SLOT_IX(BASE + i, px);
            if (at < 0 || at >= PAGE) { FAIL("island slot %d pixel %d is off the page", BASE + i, px); return; }
            page[at] = blob[HDMI_CHAR_AT(HDMI_BLOB_SW, i * 2 + px)];
            written[at] = 1;
        }

    // 1. every character landed on its own slot pixel, in order
    for (int c = 0; c < NCHARS; c++) {
        const int at = HDMI_SLOT_IX(BASE + (c >> 1), c & 1);
        if (page[at] != (hdmi_word_t)(0xA0000u + c))
            FAIL("island character %d is not at slot %d pixel %d", c, BASE + (c >> 1), c & 1);
    }
    // 2. the island touched 32 words and no others — a flat copy would write
    //    base..base+31, i.e. eight slots, and leave the other eight stale
    int touched = 0;
    for (int i = 0; i < PAGE; i++) touched += written[i];
    if (touched != NCHARS) FAIL("the island touched %d words, not %d", touched, NCHARS);
    for (int i = 0; i < NSLOTS; i++)
        for (int w = 2; w < HDMI_SLOT_WORDS; w++)
            if (written[(BASE + i) * HDMI_SLOT_WORDS + w])
                FAIL("the island wrote the unused tail of slot %d", BASE + i);
    // 3. the neighbours of the island are untouched (the palette lives there)
    for (int i = 0; i < PAGE; i++) {
        const int slot = i / HDMI_SLOT_WORDS;
        if ((slot < BASE || slot >= BASE + NSLOTS) && written[i])
            FAIL("the island spilled into slot %d", slot);
    }
    // 4. both Data-Island sets, at their real indices, fit the 4 KB page
    const int sets[2] = { 222, 184 };
    for (int s = 0; s < 2; s++)
        if (HDMI_SLOT_IX(sets[s] + NSLOTS - 1, 1) >= PAGE)
            FAIL("Data-Island set at %d runs off the addressable page", sets[s]);
    printf("Data Island: 32 characters -> slots %d..%d, %d words, nothing spilled\n\n",
           BASE, BASE + NSLOTS - 1, touched);
    checks++;
}

int main(void) {
    printf("HSTX vs PIO TMDS word equivalence\n");
    printf("=================================\n\n");

    check_layout();
    check_island_placement();

    for (int bi = 0; bi < NBOARDS; bi++) {
        const board_t *b = &boards[bi];
        const long f0 = failures;

        // --- exhaustive per channel (the packers are separable) ---------------
        static const uint16_t others[4] = { 0x000, 0x3FF, 0x155, 0x2AA };
        for (int ch = 0; ch < 3; ch++)
            for (uint16_t v = 0; v < 1024; v++) {
                check_one_channel(b, v, ch);
                for (int o = 0; o < 4; o++) {
                    const uint16_t x = others[o];
                    check_triple(b, ch == 2 ? v : x, ch == 1 ? v : x, ch == 0 ? v : x, "sweep");
                }
            }

        // --- the symbols the driver really emits ------------------------------
        for (int i = 0; i < 256; i++) {
            uint16_t a, c;
            tmds_balanced_pair((uint8_t)i, &a, &c);
            check_triple(b, a, a, a, "palette.first");
            check_triple(b, c, c, c, "palette.second");
        }
        for (int i = 0; i < 4; i++)
            check_triple(b, ctrl[0], ctrl[0], ctrl[i], "control");   // ch0 carries sync
        for (int i = 0; i < 16; i++)
            check_triple(b, terc4[i], terc4[(i + 5) & 15], terc4[(i + 11) & 15], "terc4");
        for (int i = 0; i < 2; i++)
            check_triple(b, guard[i], guard[i ^ 1], guard[i], "guard");

        // --- random triples ---------------------------------------------------
        for (int i = 0; i < 20000; i++) {
            const uint32_t r = rng();
            check_triple(b, (r >> 20) & 0x3FF, (r >> 10) & 0x3FF, r & 0x3FF, "random");
        }

        printf("  %-16s %-34s %s%s\n", b->name, b->note,
               failures == f0 ? "OK" : "FAILED",
               b->hstx ? "" : "  (PIO oracle only - HSTX unreachable there)");
    }

    // --- the property the whole port rests on ---------------------------------
    printf("\nThe HSTX word is board-independent; the pinout lives in bit[]:\n");
    {
        const hdmi_pinmap_t *pc = &boards[0].map, *m2 = &boards[1].map;
        int word_same = 1, pins_differ = 0;
        for (int i = 0; i < 4096; i++) {
            const uint32_t r = rng();
            const uint16_t c2 = (r >> 20) & 0x3FF, c1 = (r >> 10) & 0x3FF, c0 = r & 0x3FF;
            if (hdmi_pack3_hstx(c2, c1, c0) != hdmi_pack3_hstx(c2, c1, c0)) word_same = 0;
            if (hdmi_pack3_pio(pc, c2, c1, c0) != hdmi_pack3_pio(m2, c2, c1, c0)) pins_differ = 1;
        }
        if (!word_same)  FAIL("the HSTX word is not board-independent");
        if (!pins_differ) FAIL("PCp2 and m2p2 produce the same PIO word - the swap is not modelled");
        printf("  same raw word on both boards           : %s\n", word_same ? "yes" : "NO");
        printf("  different PIO word (lane swap is real) : %s\n", pins_differ ? "yes" : "NO");
    }

    // --- bit order, and the bit[] table step 2 has to program ------------------
    {
        const hdmi_pinmap_t *m = &boards[0].map;
        uint8_t pins[10];
        hdmi_pins_pio(m, hdmi_pack3_pio(m, 0, 0, 1), pins);   // ch0 = symbol bit 0 only
        const int lane0 = hdmi_lane_bit(m, 0);
        const int first_high = (pins[0] >> (lane0 + (m->invert ? 1 : 0))) & 1;
        printf("\nSymbol bit 0 goes out first (LSB first): %s\n", first_high ? "yes" : "NO");
        if (!first_high) FAIL("bit order is not LSB first");
    }

    for (int bi = 0; bi < 2; bi++) {
        const board_t *b = &boards[bi];
        hdmi_hstx_bit_t bits[8];
        hdmi_hstx_bits(&b->map, bits);
        printf("\nhstx_ctrl_hw->bit[] for %s (index = GPIO - %s):\n", b->name,
               b->map.data_off == 2 ? "12" : "base");
        for (int i = 0; i < 8; i++) {
            if (bits[i].clk)
                printf("  bit[%d] = CLK%s\n", i, bits[i].inv ? " | INV" : "");
            else if (bits[i].used)
                printf("  bit[%d] = SEL_P %2d | SEL_N %2d%s   (lane %d)\n", i,
                       bits[i].sel_p, bits[i].sel_n, bits[i].inv ? " | INV" : "        ",
                       bits[i].sel_p / 10);
            else
                printf("  bit[%d] = unused\n", i);
        }
    }

    printf("\n%ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
