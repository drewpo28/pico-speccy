/*

pico-speccy — YM2413 (OPLL) FM synthesis, for the DivMMC VGM-player sound
card (address port #C0, data port #C1).

The core is a port of MAME's ym2413.cpp (Copyright Jarek Burczynski, license
GPL-2.0+, taken from tag mame0220 — the same lineage as OpnFm/OplFm): 9
channels x 2 operators, the 15 built-in instrument patches + 1 user patch,
rhythm mode (5 drums on channels 6-8), vibrato/tremolo LFO, the OPLL dump
phase (key-on ramps the old note down, THEN resets phase and attacks — the
chip never clicks between notes). Write-only silicon: no status register and
no timers, so software plays it blind.

Deliberate differences, the same set as OplFm (see there for the war
stories): the tl/sin tables are re-derived arithmetically from a 256-entry
base row + waveform-0 sine on the HEAP (refcounted tablesReady()); MAME's
native-rate stream (clock/72) is rate-converted through freqbase; a
whole-chip quiet fast path plus per-channel EG_OFF skips; the envelope walk
computes its own next-actionable bound inline (percussive sustain clamped at
maximum attenuation is excluded — it can never act again until a register
write, and melody-mode modulators are excluded in EG_REL, which the chip
never lets them perform); optional half-rate synthesis with x2 linear
interpolation for slow system clocks (pitch exact — every constant
re-derives from the synth rate).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef OpllFm_h
#define OpllFm_h

#include <inttypes.h>

// The standard OPLL clock (SMS FM unit, MSX-MUSIC, and every VGM rip seen).
#define OPLL_YM2413_CLOCK 3579545

class OpllFm {
public:
    // Shared lookup tables (~2.5 KB heap), reference counted like OpnFm's.
    static bool tablesReady();

    OpllFm();
    ~OpllFm();

    // clock = YM2413 master clock in Hz, rate = our output sample rate.
    // halfRate: synthesize at rate/2 with x2 linear interpolation (see OplFm).
    void setRates(int clock, int rate, bool halfRate = false);
    void reset();

    void writeAddr(uint8_t v) { m_address = v; }
    void writeData(uint8_t v) { writeReg(m_address, v); }

    // Accumulates `count` mono samples (melody + rhythm) into buf[bufpos..];
    // the caller clears the range first.
    void gen(int16_t* buf, int count, int bufpos);

    // True while the chip has produced sound within the last second — gates
    // the mixer's mid-scale re-centre exactly like OplFm::audible().
    bool audible() const { return m_quiet_samples < 31250; }

private:
    struct Slot {
        uint32_t ar, dr, rr;      // internal rates (0 or 16 + r<<2)
        uint8_t  KSR;             // 0 or 2
        uint8_t  ksl;             // ksl shift (31 = off)
        uint8_t  ksr;             // kcode >> KSR
        uint8_t  mul;             // mul_tab[ML]

        uint32_t phase;           // phase counter
        uint32_t freq;            // phase increment
        uint8_t  fb_shift;        // feedback shift (0 = none)
        int32_t  op1_out[2];      // slot1 feedback memory

        uint8_t  eg_type;         // percussive / non-percussive
        uint8_t  state;           // EG_OFF..EG_DMP
        uint32_t TL;              // total level
        int32_t  TLL;             // TL + KSL
        int32_t  volume;          // envelope counter
        uint32_t sl;              // sustain level

        uint8_t  eg_sh_dp,  eg_sel_dp;   // dump phase (key-on ramp-down)
        uint8_t  eg_sh_ar,  eg_sel_ar;
        uint8_t  eg_sh_dr,  eg_sel_dr;
        uint8_t  eg_sh_rr,  eg_sel_rr;
        uint8_t  eg_sh_rs,  eg_sel_rs;   // release-sustain rates

        uint32_t key;             // 0 = off; bit0 = normal, bit1 = rhythm

        uint32_t AMmask;          // tremolo enable mask
        uint8_t  vib;             // vibrato enable
        uint8_t  wavetable;       // 0 = sine, 1 = half sine
    };

    struct Chan {
        Slot     SLOT[2];
        uint32_t block_fnum;
        uint32_t fc;
        uint32_t ksl_base;
        uint8_t  kcode;
        uint8_t  sus;             // sustain-mode flag (reg 0x20 bit 5)
    };

    static void keyOn(Slot* s, uint32_t key_set);
    static void keyOff(Slot* s, uint32_t key_clr);
    void     calcFcSlot(Chan* CH, Slot* s);
    void     setMul(int slot, int v);
    void     setKslTl(int chan, int v);
    void     setKslWaveFb(int chan, int v);
    void     setArDr(int slot, int v);
    void     setSlRr(int slot, int v);
    void     loadInstrument(int chan, int slot, const uint8_t* inst);
    void     updateInstrumentZero(uint8_t r);
    void     writeReg(int r, int v);
    void     advanceLfo();
    void     advance();
    void     chanCalc(Chan* CH);
    void     chanCalcOrSkip(Chan* CH);
    void     rhythmCalc(unsigned int noise);
    void     renderSample(int32_t& out);
    bool     allQuiet() const;

    Chan     m_ch[9];
    uint8_t  m_instvol_r[9];       // instrument/volume latches
    uint8_t  m_inst_tab[19][8];    // patch RAM (0 = user, 1-15 ROM, 16-18 drums)

    int32_t  m_out_melody, m_out_rhythm;

    uint32_t m_eg_cnt;
    uint32_t m_eg_next;            // soonest actionable eg_cnt (see OplFm)
    uint8_t  m_eg_dirty;
    uint32_t m_eg_timer;
    uint32_t m_eg_timer_add;
    uint32_t m_eg_timer_overflow;

    uint32_t m_fn_tab[1024];

    uint32_t m_LFO_AM;
    int32_t  m_LFO_PM;
    uint32_t m_lfo_am_cnt, m_lfo_am_inc;
    uint32_t m_lfo_pm_cnt, m_lfo_pm_inc;

    uint32_t m_noise_rng;
    uint32_t m_noise_p;
    uint32_t m_noise_f;

    uint8_t  m_rhythm;             // reg 0x0e latch (bits 5..0)
    uint8_t  m_address;

    uint32_t m_quiet_samples;

    // half-rate interpolation state (see OplFm)
    uint8_t  m_half;
    uint8_t  m_half_tick;
    int32_t  m_prev, m_cur;

    int      m_clock, m_rate;
};

// The one YM2413, allocated by OpllSubsys while Config::ym2413 is on.
extern OpllFm* opllfm;

#endif // OpllFm_h
