// pico-speccy — Pentagon 512 / 1024SL (and Profi, which shares them) port handling
// that used to sit inline in the RAM-resident Ports::input/output. FLASH-resident:
// these ports are written a few times per program, not per frame, and every other
// machine paid their SRAM. See CLAUDE.md "Machine code must not live in the SHARED
// RAM hot paths".
#pragma once
#include <stdint.h>

namespace Pentagon {

// OUT (#EFF7) exact address: latch + 16-colour mode (D0).
void eff7Video(uint8_t data);

// OUT to the #EFF7 family ((address & 0xF008) == 0xE000): page0 overlay /
// 128K lock-disable (D2/D3) and the 1024SL turbo-off bit D4.
void eff7Paging(uint8_t data);

// IN (#FB) / IN (#7B): hidden RAM on / off (Pentagon 512/1024, Profi). Returns 0xFF.
uint8_t hiddenRam(uint8_t p8);

}
