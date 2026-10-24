/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or
visit https://zxespectrum.speccy.org/contacto

*/

#include "CPU.h"
#include "speccy/machines/Sam/Sam.h"
#include "app/ESPectrum.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "app/hardconfig.h"
#include "app/Config.h"
#include "speccy/video/Video.h"
#include "app/PerfScope.h"
#include "z80.h"
#include "drivers/psram/psram_spi.h"
#include "app/Debug.h"
#include "speccy/devices/Z80DMA.h"
#include "speccy/devices/zifi/ZiFi.h"
#include "speccy/devices/storage/IDE.h"
#include "speccy/devices/storage/DivMMC.h"
#include "speccy/devices/gs/GS.h"      // g_ngs_zxdma + GS::zxDmaRead/zxDmaWrite (ZX-DMA window)
#include "speccy/machines/TsConf/TsConf.h"     // g_tsconf_wr + TsConf::cpuWriteGate (FMAddr window, W0_WE)
#include "speccy/machines/If2.h"
#include "speccy/machines/Timex.h"      // g_timex_mmu + Timex::rd/wr (TC2068 SCLD horizontal MMU)
#include "speccy/machines/Atm.h"        // g_atm_ro (ATM-Turbo ROM windows) + Atm::reset/intEnabled
#include "speccy/machines/EvoBase.h"    // ZX-Evo font RAM, INT ack
#include "speccy/machines/TsConf/TsFastMem.h"
#include "app/CodeOverlay.h" // TS_OVL_CODE (CPU::tsFrameLoop)
#include "speccy/core/Rzx.h"
#include "MemGates.h"
#include "app/Col16Trace.h"

alignas(4) MemGates g_memgates = {};   // MemGates.h: the fast-path gates, one word
#if COL16_TRACE
uint32_t c16_rd[9], c16_wr[9], c16_fe[9], c16_flip, c16_bank;
#endif
#include "speccy/machines/Next/NextReg.h"
#include "speccy/machines/Next/NextVideo.h"
#include "speccy/machines/Next/NextInt.h"
#if PERF_TRACE && PERF_HIST
// TS-Conf guest-memory access histogram by PHYSICAL page (the page each CPU
// bank is mapped to), fetch + peek8 + poke8. Tells which pages a title hammers,
// i.e. what an SRAM page policy would have to hold. Dumped by the [PERF] block.
uint32_t ts_page_hist[257];   // [256] = bank-0 ROM
uint32_t perf_port_us[512], perf_port_n[512];      // PerfScope.h
uint32_t perf_bucket_us[PB_N], perf_bucket_n[PB_N];
#define TS_PAGE_HIT(addr) do { if (Z80Ops::isTsconf) ts_page_hist[TsConf::r.page[(addr) >> 14]]++; } while (0)
#else
#define TS_PAGE_HIT(addr) do {} while (0)
#endif

// Place hot CPU functions in SRAM instead of XIP flash
#undef IRAM_ATTR
#define IRAM_ATTR __not_in_flash("cpu")

#pragma GCC optimize("O3")

uint32_t CPU::tstates = 0;

int32_t CPU::prev_tstates = 0;
uint32_t CPU::tstates_diff = 0;

// Frame timing accumulators (µs); read+reset in VIDEO::EndFrame diagnostic.
volatile uint32_t cpu_frame_us  = 0;  // total CPU::loop() time (incl. FDC)
volatile uint32_t fdd_step_us   = 0;  // time inside rvmWD1793Step only
volatile uint32_t endframe_us   = 0;  // last VIDEO::EndFrame() duration (main path)

uint64_t CPU::global_tstates = 0;
uint32_t CPU::statesInFrame = 0;
uint32_t CPU::tstates_frame = 0;
uint32_t CPU::tstates_active = 0;
uint8_t CPU::latetiming = 0;
int32_t CPU::IntStart = 0;
int32_t CPU::IntEnd = 0;
uint32_t CPU::stFrame = 0;
bool CPU::portBasedBP = false;
bool CPU::paused = false;

bool Z80Ops::is48;
bool Z80Ops::isByte = false;
bool Z80Ops::isALF = false;
bool Z80Ops::is128;
bool Z80Ops::isPentagon;
bool Z80Ops::is512 = false;
bool Z80Ops::is1024 = false;
bool Z80Ops::isProfi = false;
bool Z80Ops::isScorpion = false;
bool g_scorp_even_m1 = false;
bool g_scorp_gmx = false;
bool g_scorp_prof = false;
bool g_scorp_banked = false;
bool g_scorp_1024 = false;
bool g_scorp_turbo_plus = false;
bool g_gmx_tap = false;
uint8_t g_scorp_kay = 0;
bool g_scorp_evo = false;
bool Z80Ops::isP3 = false;
bool Z80Ops::isTsconf = false;
bool Z80Ops::isAtm = false;
bool Z80Ops::isTc2068 = false;
// "The SCLD is this machine's own ULA" (either Timex romset), as opposed to the
// Timex video option ticked as a CARD on an ordinary 48K/128K. It decides the #FF
// decode: on a real Timex the SCLD answers the LOW BYTE alone (MAME tc2048_io /
// ts2068_io both `mirror(0xff00)`, Fuse's periph mask is 0x00ff), while the card
// case has to keep the A8=0 qualifier because the SAA1099 shares the #FF family
// there (0x00FF data / 0x01FF address).
bool g_timex_machine = false;
bool Z80Ops::isNext = false;
bool Z80Ops::isSam = false;

void CPU::updateStatesInFrame() {
    Z80Ops::isALF = (Config::arch == A_ALF);
    Z80Ops::isNext = (Config::arch == A_NEXT);
    // Early/Late ULA timing: Early=latetiming=0, Late=latetiming=1.
    // IntEnd += latetiming shifts the INT window 1T later for Late.
    // isActiveINT adds latetiming to tstates so that Late fires 1T later,
    // including via end-of-frame straddle (tstates=statesInFrame-1 → tmp=0).
    if (Config::arch == A_48K) {
        statesInFrame = TSTATES_PER_FRAME_48;
        IntStart = INT_START48;
        IntEnd = INT_END48 + CPU::latetiming;
        if (Config::romSet48 == R_48K_BY) {
            statesInFrame = TSTATES_PER_FRAME_BYTE;
            IntStart = INT_START48;
            IntEnd = INT_END_BYTE48;
        }
    } else if (Config::arch == A_128K || Z80Ops::isALF) {
        // The +2A/+3 (romset R_P3) keeps the 128K frame (70908 T, 228 T/line, screen
        // at 14361) and differs only in the contention PATTERN and which pages are
        // contended.
        statesInFrame = TSTATES_PER_FRAME_128;
        IntStart = INT_START128;
        IntEnd = (Config::isPlus3() ? INT_END_P3 : INT_END128) + CPU::latetiming;
    } else if (Config::arch == A_P512) {
        statesInFrame = TSTATES_PER_FRAME_PENTAGON;
        IntStart = INT_START_PENTAGON;
        IntEnd = INT_END_PENTAGON;
    } else if (Config::arch == A_P1024) {
        statesInFrame = TSTATES_PER_FRAME_PENTAGON;
        IntStart = INT_START_PENTAGON;
        IntEnd = INT_END_PENTAGON;
    } else if (Config::arch == A_PROFI) {
        statesInFrame = TSTATES_PER_FRAME_PROFI;
        IntStart = INT_START_PROFI;
        IntEnd = INT_END_PROFI;
    } else if (Config::arch == A_ATM) {
        // ATM-Turbo: 312 lines x 224 T (Unreal atm.cpp: "the screen has 312 scan
        // lines of 224 T each"; MAME builds on spectrum_128's 312-line raster).
        // (ATM-Turbo 2 on BIOS 1.06: 316 lines — atmFrame316, ArchRom.h.)
        statesInFrame = atmFrame316(Config::romSetAtm) ? TSTATES_PER_FRAME_SCORPION_GR
                                                       : TSTATES_PER_FRAME_48;
        IntStart = INT_START48;
        IntEnd = INT_END48;
        if (Config::isEvoBase()) {          // ZX Evolution: the raster the AVR selected
            // video_sync_v.v VPERIOD_*; the INT is 256 fclk = 32 T in every raster (zint.v).
            statesInFrame = Config::isEvo48Raster()  ? TSTATES_PER_FRAME_48
                          : Config::isEvo128Raster() ? TSTATES_PER_FRAME_128
                          : Config::isEvo60Raster()  ? TSTATES_PER_FRAME_EVO60
                                                     : TSTATES_PER_FRAME_PENTAGON;
            IntStart = INT_START_PENTAGON;
            IntEnd = INT_END_PENTAGON;
            EvoBase::contendApply();        // 48K/128K contention: 3.5 MHz only
        }
    } else if (Config::arch == A_SCORP) {
        // Green PCB / GMX = 316 lines/frame; Yellow = 312 (see CPU.h; MAME's
        // scorpiongmx builds on the Turbo+/Green machine config).
        // (Nemo KAY: the Yellow frame too — isScorpYellowTiming.)
        statesInFrame = isKayRomset(Config::romSetScorp) ? TSTATES_PER_FRAME_KAY
                      : !isScorpYellowTiming(Config::romSetScorp)
                            ? TSTATES_PER_FRAME_SCORPION_GR
                            : TSTATES_PER_FRAME_SCORPION;
        IntStart = INT_START_SCORPION;
        IntEnd = isKayRomset(Config::romSetScorp) ? INT_END_KAY : INT_END_SCORPION;
        if (isScorpEvoRomset(Config::romSetScorp)) {   // ScorpEvo: the ZX-Evo Pentagon raster
            statesInFrame = TSTATES_PER_FRAME_PENTAGON;
            IntStart = INT_START_PENTAGON;
            IntEnd = INT_END_PENTAGON;
        }
    } else if (Config::arch == A_NEXT) {
        // The frame of the machine TIMING (nextreg 0x03) and 50/60 Hz (0x05):
        // NextReg::raster() — 48K 224x312/262, 128K/+3 228x311/261, Pentagon
        // 224x320 (Reference_machines, VGA).
        const NextReg::Raster r = NextReg::raster();
        statesInFrame = (uint32_t)r.tpl * r.lines;
        IntStart = 0;
        IntEnd = r.intEnd + (r.kind == 1 ? CPU::latetiming : 0);
    } else if (Config::arch == A_SAM) {
        statesInFrame = TSTATES_PER_FRAME_SAM;
        IntStart = 0;
        IntEnd = INT_END_SAM;
    } else { // if (Config::arch == A_PENT) - by default
        statesInFrame = TSTATES_PER_FRAME_PENTAGON;
        IntStart = INT_START_PENTAGON;
        IntEnd = INT_END_PENTAGON;
    }
    // SAM Coupe: no turbo — the 6 MHz clock is the machine's only speed. The
    // user's pick (multUser) is kept for the next machine.
    if (Config::arch == A_SAM) ESPectrum::multiplicator = 0;
    uint8_t m = ESPectrum::multiplicator;
    if (m) {
        statesInFrame <<= m;
        IntStart <<= m;
        // The classic model keeps the ULA pulse at a fixed WALL time (32 base T
        // = 256 CPU cycles at 28 MHz). The Next counts it in CPU cycles at any
        // speed (zxnext.vhd ~2033: pulse_count runs on i_CLK_CPU; 32 for 48K/+3,
        // 36 for 128K/Pentagon), so a 60-cycle IM2 handler never re-takes the
        // interrupt there. 4K Race Next at 28 MHz: `INC (IX+0B)/EI/RETI` was
        // counted twice per frame under the 256-cycle window, its 2-frame wait
        // loop ran free and the copper restart drifted through the picture.
        if (Config::arch != A_NEXT) IntEnd <<= m;
    }
    stFrame = statesInFrame - IntEnd;
    // TS-Conf: the FRAME INT window is programmable (HSINT/VSINT) — override
    // the Pentagon IntStart/IntEnd just set above with the guest's position.
    if (Z80Ops::isTsconf) TsConf::frameIntRecalc();
    TsConf::memcycRecalc();   // the 14 MHz wait-state gate follows EVERY clock change (Alt+F2 too), 0 off TS-Conf
}

void CPU::reset() {

    Z80::reset();

    CPU::latetiming = Config::AluTiming;

    Z80Ops::isALF = (Config::arch == A_ALF);
    // Derived once here (like isALF) instead of per-branch below — a branch that
    // forgets to clear it would leak Scorpion's #1FFD decode into another machine.
    Z80Ops::isScorpion = (Config::arch == A_SCORP);
    // Even-M1 is a Yellow-PCB-only trait (see CPU.h); Green and GMX dropped it.
    g_scorp_even_m1 = Z80Ops::isScorpion && (Config::romSetScorp == R_SCORP);
    g_scorp_gmx = Z80Ops::isScorpion && isScorpGmxRomset(Config::romSetScorp);
    // ScorpEvo carries the same ProfROM plane mapper (scorpevo fpga ProfROM/pfpzu.v:
    // the plane switches on a read of #0100-#010F of the service bank).
    g_scorp_prof = Z80Ops::isScorpion && (Config::romSetScorp == R_SCORP_PROF ||
                                          isScorpEvoRomset(Config::romSetScorp));
    g_scorp_banked = g_scorp_gmx || g_scorp_prof;
    // ProfROM ships on the ZS-1024 Turbo+ (speccy4ever files it under "Prof ROM
    // & ZX-1024"; ZXMAK2 has no 256K-only ProfROM machine either), so it carries
    // the same 1FFD D7,D6 page extension.
    g_scorp_1024 = Z80Ops::isScorpion && (Config::romSetScorp == R_SCORP_1024 ||
                                          Config::romSetScorp == R_SCORP_PROF ||
                                          isScorpEvoRomset(Config::romSetScorp));
    // The "+" of Turbo+ IS the read-triggered speed toggle (Ports::input). Yellow
    // is MAME's plain scorpion_state and has no such handler.
    g_scorp_turbo_plus = Z80Ops::isScorpion && !isScorpYellowTiming(Config::romSetScorp);
    // Nemo KAY: its own #1FFD (turbo is 1FFD D2 there, not a port read).
    g_scorp_evo = Z80Ops::isScorpion && isScorpEvoRomset(Config::romSetScorp);
    g_scorp_kay = !Z80Ops::isScorpion ? 0
                : Config::romSetScorp == R_KAY256 ? 2
                : Config::romSetScorp == R_PHOENIX ? 4
                : (Config::romSetScorp == R_KAY1024 || Config::romSetScorp == R_KAY2010) ? 3 : 0;
    g_gmx_tap = false;   // re-armed by Ports::scorpionRomUpdate once paging settles
    // The +2A/+3 is the R_P3 romset of the 128K arch (the way +2 is). It shares the
    // arch's frame timing but NOT its paging, contention or floating bus, so the
    // 128K branch below hands it a separate flag set.
    Z80Ops::isTsconf = (Config::arch == A_TSCONF);
    Z80Ops::isAtm = (Config::arch == A_ATM);
    Z80Ops::isSam = (Config::arch == A_SAM);
    if (!Z80Ops::isAtm && !Z80Ops::isSam) g_atm_ro = 0;
    Z80Ops::isTc2068 = Config::isTc2068();
    g_timex_machine  = Config::isTimex();
    Z80Ops::isNext = (Config::arch == A_NEXT);
    // Off the Next every port is "decoded": the shared decoders test the NR
    // 0x82-0x85 bit alone (no isNext), so leaving a 48K personality behind a
    // machine switch must not silence another machine's AY / divMMC / mouse.
    if (!Z80Ops::isNext) NextReg::portEn = ~0u;
    Z80Ops::isP3 = Config::isPlus3();
    if (Z80Ops::isP3) {
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;      // the +3 is NOT 128K here: it has its own paging,
        Z80Ops::isPentagon = false; // its own contention and no floating bus
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        // Unattached ports read 0xFF on a +2A/+3 — there is no floating bus
        // (Fuse: machine->unattached_port = spectrum_unattached_port_none).
        Ports::getFloatBusData = &Ports::getFloatBusDataNone;
        ESPectrum::target = MICROS_PER_FRAME_128;
    } else if (Config::arch == A_48K) {
        Z80Ops::isByte = (Config::romSet48 == R_48K_BY);
        Ports::getFloatBusData = &Ports::getFloatBusData48;
        Z80Ops::is48 = true;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = false;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        // Set emulation loop sync target
        ESPectrum::target = !Z80Ops::isByte ? MICROS_PER_FRAME_48 : MICROS_PER_FRAME_BYTE;
    } else if (Config::arch == A_128K || Z80Ops::isALF) {
        Z80Ops::isByte = (Config::romSet128 == R_128K_BY || Config::romSet128 == R_128K_BY_GLUK);
        Ports::getFloatBusData = &Ports::getFloatBusData128;
        Z80Ops::is48 = false;
        Z80Ops::is128 = true;
        Z80Ops::isPentagon = false;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        // Set emulation loop sync target
        ESPectrum::target = !Z80Ops::isByte ? MICROS_PER_FRAME_128 : MICROS_PER_FRAME_BYTE;
    } else if (Config::arch == A_P512) {
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = true;
        Z80Ops::is512 = true;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        // Set emulation loop sync target
        ESPectrum::target = MICROS_PER_FRAME_PENTAGON;
    } else if (Config::arch == A_P1024) {
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = true;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = true;
        Z80Ops::isProfi = false;
        // Set emulation loop sync target
        ESPectrum::target = MICROS_PER_FRAME_PENTAGON;
    } else if (Config::arch == A_PROFI) {
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = false;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = true;
        // Set emulation loop sync target
        ESPectrum::target = MICROS_PER_FRAME_PROFI;
    } else if (Config::arch == A_SCORP) {
        // Scorpion ZS-256: HAS the port-#FF floating bus (proven by the "ТЕСТ
        // SCORPION 1992" port-FF diagnostic, which fills screen attrs with
        // 0x55/0xAA and requires IN #FF to read them back — hw 2026-09-05). Its
        // line timing is the 48K one (224 T/line, paper at T 14336), so the 48K
        // float function is bit-exact for both Yellow and Green PCBs (only the
        // total frame length differs, which the per-line float math ignores).
        // No contention. My earlier "no float bus (Fuse unattached_port_none)"
        // was wrong for this hardware. Scorpion's float is ATTRIBUTE-dominant,
        // not the 48K per-T bitmap/attribute alternation — the port-FF test does
        // two reads 15 T apart (odd) and needs both equal; see getFloatBusDataScorp.
        // Nemo KAY: Unreal's KAY1024 preset has floatbus=0 and portff=0 — the
        // unattached ports read 0xFF.
        // ScorpEvo: zports.v answers every unattached port with #FF (its default
        // dout), and the FPGA has no ULA float.
        Ports::getFloatBusData = (isKayRomset(Config::romSetScorp) || isScorpEvoRomset(Config::romSetScorp))
                                     ? &Ports::getFloatBusDataNone
                                     : &Ports::getFloatBusDataScorp;
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = false;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        // Set emulation loop sync target (Green/GMX share the 316-line frame)
        ESPectrum::target = isScorpEvoRomset(Config::romSetScorp) ? MICROS_PER_FRAME_PENTAGON
                          : !isScorpYellowTiming(Config::romSetScorp)
                                ? MICROS_PER_FRAME_SCORPION_GR
                                : MICROS_PER_FRAME_SCORPION;
    } else if (Config::arch == A_ATM) {
        // ATM-Turbo: the 48K frame, no contention, no floating bus worth modelling
        // (the reads that matter here are the machine's own ports).
        Ports::getFloatBusData = &Ports::getFloatBusDataNone;
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = false;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        ESPectrum::target = Config::isEvo48Raster()  ? MICROS_PER_FRAME_48
                          : Config::isEvo128Raster() ? MICROS_PER_FRAME_128
                          : Config::isEvo60Raster()  ? MICROS_PER_FRAME_EVO60
                          : Config::isEvoBase() ? MICROS_PER_FRAME_PENTAGON
                          : atmFrame316(Config::romSetAtm) ? MICROS_PER_FRAME_SCORPION_GR
                                                           : MICROS_PER_FRAME_48;
    } else if (Config::arch == A_NEXT) {
        // Next: no floating bus, no contention, 128K frame timing
        Ports::getFloatBusData = &Ports::getFloatBusDataNext;
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = false;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        ESPectrum::target = NextReg::raster().micros;
    } else if (Config::arch == A_SAM) {
        // SAM Coupe: 119808 T at 6 MHz = the 48K frame period (19968 us).
        Ports::getFloatBusData = &Ports::getFloatBusDataNone;
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = false;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        ESPectrum::target = MICROS_PER_FRAME_48;
    } else if (Config::arch == A_TSCONF) {
        // TS-Conf: Pentagon raster (224 T/line, 71680 T/frame), uncontended.
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = false;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        // Set emulation loop sync target
        ESPectrum::target = MICROS_PER_FRAME_PENTAGON;
    } else { // if (Config::arch == A_PENT) - by default
        Z80Ops::isByte = false;
        Z80Ops::is48 = false;
        Z80Ops::is128 = false;
        Z80Ops::isPentagon = true;
        Z80Ops::is512 = false;
        Z80Ops::is1024 = false;
        Z80Ops::isProfi = false;
        // Set emulation loop sync target
        ESPectrum::target = MICROS_PER_FRAME_PENTAGON;
    }

    Ports::ulaIoContendRecalc();

    // 16col is Pentagon-only — auto-disable when switching to non-Pentagon arch.
    if (!(Z80Ops::isPentagon || Z80Ops::isProfi)) {
        if (Config::mode16col_onoff) {
            Config::mode16col_onoff = false;
            Config::save();
        }
        VIDEO::mode16col_enabled = false;
        VIDEO::free16colLut();   // release the LUT — 16col now off (0 SRAM)
    }

    // TR-DOS (betadisk) is mandatory on Pentagon — force on without saving.
    // Scorpion too: its Beta-128 is on board (TR-DOS in the machine's own rom[3]).
    if ((Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion || Z80Ops::isTsconf || Z80Ops::isAtm) && !Config::betadisk) Config::betadisk = true;

    // ...and impossible on the +3, which has the uPD765 on #2FFD/#3FFD instead. Beta's
    // #1F/#FF decode and its 0x3D00 ROM trap would both fire inside the +3's own ROMs.
    // Backstop for the paths that reach a machine without going through MachineSwitch
    // (boot with a stale config, snapshot-forced arch).
    if (Z80Ops::isP3) {
        if (Config::betadisk) Config::betadisk = false;
        if (Config::mb02) Config::mb02 = false;
        if (Config::timex_video) Config::timex_video = false;
    }
    // Backstop for the paths that never pass through a menu. An explicit Off on a +3e
    // is the user's choice and stays; only a scheme the running machine cannot reach is
    // rewritten. IDE::init() is not called here — a reset does not re-open images, and
    // setup() already resolved this for the boot machine.
    if (Config::isPlus3e()) {
        if (Config::ide_scheme != IDE::OFF && Config::ide_scheme != IDE::PLUS3E)
            Config::ide_scheme = IDE::PLUS3E;   // NEMO/PROFI/SMUC: cards the +3e cannot reach
    } else if (Config::ide_scheme == IDE::PLUS3E) {
        Config::ide_scheme = IDE::OFF;
    }
    if (Config::arch == A_SAM) {   // the Atom Lite is the SAM's card and nobody else's
        if (Config::ide_scheme != IDE::OFF && Config::ide_scheme != IDE::ATOMLITE) Config::ide_scheme = IDE::ATOMLITE;
    } else if (Config::ide_scheme == IDE::ATOMLITE) {
        Config::ide_scheme = IDE::OFF;
    }
    // ...and the same for the +3 (divIDE): its ROM drives the divIDE taskfile, and that
    // scheme must not survive onto another machine — its decode is ahead of General
    // Sound's #B3/#BB in Ports.cpp.
    if (Config::isPlus3Div()) {
        if (Config::ide_scheme != IDE::OFF && Config::ide_scheme != IDE::DIVIDE)
            Config::ide_scheme = IDE::DIVIDE;
    } else if (Config::ide_scheme == IDE::DIVIDE) {
        Config::ide_scheme = IDE::OFF;
    }

    // Timex video is incompatible with Byte ROM sets — auto-disable.
    // Also with Profi/Karabas: port #FF there is the Beta-128 FDC SYS register,
    // the Karabas-Pro native RTC AS latch (#FF/#BF, CPM=1&ROM14=1) and the SAA
    // select — the Timex handler fires whenever trdos=0, which is ROMain's
    // normal running state (CP/M code in RAM), so it steals the RTC register
    // select and ROMain's boot hangs in its MC146818 "wait while UIP=1" spin
    // (RTC::readDisabled answers UIP-clear only for the LATCHED status regs).
    // ...and on Scorpion GMX: the Timex MainScreen branch would render over the
    // GMX 640x200 pair-slot framebuffer.
    if ((Z80Ops::isByte || Z80Ops::isProfi || g_scorp_gmx || Z80Ops::isTsconf || Z80Ops::isAtm) && Config::timex_video) Config::timex_video = false;
    // ATM-Turbo: the memory manager puts ROM or RAM in any window and the BIOS runs
    // from all four at reset — DivMMC's automap and MB-02+ would page over it. Its
    // IDE is the on-board ATM interface on the 2+ (IDE::ATM), none on the ATM1.
    if (Z80Ops::isAtm) {
        if (Config::esxdos) Config::esxdos = 0;
        if (Config::mb02) Config::mb02 = false;
        const bool atm2 = atmHasIde(Config::romSetAtm);
        if (!atm2 && Config::ide_scheme == IDE::ATM) Config::ide_scheme = IDE::OFF;
        else if (atm2 && Config::ide_scheme != IDE::OFF && Config::ide_scheme != IDE::ATM &&
                 !(isAtm3Romset(Config::romSetAtm) && Config::ide_scheme == IDE::NEMO))
            Config::ide_scheme = IDE::ATM;   // (ATM3 also takes a NEMO card — NedoOS)
    } else if (Config::ide_scheme == IDE::ATM) {
        Config::ide_scheme = IDE::OFF;
    }

    // ...and the converse for both Timex machines, whose ULA IS the SCLD: Timex
    // video is the machine, and the SAA1099 cannot share the #FF family with it.
    // Backstop for the paths that never pass through the menu (boot with a stale
    // config, a snapshot that forces the machine).
    if (Config::isTimex()) {
        Config::timex_video = true;
        if (Config::SAA1099) Config::SAA1099 = false;
        // TC2068 only: esxDOS/DivMMC automaps on Sinclair-ROM entry addresses
        // (0x0000/0x0008/0x0038/0x0066/0x04C6/0x0562), which on this machine's own
        // HOME ROM are unrelated code — and its page-0 window would fight the SCLD
        // for the same 8 KB slots. Same three-place treatment as Beta on the +3.
        if (Z80Ops::isTc2068 && Config::esxdos) Config::esxdos = 0;
    }

    // SAM Coupe: none of the Spectrum interfaces exist on its bus; the SAA1099 is
    // on the board. Backstop for paths that never pass through MachineSwitch.
    if (Z80Ops::isSam) {
        Config::betadisk = false;
        if (Config::esxdos) Config::esxdos = 0;
        Config::mb02 = false;
        Config::timex_video = false;
        Config::SAA1099 = true;
        Sam::reset();
    }

    // Interface 2 cartridge: esxDOS off the bus while it is in (its automap entry
    // points are the cartridge's own vectors), back once it is out.
    If2::onReset();

    // «Байт»: RESET returns the DD66 map to native state — the built-in test's
    // dispatch (#39F9) must land on DD73's own base test at #3A00, not on the
    // DD71 blocks a previous test run left switched in.
    if (Z80Ops::isByte) Config::byteTestRomReset();

    updateStatesInFrame();

    // TS-Conf register file reset (tsinit values). Runs after
    // updateStatesInFrame so applyZclk/frameIntRecalc see the final frame
    // geometry, and after ESPectrum::reset's standard ramCurrent assignment,
    // which its setBanks() overrides with the TS-Conf window mapping.
    // Cold = the first TS-Conf reset of this session (boot, or a live switch
    // into TS-Conf): raises the STATUS pwr_up flag (b6) that TS-BIOS tests at
    // START to run its one-time init (FDV_RES → WRITE_NVRAM, dual-SD CMD0) and
    // seeds CRAM. Every later reset is warm: pwr_up and CRAM survive (only a
    // STATUS read clears pwr_up) — the old unconditional `false` never let the
    // BIOS see a power-up at all (hw 2026-09-06).
    static bool s_tsconf_live = false;
    if (Z80Ops::isTsconf) { TsConf::reset(!s_tsconf_live); s_tsconf_live = true; }
    else s_tsconf_live = false;   // g_ts_memcyc was zeroed by updateStatesInFrame above
    // ATM-Turbo: register file + memory map (overrides ESPectrum::reset's standard
    // ramCurrent assignment, like TsConf::setBanks does).
    if (Z80Ops::isAtm) Atm::reset();

    tstates = 0;
    global_tstates = 0;

    prev_tstates = 0;
    tstates_diff = 0;
}

IRAM_ATTR void CPU::step() {
    Z80::execute();
}

#define BREAKPOINTS if (pbbp || (nbp > 0 && Config::pcBreakHit(Z80::getRegPC()))) { VIDEO::EndFrame(); return; }


// Phase of the CPU's own 4 T NOP grid while it sits in HALT, taken at the
// instant it entered one. File scope because a HALT spans the frame boundary.
static uint32_t ts_halt_phase = 0;
static bool     ts_halt_phase_set = false;

// The TS-Conf frame ("Stage D", see the comment at its call site in CPU::loop).
// TS_OVL_CODE: it lives in the TS-Conf code overlay window, so on every other
// machine its ~0.7 KB of SRAM is heap. Reachable only under Z80Ops::isTsconf, the
// same gate that already reaches TsConf::portRead/trdosTrap in that window.
// BREAKPOINTS returns from HERE; CPU::loop returns right after the call anyway.
TS_OVL_CODE void CPU::tsFrameLoop(uint64_t _loop_t0, bool pbbp, int nbp, uint32_t zifi_pump_due) {
    // Stage D: the whole TS-Conf frame, event-driven.
    uint32_t ts_idle = 0;   // T-states slept in HALT this frame (several sleeps per frame with a raster split)
    while (tstates < statesInFrame) {
        if (Z80::isHalted()) {
            TsConf::workHalt();   // PERF_TRACE: close the frame-work measurement
#if PERF_TRACE
            { extern uint32_t g_halt_t; extern bool g_halt_set;
              if (!g_halt_set) { g_halt_t = CPU::tstates; g_halt_set = true; } }
#endif
            // A HALTed CPU leaves HALT only on an interrupt, so sleep straight
            // to the next T-state where the INT line can rise (frame end with
            // interrupts disabled) instead of stepping 4 T per execute(): with
            // LINE INT armed at 14 MHz that was ~72k execute() calls of pure
            // spinning per frame (TMNT: a flat 43 ms/frame, hw 2026-09-06).
            // The video machine is walked line by line, not flushed, so a
            // per-line effect programmed after the wake still renders right.
            uint32_t wake = Z80::isIFF1() ? TsConf::nextIntEvent() : statesInFrame;
            if (wake > statesInFrame) wake = statesInFrame;
            // A HALTed Z80 samples INT only on its own 4 T NOP grid, and that
            // grid is anchored where it ENTERED HALT — not on the raster. So
            // sleeping straight to the event accepts up to 3 T early, and the
            // error is the guest's HALT phase, which is why it moves from
            // scene to scene and no raster constant can absorb it.
            //
            // Snapping the wake back onto the grid is what makes TS-Conf agree
            // with Pentagon in EVERY frame: the distance from the HALT to the
            // interrupt is a property of the guest, identical on both machines,
            // so both then accept at the same offset past their own interrupt.
            // Without it TS-Conf accepts exactly AT its window (offset 0) while
            // Pentagon accepts p = (halt T) mod 4 past its own — measured on
            // "Across the Edge" (hw 2026-09-12): Pentagon's intT read 0, 1 and 3
            // in consecutive windows of one scene while TS-Conf sat pinned at 2,
            // and its border effect came out exactly p columns left of
            // Pentagon's (2 px per column).
            //
            // An earlier attempt on 2026-09-09 was reverted as "moved the
            // border but did not fix it" — it predates both the INT-at-the-
            // boundary fix and the +2 raster anchor, either of which alone
            // leaves the border in the wrong place anyway.
            if (!ts_halt_phase_set) { ts_halt_phase = tstates & 3u; ts_halt_phase_set = true; }
            if (wake < statesInFrame) {
                // statesInFrame is a multiple of 4, so the phase survives the
                // frame wrap; never round past the frame end, where the loop
                // would exit with the interrupt unhandled.
                uint32_t d = (wake - ts_halt_phase) & 3u;
                if (d) { uint32_t w2 = wake + (4u - d); if (w2 < statesInFrame) wake = w2; }
            }
            if (wake > tstates) { ts_idle += wake - tstates; haltAdvanceTo(wake); continue; }
        } else ts_halt_phase_set = false;   // re-taken at the next HALT
        if (Z80::isIFF1() && TsConf::intLine()) {
            // The INT line is up and, prefixes aside, we are AT an
            // instruction boundary: every way into this test lands on one
            // (a HALT sleep that stopped exactly on the event,
            // exec_nocheck's slice end, or a completed execute()). A Z80
            // samples INT at that boundary, so take it HERE.
            //
            // This used to call Z80::execute(), which FETCHES AND RUNS a
            // whole instruction and only checks the line at its END — 4 T
            // late out of HALT (the fetch of a NOP the CPU never had to
            // execute), one full instruction late otherwise. Every other
            // machine gets the boundary for free: its checked loop reaches
            // the window from the PREVIOUS frame's tail, where the wrap in
            // Z80Ops::isActiveINT already has the line up, so the INT is
            // taken at the frame-end overshoot itself. That is why the same
            // demo read intT = 0..3 on Pentagon and 6..9 on TS-Conf, and
            // why its border split landed right of Pentagon's (hw
            // 2026-09-09). The other 2 T of that delta are the window
            // legitimately opening at hsint=2, and the raster anchor
            // carries them (TS_SCREEN_TSCONF, Video.h).
            //
            // ...unless a DD/FD/ED/CB prefix byte has been fetched and the
            // instruction is unfinished — execute()/exec_nocheck() can both
            // return in that state, and a Z80 never samples INT there.
            if (Z80::atInstrBoundary()) Z80::checkINT();
            // Not taken means pendingEI (an EI defers the interrupt by
            // exactly one instruction) or a half-decoded prefix: either way
            // run the instruction and let its own checkINT() take it.
            // (checkINT clears IFF1 when it fires, so this doubles as the
            // "was it taken" test.)
            if (Z80::isIFF1()) Z80::execute();
        } else {
            // Nothing can interrupt before the next INT event (frame end
            // while interrupts are disabled — EI/RETN/RETI re-enabling them
            // end the slice through TsConf::intEnableHook, as do INTMask
            // and DMACtrl writes through tsWakeLoop), so run unchecked to
            // it. Per-instruction execute() here was 3x the cost of
            // exec_nocheck() on TMNT (100% of its frame ran checked).
            uint32_t next = Z80::isIFF1() ? TsConf::nextIntEvent() : statesInFrame;
            if (next <= tstates) next = tstates + 1;
            stFrame = next;
            Z80::exec_nocheck();
            if (stFrame == 0) continue;           // HALTed: the sleep above takes over
            // The INT is accepted after the instruction that crossed the
            // event boundary — same sampling point as execute(). The slice
            // can end on a fetched prefix byte, though (exec_nocheck's
            // `else continue` path), and that is mid-instruction: leave it
            // to the next iteration, which finishes the instruction first.
            if (Z80::atInstrBoundary()) Z80::checkINT();
        }
        if (Config::dma_mode) Z80DMA::handleDMA();
        if (ZiFi::cdcNicActive && tstates >= zifi_pump_due) {
            zifi_pump_due = tstates + 3500;
            ZiFi::cdcPump();
        }
        BREAKPOINTS
    }
    TsConf::endFrame();
    // Frame tail — a faithful copy of the shared tail below (kept
    // duplicated so the hot path of every other machine stays textually
    // untouched).
    {
        uint64_t _ef_t0 = time_us_64();
        VIDEO::EndFrame();
        endframe_us = (uint32_t)(time_us_64() - _ef_t0);
    }
    CPU::tstates_diff += CPU::tstates - CPU::prev_tstates;
    if ((ESPectrum::fdd.control & (kRVMWD177XHLD | kRVMWD177XHLT)) != 0)
    {
        uint64_t _fdd_t0 = time_us_64();
        rvmWD1793Step(&ESPectrum::fdd, CPU::tstates_diff / WD177XSTEPSTATES); // FDD
        fdd_step_us += (uint32_t)(time_us_64() - _fdd_t0);
    }
    CPU::tstates_diff = CPU::tstates_diff % WD177XSTEPSTATES;
    cpu_frame_us += (uint32_t)(time_us_64() - _loop_t0);
#if PERF_TRACE
    { extern uint32_t g_frm_int_taken, g_frm_int_miss;
      if (!g_frm_int_taken) g_frm_int_miss++;   // no interrupt reached the guest this whole frame
      g_frm_int_taken = 0;
      extern uint32_t g_brd_first_t, g_brd_min, g_brd_max; extern bool g_brd_first_set;
      if (g_brd_first_set) { if (g_brd_first_t < g_brd_min) g_brd_min = g_brd_first_t;
                             if (g_brd_first_t > g_brd_max) g_brd_max = g_brd_first_t; }
      g_brd_first_set = false; { extern bool g_halt_set; g_halt_set = false; } }
#endif
    global_tstates += statesInFrame;
    tstates_frame = tstates;
    tstates_active = tstates_frame - ts_idle;   // load = frame minus the HALT sleeps (haltAdvanceTo's own stamp is per sleep)
    tstates -= statesInFrame;
    CPU::prev_tstates = tstates;
    return;
}

IRAM_ATTR void CPU::loop() {
    uint64_t _loop_t0 = time_us_64();
    bool pbbp = CPU::portBasedBP;
    if (paused || pbbp) {
        VIDEO::EndFrame();
        cpu_frame_us += (uint32_t)(time_us_64() - _loop_t0);
        return;
    }
    if (__builtin_expect(Rzx::mode != 0, 0)) { loopRzx(_loop_t0); return; }
    int nbp = Config::numPcBP;

    BREAKPOINTS
    // Check NMI
    if (Z80::isNMIDOS()) {
        Z80::execute();
        Z80::doNMIDOS();
    } else if (Z80::isNMI()) {
        // Next: take the NMI at the boundary we are already on. The frame
        // tail very often accepts the frame INT (PC = 0x0038, not fetched
        // yet); execute() first would fetch it, automap esxDOS through the
        // RST 0x38 entry point, and the NMI would then fetch 0x0066 from the
        // esxDOS ROM — a RET there, the NMI lost without a trace (hw
        // 2026-10-07). A Z80 takes a pending NMI before INT anyway.
        if (!(Z80Ops::isNext && Z80::atInstrBoundary())) Z80::execute();
        Z80::doNMI();
    }
    // ZiFi over USB-CDC: service the host stack mid-frame (~1 kHz) even while the
    // guest isn't touching the ZiFi ports — see ZiFi::cdcPump(). The cadence check
    // costs one compare per instruction, same class as the dma_mode check below.
    uint32_t zifi_pump_due = tstates + 3500; // ~1 ms at 3.5 MHz guest clock

    // TS-Conf: the FRAME INT window is programmable and can sit anywhere in
    // the frame (every other machine has IntStart = 0) — and it can MOVE
    // mid-frame: a raster-split title rewrites VSINT inside the handler and
    // gets two windows per frame (Ninja Gaiden: 272 -> 94 -> 272, hw
    // 2026-09-07). The whole frame therefore runs the event-driven "Stage D"
    // below: unchecked exec_nocheck() slices to the next INT event
    // (TsConf::nextIntEvent — window start, LINE start, DMA end), one checked
    // instruction while a line is up, and HALT sleeps that WALK the video
    // machine line by line. The earlier fixed three-slice shape
    // (unchecked to IntStart / checked window / unchecked to frame end) could
    // not follow a moved window, and its FlushOnHaltTo rendered the rest of
    // the frame at the HALT with the pre-INT registers — the picture then
    // showed the level's first frame on every other frame. A window written
    // mid-slice ends the slice through tsWakeLoop (frameIntRecalc), exactly
    // like an INTMask/DMACtrl write.
    if (Z80Ops::isTsconf) { tsFrameLoop(_loop_t0, pbbp, nbp, zifi_pump_due); return; }
    // Next: the rest of the frame in the .nxovl window (CPU::nextFrameLoop), every
    // other machine the same body with the Next branches compiled out — frameRest
    // is instantiated twice (the Profi "compile twice" shape), so the RAM-resident
    // loop carries none of the Next code (2026-10-10).
    if (Z80Ops::isNext) { nextFrameLoop(_loop_t0, pbbp, nbp, zifi_pump_due); return; }
    if (__builtin_expect(Z80Ops::isSam, 0)) { samFrameLoop(_loop_t0, pbbp, nbp, zifi_pump_due); return; }
    frameRest<false>(_loop_t0, pbbp, nbp, zifi_pump_due);
}

// The rest of the frame after the INT window head; NEXT selects the Next shape.
template <bool NEXT>
__attribute__((always_inline)) inline void CPU::frameRest(uint64_t _loop_t0, bool pbbp, int nbp, uint32_t zifi_pump_due) {

    while (tstates < IntEnd) {
        // Next: a zxnDMA block that holds the bus (nxStall) runs before the CPU
        // executes anything. A block crossing the frame end resumes here, and
        // the next guest instruction must not run first: Jumping Jack's 16K
        // sprite upload from MMU 0/1 had its tail read from ROM, because the
        // instruction after the OUT put the ROM back (hw 2026-10-10).
        if (NEXT && Z80DMA::nxPending && Z80DMA::nxStall && Z80DMA::nxService(IntEnd)) { BREAKPOINTS continue; }
        Z80::execute();
        if (Config::dma_mode || NEXT) Z80DMA::handleDMA();
        if (__builtin_expect(NEXT && Z80::isNMI(), 0) && Z80::atInstrBoundary()) Z80::doNMI();
        if (ZiFi::cdcNicActive && tstates >= zifi_pump_due) {
            zifi_pump_due = tstates + 3500;
            ZiFi::cdcPump();
        }
        BREAKPOINTS
    }
    BREAKPOINTS
    bool halted = Z80::isHalted();
    if (NEXT) {
        // Next: the middle of the frame (up to the tail window) as slices to the
        // next interrupt event. With nothing beyond the classic frame INT armed
        // (NextInt::ex = 0) that is one unchecked run, exactly the shape every
        // other machine has. Otherwise — line interrupt, hardware IM2, a CTC
        // channel with its interrupt on — unchecked slices up to each event, one
        // checked instruction while /INT is up, and HALT fast-forwarded to the
        // next event without flushing the frame (NEXTVID renders by line). A
        // write that arms a source mid-slice ends the slice (NextInt::wake), as
        // do EI / RETI / RETN, so the loop re-derives the next event. The cost
        // of running the whole frame checked instead was TX-1696 at half speed.
        // Re-read every pass: NR 0x07 changes the clock MID-FRAME and rescales
        // statesInFrame / IntEnd / tstates (NextReg::applySpeed). A bound taken
        // once at the frame start stayed in the old units, so after 14 -> 3.5 MHz
        // the loop ran on far past the frame end (tstates 186074 in a 70908
        // frame) and the next AY catch-up wrote 1646 samples into a 640 buffer —
        // over the USB host's endpoint pool and hdmi_isr_mode (hw 2026-10-08).
        uint32_t sliceEnd = statesInFrame - IntEnd;
        bool stamped = false;
        halted = false;
        while (tstates < (sliceEnd = statesInFrame - IntEnd)) {
            // A mid-frame NMI (Multiface, NR 02 bit 3) is taken at the boundary
            // its request ended the slice on — see NextMf::request().
            if (__builtin_expect(Z80::isNMI(), 0) && Z80::atInstrBoundary()) {
                Z80::doNMI();
                BREAKPOINTS
                continue;
            }
            // Paced zxnDMA (Z80DMA.h): a byte that is due, or a DMA holding the
            // bus (the CPU does not execute then) — slices below end at nxNextT.
            // nxService answers false when NR 0xCC-0xCE hand the bus to the CPU
            // (an interrupt in service): the CPU runs until RETI / RETN wakes us.
            if (__builtin_expect(Z80DMA::nxPending, 0) &&
                (Z80DMA::nxStall || tstates >= Z80DMA::nxNextT) &&
                Z80DMA::nxService(sliceEnd)) {
                BREAKPOINTS
                continue;
            }
            const uint32_t dmaT = (Z80DMA::nxPending && !NextInt::dmaDelay()) ? Z80DMA::nxNextT : 0xFFFFFFFFu;
            if (NextInt::ex) {
                if (Z80::isIFF1() && NextInt::intLine()) {
                    // at a boundary take it at once (execute() would run one
                    // more instruction first); pendingEI / a fetched prefix fall
                    // through to execute(), whose own checkINT takes it
                    if (Z80::atInstrBoundary()) Z80::checkINT();
                    if (Z80::isIFF1()) Z80::execute();
                    BREAKPOINTS
                    continue;
                }
                // intLine() above has synced whenever IFF1 is set
                uint32_t target = Z80::isIFF1() ? NextInt::nextEventTSynced(sliceEnd) : sliceEnd;
                if (dmaT < target) target = dmaT;
                if (Z80::isHalted()) {
                    if (!stamped) { tstates_active = tstates; stamped = true; }
                    halted = true;
                    NEXTVID::haltAdvanceTo(target);
                } else {
                    stFrame = target;
                    Z80::exec_nocheck();
                    if (stFrame == 0) {
                        if (!stamped) { tstates_active = tstates; stamped = true; }
                        halted = true;
                        NEXTVID::haltAdvanceTo(target);
                    }
                }
            } else {
                const uint32_t end = dmaT < sliceEnd ? dmaT : sliceEnd;
                if (!Z80::isHalted()) {
                    stFrame = end;
                    Z80::exec_nocheck();
                    if (stFrame != 0) { BREAKPOINTS continue; }   // ended early: ex raised / DMA due
                }
                if (!stamped) { tstates_active = tstates; stamped = true; }
                halted = true;
                NEXTVID::haltAdvanceTo(end);
            }
            BREAKPOINTS
        }
    } else
    if (!halted) {
        stFrame = statesInFrame - IntEnd;
        Z80::exec_nocheck();
        if (stFrame == 0) { tstates_active = tstates; FlushOnHalt(); halted = true; }
    } else {
        tstates_active = tstates; FlushOnHalt();
    }
    BREAKPOINTS
    while (tstates < statesInFrame) {
        if (NEXT && Z80DMA::nxPending && Z80DMA::nxStall && Z80DMA::nxService(statesInFrame)) { BREAKPOINTS continue; }
        Z80::execute();
        if (Config::dma_mode || NEXT) Z80DMA::handleDMA();
        if (__builtin_expect(NEXT && Z80::isNMI(), 0) && Z80::atInstrBoundary()) Z80::doNMI();
        if (ZiFi::cdcNicActive && tstates >= zifi_pump_due) {
            zifi_pump_due = tstates + 3500;
            ZiFi::cdcPump();
        }
        BREAKPOINTS
    }
    {
        uint64_t _ef_t0 = time_us_64();
        VIDEO::EndFrame();
        endframe_us = (uint32_t)(time_us_64() - _ef_t0);
    }

    CPU::tstates_diff += CPU::tstates - CPU::prev_tstates;

    if ((ESPectrum::fdd.control & (kRVMWD177XHLD | kRVMWD177XHLT)) != 0)
    {
        uint64_t _fdd_t0 = time_us_64();
        rvmWD1793Step(&ESPectrum::fdd, CPU::tstates_diff / WD177XSTEPSTATES); // FDD
        fdd_step_us += (uint32_t)(time_us_64() - _fdd_t0);
    }
    CPU::tstates_diff = CPU::tstates_diff % WD177XSTEPSTATES;

    cpu_frame_us += (uint32_t)(time_us_64() - _loop_t0);

#if PERF_TRACE
    { extern uint32_t g_frm_int_taken, g_frm_int_miss;
      if (!g_frm_int_taken) g_frm_int_miss++;
      g_frm_int_taken = 0;
      extern uint32_t g_brd_first_t, g_brd_min, g_brd_max; extern bool g_brd_first_set;
      if (g_brd_first_set) { if (g_brd_first_t < g_brd_min) g_brd_min = g_brd_first_t;
                             if (g_brd_first_t > g_brd_max) g_brd_max = g_brd_first_t; }
      g_brd_first_set = false; { extern bool g_halt_set; g_halt_set = false; } }
#endif
    global_tstates += statesInFrame; // increase global Tstates
    tstates_frame = tstates;
    if (!halted) tstates_active = tstates_frame; // no HALT this frame: full load
    if (NEXT) {
        NextInt::frameEnd();                     // before the wrap, see NextInt.h
        Z80DMA::nxFrameEnd(statesInFrame);       // paced DMA's next byte, frame-relative
    }
    tstates -= statesInFrame;

    CPU::prev_tstates = tstates;
}

// SAM Coupe frame: two INT sources, the frame window at T 0 and the line window
// (STATUS bit 0, Sam::lineIntStart). Unchecked slices up to the next window,
// checked execute() while a window is up — the Stage D shape without an EI hook:
// a window is only 128 T, so it is simply run checked whatever IFF1 says, and an
// EI after it closed misses it, as on the chip. FLASH: SAM is butter-PSRAM only
// and its frame costs the RAM-resident loop one test.
__attribute__((noinline)) void CPU::samFrameLoop(uint64_t _loop_t0, bool pbbp, int nbp, uint32_t zifi_pump_due) {
    (void)zifi_pump_due;
    bool halted = false;
    while (tstates < statesInFrame) {
        const uint32_t t = tstates;
        const uint32_t tail = statesInFrame - IntEnd;       // the next frame's INT, by wrap
        if (t < (uint32_t)IntEnd || t >= tail || Sam::intWindowUp(t)) {
            Z80::execute();
            BREAKPOINTS
            continue;
        }
        uint32_t next = tail;
        const uint32_t ls = Sam::nextIntStart(t);             // line INT or MIDI-out INT
        if (ls < next) next = ls;
        if (Z80::isHalted()) {
            if (!halted) { tstates_active = tstates; halted = true; }
            haltAdvanceTo(next);
        } else {
            stFrame = next;
            Z80::exec_nocheck();
            if (stFrame == 0) {
                if (!halted) { tstates_active = tstates; halted = true; }
                haltAdvanceTo(next);
            }
        }
        BREAKPOINTS
    }
    {
        uint64_t _ef_t0 = time_us_64();
        VIDEO::EndFrame();
        endframe_us = (uint32_t)(time_us_64() - _ef_t0);
    }
    CPU::tstates_diff += CPU::tstates - CPU::prev_tstates;
    CPU::tstates_diff = CPU::tstates_diff % WD177XSTEPSTATES;
    cpu_frame_us += (uint32_t)(time_us_64() - _loop_t0);
    global_tstates += statesInFrame;
    tstates_frame = tstates;
    if (!halted) tstates_active = tstates_frame;
    tstates -= statesInFrame;
    CPU::prev_tstates = tstates;
    Sam::frameStart();               // arm the row tick for the frame that just began
}

NX_OVL_CODE void CPU::nextFrameLoop(uint64_t _loop_t0, bool pbbp, int nbp, uint32_t zifi_pump_due) {
    frameRest<true>(_loop_t0, pbbp, nbp, zifi_pump_due);
}

IRAM_ATTR void CPU::FlushOnHalt() {
#if PERF_TRACE
    { extern uint32_t g_halt_t; extern bool g_halt_set;
      if (!g_halt_set) { g_halt_t = CPU::tstates; g_halt_set = true; } }
#endif
    FlushOnHaltTo(statesInFrame - IntEnd);
}

// Fast-forward a halted CPU (burning NOPs and raster) to an explicit target.
// The classic frame loop always targets statesInFrame - IntEnd; TS-Conf's
// sliced loop targets IntStart (a HALT before the programmable INT window
// wakes at the window) or statesInFrame.
// Advance a HALTed CPU to stEnd through the video state machine one line at a
// time (TS-Conf has no memory contention and no snow, so no per-4T stepping is
// needed). Unlike FlushOnHaltTo this does NOT flush the rest of the frame's
// video, so rendering stays in step with register writes made after the wake.
IRAM_ATTR void CPU::haltAdvanceTo(uint32_t stEnd) {
    PERF_BUCKET_SCOPE(PB_HALT);
    tstates_active = tstates;
    const uint32_t pre = tstates;
    while (tstates < stEnd) {
        uint32_t n = stEnd - tstates;
        if (n > VIDEO::tStatesPerLine && VIDEO::tStatesPerLine) n = VIDEO::tStatesPerLine;
        VIDEO::Draw(n, false);
    }
    Z80::incRegR((tstates - pre) >> 2);
}

IRAM_ATTR void CPU::FlushOnHaltTo(uint32_t stEnd) {

    if (Z80Ops::isNext) {
        // NEXTVID renders by scanline and never re-points VIDEO::Draw, so the
        // legacy "flush until Draw == Blank" below spins forever (hw: core0
        // stuck here on NextZXOS's first HALT, tstates looping the 32-bit
        // ring, R/PC frozen). NEXTVID::haltAdvanceTo skips to each line
        // boundary instead of ticking 4T at a time.
        NEXTVID::haltAdvanceTo(stEnd);
        return;
    }

    uint8_t page = Z80::getRegPC() >> 13;
    if (MemESP::ramContended[page]) {

        while (tstates < stEnd ) {
            VIDEO::Draw_Opcode(true);
            Z80::incRegR(1);
        }

    } else {

        if (VIDEO::snow_toggle) {

            // ULA perfect cycle & snow effect use this code
            while (tstates < stEnd ) {
                VIDEO::Draw_Opcode(false);
                Z80::incRegR(1);
            }

        } else {

            // Flush the rest of frame
            uint32_t pre_tstates = tstates;
            while (VIDEO::Draw != &VIDEO::Blank)
                VIDEO::Draw(VIDEO::tStatesPerLine, false);
            tstates = pre_tstates;

            uint32_t incr = (stEnd - pre_tstates) >> 2;
            if (pre_tstates & 0x03) incr++;
            if (Z80Ops::isProfi) {
                // Land the HALT-wake on the absolute 4T frame grid (phase 0)
                // rather than inheriting pre_tstates' phase. A HALT only idles
                // waiting for INT, so its exact wake T-state is a modelling
                // choice, not discarded computation — same incr, hence same
                // regR. The inherited phase depends on the (non-deterministic)
                // flashload/CP/M handoff timing, which made INT-synced border
                // effects (mcprofi2016) jitter 0..3T per launch; a phase-0
                // landing is launch-independent. Other archs keep the
                // inherited-phase model their border timings were calibrated
                // against.
                tstates = (pre_tstates & ~3u) + (incr << 2);
            } else
                tstates += (incr << 2);
            Z80::incRegR(incr);

        }

    }

}

// RZX playback frame (Rzx.h). FLASH on purpose — it runs only while a
// recording plays, and the per-instruction work is Z80::execute(), which is RAM.
//
// A recorded frame is "N opcode fetches, then an interrupt", and it is NOT tied
// to our raster: the loop runs the CPU through the ordinary checked execute()
// (every IN reaches Rzx::onIn through the core's hook, HALT steps 4 T per call
// exactly as the recording emulator counted it) and, the moment the count is
// reached at an instruction boundary, raises the INT line and samples it at once
// — the same point at which the recording emulator took it (Fuse: the frame
// event fires between instructions and z80_interrupt runs straight away). The
// T-state frame keeps running underneath, so video, audio and pacing stay at
// 50 Hz: a recording from a machine with a slightly shorter frame simply takes
// its interrupts a little more often, which is what the fetch counts say.
void CPU::loopRzx(uint64_t _loop_t0) {
    if (Rzx::snapshotPending()) Rzx::loadPendingSnapshot();

    uint32_t zifi_pump_due = tstates + 3500;
    while (tstates < statesInFrame && Rzx::mode != 0) {
        if (Rzx::frameReached() && Z80::atInstrBoundary()) {
            if (!Rzx::nextFrame()) break;   // playback over: `mode` is OFF now
            const bool afterOne = Rzx::raiseInt();
            if (Rzx::snapshotPending()) break;
            if (afterOne) {
                // SPIN rule with an EI pending at the boundary: the interrupt
                // belongs after the next instruction, where execute() samples
                // it itself (and interrupt() drops the line). A prefix byte is
                // not an instruction — run on to the real boundary.
                do { Z80::execute(); } while (!Z80::atInstrBoundary());
                Rzx::intUntil = 0;
            } else {
                Z80::rzxLdAirIntBug();
                Z80::checkINT();
            }
            continue;
        }
        Z80::execute();
        if (Config::dma_mode) Z80DMA::handleDMA();
        if (ZiFi::cdcNicActive && tstates >= zifi_pump_due) {
            zifi_pump_due = tstates + 3500;
            ZiFi::cdcPump();
        }
    }
    if (Rzx::mode != 0) {
        // A mid-file snapshot is due: the machine idles to the frame end and
        // the snapshot is loaded at the next loop entry, i.e. between frames,
        // where every other loader runs too.
        while (tstates < statesInFrame) {
            uint32_t n = statesInFrame - tstates;
            if (n > VIDEO::tStatesPerLine) n = VIDEO::tStatesPerLine;
            VIDEO::Draw(n, false);
        }
    } else {
        // Playback ended inside this frame: the machine carries on under its
        // own raster from here ("continue from here").
        while (tstates < statesInFrame) {
            Z80::execute();
            if (Config::dma_mode) Z80DMA::handleDMA();
        }
    }

    {
        uint64_t _ef_t0 = time_us_64();
        VIDEO::EndFrame();
        endframe_us = (uint32_t)(time_us_64() - _ef_t0);
    }
    CPU::tstates_diff += CPU::tstates - CPU::prev_tstates;
    if ((ESPectrum::fdd.control & (kRVMWD177XHLD | kRVMWD177XHLT)) != 0) {
        uint64_t _fdd_t0 = time_us_64();
        rvmWD1793Step(&ESPectrum::fdd, CPU::tstates_diff / WD177XSTEPSTATES);
        fdd_step_us += (uint32_t)(time_us_64() - _fdd_t0);
    }
    CPU::tstates_diff = CPU::tstates_diff % WD177XSTEPSTATES;
    cpu_frame_us += (uint32_t)(time_us_64() - _loop_t0);

    global_tstates += statesInFrame;
    tstates_frame = tstates;
    tstates_active = tstates_frame;
    tstates -= statesInFrame;
    Rzx::endTFrame(statesInFrame);
    CPU::prev_tstates = tstates;
}

// Z80Ops

// Guest data access with the NeoGS ZX-DMA window folded in (GS.h g_ngs_zxdma).
// While the card runs DMA module 1, every host access to 0x0000-0x3FFF is one
// card-RAM access at the card's DMA address instead (docs/dma_zx_doc.txt).
//   read  — only when ROM is really paged there: the FPGA gates the DMA cycle on
//           CSROM, so with RAM at page 0 a real card stays silent and the host
//           reads its own RAM.
//   write — regardless of what is paged there, and it does NOT replace the
//           normal store: the byte goes to card RAM *and* to the host's RAM if
//           that window is RAM (NedoPC's own test program verifies itself that
//           way). ROM there simply swallows the second store as usual.
// Opcode fetch is deliberately not hooked — the doc's own rule is that
// interrupts must be off while the window is open (or the ZX RSTs to $38 and
// executes card data), so nothing legitimately runs code from the window.
//
// Timex TC2068 SCLD horizontal MMU (Timex.h). While port #F4 selects a DOCK or
// EX-ROM chunk for an 8 KB slot, that slot is NOT the HOME bank and never reaches
// MemESP at all — so the test has to come first, and it has to be here rather
// than in MemESP::readbyte/writebyte, which are inlined into ~170 sites.
// g_timex_mmu is zero on every other machine and on a TC2068 whose map is all
// HOME, which is the normal state right up to the moment a cartridge starts.
static IRAM_ATTR __attribute__((noinline)) uint8_t timexPeek8(uint16_t address) {
    uint8_t* p = Timex::rd[address >> 13];
    return (p == Timex::kOpen) ? 0xFF          // nothing plugged into that chunk
                               : p[address & 0x1FFF];
}

#if ATM_PAGE_TRACE
// ATM trace: CPU reads of #C044/#C04A/#C050 — NedoOS BDOS_getmainpages reading an
// app's page table through window 3 — with the page they hit and the byte returned.
static __attribute__((noinline)) void atmPgTableReadTrace(uint16_t address) {
    static uint16_t n = 0;
    if (n >= 120) return;
    n++;
    const uint8_t* pg = MemESP::ramCurrent[6];   // 16K window 3 base (8K slot 6)
    Debug::log("[ATMRD] rd %04X=%02X pc=%04X page=%p 7ffd=%02X F7w3=%03X/%03X", address,
               pg[address & 0x3FFF], Z80::getRegPC(), (const void*)pg, Atm::p7ffd,
               Atm::pF7[3], Atm::pF7[7]);
}
#endif
static inline uint8_t gsDmaPeek8(uint16_t address) {
#if ATM_PAGE_TRACE
    if (Z80Ops::isAtm && (address == 0xC044 || address == 0xC04A || address == 0xC050))
        atmPgTableReadTrace(address);
#endif
    if (__builtin_expect(g_timex_mmu != 0, 0) && Timex::rd[address >> 13])
        return timexPeek8(address);
    // Spectrum Next slot with no SRAM behind it: the data latch (MemESP.h).
    if (__builtin_expect(MemESP::next_stale_mask != 0, 0) && ((MemESP::next_stale_mask >> (address >> 13)) & 1))
        return MemESP::nextStaleByte();
    if (__builtin_expect(g_ngs_zxdma != 0, 0) && address < 0x4000
        && !MemESP::page0ram && !MemESP::newSRAM && !MemESP::divmmc_mapped)
        return GS::zxDmaRead();
    return MemESP::readbyte(address);
}

#if ZC_PORT_TRACE || ATM_PAGE_TRACE
// ATM trace: every CPU write to offset #0044 / #004A / #0050 of any window — the
// NedoOS user-kernel page table (curpg16k/32klow/32khigh) — with the page pointer
// the write lands in. The 2026-09-28 NedoOS ATM3 capture showed term.com reading
// 00/FB/FB out of a table sys_newapp had just written F2/F1/F0 into.
extern bool g_atm_trace_armed;   // Ports.cpp
static void atmPgTableWriteTrace(uint16_t address, uint8_t value) {
    // Arm the port trace at the second app's page table (NedoOS term: C050=F0).
    if (address == 0xC050 && value == 0xF0) g_atm_trace_armed = true;
    static uint16_t n = 0;
    if (n >= 200) return;
    n++;
    Debug::log("[ATMPT] wr %04X=%02X pc=%04X page=%p 7ffd=%02X", address, value, Z80::getRegPC(),
               (const void*)MemESP::ramCurrent[(address >> 14) << 1], Atm::p7ffd);
}
#endif
static inline void gsDmaPoke8(uint16_t address, uint8_t value) {
#if ZC_PORT_TRACE || ATM_PAGE_TRACE
    if (Z80Ops::isAtm) {
        const uint16_t off = address & 0x3FFF;
        // Watch the NedoOS user-kernel TEMPLATE: its physical address is the LDIR
        // source at the first template write (C044=FB from pc #4A39/#4A3A).
        static const uint8_t* s_tpl = nullptr;
        if (!s_tpl && address == 0xC044 && value == 0xFB) {
            const uint16_t hl = Z80::getRegHL();
            for (int d = 0x43; d <= 0x46; d++) {
                const uint16_t src = (uint16_t)(hl - d);
                const uint8_t* p = MemESP::ramCurrent[src >> 13] + (src & 0x1FFF);
                if (p[0] == 0x3E && p[2] == 0xD3 && p[3] == 0xFD) { s_tpl = p;
                    Debug::log("[ATMTPL] template at %04X phys=%p (hl=%04X)", src, (const void*)p, hl); break; }
            }
        }
        if (s_tpl) {
            const uint8_t* ph = MemESP::ramCurrent[address >> 13] + (address & 0x1FFF);
            if (ph >= s_tpl && ph < s_tpl + 16) {
                static uint16_t n = 0;
                if (n < 80) { n++;
                    Debug::log("[ATMTPL] wr %04X=%02X pc=%04X +%d 7ffd=%02X de=%04X hl=%04X bc=%04X",
                               address, value, Z80::getRegPC(), (int)(ph - s_tpl), Atm::p7ffd,
                               Z80::getRegDE(), Z80::getRegHL(), Z80::getRegBC()); }
            }
        }
        if (off == 0x0044 || off == 0x004A || off == 0x0050) atmPgTableWriteTrace(address, value);
        else if (off >= 0x0005 && off <= 0x0008 && g_atm_trace_armed) {
            static uint16_t n = 0;
            if (n < 80) { n++;
                Debug::log("[ATMK] wr %04X=%02X pc=%04X page=%p 7ffd=%02X", address, value,
                           Z80::getRegPC(), (const void*)MemESP::ramCurrent[(address >> 14) << 1], Atm::p7ffd); }
        }
    }
#endif
    if (__builtin_expect(g_timex_mmu != 0, 0) && Timex::rd[address >> 13]) {
        // A mapped slot swallows the write: a ROM cartridge chunk and the EX-ROM
        // drop it, a RAM chunk takes it. Either way it must NOT fall through to
        // the HOME bank underneath — the SCLD has disconnected it.
        Timex::write8(address, value);
        return;
    }
    if (__builtin_expect(g_ngs_zxdma != 0, 0) && address < 0x4000)
        GS::zxDmaWrite(value);
    // ATM-Turbo: a window showing ROM drops the write (its pages may be flattened
    // copies in butter PSRAM, which writebyte's flash-pointer filter does not see).
    if (__builtin_expect(g_atm_ro != 0, 0)) {
        if (g_atm_ro & 0x80) EvoBase::font[address & 0x7FF] = value;   // ZX-Evo font RAM (#BF D2)
        if ((g_atm_ro >> (address >> 14)) & 1) return;
    }
    // TS-Conf write-side hooks: the FMAddr window (CRAM/SFILE/register file —
    // does NOT replace the normal store, the hardware writes RAM and the FPGA
    // array in parallel, reference z80_main.inl:108) and the W0_WE protect
    // (which does suppress the store).
    if (__builtin_expect(g_tsconf_wr != 0, 0))
        if (TsConf::cpuWriteGate(address, value)) return;
    MemESP::writebyte(address, value);
}

// Read byte from RAM
// TS-Conf fast path (TsFastMem.h). The accessors below are shaped so that the
// fast path is a LEAF: every call (the line tick, the write gate, the generic
// path) is a tail call into a noinline helper, so the hot path runs without a
// push/pop pair — that prologue was a third of the fast path's instructions.
static IRAM_ATTR __attribute__((noinline)) uint8_t peek8_generic(uint16_t address);
static IRAM_ATTR __attribute__((noinline)) uint8_t peek8_tick(uint16_t address) {
    VIDEO::tsDrawTick();
    return MemESP::ramCurrent[address >> 13][address & 0x1FFF];
}
// TS-Conf DRAM model live (g_ts_memcyc: 14 MHz wait states and/or a running DMA
// the access steals a cycle from) — a tail call, so peek8 itself stays a leaf.
static IRAM_ATTR __attribute__((noinline)) uint8_t peek8_dram(uint16_t address) {   // a cache MISS (tsMemNoDram was false)
    CPU::tstates += 3 + TsConf::cpuMemMiss(address, false);
    if (CPU::tstates >= VIDEO::ts_line_t) VIDEO::tsDrawTick();
    return MemESP::ramCurrent[address >> 13][address & 0x1FFF];
}
IRAM_ATTR uint8_t Z80Ops::peek8(uint16_t address) {
    TS_PAGE_HIT(address);
    C16_RD(address);
    const uint32_t gates = g_memgates.w;   // MemGates.h: one load for the whole gate set
    if (__builtin_expect(g_ts_fastrd > tsFastLo8(address) && MEMGATES_READ_PLAIN(gates), 1)) {
        // No line test here: the line-boundary tick is taken at the next M1
        // (the fast fetch's tsFastTick), at most one instruction late — a few T
        // of a 224..1792-T line, the same as a write landing one instruction
        // earlier (ccr-05d4d17f-wh3s7v).
        CPU::tstates += 3;
        return MemESP::ramCurrent[address >> 13][address & 0x1FFF];
    }
    if (g_ts_fastrd > tsFastLo8(address)) {
        // The DRAM hit test runs FIRST, while nothing but the address is live:
        // placed after the T-state add it needed two more registers and GCC
        // paid a push/pop pair on EVERY access (2026-09-13). The _dram helpers
        // add the base T-states themselves.
        if (__builtin_expect(g_ts_memcyc != 0, 0) && !tsMemNoDram(address)) return peek8_dram(address);
        CPU::tstates += 3;
        if (__builtin_expect(CPU::tstates >= VIDEO::ts_line_t, 0)) return peek8_tick(address);
        return MemESP::ramCurrent[address >> 13][address & 0x1FFF];
    }
    return peek8_generic(address);
}
static IRAM_ATTR __attribute__((noinline)) uint8_t peek8_generic(uint16_t address) {
    VIDEO::Draw(3, MemESP::ramContended[address >> 13]);
    if (__builtin_expect(g_ts_memcyc != 0, 0)) {              // TS-Conf DRAM model (TsConf.cpp)
        const uint32_t w = TsConf::cpuMemRead(address, false);
        if (w) VIDEO::Draw(w, false);
    }
    // ProfROM plane switch — and on this firmware the switch IS a DATA read, so
    // this hook is the load-bearing one (hw 2026-09-04: without it ProfROM ran
    // its RAM test and then fell back into plane 0's 128 ROM = "black screen,
    // then 48K"). Both users of the window read it: the monitor's plane-select
    // at bank2 0x3C7E (LD HL,#0110+target / LD L,(HL) / LD L,(HL)) and the RAM
    // trampoline every plane's bank 0 copies to 0x5BEE (LD HL,#010C /
    // LD L,(HL)). Only ProfROM ever arms g_gmx_tap — GMX pins it false because
    // its own monitor sweeps the window while checksumming itself.
    // Order matters: the byte is fetched from the plane that was live when the
    // access started and the switch applies to everything AFTER it, which is
    // what MAME's install_read_tap does (the tap runs on the value already
    // read). The monitor's routine keeps using L afterwards.
    uint8_t v = gsDmaPeek8(address);
    if (__builtin_expect(g_gmx_tap != 0, 0) && (address & 0xFFF0) == 0x0100)
        Ports::gmxProfRomTap(address);
    return v;
}

// Only the Next direct-read path calls this. Instant mapping precedes the
// byte read; delayed entry/exit follows it, exactly as in the generic fetch.
IRAM_ATTR __attribute__((noinline, optimize("O2"))) uint8_t DivMMC::nextOpcodeSlow(uint16_t pc) {
    preOpcFetch(pc);
    const uint8_t op = MemESP::ramCurrent[pc >> 13][pc & 0x1FFF];
    postOpcFetch();
    return op;
}

// Fetch opcode from RAM (NON +2A/3 version)
#if DEBUG
uint16_t dbg_last_pc = 0;
#endif
IRAM_ATTR uint8_t Z80Ops::fetchOpcode() {
    uint16_t pc = Z80::getRegPC();
#if DEBUG
    dbg_last_pc = pc;
#endif
    uint8_t pg = pc >> 13;
    // TsFastMem.h — the same two fast cases as Z80::exec_nocheck's fetch. This
    // one serves the prefixed opcodes (DD/FD/CB/ED second bytes) and the
    // checked loop, which used to walk the whole generic chain below.
    if (g_ts_fastmem > tsFastLo8(pc)) {
        tsFastTick(4);
        if (__builtin_expect(g_ts_memcyc != 0, 0) && !tsMemNoDram(pc)) tsFastTick(TsConf::cpuMemMiss(pc, true));
        C16_FE(pc);
        return MemESP::ramCurrent[pg][pc & 0x1fff];
    }
    if (g_ts_fastrd == 2) {   // Next below 0x4000 (see exec_nocheck)
        tsFastTick(4);
        if (DivMMC::enabled) return DivMMC::nextOpcode(pc);
        return MemESP::ramCurrent[pg][pc & 0x1fff];
    }
    // Scorpion Yellow-PCB even-M1 (see the g_scorp_even_m1 note in CPU.h): an M1
    // cycle fetching from 0x4000+ that would start on an odd T-state gets one
    // wait T first. Draw(1) so the renderer accounts the extra tact too.
    if (g_scorp_even_m1 && (pc & 0xC000) && (CPU::tstates & 1))
        VIDEO::Draw(1, false);
    // The M1 half of the same tap (ZXMAK2 and MAME both hook fetch and read).
    // ProfROM uses it for cross-plane jumps — plane 1 bank 3 does JP #010E at
    // bank offset 0x0030 — while the ordinary plane switch is the data read in
    // peek8 above.
    if (g_gmx_tap && (pc & 0xFFF0) == 0x0100)
        Ports::gmxProfRomTap(pc);
    VIDEO::Draw_Opcode(MemESP::ramContended[pg]);
    if (__builtin_expect(g_ts_memcyc != 0, 0) && !tsMemNoDram(pc)) {   // TS-Conf DRAM model (TsConf.cpp)
        const uint32_t w = TsConf::cpuMemMiss(pc, true);
        if (w) VIDEO::Draw(w, false);
    }
    if (DivMMC::enabled) {
        DivMMC::preOpcFetch(pc);
        pg = pc >> 13; // re-read in case instant map changed it
        uint8_t opCode;
        if (pg <= 1 && MemESP::divmmc_mapped) {
            opCode = (pg == 0) ? MemESP::page0_lo[pc] : MemESP::page0_hi[pc & 0x1FFF];
        } else {
            opCode = MemESP::romPeek(pg, MemESP::ramCurrent[pg], pc);
        }
        DivMMC::postOpcFetch();
        return opCode;
    }
    // MB-02+: page0 mapped but DivMMC not active — check divmmc_mapped for fetch
    if (pg <= 1 && MemESP::divmmc_mapped) {
        return (pg == 0) ? MemESP::page0_lo[pc] : MemESP::page0_hi[pc & 0x1FFF];
    }
    // Timex SCLD: unlike the NeoGS ZX-DMA window this one MUST be hooked on the
    // fetch path — a DOCK cartridge and the EX-ROM are code, and running them is
    // the whole point (the TS2068 extended BASIC lives in the EX-ROM, and every
    // LROS cartridge takes over from 0x0000).
    if (__builtin_expect(g_timex_mmu != 0, 0) && Timex::rd[pc >> 13])
        return timexPeek8(pc);
    TS_PAGE_HIT(pc);
    C16_FE(pc);
    return MemESP::romPeek(pg, MemESP::ramCurrent[pg], pc);
}

// // Write byte to RAM
// IRAM_ATTR void Z80Ops::poke8(uint16_t address, uint8_t value) {

//     uint8_t page = address >> 14;

//     if (page == 0) {
//         VIDEO::Draw(3, false);
//         return;
//     }

//     #ifndef DIRTY_LINES

//     VIDEO::Draw(3, MemESP::ramContended[page]);
//     MemESP::ramCurrent[page][address & 0x3fff] = value;

//     #else

//     if (page == 2) {
//         VIDEO::Draw(3, false);
//         MemESP::ramCurrent[2][address & 0x3fff] = value;
//         return;
//     }

//     VIDEO::Draw(3, MemESP::ramContended[page]);

//     if (page == 3) {
//         if (MemESP::videoLatch) {
//             if (MemESP::bankLatch != 7) {
//                 MemESP::ramCurrent[3][address & 0x3fff] = value;
//                 return;
//             }
//         } else if (MemESP::bankLatch != 5) {
//             MemESP::ramCurrent[3][address & 0x3fff] = value;
//             return;
//         }
//     } else if (MemESP::videoLatch) {
//         // Page == 1 == videoLatch
//         MemESP::ramCurrent[1][address & 0x3fff] = value;
//         return;
//     }

//     uint16_t vid_line = address & 0x3fff;

//     if (vid_line < 6144) {

//         uint8_t result =  (vid_line >> 5) & 0b11000000;
//         result |=  (vid_line >> 2) & 0b00111000;
//         result |=  (vid_line >> 8) & 0b00000111;

//         VIDEO::dirty_lines[result] |= 0x01;

//     } else if (vid_line < 6912) {

//         uint8_t result = ((vid_line - 6144) >> 5) << 3;
//         // for (int i=result; i < result + 8; i++)
//         //     VIDEO::dirty_lines[i] = (value & 0x80) | 0x01;
//         memset((uint8_t *)VIDEO::dirty_lines + result, (value & 0x80) | 0x01, 8);

//     }

//     MemESP::ramCurrent[page][vid_line] = value;

//     #endif

// }

// Write byte to RAM
static inline void tsPoke8Store(uint16_t address, uint8_t value) {
    const uint8_t pg = address >> 13;
    uint8_t* p = MemESP::ramCurrent[pg];
    if ((uintptr_t)p < 0x11000000u) return;       // TS-BIOS ROM window (flash pointer)
    *mem_desc_t::bank_dirty[pg] = true;
    p[address & 0x1FFF] = value;
}
static IRAM_ATTR __attribute__((noinline)) void poke8_generic(uint16_t address, uint8_t value);
static IRAM_ATTR __attribute__((noinline)) void poke8_tick(uint16_t address, uint8_t value) {
    VIDEO::tsDrawTick();
    tsPoke8Store(address, value);
}
static IRAM_ATTR __attribute__((noinline)) void poke8_cold(uint16_t address, uint8_t value) {
    // the FMAddr / W0_WE write gate, a running DMA and/or a cache invalidate
    // (the base T-states are ours to add)
    CPU::tstates += 3;
    if (CPU::tstates >= VIDEO::ts_line_t) VIDEO::tsDrawTick();
    if (g_ts_memcyc) TsConf::cpuMemWrite(address);
    if (g_tsconf_wr && TsConf::cpuWriteGate(address, value)) return;
    // ATM-Turbo / ZX-Evo on the fast path: the ZX-Evo font RAM takes the byte too,
    // a window showing ROM drops it (gsDmaPoke8's twin).
    if (g_atm_ro) {
        if (g_atm_ro & 0x80) EvoBase::font[address & 0x7FF] = value;
        if ((g_atm_ro >> (address >> 14)) & 1) return;
    }
    tsPoke8Store(address, value);
}
IRAM_ATTR void Z80Ops::poke8(uint16_t address, uint8_t value) {
    TS_PAGE_HIT(address);
    C16_WR(address);
    const uint32_t gates = g_memgates.w;
    if (__builtin_expect(g_ts_fastmem > tsFastLo8(address) && MEMGATES_WRITE_PLAIN(gates), 1)) {
        CPU::tstates += 3;   // line tick at the next M1 (see peek8)
        tsPoke8Store(address, value);
        return;
    }
    if (g_ts_fastmem > tsFastLo8(address)) {
        // Same register discipline as peek8: the cache invalidate and the two
        // cold gates go first, the T-state add and the line test last.
        if (__builtin_expect(g_ts_memcyc != 0, 0)) {
            // a running DMA (the access steals a cycle) or a write that HITS the
            // cache (invalidate) — both are poke8_cold's, through TsConf::cpuMemWrite
            if ((g_ts_memcyc & 2) || tsMemWriteHit(address)) return poke8_cold(address, value);
        }
        if (__builtin_expect((g_tsconf_wr | g_atm_ro) != 0, 0)) return poke8_cold(address, value);   // TS write gate / ATM ROM windows
        CPU::tstates += 3;
        if (__builtin_expect(CPU::tstates >= VIDEO::ts_line_t, 0)) return poke8_tick(address, value);
        tsPoke8Store(address, value);
        return;
    }
    return poke8_generic(address, value);
}
static IRAM_ATTR __attribute__((noinline)) void poke8_generic(uint16_t address, uint8_t value) {
    // Spectrum Next, a write below 0x4000 into the Layer 2 write window (port
    // 0x123B, segments 0-2): a plain store into the overlay page. Every other
    // case down here keeps the full writebyte() rules. g_ts_fastmem == 1 means
    // no contention and no memory breakpoints; divMMC wins over Layer 2
    // (writebyte's order). Bomb Jack's menu blits its tiles this way with LDIR,
    // ~16 KB a frame at 28 MHz (hw log 2026-10-10).
    if (g_ts_fastmem == 1) {
        CPU::tstates += 3;   // no contention here; the line tick runs at the next M1
        if (!MemESP::divmmc_mapped && !g_ngs_zxdma && ((MemESP::wr_overlay_mask >> (address >> 13)) & 1)) {
            MemESP::wrOverlay[address >> 13][address & 0x1fff] = value;
            return;
        }
        // divMMC RAM/ROM window, ROM, AltROM write-redirect, NeoGS ZX-DMA: the
        // full rules, minus the Draw call (esxDOS / NextZXOS boot writes its
        // divMMC RAM ~thousands of times a frame while loading).
        gsDmaPoke8(address, value);
        return;
    }
    VIDEO::Draw(3, MemESP::ramContended[address >> 13]);
    if (__builtin_expect(g_ts_memcyc != 0, 0)) TsConf::cpuMemWrite(address);   // TS-Conf DRAM model
    gsDmaPoke8(address, value);
}

// Read word from RAM
static inline uint16_t tsPeek16Load(uint16_t address) {   // lsb first, like the generic path
    const uint16_t a1 = address + 1;
    const uint8_t lsb = MemESP::ramCurrent[address >> 13][address & 0x1FFF];
    const uint8_t msb = MemESP::ramCurrent[a1 >> 13][a1 & 0x1FFF];
    return (uint16_t)((msb << 8) | lsb);
}
static IRAM_ATTR __attribute__((noinline)) uint16_t peek16_generic(uint16_t address);
static IRAM_ATTR __attribute__((noinline)) uint16_t peek16_tick(uint16_t address) {
    VIDEO::tsDrawTick();
    return tsPeek16Load(address);
}
static IRAM_ATTR __attribute__((noinline)) uint16_t peek16_dram(uint16_t address) {   // at least one half misses
    CPU::tstates += 6 + TsConf::cpuMemRead(address, false);
    CPU::tstates += TsConf::cpuMemRead((uint16_t)(address + 1), false);
    if (CPU::tstates >= VIDEO::ts_line_t) VIDEO::tsDrawTick();
    return tsPeek16Load(address);
}
IRAM_ATTR uint16_t Z80Ops::peek16(uint16_t address) {
    TS_PAGE_HIT(address);
    C16_RD(address); C16_RD((uint16_t)(address + 1));
    const uint32_t gates = g_memgates.w;
    if (__builtin_expect(g_ts_fastrd > tsFastLo16(address) && MEMGATES_READ_PLAIN(gates), 1)) {
        CPU::tstates += 6;   // line tick at the next M1 (see peek8)
        return tsPeek16Load(address);
    }
    if (g_ts_fastrd > tsFastLo16(address)) {
        // An even address reads both halves out of ONE cache word (index a[8:1]),
        // so the second half can only miss when the first did; odd = two words.
        // Test first, add T-states after (the peek8 register note).
        if (__builtin_expect(g_ts_memcyc != 0, 0) && (!tsMemNoDram(address) || ((address & 1) && !tsMemNoDram((uint16_t)(address + 1))))) return peek16_dram(address);
        CPU::tstates += 6;
        if (__builtin_expect(CPU::tstates >= VIDEO::ts_line_t, 0)) return peek16_tick(address);
        return tsPeek16Load(address);
    }
    return peek16_generic(address);
}
static IRAM_ATTR __attribute__((noinline)) uint16_t peek16_generic(uint16_t address) {

    uint8_t page = address >> 13;

    if (page == ((address + 1) >> 13)) {    // Check if address is between two different pages

        if (MemESP::ramContended[page]) {
            VIDEO::Draw(3, true);
            VIDEO::Draw(3, true);
        } else
            VIDEO::Draw(6, false);
        if (__builtin_expect(g_ts_memcyc != 0, 0)) {          // TS-Conf DRAM model (TsConf.cpp)
            const uint32_t w = TsConf::cpuMemRead(address, false) + TsConf::cpuMemRead(address + 1, false);
            if (w) VIDEO::Draw(w, false);
        }
        // Order matters here for the same reason it does in the cross-page
        // branch below, and the old one-expression form got it backwards AND
        // left it to the compiler (the evaluation order of `|`'s operands is
        // unspecified). Harmless for real memory; fatal through the NeoGS
        // ZX-DMA window, where every read advances the card's pointer, so the
        // two halves of a POP come back swapped.
        //
        // hw 2026-08-07, TheLink: at an effect change the host points SP into
        // the DMA window and pulls its next phase's parameters with four POPs
        // (0x792C-0x7939, patching the operands at 794B/7963/7985/799D). With
        // the halves swapped every one of those addresses is wrong, the NMI
        // phase runs on garbage, and the ZX ends up waiting at 798B for a byte
        // its corrupted script asks for and the card is never told to send.
        uint8_t lsb = gsDmaPeek8(address);
        uint8_t msb = gsDmaPeek8(address + 1);
        return (uint16_t)((msb << 8) | lsb);

    } else {

        // Order matters, first read lsb, then read msb, don't "optimize"
        uint8_t lsb = Z80Ops::peek8(address);
        uint8_t msb = Z80Ops::peek8(address + 1);
        return (msb << 8) | lsb;

    }

}

// Write word to RAM
static inline void tsPoke16Store(uint16_t address, RegisterPair word) {
    tsPoke8Store(address, word.byte8.lo);
    tsPoke8Store((uint16_t)(address + 1), word.byte8.hi);
}
static IRAM_ATTR __attribute__((noinline)) void poke16_generic(uint16_t address, RegisterPair word);
static IRAM_ATTR __attribute__((noinline)) void poke16_tick(uint16_t address, RegisterPair word) {
    VIDEO::tsDrawTick();
    tsPoke16Store(address, word);
}
// DRAM model cases of the fast path: a running DMA (each byte write steals a
// cycle) and/or a write that hits the cache (invalidate). Two byte cycles on the
// hardware, so two cpuMemWrite calls even when both halves share a cache word.
static IRAM_ATTR __attribute__((noinline)) void poke16_cold(uint16_t address, RegisterPair word) {
    if (g_tsconf_wr) return poke16_generic(address, word);   // the write gate wants the generic path
    CPU::tstates += 6;
    TsConf::cpuMemWrite(address);
    TsConf::cpuMemWrite((uint16_t)(address + 1));
    if (CPU::tstates >= VIDEO::ts_line_t) VIDEO::tsDrawTick();
    tsPoke16Store(address, word);
}
IRAM_ATTR void Z80Ops::poke16(uint16_t address, RegisterPair word) {
    TS_PAGE_HIT(address);
    C16_WR(address); C16_WR((uint16_t)(address + 1));
    const uint32_t gates = g_memgates.w;
    if (__builtin_expect(g_ts_fastmem > tsFastLo16(address) && MEMGATES_WRITE_PLAIN(gates), 1)) {
        CPU::tstates += 6;   // line tick at the next M1 (see peek8)
        tsPoke16Store(address, word);
        return;
    }
    if (g_ts_fastmem > tsFastLo16(address)) {
        if (__builtin_expect(g_ts_memcyc != 0, 0)) {
            // a running DMA or a cache hit on either half -> poke16_cold (even: one cache word)
            if ((g_ts_memcyc & 2) || tsMemWriteHit(address) || ((address & 1) && tsMemWriteHit((uint16_t)(address + 1)))) return poke16_cold(address, word);
        }
        if (__builtin_expect((g_tsconf_wr | g_atm_ro) != 0, 0)) return poke16_generic(address, word);   // the write gates too
        CPU::tstates += 6;
        if (__builtin_expect(CPU::tstates >= VIDEO::ts_line_t, 0)) return poke16_tick(address, word);
        tsPoke16Store(address, word);
        return;
    }
    return poke16_generic(address, word);
}
static IRAM_ATTR __attribute__((noinline)) void poke16_generic(uint16_t address, RegisterPair word) {
    uint8_t page = address >> 13;
    uint16_t page_addr = address & 0x1fff;

    if (page_addr < 0x1fff) {    // Check if address is between two different pages
        if (g_ts_fastmem == 1) {
            // Spectrum Next below 0x4000 (see poke8_generic): no contention, no Draw
            CPU::tstates += 6;
        } else if (MemESP::ramContended[page]) {
            VIDEO::Draw(3, true);
            VIDEO::Draw(3, true);
        } else
            VIDEO::Draw(6, false);
        if (__builtin_expect(g_ts_memcyc != 0, 0)) {          // TS-Conf DRAM model (TsConf.cpp)
            TsConf::cpuMemWrite(address);
            TsConf::cpuMemWrite(address + 1);
        }
        gsDmaPoke8(address, word.byte8.lo);
        gsDmaPoke8(address + 1, word.byte8.hi);

    } else {
        // Order matters, first write lsb, then write msb, don't "optimize"
        Z80Ops::poke8(address, word.byte8.lo);
        Z80Ops::poke8(address + 1, word.byte8.hi);
    }
}

// // Write word to RAM
// IRAM_ATTR void Z80Ops::poke16(uint16_t address, RegisterPair word) {

//     uint8_t page = address >> 14;
//     uint16_t vid_line = address & 0x3fff;

//     if (vid_line < 0x3fff) {    // Check if address is between two different pages

//         if (page == 0) {
//             VIDEO::Draw(6, false);
//             return;
//         }

//         #ifndef DIRTY_LINES

//         if (MemESP::ramContended[page]) {
//             VIDEO::Draw(3, true);
//             VIDEO::Draw(3, true);
//         } else
//             VIDEO::Draw(6, false);

//         MemESP::ramCurrent[page][vid_line] = word.byte8.lo;
//         MemESP::ramCurrent[page][vid_line + 1] = word.byte8.hi;

//         #else

//         if (page == 2) {
//             VIDEO::Draw(6, false);
//             MemESP::ramCurrent[2][vid_line] = word.byte8.lo;
//             MemESP::ramCurrent[2][vid_line + 1] = word.byte8.hi;
//             return;
//         }

//         if (MemESP::ramContended[page]) {
//             VIDEO::Draw(3, true);
//             VIDEO::Draw(3, true);
//         } else
//             VIDEO::Draw(6, false);

//         if (page == 3) {
//             if (MemESP::videoLatch) {
//                 if (MemESP::bankLatch != 7) {
//                     MemESP::ramCurrent[3][vid_line] = word.byte8.lo;
//                     MemESP::ramCurrent[3][vid_line + 1] = word.byte8.hi;
//                     return;
//                 }
//             } else if (MemESP::bankLatch != 5) {
//                 MemESP::ramCurrent[3][vid_line] = word.byte8.lo;
//                 MemESP::ramCurrent[3][vid_line + 1] = word.byte8.hi;
//                 return;
//             }
//         } else if (MemESP::videoLatch) {
//             // Page == 1 == videoLatch
//             MemESP::ramCurrent[1][vid_line] = word.byte8.lo;
//             MemESP::ramCurrent[1][vid_line + 1] = word.byte8.hi;
//             return;
//         }

//         MemESP::ramCurrent[page][vid_line] = word.byte8.lo;
//         MemESP::ramCurrent[page][vid_line + 1] = word.byte8.hi;

//         if (vid_line < 6144) {

//             uint8_t result =  (vid_line >> 5) & 0b11000000;
//             result |=  (vid_line >> 2) & 0b00111000;
//             result |=  (vid_line >> 8) & 0b00000111;

//             VIDEO::dirty_lines[result] |= 0x01;

//         } else if (vid_line < 6912) {

//             uint8_t result = ((vid_line - 6144) >> 5) << 3;
//             // for (int i=result; i < result + 8; i++)
//             //     VIDEO::dirty_lines[i] = (word.byte8.lo & 0x80) | 0x01;
//             memset((uint8_t *)VIDEO::dirty_lines + result, (word.byte8.lo & 0x80) | 0x01, 8);

//         } else return;

//         vid_line++;

//         if (vid_line < 6144) {

//             uint8_t result =  (vid_line >> 5) & 0b11000000;
//             result |=  (vid_line >> 2) & 0b00111000;
//             result |=  (vid_line >> 8) & 0b00000111;

//             VIDEO::dirty_lines[result] |= 0x01;

//         } else if (vid_line < 6912) {

//             uint8_t result = ((vid_line - 6144) >> 5) << 3;
//             // for (int i=result; i < result + 8; i++)
//             //     VIDEO::dirty_lines[i] = (word.byte8.hi & 0x80) | 0x01;
//             memset((uint8_t *)VIDEO::dirty_lines + result, (word.byte8.hi & 0x80) | 0x01, 8);

//         }

//         #endif

//     } else {

//         // Order matters, first write lsb, then write msb, don't "optimize"
//         Z80Ops::poke8(address, word.byte8.lo);
//         Z80Ops::poke8(address + 1, word.byte8.hi);

//     }

// }


/* Put an address on bus lasting 'tstates' cycles */
static IRAM_ATTR __attribute__((noinline)) void addressOnBus_generic(uint16_t address, int32_t wstates);
// The fast path is a leaf (the generic path is a tail call): the generic loop
// made the whole function push three registers per call.
IRAM_ATTR void Z80Ops::addressOnBus(uint16_t address, int32_t wstates) {
    if (g_ts_fastmem) {   // TsFastMem.h: no contention on the fast path; line tick at the next M1
        CPU::tstates += (uint32_t)wstates;
        return;
    }
    return addressOnBus_generic(address, wstates);
}
// IR on the address bus (INC/DEC rr, LD SP,HL, the M1 extension of many
// instructions): the fast path ignores the address, so the IR pair is only
// built on the generic path — it was one out-of-line call per such instruction.
IRAM_ATTR void Z80Ops::addressOnBusIR(int32_t wstates) {
    if (g_ts_fastmem) {
        CPU::tstates += (uint32_t)wstates;
        return;
    }
    return addressOnBus_generic((uint16_t)((Z80::getRegI() << 8) | Z80::getRegR()), wstates);
}
static IRAM_ATTR __attribute__((noinline)) void addressOnBus_generic(uint16_t address, int32_t wstates) {
    if (MemESP::ramContended[address >> 13]) {
        for (int idx = 0; idx < wstates; idx++)
            VIDEO::Draw(1, true);
    } else
        VIDEO::Draw(wstates, false);
}

/* Callback to know when the INT signal is active */
IRAM_ATTR bool Z80Ops::isActiveINT(void) {
    // RZX playback: the line is raised by the FETCH COUNT (Rzx::raiseInt at the
    // end of each recorded frame), not by the raster — see CPU::loopRzx.
    if (__builtin_expect(Rzx::mode != 0, 0)) return (int32_t)CPU::tstates < Rzx::intUntil;
    // Karabas serial-mouse hardware INT (RST20H): level-asserted while an RX
    // byte waits with INT_EN set in CP/M mode. Sampled only in the checked
    // execute() loops (like the frame INT), so worst-case latency is one
    // exec_nocheck stretch — fine for a level line the device keeps high.
    if (Ports::serialMouseIntAsserted()) return true;
    // TS-Conf: three latched sources behind INTMask (FRAME window, LINE, DMA
    // end) — the controller owns the level; see TsConf::intLine.
    if (Z80Ops::isTsconf) return TsConf::intLine();
    // ATM-Turbo 2+: the frame INT is gated by #xx77 D5 (Unreal cpu.int_gate).
    // ZX-Evo BaseConf: no gate, but the acknowledge ends this frame's pulse.
    if (__builtin_expect(Z80Ops::isAtm, 0) &&
        (!Atm::intEnabled() ||
         (Atm::evo && EvoBase::intAckFrame ==
              EvoBase::intWindowId((uint32_t)((int32_t)CPU::tstates + CPU::latetiming),
                                   CPU::statesInFrame, CPU::global_tstates)))) return false;
    // Timex DEC (#FF) bit 6 — "17ms Interrupt Inhibit" (MAME port_ff_w). The SCLD
    // gates the line itself, so the window still opens and closes on time; the CPU
    // simply never sees it. Cleared on reset with the rest of the DEC register.
    if (__builtin_expect(VIDEO::timex_int_inhibit, 0)) return false;
    // SAM Coupe: frame window OR the line window (STATUS bits 3 / 0); flash.
    if (__builtin_expect(Z80Ops::isSam, 0)) return Sam::intLine();
    // Adding latetiming shifts the check 1T later for Late mode.
    // At end of frame (tstates=statesInFrame-1), tmp wraps to 0, firing
    // the Late INT via straddle — this is the correct hardware behaviour.
    int32_t tmp = (int32_t)CPU::tstates + CPU::latetiming;
    if (tmp >= (int32_t)CPU::statesInFrame) tmp -= CPU::statesInFrame;
    if (Z80Ops::isNext) {
        // Line interrupt, hardware IM2, CTC, NR 0x20 pulses: NextInt (flash).
        if (NextInt::ex) return NextInt::intLine();
        // Frame (ULA) interrupt, unless masked by port 0xFF bit 6 (= NR 0x22
        // bit 2 = NOT NR 0xC4 bit 0, one latch: zxnext.vhd port_ff_interrupt_disable)
        return !NextReg::portFFIntDisable && (tmp >= CPU::IntStart) && (tmp < CPU::IntEnd);
    }
    return (tmp >= CPU::IntStart) && (tmp < CPU::IntEnd);
}

