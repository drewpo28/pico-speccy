// eLeMeNt ZX (A_ELM): a real Z80 on an FPGA board, base personality Pentagon 1024,
// no FDC (esxDOS over DivSD). docs/element-zx-plan.md holds the sources, phases and
// decisions; the reference is the Programmer's Reference (Google Doc, core 20261012).
// There is no RTL: what the document leaves open is settled by the programs.
//
// Phase 0: the three ZXi register files as read-back storage, the identity and date
// the software checks, "no MB03+". Registers that switch hardware are stored only.
#pragma once
#include <stdint.h>

namespace Elm {

void reset();                                 // from CPU::reset
bool portIn(uint16_t address, uint8_t* out);  // true = claimed
bool portOut(uint16_t address, uint8_t data);

// The ports the machine claims ahead of the generic decode, tested in the RAM path.
static inline bool isPort(uint16_t address) {
    return (uint8_t)address == 0x3B || (address & 0xFEFF) == 0x9EFD;
}

}
