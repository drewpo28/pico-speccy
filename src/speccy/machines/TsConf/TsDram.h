// TS-Conf DRAM arithmetic shared by TsConf.cpp and tools/tsdram_test.cpp — pure
// functions of their arguments so the host test builds against the shipped code.
//
// arbiter.v / dram.v / clock.v: one DRAM cycle = 4 FPGA clocks at 28 MHz = half a
// 3.5 MHz T-state, 448 cycles per 224-T line. The video fetcher takes its share
// only inside the visible window (video_sync.v: video_go = hvpix && !nogfx) at
// the rate video_mode.v's bw[] table gives per mode; the DMA gets the rest.
#pragma once
#include <stdint.h>

namespace tsdram {

static const uint32_t kLineT      = 224;   // 3.5 MHz T-states per line
static const uint32_t kLineCycles = 448;   // DRAM cycles per line

// DRAM cycles the video fetcher takes on a VISIBLE line: ZX '1 of 8' over the
// 256-px paper (16 bitmap + 16 attribute words), 16c '1 of 4', 256c '1 of 2',
// text '4 of 8' over the RRES width. NOGFX (VConfig b5) stops the fetcher.
static inline uint32_t videoCyclesPerLine(uint8_t vconf) {
    if (vconf & 0x20) return 0;
    static const uint16_t kHp[4] = { 256, 320, 320, 360 };
    const uint32_t hp = kHp[vconf >> 6];
    switch (vconf & 3) {
        case 0:  return 32;
        case 1:  return hp / 4;
        default: return hp / 2;
    }
}

// First visible raster line per RRES (video_sync.v vp_beg) and the visible height.
static inline uint32_t visibleBegin(uint8_t vconf) { static const uint16_t k[4] = { 80, 76, 56, 32 };    return k[vconf >> 6]; }
static inline uint32_t visibleLines(uint8_t vconf) { static const uint16_t k[4] = { 192, 200, 240, 288 }; return k[vconf >> 6]; }

// T-state (CPU::tstates units, i.e. 3.5 MHz T << m, counted from the raster
// origin) at which a DMA needing `cycles` DRAM cycles, launched at t0, drops
// DMA_ACT — with the video fetcher's share of every visible line it crosses
// taken out, spread evenly over the line. frameLines wraps the raster (320 on
// TS-Conf); 0 = no wrap. CPU accesses are added on top by the caller, live.
static inline uint32_t dmaEnd(uint32_t t0, uint32_t cycles, uint8_t m, uint8_t vconf, uint32_t frameLines) {
    const uint32_t vid = videoCyclesPerLine(vconf);
    const uint32_t vb = visibleBegin(vconf), ve = vb + visibleLines(vconf);
    const uint32_t T0 = t0 >> m;
    uint32_t line  = T0 / kLineT;
    uint32_t pos   = (T0 % kLineT) * 2;      // cycles already gone in this line
    uint32_t slots = 0;                       // DRAM slots (half-T) elapsed
    for (;;) {
        const uint32_t l = frameLines ? line % frameLines : line;
        const uint32_t v = (vid && l >= vb && l < ve) ? vid : 0;
        const uint32_t rem = kLineCycles - pos;
        const uint32_t share = rem - (v * rem) / kLineCycles;   // the DMA's cycles in the rest of this line
        if (cycles <= share) {
            slots += share ? (cycles * rem + share - 1) / share : rem;
            break;
        }
        cycles -= share; slots += rem; pos = 0; line++;
    }
    uint32_t dur = (slots << m) >> 1;
    if (!dur) dur = 1;                        // DMA_ACT must be observable at least once
    return t0 + dur;
}

} // namespace tsdram
