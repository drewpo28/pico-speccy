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

// 16col (#EFF7 D0) whole-line path — see VIDEO::col16DrawTick. One content line
// (32 byte-columns) out of the four planes into `dst` (the fb row at the content
// offset, x^2 order), `off` = offBmp[line]; byte for byte what MainScreen's 16col
// branch writes, the per-column timing replaced by one call at the line's end.
void col16RenderLine(uint32_t* dst, uint16_t off, const uint8_t* const planes[4],
                     const uint16_t* lut);

// Can the TS-Conf fast guest-memory path (TsFastMem.h: plain pointer banks, no
// overlays, no DivMMC) run on this Pentagon session right now? Evaluated once per
// frame at the EndFrame arming; the per-access gates (ZX-DMA, DivMMC mapped,
// breakpoints) stay in VIDEO::tsFastMemRecalc.
bool col16FastMemOk();

}
