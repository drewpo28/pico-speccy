// pico-speccy — Enterprise 64/128 (A_ENT): the Nick video chip and the Dave
// system chip, line-granular. Depends on nothing from the firmware, so the host
// simulator (tools/ep_sim.cpp) runs exactly this code.
//
// Model: ep128emu by Istvan Varga (github.com/istvan-v/ep128emu, src/nick.cpp,
// src/dave.cpp, src/ep128vm.cpp; GPL-2.0+). Plan and status: docs/enterprise-plan.md.
//
// Nick: 57 slots a line (889846 Hz slot clock). Slots 0..7 read the 16-byte line
// parameter block (LPB) at the current LPT address — every line, so the palette and
// the mode of a block are re-read per line —, slots 8..53 are the display area
// (16 hi-res pixels each), slot 56 steps the LPT. Here one call of line() is one
// raster line; the slot state machine of ep128emu runs inside it unchanged.
//
// Output: 8 LORES pixels a slot (one lores pixel = two hi-res pixels), slots 8..53
// = 368 Nick colour bytes. Every mode is exact at that resolution except PIXEL with
// 2 colours, the only hi-res one (16 pixels a slot): `out` shows the left pixel of
// each pair, `hiOut` (if set) the full 16.
//
// Dave: the timers run at 250 kHz (the 1 kHz / 50 Hz / 1 Hz chain and the tone
// channels' phase counters — the sound interrupt can follow channel 0 or 1). The
// four interrupt sources are edge-latched and held until a write to #B4 clears them;
// the CPU sees the OR of the latched, enabled ones. Sound output is phase 1.
#pragma once

#include <stdint.h>

namespace EpCore {

struct Nick {
    // VRAM = segments FC..FF, 16K each (the only memory Nick can see).
    uint8_t* vram[4] = {nullptr, nullptr, nullptr, nullptr};
    // INT1 (the video interrupt line) follows the LPB interrupt flag.
    void (*irqCb)(void* ud, bool state) = nullptr;
    void* ud = nullptr;

    // Registers (ports #80..#83).
    uint8_t  port0 = 0, border = 0, port3 = 0xF0;
    uint16_t lptBase = 0, lptCur = 0;
    uint8_t  lptFlags = 0;
    // The line parameter block being displayed.
    int      nLines = 1;
    bool     irqFlag = false, vres = false, reload = true;
    uint8_t  colorMode = 0, videoMode = 7;
    bool     altInd0 = false, altInd1 = false, lsbAlt = false, msbAlt = false;
    uint8_t  leftMargin = 8, rightMargin = 54;
    uint8_t  dataBus = 0xFF;
    uint8_t  pal[16] = {};         // Nick colours (the LPB's 8 + the fixed bias 8)
    uint16_t ld1 = 0, ld2 = 0;
    int      linesRemaining = 0;
    bool     displayEnabled = false, vsync = false;

    static constexpr int SLOTS = 57, FIRST = 8, LAST = 54, OUT_PX = (LAST - FIRST) * 8;

    Nick();
    void reset();
    void writePort(uint8_t port, uint8_t v);
    uint8_t readPort() const { return dataBus; }

    // One raster line. `out` takes OUT_PX bytes (may be null: the line is run without
    // pixels), 8 a slot, **in the framebuffer's word order**: the 32-bit word at
    // pixel 4q holds pixels 4q+2, 4q+3, 4q, 4q+1 (byte 0 first) — a row written this
    // way goes to the display as is. Each byte is outMap[Nick colour]. Returns L_*.
    // Slots outside [visLo, visHi) go to `aux` (same layout, may be null) instead of
    // `out`: the caller clips the line to its row and copies the edge slots itself.
    enum { L_VSYNC = 1, L_HIRES = 2 };   // L_HIRES: a 2-colour PIXEL slot was displayed
    int line(uint8_t* out, uint8_t* aux, int visLo, int visHi);
    int line(uint8_t* out) { return line(out, nullptr, FIRST, LAST); }
    // Optional, OUT_PX * 2 bytes: the line at the 16-px-a-slot resolution of
    // 2-colour PIXEL mode (every other mode's pixels written twice), LINEAR order,
    // outMap applied. nullptr = off.
    uint8_t* hiOut = nullptr;
    static constexpr int HI_PX = OUT_PX * 2;
    // Output byte per Nick colour (256 entries); the default is the identity.
    void setOutMap(const uint8_t* m);

private:
    const uint8_t* outMap;
    uint8_t  palO[16] = {};        // outMap[pal[i]]
    __attribute__((always_inline)) inline uint8_t v(uint16_t a) const { return vram[a >> 14][a & 0x3FFF]; }
    void setIrq(bool s);
    template <int VM> bool slotT(uint8_t* o, uint8_t* hi);   // true: a 16-px slot
    template <int VM> int spanT(int n, uint8_t* o, int ostep, uint8_t* hi);
    int span(int n, uint8_t* o, int ostep, uint8_t* hi);
};

struct Dave {
    // Memory page callback: window 0..3 now shows segment `seg`.
    void (*pageCb)(void* ud, int win, uint8_t seg) = nullptr;
    void* ud = nullptr;

    uint8_t  page[4] = {0, 0, 0, 0};
    uint8_t  sysconf = 0;          // #BF: bits 2-3 wait mode (0 all, 1 M1 only, 2/3 none)
    uint8_t  kbRow = 0;
    uint8_t  reg[32] = {};         // the raw write latch (sound registers, phase 1)
    uint8_t  keys[16];             // keyboard rows 0..9, active low (host side writes)

    // Interrupts: snd = 1 kHz / 50 Hz / tone ch0 / ch1, hz = 1 Hz, i1 = Nick, i2 = external.
    uint8_t  enSnd = 0, enHz = 0, en1 = 0, en2 = 0;
    uint8_t  actSnd = 0, actHz = 0, act1 = 0, act2 = 0;
    uint8_t  stSnd = 0, stHz = 0, st1_ = 1, st2 = 1;   // source states (#B4 read)
    uint8_t  sndSrc = 0;           // #A7 bits 5-6
    int      clk1000 = 0, clk50 = 0, clk1 = 0;
    int      chFrq[3] = {0, 0, 0}, chPhase[3] = {0, 0, 0};
    uint8_t  chRun[3] = {1, 1, 1};

    // Sound (ep128emu Dave::runOneCycle_). The polynomial counters are LFSRs run
    // forward, which is exactly the order ep128emu reads its reversed tables in;
    // only a change of the variable-length counter's length differs (ep128emu
    // keeps the phase modulo the new length, here the register state carries over).
    struct Poly { uint32_t sr, m0, m1, mask; uint8_t out; };
    Poly     pvl{};                // the 9..17-bit one: stepped lazily (pvlPend)
    uint32_t pvlPend = 0;          // free-running steps not applied to pvl yet
    // The 4/5/7-bit counters are tables (built once from the same LFSRs): ph =
    // steps taken mod the period, out = the output of the last step.
    struct Tab { uint8_t ph, out; };
    Tab      t4{}, t5{}, t7{};
    uint8_t  noise7 = 0;           // #A6 bit 4: noise uses the 7-bit counter
    uint8_t  chIn[3] = {};         // #A1/3/5 bits 4-5: 0 square, 1 poly4, 2 poly5, 3 poly7/VL
    uint8_t  chHp[3] = {}, chRm[3] = {};
    uint8_t  st[4] = {}, prv[4] = {}, st1[4] = {}, st2_3 = 0; // channel outputs (3 = noise)
    uint8_t  n3Clk = 0, n3ClkPrv = 0, n3Lp = 0, n3Hp = 0, n3Rm = 0;
    uint8_t  clk62 = 0; int clk62Phase = 0;
    uint8_t  volL[4] = {}, volR[4] = {};
    uint8_t  dacL = 0, dacR = 0;
    uint8_t  outLv = 0, outRv = 0;     // this cycle's DAC output, 0..252 each
    uint32_t stPrev = 0xFFFFFFFFu;     // st[0..3] when outLv/outRv were summed (~0 = resum)

    // Output: every 128 T of the 4 MHz clock (31250 Hz, 624 a frame) the T-weighted
    // average of the Dave levels goes to outL/outR[outN] (glue-owned buffers,
    // nullptr = no sound wanted). Not every 8 Dave cycles: with #BF bit 1 a cycle is
    // 24 T, and counting cycles gave 416 samples a frame (a 50 Hz chop).
    uint8_t* outL = nullptr; uint8_t* outR = nullptr;
    int      outN = 0, outCap = 0;
    uint32_t accL = 0, accR = 0; uint32_t accT = 0;
    uint32_t cycles = 0;               // Dave cycles since reset (tests: the write log)
    uint32_t steps = 0;                // full step() calls (perf)

    void reset(bool cold);
    void write(uint8_t r, uint8_t v);          // r = port & 0x1F
    uint8_t read(uint8_t r) const;
    void tick(int n);                          // n Dave cycles at 250 kHz
    void setInt1(bool state);
    bool irq() const { return (actSnd | actHz | act1 | act2) != 0; }
    int  waitMode() const { return (sysconf >> 2) & 3; }
    int  cycleT() const { return (sysconf & 2) ? 24 : 16; } // 4 MHz T a Dave cycle (#BF bit 1)
    // Cycles from now until the first one that can move the #B4 status (the 1 kHz
    // chain, or the sound interrupt's channel running out): a status read before
    // that cycle completes sees the current latches, so it needs no catch-up.
    uint32_t statusCycles() const;

private:
    // The channel-class masks tick() skips with, derived from the registers alone:
    // recomputed only after a register write.
    uint8_t  idleC = 0, sqC = 0, nzC = 0;
    bool     maskDirty = true;
    void step();                               // one Dave cycle: timers + sound
    void pvlSync();                            // apply pvlPend
    void skip(uint32_t k, unsigned idle, bool nz);
    unsigned idleMask() const;
    unsigned sqMask(unsigned idle) const;
    bool noiseIdle() const;
    bool chFree(int c) const;
    void emitSq(uint32_t k, unsigned sq, unsigned ct);
    uint8_t clk62After(uint32_t m) const;
    void sumOut();                             // outLv/outRv from st[], the volumes, DAC mode                     // k quiet cycles in closed form
    void emit(unsigned t);                     // t T at the current level into samples
    void trigSnd() { if (!actSnd) actSnd = 1; }
    void trigHz()  { if (!actHz) actHz = 1; }
};

// Nick colour byte -> RGB888 (ep128emu Nick::convertPixelToRGB: R = b0 b3 b6,
// G = b1 b4 b7, B = b2 b5, most significant bit first).
uint32_t colourRgb(uint8_t c);

} // namespace EpCore
