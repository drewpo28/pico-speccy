/*

pico-speccy — YM2203 (OPN) FM synthesis. See OpnFm.h for provenance: this is a
reduced re-derivation of MAME's fm.cpp OPN core (Jarek Burczynski / Tatsuyuki
Satoh, GPL-2.0+), kept register-for-register faithful to it.

Deliberate differences from fm.cpp, all of them size or platform driven:

 - No LFO. A YM2203 has none (fm.cpp keeps LFO_AM/LFO_PM pinned at 0 for the
   YM2203 update loop), so the whole LFO path, its 32 KB lfo_pm_table and the
   AMS/PMS registers 0xb4+ are absent.
 - No SSG. On a YM2203 the SSG half IS an AY, and this project already has one:
   registers 0x00-0x0f never reach us, AySound keeps them.
 - fn_table (4096 x uint32 = 16 KB in fm.cpp) is computed arithmetically. It is
   only read when a channel's frequency register is written, never per sample.
 - tl_tab (13 x 2 x 256 signed ints = 26 KB) is stored as its 256-entry base row
   (uint16) plus the shift and sign that fm.cpp bakes into the flat table. The
   op_calc index arithmetic is unchanged; only the final fetch is decomposed.
 - The detune table is one raw 128-byte table scaled on use, instead of a
   per-chip 8x32 int32 table (1 KB per chip) built at every prescaler change.
 - Timers count in Q16 units of the timer prescaler instead of fm.cpp's
   FM_INTERNAL_TIMER macros (which are dead code in MAME — it always supplies an
   external timer handler — and do not survive their own arithmetic).
 - Output is scaled to +/-127 per chip (fm.cpp's FM_SAMPLE_BITS==8 path), which
   is the range this emulator's 8-bit mixer wants.

*/

#include "OpnFm.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <new>

#include "Debug.h"

// All in flash. The per-sample code was RAM-resident 2026-09-01..09-06 because
// the VGM plugin's PC-88 YM2203 rips write hundreds of registers per frame and
// every #BFFD data write runs the shared AY+FM catch-up (OPN has no write
// queue). Re-tested flash-resident on DVp2: no audible difference. If it ever
// costs frames again, an OPN write queue like the OPL one is the first lever.
// Compiled at -O2 (see CMakeLists).

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── fm.cpp constants ────────────────────────────────────────────────────────
#define FREQ_SH         16
#define EG_SH           16
#define FREQ_MASK       ((1 << FREQ_SH) - 1)

#define ENV_BITS        10
#define ENV_LEN         (1 << ENV_BITS)
#define ENV_STEP        (128.0 / ENV_LEN)

#define MAX_ATT_INDEX   (ENV_LEN - 1)
#define MIN_ATT_INDEX   0

#define EG_ATT          4
#define EG_DEC          3
#define EG_SUS          2
#define EG_REL          1
#define EG_OFF          0

#define SIN_LEN         1024
#define SIN_MASK        (SIN_LEN - 1)

#define TL_RES_LEN      256
#define TL_TAB_LEN      (13 * 2 * TL_RES_LEN)
#define ENV_QUIET       (TL_TAB_LEN >> 3)

#define RATE_STEPS      8

// Operator order inside Chan::SLOT[], as on the chip (fm.cpp's SLOT1..SLOT4).
#define SLOT1 0
#define SLOT2 2
#define SLOT3 1
#define SLOT4 3

#define OPN_CHAN(N) ((N) & 3)
#define OPN_SLOT(N) (((N) >> 2) & 3)

// ── shared tables (heap, reference counted) ─────────────────────────────────
// sin_tab: sine in the chip's logarithmic ("decibel") domain, 1024 entries.
// tl_base: the x = 0..255 row of fm.cpp's tl_tab; the other 12 rows are that
//          row shifted right by the row index, and the odd index is the sign.
static uint16_t* s_sin_tab = nullptr;
static uint16_t* s_tl_base = nullptr;
static int       s_tab_refs = 0;

static bool buildTables() {
    s_sin_tab = new (std::nothrow) uint16_t[SIN_LEN];
    s_tl_base = new (std::nothrow) uint16_t[TL_RES_LEN];
    if (!s_sin_tab || !s_tl_base) {
        delete[] s_sin_tab; s_sin_tab = nullptr;
        delete[] s_tl_base; s_tl_base = nullptr;
        Debug::log("OpnFm: table OOM");
        return false;
    }

    for (int x = 0; x < TL_RES_LEN; x++) {
        double m = floor((1 << 16) / pow(2.0, (x + 1) * (ENV_STEP / 4.0) / 8.0));
        int n = (int)m;             // 16 bits
        n >>= 4;                    // 12 bits
        n = (n & 1) ? (n >> 1) + 1 : (n >> 1);   // 11 bits, rounded
        n <<= 2;                    // 13 bits, as in the real chip
        s_tl_base[x] = (uint16_t)n;
    }

    for (int i = 0; i < SIN_LEN; i++) {
        // "non-standard sinus" — fm.cpp's comment; checked against the real chip.
        double m = sin(((i * 2) + 1) * M_PI / SIN_LEN);
        double o = (m > 0.0) ? 8 * log(1.0 / m) / log(2.0)
                             : 8 * log(-1.0 / m) / log(2.0);
        o = o / (ENV_STEP / 4);
        int n = (int)(2.0 * o);
        n = (n & 1) ? (n >> 1) + 1 : (n >> 1);
        s_sin_tab[i] = (uint16_t)(n * 2 + (m >= 0.0 ? 0 : 1));
    }
    return true;
}

bool OpnFm::tablesReady() { return s_sin_tab != nullptr; }

OpnFm* opnfm[2] = { nullptr, nullptr };

// One operator's contribution, fm.cpp's op_calc with the tl_tab fetch expanded.
static inline int op_out(uint32_t p) {
    if (p >= TL_TAB_LEN) return 0;
    const uint32_t q = p >> 1;
    const int v = (int)(s_tl_base[q & 255] >> (q >> 8));
    return (p & 1) ? -v : v;
}

static inline int op_calc(uint32_t phase, unsigned int env, int pm) {
    const uint32_t p = (env << 3) +
        s_sin_tab[(((int)((phase & ~FREQ_MASK) + (pm << 15))) >> FREQ_SH) & SIN_MASK];
    return op_out(p);
}

// Slot 1 only: its phase modulation input is its own feedback, already shifted.
static inline int op_calc1(uint32_t phase, unsigned int env, int pm) {
    const uint32_t p = (env << 3) +
        s_sin_tab[(((int)((phase & ~FREQ_MASK) + pm)) >> FREQ_SH) & SIN_MASK];
    return op_out(p);
}

// ── constant tables (fm.cpp, verbatim) ──────────────────────────────────────
#define SC(db) (uint32_t)((db) * (4.0 / ENV_STEP))
static const uint32_t sl_table[16] = {
    SC( 0), SC( 1), SC( 2), SC( 3), SC( 4), SC( 5), SC( 6), SC( 7),
    SC( 8), SC( 9), SC(10), SC(11), SC(12), SC(13), SC(14), SC(31)
};
#undef SC

static const uint8_t eg_inc[19 * RATE_STEPS] = {
/*cycle:0 1  2 3  4 5  6 7*/
/* 0 */ 0,1, 0,1, 0,1, 0,1, /* rates 00..11 0 (increment by 0 or 1) */
/* 1 */ 0,1, 0,1, 1,1, 0,1, /* rates 00..11 1 */
/* 2 */ 0,1, 1,1, 0,1, 1,1, /* rates 00..11 2 */
/* 3 */ 0,1, 1,1, 1,1, 1,1, /* rates 00..11 3 */

/* 4 */ 1,1, 1,1, 1,1, 1,1, /* rate 12 0 (increment by 1) */
/* 5 */ 1,1, 1,2, 1,1, 1,2, /* rate 12 1 */
/* 6 */ 1,2, 1,2, 1,2, 1,2, /* rate 12 2 */
/* 7 */ 1,2, 2,2, 1,2, 2,2, /* rate 12 3 */

/* 8 */ 2,2, 2,2, 2,2, 2,2, /* rate 13 0 (increment by 2) */
/* 9 */ 2,2, 2,4, 2,2, 2,4, /* rate 13 1 */
/*10 */ 2,4, 2,4, 2,4, 2,4, /* rate 13 2 */
/*11 */ 2,4, 4,4, 2,4, 4,4, /* rate 13 3 */

/*12 */ 4,4, 4,4, 4,4, 4,4, /* rate 14 0 (increment by 4) */
/*13 */ 4,4, 4,8, 4,4, 4,8, /* rate 14 1 */
/*14 */ 4,8, 4,8, 4,8, 4,8, /* rate 14 2 */
/*15 */ 4,8, 8,8, 4,8, 8,8, /* rate 14 3 */

/*16 */ 8,8, 8,8, 8,8, 8,8, /* rates 15 0..15 3 (increment by 8) */
/*17 */ 16,16,16,16,16,16,16,16, /* rates 15 2, 15 3 for attack */
/*18 */ 0,0, 0,0, 0,0, 0,0, /* infinity rates for attack and decay(s) */
};

#define O(a) ((a) * RATE_STEPS)
static const uint8_t eg_rate_select[32 + 64 + 32] = {
/* 32 infinite time rates */
O(18),O(18),O(18),O(18),O(18),O(18),O(18),O(18),
O(18),O(18),O(18),O(18),O(18),O(18),O(18),O(18),
O(18),O(18),O(18),O(18),O(18),O(18),O(18),O(18),
O(18),O(18),O(18),O(18),O(18),O(18),O(18),O(18),
/* rates 00-11 */
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
O( 0),O( 1),O( 2),O( 3), O( 0),O( 1),O( 2),O( 3),
/* rate 12 */ O( 4),O( 5),O( 6),O( 7),
/* rate 13 */ O( 8),O( 9),O(10),O(11),
/* rate 14 */ O(12),O(13),O(14),O(15),
/* rate 15 */ O(16),O(16),O(16),O(16),
/* 32 dummy rates (same as 15 3) */
O(16),O(16),O(16),O(16),O(16),O(16),O(16),O(16),
O(16),O(16),O(16),O(16),O(16),O(16),O(16),O(16),
O(16),O(16),O(16),O(16),O(16),O(16),O(16),O(16),
O(16),O(16),O(16),O(16),O(16),O(16),O(16),O(16)
};
#undef O

static const uint8_t eg_rate_shift[32 + 64 + 32] = {
/* 32 infinite time rates */
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
/* rates 00-11 */
11,11,11,11, 10,10,10,10, 9,9,9,9, 8,8,8,8,
 7, 7, 7, 7,  6, 6, 6, 6, 5,5,5,5, 4,4,4,4,
 3, 3, 3, 3,  2, 2, 2, 2, 1,1,1,1, 0,0,0,0,
/* rates 12-15 */
0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
/* 32 dummy rates */
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0
};

/* YM2151/YM2612 phase increment data, 10.10 fixed point */
static const uint8_t dt_tab[4 * 32] = {
/* FD=0 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* FD=1 */
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
    2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8,
/* FD=2 */
    1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5,
    5, 6, 6, 7, 8, 8, 9,10,11,12,13,14,16,16,16,16,
/* FD=3 */
    2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7,
    8, 8, 9,10,11,12,13,14,16,17,19,20,22,22,22,22
};

/* F-Number high 4 bits -> key code low 2 bits */
static const uint8_t opn_fktable[16] = {0,0,0,0,0,0,0,1,2,3,3,3,3,3,3,3};

// ── construction ────────────────────────────────────────────────────────────

OpnFm::OpnFm() {
    memset(&m_ch, 0, sizeof(m_ch));
    m_m2 = m_c1 = m_c2 = m_mem = 0;
    m_out_fm[0] = m_out_fm[1] = m_out_fm[2] = 0;
    m_clock = 3546900; m_rate = 31250;
    m_prescaler_sel = 2;
    m_fnMulQ16 = m_fnMax = m_dtScaleQ16 = 0;
    m_egTimerAdd = m_egTimer = m_egCnt = 0;
    m_timerStepQ16 = 0;
    m_mode = 0; m_addr = 0; m_status = 0; m_fn_h = 0;
    m_TA = m_TAC = 0; m_TB = 0; m_TBC = 0;
    memset(&m_sl3, 0, sizeof(m_sl3));

    if (s_tab_refs == 0) buildTables();
    s_tab_refs++;

    for (int c = 0; c < 3; c++) {
        setupConnection(&m_ch[c], c);
        m_ch[c].SLOT[SLOT1].Incr = -1;
    }
    setRates(m_clock, m_rate);
}

OpnFm::~OpnFm() {
    if (--s_tab_refs == 0) {
        delete[] s_sin_tab; s_sin_tab = nullptr;
        delete[] s_tl_base; s_tl_base = nullptr;
    }
}

// ── rate-dependent constants ────────────────────────────────────────────────
// fm.cpp keeps `freqbase = (clock / rate) / prescaler` and multiplies it into a
// handful of tables. We keep the same quantity and the same products, only as
// fixed point and without materialising the tables.
void OpnFm::setRates(int clock, int rate) {
    if (clock > 0) m_clock = clock;
    if (rate  > 0) m_rate  = rate;

    static const int opn_pres[4] = { 2*12, 2*12, 6*12, 3*12 };
    const int pres = opn_pres[m_prescaler_sel & 3];

    const double freqbase = m_rate ? ((double)m_clock / m_rate) / pres : 0.0;

    m_egTimerAdd = (uint32_t)((1 << EG_SH) * freqbase);

    // fm.cpp: fn_table[i] = i * 32 * freqbase * (1 << (FREQ_SH - 10)), read as
    // fn_table[fn * 2], i.e. fn * 4096 * freqbase.
    m_fnMulQ16   = (uint32_t)(4096.0 * freqbase * 65536.0);
    m_fnMax      = (uint32_t)(0x20000 * freqbase * (1 << (FREQ_SH - 10)));
    // fm.cpp: dt_tab[d][i] = dttable[..] * SIN_LEN * freqbase * (1<<FREQ_SH) / (1<<20).
    m_dtScaleQ16 = (uint32_t)(64.0 * freqbase * 65536.0);

    // One output sample is (clock / rate) chip clocks, i.e. exactly `freqbase`
    // ticks of the timer prescaler (the timer prescaler equals `pres`).
    m_timerStepQ16 = (uint32_t)(freqbase * 65536.0);

    // Every cached phase increment was derived from the old freqbase.
    for (int c = 0; c < 3; c++) m_ch[c].SLOT[SLOT1].Incr = -1;
}

/*
  Prescaler circuit, fm.cpp's best guess at verified chip behaviour:

               +--------------+  +-sel2-+
               |              +--|in20  |
         +---+ |  +-sel1-+       |      |
M-CLK -+-|1/2|-+--|in10  | +---+ |   out|--INT_CLOCK
       | +---+    |   out|-|1/3|-|in21  |
       +----------|in11  | +---+ +------+
                  +------+

  reg.2d : sel2 = in21     reg.2e : sel1 = in11
  reg.2f : clear both      reset  : sel1 = in11, sel2 = in21  (= divide by 6)

  The SSG divider changes with it (clock * 2 / ssg_pres, i.e. /2 at reset, which
  is what puts a 3.5 MHz YM2203's SSG at the ZX's 1.75 MHz). We deliberately do
  NOT re-clock AySound from here: the only sequence real TFM software uses is
  the manual's "write 0x2F, then 0x2D", which starts and ends at /6 — following
  it through the intermediate state would just detune the PSG for one write.
*/
void OpnFm::prescalerWrite(int addr) {
    switch (addr) {
        case 0:    m_prescaler_sel = 2; break;   // reset
        case 0x2d: m_prescaler_sel |= 0x02; break;
        case 0x2e: m_prescaler_sel |= 0x01; break;
        case 0x2f: m_prescaler_sel  = 0;    break;
    }
    setRates(0, 0);
}

void OpnFm::reset() {
    prescalerWrite(0);
    writeMode(0x27, 0x30);       // mode 0, both timers reset

    m_egTimer = 0;
    m_egCnt   = 0;
    m_status  = 0;
    m_addr    = 0;
    m_fn_h    = 0;
    memset(&m_sl3, 0, sizeof(m_sl3));

    m_mode = 0;
    m_TA = m_TAC = 0;
    m_TB = 0; m_TBC = 0;

    for (int c = 0; c < 3; c++) {
        m_ch[c].fc = 0;
        m_ch[c].mem_value = 0;
        m_ch[c].op1_out[0] = m_ch[c].op1_out[1] = 0;
        for (int s = 0; s < 4; s++) {
            m_ch[c].SLOT[s].ssg     = 0;
            m_ch[c].SLOT[s].ssgn    = 0;
            m_ch[c].SLOT[s].state   = EG_OFF;
            m_ch[c].SLOT[s].volume  = MAX_ATT_INDEX;
            m_ch[c].SLOT[s].vol_out = MAX_ATT_INDEX;
            m_ch[c].SLOT[s].key     = 0;
            m_ch[c].SLOT[s].phase   = 0;
        }
    }
    for (int r = 0xb2; r >= 0x30; r--) writeReg(r, 0);
    for (int r = 0x26; r >= 0x20; r--) writeReg(r, 0);
}

// ── register writes ─────────────────────────────────────────────────────────

void OpnFm::writeAddr(uint8_t r) {
    m_addr = r;
    if (r >= 0x2d && r <= 0x2f) prescalerWrite(r);
}

void OpnFm::writeData(uint8_t v) {
    const uint8_t r = m_addr;
    if (r < 0x20) return;                 // SSG half — AySound owns 0x00-0x0f
    if (r < 0x30) writeMode(r, v);
    else          writeReg(r, v);
}

void OpnFm::keyOn(Chan* ch, int s) {
    Slot* slot = &ch->SLOT[s];
    if (!slot->key) {
        slot->key   = 1;
        slot->phase = 0;                      // restart the phase generator
        slot->ssgn  = (slot->ssg & 0x04) >> 1;
        slot->state = EG_ATT;
    }
}

void OpnFm::keyOff(Chan* ch, int s) {
    Slot* slot = &ch->SLOT[s];
    if (slot->key) {
        slot->key = 0;
        if (slot->state > EG_REL) slot->state = EG_REL;
    }
}

void OpnFm::setTimers(int v) {
    // b7 CSM, b6 3-slot, b5/b4 reset B/A, b3/b2 enable B/A, b1/b0 load B/A
    m_mode = v;
    if (v & 0x20) statusReset(0x02);
    if (v & 0x10) statusReset(0x01);

    if (v & 0x02) { if (m_TBC == 0) m_TBC = (int32_t)((256 - m_TB) << 4) << 16; }
    else            m_TBC = 0;

    if (v & 0x01) { if (m_TAC == 0) m_TAC = (int32_t)(1024 - m_TA) << 16; }
    else            m_TAC = 0;
}

void OpnFm::timerAOver() {
    if (m_mode & 0x04) statusSet(0x01);
    m_TAC = (int32_t)(1024 - m_TA) << 16;
    // CSM: key every operator of channel 3 on and straight back off, which is
    // what makes the envelope retrigger at the timer rate.
    if (m_mode & 0x80) {
        Chan* ch = &m_ch[2];
        for (int s = 0; s < 4; s++) {
            if (!ch->SLOT[s].key) { keyOn(ch, s); keyOff(ch, s); }
        }
    }
}

void OpnFm::timerBOver() {
    if (m_mode & 0x08) statusSet(0x02);
    m_TBC = (int32_t)((256 - m_TB) << 4) << 16;
}

void OpnFm::writeMode(int r, int v) {
    switch (r) {
        case 0x21: break;                              // test
        case 0x22: break;                              // LFO — no LFO on a YM2203
        case 0x24: m_TA = (m_TA & 0x03) | (v << 2); break;   // timer A high 8
        case 0x25: m_TA = (m_TA & 0x3fc) | (v & 3);  break;  // timer A low 2
        case 0x26: m_TB = (uint8_t)v; break;
        case 0x27: setTimers(v); break;
        case 0x28: {                                   // key on / off
            const int c = v & 0x03;
            if (c == 3) break;                         // no 4th channel
            Chan* ch = &m_ch[c];
            if (v & 0x10) keyOn(ch, SLOT1); else keyOff(ch, SLOT1);
            if (v & 0x20) keyOn(ch, SLOT2); else keyOff(ch, SLOT2);
            if (v & 0x40) keyOn(ch, SLOT3); else keyOff(ch, SLOT3);
            if (v & 0x80) keyOn(ch, SLOT4); else keyOff(ch, SLOT4);
            break;
        }
    }
}

void OpnFm::writeReg(int r, int v) {
    const uint8_t c = OPN_CHAN(r);
    if (c == 3) return;                                // 0xX3, 0xX7, 0xXB, 0xXF

    Chan* ch = &m_ch[c];
    Slot* slot = &ch->SLOT[OPN_SLOT(r)];

    switch (r & 0xf0) {
    case 0x30:                                          // DT, MUL
        slot->mul = (v & 0x0f) ? (v & 0x0f) * 2 : 1;
        slot->dt  = (v >> 4) & 7;
        ch->SLOT[SLOT1].Incr = -1;
        break;

    case 0x40:                                          // TL
        slot->tl = (uint32_t)(v & 0x7f) << (ENV_BITS - 7);
        break;

    case 0x50: {                                        // KS, AR
        const uint8_t old_KSR = slot->KSR;
        slot->ar  = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;
        slot->KSR = 3 - (v >> 6);
        if (slot->KSR != old_KSR) ch->SLOT[SLOT1].Incr = -1;
        if ((slot->ar + slot->ksr) < 32 + 62) {
            slot->eg_sh_ar  = eg_rate_shift [slot->ar + slot->ksr];
            slot->eg_sel_ar = eg_rate_select[slot->ar + slot->ksr];
        } else {
            slot->eg_sh_ar  = 0;
            slot->eg_sel_ar = 17 * RATE_STEPS;
        }
        break;
    }

    case 0x60:                                          // DR (bit 7 = AM, no LFO here)
        slot->d1r = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;
        slot->eg_sh_d1r  = eg_rate_shift [slot->d1r + slot->ksr];
        slot->eg_sel_d1r = eg_rate_select[slot->d1r + slot->ksr];
        break;

    case 0x70:                                          // SR
        slot->d2r = (v & 0x1f) ? 32 + ((v & 0x1f) << 1) : 0;
        slot->eg_sh_d2r  = eg_rate_shift [slot->d2r + slot->ksr];
        slot->eg_sel_d2r = eg_rate_select[slot->d2r + slot->ksr];
        break;

    case 0x80:                                          // SL, RR
        slot->sl = sl_table[v >> 4];
        slot->rr = 34 + ((v & 0x0f) << 2);
        slot->eg_sh_rr  = eg_rate_shift [slot->rr + slot->ksr];
        slot->eg_sel_rr = eg_rate_select[slot->rr + slot->ksr];
        break;

    case 0x90:                                          // SSG-EG
        slot->ssg  = v & 0x0f;
        slot->ssgn = (v & 0x04) >> 1;                   // bit 1 of ssgn = attack
        break;

    case 0xa0:
        switch (OPN_SLOT(r)) {
        case 0: {                                       // 0xa0-0xa2: F-Num low
            const uint32_t fn  = ((uint32_t)(m_fn_h & 7) << 8) + v;
            const uint8_t  blk = m_fn_h >> 3;
            ch->kcode      = (blk << 2) | opn_fktable[fn >> 7];
            ch->fc         = fnCalc(fn, blk);
            ch->block_fnum = (blk << 11) | fn;
            ch->SLOT[SLOT1].Incr = -1;
            break;
        }
        case 1:                                         // 0xa4-0xa6: block + F-Num high
            m_fn_h = v & 0x3f;
            break;
        case 2: {                                       // 0xa8-0xaa: ch3 per-operator F-Num low
            const uint32_t fn  = ((uint32_t)(m_sl3.fn_h & 7) << 8) + v;
            const uint8_t  blk = m_sl3.fn_h >> 3;
            m_sl3.kcode[c]      = (blk << 2) | opn_fktable[fn >> 7];
            m_sl3.fc[c]         = fnCalc(fn, blk);
            m_sl3.block_fnum[c] = (blk << 11) | fn;
            m_ch[2].SLOT[SLOT1].Incr = -1;
            break;
        }
        case 3:                                         // 0xac-0xae
            m_sl3.fn_h = v & 0x3f;
            break;
        }
        break;

    case 0xb0:
        switch (OPN_SLOT(r)) {
        case 0: {                                       // 0xb0-0xb2: feedback + algorithm
            const int feedback = (v >> 3) & 7;
            ch->ALGO = v & 7;
            ch->FB   = feedback ? feedback + 6 : 0;
            setupConnection(ch, c);
            break;
        }
        case 1: break;                                  // 0xb4-0xb6: L/R/AMS/PMS — YM2612 only
        }
        break;
    }
}

// fm.cpp: fn_table[fn * 2] >> (7 - blk).
uint32_t OpnFm::fnCalc(uint32_t fn, uint8_t blk) const {
    const uint32_t inc = (uint32_t)(((uint64_t)fn * m_fnMulQ16) >> 16);
    return inc >> (7 - blk);
}

int32_t OpnFm::dtVal(uint8_t dt, int kc) const {
    const int32_t v = (int32_t)(((uint64_t)dt_tab[(dt & 3) * 32 + kc] * m_dtScaleQ16) >> 16);
    return (dt & 4) ? -v : v;
}

void OpnFm::setupConnection(Chan* ch, int idx) {
    int32_t* carrier = &m_out_fm[idx];

    switch (ch->ALGO) {
    case 0:   /* M1---C1---MEM---M2---C2---OUT */
        ch->connect1 = &m_c1;  ch->connect2 = &m_mem; ch->connect3 = &m_c2;
        ch->mem_connect = &m_m2; break;
    case 1:   /* M1------+-MEM---M2---C2---OUT, C1-+ */
        ch->connect1 = &m_mem; ch->connect2 = &m_mem; ch->connect3 = &m_c2;
        ch->mem_connect = &m_m2; break;
    case 2:   /* M1-----------------+-C2---OUT, C1---MEM---M2-+ */
        ch->connect1 = &m_c2;  ch->connect2 = &m_mem; ch->connect3 = &m_c2;
        ch->mem_connect = &m_m2; break;
    case 3:   /* M1---C1---MEM------+-C2---OUT, M2-+ */
        ch->connect1 = &m_c1;  ch->connect2 = &m_mem; ch->connect3 = &m_c2;
        ch->mem_connect = &m_c2; break;
    case 4:   /* M1---C1-+-OUT, M2---C2-+ */
        ch->connect1 = &m_c1;  ch->connect2 = carrier; ch->connect3 = &m_c2;
        ch->mem_connect = &m_mem; break;
    case 5:   /* +----C1----+ / M1-+-MEM---M2-+-OUT / +----C2----+ */
        ch->connect1 = nullptr;                          // special mark
        ch->connect2 = carrier; ch->connect3 = carrier;
        ch->mem_connect = &m_m2; break;
    case 6:   /* M1---C1-+, M2-+-OUT, C2-+ */
        ch->connect1 = &m_c1;  ch->connect2 = carrier; ch->connect3 = carrier;
        ch->mem_connect = &m_mem; break;
    case 7:   /* all four operators straight out */
        ch->connect1 = carrier; ch->connect2 = carrier; ch->connect3 = carrier;
        ch->mem_connect = &m_mem; break;
    }
    ch->connect4 = carrier;
}

// ── per-sample engine ───────────────────────────────────────────────────────

void OpnFm::refreshSlot(Slot* s, int fc, int kc) {
    const int ksr = kc >> s->KSR;

    fc += dtVal(s->dt, kc);
    if (fc < 0) fc += m_fnMax;      // phase overflow, aka the detune bug (Nemesis)

    s->Incr = (fc * (int)s->mul) >> 1;

    if (s->ksr != ksr) {
        s->ksr = (uint8_t)ksr;
        if ((s->ar + s->ksr) < 32 + 62) {
            s->eg_sh_ar  = eg_rate_shift [s->ar + s->ksr];
            s->eg_sel_ar = eg_rate_select[s->ar + s->ksr];
        } else {
            s->eg_sh_ar  = 0;
            s->eg_sel_ar = 17 * RATE_STEPS;
        }
        s->eg_sh_d1r  = eg_rate_shift [s->d1r + s->ksr];
        s->eg_sh_d2r  = eg_rate_shift [s->d2r + s->ksr];
        s->eg_sh_rr   = eg_rate_shift [s->rr  + s->ksr];
        s->eg_sel_d1r = eg_rate_select[s->d1r + s->ksr];
        s->eg_sel_d2r = eg_rate_select[s->d2r + s->ksr];
        s->eg_sel_rr  = eg_rate_select[s->rr  + s->ksr];
    }
}

void OpnFm::refreshChan(Chan* ch) {
    if (ch->SLOT[SLOT1].Incr == -1) {
        const int fc = (int)ch->fc;
        const int kc = ch->kcode;
        refreshSlot(&ch->SLOT[SLOT1], fc, kc);
        refreshSlot(&ch->SLOT[SLOT2], fc, kc);
        refreshSlot(&ch->SLOT[SLOT3], fc, kc);
        refreshSlot(&ch->SLOT[SLOT4], fc, kc);
    }
}

void OpnFm::advanceEg(Slot* slot) {
    for (int i = 0; i < 4; i++, slot++) {
        unsigned int swap_flag = 0;

        switch (slot->state) {
        case EG_ATT:
            if (!(m_egCnt & ((1u << slot->eg_sh_ar) - 1))) {
                slot->volume += (~slot->volume *
                    (eg_inc[slot->eg_sel_ar + ((m_egCnt >> slot->eg_sh_ar) & 7)])) >> 4;
                if (slot->volume <= MIN_ATT_INDEX) {
                    slot->volume = MIN_ATT_INDEX;
                    slot->state  = EG_DEC;
                }
            }
            break;

        case EG_DEC:
            if (!(m_egCnt & ((1u << slot->eg_sh_d1r) - 1))) {
                // SSG-EG runs decay and sustain four times coarser (256 steps).
                const int step = eg_inc[slot->eg_sel_d1r + ((m_egCnt >> slot->eg_sh_d1r) & 7)];
                slot->volume += (slot->ssg & 0x08) ? 4 * step : step;
                if (slot->volume >= (int32_t)slot->sl) slot->state = EG_SUS;
            }
            break;

        case EG_SUS:
            if (slot->ssg & 0x08) {
                if (!(m_egCnt & ((1u << slot->eg_sh_d2r) - 1))) {
                    slot->volume += 4 * eg_inc[slot->eg_sel_d2r + ((m_egCnt >> slot->eg_sh_d2r) & 7)];
                    if (slot->volume >= ENV_QUIET) {
                        slot->volume = MAX_ATT_INDEX;
                        if (slot->ssg & 0x01) {            // bit 0 = hold
                            if (!(slot->ssgn & 1))
                                swap_flag = (slot->ssg & 0x02) | 1;   // bit 1 = alternate
                        } else {
                            // same as a key-on: restart the phase generator
                            slot->phase  = 0;
                            slot->volume = 511;
                            slot->state  = EG_ATT;
                            swap_flag = (slot->ssg & 0x02);
                        }
                    }
                }
            } else {
                if (!(m_egCnt & ((1u << slot->eg_sh_d2r) - 1))) {
                    slot->volume += eg_inc[slot->eg_sel_d2r + ((m_egCnt >> slot->eg_sh_d2r) & 7)];
                    if (slot->volume >= MAX_ATT_INDEX) {
                        slot->volume = MAX_ATT_INDEX;
                        // do not change state (verified on real chip)
                    }
                }
            }
            break;

        case EG_REL:
            if (!(m_egCnt & ((1u << slot->eg_sh_rr) - 1))) {
                slot->volume += eg_inc[slot->eg_sel_rr + ((m_egCnt >> slot->eg_sh_rr) & 7)];
                if (slot->volume >= MAX_ATT_INDEX) {
                    slot->volume = MAX_ATT_INDEX;
                    slot->state  = EG_OFF;
                }
            }
            break;
        }

        uint32_t out = (uint32_t)slot->volume;
        // Negate the output; the change comes from the alternate bit, the initial
        // state from the attack bit.
        if ((slot->ssg & 0x08) && (slot->ssgn & 2) && (slot->state > EG_REL))
            out ^= MAX_ATT_INDEX;

        slot->vol_out = out + slot->tl;
        slot->ssgn ^= swap_flag;
    }
}

void OpnFm::chanCalc(Chan* ch) {
    m_m2 = m_c1 = m_c2 = m_mem = 0;
    *ch->mem_connect = ch->mem_value;      // the one-sample MEM delay

    unsigned int eg_out = ch->SLOT[SLOT1].vol_out;
    {
        int32_t out = ch->op1_out[0] + ch->op1_out[1];
        ch->op1_out[0] = ch->op1_out[1];

        if (!ch->connect1) {
            m_mem = m_c1 = m_c2 = ch->op1_out[0];    // algorithm 5
        } else {
            *ch->connect1 += ch->op1_out[0];
        }

        ch->op1_out[1] = 0;
        if (eg_out < ENV_QUIET) {
            if (!ch->FB) out = 0;
            ch->op1_out[1] = op_calc1(ch->SLOT[SLOT1].phase, eg_out, out << ch->FB);
        }
    }

    eg_out = ch->SLOT[SLOT3].vol_out;
    if (eg_out < ENV_QUIET) *ch->connect3 += op_calc(ch->SLOT[SLOT3].phase, eg_out, m_m2);

    eg_out = ch->SLOT[SLOT2].vol_out;
    if (eg_out < ENV_QUIET) *ch->connect2 += op_calc(ch->SLOT[SLOT2].phase, eg_out, m_c1);

    eg_out = ch->SLOT[SLOT4].vol_out;
    if (eg_out < ENV_QUIET) *ch->connect4 += op_calc(ch->SLOT[SLOT4].phase, eg_out, m_c2);

    ch->mem_value = m_mem;

    ch->SLOT[0].phase += ch->SLOT[0].Incr;
    ch->SLOT[1].phase += ch->SLOT[1].Incr;
    ch->SLOT[2].phase += ch->SLOT[2].Incr;
    ch->SLOT[3].phase += ch->SLOT[3].Incr;
}

// Deliberately in flash, unlike AySound::gen_sound / SAASound::gen_sound: the
// two lookup tables it reads are on the heap, so the XIP cache holds the working
// set, and the heap margin at VIDEO::Init is under 4 KB on PICO_DV (see the
// header comment for the RAM-resident period).
void OpnFm::gen(int16_t* buf, int bufsize, int bufpos) {
    if (!s_sin_tab) return;

    refreshChan(&m_ch[0]);
    refreshChan(&m_ch[1]);
    if (m_mode & 0xc0) {
        // Channel 3 special mode: operators 1..3 take their own frequencies,
        // operator 4 keeps the channel's. Note the slot-to-fc mapping is not the
        // identity — it is the chip's, and fm.cpp's.
        if (m_ch[2].SLOT[SLOT1].Incr == -1) {
            refreshSlot(&m_ch[2].SLOT[SLOT1], (int)m_sl3.fc[1], m_sl3.kcode[1]);
            refreshSlot(&m_ch[2].SLOT[SLOT2], (int)m_sl3.fc[2], m_sl3.kcode[2]);
            refreshSlot(&m_ch[2].SLOT[SLOT3], (int)m_sl3.fc[0], m_sl3.kcode[0]);
            refreshSlot(&m_ch[2].SLOT[SLOT4], (int)m_ch[2].fc,  m_ch[2].kcode);
        }
    } else {
        refreshChan(&m_ch[2]);
    }

    const uint32_t eg_overflow = 3 << EG_SH;   // the EG runs at a third of the FM rate

    // Silent-chip fast path. With every operator in EG_OFF the chip contributes
    // nothing and cannot start to: an envelope only ever leaves EG_OFF on a
    // key-on, which is a register write, and register writes happen between
    // gen() calls (the mixer is caught up before each one). This is the state a
    // chip sits in whenever TurboSound FM is enabled in Config but the software
    // running is an ordinary AY title — most of the time, in other words — and
    // skipping it there costs a scan of 12 bytes per call.
    // CSM (0x27 bit 7) is excluded because a timer A overflow keys channel 3 on
    // from inside the loop, so "silent now" would not stay true.
    bool silent = !(m_mode & 0x80);
    for (int c = 0; c < 3 && silent; c++)
        for (int s = 0; s < 4; s++)
            if (m_ch[c].SLOT[s].state != EG_OFF) { silent = false; break; }
    if (silent) {
        // The timers are the one thing that keeps running: a player can drive its
        // tempo from timer A/B with every voice released.
        for (int i = 0; i < bufsize; i++) {
            if (m_TAC) { m_TAC -= (int32_t)m_timerStepQ16; if (m_TAC <= 0) timerAOver(); }
            if (m_TBC) { m_TBC -= (int32_t)m_timerStepQ16; if (m_TBC <= 0) timerBOver(); }
        }
        return;
    }

    for (int i = 0; i < bufsize; i++) {
        m_out_fm[0] = m_out_fm[1] = m_out_fm[2] = 0;

        m_egTimer += m_egTimerAdd;
        while (m_egTimer >= eg_overflow) {
            m_egTimer -= eg_overflow;
            m_egCnt++;
            advanceEg(&m_ch[0].SLOT[0]);
            advanceEg(&m_ch[1].SLOT[0]);
            advanceEg(&m_ch[2].SLOT[0]);
        }

        chanCalc(&m_ch[0]);
        chanCalc(&m_ch[1]);
        chanCalc(&m_ch[2]);

        int lt = (m_out_fm[0] + m_out_fm[1] + m_out_fm[2]) >> 8;
        if (lt > 127) lt = 127; else if (lt < -128) lt = -128;
        buf[bufpos + i] += (int16_t)lt;

        if (m_TAC) { m_TAC -= (int32_t)m_timerStepQ16; if (m_TAC <= 0) timerAOver(); }
        if (m_TBC) { m_TBC -= (int32_t)m_timerStepQ16; if (m_TBC <= 0) timerBOver(); }
    }
}
