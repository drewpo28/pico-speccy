/*

pico-speccy — the ZXM-MoonSound (OPL4) synthesized on core1.

An OPL4 track keys the FM half (OplFm) and up to 24 wave channels (MoonPcm):
measured 8.5-10.5 ms a frame on core0 at 504 MHz (TS-Conf + VGMPLAY.WMF,
"05 encounter", hw 2026-10-22: FM ~5.2, wave 3.4-5.3), on top of a 14 MHz
guest. Both halves are generated here on core1 WHILE core0 emulates the frame;
core0 only sums them at the frame end — and generates whatever core1 did not
get to, so a core1 with no slack (a 576p render, a card read) costs core0
exactly the remainder and never a wait.

The contract, all frame-relative sample positions:
 - core0 PUBLISHES how far the guest has got (`publish`, from the per-line
   hooks of the renderers and from every chip write) — core1 may only generate
   up to that point, so a register write queued later (its position is >= the
   published one by program order on core0) is never applied late;
 - FM port writes and wave register writes go into one ring with their sample
   position (`fmWrite` / `waveWrite`, core0, no lock); whoever generates
   applies them at those positions;
 - a generate step runs under ONE hardware spinlock (core1's pump, core0's
   `sync` — a status read needs the timers caught up, a wave read-back every
   write before it — and the frame-end `frameJoin`);
 - the FM half accumulates straight into the OPL buffer (zeroed by
   `frameBoundary`), the wave half into its own; `frameSum` scales the FM by
   the chip's FM mix level and adds the wave.

The memory-access registers, NEW2 and the ROM/RAM stay on core0 as before
(MoonPcm.h). Without the block (no heap) the old inline path in OPLGenSound
serves, what 1.1.0 did.

*/

#ifndef MoonSplit_h
#define MoonSplit_h

#include <stdint.h>

#if !defined(MOONSOUND) || MOONSOUND

class OplFm;

namespace MoonSplit {
    // Allocates the ring + the wave frame buffers and claims the lock; false =
    // keep the inline path.
    bool init();
    bool on();

    // core0, lock-free: the guest has reached sample `pos` of this frame (one
    // store; the per-line hooks call it).
    extern volatile uint32_t safe;
    inline void publish(uint32_t pos) { safe = pos; }
    // core0, lock-free: timed writes at `pos` — an FM port (0..3) / a wave register.
    void fmWrite(uint32_t pos, uint8_t port, uint8_t v);
    void waveWrite(uint32_t pos, uint8_t reg, uint8_t v);
    // core0: generate (applying queued writes) up to `pos` right now — an FM
    // status read (timers), a wave read-back, an immediate wave register.
    void sync(uint32_t pos);
    // core0: the lock, for a chip reset / rate change of the attached OplFm.
    void hold();
    void release();
    // core0: the FM half goes away (OplSubsys off) — before its buffers are freed;
    // and comes up (OplSubsys on, at applyPending — AFTER this frame's boundary,
    // so the frame's FM writes would otherwise be dropped): attach it now.
    void fmDetach();
    void fmAttach(OplFm* fm, int16_t* fmL, int16_t* fmR);
    // core0, frame start: the buffers hold `samples` this frame; the FM half to
    // generate into fmL/fmR (nullptr = wave only), zeroed here; cursors to 0;
    // stragglers of an unmixed frame applied directly.
    void frameBoundary(int samples, OplFm* fm, int16_t* fmL, int16_t* fmR);
    // core0, frame end: join the generation to `samples`, apply the entries at
    // or past the end. Returns whether the wave half sounded this frame.
    bool frameJoin(int samples);
    // core0, after frameJoin (and the FM buffer's DC blocker): L/R (the FM
    // half) scaled by the chip's FM mix level + the wave half.
    void frameSum(int16_t* L, int16_t* R, int samples);
    // core0: MoonPcm::reset + setRate under the lock (ESPectrum::reset).
    void resetChip(uint32_t rate);

    // core1 (render_core): one bounded step, never blocks on core0.
    void core1Pump();

    // OPL_PERF_TRACE: core1 microseconds in the FM / wave halves and core0
    // microseconds in the frame-end join since the last call (all reset).
    void perfTake(uint32_t& c1fm, uint32_t& c1wave, uint32_t& joinus);
}

// render_core's gate (main.cpp): a plain global, true once init() succeeded.
extern bool g_moon_c1_live;

#endif
#endif
