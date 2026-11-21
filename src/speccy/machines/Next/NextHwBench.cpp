// [Z80HW] (CMake Z80_HWBENCH, test builds only): what one Z80 instruction costs core0 on the
// board, against the z80bench figures (QEMU, ARM instructions). Once, ~12 s into a Next
// session: tools/z80bench's `game` / `high` workloads (mkmix.py, org #8000) run through the
// real loop (Z80::exec_nocheck -> exec_nocheck_next) in
// slots 4-7 swapped for scratch pages - the guest's memory and state are put back after.
// No raster ticks (ts_line_t parked), no Z80 interrupts (exec_nocheck only). Each run is a
// fixed T budget; the line gives us per run, M1 (the fetch counter), ns/M1 and core0 cycles
// per M1 at clk_sys. Runs:
//   mem  P  = code and data pages in butter PSRAM, S = code page in SRAM (data PSRAM),
//        SS = code and data in SRAM (slots 5-7 share one page: the workloads only need the
//        stack consistent)
//   irq  on = as the frame runs (HDMI / audio ISRs on core0), off = IRQs masked (the run is
//        ~1-3 ms: an HDMI hiccup is possible)
//   (core A was the generated asm loop of 2026-11-21, removed: same cycles as C on hw)
// The hot-page pool gives its frame back for the SRAM runs (re-taken ~2 s later).
#if Z80_HWBENCH && !HOST_BUILD
#include "app/Debug.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "speccy/z80/z80.h"
#include "speccy/z80/CPU.h"
#include "speccy/core/MemESP.h"
#include "speccy/video/Video.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include <cstring>
#include <cstdlib>
extern bool g_next_fastrd;


namespace NextHwBench {

static const uint8_t k_game[] = {
    0x31, 0x00, 0xF0, 0x21, 0x00, 0xC0, 0x06, 0x00, 0x70, 0x23, 0x10, 0xFC, 0x21, 0x00, 0xC0, 0x11, 0x00, 0xC8, 0x06, 0x40, 0x0E, 0x03, 0x7E, 0x23, 0xA9, 0x4F, 0xE6, 0x1F, 0xFE, 0x10, 0x38, 0x03, 0xD6, 0x08, 0x07, 0x12, 0x13, 0x78, 0x81, 0x57, 0x7A, 0xB7, 0x16, 0xC8, 0x20, 0x01, 0x0C, 0x10, 0xE5, 0x06, 0x18, 0xCD, 0x59, 0x80, 0x10, 0xFB, 0x3A, 0x10, 0xC0, 0x3C, 0x32, 0x10, 0xC0, 0xDD, 0x21, 0x00, 0xC1, 0x06, 0x08, 0xDD, 0x7E, 0x00, 0xDD, 0x86, 0x01, 0xDD, 0x77, 0x02, 0xDD, 0x23, 0xCB, 0x3F, 0xCB, 0x47, 0x10, 0xEF, 0xC3, 0x0C, 0x80, 0xC5, 0xE5, 0xD5, 0x2A, 0x20, 0xC0, 0x11, 0x05, 0x00, 0x19, 0x22, 0x20, 0xC0, 0x7C, 0xE6, 0x07, 0x6F, 0x26, 0xC2, 0x7E, 0x3C, 0x77, 0x47, 0x0E, 0x00, 0x78, 0xE6, 0x03, 0x47, 0x04, 0x0C, 0x79, 0x87, 0x4F, 0x10, 0xFA, 0xEB, 0x1B, 0x7A, 0xB3, 0x28, 0x01, 0x13, 0xD1, 0xE1, 0xC1, 0xC9 };
static const uint8_t k_high[] = {
    0x31, 0x00, 0xF0, 0x21, 0x00, 0xA0, 0x11, 0x01, 0xA0, 0x01, 0xFF, 0x0F, 0x36, 0x5A, 0xED, 0xB0, 0x21, 0x00, 0xA0, 0x11, 0x00, 0xC0, 0x01, 0x00, 0x01, 0xED, 0xB0, 0xDD, 0x21, 0x00, 0xA1, 0x06, 0x40, 0xDD, 0x7E, 0x00, 0xDD, 0xAE, 0x01, 0xDD, 0x77, 0x02, 0xDD, 0xCB, 0x03, 0x06, 0xDD, 0x23, 0x10, 0xEF, 0x21, 0x00, 0xB0, 0x06, 0x80, 0x0E, 0x00, 0x7E, 0x81, 0x77, 0x23, 0xCB, 0x47, 0x28, 0x01, 0x0C, 0x10, 0xF5, 0x06, 0x20, 0xCD, 0x67, 0x80, 0x10, 0xFB, 0xFD, 0x21, 0x00, 0xA8, 0x06, 0x20, 0xFD, 0x7E, 0x00, 0xFD, 0x86, 0x01, 0xFD, 0x77, 0x00, 0xFD, 0x23, 0xFD, 0x23, 0xED, 0x44, 0xCB, 0x3F, 0x10, 0xED, 0xC3, 0x10, 0x80, 0xC5, 0xE5, 0xEB, 0x2A, 0x00, 0xA0, 0x19, 0x22, 0x02, 0xA0, 0xE1, 0xC1, 0xC9 };

static constexpr uint32_t RUN_T = 300000;   // ~10.7 ms of a 28 MHz Next; a few ms of core0

static void runOne(const char* wl, const uint8_t* body, size_t n, uint8_t* const pg[4], const char* mem, bool irqOff, bool asmOn) {
    for (int s = 0; s < 4; s++) MemESP::ramCurrent[4 + s] = pg[s];
    memset(pg[0], 0, 0x2000);
    memcpy(pg[0], body, n);
    Z80::setRegAF(0); Z80::setRegBC(0); Z80::setRegDE(0); Z80::setRegHL(0);
    Z80::setRegIX(0); Z80::setRegIY(0); Z80::setRegSP(0xF000); Z80::setRegPC(0x8000);
    Z80::setHalted(false); Z80::setPendingEI(false);
    if (asmOn) return;   // (the asm loop is gone, 2026-11-21: the C loop's cycles on hw)
    CPU::tstates = 0;
    CPU::stFrame = RUN_T;
    const uint32_t r0 = Z80::getFetchCounter();
    const uint32_t st = irqOff ? save_and_disable_interrupts() : 0;
    const uint32_t t0 = time_us_32();
    while (CPU::tstates < CPU::stFrame) Z80::exec_nocheck();
    const uint32_t us = time_us_32() - t0;
    if (irqOff) restore_interrupts(st);
    const uint32_t m1 = Z80::getFetchCounter() - r0;
    const uint32_t mhz = clock_get_hz(clk_sys) / 1000000;
    Debug::log("[Z80HW] %-4s mem=%-2s irq=%-3s core=%c gate=%d: %lu T in %lu us, M1 %lu, %lu ns/M1, %lu.%lu cyc/M1 @%lu MHz",
               wl, mem, irqOff ? "off" : "on", asmOn ? 'A' : 'C', (int)g_next_fastrd, (unsigned long)CPU::tstates, (unsigned long)us,
               (unsigned long)m1, m1 ? (unsigned long)((uint64_t)us * 1000 / m1) : 0,
               m1 ? (unsigned long)((uint64_t)us * mhz / m1) : 0, m1 ? (unsigned long)((uint64_t)us * mhz * 10 / m1 % 10) : 0,
               (unsigned long)mhz);
}

// once per frame from NextNeg::frame (flash)
void tick() {
    static uint32_t frames = 0;
    if (++frames != 600) return;
    // the state to put back
    const uint16_t af = Z80::getRegAF(), bc = Z80::getRegBC(), de = Z80::getRegDE(), hl = Z80::getRegHL();
    const uint16_t ix = Z80::getRegIX(), iy = Z80::getRegIY(), sp = Z80::getRegSP(), pc = Z80::getRegPC(), wz = Z80::getMemPtr();
    const uint8_t r = Z80::getRegR();
    const bool halted = Z80::isHalted(), pei = Z80::isPendingEI();
    const uint32_t ts = CPU::tstates, sf = CPU::stFrame, lt = VIDEO::ts_line_t;
    uint8_t* slots[4]; for (int s = 0; s < 4; s++) slots[s] = MemESP::ramCurrent[4 + s];
    VIDEO::ts_line_t = UINT32_MAX;          // no raster tick inside the runs
    MemESP::nextHotRelease();               // its 8K back to the heap for the SRAM pages
    uint8_t* ps[4] = {};
    for (int s = 0; s < 4; s++) ps[s] = (uint8_t*)Buffer::palloc(0x2000, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    uint8_t* sc = (uint8_t*)tryMalloc(0x2000);
    uint8_t* sd = (uint8_t*)tryMalloc(0x2000);
    Debug::log("[Z80HW] pages: psram %p %p %p %p, sram %p %p; run %lu T", ps[0], ps[1], ps[2], ps[3], sc, sd, (unsigned long)RUN_T);
    struct WL { const char* n; const uint8_t* b; size_t len; } wls[2] = { {"game", k_game, sizeof k_game}, {"high", k_high, sizeof k_high} };
    for (const WL& w : wls) {
        for (int a = 0; a < 2; a++) {
            if (ps[0] && ps[1] && ps[2] && ps[3]) {
                uint8_t* p[4] = { ps[0], ps[1], ps[2], ps[3] };
                runOne(w.n, w.b, w.len, p, "P", false, a);
                runOne(w.n, w.b, w.len, p, "P", true, a);
            }
            if (sc && ps[1] && ps[2] && ps[3]) {
                uint8_t* p[4] = { sc, ps[1], ps[2], ps[3] };
                runOne(w.n, w.b, w.len, p, "S", false, a);
                runOne(w.n, w.b, w.len, p, "S", true, a);
            }
            if (sc && sd) {
                uint8_t* p[4] = { sc, sd, sd, sd };
                runOne(w.n, w.b, w.len, p, "SS", false, a);
                runOne(w.n, w.b, w.len, p, "SS", true, a);
            }
        }
    }
    for (int s = 0; s < 4; s++) MemESP::ramCurrent[4 + s] = slots[s];
    if (sc) free(sc);
    if (sd) free(sd);
    for (int s = 0; s < 4; s++) if (ps[s]) Buffer::pfree(ps[s]);
    Z80::setRegAF(af); Z80::setRegBC(bc); Z80::setRegDE(de); Z80::setRegHL(hl);
    Z80::setRegIX(ix); Z80::setRegIY(iy); Z80::setRegSP(sp); Z80::setRegPC(pc); Z80::setMemPtr(wz);
    Z80::setRegR(r); Z80::setHalted(halted); Z80::setPendingEI(pei);
    CPU::tstates = ts; CPU::stFrame = sf; VIDEO::ts_line_t = lt;
    Debug::log("[Z80HW] done, guest state back");
}

} // namespace NextHwBench
#endif
