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
// 2 colours, the only hi-res one (16 pixels a slot): phase 0 shows its left pixel
// of each pair.
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
    uint8_t  pal[16] = {};
    uint16_t ld1 = 0, ld2 = 0;
    int      linesRemaining = 0;
    bool     displayEnabled = false, vsync = false;

    static constexpr int SLOTS = 57, FIRST = 8, LAST = 54, OUT_PX = (LAST - FIRST) * 8;

    void reset();
    void writePort(uint8_t port, uint8_t v);
    uint8_t readPort() const { return dataBus; }

    // One raster line. `out` takes OUT_PX Nick colour bytes (may be null: the line is
    // run without pixels). Returns L_* flags.
    enum { L_VSYNC = 1 };
    int line(uint8_t* out);

private:
    inline uint8_t v(uint16_t a) const { return vram[a >> 14][a & 0x3FFF]; }
    void setIrq(bool s);
    void slot(int s, uint8_t* o);
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
    Poly     p4{}, p5{}, p7{}, pvl{};
    uint8_t  noise7 = 0;           // #A6 bit 4: noise uses the 7-bit counter
    uint8_t  chIn[3] = {};         // #A1/3/5 bits 4-5: 0 square, 1 poly4, 2 poly5, 3 poly7/VL
    uint8_t  chHp[3] = {}, chRm[3] = {};
    uint8_t  st[4] = {}, prv[4] = {}, st1[4] = {}, st2_3 = 0; // channel outputs (3 = noise)
    uint8_t  n3Clk = 0, n3ClkPrv = 0, n3Lp = 0, n3Hp = 0, n3Rm = 0;
    uint8_t  clk62 = 0; int clk62Phase = 0;
    uint8_t  volL[4] = {}, volR[4] = {};
    uint8_t  dacL = 0, dacR = 0;
    uint8_t  outLv = 0, outRv = 0;     // this cycle's DAC output, 0..252 each

    // Output: every 8 Dave cycles (128 T = 31250 Hz) the average of the cycles goes
    // to outL/outR[outN] (glue-owned buffers, nullptr = no sound wanted).
    uint8_t* outL = nullptr; uint8_t* outR = nullptr;
    int      outN = 0, outCap = 0;
    uint16_t accL = 0, accR = 0; uint8_t accN = 0;

    void reset(bool cold);
    void write(uint8_t r, uint8_t v);          // r = port & 0x1F
    uint8_t read(uint8_t r) const;
    void tick(int n);                          // n Dave cycles at 250 kHz
    void setInt1(bool state);
    bool irq() const { return (actSnd | actHz | act1 | act2) != 0; }
    int  waitMode() const { return (sysconf >> 2) & 3; }
    int  cycleT() const { return (sysconf & 2) ? 24 : 16; } // 4 MHz T a Dave cycle (#BF bit 1)

private:
    void step();                               // one Dave cycle: timers + sound
    void trigSnd() { if (!actSnd) actSnd = 1; }
    void trigHz()  { if (!actHz) actHz = 1; }
};

// Nick colour byte -> RGB888 (ep128emu Nick::convertPixelToRGB: R = b0 b3 b6,
// G = b1 b4 b7, B = b2 b5, most significant bit first).
uint32_t colourRgb(uint8_t c);

} // namespace EpCore
