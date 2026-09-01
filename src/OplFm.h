/*

pico-speccy — YMF262 (OPL3) FM synthesis, for the AlexZor DivMMC VGM-player
sound card (address/data register pairs on Z80 ports #C4/#C5 and #C6/#C7).

The core is a port of MAME's ymf262.cpp (Copyright Jarek Burczynski, license
GPL-2.0+, taken from tag mame0220 — the same lineage as our OpnFm/fm.cpp):
18 channels x 2 operators, 8 waveforms, 4-op channel pairing, rhythm mode,
tremolo/vibrato LFO, both timers. Register semantics, the EG tables, the
connection tables and the sine/attenuation math are all ymf262.cpp's.

Deliberate differences, all size or platform driven:

 - tl_tab (26 KB) and sin_tab (32 KB), which MAME builds into .bss at runtime,
   are decomposed exactly like OpnFm's: the 256-entry tl base row (512 B) and
   the 1024-entry waveform-0 sine (2 KB) live on the HEAP while the chip is
   enabled, and every flat-table fetch is re-derived arithmetically (row shift
   + one's-complement sign for tl; waveforms 1-7 are index transforms of
   waveform 0). Bit-exact vs the flat tables — verified over every entry. The
   first cut kept the flat tables as const FLASH and op_calc's ~2.2M random
   lookups/s thrashed the shared XIP cache: an Adlib Tracker II rip dragged
   the whole emulator to 35.5 FPS (hw 2026-09-01). ~2.5 KB heap via the
   refcounted tablesReady() pattern.
 - Timers count in Q16 chip-sample units advanced from gen() instead of MAME's
   attotime callbacks. The chip is only asked for status from the Z80 port
   read, which catches the sample stream up first, so flag timing is sample-
   accurate (~32 us) — enough for the VGM plugin's ~950 us detect wait.
 - The struct padding MAME keeps "to pump the struct size to a power of 2" is
   dropped; state is ~9 KB on the heap (fn_tab kept — the vibrato path reads
   it per sample).
 - Only outputs A (left) and B (right) are accumulated — the card has a
   two-channel DAC; C/D are the OPL4-only DO0 pair.
 - A whole-chip quiet fast path: with every operator in EG_OFF, gen() only
   advances the timers. An envelope can leave EG_OFF only on a key-on, i.e. a
   register write, i.e. between gen() calls (the YMF262 has no CSM), and two
   silent samples always settle op1_out to zero, which the fast path forces.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef OplFm_h
#define OplFm_h

#include <inttypes.h>

// The AlexZor card clocks the YMF262 at the standard PC/AdLib-family rate.
// VGM wait commands carry the tune's own timing, so nothing else would even
// be observable; the timers derive from this too (T1 tick = 80.4 us).
#define OPL3_YMF262_CLOCK 14318180

class OplFm {
public:
    // The two shared lookup tables (~2.5 KB) live on the heap, reference
    // counted like OpnFm's: identical for every chip, freed with the last one.
    static bool tablesReady();

    OplFm();
    ~OplFm();

    // clock = YMF262 master clock in Hz, rate = our output sample rate.
    // Re-derives every rate-dependent constant; safe to call at any time.
    // halfRate: synthesize the chip at rate/2 and linearly interpolate x2 on
    // output — pitch is exact (every increment re-derives from the synth
    // rate), only content above rate/4 is lost. 18 channels x 2 operators at
    // the full 31250 Hz cost ~4-5k cycles per sample, which a 378 MHz core0
    // cannot afford on dense OPL3 scores (Doom: IDL -1750 us, hw 2026-09-01);
    // half-rate halves exactly the expensive part. The timers always count
    // real output time, so the VGM plugin's detect stays sample-exact.
    void setRates(int clock, int rate, bool halfRate = false);
    void reset();

    // The four Z80 ports: a = low two address bits, exactly MAME's OPL3Write.
    // 0 = address set #1 (#C4), 1/3 = data (#C5/#C7), 2 = address set #2 (#C6).
    void write(int a, uint8_t v);

    // Status register (#C4 read): bit 7 IRQ, bit 6 timer 1, bit 5 timer 2;
    // bits 4..0 read LOW on a real YMF262 (that is how software tells it from
    // an OPL2). The caller must catch the sample stream up first — the timers
    // advance in gen().
    uint8_t status() const { return m_status; }

    // Accumulates `count` samples into bufL/bufR starting at `bufpos`.
    // The caller clears the range first.
    void gen(int16_t* bufL, int16_t* bufR, int count, int bufpos);

    // True while the chip has produced sound within the last second. The
    // mixer's mid-scale re-centre (+DC on a 0..255 rail) must only be paid
    // while the chip actually plays — a merely ENABLED idle OPL3 would
    // otherwise push every AY/beeper program against the clip ceiling.
    bool audible() const { return m_quiet_samples < 31250; }

private:
    struct Slot {
        uint32_t ar, dr, rr;      // internal rates (0 or 16 + r<<2)
        uint8_t  KSR;             // 0 or 2
        uint8_t  ksl;             // ksl shift (31 = off)
        uint8_t  ksr;             // kcode >> KSR
        uint8_t  mul;             // mul_tab[ML]

        uint32_t Cnt;             // phase counter
        uint32_t Incr;            // phase increment
        uint8_t  FB;              // feedback shift (0 = none)
        uint8_t  conn_enum;       // slot output route (CONN_*)
        int32_t* connect;         // resolved from conn_enum
        int32_t  op1_out[2];      // slot1 feedback memory
        uint8_t  CON;

        uint8_t  eg_type;         // percussive / non-percussive
        uint8_t  state;           // EG_OFF..EG_ATT
        uint32_t TL;              // total level << 3
        int32_t  TLL;             // TL + KSL, what volume_calc uses
        int32_t  volume;          // envelope counter
        uint32_t sl;              // sustain level

        uint32_t eg_m_ar;  uint8_t eg_sh_ar,  eg_sel_ar;
        uint32_t eg_m_dr;  uint8_t eg_sh_dr,  eg_sel_dr;
        uint32_t eg_m_rr;  uint8_t eg_sh_rr,  eg_sel_rr;

        uint32_t key;             // 0 = off; bit0 = normal, bit1 = rhythm

        uint32_t AMmask;          // tremolo enable mask
        uint8_t  vib;             // vibrato enable

        uint8_t  waveform_number;
        uint8_t  wavetable;       // effective waveform 0-7 (OPL2 mode masks to 0-3)
    };

    struct Chan {
        Slot     SLOT[2];
        uint32_t block_fnum;
        uint32_t fc;
        uint32_t ksl_base;
        uint8_t  kcode;
        uint8_t  extended;        // first channel of an active 4-op pair
    };

    static void keyOn(Slot* s, uint32_t key_set);
    static void keyOff(Slot* s, uint32_t key_clr);
    void     slotConnect(Slot* s);
    void     statusSet(int flag);
    void     statusReset(int flag);
    void     statusMaskSet(int flag);
    void     advanceLfo();
    void     advance();
    void     chanCalc(Chan* CH);
    void     chanCalcOrSkip(Chan* CH);   // exact skip for a both-slots-EG_OFF channel
    void     pairCalc(int a);            // channel a + its 4-op partner a+3

    void     chanCalcExt(Chan* CH);
    void     chanCalcRhythm(unsigned int noise);
    void     calcFcSlot(Chan* CH, Slot* s);
    void     setMul(int slot, int v);
    void     setKslTl(int slot, int v);
    void     setArDr(int slot, int v);
    void     setSlRr(int slot, int v);
    void     writeReg(int r, int v);
    void     timerOver(int c);
    void     runTimers(int samples);   // Q16 countdowns, called from gen()
    void     renderSample(int32_t& a, int32_t& b);  // one chip sample (L, R)
    bool     allQuiet() const;

    Chan     m_ch[18];

    uint32_t m_pan[18 * 4];        // output masks, 4 per channel (A,B,C,D)
    uint8_t  m_pan_ctrl_value[18]; // raw c0-c8 values for mode switching

    int32_t  m_chanout[18];
    int32_t  m_phase_modulation;   // phase modulation input (SLOT 2)
    int32_t  m_phase_modulation2;  // phase modulation input (SLOT 3 of 4-op)

    uint32_t m_eg_cnt;
    // The envelope walk visits all 36 slots every EG tick (~49.7 kHz), yet
    // almost every visit is a no-op: the per-slot rate masks gate the action,
    // and a non-percussive slot in EG_SUS never acts at all. m_eg_next is the
    // soonest eg_cnt at which ANY slot can act — ticks below it skip the walk
    // entirely (bit-exact: a skipped tick is provably a no-op for every
    // slot). Recomputed after every walk; any register write sets m_eg_dirty
    // because it can change rates/states between walks.
    uint32_t m_eg_next;
    uint8_t  m_eg_dirty;
    uint32_t m_eg_timer;
    uint32_t m_eg_timer_add;
    uint32_t m_eg_timer_overflow;

    uint32_t m_fn_tab[1024];       // fnumber -> increment counter

    uint32_t m_LFO_AM;
    int32_t  m_LFO_PM;
    uint8_t  m_lfo_am_depth;
    uint8_t  m_lfo_pm_depth_range;
    uint32_t m_lfo_am_cnt, m_lfo_am_inc;
    uint32_t m_lfo_pm_cnt, m_lfo_pm_inc;

    uint32_t m_noise_rng;          // 23-bit noise shift register
    uint32_t m_noise_p;            // noise phase (Q16)
    uint32_t m_noise_f;            // noise period (Q16)

    uint8_t  m_OPL3_mode;          // reg 0x105 bit 0
    uint8_t  m_rhythm;             // reg 0xbd
    uint8_t  m_nts;                // reg 0x08

    int32_t  m_T[2];               // timer periods in chip samples
    int64_t  m_Tcnt[2];            // Q16 chip samples until overflow
    uint8_t  m_st[2];              // timer running flags
    uint32_t m_timer_step_q16;     // chip samples per output sample, Q16

    uint32_t m_quiet_samples;      // consecutive all-EG_OFF samples (saturating)

    // half-rate interpolation state (see setRates)
    uint8_t  m_half;               // synthesizing at rate/2
    uint8_t  m_half_tick;          // toggles: compute vs hold
    int32_t  m_pA, m_pB;           // previous chip sample
    int32_t  m_cA, m_cB;           // current chip sample

    uint32_t m_address;            // register-number latch (9 bits)
    uint8_t  m_status;
    uint8_t  m_statusmask;

    int      m_clock, m_rate;
};

// The one YMF262, allocated by OplSubsys while Config::opl3 is on.
extern OplFm* oplfm;

#endif // OplFm_h
