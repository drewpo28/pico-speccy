// eLeMeNt ZX (A_ELM): a real Z80 on an FPGA board, base personality Pentagon 1024,
// no FDC (esxDOS over DivSD). docs/element-zx-plan.md holds the sources, phases and
// decisions; the reference is the Programmer's Reference (Google Doc, core 20261012).
// There is no RTL: what the document leaves open is settled by the programs.
//
// The three ZXi register files, the identity and date the software checks, "no MB03+"
// and URAM with LCU1. Registers of what is not built yet are stored only.
#pragma once
#include <stdint.h>

namespace Elm {

void reset();                                 // from CPU::reset
bool portIn(uint16_t address, uint8_t* out);  // true = claimed
bool portOut(uint16_t address, uint8_t data);

void frameEnd();                              // EndFrame
void remap();                                 // URAM back over the legacy paging
bool memWrite(uint16_t address, uint8_t v);   // true = the store is consumed
void busWrite(uint16_t address, uint8_t v);   // a DMA write, windows included
bool nmiBlocked();                            // SafeNMI: the button is locked
void nmiTaken(uint16_t pc);                   // SafeNMI: the return address
extern uint8_t* wrKind;                       // per high address byte: 0 = plain RAM

// The ports the machine claims ahead of the generic decode, tested in the RAM path:
// #xx3B, #9EFD/#9FFD, the URAM ports #07 #27 #47 #67 #53 #73 and the DACs #0F-#7F
// (low byte only). portIn leaves the DAC ports to the generic decode.
static inline bool isPort(uint16_t address) {
    const uint8_t lo = (uint8_t)address;
    return lo == 0x3B || (lo & 0x9F) == 0x07 || (lo & 0xDF) == 0x53 ||
           (lo & 0x8F) == 0x0F || lo == 0x2B || (address & 0xF0FF) == 0x0003 ||
           (address & 0xFEFF) == 0x9EFD || address == 0xEFF7;
}

}
