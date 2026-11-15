// Host test for src/speccy/machines/Next/NextInt.cpp (Next interrupt controller
// + CTC). Expected values are derived from the VHDL (ctc_chan.vhd prescaler and
// ZC/TO, im2_device.vhd daisy chain, zxnext.vhd 1925-2085 / 5590-5640 / 6230-6263,
// zxula_timing.vhd 546-585), not from the implementation.
//
//   g++ -O2 -Wall -Wextra -Itools/nextint_stub -Isrc -o /tmp/nextint_test
//       tools/nextint_test.cpp src/speccy/machines/Next/NextInt.cpp && /tmp/nextint_test
// (the stub dir FIRST: its CPU.h / z80.h / ESPectrum.h / Video.h stand in for the firmware's)
#include <cstdio>
#include "speccy/machines/Next/NextInt.h"
#include "speccy/machines/Next/NextReg.h"
#include "speccy/machines/Next/NextMf.h"
#include "app/ESPectrum.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"
#include "speccy/video/Video.h"

// ---- stubs ----
uint8_t ESPectrum::multiplicator = 0;
uint32_t CPU::tstates = 0, CPU::statesInFrame = 70908, CPU::stFrame = 0;
uint8_t CPU::latetiming = 0;
int32_t CPU::IntStart = 0, CPU::IntEnd = 36;
Z80::IntMode Z80::im = Z80::IM1;
uint16_t VIDEO::tStatesPerLine = 228;
uint8_t NextReg::reg[256];
uint8_t NextReg::portFF = 0;
bool NextReg::portFFIntDisable = false;
uint16_t NextReg::paperLineV = 63;
static uint8_t g_timing = 2;   // 128K
uint8_t NextReg::machineTiming() { return g_timing; }
bool NextMf::active() { return false; }
#include "speccy/devices/Z80DMA.h"
bool Z80DMA::nxPending = false, Z80DMA::nxStall = false;   // NextInt::reschedule
uint32_t Z80DMA::nxNextT = 0;

static int g_fail = 0, g_n = 0;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t rd(uint8_t r) { uint8_t o = 0xEE; NextInt::nrRead(r, o); return o; }
static void at(uint32_t t) { CPU::tstates = t; }
// frame wrap exactly as CPU::loop does it
static void wrap() { NextInt::frameEnd(); CPU::tstates -= CPU::statesInFrame; }
static void fresh() {
    CPU::tstates = 0; CPU::statesInFrame = 70908; ESPectrum::multiplicator = 0;
    CPU::IntEnd = 36; VIDEO::tStatesPerLine = 228; NextReg::paperLineV = 63; g_timing = 2;
    for (auto& r : NextReg::reg) r = 0;
    NextReg::portFF = 0; NextReg::portFFIntDisable = false; Z80::im = Z80::IM1;
    NextInt::reset(true);
}


// Differential trace for NextInt::ctcWrite's independent-timer fast path: build
// twice (with and without -DNXI_NO_WFAST) and diff the output.
//   for f in "" -DNXI_NO_WFAST; do g++ -O2 $f -Itools/nextint_stub -Isrc -Iexternal -o /tmp/wf$f \
//       tools/nextint_wfast_test.cpp src/speccy/machines/Next/NextInt.cpp; done
//   diff <(/tmp/wf) <(/tmp/wf-DNXI_NO_WFAST) && echo same
static uint32_t rs = 12345;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
int main() {
    unsigned long long h = 1469598103934665603ull; unsigned long lines = 0;
    auto mix = [&](unsigned long long v) { h ^= v; h *= 1099511628211ull; };
    for (int scen = 0; scen < 4000; scen++) {
        fresh();
        ESPectrum::multiplicator = (uint8_t)(rnd() % 4);
        NextInt::nrWrite(0xC0, (uint8_t)((rnd() & 1) ? 0x01 : 0x00));   // hardware IM2 on / off
        Z80::im = (rnd() & 1) ? Z80::IM2 : Z80::IM1;
        uint32_t t = 0;
        const int style = rnd() % 4;       // 0: SID-like only, 1: + plain TC rewrites, 2: anything, 3: counters too
        for (int step = 0; step < 400; step++) {
            t += (style == 0 || (rnd() & 3)) ? rnd() % 300 : rnd() % 6000;
            if (t >= CPU::statesInFrame) { CPU::tstates = CPU::statesInFrame; (void)NextInt::intLine(); wrap(); t -= CPU::statesInFrame; }
            at(t);
            const int ch = rnd() % 4;
            const int op = rnd() % 16;
            if (op < 6) {                   // reset + TC follows, then the constant
                const uint8_t cw = (uint8_t)(0x07 | ((rnd() & 1) ? 0x80 : 0) | ((rnd() % 5 == 0) ? 0x20 : 0)
                                             | (style == 3 && rnd() % 6 == 0 ? 0x40 : 0)
                                             | (style >= 2 && rnd() % 6 == 0 ? 0x08 : 0)
                                             | (style >= 2 && (rnd() & 1) ? 0x10 : 0));
                NextInt::ctcWrite((uint8_t)ch, cw);
                if (style == 0 || rnd() % 8) NextInt::ctcWrite((uint8_t)ch, (uint8_t)(rnd() % (rnd() & 1 ? 256 : 12)));
            } else if (op < 9 && style >= 1) {   // TC follows without reset, then the constant
                NextInt::ctcWrite((uint8_t)ch, (uint8_t)(0x05 | (rnd() & 0x80) | ((style >= 2) ? (rnd() & 0x30) : 0)));
                NextInt::ctcWrite((uint8_t)ch, (uint8_t)rnd());
            } else if (op == 9 && style >= 2) {
                NextInt::ctcWrite((uint8_t)ch, (uint8_t)rnd());   // anything
            } else if (op == 10) {
                NextInt::ctcWrite((uint8_t)ch, (uint8_t)(0x03 | (rnd() & 0x80)));   // reset, stay stopped
            } else if (op == 11) {
                mix(NextInt::ctcRead((uint8_t)ch));
            } else if (op == 12) {
                if (NextInt::intLine()) { mix(NextInt::ack()); if (rnd() & 1) NextInt::reti(); }
            } else if (op == 13) {
                NextInt::nrWrite(0xC9, (uint8_t)rnd());           // clear status bits
            }
            const bool il = NextInt::intLine();
            const uint32_t ne = NextInt::nextEventT(CPU::statesInFrame);
            mix(il); mix(ne); mix(rd(0xC9)); mix(rd(0x20)); mix(NextInt::ex);
            for (uint8_t c = 0; c < 4; c++) mix(NextInt::ctcRead(c));
            if ((step & 63) == 0) { printf("%d.%d t=%u il=%d ne=%u c9=%02X r=%02X%02X%02X%02X h=%016llx\n", scen, step, t, il, ne,
                rd(0xC9), NextInt::ctcRead(0), NextInt::ctcRead(1), NextInt::ctcRead(2), NextInt::ctcRead(3), h); lines++; }
        }
    }
    printf("final %016llx (%lu lines)\n", h, lines);
    return 0;
}
