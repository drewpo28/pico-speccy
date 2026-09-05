/*

pico-speccy — YM2413 (OPLL) FM synthesis. See OpllFm.h for provenance: this
is a port of MAME's ym2413.cpp (Jarek Burczynski, GPL-2.0+, tag mame0220),
kept register-for-register faithful, with the same platform adaptations as
OplFm (heap tables, freqbase rate conversion, EG-skip, half-rate option).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include "OpllFm.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

// All in flash, like the other FM cores (see OplFm.cpp for the history of the
// RAM-resident period and why it ended).

OpllFm* opllfm = nullptr;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── ym2413.cpp constants ────────────────────────────────────────────────────
#define FREQ_SH         16
#define EG_SH           16
#define LFO_SH          24
#define FREQ_MASK       ((1 << FREQ_SH) - 1)

#define ENV_BITS        10
#define ENV_LEN         (1 << ENV_BITS)
#define ENV_STEP        (128.0 / ENV_LEN)

#define MAX_ATT_INDEX   ((1 << (ENV_BITS - 2)) - 1)   // 255
#define MIN_ATT_INDEX   0

#define EG_DMP          5
#define EG_ATT          4
#define EG_DEC          3
#define EG_SUS          2
#define EG_REL          1
#define EG_OFF          0

#define SIN_BITS        10
#define SIN_LEN         (1 << SIN_BITS)
#define SIN_MASK        (SIN_LEN - 1)

#define TL_RES_LEN      256
#define TL_TAB_LEN      (11 * 2 * TL_RES_LEN)
#define ENV_QUIET       (TL_TAB_LEN >> 5)

#define RATE_STEPS      8

#define SLOT1           0
#define SLOT2           1

// Key scale level: MAME stores doubles / DV with DV = 0.1875; every entry is
// an exact integer (KSL(db) = db * 16 / 3).
#define KSL(db) ((uint8_t)((db) * 16.0 / 3.0))
static const uint8_t ksl_tab[8 * 16] = {
    /* OCT 0 */
    KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),
    KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),
    /* OCT 1 */
    KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),
    KSL( 0.000),KSL( 0.750),KSL( 1.125),KSL( 1.500),KSL( 1.875),KSL( 2.250),KSL( 2.625),KSL( 3.000),
    /* OCT 2 */
    KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 1.125),KSL( 1.875),KSL( 2.625),
    KSL( 3.000),KSL( 3.750),KSL( 4.125),KSL( 4.500),KSL( 4.875),KSL( 5.250),KSL( 5.625),KSL( 6.000),
    /* OCT 3 */
    KSL( 0.000),KSL( 0.000),KSL( 0.000),KSL( 1.875),KSL( 3.000),KSL( 4.125),KSL( 4.875),KSL( 5.625),
    KSL( 6.000),KSL( 6.750),KSL( 7.125),KSL( 7.500),KSL( 7.875),KSL( 8.250),KSL( 8.625),KSL( 9.000),
    /* OCT 4 */
    KSL( 0.000),KSL( 0.000),KSL( 3.000),KSL( 4.875),KSL( 6.000),KSL( 7.125),KSL( 7.875),KSL( 8.625),
    KSL( 9.000),KSL( 9.750),KSL(10.125),KSL(10.500),KSL(10.875),KSL(11.250),KSL(11.625),KSL(12.000),
    /* OCT 5 */
    KSL( 0.000),KSL( 3.000),KSL( 6.000),KSL( 7.875),KSL( 9.000),KSL(10.125),KSL(10.875),KSL(11.625),
    KSL(12.000),KSL(12.750),KSL(13.125),KSL(13.500),KSL(13.875),KSL(14.250),KSL(14.625),KSL(15.000),
    /* OCT 6 */
    KSL( 0.000),KSL( 6.000),KSL( 9.000),KSL(10.875),KSL(12.000),KSL(13.125),KSL(13.875),KSL(14.625),
    KSL(15.000),KSL(15.750),KSL(16.125),KSL(16.500),KSL(16.875),KSL(17.250),KSL(17.625),KSL(18.000),
    /* OCT 7 */
    KSL( 0.000),KSL( 9.000),KSL(12.000),KSL(13.875),KSL(15.000),KSL(16.125),KSL(16.875),KSL(17.625),
    KSL(18.000),KSL(18.750),KSL(19.125),KSL(19.500),KSL(19.875),KSL(20.250),KSL(20.625),KSL(21.000)
};
#undef KSL

// 0 / 1.5 / 3.0 / 6.0 dB/OCT — confirmed on a real YM2413 (the application
// manual is incorrect); note the order differs from the OPL3 table.
static const uint32_t ksl_shift[4] = { 31, 2, 1, 0 };

// sustain level table: 0..45 dB in 3 dB steps
#define SC(db) ((uint32_t)((db) * (1.0 / ENV_STEP)))
static const uint32_t sl_tab[16] = {
    SC( 0),SC( 1),SC( 2),SC( 3),SC( 4),SC( 5),SC( 6),SC( 7),
    SC( 8),SC( 9),SC(10),SC(11),SC(12),SC(13),SC(14),SC(15)
};
#undef SC

static const uint8_t eg_inc[15 * RATE_STEPS] = {
/*cycle:0 1  2 3  4 5  6 7*/
/* 0 */ 0,1, 0,1, 0,1, 0,1,
/* 1 */ 0,1, 0,1, 1,1, 0,1,
/* 2 */ 0,1, 1,1, 0,1, 1,1,
/* 3 */ 0,1, 1,1, 1,1, 1,1,

/* 4 */ 1,1, 1,1, 1,1, 1,1,
/* 5 */ 1,1, 1,2, 1,1, 1,2,
/* 6 */ 1,2, 1,2, 1,2, 1,2,
/* 7 */ 1,2, 2,2, 1,2, 2,2,

/* 8 */ 2,2, 2,2, 2,2, 2,2,
/* 9 */ 2,2, 2,4, 2,2, 2,4,
/*10 */ 2,4, 2,4, 2,4, 2,4,
/*11 */ 2,4, 4,4, 2,4, 4,4,

/*12 */ 4,4, 4,4, 4,4, 4,4,
/*13 */ 8,8, 8,8, 8,8, 8,8,
/*14 */ 0,0, 0,0, 0,0, 0,0,
};

#define O(a) ((a) * RATE_STEPS)
static const uint8_t eg_rate_select[16 + 64 + 16] = {
O(14),O(14),O(14),O(14),O(14),O(14),O(14),O(14),
O(14),O(14),O(14),O(14),O(14),O(14),O(14),O(14),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3),
O( 4),O( 5),O( 6),O( 7),
O( 8),O( 9),O(10),O(11),
O(12),O(12),O(12),O(12),
O(12),O(12),O(12),O(12),O(12),O(12),O(12),O(12),
O(12),O(12),O(12),O(12),O(12),O(12),O(12),O(12),
};
#undef O

#define O(a) (a)
static const uint8_t eg_rate_shift[16 + 64 + 16] = {
O(0),O(0),O(0),O(0),O(0),O(0),O(0),O(0),
O(0),O(0),O(0),O(0),O(0),O(0),O(0),O(0),
O(13),O(13),O(13),O(13), O(12),O(12),O(12),O(12),
O(11),O(11),O(11),O(11), O(10),O(10),O(10),O(10),
O( 9),O( 9),O( 9),O( 9), O( 8),O( 8),O( 8),O( 8),
O( 7),O( 7),O( 7),O( 7), O( 6),O( 6),O( 6),O( 6),
O( 5),O( 5),O( 5),O( 5), O( 4),O( 4),O( 4),O( 4),
O( 3),O( 3),O( 3),O( 3), O( 2),O( 2),O( 2),O( 2),
O( 1),O( 1),O( 1),O( 1),
O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
};
#undef O

#define ML 2
static const uint8_t mul_tab[16] = {
    ML/2, 1*ML, 2*ML, 3*ML, 4*ML, 5*ML, 6*ML, 7*ML,
    8*ML, 9*ML,10*ML,10*ML,12*ML,12*ML,15*ML,15*ML
};
#undef ML

#define LFO_AM_TAB_ELEMENTS 210
static const uint8_t lfo_am_table[LFO_AM_TAB_ELEMENTS] = {
0,0,0,0,0,0,0,
1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4, 5,5,5,5, 6,6,6,6, 7,7,7,7,
8,8,8,8, 9,9,9,9, 10,10,10,10, 11,11,11,11, 12,12,12,12, 13,13,13,13,
14,14,14,14, 15,15,15,15, 16,16,16,16, 17,17,17,17, 18,18,18,18,
19,19,19,19, 20,20,20,20, 21,21,21,21, 22,22,22,22, 23,23,23,23,
24,24,24,24, 25,25,25,25, 26,26,26,
25,25,25,25, 24,24,24,24, 23,23,23,23, 22,22,22,22, 21,21,21,21,
20,20,20,20, 19,19,19,19, 18,18,18,18, 17,17,17,17, 16,16,16,16,
15,15,15,15, 14,14,14,14, 13,13,13,13, 12,12,12,12, 11,11,11,11,
10,10,10,10, 9,9,9,9, 8,8,8,8, 7,7,7,7, 6,6,6,6, 5,5,5,5,
4,4,4,4, 3,3,3,3, 2,2,2,2, 1,1,1,1
};

// LFO Phase Modulation table (verified on real YM2413 — the OPLL variant)
static const int8_t lfo_pm_table[8 * 8] = {
/* FNUM2/FNUM = 0 00xxxxxx (0x0000) */ 0, 0, 0, 0, 0, 0, 0, 0,
/* FNUM2/FNUM = 0 01xxxxxx (0x0040) */ 1, 0, 0, 0,-1, 0, 0, 0,
/* FNUM2/FNUM = 0 10xxxxxx (0x0080) */ 2, 1, 0,-1,-2,-1, 0, 1,
/* FNUM2/FNUM = 0 11xxxxxx (0x00C0) */ 3, 1, 0,-1,-3,-1, 0, 1,
/* FNUM2/FNUM = 1 00xxxxxx (0x0100) */ 4, 2, 0,-2,-4,-2, 0, 2,
/* FNUM2/FNUM = 1 01xxxxxx (0x0140) */ 5, 2, 0,-2,-5,-2, 0, 2,
/* FNUM2/FNUM = 1 10xxxxxx (0x0180) */ 6, 3, 0,-3,-6,-3, 0, 3,
/* FNUM2/FNUM = 1 11xxxxxx (0x01C0) */ 7, 3, 0,-3,-7,-3, 0, 3,
};

// The YM2413 instrument ROM (MAME's dumps: melody patches via audio
// analysis by Jarek, drum patches from the VRC7 debug-mode dump).
// MULT MULT modTL DcDmFb AR/DR AR/DR SL/RR SL/RR
static const uint8_t opll_rom[19][8] = {
    {0x49, 0x4c, 0x4c, 0x12, 0x00, 0x00, 0x00, 0x00},   // 0 (user)
    {0x61, 0x61, 0x1e, 0x17, 0xf0, 0x78, 0x00, 0x17},   // 1 violin
    {0x13, 0x41, 0x1e, 0x0d, 0xd7, 0xf7, 0x13, 0x13},   // 2 guitar
    {0x13, 0x01, 0x99, 0x04, 0xf2, 0xf4, 0x11, 0x23},   // 3 piano
    {0x21, 0x61, 0x1b, 0x07, 0xaf, 0x64, 0x40, 0x27},   // 4 flute
    {0x22, 0x21, 0x1e, 0x06, 0xf0, 0x75, 0x08, 0x18},   // 5 clarinet
    {0x31, 0x22, 0x16, 0x05, 0x90, 0x71, 0x00, 0x13},   // 6 oboe
    {0x21, 0x61, 0x1d, 0x07, 0x82, 0x80, 0x10, 0x17},   // 7 trumpet
    {0x23, 0x21, 0x2d, 0x16, 0xc0, 0x70, 0x07, 0x07},   // 8 organ
    {0x61, 0x61, 0x1b, 0x06, 0x64, 0x65, 0x10, 0x17},   // 9 horn
    {0x61, 0x61, 0x0c, 0x18, 0x85, 0xf0, 0x70, 0x07},   // A synth
    {0x23, 0x01, 0x07, 0x11, 0xf0, 0xa4, 0x00, 0x22},   // B harpsichord
    {0x97, 0xc1, 0x24, 0x07, 0xff, 0xf8, 0x22, 0x12},   // C vibraphone
    {0x61, 0x10, 0x0c, 0x05, 0xf2, 0xf4, 0x40, 0x44},   // D synth bass
    {0x01, 0x01, 0x55, 0x03, 0xf3, 0x92, 0xf3, 0xf3},   // E acoustic bass
    {0x61, 0x41, 0x89, 0x03, 0xf1, 0xf4, 0xf0, 0x13},   // F electric guitar
    {0x01, 0x01, 0x18, 0x0f, 0xdf, 0xf8, 0x6a, 0x6d},   // BD
    {0x01, 0x01, 0x00, 0x00, 0xc8, 0xd8, 0xa7, 0x68},   // HH, SD
    {0x05, 0x01, 0x00, 0x00, 0xf8, 0xaa, 0x59, 0x55},   // TOM, TOP CYM
};

#define SLOT7_1 (&m_ch[7].SLOT[SLOT1])
#define SLOT7_2 (&m_ch[7].SLOT[SLOT2])
#define SLOT8_1 (&m_ch[8].SLOT[SLOT1])
#define SLOT8_2 (&m_ch[8].SLOT[SLOT2])

static inline int limit(int val, int max, int min) {
    if (val > max) return max;
    if (val < min) return min;
    return val;
}

// ── shared tables (heap, reference counted — same scheme as OplFm) ──────────
// tl_base: the x = 0..255 row of MAME's tl_tab (rows 1..10 = shifted, sign =
// NEGATION on the OPLL, not one's complement); sin0: waveform 0 in the log
// domain (value*2 + sign), waveform 1 = positive half only. Byte-for-byte
// device_start() math.
static int       s_tab_refs = 0;
static uint16_t* s_tl_base = nullptr;   // 256 entries
static uint16_t* s_sin0    = nullptr;   // SIN_LEN entries

static void opll_build_tables() {
    if (s_tab_refs++ > 0) return;
    s_tl_base = (uint16_t*)malloc(TL_RES_LEN * sizeof(uint16_t));
    s_sin0    = (uint16_t*)malloc(SIN_LEN * sizeof(uint16_t));
    if (!s_tl_base || !s_sin0) {
        free(s_tl_base); s_tl_base = nullptr;
        free(s_sin0);    s_sin0 = nullptr;
        return;
    }
    for (int x = 0; x < TL_RES_LEN; x++) {
        double m = floor((1 << 16) / pow(2, (x + 1) * (ENV_STEP / 4.0) / 8.0));
        int n = ((int)m) >> 4;
        n = (n & 1) ? (n >> 1) + 1 : n >> 1;
        s_tl_base[x] = (uint16_t)n;
    }
    for (int i = 0; i < SIN_LEN; i++) {
        double m = sin(((i * 2) + 1) * M_PI / SIN_LEN);
        double o = 8 * log(1.0 / fabs(m)) / log(2.0);
        o = o / (ENV_STEP / 4);
        int n = (int)(2.0 * o);
        n = (n & 1) ? (n >> 1) + 1 : n >> 1;
        s_sin0[i] = (uint16_t)(n * 2 + (m >= 0.0 ? 0 : 1));
    }
}

static void opll_free_tables() {
    if (--s_tab_refs > 0) return;
    free(s_tl_base); s_tl_base = nullptr;
    free(s_sin0);    s_sin0 = nullptr;
}

bool OpllFm::tablesReady() { return s_tl_base && s_sin0; }

// tl_tab[p]: base entry ((p&511)>>1), row (p>>9), sign (p&1) as negation
static inline int32_t tl_fetch(uint32_t p) {
    int32_t v = s_tl_base[(p & 511) >> 1] >> (p >> 9);
    return (p & 1) ? -v : v;
}

// sin_tab[wave][i]: wave 1 silences the negative half
static inline uint32_t sin_fetch(uint32_t wave, uint32_t i) {
    if (wave && (i & 512)) return TL_TAB_LEN;
    return s_sin0[i];
}

static inline int32_t op_calc(uint32_t phase, uint32_t env, int32_t pm, uint32_t wave) {
    uint32_t p = (env << 5) +
        sin_fetch(wave, (((int32_t)((phase & ~FREQ_MASK) + (pm << 17))) >> FREQ_SH) & SIN_MASK);
    if (p >= TL_TAB_LEN) return 0;
    return tl_fetch(p);
}

static inline int32_t op_calc1(uint32_t phase, uint32_t env, int32_t pm, uint32_t wave) {
    int32_t i = (phase & ~FREQ_MASK) + pm;
    uint32_t p = (env << 5) + sin_fetch(wave, (i >> FREQ_SH) & SIN_MASK);
    if (p >= TL_TAB_LEN) return 0;
    return tl_fetch(p);
}

#define volume_calc(OP) ((OP)->TLL + ((uint32_t)(OP)->volume) + (m_LFO_AM & (OP)->AMmask))

// ── construction / rates ────────────────────────────────────────────────────

OpllFm::OpllFm() {
    memset((void*)this, 0, sizeof(*this));
    opll_build_tables();
}

OpllFm::~OpllFm() {
    opll_free_tables();
}

void OpllFm::setRates(int clock, int rate, bool halfRate) {
    m_clock = clock;
    m_rate  = rate;
    m_half  = halfRate ? 1 : 0;

    // MAME runs the OPLL at its native clock/72; we rate-convert exactly the
    // way ymf262.cpp does, from the SYNTH rate (rate/2 in half-rate mode).
    int synth_rate = halfRate ? rate / 2 : rate;
    double freqbase = synth_rate ? ((double)clock / 72.0) / (double)synth_rate : 0.0;

    for (int i = 0; i < 1024; i++)
        m_fn_tab[i] = (uint32_t)((double)i * 64 * freqbase * (1 << (FREQ_SH - 10)));

    m_lfo_am_inc = (uint32_t)(((1 << LFO_SH) / 64.0)   * freqbase);
    m_lfo_pm_inc = (uint32_t)(((1 << LFO_SH) / 1024.0) * freqbase);
    m_noise_f    = (uint32_t)((1 << FREQ_SH) * freqbase);

    m_eg_timer_add      = (uint32_t)((1 << EG_SH) * freqbase);
    m_eg_timer_overflow = 1 << EG_SH;
}

// ── key on/off, rates, setters — exactly MAME ───────────────────────────────

void OpllFm::keyOn(Slot* SLOT, uint32_t key_set) {
    if (!SLOT->key) {
        // do NOT restart the Phase Generator (verified on real YM2413):
        // the dump phase ramps the old note down first, then resets phase
        SLOT->state = EG_DMP;
    }
    SLOT->key |= key_set;
}

void OpllFm::keyOff(Slot* SLOT, uint32_t key_clr) {
    if (SLOT->key) {
        SLOT->key &= key_clr;
        if (!SLOT->key) {
            if (SLOT->state > EG_REL)
                SLOT->state = EG_REL;
        }
    }
}

void OpllFm::calcFcSlot(Chan* CH, Slot* SLOT) {
    SLOT->freq = CH->fc * SLOT->mul;
    int ksr = CH->kcode >> SLOT->KSR;

    if (SLOT->ksr != ksr) {
        SLOT->ksr = ksr;
        if ((SLOT->ar + SLOT->ksr) < 16 + 62) {
            SLOT->eg_sh_ar  = eg_rate_shift [SLOT->ar + SLOT->ksr];
            SLOT->eg_sel_ar = eg_rate_select[SLOT->ar + SLOT->ksr];
        } else {
            SLOT->eg_sh_ar  = 0;
            SLOT->eg_sel_ar = 13 * RATE_STEPS;
        }
        SLOT->eg_sh_dr  = eg_rate_shift [SLOT->dr + SLOT->ksr];
        SLOT->eg_sel_dr = eg_rate_select[SLOT->dr + SLOT->ksr];
        SLOT->eg_sh_rr  = eg_rate_shift [SLOT->rr + SLOT->ksr];
        SLOT->eg_sel_rr = eg_rate_select[SLOT->rr + SLOT->ksr];
    }

    uint32_t SLOT_rs = CH->sus ? (16 + (5 << 2)) : (16 + (7 << 2));
    SLOT->eg_sh_rs  = eg_rate_shift [SLOT_rs + SLOT->ksr];
    SLOT->eg_sel_rs = eg_rate_select[SLOT_rs + SLOT->ksr];

    uint32_t SLOT_dp = 16 + (13 << 2);
    SLOT->eg_sh_dp  = eg_rate_shift [SLOT_dp + SLOT->ksr];
    SLOT->eg_sel_dp = eg_rate_select[SLOT_dp + SLOT->ksr];
}

void OpllFm::setMul(int slot, int v) {
    Chan* CH   = &m_ch[slot / 2];
    Slot* SLOT = &CH->SLOT[slot & 1];

    SLOT->mul     = mul_tab[v & 0x0f];
    SLOT->KSR     = (v & 0x10) ? 0 : 2;
    SLOT->eg_type = (v & 0x20);
    SLOT->vib     = (v & 0x40);
    SLOT->AMmask  = (v & 0x80) ? ~0u : 0;
    calcFcSlot(CH, SLOT);
}

void OpllFm::setKslTl(int chan, int v) {
    Chan* CH   = &m_ch[chan];
    Slot* SLOT = &CH->SLOT[SLOT1];      // modulator

    SLOT->ksl = ksl_shift[v >> 6];
    SLOT->TL  = (v & 0x3f) << (ENV_BITS - 2 - 7);
    SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);
}

void OpllFm::setKslWaveFb(int chan, int v) {
    Chan* CH   = &m_ch[chan];
    Slot* SLOT = &CH->SLOT[SLOT1];      // modulator
    SLOT->wavetable = (v & 0x08) >> 3;
    SLOT->fb_shift  = (v & 7) ? (v & 7) + 8 : 0;

    SLOT = &CH->SLOT[SLOT2];            // carrier
    SLOT->ksl = ksl_shift[v >> 6];
    SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);
    SLOT->wavetable = (v & 0x10) >> 4;
}

void OpllFm::setArDr(int slot, int v) {
    Chan* CH   = &m_ch[slot / 2];
    Slot* SLOT = &CH->SLOT[slot & 1];

    SLOT->ar = (v >> 4) ? 16 + ((v >> 4) << 2) : 0;
    if ((SLOT->ar + SLOT->ksr) < 16 + 62) {
        SLOT->eg_sh_ar  = eg_rate_shift [SLOT->ar + SLOT->ksr];
        SLOT->eg_sel_ar = eg_rate_select[SLOT->ar + SLOT->ksr];
    } else {
        SLOT->eg_sh_ar  = 0;
        SLOT->eg_sel_ar = 13 * RATE_STEPS;
    }
    SLOT->dr        = (v & 0x0f) ? 16 + ((v & 0x0f) << 2) : 0;
    SLOT->eg_sh_dr  = eg_rate_shift [SLOT->dr + SLOT->ksr];
    SLOT->eg_sel_dr = eg_rate_select[SLOT->dr + SLOT->ksr];
}

void OpllFm::setSlRr(int slot, int v) {
    Chan* CH   = &m_ch[slot / 2];
    Slot* SLOT = &CH->SLOT[slot & 1];

    SLOT->sl        = sl_tab[v >> 4];
    SLOT->rr        = (v & 0x0f) ? 16 + ((v & 0x0f) << 2) : 0;
    SLOT->eg_sh_rr  = eg_rate_shift [SLOT->rr + SLOT->ksr];
    SLOT->eg_sel_rr = eg_rate_select[SLOT->rr + SLOT->ksr];
}

void OpllFm::loadInstrument(int chan, int slot, const uint8_t* inst) {
    setMul      (slot,     inst[0]);
    setMul      (slot + 1, inst[1]);
    setKslTl    (chan,     inst[2]);
    setKslWaveFb(chan,     inst[3]);
    setArDr     (slot,     inst[4]);
    setArDr     (slot + 1, inst[5]);
    setSlRr     (slot,     inst[6]);
    setSlRr     (slot + 1, inst[7]);
}

// A user-patch register write re-applies that byte to every channel that has
// instrument 0 selected — exactly MAME's update_instrument_zero.
void OpllFm::updateInstrumentZero(uint8_t r) {
    const uint8_t* inst = &m_inst_tab[0][0];
    int chan_max = (m_rhythm & 0x20) ? 6 : 9;

    for (int chan = 0; chan < chan_max; chan++) {
        if ((m_instvol_r[chan] & 0xf0) != 0) continue;
        switch (r) {
        case 0: setMul      (chan * 2,     inst[0]); break;
        case 1: setMul      (chan * 2 + 1, inst[1]); break;
        case 2: setKslTl    (chan,         inst[2]); break;
        case 3: setKslWaveFb(chan,         inst[3]); break;
        case 4: setArDr     (chan * 2,     inst[4]); break;
        case 5: setArDr     (chan * 2 + 1, inst[5]); break;
        case 6: setSlRr     (chan * 2,     inst[6]); break;
        case 7: setSlRr     (chan * 2 + 1, inst[7]); break;
        }
    }
}

// ── register write — exactly MAME's write_reg ───────────────────────────────

void OpllFm::writeReg(int r, int v) {
    m_eg_dirty = 1;

    r &= 0xff;
    v &= 0xff;

    int chan, slot;
    Chan* CH;
    Slot* SLOT;
    const uint8_t* inst;

    switch (r & 0xf0) {
    case 0x00:
        switch (r & 0x0f) {
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x04: case 0x05: case 0x06: case 0x07:
            m_inst_tab[0][r & 0x07] = (uint8_t)v;
            updateInstrumentZero(r & 7);
            break;

        case 0x0e:      /* x, x, r,bd,sd,tom,tc,hh */
            if (v & 0x20) {
                if ((m_rhythm & 0x20) == 0) {
                    /* rhythm off -> on: load the drum instruments */
                    loadInstrument(6, 12, &m_inst_tab[16][0]);

                    loadInstrument(7, 14, &m_inst_tab[17][0]);
                    CH   = &m_ch[7];
                    SLOT = &CH->SLOT[SLOT1];    /* modulator envelope is HH */
                    SLOT->TL  = ((m_instvol_r[7] >> 4) << 2) << (ENV_BITS - 2 - 7);
                    SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);

                    loadInstrument(8, 16, &m_inst_tab[18][0]);
                    CH   = &m_ch[8];
                    SLOT = &CH->SLOT[SLOT1];    /* modulator envelope is TOM */
                    SLOT->TL  = ((m_instvol_r[8] >> 4) << 2) << (ENV_BITS - 2 - 7);
                    SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);
                }
                /* BD key on/off */
                if (v & 0x10) { keyOn(&m_ch[6].SLOT[SLOT1], 2); keyOn(&m_ch[6].SLOT[SLOT2], 2); }
                else          { keyOff(&m_ch[6].SLOT[SLOT1], ~2u); keyOff(&m_ch[6].SLOT[SLOT2], ~2u); }
                /* HH */
                if (v & 0x01) keyOn (&m_ch[7].SLOT[SLOT1], 2);
                else          keyOff(&m_ch[7].SLOT[SLOT1], ~2u);
                /* SD */
                if (v & 0x08) keyOn (&m_ch[7].SLOT[SLOT2], 2);
                else          keyOff(&m_ch[7].SLOT[SLOT2], ~2u);
                /* TOM */
                if (v & 0x04) keyOn (&m_ch[8].SLOT[SLOT1], 2);
                else          keyOff(&m_ch[8].SLOT[SLOT1], ~2u);
                /* TOP-CY */
                if (v & 0x02) keyOn (&m_ch[8].SLOT[SLOT2], 2);
                else          keyOff(&m_ch[8].SLOT[SLOT2], ~2u);
            } else {
                if (m_rhythm & 0x20) {
                    /* rhythm on -> off: restore the melody instruments */
                    loadInstrument(6, 12, &m_inst_tab[m_instvol_r[6] >> 4][0]);
                    loadInstrument(7, 14, &m_inst_tab[m_instvol_r[7] >> 4][0]);
                    loadInstrument(8, 16, &m_inst_tab[m_instvol_r[8] >> 4][0]);
                }
                keyOff(&m_ch[6].SLOT[SLOT1], ~2u); keyOff(&m_ch[6].SLOT[SLOT2], ~2u);
                keyOff(&m_ch[7].SLOT[SLOT1], ~2u); keyOff(&m_ch[7].SLOT[SLOT2], ~2u);
                keyOff(&m_ch[8].SLOT[SLOT1], ~2u); keyOff(&m_ch[8].SLOT[SLOT2], ~2u);
            }
            m_rhythm = v & 0x3f;
            break;
        }
        break;

    case 0x10:
    case 0x20: {
        uint32_t block_fnum;

        chan = r & 0x0f;
        if (chan >= 9) chan -= 9;       /* verified on real YM2413 */
        CH = &m_ch[chan];

        if (r & 0x10) {     /* 10-18: FNUM 0-7 */
            block_fnum = (CH->block_fnum & 0x0f00) | v;
        } else {            /* 20-28: suson, keyon, block, FNUM 8 */
            block_fnum = ((v & 0x0f) << 8) | (CH->block_fnum & 0xff);

            if (v & 0x10) { keyOn(&CH->SLOT[SLOT1], 1); keyOn(&CH->SLOT[SLOT2], 1); }
            else          { keyOff(&CH->SLOT[SLOT1], ~1u); keyOff(&CH->SLOT[SLOT2], ~1u); }

            CH->sus = v & 0x20;
        }

        if (CH->block_fnum != block_fnum) {
            CH->block_fnum = block_fnum;

            /* BLK 2,1,0 -> kcode bits 3,2,1; FNUM MSB -> kcode LSB */
            CH->kcode    = (block_fnum & 0x0f00) >> 8;
            CH->ksl_base = ksl_tab[block_fnum >> 5];

            block_fnum   = block_fnum * 2;
            uint8_t block = (block_fnum & 0x1c00) >> 10;
            CH->fc       = m_fn_tab[block_fnum & 0x03ff] >> (7 - block);

            CH->SLOT[SLOT1].TLL = CH->SLOT[SLOT1].TL + (CH->ksl_base >> CH->SLOT[SLOT1].ksl);
            CH->SLOT[SLOT2].TLL = CH->SLOT[SLOT2].TL + (CH->ksl_base >> CH->SLOT[SLOT2].ksl);

            calcFcSlot(CH, &CH->SLOT[SLOT1]);
            calcFcSlot(CH, &CH->SLOT[SLOT2]);
        }
        break;
    }

    case 0x30: {    /* inst 4 MSBs, VOL 4 LSBs */
        chan = r & 0x0f;
        if (chan >= 9) chan -= 9;       /* verified on real YM2413 */

        uint8_t old_instvol = m_instvol_r[chan];
        m_instvol_r[chan] = (uint8_t)v;

        CH   = &m_ch[chan];
        SLOT = &CH->SLOT[SLOT2];        /* carrier */
        SLOT->TL  = ((v & 0x0f) << 2) << (ENV_BITS - 2 - 7);
        SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);

        if ((chan >= 6) && (m_rhythm & 0x20)) {
            /* rhythm mode: channels 7/8 keep the drum patches; the high
               nibble is the second drum's volume, on the modulator */
            if (chan >= 7) {
                SLOT = &CH->SLOT[SLOT1];    /* HH (chan 7) or TOM (chan 8) */
                SLOT->TL  = ((m_instvol_r[chan] >> 4) << 2) << (ENV_BITS - 2 - 7);
                SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);
            }
        } else {
            if ((old_instvol & 0xf0) == (v & 0xf0))
                return;
            inst = &m_inst_tab[m_instvol_r[chan] >> 4][0];
            slot = chan * 2;
            loadInstrument(chan, slot, inst);
        }
        break;
    }

    default:
        break;
    }
}

// ── reset ───────────────────────────────────────────────────────────────────

void OpllFm::reset() {
    m_eg_timer = 0;
    m_eg_cnt   = 0;
    m_eg_next  = 0;
    m_eg_dirty = 1;
    m_noise_rng = 1;
    m_noise_p   = 0;
    m_half_tick = 0;
    m_prev = m_cur = 0;

    memcpy(m_inst_tab, opll_rom, sizeof(m_inst_tab));

    /* reset with register writes, exactly MAME's device_reset */
    writeReg(0x0f, 0);
    for (int i = 0x3f; i >= 0x10; i--)
        writeReg(i, 0x00);

    for (int c = 0; c < 9; c++) {
        Chan* CH = &m_ch[c];
        for (int s = 0; s < 2; s++) {
            CH->SLOT[s].wavetable = 0;
            CH->SLOT[s].state     = EG_OFF;
            CH->SLOT[s].volume    = MAX_ATT_INDEX;
            CH->SLOT[s].op1_out[0] = CH->SLOT[s].op1_out[1] = 0;
        }
    }
}

// ── per-sample pipeline ─────────────────────────────────────────────────────

void OpllFm::advanceLfo() {
    m_lfo_am_cnt += m_lfo_am_inc;
    if (m_lfo_am_cnt >= ((uint32_t)LFO_AM_TAB_ELEMENTS << LFO_SH))
        m_lfo_am_cnt -= ((uint32_t)LFO_AM_TAB_ELEMENTS << LFO_SH);

    m_LFO_AM = lfo_am_table[m_lfo_am_cnt >> LFO_SH] >> 1;

    m_lfo_pm_cnt += m_lfo_pm_inc;
    m_LFO_PM = (m_lfo_pm_cnt >> LFO_SH) & 7;
}

void OpllFm::advance() {
    m_eg_timer += m_eg_timer_add;

    while (m_eg_timer >= m_eg_timer_overflow) {
        m_eg_timer -= m_eg_timer_overflow;
        m_eg_cnt++;

        if (m_eg_cnt < m_eg_next && !m_eg_dirty)
            continue;               // provably no slot can act on this tick

        // Like OplFm: the walk recomputes the next-actionable bound inline.
        // Excluded as unable to act: EG_OFF; sustain that holds (non-perc,
        // or percussive clamped at MAX_ATT); melody-mode modulators in
        // EG_REL (the chip never lets them perform release); infinity rows.
        uint32_t eg_next = 0xFFFFFFFFu;
        uint8_t  rhy = m_rhythm & 0x20;

        for (int i = 0; i < 9 * 2; i++) {
            Chan* CH = &m_ch[i / 2];
            Slot* op = &CH->SLOT[i & 1];

            switch (op->state) {
            case EG_DMP:    /* dump: ramp the old note down, then attack */
                if (!(m_eg_cnt & ((1 << op->eg_sh_dp) - 1))) {
                    op->volume += eg_inc[op->eg_sel_dp + ((m_eg_cnt >> op->eg_sh_dp) & 7)];
                    if (op->volume >= MAX_ATT_INDEX) {
                        op->volume = MAX_ATT_INDEX;
                        op->state = EG_ATT;
                        op->phase = 0;      /* THIS is where phase restarts */
                    }
                }
                break;

            case EG_ATT:
                if (!(m_eg_cnt & ((1 << op->eg_sh_ar) - 1))) {
                    op->volume += (~op->volume *
                        (eg_inc[op->eg_sel_ar + ((m_eg_cnt >> op->eg_sh_ar) & 7)])) >> 2;
                    if (op->volume <= MIN_ATT_INDEX) {
                        op->volume = MIN_ATT_INDEX;
                        op->state = EG_DEC;
                    }
                }
                break;

            case EG_DEC:
                if (!(m_eg_cnt & ((1 << op->eg_sh_dr) - 1))) {
                    op->volume += eg_inc[op->eg_sel_dr + ((m_eg_cnt >> op->eg_sh_dr) & 7)];
                    if (op->volume >= (int32_t)op->sl)
                        op->state = EG_SUS;
                }
                break;

            case EG_SUS:
                if (op->eg_type) {
                    /* non-percussive: hold */
                } else {
                    if (!(m_eg_cnt & ((1 << op->eg_sh_rr) - 1))) {
                        op->volume += eg_inc[op->eg_sel_rr + ((m_eg_cnt >> op->eg_sh_rr) & 7)];
                        if (op->volume >= MAX_ATT_INDEX)
                            op->volume = MAX_ATT_INDEX;
                    }
                }
                break;

            case EG_REL:
                /* only carriers — and rhythm slots in rhythm mode — perform
                   release; melody modulators hold (verified, see MAME) */
                if ((i & 1) || (rhy && i >= 12)) {
                    uint8_t sh, sel;
                    if (op->eg_type) {  /* non-percussive: RR, or RS when SUS on */
                        if (CH->sus) { sh = op->eg_sh_rs; sel = op->eg_sel_rs; }
                        else         { sh = op->eg_sh_rr; sel = op->eg_sel_rr; }
                    } else {            /* percussive: RS */
                        sh = op->eg_sh_rs; sel = op->eg_sel_rs;
                    }
                    if (!(m_eg_cnt & ((1 << sh) - 1))) {
                        op->volume += eg_inc[sel + ((m_eg_cnt >> sh) & 7)];
                        if (op->volume >= MAX_ATT_INDEX) {
                            op->volume = MAX_ATT_INDEX;
                            op->state = EG_OFF;
                        }
                    }
                }
                break;

            default:
                break;
            }

            /* inline next-fire bound for this slot's (possibly new) state */
            uint8_t nsh;
            uint16_t nsel;
            switch (op->state) {
            case EG_DMP: nsh = op->eg_sh_dp; nsel = op->eg_sel_dp; break;
            case EG_ATT: nsh = op->eg_sh_ar; nsel = op->eg_sel_ar; break;
            case EG_DEC: nsh = op->eg_sh_dr; nsel = op->eg_sel_dr; break;
            case EG_SUS:
                if (op->eg_type || op->volume >= MAX_ATT_INDEX) continue;
                nsh = op->eg_sh_rr; nsel = op->eg_sel_rr; break;
            case EG_REL:
                if (!((i & 1) || (rhy && i >= 12))) continue;
                if (op->eg_type && !CH->sus) { nsh = op->eg_sh_rr; nsel = op->eg_sel_rr; }
                else                         { nsh = op->eg_sh_rs; nsel = op->eg_sel_rs; }
                break;
            default:     continue;          /* EG_OFF */
            }
            if (nsel == 14 * RATE_STEPS) continue;
            uint32_t nx = ((m_eg_cnt >> nsh) + 1) << nsh;
            if (nx < eg_next) eg_next = nx;
        }

        m_eg_next = eg_next;
        m_eg_dirty = 0;
    }

    uint8_t rhy = m_rhythm & 0x20;
    for (int i = 0; i < 9 * 2; i++) {
        Chan* CH = &m_ch[i / 2];
        Slot* op = &CH->SLOT[i & 1];

        /* A silent operator's phase is unobservable: an EG_OFF slot sits at
           MAX_ATT, so its key-on dump ends on the FIRST eg fire, which is
           what resets the phase — EXCEPT rhythm mode, where HH/SD/TOP read
           ch7 slot1 / ch8 slot2 phases regardless of their own envelopes. */
        if (op->state == EG_OFF && !(rhy && i >= 12))
            continue;

        if (op->vib) {
            uint32_t fnum_lfo   = 8 * ((CH->block_fnum & 0x01c0) >> 6);
            uint32_t block_fnum = CH->block_fnum * 2;
            int32_t  lfo_fn_table_index_offset = lfo_pm_table[m_LFO_PM + fnum_lfo];

            if (lfo_fn_table_index_offset) {
                block_fnum += lfo_fn_table_index_offset;
                uint8_t block = (block_fnum & 0x1c00) >> 10;
                op->phase += (m_fn_tab[block_fnum & 0x03ff] >> (7 - block)) * op->mul;
            } else {
                op->phase += op->freq;
            }
        } else {
            op->phase += op->freq;
        }
    }

    m_noise_p += m_noise_f;
    int i = m_noise_p >> FREQ_SH;
    m_noise_p &= FREQ_MASK;
    while (i) {
        if (m_noise_rng & 1) m_noise_rng ^= 0x800302;
        m_noise_rng >>= 1;
        i--;
    }
}

void OpllFm::chanCalc(Chan* CH) {
    /* SLOT 1 */
    Slot* SLOT = &CH->SLOT[SLOT1];
    uint32_t env = volume_calc(SLOT);
    int32_t  out = SLOT->op1_out[0] + SLOT->op1_out[1];

    SLOT->op1_out[0] = SLOT->op1_out[1];
    int32_t phase_modulation = SLOT->op1_out[0];

    SLOT->op1_out[1] = 0;
    if (env < ENV_QUIET) {
        if (!SLOT->fb_shift) out = 0;
        SLOT->op1_out[1] = op_calc1(SLOT->phase, env, (out << SLOT->fb_shift), SLOT->wavetable);
    }

    /* SLOT 2 */
    SLOT++;
    env = volume_calc(SLOT);
    if (env < ENV_QUIET)
        m_out_melody += op_calc(SLOT->phase, env, phase_modulation, SLOT->wavetable);
}

// A melody channel is silent when its CARRIER is EG_OFF and the modulator is
// either off or PARKED in EG_REL — the chip never lets melody modulators
// perform release, so after a key-off they sit in EG_REL forever with the
// carrier gating the output to zero. Requiring both slots EG_OFF here (the
// OPL3 rule) meant allQuiet()/audible() never came true after the first note
// and the mixer's +128 re-centre stayed in the output as permanent DC
// (hw 2026-09-02: OBS meter showed a steady level after playback stopped —
// the plugin's own exit mute is correct). Skipping while the parked
// modulator's feedback would still evolve is exact for the audible output:
// nothing hears it, and a re-key dumps both slots to MAX and resets phase
// before anything becomes audible again.
void OpllFm::chanCalcOrSkip(Chan* CH) {
    Slot* s = CH->SLOT;
    if (s[1].state == EG_OFF &&
        (s[0].state == EG_OFF || s[0].state == EG_REL)) {
        s[0].op1_out[0] = s[0].op1_out[1] = 0;
        return;
    }
    chanCalc(CH);
}

void OpllFm::rhythmCalc(unsigned int noise) {
    /* Bass Drum: op1->op2 with the usual feedback path; out x2 */
    Slot* SLOT = &m_ch[6].SLOT[SLOT1];
    uint32_t env = volume_calc(SLOT);
    int32_t  out = SLOT->op1_out[0] + SLOT->op1_out[1];
    SLOT->op1_out[0] = SLOT->op1_out[1];
    int32_t phase_modulation = SLOT->op1_out[0];
    SLOT->op1_out[1] = 0;
    if (env < ENV_QUIET) {
        if (!SLOT->fb_shift) out = 0;
        SLOT->op1_out[1] = op_calc1(SLOT->phase, env, (out << SLOT->fb_shift), SLOT->wavetable);
    }
    SLOT++;
    env = volume_calc(SLOT);
    if (env < ENV_QUIET)
        m_out_rhythm += op_calc(SLOT->phase, env, phase_modulation, SLOT->wavetable) * 2;

    /* High Hat */
    env = volume_calc(SLOT7_1);
    if (env < ENV_QUIET) {
        uint8_t bit7 = ((SLOT7_1->phase >> FREQ_SH) >> 7) & 1;
        uint8_t bit3 = ((SLOT7_1->phase >> FREQ_SH) >> 3) & 1;
        uint8_t bit2 = ((SLOT7_1->phase >> FREQ_SH) >> 2) & 1;
        uint8_t res1 = (bit2 ^ bit7) | bit3;
        uint32_t phase = res1 ? (0x200 | (0xd0 >> 2)) : 0xd0;

        uint8_t bit5e = ((SLOT8_2->phase >> FREQ_SH) >> 5) & 1;
        uint8_t bit3e = ((SLOT8_2->phase >> FREQ_SH) >> 3) & 1;
        if (bit3e | bit5e)              /* NB: OR on the OPLL (XOR on OPL3) */
            phase = 0x200 | (0xd0 >> 2);

        if (phase & 0x200) {
            if (noise) phase = 0x200 | 0xd0;
        } else {
            if (noise) phase = 0xd0 >> 2;
        }
        m_out_rhythm += op_calc(phase << FREQ_SH, env, 0, SLOT7_1->wavetable) * 2;
    }

    /* Snare Drum */
    env = volume_calc(SLOT7_2);
    if (env < ENV_QUIET) {
        uint8_t bit8 = ((SLOT7_1->phase >> FREQ_SH) >> 8) & 1;
        uint32_t phase = bit8 ? 0x200 : 0x100;
        if (noise) phase ^= 0x100;
        m_out_rhythm += op_calc(phase << FREQ_SH, env, 0, SLOT7_2->wavetable) * 2;
    }

    /* Tom Tom */
    env = volume_calc(SLOT8_1);
    if (env < ENV_QUIET)
        m_out_rhythm += op_calc(SLOT8_1->phase, env, 0, SLOT8_1->wavetable) * 2;

    /* Top Cymbal */
    env = volume_calc(SLOT8_2);
    if (env < ENV_QUIET) {
        uint8_t bit7 = ((SLOT7_1->phase >> FREQ_SH) >> 7) & 1;
        uint8_t bit3 = ((SLOT7_1->phase >> FREQ_SH) >> 3) & 1;
        uint8_t bit2 = ((SLOT7_1->phase >> FREQ_SH) >> 2) & 1;
        uint8_t res1 = (bit2 ^ bit7) | bit3;
        uint32_t phase = res1 ? 0x300 : 0x100;

        uint8_t bit5e = ((SLOT8_2->phase >> FREQ_SH) >> 5) & 1;
        uint8_t bit3e = ((SLOT8_2->phase >> FREQ_SH) >> 3) & 1;
        if (bit3e | bit5e)
            phase = 0x300;

        m_out_rhythm += op_calc(phase << FREQ_SH, env, 0, SLOT8_2->wavetable) * 2;
    }
}

bool OpllFm::allQuiet() const {
    uint8_t rhy = m_rhythm & 0x20;
    for (int i = 0; i < 9; i++) {
        const Slot* s = m_ch[i].SLOT;
        if (s[1].state != EG_OFF)
            return false;
        // melody modulators park in EG_REL forever (see chanCalcOrSkip); in
        // rhythm mode ch6-8 modulators DO release, so they must reach EG_OFF
        bool relAllowed = rhy && i >= 6;
        if (s[0].state != EG_OFF && !(s[0].state == EG_REL && !relAllowed))
            return false;
    }
    return true;
}

// One chip sample (melody + rhythm, doubled like MAME's two output pins
// summed; the x2 keeps a lone OPLL at a level comparable to the OPL3 core).
void OpllFm::renderSample(int32_t& out) {
    advanceLfo();

    m_out_melody = 0;
    m_out_rhythm = 0;

    for (int j = 0; j < 6; j++)
        chanCalcOrSkip(&m_ch[j]);

    if (!(m_rhythm & 0x20)) {
        chanCalcOrSkip(&m_ch[6]);
        chanCalcOrSkip(&m_ch[7]);
        chanCalcOrSkip(&m_ch[8]);
    } else {
        rhythmCalc(m_noise_rng & 1);
    }

    out = (m_out_melody + m_out_rhythm) << 1;

    advance();
}

void OpllFm::gen(int16_t* buf, int count, int bufpos) {
    if (count <= 0) return;

    if (allQuiet()) {
        for (int i = 0; i < 9; i++) {
            m_ch[i].SLOT[0].op1_out[0] = m_ch[i].SLOT[0].op1_out[1] = 0;
            m_ch[i].SLOT[1].op1_out[0] = m_ch[i].SLOT[1].op1_out[1] = 0;
        }
        if (m_quiet_samples < 0x10000000u) m_quiet_samples += count;
        m_prev = m_cur = 0;
        return;
    }
    m_quiet_samples = 0;

    for (int i = 0; i < count; i++) {
        int32_t v;
        if (!m_half) {
            renderSample(v);
        } else if ((m_half_tick ^= 1)) {
            m_prev = m_cur;
            renderSample(m_cur);
            v = (m_prev + m_cur) >> 1;
        } else {
            v = m_cur;
        }
        buf[bufpos + i] = (int16_t)limit(buf[bufpos + i] + v, 32767, -32768);
    }
}
