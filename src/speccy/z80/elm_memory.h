// eLeMeNt ZX operand accessors for Z80::exec_nocheck_elm (the Next-shaped opcode
// switch, tools/gen_next_dispatch.py's `elm` flavour). Include after CPU, VIDEO,
// MemESP and Z80Ops, next to next_memory.h, whose NX_WIN / lim_nx() the loop reuses.
//
// The Next's accessors are gate-free because every Next window lives below 0x4000.
// The eLeMeNt maps its windows anywhere: the HGFX VRAM / register / colour pages,
// the chunky and LCU slots, the FastDMA / SoundDMA / LCU2 pages, the HRXC colour
// table, FRAM / flash / write-protected URAM - all taken by Elm::memWrite - and in
// COPY mode a READ of the VRAM window arms the copy latch (Elm::memRead, the
// generic read funnel). Two per-8K-slot masks say where the plain load / store is
// NOT the whole story (Element.cpp elmMasksUpdate: bits 0-1 always, ROM / divSD /
// URAM zone 0 stay generic): one byte load + a shift + a branch per access, against
// the generic accessor's three gate bytes + the 0x4000 test + a call.
#pragma once
#include <stdint.h>

extern bool g_elm_fastrd;   // the loop's gate: eLeMeNt at fastmem level 1 (Video.cpp tsFastMemRecalc)
// The slot masks the loop reads are the shared g_z80_rd_fast / g_z80_wr_fast
// (next_memory.h; bit set = plain access), the complement of Element.cpp's
// g_elm_rdmask / g_elm_wrmask while the gate is on. Video.cpp recomputes them
// from both (elmFastSlotsUpdate) when either changes.
extern uint8_t g_z80_rd_fast, g_z80_wr_fast;
void elmFastSlotsUpdate();

namespace ElmMemory {

__attribute__((always_inline)) inline void addressOnBus_elm(uint32_t& t, uint16_t, int32_t cycles) {
    t += (uint32_t)cycles;
}
__attribute__((always_inline)) inline void addressOnBusIR_elm(uint32_t& t, int32_t cycles) {
    t += (uint32_t)cycles;
}

// The generic accessor for a masked slot: publishes the loop's T-state counter,
// takes it back. No line test here either - the loop's head decided this whole
// instruction ends before the next raster line (next_memory.h), and the generic
// accessor's own test is then a no-op.
uint32_t elmPeek8Slow(uint32_t t, uint32_t address);
uint32_t elmPeek16Slow(uint32_t t, uint32_t address);
void elmPoke8Slow(uint32_t t, uint32_t pc, uint32_t address, uint32_t value);
void elmPoke16Slow(uint32_t t, uint32_t pc, uint32_t address, uint32_t value);

__attribute__((always_inline)) inline uint8_t peek8_elm(uint32_t& t, uint16_t address) {
    if (__builtin_expect((g_z80_rd_fast >> (address >> 13)) & 1, 1)) {
        t += 3;
        return MemESP::ramCurrent[address >> 13][address & 0x1FFF];
    }
    const uint32_t v = elmPeek8Slow(t, address);
    t = CPU::tstates;
    return (uint8_t)v;
}

// A word inside one unmasked slot is one slot lookup; across two slots, or in a
// masked one, per byte out of line.
__attribute__((always_inline)) inline uint16_t peek16_elm(uint32_t& t, uint16_t address) {
    if (__builtin_expect(((g_z80_rd_fast >> (address >> 13)) & 1) && (address & 0x1FFF) != 0x1FFF, 1)) {
        t += 6;
        const uint8_t* const p = MemESP::ramCurrent[address >> 13] + (address & 0x1FFF);
        return (uint16_t)(p[0] | (p[1] << 8));
    }
    const uint32_t v = elmPeek16Slow(t, address);
    t = CPU::tstates;
    return (uint16_t)v;
}

// A write to an unmasked slot: +3 T, the page's dirty mark, the store - what
// Z80Ops::poke8's fast path does (tsPoke8Store) minus the TS-BIOS flash-pointer
// test, which no eLeMeNt page above 0x4000 needs (its ROM blocks are PSRAM copies,
// the 48K ROM sits in slots 0-1, masked).
__attribute__((always_inline)) inline void poke8_elm(uint32_t& t, uint16_t pc, uint16_t address, uint8_t value) {
    const uint32_t slot = address >> 13;
    if (__builtin_expect((g_z80_wr_fast >> slot) & 1, 1)) {
        t += 3;
        *mem_desc_t::bank_dirty[slot] = true;
        MemESP::ramCurrent[slot][address & 0x1FFF] = value;
        return;
    }
    elmPoke8Slow(t, pc, address, value);
    t = CPU::tstates;
}

// PUSH's two writes as one, high byte first like the two pokes it replaces.
__attribute__((always_inline)) inline void poke16_elm(uint32_t& t, uint16_t pc, uint16_t address, uint16_t value) {
    const uint32_t slot = address >> 13;
    if (__builtin_expect(((g_z80_wr_fast >> slot) & 1) && (address & 0x1FFF) != 0x1FFF, 1)) {
        t += 6;
        *mem_desc_t::bank_dirty[slot] = true;
        uint8_t* const p = MemESP::ramCurrent[slot] + (address & 0x1FFF);
        p[1] = (uint8_t)(value >> 8);
        p[0] = (uint8_t)value;
        return;
    }
    elmPoke16Slow(t, pc, address, value);
    t = CPU::tstates;
}

} // namespace ElmMemory
