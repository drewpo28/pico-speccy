/*

pico-speccy — YM2203 (OPN) FM synthesis, the FM half of a TurboSound FM chip.

The FM core is a compact re-derivation of MAME's fm.cpp OPN implementation
(Copyright Jarek Burczynski, Copyright Tatsuyuki Satoh, license GPL-2.0+),
reduced to what a YM2203 needs: 3 channels x 4 operators, no LFO (the YM2203
has none), no ADPCM, no SSG (the SSG half is our AySound), no pan (mono).

The tables, the envelope rate/shift/increment tables, the detune table and the
sine/attenuation math are MAME's, and the register semantics follow it exactly;
the state was flattened into a class and the two big lookup tables MAME keeps as
static arrays (fn_table, 16 KB, and tl_tab, 26 KB) are computed on the fly
instead, which is what makes the core fit on an RP2350.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef OpnFm_h
#define OpnFm_h

#include <inttypes.h>

// A TFM board clocks each YM2203 at TWICE the AY clock: the CPLD contains a
// delay-line frequency doubler (turbofm.tdf, `CLK2OUT = INTDELAY_OUT xor CLK1`,
// CLK1 = "AY clk generator"). That is not a detail — a YM2203's SSG half divides
// the master clock by 2 at the reset prescaler, so 2x is exactly what puts the
// PSG channels back on the ZX's own 1.75 MHz and makes the board a drop-in AY
// replacement. The FM half runs at clock/72.
// AYEMU_DEFAULT_CHIP_FREQ (AySound.cpp) is the AY clock and never varies by arch.
#define TSFM_YM2203_CLOCK (1773400 * 2)

class OpnFm {
public:
    // The two shared lookup tables (sine + attenuation, ~2.5 KB) live on the heap
    // and are reference counted: they are identical for every chip and there is no
    // reason to pay for them while TurboSound FM is off. Every constructed OpnFm
    // holds one reference.
    static bool tablesReady();

    OpnFm();
    ~OpnFm();

    // clock = YM2203 master clock in Hz, rate = our output sample rate.
    // Re-derives every rate-dependent constant; safe to call at any time.
    void setRates(int clock, int rate);
    void reset();

    // The register-number write (A0 = 0). The FM half keeps its OWN copy of the
    // address latch rather than reading AySound's: on a TFM board each YM2203 has
    // one latch shared by both halves, but our AySound chip1 only exists while
    // Config::turbosound is on, and ayChipFor() silently falls back to chip0 when
    // it does not — which would land every chip-1 FM write on chip 0.
    // 0x2D..0x2F additionally drive the prescaler, on the address write itself.
    void writeAddr(uint8_t r);
    // The data write (A0 = 1) for the currently latched register number.
    // Registers below 0x20 are the SSG half and are NOT ours (AySound has them).
    void writeData(uint8_t v);

    // OPN status: bit 1 = timer B overflow, bit 0 = timer A overflow. BUSY (bit 7)
    // is the caller's business — we complete every write instantly, so it is 0.
    uint8_t status() const { return m_status; }

    // Accumulates `bufsize` samples starting at `buf[bufpos]`. The caller clears
    // the range first (both chips of a TFM board sum into one FM output).
    void gen(int16_t* buf, int bufsize, int bufpos);

private:
    struct Slot {
        uint8_t  dt;          // detune index 0..7 (bit 2 = negative), MAME's DT row
        uint8_t  KSR;         // 3 - KSR
        uint8_t  ksr;         // kcode >> KSR
        uint16_t mul;         // multiple: 1 or 2*MUL
        uint16_t ar, d1r, d2r, rr;   // internal rates (0 or 32 + 2*r)

        uint32_t phase;
        int32_t  Incr;        // -1 = needs a refresh

        uint8_t  state;       // EG_OFF..EG_ATT
        uint32_t tl;          // total level << 3
        int32_t  volume;      // envelope counter
        uint32_t sl;          // sustain level
        uint32_t vol_out;     // volume + tl, what the operator actually uses

        uint8_t  eg_sh_ar,  eg_sel_ar;
        uint8_t  eg_sh_d1r, eg_sel_d1r;
        uint8_t  eg_sh_d2r, eg_sel_d2r;
        uint8_t  eg_sh_rr,  eg_sel_rr;

        uint8_t  ssg, ssgn;   // SSG-EG waveform / negate flag
        uint8_t  key;
    };

    struct Chan {
        Slot     SLOT[4];
        uint8_t  ALGO;
        uint8_t  FB;          // feedback shift (0 = none)
        int32_t  op1_out[2];
        int32_t *connect1, *connect2, *connect3, *connect4;
        int32_t *mem_connect;
        int32_t  mem_value;
        uint32_t fc;          // phase increment for the channel frequency
        uint8_t  kcode;
        uint32_t block_fnum;
    };

    void     writeMode(int r, int v);       // 0x20-0x2f
    void     writeReg(int r, int v);        // 0x30-0xff
    void     prescalerWrite(int addr);
    void     setupConnection(Chan* ch, int idx);
    void     refreshChan(Chan* ch);
    void     refreshSlot(Slot* s, int fc, int kc);
    void     advanceEg(Slot* s);
    void     chanCalc(Chan* ch);
    void     setTimers(int v);
    void     timerAOver();
    void     timerBOver();
    void     statusSet(uint8_t f)   { m_status |= f; }
    void     statusReset(uint8_t f) { m_status &= ~f; }
    void     keyOn(Chan* ch, int s);
    void     keyOff(Chan* ch, int s);
    uint32_t fnCalc(uint32_t fn, uint8_t blk) const;
    int32_t  dtVal(uint8_t dt, int kc) const;

    Chan     m_ch[3];

    // Phase-modulation scratch shared by the connection pointers, exactly as in
    // MAME: an operator writes its output into m2/c1/c2/mem or straight into the
    // channel's out_fm slot, and the algorithm decides which.
    int32_t  m_m2, m_c1, m_c2, m_mem;
    int32_t  m_out_fm[3];

    int      m_clock, m_rate;
    uint8_t  m_prescaler_sel;
    uint32_t m_fnMulQ16;     // fn -> phase increment (before the block shift)
    uint32_t m_fnMax;        // phase overflow wrap (Nemesis' detune-bug fix)
    uint32_t m_dtScaleQ16;   // raw detune table entry -> phase increment
    uint32_t m_egTimerAdd;
    uint32_t m_egTimer;
    uint32_t m_egCnt;
    uint32_t m_timerStepQ16; // timer decrement per output sample

    uint32_t m_mode;         // register 0x27
    uint8_t  m_addr;         // register-number latch (this chip's, see writeAddr)
    uint8_t  m_status;
    uint8_t  m_fn_h;         // 0xa4-0xa6 latch
    int32_t  m_TA;
    int32_t  m_TAC;          // Q16
    uint8_t  m_TB;
    int32_t  m_TBC;          // Q16

    // Channel 3 special ("3-slot") mode: each operator gets its own frequency.
    struct {
        uint32_t fc[3];
        uint8_t  fn_h;
        uint8_t  kcode[3];
        uint32_t block_fnum[3];
    } m_sl3;
};

// The FM halves of the two YM2203s, allocated by TsfmSubsys while
// Config::tsfm is on and null otherwise. Indexed by AySound::selected_chip.
extern OpnFm* opnfm[2];

#endif // OpnFm_h
