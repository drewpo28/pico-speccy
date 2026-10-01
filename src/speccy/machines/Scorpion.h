// pico-speccy — Scorpion-family port handling (Scorpion ZS-256/1024, GMX,
// ProfROM, Nemo KAY, ZXM-Phoenix) that used to sit inline in Ports::output.
//
// Everything here runs from FLASH: Ports::output is RAM-resident and executed
// by every machine, so a Scorpion branch inlined there cost every other machine
// its bytes of SRAM. The RAM side keeps only the address decode and one call.
// See CLAUDE.md "Machine code must not live in the SHARED RAM hot paths".
#pragma once
#include <stdint.h>

namespace Scorpion {

// The RAM page at 0xC000 for a 7FFD bits 0-2 value, composed with the
// family's extra page bits (1FFD D4/D6/D7, GMX DFFD, KAY 7FFD D7).
uint32_t c000Page(uint32_t low3);

// Nemo KAY / ZXM-Phoenix #1FFD write ((address & 0xC003) == 0x0001).
void kay1FFDWrite(uint8_t data);

// Scorpion / GMX / ProfROM #1FFD write ((address & 0xC002) == 0 && A5).
void write1FFD(uint16_t address, uint8_t data);

// Turbo+ speed toggle: an IN from (address & 0x8023) == 0x0021, A14 picks
// 3.5 / 7 MHz (MAME scorpiontb_state::scorpion_io). Returns the bus value, 0xFF.
uint8_t turboPlusRead(uint16_t address);

}
