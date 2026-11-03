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
void flush(bool force = false);               // flash image / FRAM write-back (main loop; force on reboot)
bool enterSetup();                            // next reset boots the SetUp ROM (block 7); false = not flashed
// The SetUp leaves by RST 0 (Shift+R, Shift+L): the board restarts, so the emulator
// does a machine reset too instead of letting the ROM run on into a warm esxDOS.
// rst0 is called from the Z80 core on every RST 0 (flash, rare); takeReset is
// polled at the frame end (ESPectrum.cpp) and clears the request.
void rst0(uint16_t pc);
bool takeReset();
// The personality e_zxi_000 names, applied at reset only (the SetUp restarts, FLASH6
// flips it and back without one): CPU::reset arms it before its timing chain, then
// cpuFlags() sets the Z80Ops machine flags, the floating bus and the frame target
// the way the stock 48K / 128K / +2A / Pentagon branches would.
void armPersonality();
void cpuFlags();
void remap();                                 // URAM back over the legacy paging
bool memWrite(uint16_t address, uint8_t v);   // true = the store is consumed
void memRead(uint16_t address);               // a read of the HGFX VRAM window arms COPY
extern volatile uint8_t g_elm_vram_rd;        // non-zero while memRead is wanted (COPY mode)
bool fastReadOk();                            // the fast read path may bypass memRead
void busWrite(uint16_t address, uint8_t v);   // a DMA write, windows included
uint8_t busRead(uint16_t address);            // a DMA read: arms the HGFX COPY latch too
bool nmiBlocked();                            // SafeNMI: the button is locked
void nmiTaken(uint16_t pc);                   // SafeNMI: the return address
extern uint8_t* wrKind;                       // per high address byte: 0 = plain RAM

// The ports the machine claims ahead of the generic decode, tested in the RAM path:
// #xx3B, #9EFD/#9FFD, the URAM ports #07 #27 #47 #67 #53 #73 and the DACs #0F-#7F
// (low byte only), and #7FFD for the 48K personalities' ROM select. portIn leaves the
// DAC ports and #7FFD to the generic decode.
static inline bool isPort(uint16_t address) {
    const uint8_t lo = (uint8_t)address;
    return lo == 0x3B || (lo & 0x9F) == 0x07 || (lo & 0xDF) == 0x53 ||
           (lo & 0x8F) == 0x0F || lo == 0x2B || (address & 0xF0FF) == 0x0003 ||
           (address & 0xFEFF) == 0x9EFD || address == 0xEFF7 || address == 0x7FFD;
}

}
