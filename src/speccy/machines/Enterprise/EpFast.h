// pico-speccy — Enterprise 64/128: the memory-access timing the Z80's RAM-resident
// accessors (CPU.cpp Z80Ops::peek8/poke8/peek16/poke16/fetchOpcode and the fetch
// inside Z80::exec_nocheck) apply INLINE while the Enterprise runs, instead of the
// generic chain (VIDEO::Draw pointer -> Ep::draw -> gsDmaPeek8 -> readbyte ->
// romPeek, three calls an access; ~37% of the Z80's share, hw 2026-10-30).
//
// Marker: MEMGATES_EP in g_memgates (bit 6 of the atm_ro byte, set by Ep::mapWin
// beside the read-only window bits); the accessors test the gate word they have
// already loaded. State: Ep::lineT (the next Nick line end), Ep::frac57 (the wait
// states' carried 1/57 T), Ep::wait1Rd / wait1Op (#BF wait mode: +1 T on a non-VRAM
// data access / M1). Same arithmetic as Ep::draw / drawOpcode, which still serve
// the internal cycles (addressOnBus) and the generic path (memory breakpoints).
#pragma once

#include <stdint.h>
#include "speccy/z80/CPU.h"
#include "speccy/machines/Enterprise/Ep.h"

namespace Ep {
    extern uint32_t lineT;           // frame T of the next line end
    extern uint32_t frac57;          // wait states: carried 1/57 T
    extern uint8_t  wait1Rd, wait1Op;

    // A VRAM access waits for the next Nick slot boundary (slots = 256/57 T) plus the
    // RAM latency: `pre57` T x57 before the sync point, `post57` after it, `base` =
    // the access's own T, which the caller adds itself.
    static __attribute__((always_inline)) inline void vramWait(uint32_t pre57, uint32_t post57, uint32_t base) {
        const uint32_t s = CPU::tstates * 57u + frac57;
        const uint32_t end = ((s + pre57 + 255u) & ~255u) + post57;
        uint32_t extra = end - s;
        extra = extra > base * 57u ? extra - base * 57u : 0;
        frac57 += extra;
        CPU::tstates += frac57 / 57u;
        frac57 %= 57u;
    }
    // A data access (3 T) / an M1 (4 T) in a window showing VRAM (vram) or not.
    static __attribute__((always_inline)) inline void tickRd(bool vram) {
        if (__builtin_expect(CPU::tstates >= lineT, 0)) catchUp();
        if (vram) { vramWait(2 * 57, 57 + 82, 3); CPU::tstates += 3; }
        else CPU::tstates += 3u + wait1Rd;
    }
    static __attribute__((always_inline)) inline void tickOp(bool vram) {
        if (__builtin_expect(CPU::tstates >= lineT, 0)) catchUp();
        if (vram) { vramWait(86, 142 + 81, 4); CPU::tstates += 4; }
        else CPU::tstates += 4u + wait1Op;
    }
}
