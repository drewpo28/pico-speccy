// Next prefix-byte fetch. Include after Z80, Z80Ops, CPU, VIDEO, MemESP and
// DivMMC. The first M1 uses exec_nocheck_next; CB/DD/ED/FD use this second M1.
#pragma once
#include <stdint.h>

extern bool g_next_fastrd;

namespace NextOpcode {
__attribute__((always_inline)) inline uint8_t fetch() {
#if !DEBUG
    const uint32_t end = CPU::tstates + 4;
    if (g_next_fastrd && end < VIDEO::ts_line_t) {
        const uint16_t pc = Z80::getRegPC();
        CPU::tstates = end;
        if (DivMMC::enabled) return DivMMC::nextOpcode(pc);
        return MemESP::ramCurrent[pc >> 13][pc & 0x1FFF];
    }
#endif
    // Let the original fetch process a raster/MMU change before loading the
    // byte. Debug builds also keep its dbg_last_pc instrumentation.
    return Z80Ops::fetchOpcode();
}

// Z80::exec_nocheck_next fetches inline (no time test: next_memory.h); this is
// what it calls for the second M1 of an ED instruction on a divMMC trap page —
// the automap hooks around the load, or the generic fetch at a raster line.
// Z80_JLS.cpp, .nxovl. Publishes PC and the T-states itself.
uint32_t fetchSlow(uint32_t t, uint32_t pc);
}
