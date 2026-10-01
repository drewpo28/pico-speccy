// Host test for src/speccy/machines/TsConf/TsDram.h — the TS-Conf DMA duration arithmetic (DRAM cycles,
// video fetcher share, frame wrap, clock scaling). Builds against the shipped header:
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/tsdram_test tools/tsdram_test.cpp && /tmp/tsdram_test
// Re-run after any change to TsDram.h. The Bomberman Evolution numbers are the
// ones that decide whether its intro's screen copy spans the frame interrupt
// (see the DRAM model section in CLAUDE.md).
#include <cstdio>
#include <cstdint>
#include "speccy/machines/TsConf/TsDram.h"

static int fails = 0;
static void check(const char* what, double got, double lo, double hi) {
    const bool ok = got >= lo && got <= hi;
    printf("%-52s %10.3f  [%g..%g] %s\n", what, got, lo, hi, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}
static double lines(uint32_t dt, uint8_t m) { return (double)(dt >> m) / 224.0; }

int main() {
    const uint32_t frame14 = 320;   // TS-Conf: 320 raster lines
    // Bomberman: 14 MHz, NOGFX (VConfig 0x20). Wipe 65536 words RAM->RAM (2 cycles) from line 8;
    // copy 38400 words. At the DRAM peak: 292.57 and 171.43 lines.
    uint32_t t0 = (8 * 224) << 2;
    check("wipe 65536w NOGFX @14MHz, lines", lines(tsdram::dmaEnd(t0, 65536 * 2, 2, 0x20, frame14) - t0, 2), 292.5, 292.7);
    check("copy 38400w NOGFX @14MHz, lines", lines(tsdram::dmaEnd(t0, 38400 * 2, 2, 0x20, frame14) - t0, 2), 171.4, 171.5);
    // 256c 320x240 (0x82): visible lines 56..295 give the DMA 288 of 448 -> x1.556
    uint32_t t60 = (60 * 224) << 2;
    check("256c inside picture, 10 lines of cycles", lines(tsdram::dmaEnd(t60, 448 * 10, 2, 0x82, frame14) - t60, 2), 15.5, 15.6);
    uint32_t t300 = (300 * 224) << 2;
    check("256c in the border, 10 lines of cycles", lines(tsdram::dmaEnd(t300, 448 * 10, 2, 0x82, frame14) - t300, 2), 9.99, 10.01);
    uint32_t t315 = (315 * 224) << 2;
    check("wrap over frame end (border both sides)", lines(tsdram::dmaEnd(t315, 448 * 10, 2, 0x82, frame14) - t315, 2), 9.99, 10.01);
    // 16c 320 (0x81): 80 of 448 -> x1.217; ZX (0x00) 32 of 448 on lines 80..271 -> x1.077
    check("16c inside picture, 10 lines", lines(tsdram::dmaEnd(t60, 448 * 10, 2, 0x81, frame14) - t60, 2), 12.1, 12.3);
    uint32_t t100 = (100 * 224) << 2;
    check("ZX inside picture, 10 lines", lines(tsdram::dmaEnd(t100, 448 * 10, 2, 0x00, frame14) - t100, 2), 10.7, 10.8);
    // clock scaling: the same DMA takes the same WALL time at every ZCLK
    check("3.5 MHz 1 word = 2 cycles = 1 T", (double)(tsdram::dmaEnd(1000, 2, 0, 0x20, 320) - 1000), 1, 1);
    check("7 MHz 1 word = 2 tstates", (double)(tsdram::dmaEnd(1000, 2, 1, 0x20, 320) - 1000), 2, 2);
    check("14 MHz 1 word = 4 tstates", (double)(tsdram::dmaEnd(1000, 2, 2, 0x20, 320) - 1000), 4, 4);
    check("zero cycles still 1 tstate (DMA_ACT observable)", (double)(tsdram::dmaEnd(1000, 0, 2, 0x20, 320) - 1000), 1, 1);
    check("mid-line start, one full line of cycles @14", (double)(tsdram::dmaEnd(400, 448, 2, 0x20, 320) - 400), 896, 896);
    check("videoCyclesPerLine ZX", tsdram::videoCyclesPerLine(0x00), 32, 32);
    check("videoCyclesPerLine 256c 360", tsdram::videoCyclesPerLine(0xC2), 180, 180);
    check("videoCyclesPerLine text 320", tsdram::videoCyclesPerLine(0x83), 160, 160);
    check("videoCyclesPerLine NOGFX", tsdram::videoCyclesPerLine(0x22), 0, 0);
    printf(fails ? "FAIL (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
