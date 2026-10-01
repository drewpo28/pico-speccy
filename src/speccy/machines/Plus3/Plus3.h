// pico-speccy — ZX Spectrum +3 paging and uPD765 port writes that used to sit
// inline in the RAM-resident Ports::output. FLASH-resident: see CLAUDE.md "Machine
// code must not live in the SHARED RAM hot paths".
#pragma once
#include <stdint.h>

namespace Plus3 {

// OUT on a +3: #7FFD / #1FFD (tight Fuse decodes) and the FDC's #3FFD / #2FFD.
// Returns true when the write was one of them and must not reach the loose
// 128K decode below.
bool portWrite(uint16_t address, uint8_t data);

}
