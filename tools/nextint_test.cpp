// Host test for src/speccy/machines/Next/NextInt.cpp (Next interrupt controller
// + CTC). Expected values are derived from the VHDL (ctc_chan.vhd prescaler and
// ZC/TO, im2_device.vhd daisy chain, zxnext.vhd 1925-2085 / 5590-5640 / 6230-6263,
// zxula_timing.vhd 546-585), not from the implementation.
//
//   g++ -O2 -Wall -Wextra -Itools/nextint_stub -Isrc -Iexternal -o /tmp/nextint_test
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

int main() {
    // ---- reset values (zxnext.vhd 5092-5105, read mux 6230-6263) ----
    fresh();
    CHECK(NextInt::ex == 0, "ex after reset %u", NextInt::ex);
    CHECK(rd(0xC0) == 0x02, "C0 = %02X (IM1 -> bits 2:1 = 01)", rd(0xC0));
    CHECK(rd(0xC4) == 0x81, "C4 = %02X", rd(0xC4));
    at(1000); CHECK(rd(0x22) == 0x00, "22 = %02X", rd(0x22));
    CHECK(NextInt::ctcRead(0) == 0, "ctc0 = %02X (tc reset 0)", NextInt::ctcRead(0));

    // ---- CTC timer, /16, TC 30: ZC every 480 28-MHz cycles = 60 T at 3.5 MHz ----
    fresh();
    at(100);
    NextInt::ctcWrite(0, 0x85);   // int on, timer, /16, falling, auto, TC follows, control
    NextInt::ctcWrite(0, 30);
    CHECK(NextInt::ex == 1, "ex with CTC int");
    CHECK(rd(0xC5) == 0x01, "C5 = %02X", rd(0xC5));
    at(108);   // 64 cycles: ticks at 15,31,47,63 -> count 26
    CHECK(NextInt::ctcRead(0) == 26, "count %u", NextInt::ctcRead(0));
    // first ZC at run0 + 30*16 - 1 = cycle 479 -> T 100 + ceil(479/8) = 160
    CHECK(NextInt::nextEventT(70000) == 160, "next %u", NextInt::nextEventT(70000));
    at(159); CHECK(!NextInt::intLine(), "no INT before ZC");
    at(160); CHECK(NextInt::intLine(), "pulse at ZC");
    CHECK(rd(0x20) == 0x01, "NR20 = %02X", rd(0x20));
    CHECK(rd(0xC9) == 0x01, "C9 = %02X", rd(0xC9));
    // pulse 36 CPU cycles in 128K timing (zxnext.vhd 2030): cycles 1279..1566
    at(195); CHECK(NextInt::intLine(), "pulse still up");
    at(196); CHECK(!NextInt::intLine(), "pulse ended");
    NextInt::nrWrite(0xC9, 0x01);
    CHECK(rd(0x20) == 0x00, "status cleared, NR20 = %02X", rd(0x20));
    // 11 periods on: status set again, the counter just reloaded
    at(100 + 60 * 11 + 1);
    CHECK(rd(0x20) == 0x01, "status after later ZC");
    CHECK(NextInt::ctcRead(0) == 30, "count right after a reload %u", NextInt::ctcRead(0));

    // ---- prescaler 256, TC 0 = 256: period 65536 cycles = 8192 T ----
    fresh();
    NextInt::ctcWrite(1, 0xA5);   // int on, /256, TC follows
    NextInt::ctcWrite(1, 0);
    CHECK(NextInt::nextEventT(70000) == 8192, "next %u", NextInt::nextEventT(70000));

    // ---- counter chain: ch1 counts ch0's ZC/TO (TC 3) ----
    fresh();
    NextInt::ctcWrite(0, 0x05);   // ch0 timer, no int, /16, TC 10
    NextInt::ctcWrite(0, 10);
    NextInt::ctcWrite(1, 0xC5);   // ch1 int, counter, TC follows
    NextInt::ctcWrite(1, 3);
    // ch0 ZCs at T 20, 40, 60 (ceil): ch1 ZC with the third
    CHECK(NextInt::nextEventT(70000) == 60, "chain next %u", NextInt::nextEventT(70000));
    at(45); CHECK(NextInt::ctcRead(1) == 1, "ch1 count %u", NextInt::ctcRead(1));
    CHECK(!NextInt::intLine(), "no INT from ch0 (int off)");
    CHECK((rd(0x20) & 0x0F) == 0x01, "ch0 status set without int enable");
    at(60); CHECK(NextInt::intLine(), "ch1 pulse");
    CHECK((rd(0x20) & 0x0F) == 0x03, "ch0+ch1 status");
    // ch3 -> ch0 ring of counters only: never fires
    fresh();
    for (int ch = 0; ch < 4; ch++) { NextInt::ctcWrite(ch, 0xC5); NextInt::ctcWrite(ch, 1); }
    CHECK(NextInt::nextEventT(70000) == 70000, "ring of counters %u", NextInt::nextEventT(70000));

    // ---- timer waiting for a trigger (bit 3) starts on the previous channel's ZC ----
    fresh();
    NextInt::ctcWrite(1, 0x05); NextInt::ctcWrite(1, 10);     // ZC at T 20
    NextInt::ctcWrite(2, 0x8D); NextInt::ctcWrite(2, 1);      // int, timer, trigger, TC 1
    CHECK(NextInt::ctcRead(2) == 1, "held at TC");
    // started at ch1's ZC (cycle 159), ZC 16 cycles later at 174 -> T 22 (ceil)
    CHECK(NextInt::nextEventT(70000) == 22, "trigger next %u", NextInt::nextEventT(70000));
    at(30); CHECK((rd(0x20) & 0x04) == 0x04, "ch2 fired after trigger");

    // ---- software reset stops counting; ch4-7 do not exist ----
    fresh();
    NextInt::ctcWrite(0, 0x85); NextInt::ctcWrite(0, 10);
    NextInt::ctcWrite(0, 0x03);   // control, reset, no TC
    at(1000); CHECK(rd(0x20) == 0, "no ZC after reset");
    NextInt::ctcWrite(5, 0x85); NextInt::ctcWrite(5, 10);
    CHECK(NextInt::ctcRead(5) == 0, "ch5 reads 0");

    // ---- line interrupt position (zxula_timing.vhd 561-585) ----
    // 128K: hc_ula 255 = 126 T after c_int_h on the line BEFORE the target
    fresh();
    NextInt::nrWrite(0x23, 10);
    NextInt::nrWrite(0x22, 0x02);
    CHECK(NextInt::ex == 1, "ex with line int");
    const uint32_t L10 = (63 + 9) * 228 + 126;
    CHECK(NextInt::nextEventT(70000) == L10, "line 10 at %u, want %u", NextInt::nextEventT(70000), L10);
    at(L10 - 1); CHECK(!NextInt::intLine(), "before line");
    at(L10);     CHECK(NextInt::intLine(), "line int");
    CHECK(rd(0x20) & 0x80, "line status");
    CHECK(rd(0xC8) == 0x02, "C8 = %02X", rd(0xC8));
    NextInt::nrWrite(0xC8, 0x02);
    CHECK(rd(0xC8) == 0x00, "C8 cleared");
    // line 0 = the last raster line before the paper; NR 0x64 offsets the count
    fresh();
    NextInt::nrWrite(0x22, 0x02);   // line 0
    CHECK(NextInt::nextEventT(70000) == 62u * 228 + 126, "line 0 %u", NextInt::nextEventT(70000));
    NextReg::reg[0x64] = 5; NextInt::nrWrite(0x64, 5);
    // cvc = vc - paper + 5 -> cvc 310 (line 0 - 1) is paper row 305 -> frame row (63+305)%311 = 57
    CHECK(NextInt::nextEventT(70000) == 57u * 228 + 126, "line 0 offset 5 %u", NextInt::nextEventT(70000));
    // 48K +128, Pentagon +190 (INT near the end of line 319)
    fresh(); g_timing = 1; VIDEO::tStatesPerLine = 224; NextReg::paperLineV = 64; CPU::statesInFrame = 69888;
    NextInt::nrWrite(0x23, 1); NextInt::nrWrite(0x22, 0x02);
    CHECK(NextInt::nextEventT(70000) == 64u * 224 + 128, "48K line 1 %u", NextInt::nextEventT(70000));
    fresh(); g_timing = 4; VIDEO::tStatesPerLine = 224; NextReg::paperLineV = 80; CPU::statesInFrame = 71680;
    NextInt::nrWrite(0x23, 1); NextInt::nrWrite(0x22, 0x02);
    CHECK(NextInt::nextEventT(71000) == 80u * 224 + 190, "Pentagon line 1 %u", NextInt::nextEventT(71000));
    // the line beyond the frame never fires
    fresh(); NextInt::nrWrite(0x23, 0xFF); NextInt::nrWrite(0x22, 0x03);   // 511
    CHECK(NextInt::nextEventT(70000) == 70000, "line 511 never");

    // ---- one latch: port #FF bit 6 = NR 0x22 bit 2 = NOT NR 0xC4 bit 0 ----
    fresh();
    NextInt::nrWrite(0xC4, 0x00);
    CHECK(NextReg::portFFIntDisable && (NextReg::portFF & 0x40), "C4 bit0=0 disables ULA");
    CHECK(rd(0x22) == 0x04, "22 = %02X", rd(0x22));
    NextInt::nrWrite(0x22, 0x00);
    CHECK(!NextReg::portFFIntDisable && rd(0xC4) == 0x01, "C4 = %02X", rd(0xC4));
    // NR 0x22 bit 7 = live pulse INT: inside the frame window
    at(10); CHECK(rd(0x22) & 0x80, "22 bit 7 in window");
    at(40); CHECK(!(rd(0x22) & 0x80), "22 bit 7 out of window");

    // ---- NR 0x20 software interrupt (pulse mode): a pulse now, status set ----
    fresh();
    at(1000);
    NextInt::nrWrite(0x20, 0x80);
    CHECK(NextInt::ex == 1 && NextInt::intLine(), "NR20 pulse");
    CHECK(rd(0x20) == 0x80, "NR20 status %02X", rd(0x20));
    at(1036); CHECK(!NextInt::intLine(), "pulse over");
    CHECK(NextInt::ex == 0, "ex drops after the NR20 pulse");

    // ---- hardware IM2 ----
    fresh();
    Z80::im = Z80::IM2;
    NextInt::nrWrite(0xC0, 0xA1);   // vector 101, hw IM2
    CHECK(rd(0xC0) == 0xA5, "C0 = %02X (IM2 -> 10)", rd(0xC0));
    NextInt::ctcWrite(0, 0x85); NextInt::ctcWrite(0, 10);   // ZC at T 20
    at(19); CHECK(!NextInt::intLine(), "nothing yet");
    at(21); CHECK(NextInt::intLine(), "CTC0 requests");
    at(500); CHECK(NextInt::intLine(), "request LATCHED (level) until acknowledged");
    CHECK(NextInt::ack() == (0xA0 | (3 << 1)), "vector");
    CHECK(!NextInt::intLine(), "in service, nothing else");
    at(800); CHECK(!NextInt::intLine(), "further CTC0 ZCs merge while in service");
    // a higher-priority source interrupts the CTC0 handler (nested)
    NextInt::nrWrite(0x20, 0x80);   // line, unqualified
    CHECK(NextInt::intLine(), "line preempts");
    CHECK(NextInt::ack() == 0xA0, "line vector %02X", 0xA0);
    NextInt::reti();                // line leaves service
    CHECK(!NextInt::intLine(), "CTC0 still in service blocks itself");
    NextInt::reti();                // CTC0 leaves service, its merged request cleared
    CHECK(!NextInt::intLine(), "merged ZCs were lost (im2_int_req cleared at service)");
    at(800 + 25); CHECK(NextInt::intLine(), "next ZC requests again");
    // the ULA frame event latches source 11
    fresh(); Z80::im = Z80::IM2; NextInt::nrWrite(0xC0, 0x01);
    at(CPU::statesInFrame + 5); wrap();
    CHECK(NextInt::intLine(), "ULA request");
    CHECK(NextInt::ack() == (11 << 1), "ULA vector");
    // the classic window does not double it while the Z80 is in IM 2
    NextInt::reti(); at(10); CHECK(!NextInt::intLine(), "no classic pulse in hw IM2 + IM 2");
    // ...but it does pulse with the Z80 in IM 1 (the ULA exception)
    Z80::im = Z80::IM1; at(10); CHECK(NextInt::intLine(), "ULA exception pulse");
    CHECK(NextInt::ack() == 0xFF, "no vector in IM 1");
    // leaving hw IM2 drops every request
    Z80::im = Z80::IM2;
    NextInt::nrWrite(0x20, 0x01);
    NextInt::nrWrite(0xC0, 0x00);
    at(2000); CHECK(!NextInt::intLine(), "pulse mode, pulse over");
    CHECK((rd(0x20) & 0x0F) == 0x01, "status survives the mode switch (int_status)");

    // a lower-priority request waits while a higher source is in service
    fresh(); Z80::im = Z80::IM2; NextInt::nrWrite(0xC0, 0x01);
    at(100); NextInt::nrWrite(0x20, 0x01);           // CTC0
    CHECK(NextInt::ack() == (3 << 1), "CTC0 taken");
    NextInt::nrWrite(0x20, 0x40);                    // ULA, lower priority
    CHECK(!NextInt::intLine(), "ULA blocked by CTC0 in service");
    NextInt::reti();
    CHECK(NextInt::intLine() && NextInt::ack() == (11 << 1), "ULA after RETI");

    // ---- 28 MHz CPU: one T-state = one CTC cycle, boundaries exact ----
    fresh(); ESPectrum::multiplicator = 3; CPU::statesInFrame = 70908u << 3;
    at(1000);
    NextInt::ctcWrite(0, 0x85); NextInt::ctcWrite(0, 4);   // ZC at 1000 + 4*16 - 1 = 1063, then every 64
    CHECK(NextInt::nextEventT(500000) == 1063, "28 MHz next %u", NextInt::nextEventT(500000));
    at(1062); CHECK(!NextInt::intLine(), "cycle before ZC");
    at(1063); CHECK(NextInt::intLine(), "ZC cycle");
    at(1063 + 35); CHECK(NextInt::intLine(), "last pulse cycle");
    at(1063 + 36); CHECK(!NextInt::intLine(), "pulse is 36 cycles");
    CHECK(NextInt::ctcRead(0) == 2, "count %u (ticks 5 and 6 after the reload)", NextInt::ctcRead(0));
    CHECK(NextInt::nextEventT(500000) == 1127, "next ZC %u", NextInt::nextEventT(500000));

    // ---- stackless NMI ----
    fresh();
    CHECK(!NextInt::nmiStackless(0x1234) && NextReg::reg[0xC2] == 0x34 && NextReg::reg[0xC3] == 0x12,
          "C2/C3 capture without stackless");
    NextInt::nrWrite(0xC0, 0x08);
    CHECK(NextInt::nmiStackless(0x5678) && NextInt::retnArmed, "stackless");
    CHECK(NextInt::stacklessRetn() == 0x5678 && !NextInt::retnArmed, "retn address");

    // ---- speed change mid-frame: the 28 MHz clock does not jump ----
    fresh();
    NextInt::ctcWrite(0, 0x85); NextInt::ctcWrite(0, 10);   // ZC at 28 MHz cycle 159
    at(10);
    ESPectrum::multiplicator = 1; CPU::tstates <<= 1; CPU::statesInFrame <<= 1;
    // at 7 MHz a T is 4 cycles: ZC at ceil(159/4) = 40
    CHECK(NextInt::nextEventT(140000) == 40, "after speed change %u", NextInt::nextEventT(140000));

    printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail != 0;
}
