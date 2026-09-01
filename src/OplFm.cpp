/*

pico-speccy — YMF262 (OPL3) FM synthesis. See OplFm.h for provenance: this is
a port of MAME's ymf262.cpp (Jarek Burczynski, GPL-2.0+, tag mame0220), kept
register-for-register faithful to it. The two big lookup tables live in flash
(OplTabs.h, generated); the small const tables below are copied verbatim.

Everything is deliberately in FLASH, like OpnFm: the OPL3 path only runs while
Config::opl3 is on, and the quiet fast path makes an idle chip nearly free. If
FM playback costs frames on hardware, __not_in_flash("audio") on gen/chanCalc/
advance is the whole change.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include "OplFm.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

// The per-SAMPLE code lives in RAM (.time_critical): even with the write
// queue batching a frame into one contiguous gen() pass, the Z80 core still
// evicts these ~6 KB from the XIP cache between frames, and re-faulting them
// through flash showed up as rare IDL<0 frames with audible clicks on heavy
// Adlib rips (hw 2026-09-01). Register-write code (writeReg, setters) stays
// in flash — it runs in bursts inside the same pass and caches fine.
// Host builds (tools/oplfm_test.cpp) have no pico headers: annotation off.
#if __has_include("pico.h")
#include "pico.h"
#define OPL_HOT __not_in_flash("audio")
#else
#define OPL_HOT
#endif

OplFm* oplfm = nullptr;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── ymf262.cpp constants ────────────────────────────────────────────────────
#define FREQ_SH         16
#define EG_SH           16
#define LFO_SH          24
#define FREQ_MASK       ((1 << FREQ_SH) - 1)

#define ENV_BITS        10
#define ENV_LEN         (1 << ENV_BITS)
#define ENV_STEP        (128.0 / ENV_LEN)

#define MAX_ATT_INDEX   ((1 << (ENV_BITS - 1)) - 1)   // 511
#define MIN_ATT_INDEX   0

#define EG_ATT          4
#define EG_DEC          3
#define EG_SUS          2
#define EG_REL          1
#define EG_OFF          0

#define SIN_BITS        10
#define SIN_LEN         (1 << SIN_BITS)
#define SIN_MASK        (SIN_LEN - 1)

#define TL_RES_LEN      256
#define TL_TAB_LEN      (13 * 2 * TL_RES_LEN)
#define ENV_QUIET       (TL_TAB_LEN >> 4)

#define RATE_STEPS      8

#define SLOT1           0
#define SLOT2           1

// Slot output routing (Slot::conn_enum)
#define CONN_NULL       0
#define CONN_CHAN0      1
#define CONN_PHASEMOD   19
#define CONN_PHASEMOD2  20

// register number (offset & 0x1f) -> slot number
static const int8_t slot_array[32] = {
     0,  2,  4,  1,  3,  5, -1, -1,
     6,  8, 10,  7,  9, 11, -1, -1,
    12, 14, 16, 13, 15, 17, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};

// Key scale level. MAME keeps this as doubles divided by DV = 0.1875/2 and
// casts to uint32 on use; every entry is an exact integer, stored as such
// (KSL(db) = db / 0.09375). Table is 3 dB/octave, ksl_shift makes 0/3/1.5/6.
#define KSL(db) ((uint8_t)((db) * 32.0 / 3.0))
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

// 0 / 3.0 / 1.5 / 6.0 dB/OCT
static const uint32_t ksl_shift[4] = { 31, 1, 2, 0 };

// sustain level table (3 dB per step): 0..42 dB, then 93 dB
#define SC(db) ((uint32_t)((db) * (2.0 / ENV_STEP)))
static const uint32_t sl_tab[16] = {
    SC( 0),SC( 1),SC( 2),SC( 3),SC( 4),SC( 5),SC( 6),SC( 7),
    SC( 8),SC( 9),SC(10),SC(11),SC(12),SC(13),SC(14),SC(31)
};
#undef SC

static const uint8_t eg_inc[15 * RATE_STEPS] = {
/*cycle:0 1  2 3  4 5  6 7*/
/* 0 */ 0,1, 0,1, 0,1, 0,1, /* rates 00..12 0 (increment by 0 or 1) */
/* 1 */ 0,1, 0,1, 1,1, 0,1, /* rates 00..12 1 */
/* 2 */ 0,1, 1,1, 0,1, 1,1, /* rates 00..12 2 */
/* 3 */ 0,1, 1,1, 1,1, 1,1, /* rates 00..12 3 */

/* 4 */ 1,1, 1,1, 1,1, 1,1, /* rate 13 0 (increment by 1) */
/* 5 */ 1,1, 1,2, 1,1, 1,2, /* rate 13 1 */
/* 6 */ 1,2, 1,2, 1,2, 1,2, /* rate 13 2 */
/* 7 */ 1,2, 2,2, 1,2, 2,2, /* rate 13 3 */

/* 8 */ 2,2, 2,2, 2,2, 2,2, /* rate 14 0 (increment by 2) */
/* 9 */ 2,2, 2,4, 2,2, 2,4, /* rate 14 1 */
/*10 */ 2,4, 2,4, 2,4, 2,4, /* rate 14 2 */
/*11 */ 2,4, 4,4, 2,4, 4,4, /* rate 14 3 */

/*12 */ 4,4, 4,4, 4,4, 4,4, /* rates 15 0, 15 1, 15 2, 15 3 for decay */
/*13 */ 8,8, 8,8, 8,8, 8,8, /* rates 15 0, 15 1, 15 2, 15 3 for attack (zero time) */
/*14 */ 0,0, 0,0, 0,0, 0,0, /* infinity rates for attack and decay(s) */
};

#define O(a) ((a) * RATE_STEPS)
static const uint8_t eg_rate_select[16 + 64 + 16] = {
/* 16 infinite time rates */
O(14),O(14),O(14),O(14),O(14),O(14),O(14),O(14),
O(14),O(14),O(14),O(14),O(14),O(14),O(14),O(14),
/* rates 00-12 */
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3),
/* rate 13 */
O( 4),O( 5),O( 6),O( 7),
/* rate 14 */
O( 8),O( 9),O(10),O(11),
/* rate 15 */
O(12),O(12),O(12),O(12),
/* 16 dummy rates (same as 15 3) */
O(12),O(12),O(12),O(12),O(12),O(12),O(12),O(12),
O(12),O(12),O(12),O(12),O(12),O(12),O(12),O(12),
};
#undef O

#define O(a) (a)
static const uint8_t eg_rate_shift[16 + 64 + 16] = {
/* 16 infinite time rates */
O(0),O(0),O(0),O(0),O(0),O(0),O(0),O(0),
O(0),O(0),O(0),O(0),O(0),O(0),O(0),O(0),
/* rates 00-12 */
O(12),O(12),O(12),O(12), O(11),O(11),O(11),O(11),
O(10),O(10),O(10),O(10), O( 9),O( 9),O( 9),O( 9),
O( 8),O( 8),O( 8),O( 8), O( 7),O( 7),O( 7),O( 7),
O( 6),O( 6),O( 6),O( 6), O( 5),O( 5),O( 5),O( 5),
O( 4),O( 4),O( 4),O( 4), O( 3),O( 3),O( 3),O( 3),
O( 2),O( 2),O( 2),O( 2), O( 1),O( 1),O( 1),O( 1),
O( 0),O( 0),O( 0),O( 0),
/* rate 13 */
O( 0),O( 0),O( 0),O( 0),
/* rate 14 */
O( 0),O( 0),O( 0),O( 0),
/* rate 15 */
O( 0),O( 0),O( 0),O( 0),
/* 16 dummy rates */
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),O( 0),
};
#undef O

// multiple table: 1/2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15
#define ML 2
static const uint8_t mul_tab[16] = {
    ML/2, 1*ML, 2*ML, 3*ML, 4*ML, 5*ML, 6*ML, 7*ML,
    8*ML, 9*ML,10*ML,10*ML,12*ML,12*ML,15*ML,15*ML
};
#undef ML

// LFO Amplitude Modulation table (verified on real YM3812): 27 output levels,
// triangle; one entry lasts 64 samples, whole table 64*210 samples.
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

// LFO Phase Modulation table (verified on real YM3812)
static const int8_t lfo_pm_table[8 * 8 * 2] = {
/* FNUM2/FNUM = 00 0xxxxxxx (0x0000) */
0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
/* FNUM2/FNUM = 00 1xxxxxxx (0x0080) */
0, 0, 0, 0, 0, 0, 0, 0,   1, 0, 0, 0,-1, 0, 0, 0,
/* FNUM2/FNUM = 01 0xxxxxxx (0x0100) */
1, 0, 0, 0,-1, 0, 0, 0,   2, 1, 0,-1,-2,-1, 0, 1,
/* FNUM2/FNUM = 01 1xxxxxxx (0x0180) */
1, 0, 0, 0,-1, 0, 0, 0,   3, 1, 0,-1,-3,-1, 0, 1,
/* FNUM2/FNUM = 10 0xxxxxxx (0x0200) */
2, 1, 0,-1,-2,-1, 0, 1,   4, 2, 0,-2,-4,-2, 0, 2,
/* FNUM2/FNUM = 10 1xxxxxxx (0x0280) */
2, 1, 0,-1,-2,-1, 0, 1,   5, 2, 0,-2,-5,-2, 0, 2,
/* FNUM2/FNUM = 11 0xxxxxxx (0x0300) */
3, 1, 0,-1,-3,-1, 0, 1,   6, 3, 0,-3,-6,-3, 0, 3,
/* FNUM2/FNUM = 11 1xxxxxxx (0x0380) */
3, 1, 0,-1,-3,-1, 0, 1,   7, 3, 0,-3,-7,-3, 0, 3
};

// ── shared tables (heap, reference counted — see OplFm.h) ───────────────────
// tl_base: the x = 0..255 row of MAME's tl_tab; the other 12 rows are that row
// shifted right, with the sign baked in as one's complement — re-applied at
// the fetch. sin0: waveform 0 in the chip's logarithmic domain (value*2 +
// sign bit, TL_TAB_LEN = "silent"); waveforms 1-7 are index transforms of it.
// Byte-for-byte MAME init_tables() math; verified bit-exact against the flat
// tables over every (p, wave, index).
static int       s_tab_refs = 0;
static uint16_t* s_tl_base = nullptr;   // 256 entries
static uint16_t* s_sin0    = nullptr;   // SIN_LEN entries

static void opl_build_tables() {
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
        s_tl_base[x] = (uint16_t)(n << 1);
    }
    for (int i = 0; i < SIN_LEN; i++) {
        double m = sin(((i * 2) + 1) * M_PI / SIN_LEN);
        double o = 8 * log((m > 0.0 ? 1.0 : -1.0) / m) / log(2.0);
        o = o / (ENV_STEP / 4);
        int n = (int)(2.0 * o);
        n = (n & 1) ? (n >> 1) + 1 : n >> 1;
        s_sin0[i] = (uint16_t)(n * 2 + (m >= 0.0 ? 0 : 1));
    }
}

static void opl_free_tables() {
    if (--s_tab_refs > 0) return;
    free(s_tl_base); s_tl_base = nullptr;
    free(s_sin0);    s_sin0 = nullptr;
}

bool OplFm::tablesReady() { return s_tl_base && s_sin0; }

static inline int limit(int val, int max, int min) {
    if (val > max) return max;
    if (val < min) return min;
    return val;
}

// ── construction ────────────────────────────────────────────────────────────

OplFm::OplFm() {
    // Standard-layout, no vtable, all-POD members: zero the lot, then the
    // subsystem calls setRates() + reset() before the first write.
    memset((void*)this, 0, sizeof(*this));
    opl_build_tables();
}

OplFm::~OplFm() {
    opl_free_tables();
}

void OplFm::setRates(int clock, int rate, bool halfRate) {
    m_clock = clock;
    m_rate  = rate;
    m_half  = halfRate ? 1 : 0;

    // Every synthesis constant derives from the SYNTH rate (rate/2 in
    // half-rate mode — that is what keeps the pitch exact); only the timers
    // below stay in real output time.
    int synth_rate = halfRate ? rate / 2 : rate;

    // divider is 8*36 = 288 for a YMF262 (chip sample rate ~49716 Hz)
    double freqbase = synth_rate ? ((double)clock / 288.0) / (double)synth_rate : 0.0;

    // fnumber -> phase increment (chip works in 10.10, we use 16.16)
    for (int i = 0; i < 1024; i++)
        m_fn_tab[i] = (uint32_t)((double)i * 64 * freqbase * (1 << (FREQ_SH - 10)));

    // AM: one lfo_am_table entry lasts 64 chip samples; PM: one level = 1024
    m_lfo_am_inc = (uint32_t)((1.0 / 64.0)   * (1 << LFO_SH) * freqbase);
    m_lfo_pm_inc = (uint32_t)((1.0 / 1024.0) * (1 << LFO_SH) * freqbase);

    // noise shifts once per chip sample
    m_noise_f = (uint32_t)((1 << FREQ_SH) * freqbase);

    m_eg_timer_add      = (uint32_t)((1 << EG_SH) * freqbase);
    m_eg_timer_overflow = 1 << EG_SH;

    // timers count chip samples (T1 unit = 4 of them = 80.4 us), and they are
    // advanced once per OUTPUT sample — real time, independent of half-rate
    m_timer_step_q16 = (uint32_t)((rate ? ((double)clock / 288.0) / (double)rate : 0.0) * 65536.0);
}

// ── status / IRQ ────────────────────────────────────────────────────────────

void OplFm::statusSet(int flag) {
    m_status |= (flag & m_statusmask);
    if (!(m_status & 0x80)) {
        if (m_status & 0x7f)
            m_status |= 0x80;   // no IRQ line to raise on this card
    }
}

void OplFm::statusReset(int flag) {
    m_status &= ~flag;
    if (m_status & 0x80) {
        if (!(m_status & 0x7f))
            m_status &= 0x7f;
    }
}

void OplFm::statusMaskSet(int flag) {
    m_statusmask = flag;
    statusSet(0);
    statusReset(0);
}

// ── timers (MAME uses attotime callbacks; we count Q16 chip samples) ────────

void OplFm::timerOver(int c) {
    statusSet(c ? 0x20 : 0x40);
}

OPL_HOT void OplFm::runTimers(int samples) {
    for (int c = 0; c < 2; c++) {
        if (!m_st[c]) continue;
        m_Tcnt[c] -= (int64_t)samples * m_timer_step_q16;
        while (m_Tcnt[c] <= 0) {
            timerOver(c);
            int32_t period = m_T[c] > 0 ? m_T[c] : 1;
            m_Tcnt[c] += (int64_t)period << 16;   // reload, as real hw does
        }
    }
}

// ── LFO / EG / phase advance, per output sample ─────────────────────────────

OPL_HOT void OplFm::advanceLfo() {
    m_lfo_am_cnt += m_lfo_am_inc;
    if (m_lfo_am_cnt >= ((uint32_t)LFO_AM_TAB_ELEMENTS << LFO_SH))
        m_lfo_am_cnt -= ((uint32_t)LFO_AM_TAB_ELEMENTS << LFO_SH);

    uint8_t tmp = lfo_am_table[m_lfo_am_cnt >> LFO_SH];
    m_LFO_AM = m_lfo_am_depth ? tmp : (tmp >> 2);

    m_lfo_pm_cnt += m_lfo_pm_inc;
    m_LFO_PM = ((m_lfo_pm_cnt >> LFO_SH) & 7) | m_lfo_pm_depth_range;
}

OPL_HOT void OplFm::advance() {
    m_eg_timer += m_eg_timer_add;

    while (m_eg_timer >= m_eg_timer_overflow) {
        m_eg_timer -= m_eg_timer_overflow;
        m_eg_cnt++;

        if (m_eg_cnt < m_eg_next && !m_eg_dirty)
            continue;               // provably no slot can act on this tick

        // The walk also recomputes m_eg_next inline — the soonest eg_cnt at
        // which any slot's rate mask can match again. Slots that cannot act
        // are excluded: EG_OFF, sustain that holds (non-percussive, or
        // percussive already clamped at maximum attenuation — nothing but a
        // register write, which sets m_eg_dirty, can move those again), and
        // the all-zero "infinity" eg_inc row. A separate 36-slot recompute
        // pass doubled the cost of exactly the walk-heavy frames (hw
        // 2026-09-01, Doom clicks).
        uint32_t eg_next = 0xFFFFFFFFu;

        for (int i = 0; i < 9 * 2 * 2; i++) {
            Chan* CH = &m_ch[i / 2];
            Slot* op = &CH->SLOT[i & 1];

            switch (op->state) {
            case EG_ATT:    /* attack phase */
                if (!(m_eg_cnt & op->eg_m_ar)) {
                    op->volume += (~op->volume *
                        (eg_inc[op->eg_sel_ar + ((m_eg_cnt >> op->eg_sh_ar) & 7)])) >> 3;
                    if (op->volume <= MIN_ATT_INDEX) {
                        op->volume = MIN_ATT_INDEX;
                        op->state = EG_DEC;
                    }
                }
                break;

            case EG_DEC:    /* decay phase */
                if (!(m_eg_cnt & op->eg_m_dr)) {
                    op->volume += eg_inc[op->eg_sel_dr + ((m_eg_cnt >> op->eg_sh_dr) & 7)];
                    if (op->volume >= (int32_t)op->sl)
                        op->state = EG_SUS;
                }
                break;

            case EG_SUS:    /* sustain phase */
                // percussive/non-percussive can change on the fly and the chip
                // stays in sustain — verified on real YM3812 (MAME comment)
                if (op->eg_type) {
                    /* non-percussive: do nothing */
                } else {
                    /* percussive: sustain adds the release rate */
                    if (!(m_eg_cnt & op->eg_m_rr)) {
                        op->volume += eg_inc[op->eg_sel_rr + ((m_eg_cnt >> op->eg_sh_rr) & 7)];
                        if (op->volume >= MAX_ATT_INDEX)
                            op->volume = MAX_ATT_INDEX;
                    }
                }
                break;

            case EG_REL:    /* release phase */
                if (!(m_eg_cnt & op->eg_m_rr)) {
                    op->volume += eg_inc[op->eg_sel_rr + ((m_eg_cnt >> op->eg_sh_rr) & 7)];
                    if (op->volume >= MAX_ATT_INDEX) {
                        op->volume = MAX_ATT_INDEX;
                        op->state = EG_OFF;
                    }
                }
                break;

            default:
                break;
            }

            // inline next-fire bound for this slot's (possibly new) state
            uint8_t  nsh;
            uint16_t nsel;
            switch (op->state) {
            case EG_ATT: nsh = op->eg_sh_ar; nsel = op->eg_sel_ar; break;
            case EG_DEC: nsh = op->eg_sh_dr; nsel = op->eg_sel_dr; break;
            case EG_SUS:
                if (op->eg_type || op->volume >= MAX_ATT_INDEX) continue;
                nsh = op->eg_sh_rr; nsel = op->eg_sel_rr; break;
            case EG_REL: nsh = op->eg_sh_rr; nsel = op->eg_sel_rr; break;
            default:     continue;          // EG_OFF
            }
            if (nsel == 14 * RATE_STEPS) continue;   // infinity row: never acts
            uint32_t nx = ((m_eg_cnt >> nsh) + 1) << nsh;
            if (nx < eg_next) eg_next = nx;
        }

        m_eg_next = eg_next;
        m_eg_dirty = 0;
    }

    uint8_t rhy = m_rhythm & 0x20;
    for (int i = 0; i < 9 * 2 * 2; i++) {
        Chan* CH = &m_ch[i / 2];
        Slot* op = &CH->SLOT[i & 1];

        /* A silent operator's phase is unobservable: key-on restarts Cnt at 0
           and every output of an EG_OFF slot is gated off — EXCEPT in rhythm
           mode, where HH/SD/TOP derive their phase from ch7 slot1 / ch8 slot2
           regardless of those slots' own envelopes, so ch6-8 keep counting. */
        if (op->state == EG_OFF && !(rhy && i >= 12 && i < 18))
            continue;

        /* Phase Generator */
        if (op->vib) {
            uint32_t block_fnum = CH->block_fnum;
            uint32_t fnum_lfo   = (block_fnum & 0x0380) >> 7;
            int32_t  lfo_fn_table_index_offset = lfo_pm_table[m_LFO_PM + 16 * fnum_lfo];

            if (lfo_fn_table_index_offset) {  /* LFO phase modulation active */
                block_fnum += lfo_fn_table_index_offset;
                uint8_t block = (block_fnum & 0x1c00) >> 10;
                op->Cnt += (m_fn_tab[block_fnum & 0x03ff] >> (7 - block)) * op->mul;
            } else {
                op->Cnt += op->Incr;
            }
        } else {
            op->Cnt += op->Incr;
        }
    }

    /* 23-bit noise shift register, one shift per chip sample; MAME's trick:
       bit 0 is the output and the XOR taps are folded into one constant. */
    m_noise_p += m_noise_f;
    int i = m_noise_p >> FREQ_SH;
    m_noise_p &= FREQ_MASK;
    while (i) {
        if (m_noise_rng & 1) m_noise_rng ^= 0x800302;
        m_noise_rng >>= 1;
        i--;
    }
}

// ── operator output ─────────────────────────────────────────────────────────

// sin_tab[wave][i] re-derived from waveform 0 (bit-exact vs the flat table)
static inline uint32_t sin_fetch(uint32_t wave, uint32_t i) {
    switch (wave) {
    default: return s_sin0[i];
    case 1:  return (i & 512) ? TL_TAB_LEN : s_sin0[i];
    case 2:  return s_sin0[i & 511];
    case 3:  return (i & 256) ? TL_TAB_LEN : s_sin0[i & 255];
    case 4:  return (i & 512) ? TL_TAB_LEN : s_sin0[(i * 2) & 1023];
    case 5:  return (i & 512) ? TL_TAB_LEN : s_sin0[(i * 2) & 511];
    case 6:  return (i & 512) ? 1 : 0;
    case 7: {
        uint32_t x = (i & 512) ? ((1023 - i) * 16 + 1) : i * 16;
        return x > TL_TAB_LEN ? TL_TAB_LEN : x;
    }
    }
}

// tl_tab[p]: index = row (p>>9) | base entry ((p&511)>>1) | sign (p&1)
static inline int32_t tl_fetch(uint32_t p) {
    int32_t v = s_tl_base[(p & 511) >> 1] >> (p >> 9);
    return (p & 1) ? ~v : v;
}

static inline int32_t op_calc(uint32_t phase, uint32_t env, int32_t pm, uint32_t wave) {
    uint32_t p = (env << 4) +
        sin_fetch(wave, (((int32_t)((phase & ~FREQ_MASK) + (pm << 16))) >> FREQ_SH) & SIN_MASK);
    if (p >= TL_TAB_LEN) return 0;
    return tl_fetch(p);
}

static inline int32_t op_calc1(uint32_t phase, uint32_t env, int32_t pm, uint32_t wave) {
    uint32_t p = (env << 4) +
        sin_fetch(wave, (((int32_t)((phase & ~FREQ_MASK) + pm)) >> FREQ_SH) & SIN_MASK);
    if (p >= TL_TAB_LEN) return 0;
    return tl_fetch(p);
}

#define volume_calc(OP) ((OP)->TLL + ((uint32_t)(OP)->volume) + (m_LFO_AM & (OP)->AMmask))

/* standard 2-operator channel (or 1st half of a 4-op channel) */
OPL_HOT void OplFm::chanCalc(Chan* CH) {
    m_phase_modulation  = 0;
    m_phase_modulation2 = 0;

    /* SLOT 1 */
    Slot* SLOT = &CH->SLOT[SLOT1];
    uint32_t env = volume_calc(SLOT);
    int32_t  out = SLOT->op1_out[0] + SLOT->op1_out[1];
    SLOT->op1_out[0] = SLOT->op1_out[1];
    SLOT->op1_out[1] = 0;
    if (env < ENV_QUIET) {
        if (!SLOT->FB) out = 0;
        SLOT->op1_out[1] = op_calc1(SLOT->Cnt, env, (out << SLOT->FB), SLOT->wavetable);
    }
    if (SLOT->connect)
        *SLOT->connect += SLOT->op1_out[1];

    /* SLOT 2 */
    SLOT++;
    env = volume_calc(SLOT);
    if ((env < ENV_QUIET) && SLOT->connect)
        *SLOT->connect += op_calc(SLOT->Cnt, env, m_phase_modulation, SLOT->wavetable);
}

/* 2nd half of a 4-op channel */
OPL_HOT void OplFm::chanCalcExt(Chan* CH) {
    m_phase_modulation = 0;

    Slot* SLOT = &CH->SLOT[SLOT1];
    uint32_t env = volume_calc(SLOT);
    if (env < ENV_QUIET && SLOT->connect)
        *SLOT->connect += op_calc(SLOT->Cnt, env, m_phase_modulation2, SLOT->wavetable);

    SLOT++;
    env = volume_calc(SLOT);
    if (env < ENV_QUIET && SLOT->connect)
        *SLOT->connect += op_calc(SLOT->Cnt, env, m_phase_modulation, SLOT->wavetable);
}

#define SLOT7_1 (&m_ch[7].SLOT[SLOT1])
#define SLOT7_2 (&m_ch[7].SLOT[SLOT2])
#define SLOT8_1 (&m_ch[8].SLOT[SLOT1])
#define SLOT8_2 (&m_ch[8].SLOT[SLOT2])

/* rhythm mode: BD/HH/SD/TOM/TOP on channels 6-8, phase quirks as verified
   on real YM3812 (see ymf262.cpp for the full commentary) */
OPL_HOT void OplFm::chanCalcRhythm(unsigned int noise) {
    /* Bass Drum: connect=0 -> op1->op2->out, connect=1 -> op2 only; out x2 */
    m_phase_modulation = 0;

    Slot* SLOT = &m_ch[6].SLOT[SLOT1];
    uint32_t env = volume_calc(SLOT);
    int32_t  out = SLOT->op1_out[0] + SLOT->op1_out[1];
    SLOT->op1_out[0] = SLOT->op1_out[1];
    if (!SLOT->CON)
        m_phase_modulation = SLOT->op1_out[0];
    /* else ignore output of operator 1 */
    SLOT->op1_out[1] = 0;
    if (env < ENV_QUIET) {
        if (!SLOT->FB) out = 0;
        SLOT->op1_out[1] = op_calc1(SLOT->Cnt, env, (out << SLOT->FB), SLOT->wavetable);
    }

    SLOT++;
    env = volume_calc(SLOT);
    if (env < ENV_QUIET)
        m_chanout[6] += op_calc(SLOT->Cnt, env, m_phase_modulation, SLOT->wavetable) * 2;

    /* High Hat */
    env = volume_calc(SLOT7_1);
    if (env < ENV_QUIET) {
        /* base frequency derived from operator 1 in channel 7 */
        uint8_t bit7 = ((SLOT7_1->Cnt >> FREQ_SH) >> 7) & 1;
        uint8_t bit3 = ((SLOT7_1->Cnt >> FREQ_SH) >> 3) & 1;
        uint8_t bit2 = ((SLOT7_1->Cnt >> FREQ_SH) >> 2) & 1;
        uint8_t res1 = (bit2 ^ bit7) | bit3;
        uint32_t phase = res1 ? (0x200 | (0xd0 >> 2)) : 0xd0;

        /* enable gate based on frequency of operator 2 in channel 8 */
        uint8_t bit5e = ((SLOT8_2->Cnt >> FREQ_SH) >> 5) & 1;
        uint8_t bit3e = ((SLOT8_2->Cnt >> FREQ_SH) >> 3) & 1;
        if (bit3e ^ bit5e)
            phase = 0x200 | (0xd0 >> 2);

        if (phase & 0x200) {
            if (noise) phase = 0x200 | 0xd0;
        } else {
            if (noise) phase = 0xd0 >> 2;
        }

        m_chanout[7] += op_calc(phase << FREQ_SH, env, 0, SLOT7_1->wavetable) * 2;
    }

    /* Snare Drum */
    env = volume_calc(SLOT7_2);
    if (env < ENV_QUIET) {
        uint8_t bit8 = ((SLOT7_1->Cnt >> FREQ_SH) >> 8) & 1;
        uint32_t phase = bit8 ? 0x200 : 0x100;
        if (noise) phase ^= 0x100;
        m_chanout[7] += op_calc(phase << FREQ_SH, env, 0, SLOT7_2->wavetable) * 2;
    }

    /* Tom Tom */
    env = volume_calc(SLOT8_1);
    if (env < ENV_QUIET)
        m_chanout[8] += op_calc(SLOT8_1->Cnt, env, 0, SLOT8_1->wavetable) * 2;

    /* Top Cymbal */
    env = volume_calc(SLOT8_2);
    if (env < ENV_QUIET) {
        uint8_t bit7 = ((SLOT7_1->Cnt >> FREQ_SH) >> 7) & 1;
        uint8_t bit3 = ((SLOT7_1->Cnt >> FREQ_SH) >> 3) & 1;
        uint8_t bit2 = ((SLOT7_1->Cnt >> FREQ_SH) >> 2) & 1;
        uint8_t res1 = (bit2 ^ bit7) | bit3;
        uint32_t phase = res1 ? 0x300 : 0x100;

        uint8_t bit5e = ((SLOT8_2->Cnt >> FREQ_SH) >> 5) & 1;
        uint8_t bit3e = ((SLOT8_2->Cnt >> FREQ_SH) >> 3) & 1;
        if (bit3e ^ bit5e)
            phase = 0x300;

        m_chanout[8] += op_calc(phase << FREQ_SH, env, 0, SLOT8_2->wavetable) * 2;
    }
}

// ── key on/off and slot parameter setters ───────────────────────────────────

void OplFm::keyOn(Slot* SLOT, uint32_t key_set) {
    if (!SLOT->key) {
        SLOT->Cnt = 0;          /* restart Phase Generator */
        SLOT->state = EG_ATT;   /* phase -> Attack */
    }
    SLOT->key |= key_set;
}

void OplFm::keyOff(Slot* SLOT, uint32_t key_clr) {
    if (SLOT->key) {
        SLOT->key &= key_clr;
        if (!SLOT->key) {
            if (SLOT->state > EG_REL)
                SLOT->state = EG_REL;   /* phase -> Release */
        }
    }
}

void OplFm::slotConnect(Slot* s) {
    if (s->conn_enum == CONN_NULL)
        s->connect = nullptr;
    else if (s->conn_enum >= CONN_CHAN0 && s->conn_enum < CONN_PHASEMOD)
        s->connect = &m_chanout[s->conn_enum - CONN_CHAN0];
    else if (s->conn_enum == CONN_PHASEMOD)
        s->connect = &m_phase_modulation;
    else if (s->conn_enum == CONN_PHASEMOD2)
        s->connect = &m_phase_modulation2;
}

/* update phase increment counter of operator (and the EG rates if needed) */
void OplFm::calcFcSlot(Chan* CH, Slot* SLOT) {
    SLOT->Incr = CH->fc * SLOT->mul;
    int ksr = CH->kcode >> SLOT->KSR;

    if (SLOT->ksr != ksr) {
        SLOT->ksr = ksr;

        if ((SLOT->ar + SLOT->ksr) < 16 + 60) {
            SLOT->eg_sh_ar  = eg_rate_shift [SLOT->ar + SLOT->ksr];
            SLOT->eg_m_ar   = (1u << SLOT->eg_sh_ar) - 1;
            SLOT->eg_sel_ar = eg_rate_select[SLOT->ar + SLOT->ksr];
        } else {
            SLOT->eg_sh_ar  = 0;
            SLOT->eg_m_ar   = (1u << SLOT->eg_sh_ar) - 1;
            SLOT->eg_sel_ar = 13 * RATE_STEPS;
        }
        SLOT->eg_sh_dr  = eg_rate_shift [SLOT->dr + SLOT->ksr];
        SLOT->eg_m_dr   = (1u << SLOT->eg_sh_dr) - 1;
        SLOT->eg_sel_dr = eg_rate_select[SLOT->dr + SLOT->ksr];
        SLOT->eg_sh_rr  = eg_rate_shift [SLOT->rr + SLOT->ksr];
        SLOT->eg_m_rr   = (1u << SLOT->eg_sh_rr) - 1;
        SLOT->eg_sel_rr = eg_rate_select[SLOT->rr + SLOT->ksr];
    }
}

/* set multi, am, vib, EG-TYP, KSR, mul. In OPL3 mode the 2nd channel of an
   active 4-op pair takes its frequency data from the 1st. */
void OplFm::setMul(int slot, int v) {
    Chan* CH   = &m_ch[slot / 2];
    Slot* SLOT = &CH->SLOT[slot & 1];

    SLOT->mul     = mul_tab[v & 0x0f];
    SLOT->KSR     = (v & 0x10) ? 0 : 2;
    SLOT->eg_type = (v & 0x20);
    SLOT->vib     = (v & 0x40);
    SLOT->AMmask  = (v & 0x80) ? ~0u : 0;

    if (m_OPL3_mode & 1) {
        int chan_no = slot / 2;
        switch (chan_no) {
        case 3: case 4: case 5:
        case 12: case 13: case 14:
            if ((CH - 3)->extended)
                calcFcSlot(CH - 3, SLOT);  /* freq data of 1st channel of the pair */
            else
                calcFcSlot(CH, SLOT);
            break;
        default:
            calcFcSlot(CH, SLOT);
            break;
        }
    } else {
        calcFcSlot(CH, SLOT);
    }
}

/* set ksl & tl */
void OplFm::setKslTl(int slot, int v) {
    Chan* CH   = &m_ch[slot / 2];
    Slot* SLOT = &CH->SLOT[slot & 1];

    SLOT->ksl = ksl_shift[v >> 6];
    SLOT->TL  = (v & 0x3f) << (ENV_BITS - 1 - 7);   /* 7 bits TL (bit 6 = always 0) */

    if (m_OPL3_mode & 1) {
        int chan_no = slot / 2;
        switch (chan_no) {
        case 3: case 4: case 5:
        case 12: case 13: case 14:
            if ((CH - 3)->extended)
                SLOT->TLL = SLOT->TL + ((CH - 3)->ksl_base >> SLOT->ksl);
            else
                SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);
            break;
        default:
            SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);
            break;
        }
    } else {
        SLOT->TLL = SLOT->TL + (CH->ksl_base >> SLOT->ksl);
    }
}

/* set attack rate & decay rate */
void OplFm::setArDr(int slot, int v) {
    Chan* CH   = &m_ch[slot / 2];
    Slot* SLOT = &CH->SLOT[slot & 1];

    SLOT->ar = (v >> 4) ? 16 + ((v >> 4) << 2) : 0;

    /* verified on real YMF262: all 15 x attack rates take "zero" time */
    if ((SLOT->ar + SLOT->ksr) < 16 + 60) {
        SLOT->eg_sh_ar  = eg_rate_shift [SLOT->ar + SLOT->ksr];
        SLOT->eg_m_ar   = (1u << SLOT->eg_sh_ar) - 1;
        SLOT->eg_sel_ar = eg_rate_select[SLOT->ar + SLOT->ksr];
    } else {
        SLOT->eg_sh_ar  = 0;
        SLOT->eg_m_ar   = (1u << SLOT->eg_sh_ar) - 1;
        SLOT->eg_sel_ar = 13 * RATE_STEPS;
    }

    SLOT->dr        = (v & 0x0f) ? 16 + ((v & 0x0f) << 2) : 0;
    SLOT->eg_sh_dr  = eg_rate_shift [SLOT->dr + SLOT->ksr];
    SLOT->eg_m_dr   = (1u << SLOT->eg_sh_dr) - 1;
    SLOT->eg_sel_dr = eg_rate_select[SLOT->dr + SLOT->ksr];
}

/* set sustain level & release rate */
void OplFm::setSlRr(int slot, int v) {
    Chan* CH   = &m_ch[slot / 2];
    Slot* SLOT = &CH->SLOT[slot & 1];

    SLOT->sl        = sl_tab[v >> 4];
    SLOT->rr        = (v & 0x0f) ? 16 + ((v & 0x0f) << 2) : 0;
    SLOT->eg_sh_rr  = eg_rate_shift [SLOT->rr + SLOT->ksr];
    SLOT->eg_m_rr   = (1u << SLOT->eg_sh_rr) - 1;
    SLOT->eg_sel_rr = eg_rate_select[SLOT->rr + SLOT->ksr];
}

// ── register write ──────────────────────────────────────────────────────────

void OplFm::writeReg(int r, int v) {
    m_eg_dirty = 1;
    Chan* CH;
    unsigned int ch_offset = 0;
    int slot;
    uint32_t block_fnum;

    if (r & 0x100) {
        switch (r) {
        case 0x101:     /* test register */
            return;

        case 0x104:     /* 4-op channel pairing enable, 6 bits */
            CH = &m_ch[0];
            CH[0].extended = (v >> 0) & 1;
            CH[1].extended = (v >> 1) & 1;
            CH[2].extended = (v >> 2) & 1;
            CH = &m_ch[9];
            CH[0].extended = (v >> 3) & 1;
            CH[1].extended = (v >> 4) & 1;
            CH[2].extended = (v >> 5) & 1;
            return;

        case 0x105:     /* OPL3 extensions enable */
            /* Verified on real YMF262 (MAME): switching modes on the fly
               keeps the selected waveforms, the c0-c8 output selectors, the
               upper 9 channels and the 4-op pairings as they are. */
            m_OPL3_mode = v & 0x01;
            return;

        default:
            break;
        }
        ch_offset = 9;  /* register page #2 starts from channel 9 */
    }

    r &= 0xff;
    v &= 0xff;

    switch (r & 0xe0) {
    case 0x00:  /* 00-1f: control */
        switch (r & 0x1f) {
        case 0x01:  /* test register */
            break;
        case 0x02:  /* Timer 1 */
            m_T[0] = (256 - v) * 4;
            break;
        case 0x03:  /* Timer 2 */
            m_T[1] = (256 - v) * 16;
            break;
        case 0x04:  /* IRQ clear / mask and timer enable */
            if (v & 0x80) {
                /* IRQ flags clear */
                statusReset(0x60);
            } else {
                /* IRQRST,T1MSK,T2MSK,x,x,x,ST2,ST1 */
                uint8_t st1 = v & 1;
                uint8_t st2 = (v >> 1) & 1;

                statusReset(v & 0x60);
                statusMaskSet((~v) & 0x60);

                if (m_st[1] != st2) {
                    m_st[1] = st2;
                    if (st2) m_Tcnt[1] = (int64_t)m_T[1] << 16;
                }
                if (m_st[0] != st1) {
                    m_st[0] = st1;
                    if (st1) m_Tcnt[0] = (int64_t)m_T[0] << 16;
                }
            }
            break;
        case 0x08:  /* x,NTS,x,x,x,x,x,x */
            m_nts = v;
            break;
        default:
            break;
        }
        break;

    case 0x20:  /* am ON, vib ON, ksr, eg_type, mul */
        slot = slot_array[r & 0x1f];
        if (slot < 0) return;
        setMul(slot + ch_offset * 2, v);
        break;

    case 0x40:
        slot = slot_array[r & 0x1f];
        if (slot < 0) return;
        setKslTl(slot + ch_offset * 2, v);
        break;

    case 0x60:
        slot = slot_array[r & 0x1f];
        if (slot < 0) return;
        setArDr(slot + ch_offset * 2, v);
        break;

    case 0x80:
        slot = slot_array[r & 0x1f];
        if (slot < 0) return;
        setSlRr(slot + ch_offset * 2, v);
        break;

    case 0xa0:
        if (r == 0xbd) {    /* am depth, vibrato depth, r,bd,sd,tom,tc,hh */
            if (ch_offset != 0)     /* 0xbd is present in set #1 only */
                return;

            m_lfo_am_depth       = v & 0x80;
            m_lfo_pm_depth_range = (v & 0x40) ? 8 : 0;
            m_rhythm             = v & 0x3f;

            if (m_rhythm & 0x20) {
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
                keyOff(&m_ch[6].SLOT[SLOT1], ~2u); keyOff(&m_ch[6].SLOT[SLOT2], ~2u);
                keyOff(&m_ch[7].SLOT[SLOT1], ~2u); keyOff(&m_ch[7].SLOT[SLOT2], ~2u);
                keyOff(&m_ch[8].SLOT[SLOT1], ~2u); keyOff(&m_ch[8].SLOT[SLOT2], ~2u);
            }
            return;
        }

        /* keyon, block, fnum */
        if ((r & 0x0f) > 8) return;
        CH = &m_ch[(r & 0x0f) + ch_offset];

        if (!(r & 0x10)) {  /* a0-a8 */
            block_fnum = (CH->block_fnum & 0x1f00) | v;
        } else {            /* b0-b8 */
            block_fnum = ((v & 0x1f) << 8) | (CH->block_fnum & 0xff);

            if (m_OPL3_mode & 1) {
                int chan_no = (r & 0x0f) + ch_offset;
                switch (chan_no) {
                case 0: case 1: case 2:
                case 9: case 10: case 11:
                    if (CH->extended) {
                        /* 1st channel of a 4-op pair: key all FOUR slots */
                        if (v & 0x20) {
                            keyOn(&CH->SLOT[SLOT1], 1); keyOn(&CH->SLOT[SLOT2], 1);
                            keyOn(&(CH + 3)->SLOT[SLOT1], 1); keyOn(&(CH + 3)->SLOT[SLOT2], 1);
                        } else {
                            keyOff(&CH->SLOT[SLOT1], ~1u); keyOff(&CH->SLOT[SLOT2], ~1u);
                            keyOff(&(CH + 3)->SLOT[SLOT1], ~1u); keyOff(&(CH + 3)->SLOT[SLOT2], ~1u);
                        }
                    } else {
                        if (v & 0x20) { keyOn(&CH->SLOT[SLOT1], 1); keyOn(&CH->SLOT[SLOT2], 1); }
                        else          { keyOff(&CH->SLOT[SLOT1], ~1u); keyOff(&CH->SLOT[SLOT2], ~1u); }
                    }
                    break;
                case 3: case 4: case 5:
                case 12: case 13: case 14:
                    if ((CH - 3)->extended) {
                        /* 2nd channel of a 4-op pair: do nothing */
                    } else {
                        if (v & 0x20) { keyOn(&CH->SLOT[SLOT1], 1); keyOn(&CH->SLOT[SLOT2], 1); }
                        else          { keyOff(&CH->SLOT[SLOT1], ~1u); keyOff(&CH->SLOT[SLOT2], ~1u); }
                    }
                    break;
                default:
                    if (v & 0x20) { keyOn(&CH->SLOT[SLOT1], 1); keyOn(&CH->SLOT[SLOT2], 1); }
                    else          { keyOff(&CH->SLOT[SLOT1], ~1u); keyOff(&CH->SLOT[SLOT2], ~1u); }
                    break;
                }
            } else {
                if (v & 0x20) { keyOn(&CH->SLOT[SLOT1], 1); keyOn(&CH->SLOT[SLOT2], 1); }
                else          { keyOff(&CH->SLOT[SLOT1], ~1u); keyOff(&CH->SLOT[SLOT2], ~1u); }
            }
        }

        if (CH->block_fnum != block_fnum) {
            uint8_t block = block_fnum >> 10;

            CH->block_fnum = block_fnum;
            CH->ksl_base   = ksl_tab[block_fnum >> 6];
            CH->fc         = m_fn_tab[block_fnum & 0x03ff] >> (7 - block);

            /* BLK 2,1,0 bits -> bits 3,2,1 of kcode; NTS picks the lsb —
               opposite to the manuals, verified on real YMF262 (MAME) */
            CH->kcode = (CH->block_fnum & 0x1c00) >> 9;
            if (m_nts & 0x40)
                CH->kcode |= (CH->block_fnum & 0x100) >> 8;   /* notesel == 1 */
            else
                CH->kcode |= (CH->block_fnum & 0x200) >> 9;   /* notesel == 0 */

            if (m_OPL3_mode & 1) {
                int chan_no = (r & 0x0f) + ch_offset;
                switch (chan_no) {
                case 0: case 1: case 2:
                case 9: case 10: case 11:
                    if (CH->extended) {
                        /* refresh TL and fc in all FOUR slots using THIS channel's data */
                        CH->SLOT[SLOT1].TLL = CH->SLOT[SLOT1].TL + (CH->ksl_base >> CH->SLOT[SLOT1].ksl);
                        CH->SLOT[SLOT2].TLL = CH->SLOT[SLOT2].TL + (CH->ksl_base >> CH->SLOT[SLOT2].ksl);
                        (CH + 3)->SLOT[SLOT1].TLL = (CH + 3)->SLOT[SLOT1].TL + (CH->ksl_base >> (CH + 3)->SLOT[SLOT1].ksl);
                        (CH + 3)->SLOT[SLOT2].TLL = (CH + 3)->SLOT[SLOT2].TL + (CH->ksl_base >> (CH + 3)->SLOT[SLOT2].ksl);
                        calcFcSlot(CH, &CH->SLOT[SLOT1]);
                        calcFcSlot(CH, &CH->SLOT[SLOT2]);
                        calcFcSlot(CH, &(CH + 3)->SLOT[SLOT1]);
                        calcFcSlot(CH, &(CH + 3)->SLOT[SLOT2]);
                    } else {
                        CH->SLOT[SLOT1].TLL = CH->SLOT[SLOT1].TL + (CH->ksl_base >> CH->SLOT[SLOT1].ksl);
                        CH->SLOT[SLOT2].TLL = CH->SLOT[SLOT2].TL + (CH->ksl_base >> CH->SLOT[SLOT2].ksl);
                        calcFcSlot(CH, &CH->SLOT[SLOT1]);
                        calcFcSlot(CH, &CH->SLOT[SLOT2]);
                    }
                    break;
                case 3: case 4: case 5:
                case 12: case 13: case 14:
                    if ((CH - 3)->extended) {
                        /* 2nd channel of a 4-op pair: do nothing */
                    } else {
                        CH->SLOT[SLOT1].TLL = CH->SLOT[SLOT1].TL + (CH->ksl_base >> CH->SLOT[SLOT1].ksl);
                        CH->SLOT[SLOT2].TLL = CH->SLOT[SLOT2].TL + (CH->ksl_base >> CH->SLOT[SLOT2].ksl);
                        calcFcSlot(CH, &CH->SLOT[SLOT1]);
                        calcFcSlot(CH, &CH->SLOT[SLOT2]);
                    }
                    break;
                default:
                    CH->SLOT[SLOT1].TLL = CH->SLOT[SLOT1].TL + (CH->ksl_base >> CH->SLOT[SLOT1].ksl);
                    CH->SLOT[SLOT2].TLL = CH->SLOT[SLOT2].TL + (CH->ksl_base >> CH->SLOT[SLOT2].ksl);
                    calcFcSlot(CH, &CH->SLOT[SLOT1]);
                    calcFcSlot(CH, &CH->SLOT[SLOT2]);
                    break;
                }
            } else {
                CH->SLOT[SLOT1].TLL = CH->SLOT[SLOT1].TL + (CH->ksl_base >> CH->SLOT[SLOT1].ksl);
                CH->SLOT[SLOT2].TLL = CH->SLOT[SLOT2].TL + (CH->ksl_base >> CH->SLOT[SLOT2].ksl);
                calcFcSlot(CH, &CH->SLOT[SLOT1]);
                calcFcSlot(CH, &CH->SLOT[SLOT2]);
            }
        }
        break;

    case 0xc0: {
        /* CH.D, CH.C, CH.B, CH.A, FB(3 bits), C */
        if ((r & 0xf) > 8) return;

        CH = &m_ch[(r & 0xf) + ch_offset];
        int base = ((r & 0xf) + ch_offset) * 4;

        if (m_OPL3_mode & 1) {
            m_pan[base    ] = (v & 0x10) ? ~0u : 0;   /* ch.A (L) */
            m_pan[base + 1] = (v & 0x20) ? ~0u : 0;   /* ch.B (R) */
            m_pan[base + 2] = (v & 0x40) ? ~0u : 0;   /* ch.C */
            m_pan[base + 3] = (v & 0x80) ? ~0u : 0;   /* ch.D */
        } else {
            /* OPL2 mode: always enabled */
            m_pan[base] = m_pan[base + 1] = m_pan[base + 2] = m_pan[base + 3] = ~0u;
        }
        m_pan_ctrl_value[(r & 0xf) + ch_offset] = v;

        CH->SLOT[SLOT1].FB  = (v >> 1) & 7 ? ((v >> 1) & 7) + 7 : 0;
        CH->SLOT[SLOT1].CON = v & 1;

        if (m_OPL3_mode & 1) {
            int chan_no = (r & 0x0f) + ch_offset;
            switch (chan_no) {
            case 0: case 1: case 2:
            case 9: case 10: case 11:
                if (CH->extended) {
                    uint8_t conn = (CH->SLOT[SLOT1].CON << 1) | ((CH + 3)->SLOT[SLOT1].CON << 0);
                    switch (conn) {
                    case 0:
                        /* 1 -> 2 -> 3 -> 4 -> out */
                        CH->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        CH->SLOT[SLOT2].conn_enum = CONN_PHASEMOD2;
                        (CH + 3)->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        (CH + 3)->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no + 3;
                        break;
                    case 1:
                        /* 1 -> 2 -\; 3 -> 4 -+- out */
                        CH->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no;
                        (CH + 3)->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        (CH + 3)->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no + 3;
                        break;
                    case 2:
                        /* 1 --\; 2 -> 3 -> 4 -+- out */
                        CH->SLOT[SLOT1].conn_enum = CONN_CHAN0 + chan_no;
                        CH->SLOT[SLOT2].conn_enum = CONN_PHASEMOD2;
                        (CH + 3)->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        (CH + 3)->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no + 3;
                        break;
                    case 3:
                        /* 1 --\; 2 -> 3 -+- out; 4 --/ */
                        CH->SLOT[SLOT1].conn_enum = CONN_CHAN0 + chan_no;
                        CH->SLOT[SLOT2].conn_enum = CONN_PHASEMOD2;
                        (CH + 3)->SLOT[SLOT1].conn_enum = CONN_CHAN0 + chan_no + 3;
                        (CH + 3)->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no + 3;
                        break;
                    }
                    slotConnect(&CH->SLOT[SLOT1]);
                    slotConnect(&CH->SLOT[SLOT2]);
                    slotConnect(&(CH + 3)->SLOT[SLOT1]);
                    slotConnect(&(CH + 3)->SLOT[SLOT2]);
                } else {
                    CH->SLOT[SLOT1].conn_enum = CH->SLOT[SLOT1].CON ? CONN_CHAN0 + (r & 0xf) + ch_offset : CONN_PHASEMOD;
                    CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + (r & 0xf) + ch_offset;
                    slotConnect(&CH->SLOT[SLOT1]);
                    slotConnect(&CH->SLOT[SLOT2]);
                }
                break;

            case 3: case 4: case 5:
            case 12: case 13: case 14:
                if ((CH - 3)->extended) {
                    uint8_t conn = ((CH - 3)->SLOT[SLOT1].CON << 1) | (CH->SLOT[SLOT1].CON << 0);
                    switch (conn) {
                    case 0:
                        (CH - 3)->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        (CH - 3)->SLOT[SLOT2].conn_enum = CONN_PHASEMOD2;
                        CH->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no;
                        break;
                    case 1:
                        (CH - 3)->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        (CH - 3)->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no - 3;
                        CH->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no;
                        break;
                    case 2:
                        (CH - 3)->SLOT[SLOT1].conn_enum = CONN_CHAN0 + chan_no - 3;
                        (CH - 3)->SLOT[SLOT2].conn_enum = CONN_PHASEMOD2;
                        CH->SLOT[SLOT1].conn_enum = CONN_PHASEMOD;
                        CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no;
                        break;
                    case 3:
                        (CH - 3)->SLOT[SLOT1].conn_enum = CONN_CHAN0 + chan_no - 3;
                        (CH - 3)->SLOT[SLOT2].conn_enum = CONN_PHASEMOD2;
                        CH->SLOT[SLOT1].conn_enum = CONN_CHAN0 + chan_no;
                        CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + chan_no;
                        break;
                    }
                    slotConnect(&(CH - 3)->SLOT[SLOT1]);
                    slotConnect(&(CH - 3)->SLOT[SLOT2]);
                    slotConnect(&CH->SLOT[SLOT1]);
                    slotConnect(&CH->SLOT[SLOT2]);
                } else {
                    CH->SLOT[SLOT1].conn_enum = CH->SLOT[SLOT1].CON ? CONN_CHAN0 + (r & 0xf) + ch_offset : CONN_PHASEMOD;
                    CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + (r & 0xf) + ch_offset;
                    slotConnect(&CH->SLOT[SLOT1]);
                    slotConnect(&CH->SLOT[SLOT2]);
                }
                break;

            default:
                CH->SLOT[SLOT1].conn_enum = CH->SLOT[SLOT1].CON ? CONN_CHAN0 + (r & 0xf) + ch_offset : CONN_PHASEMOD;
                CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + (r & 0xf) + ch_offset;
                slotConnect(&CH->SLOT[SLOT1]);
                slotConnect(&CH->SLOT[SLOT2]);
                break;
            }
        } else {
            /* OPL2 mode: always 2-operator */
            CH->SLOT[SLOT1].conn_enum = CH->SLOT[SLOT1].CON ? CONN_CHAN0 + (r & 0xf) + ch_offset : CONN_PHASEMOD;
            CH->SLOT[SLOT2].conn_enum = CONN_CHAN0 + (r & 0xf) + ch_offset;
            slotConnect(&CH->SLOT[SLOT1]);
            slotConnect(&CH->SLOT[SLOT2]);
        }
        break;
    }

    case 0xe0: {    /* waveform select */
        slot = slot_array[r & 0x1f];
        if (slot < 0) return;
        slot += ch_offset * 2;
        CH = &m_ch[slot / 2];

        /* the 3-bit value is stored regardless of the current mode (verified
           on real YMF262), but only waveforms 0-3 are selected in OPL2 mode */
        v &= 7;
        CH->SLOT[slot & 1].waveform_number = v;
        if (!(m_OPL3_mode & 1))
            v &= 3;
        CH->SLOT[slot & 1].wavetable = v;
        break;
    }
    }
}

// ── reset / ports ───────────────────────────────────────────────────────────

void OplFm::reset() {
    m_eg_timer = 0;
    m_eg_cnt   = 0;
    m_eg_next  = 0;
    m_eg_dirty = 1;

    m_noise_rng = 1;
    m_noise_p   = 0;
    m_nts       = 0;
    statusReset(0x60);

    m_st[0] = m_st[1] = 0;
    m_Tcnt[0] = m_Tcnt[1] = 0;

    /* reset with register writes, exactly MAME's OPL3ResetChip */
    writeReg(0x01, 0);
    writeReg(0x02, 0);
    writeReg(0x03, 0);
    writeReg(0x04, 0);

    for (int c = 0xff; c >= 0x20; c--)
        writeReg(c, 0);
    for (int c = 0x1ff; c >= 0x120; c--)
        writeReg(c, 0);

    for (int c = 0; c < 9 * 2; c++) {
        Chan* CH = &m_ch[c];
        for (int s = 0; s < 2; s++) {
            CH->SLOT[s].state  = EG_OFF;
            CH->SLOT[s].volume = MAX_ATT_INDEX;
            CH->SLOT[s].op1_out[0] = CH->SLOT[s].op1_out[1] = 0;
        }
    }
}

void OplFm::write(int a, uint8_t v) {
    switch (a & 3) {
    case 0:     /* address port 0 (register set #1) */
        m_address = v;
        break;

    case 1:     /* data port — A1 is ignored on data writes */
    case 3:
        writeReg(m_address, v);
        break;

    case 2:     /* address port 1 (register set #2) */
        /* verified on real YMF262: in OPL2 mode the only register reachable
           in set #2 is 0x105 — everything else lands in set #1 */
        if (m_OPL3_mode & 1)
            m_address = v | 0x100;
        else
            m_address = (v == 5) ? (v | 0x100) : v;
        break;
    }
}

// A channel whose BOTH envelopes sit in EG_OFF contributes exactly zero:
// env = TLL + MAX_ATT_INDEX >= ENV_QUIET gates every op_calc, and two silent
// samples settle slot 1's feedback memory to zero — force that and skip the
// whole per-sample walk. An envelope can only leave EG_OFF on a key-on, i.e.
// between gen() calls, so the skip can never miss an attack.
OPL_HOT void OplFm::chanCalcOrSkip(Chan* CH) {
    Slot* s = CH->SLOT;
    if (s[0].state == EG_OFF && s[1].state == EG_OFF) {
        s[0].op1_out[0] = s[0].op1_out[1] = 0;
        return;
    }
    chanCalc(CH);
}

// Channel a of a 4-op-capable pair plus its partner a+3.
OPL_HOT void OplFm::pairCalc(int a) {
    Chan* C = &m_ch[a];
    if (C->extended) {
        Chan* D = C + 3;
        if (C->SLOT[0].state == EG_OFF && C->SLOT[1].state == EG_OFF &&
            D->SLOT[0].state == EG_OFF && D->SLOT[1].state == EG_OFF) {
            C->SLOT[0].op1_out[0] = C->SLOT[0].op1_out[1] = 0;
            return;
        }
        chanCalc(C);
        chanCalcExt(D);
    } else {
        chanCalcOrSkip(C);
        chanCalcOrSkip(C + 3);
    }
}

// ── generation ──────────────────────────────────────────────────────────────

OPL_HOT bool OplFm::allQuiet() const {
    for (int i = 0; i < 18; i++)
        if (m_ch[i].SLOT[0].state != EG_OFF || m_ch[i].SLOT[1].state != EG_OFF)
            return false;
    return true;
}

OPL_HOT void OplFm::gen(int16_t* bufL, int16_t* bufR, int count, int bufpos) {
    if (count <= 0) return;

    // Timers first: they must keep true time even while the chip is silent —
    // the VGM plugin's detect sequence is "start timer 1, wait, read status".
    runTimers(count);

    // Whole-chip quiet fast path (see OplFm.h). Forcing op1_out to zero is
    // what two generated samples of silence would have done.
    if (allQuiet()) {
        for (int i = 0; i < 18; i++) {
            m_ch[i].SLOT[0].op1_out[0] = m_ch[i].SLOT[0].op1_out[1] = 0;
            m_ch[i].SLOT[1].op1_out[0] = m_ch[i].SLOT[1].op1_out[1] = 0;
        }
        if (m_quiet_samples < 0x10000000u) m_quiet_samples += count;
        m_pA = m_pB = m_cA = m_cB = 0;
        return;
    }
    m_quiet_samples = 0;

    for (int i = 0; i < count; i++) {
        int32_t a, b;
        if (!m_half) {
            renderSample(a, b);
        } else if ((m_half_tick ^= 1)) {
            // compute tick: new chip sample, output the midpoint (x2 lerp)
            m_pA = m_cA; m_pB = m_cB;
            renderSample(m_cA, m_cB);
            a = (m_pA + m_cA) >> 1;
            b = (m_pB + m_cB) >> 1;
        } else {
            // hold tick
            a = m_cA;
            b = m_cB;
        }
        bufL[bufpos + i] = (int16_t)limit(bufL[bufpos + i] + a, 32767, -32768);
        bufR[bufpos + i] = (int16_t)limit(bufR[bufpos + i] + b, 32767, -32768);
    }
}

// One chip sample through the full pipeline (both output accumulators).
OPL_HOT void OplFm::renderSample(int32_t& outA, int32_t& outB) {
    advanceLfo();

    memset(m_chanout, 0, sizeof(m_chanout));

    uint8_t rhythm = m_rhythm & 0x20;

    /* register set #1 */
    pairCalc(0);
    pairCalc(1);
    pairCalc(2);

    if (!rhythm) {
        chanCalcOrSkip(&m_ch[6]);
        chanCalcOrSkip(&m_ch[7]);
        chanCalcOrSkip(&m_ch[8]);
    } else {
        chanCalcRhythm(m_noise_rng & 1);
    }

    /* register set #2 */
    pairCalc(9);
    pairCalc(10);
    pairCalc(11);

    chanCalcOrSkip(&m_ch[15]);
    chanCalcOrSkip(&m_ch[16]);
    chanCalcOrSkip(&m_ch[17]);

    /* accumulate outputs A (left) and B (right); C/D are OPL4-only */
    int32_t a = 0, b = 0;
    for (int ch = 0; ch < 18; ch++) {
        a += m_chanout[ch] & m_pan[ch * 4];
        b += m_chanout[ch] & m_pan[ch * 4 + 1];
    }
    outA = a;
    outB = b;

    advance();
}
