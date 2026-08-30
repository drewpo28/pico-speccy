/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo
[dcrespo3d] https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectruma

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
To Contact the dev team you can write to zxespectrum@gmail.com or
visit https://zxespectrum.speccy.org/contacto

*/

#include "Ports.h"
#include "AySound.h"
#include "OpnFm.h"
#include "SAASound.h"
#include "CPU.h"
#include "Config.h"
#include "ESPectrum.h"
#include "LEDIndicators.h"
#include "MemESP.h"
#include "AlfCart.h"
#include "Tape.h"
#include "Video.h"
#include "Z80_JLS/z80.h"
#include "pwm_audio.h"
#include "roms.h"
#include "wd1793.h"
#include "Debug.h"

#include "OSDMain.h"

#include "../drivers/graphics/graphics.h"
extern "C" const uint32_t profi_default_palette16[16];

#include "Midi.h"
#include "Z80DMA.h"
#include "GS/GS.h"
#include "DivMMC.h"
#include "IDE.h"
#include "ZiFi.h"
#include "RTC.h"
#include "MB02.h"
#include "hardware/gpio.h"
#include "sdcard.h"

// Set to 1 to trace every 0x7FFD / 0xDFFD paging-port write (Profi debugging).
// Off by default — these fire thousands of times during DS80/CP/M init.
#ifndef PROFI_PORT_TRACE
#define PROFI_PORT_TRACE 0
#endif

#ifndef MC7FFD_TRACE
#define MC7FFD_TRACE 0
#endif
#ifndef TSFM_TRACE
#define TSFM_TRACE 0
#endif
#if TSFM_TRACE
// TSFM budget probe (TheLink tunnel stutter, 2026-08-14). At 3.5 MHz the
// tunnel frame leaves ~11.4k T after delay+copy, all of it for the music
// call — and the log showed ~25% of INTs landing while the ZX still waits in
// the post-music handshake. This measures WHERE the music's guest-T goes:
//   stRd      — YM2203 status reads (IN #FFFD in ready-poll mode) per window
//   wr        — #FFFD/#BFFD writes per window
//   lastT     — latest FM-port access tstate seen in any frame of the window
//               (how deep into the frame the player runs; frame = 71680 at 3.5)
//   late      — frames whose last FM access came past tstates 68000
//   maxStRd/fr— longest status-poll burst in one frame (timer-flag spinning?)
// Frames are detected by tstates wrap between FM accesses, so only frames
// with FM traffic are counted — rates are per counted frame.
static uint32_t tsfm_st_rd = 0, tsfm_wr = 0;
static uint16_t tsfm_frames = 0, tsfm_late = 0;
static uint32_t tsfm_max_t = 0, tsfm_frame_last = 0, tsfm_prev_ts = 0;
static uint16_t tsfm_frame_strd = 0, tsfm_max_strd = 0;
static void tsfmProbe(bool status_read) {
  uint32_t ts = CPU::tstates;
  if (ts < tsfm_prev_ts) {   // frame boundary passed since the last FM access
    tsfm_frames++;
    if (tsfm_frame_last > 68000) tsfm_late++;
    if (tsfm_frame_strd > tsfm_max_strd) tsfm_max_strd = tsfm_frame_strd;
    if (tsfm_frame_last > tsfm_max_t) tsfm_max_t = tsfm_frame_last;
    tsfm_frame_strd = 0; tsfm_frame_last = 0;
    if (tsfm_frames >= 100) {
      Debug::log("TSFM: fr=%u stRd=%lu wr=%lu lastT=%lu late(>68k)=%u maxStRd/fr=%u",
                 tsfm_frames, (unsigned long)tsfm_st_rd, (unsigned long)tsfm_wr,
                 (unsigned long)tsfm_max_t, tsfm_late, tsfm_max_strd);
      tsfm_frames = 0; tsfm_late = 0; tsfm_st_rd = 0; tsfm_wr = 0;
      tsfm_max_t = 0; tsfm_max_strd = 0;
    }
  }
  tsfm_prev_ts = ts;
  if (status_read) { tsfm_st_rd++; tsfm_frame_strd++; } else tsfm_wr++;
  tsfm_frame_last = ts;
}
#endif
#if MC7FFD_TRACE
// One-shot capture of #7FFD write times, for beam-raced multicolor diagnosis
// (TheLink tunnel, 2026-08-14: 1-scanline attr stripes in column 0 of rows
// 19-23). The tunnel's ZX side is phase-locked to the beam: HALT on INT, a
// 17,285 T delay loop, then 24 iterations of exactly 1792 T (one attr row of
// beam time), each flipping the displayed screen 4x via OUT (C),A to #7FFD
// with every flip designed to land in a horizontal border — the second flip
// of each iteration falls on the 17920 + 1792*i grid exactly (= start of
// machine line 80+8i on the Pentagon it was tuned for). This trace measures
// where OUR core puts those flips: the offset of flip #2 from the 17920 grid
// is the phase error vs TS_SCREEN_PENTAGON, and the spacing between flips is
// the actual per-block cost in this core (design: 437/429/452/452+22).
//
// Arms itself on the multicolor signature — >= 90 paging writes in one frame
// (the tunnel does 96: 4 per attr row) — then records the next ~200 writes
// with their frame-relative tstate and dumps once per boot. A frame boundary
// shows up in the dump as tstates decreasing.
static uint32_t mc_tr_t[200];
static uint8_t  mc_tr_d[200];
static uint16_t mc_tr_n = 0;
static uint16_t mc_frame_writes = 0;
static uint32_t mc_prev_ts = 0;
static uint8_t  mc_state = 0;      // 0=watching 1=recording 2=cooldown
static uint16_t mc_cool = 0;       // frames left in cooldown
static uint16_t mc_dump_no = 0;
static void mc7ffdTrace(uint8_t data) {
  uint32_t ts = CPU::tstates;
  bool new_frame = ts < mc_prev_ts;
  mc_prev_ts = ts;
  if (mc_state == 2) {             // cooldown between captures (~5 s), then re-arm
    if (new_frame && --mc_cool == 0) { mc_state = 0; mc_frame_writes = 0; }
    return;
  }
  if (mc_state == 0) {
    if (new_frame) {
      if (mc_frame_writes >= 90) {
        mc_state = 1;
        mc_tr_n = 0;
        Debug::log("MC7FFD: armed #%u (%u writes/frame)", mc_dump_no, mc_frame_writes);
      }
      mc_frame_writes = 0;
    }
    mc_frame_writes++;
    if (mc_state == 0) return;
  }
  mc_tr_t[mc_tr_n] = ts;
  mc_tr_d[mc_tr_n] = data;
  if (++mc_tr_n < 200) return;
  mc_state = 2;
  mc_cool = 250;
  Debug::log("MC7FFD: dump #%u tsScreen=%d tsLine=%d frame=%u IntEnd=%ld",
             mc_dump_no++, VIDEO::tStatesScreen, (int)VIDEO::tStatesPerLine,
             (unsigned)CPU::statesInFrame, (long)CPU::IntEnd);
  for (int i = 0; i < 200; i += 8) {
    Debug::log("MC7FFD: %lu/%02X %lu/%02X %lu/%02X %lu/%02X %lu/%02X %lu/%02X %lu/%02X %lu/%02X",
               (unsigned long)mc_tr_t[i],   mc_tr_d[i],
               (unsigned long)mc_tr_t[i+1], mc_tr_d[i+1],
               (unsigned long)mc_tr_t[i+2], mc_tr_d[i+2],
               (unsigned long)mc_tr_t[i+3], mc_tr_d[i+3],
               (unsigned long)mc_tr_t[i+4], mc_tr_d[i+4],
               (unsigned long)mc_tr_t[i+5], mc_tr_d[i+5],
               (unsigned long)mc_tr_t[i+6], mc_tr_d[i+6],
               (unsigned long)mc_tr_t[i+7], mc_tr_d[i+7]);
  }
}
#endif

// Helper so MemESP.h writebyte() can read the Z80 PC without pulling
// in Z80_JLS/z80.h (which would create circular include chains via MemESP.h).
// Unconditional (not just under PROFI_PORT_TRACE) — the #0100 write trace
// below also needs it, independently of the DS80 display-write trace.
uint16_t _ds80_dbg_get_pc(void) { return Z80::getRegPC(); }

// TurboSound chip decode for an AY port access: the NedoPC latch (writing #FF /
// #FE to #FFFD) is the ONLY chip select. An earlier build additionally routed any
// A8=0 access to chip 1 ("old TS" #FEFD/#BEFD address scheme) — that broke real
// single-AY software, which relies on the Pentagon's partial decode (A15=1, A1=0,
// A8 is DON'T CARE): players hitting the AY through A8=0 aliases had their
// select/data stream split across the two chips (per-chip register latches went
// out of step → silence or garbage). Symptom on hw (2026-07-27): demos on ONE
// TRD played or stayed mute depending on which port alias their player used.
// Returns chip0 when the latched chip does not exist (chip1 is heap-allocated by
// TurboSubsys and may lag a Config change, or have failed on OOM).
static inline AySound* ayChipFor(uint16_t /*address*/) {
  AySound* ch = chips[AySound::selected_chip];
  return ch ? ch : chips[0];
}

// One #FFFD/#BFFD access, both halves of the latched YM2203. The FM half keeps
// its own register-number latch (OpnFm::writeAddr) instead of reading AySound's:
// ayChipFor() falls back to chip0 when chip1 does not exist, which is right for
// the AY side but would put every chip-1 FM write on chip 0. opnfm[] is null
// unless TsfmSubsys is up, so this costs one null test while TSFM is off.
// `genSample` is false only on the DMA path, which has never caught the mixer up.
static inline void ayPortWrite(uint16_t address, uint8_t data, bool genSample) {
#if TSFM_TRACE
  tsfmProbe(false);
#endif
  AySound* chip = ayChipFor(address);
  OpnFm*   fm   = opnfm[AySound::selected_chip];
  if ((address & 0x4000) != 0) {
    if (chip) chip->selectRegister(data);
    if (fm)   fm->writeAddr(data);
  } else {
    if (genSample && Tape::tapeStatus != TAPE_LOADING) ESPectrum::AYGetSample();
    if (chip) chip->setRegisterData(data);
    if (fm)   fm->writeData(data);
  }
}

#if PROFI_PORT_TRACE
// Pointers to the CURRENT display pages (updated whenever profi_clrmem/grmem change).
// writebyte() compares ramCurrent[slot] against these to detect writes to display pages.
uint8_t* ds80_dbg_clrmem = nullptr;  // display color-attribute page (56 or 58)
uint8_t* ds80_dbg_grmem  = nullptr;  // display pixel page (4 or 6)
int      ds80_dbg_wr_cnt = 0;        // reset each frame so we always capture first write
#endif

#if FDD_PORT_TRACE
// Watchdog for the DS80/CP/M paging hang investigated 2026-07-08/09: DFFD/7FFD
// writes cycling forever among a small handful of PCs, with no manual dump
// timing possible (the freeze point isn't predictable enough to catch by hand).
// Call from every DFFD/7FFD write. Tracks the last few DISTINCT write-site PCs;
// once we've gone a long stretch without seeing a genuinely NEW one, we're
// stuck cycling — dump full registers once (not spamming) so the next repro
// self-documents without a manual debug-dump.
static void checkPagingStuck(uint16_t pc) {
  static uint16_t recentPC[8] = {0};
  static uint8_t recentCount = 0;
  static uint32_t stuckRun = 0;
  static bool alreadyLogged = false;
  for (uint8_t i = 0; i < recentCount; i++) {
    if (recentPC[i] == pc) {
      stuckRun++;
      if (stuckRun == 4000 && !alreadyLogged) {
        alreadyLogged = true;
        Debug::log("[STUCK-PAGING] %lu paging writes cycling among {%04X %04X %04X %04X %04X %04X %04X %04X} romInUse=%d",
                   (unsigned long)stuckRun, recentPC[0], recentPC[1], recentPC[2], recentPC[3],
                   recentPC[4], recentPC[5], recentPC[6], recentPC[7], MemESP::romInUse);
        Debug::log("[STUCK-PAGING] AF=%04X BC=%04X DE=%04X HL=%04X AF'=%04X BC'=%04X DE'=%04X HL'=%04X",
                   Z80::getRegAF(), Z80::getRegBC(), Z80::getRegDE(), Z80::getRegHL(),
                   Z80::getRegAFx(), Z80::getRegBCx(), Z80::getRegDEx(), Z80::getRegHLx());
        Debug::log("[STUCK-PAGING] IX=%04X IY=%04X SP=%04X PC=%04X",
                   Z80::getRegIX(), Z80::getRegIY(), Z80::getRegSP(), Z80::getRegPC());
      }
      return;
    }
  }
  // Genuinely new PC — the cycle just grew (or broke); reset the streak.
  if (recentCount < 8) {
    recentPC[recentCount++] = pc;
  } else {
    for (uint8_t i = 0; i < 7; i++) recentPC[i] = recentPC[i + 1];
    recentPC[7] = pc;
  }
  stuckRun = 0;
  alreadyLogged = false;
}
#endif

// Per-frame port-call counters — read and reset in VIDEO::EndFrame diagnostic.
uint32_t Ports::port7ffd_cnt  = 0;
uint32_t Ports::portdffd_cnt  = 0;
volatile uint32_t Ports::fdd_ports_us = 0;
volatile uint32_t Ports::fdd_ports_calls = 0;  // stepping calls (µs/call = us/calls)
volatile uint32_t Ports::fdd_ports_max = 0;    // longest single stepping call, µs

// IDE_PORT_TRACE (PROFI IDE/HDD port tracing) is defined by CMake (default 0).
// Undefined → 0 in #if, so no fallback #define is needed here.

// Place hot port functions in SRAM instead of XIP flash
#undef IRAM_ATTR
#define IRAM_ATTR __not_in_flash("ports")

#pragma GCC optimize("O3")

// Values calculated for BEEPER, EAR, MIC bit mask (values 0-7)
// Taken from FPGA values suggested by Rampa
//   0: ula <= 8'h00;
//   1: ula <= 8'h24;
//   2: ula <= 8'h40;
//   3: ula <= 8'h64;
//   4: ula <= 8'hB8;
//   5: ula <= 8'hC0;
//   6: ula <= 8'hF8;
//   7: ula <= 8'hFF;
// and adjusted for BEEPER_MAX_VOLUME = 97
uint8_t Ports::speaker_values[8] = {0, 19, 34, 53, 97, 101, 130, 134};
uint8_t Ports::port[128];
uint8_t Ports::extPort[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t Ports::port254 = 0;
uint8_t Ports::sndriveLatch[6] = {0, 0, 0, 0, 0, 0};
uint8_t Ports::sndriveUsed = 0;
uint8_t Ports::portAFF7 = 0;
uint8_t Ports::portDFFD = 0;
uint8_t Ports::portEFF7 = 0;
uint8_t Ports::port1FFD = 0;
uint8_t Ports::gmxPort00 = 0;
uint8_t Ports::gmxPort78FD = 0;
uint8_t Ports::gmxPort7EFD = 0;
uint8_t Ports::gmxScrollLo = 0;
uint8_t Ports::gmxScrollHi = 0;
uint8_t Ports::gmxPlane = 0;
uint8_t Ports::gmxMagicShift = 0;
uint8_t Ports::portDFFDgmx = 0;
uint8_t Ports::port008B = 0;
uint8_t Ports::port018B = 0;
uint8_t Ports::port028B = 0;
uint8_t Ports::serialMouseCtl = 0;
uint8_t Ports::serialMouseIntEn = 0;

// Serial-mouse packet pipeline (serial_mouse.vhd st_prepare/st_byteN machine,
// simplified: the 8-tact RxRDY gaps between bytes are dropped — drivers poll
// the status register or take level INTs, and both re-sample RxRDY anyway).
static uint8_t sm_pkt[3] = {0, 0, 0};
static uint8_t sm_pkt_pos = 3;   // 3 = idle, no packet in flight
static bool    sm_rxrdy = false;
static uint8_t sm_last_btns = 0;

static void smTryBuildPacket() {
  if (sm_pkt_pos < 3 || !(Ports::serialMouseCtl & 0x04)) return; // busy / RxE off
  uint8_t btns = (ESPectrum::mouseButtonL ? 0x20 : 0) |
                 (ESPectrum::mouseButtonR ? 0x10 : 0);
  // Sensitivity: modern USB mice report far more counts than the ~200 DPI a
  // serial mouse era expects — scale by 2 (÷4 felt sluggish on hw), at
  // packet-build time with the remainder kept in the accumulator so slow
  // movements still add up instead of being truncated away.
  int dx = ESPectrum::mouseDX / 2, dy = ESPectrum::mouseDY / 2;
  if (dx > 127) dx = 127; else if (dx < -128) dx = -128;
  if (dy > 127) dy = 127; else if (dy < -128) dy = -128;
  if (!dx && !dy && btns == sm_last_btns) return; // st_prepare: nothing new
  ESPectrum::mouseDX -= dx * 2;
  ESPectrum::mouseDY -= dy * 2;
  sm_last_btns = btns;
  // Microsoft Mouse 3-byte packet: 01LRyyxx, 00xxxxxx, 00yyyyyy
  sm_pkt[0] = (uint8_t)(0x40 | btns | (((uint8_t)dy >> 4) & 0x0C) | (((uint8_t)dx >> 6) & 0x03));
  sm_pkt[1] = (uint8_t)dx & 0x3F;
  sm_pkt[2] = (uint8_t)dy & 0x3F;
  sm_pkt_pos = 0;
  sm_rxrdy = true;
}

void Ports::serialMouseTick() {
  smTryBuildPacket(); // no-op unless RxE armed and deltas/buttons are pending
}

void Ports::serialMouseReset() {
  serialMouseCtl = 0;
  serialMouseIntEn = 0;
  sm_pkt_pos = 3;
  sm_rxrdy = false;
  sm_last_btns = 0;
  ESPectrum::mouseDX = ESPectrum::mouseDY = 0;
}

bool Ports::serialMouseIntAsserted() {
  // hw_int.vhd: INT while (RxRDY && RxE) && CPM && INT_EN. sm_rxrdy is only
  // ever set with RxE on, so the RxE term is already folded in.
  return sm_rxrdy && serialMouseIntEn && Z80Ops::isProfi && (portDFFD & 0x20);
}

// PQ-DOS serial keyboard scancode queue (drained by the #D3 read handler).
volatile uint8_t Ports::pqkBuf[16] = {0};
volatile uint8_t Ports::pqkHead = 0;
volatile uint8_t Ports::pqkTail = 0;
void Ports::pushKey(uint8_t scan) {
  uint8_t nh = (pqkHead + 1) & 0x0F;
  if (nh == pqkTail) return; // full → drop (menu keys are slow, never fills)
  pqkBuf[pqkHead] = scan;
  pqkHead = nh;
}
Ports::PIT8253Channel Ports::pitChannels[3] = {};

uint8_t (*Ports::getFloatBusData)() = &Ports::getFloatBusData48;

#if SND_PORT_TRACE
uint32_t Ports::sndTraceWr[256];
uint32_t Ports::sndTraceRd[256];
uint8_t  Ports::sndTraceLastVal[256];
// Not IRAM: called once per ~5 s from the main loop. Prints every port (by low
// address byte) touched since the previous dump, then clears the histograms.
void Ports::sndTraceDump() {
    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "SNDTRC bank=%d rom=%d DFFD=%02X W:",
                       (int)MemESP::bankLatch, (int)MemESP::romLatch, portDFFD);
    for (int p = 0; p < 256; p++) {
        if (!sndTraceWr[p]) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X=%lu(%02X)",
                        p, (unsigned long)sndTraceWr[p], sndTraceLastVal[p]);
        sndTraceWr[p] = 0;
        if (pos > (int)sizeof(buf) - 16) break;
    }
    Debug::log("%s\n", buf);
    pos = snprintf(buf, sizeof(buf), "SNDTRC R:");
    for (int p = 0; p < 256; p++) {
        if (!sndTraceRd[p]) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X=%lu",
                        p, (unsigned long)sndTraceRd[p]);
        sndTraceRd[p] = 0;
        if (pos > (int)sizeof(buf) - 16) break;
    }
    Debug::log("%s\n", buf);
}
#endif

IRAM_ATTR uint8_t Ports::getFloatBusData48() {

  unsigned int currentTstates = CPU::tstates;

  unsigned int line = (currentTstates / 224) - 64;
  if (line >= 192) {
#if HALT2INT_TRACE
    Debug::log("[FLOAT] ts=%u line=%d(off) -> 0xFF (lt=%u IntEnd=%d)",
               currentTstates, (int)line, (unsigned)CPU::latetiming, (int)CPU::IntEnd);
#endif
    return 0xFF;
  }

  unsigned char halfpix = (currentTstates % 224) - 3;
  if ((halfpix >= 125) || (halfpix & 0x04)) {
#if HALT2INT_TRACE
    Debug::log("[FLOAT] ts=%u line=%u halfpix=%u -> 0xFF (lt=%u)",
               currentTstates, line, (unsigned)halfpix, (unsigned)CPU::latetiming);
#endif
    return 0xFF;
  }

  int hpoffset = (halfpix >> 2) + ((halfpix >> 1) & 0x01);

  uint8_t fbdata = (halfpix & 0x01)
                       ? VIDEO::grmem[VIDEO::offAtt[line] + hpoffset]
                       : VIDEO::grmem[VIDEO::offBmp[line] + hpoffset];
#if HALT2INT_TRACE
  Debug::log("[FLOAT] ts=%u line=%u halfpix=%u hpoff=%d %s byte=%02X (lt=%u)",
             currentTstates, line, (unsigned)halfpix, hpoffset,
             (halfpix & 0x01) ? "ATT" : "BMP", fbdata, (unsigned)CPU::latetiming);
#endif
  return fbdata;
}

IRAM_ATTR uint8_t Ports::getFloatBusData128() {

  unsigned int currentTstates = CPU::tstates - 1;

  unsigned int line = (currentTstates / 228) - 63;
  if (line >= 192)
    return 0xFF;

  unsigned char halfpix = currentTstates % 228;
  if ((halfpix >= 128) || (halfpix & 0x04))
    return 0xFF;

  int hpoffset = (halfpix >> 2) + ((halfpix >> 1) & 0x01);
  ;

  if (halfpix & 0x01)
    return (VIDEO::grmem[VIDEO::offAtt[line] + hpoffset]);

  return (VIDEO::grmem[VIDEO::offBmp[line] + hpoffset]);
}

static uint32_t p_states;

IRAM_ATTR void Ports::FDDStep(bool force) {

  CPU::tstates_diff += p_states - CPU::prev_tstates;
  CPU::prev_tstates = p_states;

  // Fast exit: less than one WD step elapsed since the previous port access.
  // CP/M's SYS-status busy-wait polls run ~30-60 T per iteration
  // (< WD177XSTEPSTATES), and those callers pass force=true — but force only
  // means "step even without HLD/HLT"; with steps==0 rvmWD1793Step(0) is a
  // pure no-op (its whole body is the `for (;steps > 0;)` loop), so skipping
  // the call is semantics-identical for force too.  This removes ~2000 no-op
  // flash calls + time_us_64() pairs per frame during CP/M polling (measured
  // ports=7ms/frame → the dominant worst-frame cost after the strcmp fix).
  if (CPU::tstates_diff < WD177XSTEPSTATES)
    return;

  if (force ||
      ((ESPectrum::fdd.control & (kRVMWD177XHLD | kRVMWD177XHLT)) != 0)) {
    uint8_t pre_step_state = ESPectrum::fdd.stepState;
    uint32_t steps = CPU::tstates_diff / WD177XSTEPSTATES;
    uint64_t _t0 = time_us_64();
    rvmWD1793Step(&ESPectrum::fdd, steps); // FDD
    uint32_t _dt = (uint32_t)(time_us_64() - _t0);
    fdd_ports_us += _dt;
    fdd_ports_calls++;
    if (_dt > fdd_ports_max) fdd_ports_max = _dt;
    // One-shot trace of an anomalously slow single step call (rate-limited):
    // pins down WHAT is slow inside — state machine step vs something it calls.
    if (_dt > 300) {
      static uint64_t last_slow_log = 0;
      if (time_us_64() - last_slow_log > 1000000) {
        last_slow_log = time_us_64();
        Debug::log("[FDDSLOW] dt=%u steps=%u preSS=%u SS=%u st=%u cmd=%02X",
                   _dt, (unsigned)steps, pre_step_state,
                   ESPectrum::fdd.stepState, (unsigned)ESPectrum::fdd.state,
                   ESPectrum::fdd.command);
      }
    }
  }

  CPU::tstates_diff = CPU::tstates_diff % WD177XSTEPSTATES;
}

IRAM_ATTR static void FDDStep_MB02(bool force) {
  CPU::tstates_diff += p_states - CPU::prev_tstates;
  CPU::prev_tstates = p_states;
  if (CPU::tstates_diff < WD177XSTEPSTATES)   // same fast exit as FDDStep
    return;
  if (force ||
      ((ESPectrum::mb02_fdd.control & (kRVMWD177XHLD | kRVMWD177XHLT)) != 0))
    rvmWD1793Step(&ESPectrum::mb02_fdd, CPU::tstates_diff / WD177XSTEPSTATES);
  CPU::tstates_diff = CPU::tstates_diff % WD177XSTEPSTATES;
}

uint8_t nes_pad2_for_alf(void);
static uint8_t newAlfBit = 0;
// ALF cartridges are served lazily from SD on demand (AlfCart), like a wd1793 disk:
// no built-in cart, no flash region. There is no default cart — the cart "drive" is
// empty (open bus) until the user mounts a .rom/.bin from SD, exactly like TR-DOS
// with no disk inserted. The system ROM (gb_rom_Alf) runs when nothing is mounted.
// Bind/refresh the cart from Config (call at boot after Config::load and whenever a
// cartridge is loaded/unloaded). On a missing SD file: empty drive, never hang.
void alfBindCart() {
    if (!Config::alfCartPath.empty()) {
        if (!AlfCart::active() || AlfCart::path() != Config::alfCartPath) {
            if (!AlfCart::mount(Config::alfCartPath)) {
                Config::alfCartBanks = 0;   // SD file gone (card removed) → empty drive
                Config::alfCartPath  = "";
                return;
            }
        }
        Config::alfCartBanks = (uint8_t)AlfCart::bankCount();
    } else {
        AlfCart::unmount();
    }
}
static uint8_t profi_fdc_busy = 0;
// Profi CP/M: detect DSKKE9A "CALL 0x40EA → JR 0x40D9" re-issue loop.
// When drive has no disk, successive OUT(0x1F) commands are issued at CPU
// speed via the re-issue loop. After a few re-issues we force-exit: walk
// the Z80 stack to find the original return address (non-0x40DE frame) and
// redirect execution there via EI+RET, avoiding stack overflow and crash.
static int profi_nodisk_reissue_cnt = 0;
// Tracks whether the last Profi CP/M FDC command was issued via the shifted
// 0x83 port path (Dos5 5.30 driver) vs the standard 0x1F/0x3F path.
// Used to decide what IN A,(0x3F) returns: INTRQ/DRQ status (shifted scheme)
// vs track register (standard scheme). Set on CMD write via 0x83; cleared on
// CMD write via normal path (address & 0xE3 == 0x03).
static bool profi_shifted_fdc = false;

extern int ram_pages, butter_pages, psram_pages, swap_pages;

// Proxy for GS.cpp — that TU includes Z80_redcode.h which clashes with
// Z80_JLS/z80.h, so it can't query the host PC directly.
extern "C" uint16_t gs_host_z80_pc(void) { return Z80::getRegPC(); }
// Guest clock snapshot for the GS host-poll pacing (GS.cpp hostReadBB).
extern "C" void gs_host_clock(uint32_t* tstates, uint32_t* states_in_frame,
                              uint8_t* mult, uint8_t* max_speed) {
  *tstates = CPU::tstates;
  *states_in_frame = CPU::statesInFrame;
  *mult = ESPectrum::multiplicator;
  *max_speed = ESPectrum::maxSpeed ? 1 : 0;
}
// Return address of whatever called the #BB poll loop. The PC alone is useless
// there — all three waits are three-byte loops and the host sits in one of them
// permanently — but the word on top of its stack names the routine, exactly as
// the memory dump did when 8758 identified FGETVTS.
extern "C" uint16_t gs_host_z80_ret(void) {
    uint16_t sp = Z80::getRegSP();
    return (uint16_t)(MemESP::readbyte(sp) | (MemESP::readbyte(sp + 1) << 8));
}
inline static size_t extendedZxRamPages() {
  if (Z80Ops::is1024)
    return 64;
  if (Z80Ops::is512)
    return 32;
  if (Z80Ops::isScorpion)
    return 16;
  if (Z80Ops::is128 || (Z80Ops::isPentagon || Z80Ops::isProfi))
    return 8;
  return 4;
}

IRAM_ATTR uint8_t Ports::input(uint16_t address) {
  uint8_t data;
#if SND_PORT_TRACE
  sndTraceRd[address & 0xFF]++;
#endif
  if (Config::numPortReadBP > 0 && Config::hasBreakPoint(address, Config::BP_PORT_READ))
    CPU::portBasedBP = true;
  uint8_t rambank = address >> 14;
  p_states = CPU::tstates;

#if FDD_PORT_TRACE
  // Unconditional probe: fires for ANY IN on a Profi FDC-relevant low byte,
  // regardless of whether the CPM/ROM14 gates below actually claim it — shows
  // whether the Z80 program even reaches these addresses, and with what
  // cpm/rom14/trdos/romInUse/disk state, when the normal FDD_PORT_TRACE
  // logging (inside wd1793.cpp, reached only once a gate already passed)
  // stays silent.
  if (Z80Ops::isProfi) {
    uint8_t lo8f = address & 0xFF;
    if (lo8f == 0x1F || lo8f == 0x3F || lo8f == 0x5F || lo8f == 0x7F ||
        lo8f == 0x83 || lo8f == 0xA3 || lo8f == 0xC3 || lo8f == 0xE3 ||
        lo8f == 0xFF || lo8f == 0xBF) {
      bool has_any_disk_p = ESPectrum::fdd.disk[ESPectrum::fdd.diskS] != nullptr;
      bool skip_p = (MemESP::romInUse == 0 && !has_any_disk_p);
      uint16_t pcNow = Z80::getRegPC();
      bool cpmNow = (portDFFD & 0x20) != 0;
      // Tight loops here come in two shapes: (a) the SAME instruction spinning
      // thousands of times waiting for a status bit, or (b) a two-instruction
      // status/data PAIR alternating every call (e.g. pc=BC84 polls SYS, pc=BC8D
      // reads DATA, back and forth for the whole sector). Either way the high
      // byte of addr (the accumulator, used as data/delay counter) keeps
      // rotating, so dedupe on (pc, low byte, decode state) — never on addr
      // itself — and track up to 2 alternating "sites" so shape (b) collapses
      // too, not just shape (a).
      struct Site { uint16_t pc=0xFFFF, loAddr=0, hiAddr=0; uint8_t lo=0xFF, cpm=0xFF, rom14=0xFF, trdos=0xFF, romInUse=0xFF; bool disk=false, skip=false; uint32_t rep=0; bool used=false; };
      static Site slot[2];
      auto matches = [&](const Site &s) {
        return s.used && pcNow == s.pc && lo8f == s.lo && cpmNow == s.cpm &&
               MemESP::romLatch == s.rom14 && ESPectrum::trdos == s.trdos &&
               MemESP::romInUse == s.romInUse && has_any_disk_p == s.disk && skip_p == s.skip;
      };
      auto flush = [&](Site &s) {
        if (s.rep)
          Debug::log("[FDC IN probe] pc=%04X lo=%02X addr %04X..%04X (repeated x%u)",
                     s.pc, s.lo, s.loAddr, s.hiAddr, s.rep);
        s = Site();
      };
      auto fill = [&](Site &s) {
        s.used = true; s.pc = pcNow; s.lo = lo8f; s.cpm = cpmNow; s.rom14 = MemESP::romLatch;
        s.trdos = ESPectrum::trdos; s.romInUse = MemESP::romInUse; s.disk = has_any_disk_p; s.skip = skip_p;
        s.loAddr = s.hiAddr = address; s.rep = 0;
        Debug::log("[FDC IN probe] addr=%04X lo=%02X cpm=%d rom14=%d trdos=%d romInUse=%d disk=%d skip=%d pc=%04X",
                   address, lo8f, cpmNow, MemESP::romLatch,
                   ESPectrum::trdos, MemESP::romInUse, has_any_disk_p, skip_p, pcNow);
      };
      if (matches(slot[0])) {
        slot[0].rep++;
        if (address < slot[0].loAddr) slot[0].loAddr = address;
        if (address > slot[0].hiAddr) slot[0].hiAddr = address;
      } else if (matches(slot[1])) {
        slot[1].rep++;
        if (address < slot[1].loAddr) slot[1].loAddr = address;
        if (address > slot[1].hiAddr) slot[1].hiAddr = address;
      } else if (!slot[0].used) {
        fill(slot[0]);
      } else if (!slot[1].used) {
        fill(slot[1]);
      } else {
        flush(slot[0]);
        flush(slot[1]);
        fill(slot[0]);
      }
    }
  }

  // SPI-flash port probe (Karabas-Pro dev manual: #C7/#87/#A7/#E7/#67, CS
  // requires ~IORQ=0 & A(7:0)=port & CPM(DFFD.5)=1 & ROM14(7FFD.4)=1 &
  // DS80(DFFD.7)=1 — pico-speccy doesn't decode these at all). PQDOS bank0 ROM
  // has real IN/OUT to #C7/#A7/#E7 (~0x2492-0x24DB in profi64k.rom) — unknown
  // yet whether the boot/hang path actually reaches it. Unconditional probe,
  // capped, to settle that on real hardware.
  if (Z80Ops::isProfi) {
    uint8_t lo8spi = address & 0xFF;
    if (lo8spi == 0xC7 || lo8spi == 0x87 || lo8spi == 0xA7 || lo8spi == 0xE7 || lo8spi == 0x67) {
      static uint32_t spiInCnt = 0;
      if (spiInCnt < 200) {
        spiInCnt++;
        Debug::log("[SPI-FLASH IN] addr=%04X lo=%02X cpm=%d rom14=%d ds80=%d pc=%04X",
                   address, lo8spi, (portDFFD >> 5) & 1, (int)MemESP::romLatch,
                   (portDFFD >> 7) & 1, Z80::getRegPC());
      }
    }
  }
#endif

  if (Z80Ops::isByte && address >= 0xC000) {
    // вместо VIDEO::Draw(1, MemESP::ramContended[rambank]);
    // добавляем задержку через таблицу MemESP
    int delay = MemESP::getByteContention(address);
    VIDEO::Draw(delay, true);
  } else {
    // // ULA ports (A0=0): ULA always applies contention during display area
    // // Non-ULA ports (A0=1): contention only if port address maps to contended memory
    // bool earlyContend = ((address & 0x0001) == 0) ? !(Z80Ops::isPentagon || Z80Ops::isProfi) : MemESP::ramContended[rambank];
    // VIDEO::Draw(1, earlyContend); // I/O Contention (Early)
    // Early contention depends on ADDRESS (contended memory?), not port type
    // Wiki: ULA port non-contended addr = N:1,C:3; contended addr = C:1,C:3
    //        Non-ULA contended addr = C:1,C:1,C:1,C:1; non-contended = N:4
    VIDEO::Draw(1, MemESP::ramContended[rambank]); // I/O Contention (Early)
  }

  if (MEM_PG_CNT > 64 && address == 0xAFF7) {
    LED::touchR(LED::RAM);
    return portAFF7;
  }
  if (Z80Ops::isProfi && address == 0xDFFD) {
    LED::touchR(LED::RAM);
    return portDFFD;
  }
  bool ia = Z80Ops::isALF;
  uint8_t p8 = address & 0xFF;

  // «Байт»: any access to the Kempston-decoded port (#1F/#9F) toggles the
  // DD71 доп. ПЗУ overlay — the built-in test's switch stub at #387A is
  // IN A,(#9F); RET. Side effect only: the read still falls through to
  // whatever answers below (Kempston joystick / bus float). In TR-DOS #1F
  // belongs to the FDC.
  if (Z80Ops::isByte && !ESPectrum::trdos && (p8 & 0x7F) == 0x1F)
    Config::byteTestRomToggle();

  // Hidden RAM — Pentagon 512/1024 (and Profi) only. Plain Pentagon 128 must NOT
  // react: stock software probes #xxFB (printer port) — e.g. BALLQ's loader does
  // IN A,(#FB) at init — and remapping bank 0 to ram[MEM_PG_CNT+romLatch] sends
  // every bank-0 access through SD-swap on no-PSRAM boards (FPS halves).
  if ((Z80Ops::is512 || Z80Ops::is1024 || Z80Ops::isProfi)) {
    if (p8 == 0xFB) { // Hidden RAM on
#if FDD_PORT_TRACE
      // Suspected trigger for the "loaded data landed in the wrong page0 bank"
      // hang: recoverPage0() maps page0 to ram[MEM_PG_CNT+romLatch] once
      // newSRAM is true, which can differ from whatever bank a file-load loop
      // was just writing into. If this fires between a #0100 load and CALL
      // #0100, that's the mechanism.
      Debug::log("[HIDDEN-RAM] ON  romLatch=%d pc=%04X", MemESP::romLatch, Z80::getRegPC());
#endif
      MemESP::newSRAM = true;
      MemESP::recoverPage0();
      return 0xFF;
    }
    if (p8 == 0x7B) { // Hidden RAM off
#if FDD_PORT_TRACE
      Debug::log("[HIDDEN-RAM] OFF romLatch=%d pc=%04X", MemESP::romLatch, Z80::getRegPC());
#endif
      MemESP::newSRAM = false;
      MemESP::recoverPage0();
      return 0xFF;
    }
  }
  // IDE/HDD — NEMO scheme. Enabled on ANY machine when the user selects NEMO
  // (the NEMO interface is a bus card, not machine-specific). Decoded BEFORE the
  // ULA even-port branch because NEMO register ports (e.g. 0xC8/0xD0/0xF0) have
  // A0=0 and would otherwise be swallowed by the ULA port handler. 16-bit data
  // via A0 latch. Authentic NEMO is mapped outside TR-DOS; on Profi the SYSEN
  // line keeps ESPectrum::trdos permanently asserted (not real TR-DOS paging),
  // so the !trdos rule is bypassed there.
  if (IDE::scheme == IDE::NEMO && !(address & 6) && (Z80Ops::isProfi || !ESPectrum::trdos)) {
    if (address & 1) { LED::touchR(LED::IDE); return IDE::read_latch(); } // A0=1: high-byte latch
    if ((address & 0x18) == 0x08 && (address & 0xE0) == 0xC0) {          // control / alt-status
      LED::touchR(LED::IDE); return IDE::read8(8);
    }
    if ((address & 0x18) == 0x10) {                                      // register window
      LED::touchR(LED::IDE);
      uint8_t reg = (address >> 5) & 7;
      return (reg == 0) ? IDE::read_data_low() : IDE::read8(reg);
    }
    // else: not an IDE sub-address — fall through (don't shadow AY/ULA etc.)
  }
  // Scorpion GMX register read-backs — cold flash dispatch, see gmxPortRead.
  if (g_scorp_gmx) {
    uint8_t gmxData;
    if (gmxPortRead(address, &gmxData)) return gmxData;
  }
  // ULA PORT
  if ((address & 0x0001) == 0) {
    VIDEO::Draw(3, !(Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion)); // I/O Contention (Late)
    if (ia && p8 == 0xFE) {
      data = nes_pad2_for_alf(); // default port value is 0xFF.
    } else {
      data = 0xbf; // default port value is 0xBF.
      uint8_t portHigh = ~(address >> 8) & 0xff;
      for (int row = 0, mask = 0x01; row < 8; row++, mask <<= 1) {
        if ((portHigh & mask) != 0)
          data &= port[row];
      }
      // Profi extended keyboard: bit 5 of each standard row.
      // portHigh bit i set → row i is selected → AND extPort[i] with bit 5 only.
      // (other bits of extPort are kept 1 so they don't affect bits 0-4 of data)
      if (Z80Ops::isProfi && Config::profi_ext_keys) {
        for (int row = 0, mask = 0x01; row < 8; row++, mask <<= 1) {
          if ((portHigh & mask) != 0)
            data &= (extPort[row] | 0xDF); // mask: only bit 5 can be cleared
        }
      }
      // PAL_DETECT (bit7) = GX0 XOR BX0 — lets DS80 software self-detect the
      // palette IC's presence/type (3:3:2 vs 3:3:3) by writing known values to
      // GX0 (#7E) / BX0 (#FE bit7) and reading this back.
      if (Z80Ops::isProfi) {
        if (VIDEO::profi_gx0_latch ^ VIDEO::profi_bx0_latch)
          data |= 0x80;
        else
          data &= ~0x80;
      }
    }
    if (Tape::tapeStatus == TAPE_LOADING) LED::touchR(LED::TAPE);
    if (Tape::TapePortRead()) return data;
    // Turbo loaders at 0xFE00+ write to port254 to set border colors, which
    // on Issue2 hardware feeds bit3 back into EAR input (bit6), inverting
    // the tape signal. Bypass port254 feedback for turbo loaders.
    if (Tape::tapeStatus == TAPE_LOADING && Z80::getRegPC() >= 0xFE00) {
      if (Tape::tapeEarBit)
        data |= 0x40;
    } else {
      if ((Z80Ops::is48) &&
          (Config::Issue2)) { // Issue 2 behaviour only on Spectrum 48K
        if (port254 & 0x18)
          data |= 0x40;
      } else if (Z80Ops::isPentagon) {
        // Pentagon: the EAR input comes from the tape amplifier and idles
        // HIGH with nothing connected — real hardware reads bit 6 = 1 (no
        // port-254 feedback path on Pentagon). Software probes rely on it:
        // Neo8Tracker's PentEvo/TS-Conf/Pent1024v2 memory driver detects a
        // ZXEVO with `IN A,(#04BE); CP 255` — an idle-0 EAR made every even
        // port read 0xBF and misdetected ZXEVO on P512/P1024. Tape pulses
        // still toggle via the tapeEarBit XOR below (edge-based loaders are
        // polarity-insensitive).
        data |= 0x40;
      } else {
        if (port254 & 0x10)
          data |= 0x40;
      }
      if (Tape::tapeEarBit)
        data ^= 0x40;
    }
  } else {
    ioContentionLate(MemESP::ramContended[rambank]);
    // ZiFi NIC port: A0..A7 == 0xEF, A8..A15 selects register (0x00..0xC7)
    // 0xEFF7 (hi=0xEF > 0xC7) falls through to Pentagon mode16col handler below
    if (Config::zifi_enabled && p8 == 0xEF) {
      uint8_t zifi_hi = address >> 8;
      if (zifi_hi <= 0xC7)
        return ZiFi::read(zifi_hi);
      if (zifi_hi >= 0xF8) // 16550 UART window (#F8EF..#FFEF) — raw-UART drivers
        return ZiFi::uart16550Read(zifi_hi);
    }
    // ZX UNO register file (#FC3B address / #FD3B data) — Karabas-Pro's UART
    // bridge to its on-board ESP8266. Full 16-bit decode (as on the FPGA), bit8
    // picks the data port; bridges to the same ESP link as the #xxEF windows.
    if (Config::zifi_enabled && (address | 0x0100) == 0xFD3B)
      return ZiFi::unoUartRead(address & 0x0100);
    // MC146818 RTC data read (#BFF7) — Pentagon/Profi "Mr Gluk" TimeKeeper.
    // Register index was latched via OUT (#DFF7). Port is RTC-specific on these
    // machines, so no extra gating needed.
    if ((Z80Ops::isPentagon || Z80Ops::isProfi) && address == 0xBFF7) {
      // RTC off → static response (see RTC::readDisabled) instead of leaving the
      // port unclaimed; keeps the boot clock's UIP-wait from hanging.
      uint8_t rv = Config::rtc_enabled ? RTC::readData() : RTC::readDisabled();
#if RTC_PORT_TRACE
      // Rate cap: the ROMain status clock polls 6 regs per 50 Hz frame — an
      // uncapped log (~300 lines/s) exceeds the 115200 console and stalls
      // emulation. First 150 reads verbatim, then 1 of every 256.
      {
        static uint32_t rd_n = 0;
        if (++rd_n <= 150 || (rd_n & 0xFF) == 0)
          Debug::log("[RTC RD ] BFF7 sel=%02X -> %02X pc=%04X eff7=%02X n=%u",
                     RTC::dbgSel(), rv, Z80::getRegPC(), Ports::portEFF7, (unsigned)rd_n);
      }
#endif
      return rv;
    }
    // Karabas-Pro's OWN native RTC port interface (#FF/#BF AS, #DF/#9F DS) is
    // handled LATER in this function, after the Beta-128/FDC switch — see the
    // comment there for why (it must run only once FDC has declined the address).
#if RTC_PORT_TRACE
    // Catch-all: any other IN with low byte 0xF7 (reveals a non-#BFF7 data port).
    if ((Z80Ops::isPentagon || Z80Ops::isProfi) && (address & 0xFF) == 0xF7) {
      static uint32_t in_n = 0;
      if (++in_n <= 150 || (in_n & 0xFF) == 0)
        Debug::log("[RTC IN?] %04X pc=%04X eff7=%02X sel=%02X n=%u",
                   address, Z80::getRegPC(), Ports::portEFF7, RTC::dbgSel(), (unsigned)in_n);
    }
#endif
    if (ia && bitRead(p8, 7) == 0) {
      if (bitRead(p8, 1) == 0) { // 1D
        MemESP::newSRAM = true;
        MemESP::recoverPage0();
      } else { // 1F
        MemESP::newSRAM = false;
        MemESP::recoverPage0();
      }
    }
    // ULA+ data port read
    if (Config::ulaplus && address == 0xFF3B) {
      LED::touchR(LED::ULAPLUS);
      uint8_t reg = VIDEO::ulaplus_reg;
      if ((reg & 0xC0) == 0x00)
        return VIDEO::ulaplus_palette[reg & 0x3F];
      else
        return VIDEO::ulaplus_enabled ? 1 : 0;
    }
    // ShamaZX MIDI — status read from 0xA1CF
    // Bit 6 = "receiver full" — reflect real UART FIFO state
    // enabled 2=ShamaZX HW, 3=Soft Synth (both use ShamaZX ports)
    if (Midi::enabled >= 2 && address == 0xA1CF) {
      return Midi::busy() ? 0x40 : 0x00;
    }
    // ShamaZX MIDI — read from 0xA0CF (parallel mode handshake)
    if (Midi::enabled >= 2 && address == 0xA0CF) {
      return 0x00;
    }
    // General Sound — host-side status/data ports
    // {
    //   uint8_t a8 = address & 0xFF;
    //   if (a8 == 0xB3 || a8 == 0xBB) {
    //     Debug::log("IN %04X (a8=%02X) GS.en=%d", address, a8, GS::enabled);
    //   }
    // }
    if (GS::enabled && !DivMMC::divide_mode) {
      uint8_t a8 = address & 0xFF;
      if (a8 == 0xB3 || a8 == 0xBB) {
        LED::touchR(LED::GS);
        ioContentionLate(MemESP::ramContended[rambank]);
        return (a8 == 0xB3) ? GS::hostReadB3() : GS::hostReadBB();
      }
    }
    // Timex SCLD port read (port 0x00FF) — skip when TR-DOS is active (port conflict)
    if (Config::timex_video && !ESPectrum::trdos && address == 0x00FF) {
      LED::touchR(LED::TIMEX);
      ioContentionLate(MemESP::ramContended[rambank]);
      return VIDEO::timex_port_ff;
    }
    // Z80 DMA / zxnDMA port read: listen on both 0x0B and 0x6B
    if (Config::dma_mode && ((address & 0xFF) == 0x0B || (address & 0xFF) == 0x6B)) {
      LED::touchR(LED::DMA);
      ioContentionLate(MemESP::ramContended[rambank]);
      return Z80DMA::readPort();
    }
    // The default port value is 0xFF.
    data = 0xff;

    // MB-02+ ports: FDC (#0F/#2F/#4F/#6F), floppy status (#13)
    if (MB02::enabled) {
      uint8_t lo = address & 0xFF;
      if ((lo & 0x9F) == 0x0F) { // WD2797 registers
        FDDStep_MB02(true); // force step — WD2797 needs step advancement for Seek/Restore
        ioContentionLate(MemESP::ramContended[rambank]);
        uint8_t r = (lo >> 5) & 3;
        // FDD lamp/glyph/hum now come from rvmWD1793::fdd_active_decay (set by the
        // WD1793 state machine on genuine activity — see wd1793.h/.cpp), not from
        // port-access direction, so no LED::touchR here.
        uint8_t val = rvmWD1793Read(&ESPectrum::mb02_fdd, r);
        return val;
      }
      if (lo == 0x13) { // Floppy status (poll — not counted as access)
        FDDStep_MB02(true);
        ioContentionLate(MemESP::ramContended[rambank]);
        return MB02::readPort13();
      }
    }

    if (DivMMC::enabled) {
      uint8_t lo = address & 0xFF;
      if (lo == 0xE3) {
        LED::touchR(LED::SD);
        return (DivMMC::conmem ? 0x80 : 0) | (DivMMC::mapram ? 0x40 : 0) | DivMMC::bank;
      }
      if (DivMMC::divide_mode) {
        if ((lo & 0xE3) == 0xA3) {
          LED::touchR(LED::SD);
          uint8_t reg = (lo >> 2) & 0x07;
          return DivMMC::ide_read(reg);
        }
      } else {
        if (lo == 0xEB) {
          LED::touchR(LED::SD);
          return DivMMC::mmc_read();
        }
        if (lo == 0xE7) {
          LED::touchR(LED::SD);
          return 0xFF;
        }
      }
    }

    if (DivMMC::zc_enabled) {
      uint8_t lo = address & 0xFF;
      if (lo == 0x77) { LED::touchR(LED::ZCTRL); return DivMMC::zc_read_status(); }
      if (lo == 0x57) { LED::touchR(LED::ZCTRL); return DivMMC::zc_read_data(); }
    }

#if IDE_PORT_TRACE
    // Unconditional probe — fires for ANY IN with (addr&0xFF&0x9F)==0x8B
    // (the IDE-PROFI family: #xxCB/#xxEB/#xxAB), regardless of the IDE::scheme
    // and cpm/rom14 gates below. See the matching comment near the top of this
    // function for why (same investigation as the FDC/RTC probes).
    if (Z80Ops::isProfi && ((address & 0xFF) & 0x9F) == 0x8B) {
      Debug::log("[IDE IN probe] addr=%04X scheme=%d cpm=%d rom14=%d trdos=%d pc=%04X",
                 address, (int)IDE::scheme, (portDFFD & 0x20) != 0, MemESP::romLatch,
                 ESPectrum::trdos, Z80::getRegPC());
    }
#endif
    // IDE/HDD — PROFI scheme. Per Karabas-Pro/Profi manual "Порты IDE HDD (CF)":
    //   read regs at #xxCB, write regs at #xxEB, system reg at #xxAB.
    //   register selector = high byte A(10:8) = (address>>8)&7; #00CB = data low.
    //   CS active when (CPM=1 & ROM14=1) OR (DOS=1 & ROM14=0).
    //   CPM=(portDFFD&0x20), ROM14=MemESP::romLatch, DOS=ESPectrum::trdos.
    // Profi IDE — per UnrealSpeccy io.cpp MM_PROFI modified-ports section:
    //   Gate: (p7FFD & 0x10) && (pDFFD & 0x20) = ROM14=1 AND CPM=1 only.
    //   Port decode: (p1 & 0x9F)==0x8B, then A6 selects CS1 vs CS3.
    //   16-bit latch: #xxCB(A6=1,A5=0) → read_data()+latch_hi, return lo;
    //                 #xxEB(A6=1,A5=1) → return latch_hi (HIGH byte).
    if (IDE::scheme == IDE::PROFI && Z80Ops::isProfi) {
      bool cpm = (portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
      // UnrealSpeccy's gate (cpm&&rom14, "MBOOTHDD" scheme) never covered the
      // DOS=1&&!ROM14 case from the manual's own CS formula above (line 647):
      // the SYS-ROM self-test's own HDD0:/HDD1: probe (ROM 0x03BB → CALL
      // 0x1AB0: OUT (#06AB),0x06/0x02 soft-reset, IN (#07CB)/(#01CB) status)
      // runs with CPM=0/ROM14=0 — before CP/M is ever toggled on — so it was
      // silently unclaimed and HDD0:/HDD1: always showed "None"/"Fail"
      // regardless of a mounted image (hw-confirmed 2026-07-09 by
      // disassembling github.com/andykarpov/karabas-pro's bios_pqdos.hex).
      if ((cpm && rom14) || (dos && !rom14 && !cpm)) {
        uint8_t p1 = address & 0xFF;
        uint8_t reg = (address >> 8) & 7;
        if ((p1 & 0x9F) == 0x8B) {
          if (p1 & 0x40) {                             // CS1 (A6=1): data/registers
            LED::touchR(LED::IDE);
            uint8_t rv;
            if (p1 & 0x20)                             // A5=1 = #xxEB: HIGH byte latch
              rv = IDE::read_latch();
            else if (reg == 0)                         // A5=0 = #xxCB: low byte (16-bit data)
              rv = IDE::read_data_low();
            else
              rv = IDE::read8(reg);
#if IDE_PORT_TRACE
            Debug::log("[IDE RD] pc=%04X port=%02X reg=%d val=%02X CS1",
                       Z80::getRegPC(), (unsigned)p1, reg, rv);
#endif
            return rv;
          }
          // CS3 (A6=0) = #xxAB: ATA control block. reg6 → alternate status
          // (mirror of the status register). MBOOTHDD reads/writes #06AB with A5=0,
          // so do NOT gate on A5 here.
          if (reg == 6) {
            LED::touchR(LED::IDE);
            uint8_t rv = IDE::read8(7);                  // altstatus == status
#if IDE_PORT_TRACE
            Debug::log("[IDE RD] pc=%04X port=%02X reg=%d val=%02X CS3-altstatus",
                       Z80::getRegPC(), (unsigned)p1, reg, rv);
#endif
            return rv;
          }
#if IDE_PORT_TRACE
          Debug::log("[IDE RD] pc=%04X port=%02X reg=%d CS3 (unhandled)",
                     Z80::getRegPC(), (unsigned)p1, reg);
#endif
        }
      }
    }

    // PQ-DOS extended config ports #008B/#018B/#028B. CS formula verified
    // against the actual FPGA source (andykarpov/karabas-pro,
    // firmware/src/fpga/profi/rtl/karabas_pro.vhd:1332-1365) rather than just
    // the dev manual — #008B/#018B are CPM/ROM14/DOS-gated, but #028B is NOT
    // (cs_028b has no cpm/rom14/dos_act term at all, unlike cs_008b/cs_018b).
    // Register contents are stored/read back faithfully. Side effects wired
    // per the FPGA "TR-DOS FLAG" process (2026-07-10): #008B ONROM (bit6) =
    // forced DOS level (applied in the write handler below + exit suppression
    // in Z80::check_trdos), UNLOCK_128 (bit7) = 0x3Dxx automap also from the
    // 128K ROM (consumed in the trap). #028B TURBO_MODE (bits 5-6) is live
    // (synthesized from/applied to ESPectrum::multiplicator). The rest are
    // dead signals even in real hardware (rom1..rom5, ram0..ram7 assigned but
    // unused; rom0 only feeds the FPGA config-flash loader path — not
    // applicable to pico-speccy's static ROM-array model). No PQDOS build up to
    // BIOS 0.41h1 touches #008B/#018B at all (checked 2026-07-08).
    if (Z80Ops::isProfi) {
      if (address == 0x028B) {
        // TURBO_MODE (bits 5-6) is LIVE state, not a stored latch: ROMain's
        // status bar polls IN #028B & 0x60 every frame and redraws its
        // "Turbo:" field on change (rt 0x0592), and the FPGA hotkeys change
        // the same latch on real hardware. Reflect our CPU turbo
        // (multiplicator 0..3 = 3.5/7/14/28 MHz) so the guest sees the truth;
        // the remaining bits stay stub-stored (see the caveat above).
        uint8_t v = (port028B & ~0x60) | ((ESPectrum::multiplicator & 3) << 5);
#if PROFI_PORT_TRACE
        Debug::log("[8B IN] #028B -> %02X pc=%04X", v, Z80::getRegPC());
#endif
        return v;                     // unconditional (no CPM/ROM14/DOS gate)
      }
      bool cpm = (portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
#if PROFI_PORT_TRACE
      if (address == 0x008B || address == 0x018B)
        Debug::log("[8B IN probe] addr=%04X cpm=%d rom14=%d dos=%d pc=%04X",
                   address, cpm, rom14, dos, Z80::getRegPC());
#endif
      if ((cpm && rom14) || (dos && !rom14)) {
        if (address == 0x008B) return port008B;
        if (address == 0x018B) return port018B;
      }

      // PQDOS serial keyboard controller — ports #F3 (status) / #D3 (data).
      // Reverse-engineered from the QDOS keyboard driver (bank5):
      //   0x53F6: IN A,(#F3); OR A; RET     — status. bit1 = key-ready.
      //   0x546A: IN A,(#D3); OR A; RET     — next key scancode.
      //   0x5411: presence check — IN(#F3); INC A; RET NZ  → 0xFF means the
      //           device is absent, so we must return NON-0xFF.
      //   0x54C4: the boot-menu poll loop: reads #F3, if bit1 set reads #D3
      //           (scancode → E) then #F3 again (modifiers → D), toggling the
      //           border each pass (the "flashing border" = waiting for a key).
      // PQDOS has NO IN A,(#FE) matrix path anywhere, so this is the only way to
      // feed input.  We drain Ports::pqkBuf (filled from pico-speccy's keyboard,
      // see ESPectrum::processKeyboard): #F3 reports bit1=1 while a scancode is
      // queued; #D3 returns it and pops the queue; the follow-up #F3 read then
      // reports 0 → modifiers 0.  Scancodes are QDOS key-table indices.
      uint8_t lo8 = address & 0xFF;
      if (lo8 == 0xF3 || lo8 == 0xD3) {
        // К580ВВ51 serial mouse vs the PQDOS serial-keyboard hack: both live
        // on #F3/#D3. The VV51 answers with a real 8251 status (base 0x05 =
        // TxRDY+TxE — mouse drivers' presence check needs it), while the
        // PQDOS keyboard driver expects RAW 0x00/0x02 there (its follow-up
        // #F3 read after a scancode is the MODIFIER byte — a 0x05 base would
        // read as phantom modifiers). The two are irreconcilable on one read,
        // so pick by romset: the keyboard hack only exists for PQDOS. Within
        // any romset, RxE (ctl bit2) set always selects the mouse — the PQDOS
        // keyboard init never sets RxE (FPGA behaviour: the VV51 RX machine
        // is gated on RxE).
        if ((serialMouseCtl & 0x04) || Config::romSet != R_PROFI_PQ) {
          if (lo8 == 0xF3) {              // VV51 status register
            smTryBuildPacket();
            uint8_t st = (uint8_t)(0x05 | (sm_rxrdy ? 0x02 : 0x00)); // TxRDY+TxE | RxRDY
#if FDD_PORT_TRACE
            static uint16_t smLastPc = 0xFFFF; static uint8_t smLastSt = 0xFF;
            if (Z80::getRegPC() != smLastPc || st != smLastSt) {
              smLastPc = Z80::getRegPC(); smLastSt = st;
              Debug::log("[VV51 IN] F3 st=%02X ctl=%02X inten=%d pc=%04X",
                         st, serialMouseCtl, (int)serialMouseIntEn, smLastPc);
            }
#endif
            return st;
          }
          // #D3: VV51 data — current packet byte; a read advances the pipeline
          uint8_t v = sm_pkt[sm_pkt_pos > 2 ? 2 : sm_pkt_pos];
          if (sm_rxrdy) {
            sm_rxrdy = false;
            if (++sm_pkt_pos < 3) sm_rxrdy = true;
            else smTryBuildPacket();      // next packet if more deltas queued
          }
#if FDD_PORT_TRACE
          {
            static uint32_t smRd = 0;
            if (++smRd <= 60 || (smRd & 0x3F) == 0)
              Debug::log("[VV51 IN] D3 -> %02X pos=%u pc=%04X n=%u",
                         v, (unsigned)sm_pkt_pos, Z80::getRegPC(), (unsigned)smRd);
          }
#endif
          return v;
        }
        bool hasKey = (pqkHead != pqkTail);
#if FDD_PORT_TRACE
        static uint16_t lastKbPc = 0xFFFF; static uint8_t lastKbLo = 0;
        static bool lastKbKey = false;
        uint16_t pcn = Z80::getRegPC();
        if (pcn != lastKbPc || lo8 != lastKbLo || hasKey != lastKbKey) {
          lastKbPc = pcn; lastKbLo = lo8; lastKbKey = hasKey;
          Debug::log("[PQKBD IN] port=%02X (%s) key=%d pc=%04X",
                     lo8, lo8 == 0xF3 ? "status" : "data", (int)hasKey, pcn);
        }
#endif
        if (lo8 == 0xF3)
          return hasKey ? 0x02 : 0x00;  // bit1 = key ready; never 0xFF (present)
        // #D3: pop next scancode
        if (!hasKey) return 0x00;
        uint8_t s = pqkBuf[pqkTail];
        pqkTail = (pqkTail + 1) & 0x0F;
#if FDD_PORT_TRACE
        Debug::log("[PQKBD POP] scan=%02X", s);
#endif
        return s;
      }
    }

    // Beta-128 ports: accessible when TR-DOS ROM is paged in,
    // or when a raw-format disk (UDI/FDI/MBD/PRO) is inserted (copy-protected
    // loaders + Profi CP/M access WD1793 ports from RAM with TR-DOS ROM paged out)
    // Profi SYS ROM (romInUse=0) probes FDC during boot — use the stub below
    // (no real disk attached) so the BIOS boot menu can proceed. But if ANY
    // disk is mounted (TRD/SCL/FDI/UDI/MBD/Pro), route to real FDC so TR-DOS
    // and CP/M boot disk detection works.
    bool has_raw_disk = ESPectrum::fdd.disk[ESPectrum::fdd.diskS] &&
        (ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsUDIFile ||
         ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsFDIFile ||
         ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsMBDFile ||
         ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsTD0File ||
         ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsProFile);
    // Any mounted disk — includes TRD/SCL which are not "raw" but still need
    // real FDC routing so Profi SYS ROM disk probe succeeds.
    bool has_any_disk = ESPectrum::fdd.disk[ESPectrum::fdd.diskS] != nullptr;
    // Scorpion SYSEN (1FFD D1): the service monitor drives the WD1793 directly —
    // ZXMAK2 FddController: "Ports active when DOSEN=1 or SYSEN=1". Without this
    // the monitor's disk boot (reached from the guest 128 menu's TR-DOS row, the
    // reset-to-TR-DOS chain and the magic NMI) polls #1F forever: the FDC branch
    // declines (trdos=false — DOSEN drops at PC>=0x4000 while SYSEN stays), the
    // Kempston block below answers 0x00, and the monitor's head-load wait
    // `IN A,(#1F); AND #E0; JR Z` never exits (hw dump 2026-08-30: PC=0237 in
    // bank2, romInUse=2, romLatch=1).
    bool scorp_sysen = Z80Ops::isScorpion && (port1FFD & 0x02);
    // skip_real_fdc: bypass real WD1793 during Profi SYS ROM boot ONLY when
    // no disk is mounted at all.  With any disk (TRD/SCL/FDI/...), let the
    // real FDC handle it so the SYS ROM disk probe can succeed.
    bool skip_real_fdc = (Z80Ops::isProfi && MemESP::romInUse == 0 && !has_any_disk);

    // Profi CP/M mode: FDC data registers shift to 0x83/0xA3/0xC3/0xE3
    // UnrealSpeccy decode: (addr & 0x9F) == 0x83 → reg index = (addr >> 5) & 3
    //   0x83 → reg0 (CMD/STATUS), 0xA3 → reg1 (TRACK),
    //   0xC3 → reg2 (SECTOR),     0xE3 → reg3 (DATA)
    // 0xBF & 0x9F == 0x9F ≠ 0x83, so SYS port 0xBF falls through to switch below.
    // Profi CP/M shifted FDC: 0x83/0xA3/0xC3/0xE3 → WD1793 regs 0..3.
    // The Karabas-Pro manual p.22 says this is gated by ROM14=0, but the
    // Dos5 5.30 CP/M floppy driver (e.g. at 0x8625: OUT (0x3F)/OUT (0x83) cmd;
    // IN (0x83) BUSY poll) accesses these ports with ROM14=1 too. Gating on
    // ROM14=0 left IN (0x83) returning 0xFF (bus float) → BUSY bit stuck high
    // → the Type-I busy-wait at 0x862B (IN A,(0x83); RRCA; JR C) spun forever.
    // CPM=1 alone is the correct enable; 0xBF (SYS) is unaffected since
    // 0xBF & 0x9F == 0x9F ≠ 0x83.
    // Same OR-gate as RTC AS/DS and #008B/#018B: the PQDOS self-test's FDC
    // register round-trip check (ROM 0x140C: OUT/IN (0xC3), i.e. the SECTOR
    // register via this shifted decode) runs from the SYS ROM boot context
    // (DOS=1, ROM14=0, CPM=0 — CPM hasn't been toggled on yet at POST time),
    // so CPM-only left this port unclaimed → floating-bus mismatch → self-test
    // "Floppy Disc Controller: Fail" (confirmed via ROM disassembly of a
    // hardware self-test memory dump plus a live hw trace: skip_real_fdc was
    // true at that exact IN — see below — this block MUST be placed before
    // the skip_real_fdc gate, not just gain the DOS&&!ROM14 OR-term).
    // DELIBERATELY placed BEFORE skip_real_fdc/has_any_disk gating below:
    // a real WD1793 register (esp. the plain SECTOR register under test here)
    // is directly readable/writable regardless of whether a disk is in the
    // drive — only STATUS bits depend on media presence, and those are
    // synthesized separately (the #1F/#03-family stub right below, and the
    // real rvmWD1793 status bits elsewhere). Gating this on skip_real_fdc
    // (no disk mounted) left it fully unclaimed during the SYS ROM self-test
    // (which never has a disk mounted at that point) — floating-bus mismatch
    // on the very first OUT/IN(0xC3) pair → immediate self-test "Fail".
    bool cpm83 = (portDFFD & 0x20), rom14_83 = MemESP::romLatch, dos83 = ESPectrum::trdos;
    uint8_t fr83 = (address >> 5) & 0x3;
    // In the DOS&&!ROM14 SYS-ROM context (CPM not yet toggled on) ALL FOUR
    // shifted registers are the WD1793 — per the official Profi peripheral
    // map ("Основная периферия v0.03", CPM=0 & ROM14=0 BAS=0 ПЗУ SYS page):
    // #83=CMD/STATUS, #A3=TRACK, #C3=SECTOR, #E3=DATA, and RQ93 SYS = #3F
    // (dedicated branch below; 0x3F&0x9F≠0x83 so no overlap here).
    // HISTORY: fr was once restricted to 0/2 here on the belief that the
    // self-test's drive-select went through #A3 ("ROM 0x148D, OUT (#A3),A") —
    // that was a misread: 0x148D is `OUT (0x3F),A` (reset-release half of the
    // 0x147F drive-select routine; the whole routine only ever touches #3F),
    // and the self-test was actually fixed by the dedicated #3F SYS branch.
    // The leftover fr restriction bounced the BIOS boot loader's writes into
    // the case-0xa3/0xe3 SYS decode: OUT (#A3),track at ROM 0x15F5 became
    // profiFdcSysWrite(0) → bit2(reset)=0 → rvmWD1793Reset → track=0xFF →
    // every RDSEC RecordNotFound → the RESTORE↔RDSEC infinite retry loop
    // ("PQDOS BIOS boot hangs", hw log 2026-07-09: [FDC SYS] data=00 pc=15F7
    // + [FDC T2-STATUS] recNF=1 track=255 on all 20 reads). Same for the
    // SEEK-target write OUT (#E3) at ROM 0x15B3 (pc=15B5).
    if (Z80Ops::isProfi && ((address & 0x9F) == 0x83) &&
        (cpm83 || (dos83 && !rom14_83))) {
      // FDDStep(false) here, NOT (true): this path is now reachable with NO
      // disk mounted (moved outside skip_real_fdc, see above) — force=true
      // unconditionally drives rvmWD1793Step()'s real state machine, which
      // was never previously exercised with fdd.disk[]==nullptr (force=true
      // reads were always gated behind !skip_real_fdc, i.e. a disk present).
      // The self-test's tight 0x140C round-trip loop (~254 back-to-back
      // OUT/IN pairs) calling that every iteration hard-faulted the board
      // (reboot loop, hw-confirmed 2026-07-08). force=false matches the
      // sibling WRITE path just below (already proven safe with no disk:
      // it's been reachable unconditionally all along) — with no disk, HLD/
      // HLT are never set, so this is a no-op step, which is fine: register
      // *contents* don't need FDC-state advancement to read back correctly.
      FDDStep(false);
      return rvmWD1793Read(&ESPectrum::fdd, fr83);
    }

    // Profi CP/M mode: when the selected drive has no disk, FDC status reads
    // must return NOT_READY | SEEK_ERROR (0x90) with BUSY=0.
    //
    // Without this, IN A,(0x1F) returns 0xFF (bus float — FDC input not handled),
    // and the DSKKE9A busy-wait at 0x4043-0x4060 (IN A,(0x1F); RRCA; JR C loop)
    // spins forever: the timeout at 0x4050 has been disabled by self-modifying code
    // from a previous successful operation, so there is no exit.
    //
    // Returning 0x90 (BUSY=0, NOT_READY=1, SEEK_ERROR=1):
    //   • bit 0 = 0  → RRCA carry = 0 → JR C not taken → busy-wait exits normally
    //   • bit 4 = 1  → AND 0x10 ≠ 0  → error path at 0x40BD (SCF/RET carry=1)
    if (Z80Ops::isProfi && (portDFFD & 0x20) && !has_raw_disk &&
        (address & 0xE3) == 0x03) {
      return kRVMWD177XStatusNotReady | kRVMWD177XStatusSeek;
    }

    if (!skip_real_fdc && (ESPectrum::trdos || scorp_sysen || has_raw_disk)) {

      uint8_t dat;

      // Profi CP/M port 0x3F: per manual "Порты FDD", in the ROM14=1 & CPM=1
      // (MBOOTHDD) scheme #3F is the WD93 SYS register (RQ93) — read returns the
      // status (INTRQ bit7, DRQ bit6), used in the sector-read loop at 0x86A4
      // (IN A,(0x3F); AND 0xC0; JP M → INI from 0xE3). In ROM14=0 & CPM=1
      // (BOOTFDD) #3F is the WD track register — handled by case 0x23 below.
      // Gate matches the OUT(#3F) SYS write path: CPM=1 & ROM14=1.
      // THIRD context (CPM=0, ROM14=0, DOS=1 — the SYS-ROM self-test itself,
      // before CP/M is ever toggled on): #3F is ALSO the SYS register here.
      // Confirmed by disassembling github.com/andykarpov/karabas-pro's
      // bios_pqdos.hex (ROM 0x1432/0x1478, the FDD0:/FDD1: detect routine):
      // it computes a drive/side/reset/test control byte and writes it to
      // #3F while ROM14=0 and CPM has not been set — treating #3F as track
      // register there (case 0x23) meant the self-test's drive-select write
      // was silently dropped, so fdd.diskS never left its default and
      // FDD0:/FDD1: showed "Fail" even with a disk mounted (hw-confirmed
      // 2026-07-09).
      bool cpm3f = (portDFFD & 0x20), rom14_3f = MemESP::romLatch, dos3f = ESPectrum::trdos;
      if (Z80Ops::isProfi && ((address & 0xFF) == 0x3F) &&
          ((cpm3f && rom14_3f) || (dos3f && !rom14_3f && !cpm3f))) {
        // SYS status poll — not counted as disk access.
        FDDStep(true);
        uint8_t v = 0;
        if (ESPectrum::fdd.control & kRVMWD177XDRQ)                        v |= 0x40;
        if (ESPectrum::fdd.control & (kRVMWD177XINTRQ | kRVMWD177XFINTRQ)) v |= 0x80;
        return v;
      }

      // Profi port #BF: per manual "Порты FDD" (table on p.22) + "Системный
      // регистр ВГ93 (RQ93)" (p.23), in the ROM14=0 & CPM=1 (BOOTFDD) scheme
      // #BF is the WD93 SYS register — same bit layout as #3F above (read:
      // INTRQ bit7, DRQ bit6; write: DRIVE0/1, RESET, HRDY, SIDE, ~DDEN —
      // already implemented by profiFdcSysWrite() on the write side via case
      // 0xa3 below). Only the READ side was missing: IN A,(#BF) fell through
      // this whole switch unclaimed (0xBF&0xE3==0xA3 has no read case),
      // returning floating-bus garbage instead of DRQ/INTRQ. PQDOS's boot
      // loader (romInUse=2, CPM=1/ROM14=0) polls #BF waiting for DRQ/INTRQ
      // and spun forever — hw-confirmed 2026-07-09 (log: tight IN(#7F)/IN(#BF)
      // loop at pc=BC84/BC8D, thousands of iterations/frame, "PQ-DOS
      // Loading..." hang).
      if (Z80Ops::isProfi && (portDFFD & 0x20) && !MemESP::romLatch &&
          ((address & 0xFF) == 0xBF)) {
        FDDStep(true);
        uint8_t v = 0;
        if (ESPectrum::fdd.control & kRVMWD177XDRQ)                        v |= 0x40;
        if (ESPectrum::fdd.control & (kRVMWD177XINTRQ | kRVMWD177XFINTRQ)) v |= 0x80;
#if FDD_PORT_TRACE
        // Capture the FDC state while the boot loader spins on #BF with neither
        // DRQ nor INTRQ (v==0) — the "load hangs mid-sector" stall. Rate-limited.
        if (v == 0) {
          static uint64_t lastLog = 0;
          uint64_t now = time_us_64();
          if (now - lastLog > 500000) {
            lastLog = now;
            auto &f = ESPectrum::fdd;
            int dt = f.disk[f.diskS] ? (int)f.disk[f.diskS]->t : -1;
            Debug::log("[BF-STALL] state=%u ss=%u c=%u retry=%u cmd=%02X "
                       "trk=%u dt=%d sec=%u side=%u ctrl=%05X trkPend=%u fast=%u "
                       "fdiSecCnt=%d ldCyl=%d ldSide=%d ldUnit=%d dS=%u pc=%04X",
                       (unsigned)f.state, (unsigned)f.stepState, (unsigned)f.c,
                       (unsigned)f.retry, f.command, f.track, dt, f.sector, f.side,
                       (unsigned)f.control, (unsigned)f.trackLoadPending,
                       (unsigned)f.fastmode, f.fdiSectorCount,
                       f.diskLoadedCyl, f.diskLoadedSide, f.diskLoadedUnit,
                       (unsigned)f.diskS, Z80::getRegPC());
            // Dump the MFM stream around indx so we can see whether the data
            // mark (A1 A1 A1 FB) the byte-scan waits for is actually present.
            if (f.disk[f.diskS] && f.diskTrackBuf && f.diskTrackLen) {
              uint32_t ix = f.disk[f.diskS]->indx;
              uint32_t tl = f.diskTrackLen;
              char sids[96]; int p = 0;
              for (int n = 0; n < f.fdiSectorCount && n < 10 && p < 80; n++)
                p += snprintf(sids + p, sizeof(sids) - p, "%u:%02X ",
                              (unsigned)f.fdiSectorIdPos[n],
                              (unsigned)f.fdiSectorFlags[n]);
              Debug::log("[BF-STALL2] indx=%u trkLen=%u idPos/flags: %s",
                         (unsigned)ix, (unsigned)tl, sids);
              if (ix < tl) {
                char hx[80]; int q = 0;
                for (int i = -4; i <= 19 && q < 70; i++) {
                  int a = (int)ix + i;
                  if (a >= 0 && a < (int)tl)
                    q += snprintf(hx + q, sizeof(hx) - q, "%02X ",
                                  f.diskTrackBuf[a]);
                }
                Debug::log("[BF-STALL2] buf[indx-4..+19]: %s", hx);
              }
            }
          }
        }
#endif
        return v;
      }

      // SPI-flash ports (#C7/#87/#A7/#E7/#67 per Karabas-Pro dev manual) are
      // reserved for the on-board flash chip regardless of CPM/ROM14/DS80
      // state — real hardware never routes them to the WD1793. The (address &
      // 0xe3) alias mask below was widened to catch the #FF/#BF "families"
      // for OTHER hw-confirmed cases, but #A7 (aliases #BF/case 0xa3) and #67
      // (aliases #7F DATA reg/case 0x63) collide with it: PQDOS's own SPI-
      // flash probe (bank0 ROM ~0x28xx, IN A,(#A7)/#C7 polling FLASH_READY)
      // got back bogus WD1793 status/data instead of flash status — hw log
      // 2026-07-09. #C7/#87 happen not to alias into this mask, but exclude
      // all 5 for correctness/documentation symmetry with the write side.
      if (Z80Ops::isProfi) {
        uint8_t lo8spiEx = address & 0xFF;
        if (lo8spiEx == 0x67 || lo8spiEx == 0x87 || lo8spiEx == 0xA7 ||
            lo8spiEx == 0xC7 || lo8spiEx == 0xE7)
          goto skip_fdc_alias_switch;
      }

      switch (address & 0xe3) {
      case 0x03:
        // Port #1F is shared: WD1793 status register AND the standard Kempston
        // joystick (decodes A5=0). With a raw disk mounted (e.g. TD0/Pro CP/M
        // images stay mounted while a game runs), this FDC branch shadowed the
        // Kempston read below and broke the joystick. Per Karabas-Pro manual
        // p.24 the FDC owns #1F only when CPM=1 (DOS=0) — i.e. an active loader
        // context: TR-DOS ROM paged in or Profi CP/M mode. Otherwise (a running
        // game polling the joystick) let it fall through to the Kempston block.
        if (Config::joystick == JOY_KEMPSTON && !ESPectrum::trdos && !scorp_sysen &&
            !(Z80Ops::isProfi && (portDFFD & 0x20)))
          break;
        // fallthrough — FDC owns #1F in loader/CP-M context
      case 0x23:
      case 0x43:
      case 0x63:
        FDDStep(false);
        return rvmWD1793Read(&ESPectrum::fdd, ((address >> 5) & 0x3));

      case 0xa3:
        // Port #BF (address & 0xe3 == 0xa3) is the RQ93 SYS register only in
        // ROM14=0 & CPM=1 (BOOTFDD). When ROM14=1 the SYS register moves to #3F
        // (MBOOTHDD scheme, handled before this switch) and #BF is reassigned
        // to extended periphery.
        if (Config::arch != A_PROFI || MemESP::romLatch)
          break;
        goto fdc_sys_status;
      case 0xe3:
        // Port #FF is the Beta128 SYS register ONLY when the TR-DOS ROM is
        // paged in (real Beta128 decodes its FDC ports only while its ROM is
        // active). With a raw disk merely mounted but TR-DOS not paged (e.g. a
        // 48K program running with an FDI/UDI image still mounted), #FF must
        // float — otherwise IN A,(0xFF) returns FDC status (~0x00) instead of
        // the floating bus, breaking floating-bus reads (games + halt2int's
        // Float test → "Unknown"). On Profi trdos is permanently asserted
        // (SYSEN), so its SYS-register path is unaffected. Scorpion's SYSEN is
        // a separate latch (1FFD D1) — the service monitor selects drives via
        // #FF too, so it counts as "TR-DOS paged" here.
        if (!ESPectrum::trdos && !scorp_sysen)
          break;
        // Port #FF (and #FF-family) is the SYS register only in the standard
        // scheme (CPM=0). In CP/M the SYS register is at #BF/#3F and the
        // #FF-family belongs to extended periphery (IDE etc.) — see the write
        // path. So do NOT return FDC status for these ports in CP/M mode.
        if (Z80Ops::isProfi && (portDFFD & 0x20))
          break;
      fdc_sys_status: {
        // SYS-register status read: bit 7 = INTRQ, bit 6 = DRQ (Beta-128
        // ordering, verified on Profi 5.06 SYS-ROM at 0x07A4: `JP M`).
        // Pure status poll — not counted as disk access (would pin the LED).
        FDDStep(true);
        uint8_t v = 0;
        if (ESPectrum::fdd.control & kRVMWD177XDRQ)                        v |= 0x40;
        if (ESPectrum::fdd.control & (kRVMWD177XINTRQ | kRVMWD177XFINTRQ)) v |= 0x80;
        return v;
      }
      }
    skip_fdc_alias_switch: ;
    }

    // Same RTC:: singleton, Karabas-Pro's OWN native port interface (dev manual
    // v1.01, distinct from the Gluk #DFF7/#BFF7 pair above):
    //   #FF/#BF = AS (address latch, write-only, low byte only, bit6 don't-care)
    //   #DF/#9F = DS (data, R/W, low byte only, bit6 don't-care)
    // Full CS per the manual: (CPM=1&&ROM14=1)||(DOS=1&&ROM14=0). Placed HERE
    // (after the Beta-128/FDC switch above, instead of using the same early
    // spot as the Gluk ports) so it only ever fires once FDC has had first
    // refusal on #FF/#BF: the switch above already `return`s for every case it
    // claims and only reaches here via `break` (declined) or by never entering
    // at all (skip_real_fdc, or trdos==false && !has_raw_disk). Confirmed via a
    // real PC dump (2026-07-08, PQDOS BIOS 0.41h1 self-test, romInUse=0,
    // romLatch=0, no disk mounted -> skip_real_fdc=true, FDC inert) that the
    // boot-time RTC-format patch (pqdos_rtc_patch.asm get_ad/set_ad) runs
    // exactly in this ROM14=0 window and NEEDS the DOS=1&&ROM14=0 branch —
    // dropping it (as an earlier revision of this code did, to dodge a
    // *theoretical* collision with FDC case 0xa3 when a disk IS mounted) left
    // #BF/#9F unclaimed by anyone during the self-test, which is why RTC kept
    // showing Fail even after the port decode itself was verified correct.
#if RTC_PORT_TRACE
    // Unconditional probe log: fires even when the gate is false, so a trace
    // capture shows whether PQDOS ever touches #FF/#BF/#DF/#9F at all, and
    // with what cpm/rom14/trdos state, when the gate doesn't pass.
    if (Z80Ops::isProfi) {
      uint8_t lo8t = address & 0xFF;
      if ((lo8t | 0x40) == 0xFF || (lo8t | 0x40) == 0xDF) {
        static uint32_t pin_n = 0;
        if (++pin_n <= 150 || (pin_n & 0x3FF) == 0)
          Debug::log("[RTC-AS/DS IN probe] addr=%04X lo=%02X cpm=%d rom14=%d trdos=%d pc=%04X n=%u",
                     address, lo8t, (portDFFD & 0x20) != 0, MemESP::romLatch,
                     ESPectrum::trdos, Z80::getRegPC(), (unsigned)pin_n);
      }
    }
#endif
    if (Z80Ops::isProfi) {
      bool cpm = (portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
      if ((cpm && rom14) || (dos && !rom14)) {
        uint8_t lo8 = address & 0xFF;
        if ((lo8 | 0x40) == 0xDF) {
          // RTC off → static response (UIP-clear on status regs) so ROMain's
          // boot MC146818 UIP-wait exits instead of spinning on 0xFF forever.
          uint8_t rv = Config::rtc_enabled ? RTC::readData() : RTC::readDisabled();
#if RTC_PORT_TRACE
          Debug::log("[RTC-DS IN] sel=%02X -> %02X pc=%04X", RTC::dbgSel(), rv, Z80::getRegPC());
#endif
          return rv;
        }
        // #FF/#BF (AS) is write-only per the manual — no read defined.
      }
    }

    /// if (ESPectrum::ps2mouse && Config::mouse == 1)
    // Karabas-Pro manual p.25-27: Kempston Mouse gate is "CPM=0" — in CP/M
    // mode #xxDF ports are reassigned to extended periphery (e.g. RTC #DF).
    // Decode: the manual specifies FULL 16-bit addresses (#FADF/#FBDF/#FFDF),
    // so on Profi we match them exactly — the classic partial &0x05FF decode
    // aliased e.g. #0ADF onto the buttons port, which is exactly the address
    // ROMain's Karabas-RTC presence probe reads (reg 0x0A in A → IN A,(#DF)),
    // and a non-0xFF answer there fakes an RTC. Other archs keep the
    // traditional partial decode (Pentagon-style Kempston mice rely on it).
    if (!(Z80Ops::isProfi && (portDFFD & 0x20))) {
      uint16_t mdec = Z80Ops::isProfi ? (uint16_t)address : (address & 0x05ff);
      if (mdec == (Z80Ops::isProfi ? 0xFBDF : 0x01df)) {
        LED::touchR(LED::KEMPMOUSE);
        return (uint8_t)ESPectrum::mouseX;
      }
      if (mdec == (Z80Ops::isProfi ? 0xFFDF : 0x05df)) {
        LED::touchR(LED::KEMPMOUSE);
        return (uint8_t)ESPectrum::mouseY;
      }
      if (mdec == (Z80Ops::isProfi ? 0xFADF : 0x00df)) {
        LED::touchR(LED::KEMPMOUSE);
        // No mouse ever attached → keep the bus-float 0xFF so presence
        // detection (buttons==0xFF) still reads "absent".
        if (!ESPectrum::mouseSeen) return 0xff;
        // Manual p.25: bit0=R, bit1=L, bit2=M (active low), bit3=1,
        // bits4-7 = wheel notch counter.
        return (uint8_t)(((ESPectrum::mouseWheel & 0x0F) << 4) | 0x08 |
                         (ESPectrum::mouseButtonM ? 0 : 0x04) |
                         (ESPectrum::mouseButtonL ? 0 : 0x02) |
                         (ESPectrum::mouseButtonR ? 0 : 0x01));
      }
    }

    // Profi FDC stub: return WD1793 "no disk" sequence so boot ROM's FDC
    // detection fails cleanly instead of hanging in its wait-for-BUSY loop.
    // Stateful: returns 0x81 (BUSY|NOT_READY) once after an OUT command, then
    // 0x90 (SEEK_ERROR|NOT_READY) — ROM sees error at 0x073D → gives up on FDC.
    // Applies when SYS ROM is active (Profi BIOS probes FDC even with SYSEN).
    if (Z80Ops::isProfi && MemESP::romInUse == 0 && (address & 0xE3) == 0x03) {
      if (profi_fdc_busy) {
        profi_fdc_busy = 0;
        return 0x81; // BUSY|NOT_READY — exits ROM wait-for-busy at 0x0710
      }
      return 0x90; // SEEK_ERROR|NOT_READY — fails FDC presence check at 0x073D
    }

    // Kempston Joystick
    // Standard Kempston decodes A5=0 — always honored so games like Dizzy
    // that read port 0x1F keep working even when an alternate kempstonPort
    // (0x37, 0x5F) is selected for boards that also map joystick reads there.
    // Karabas-Pro manual p.24: gate is "CPM=0 & DOS=0" — in CP/M mode the
    // port #1F belongs to the FDC and Kempston must stay off the bus.
    if (Config::joystick == JOY_KEMPSTON &&
        !(Z80Ops::isProfi && (portDFFD & 0x20))) {
      if (((p8 & 0x20) == 0) || (p8 == Config::kempstonPort)) {
        LED::touchR(LED::KEMPJOY);
        return ia ? (port[Config::kempstonPort] ^ 0xA0)
                  : port[Config::kempstonPort];
      }
    }

    // Fuller Joystick
    if (Config::joystick == JOY_FULLER && p8 == 0x7F)
      return port[0x7f];

    // Sound (AY-3-8912)
    if (ESPectrum::AY_emu) {
      if ((address & 0xC002) == 0xC000) {
        LED::touchR(LED::AY);
        AySound* chip = ayChipFor(address);
        // TurboSound FM status mode (see the #F8..#FF select in Ports::output):
        // the YM2203 status byte is bit 7 = BUSY plus the two timer-overflow
        // flags in bits 1..0. BUSY is always clear — every register write here
        // completes inside the OUT, so there is nothing to wait for, and a driver
        // polling BUSY has to see it go away or it spins forever (hw 2026-08-07).
        // The timer flags are real (OpnFm runs both timers); with no FM half
        // allocated they read 0, which is the same "idle" answer as before.
        uint8_t rd;
        if (AySound::ts_status_read) {
#if TSFM_TRACE
          tsfmProbe(true);
#endif
          OpnFm* fm = opnfm[AySound::selected_chip];
          rd = fm ? fm->status() : 0x00;
        } else {
          rd = chip ? chip->getRegisterData() : 0xFF;
        }
        if (ia) {
          return rd | newAlfBit;
        }
        return rd;
      }
    }
    // Scorpion has no float bus either (Fuse: unattached_port_none) — unmapped
    // reads answer 0xFF and the 128K "IN #7FFD rewrites the latch" quirk (a
    // 128K-ULA artifact) never happens there.
    if (!(Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion)) {
#if HALT2INT_TRACE
      if (address == 0xFFFF)
        Debug::log("[FLOAT-IN] addr=%04X ts=%u ia=%d", address, CPU::tstates, (int)ia);
#endif
      data = getFloatBusData();
      if ((!Z80Ops::is48) && ((address & 0x8002) == 0) &&
          (!Z80Ops::isALF || (address & 0x0080))) { // ALF: #7FFD reflect, A7=1 only
        LED::touchR(LED::RAM);
        // //  Solo en el modelo 128K, pero no en los +2/+2A/+3, si se lee el
        // puerto
        // //  0x7ffd, el valor leído es reescrito en el puerto 0x7ffd.
        // //  http://www.speccy.org/foro/viewtopic.php?f=8&t=2374
        if (!MemESP::pagingLock) {
          MemESP::pagingLock = bitRead(data, 5);
          uint32_t page = (data & 0x7);
          if (MEM_PG_CNT > 64) {
            page += portAFF7 * extendedZxRamPages();
            uint32_t pages =
                ram_pages + butter_pages + psram_pages + swap_pages;
            if (page >= pages) {
              page = (data &
                      0x7); // W/A: protection of incorrect page selection logic
            }
          }
          if (MemESP::bankLatch != page) {
            MemESP::bankLatch = page;
            MemESP::ramCurrent[3] = MemESP::ram[page].sync(3);
            MemESP::ramContended[3] = page & 0x01 ? true : false;
          }
          if (MemESP::videoLatch != bitRead(data, 3)) {
            MemESP::videoLatch = bitRead(data, 3);
            if (Z80Ops::isProfi && (portDFFD & 0x80)) {
              VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[6].direct() : MemESP::ram[4].direct();
              uint32_t clrPage = MemESP::videoLatch ? 58 : 56;
              uint32_t totPages = ram_pages + butter_pages + psram_pages + swap_pages;
              VIDEO::profi_clrmem = (clrPage < totPages) ? MemESP::ram[clrPage].direct() : nullptr;
            } else {
              VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();
              if (Z80Ops::isProfi) VIDEO::profi_clrmem = nullptr;
            }
            if (Config::gigascreen_onoff == 2) VIDEO::gigascreen_auto_countdown = 3;
            if (VIDEO::mode16col_enabled) VIDEO::mode16colUpdatePlanes();
          }
          MemESP::romLatch = bitRead(data, 4);
          if (!ESPectrum::trdos) {
            // Profi: 7FFD bit4 selects bank 2 (128K compat) vs bank 3 (SOS)
            // — banks 0 (SYS) and 1 (TR-DOS) are reserved for SYSEN/DOSEN.
            MemESP::romInUse = (Z80Ops::isProfi)
                ? (MemESP::romLatch ? 3 : 2)
                : MemESP::romLatch;
            MemESP::recoverPage0();
          }
        }
      }
    }
  }
  return data;
}

// Profi CP/M system (RQ93) register write: drive select, soft-reset, HLT/test,
// side select (bit4: 1→side0, 0→side1) and density (bit5: ~DDEN). Shared by the
// standard scheme (SYS at 0xBF/0xFF) and the Dos5 5.30 shifted scheme, where the
// MBOOTHDD loader addresses the SYS register at 0x3F (not 0xBF). Without routing
// 0x3F here it landed in the WD TRACK register (0x3F&0xe3==0x23), so the
// side-select OUT(0x3F),0x1C was silently lost and fdd.side stuck → side-compare
// rejected the catalog on track0/side0 → "FDD Read Error".
static inline void profiFdcSysWrite(uint8_t data) {
#if FDD_PORT_TRACE
  // Some ROMs pulse just the HLT bit (bit3) in a tight software-timed wait loop —
  // logging every single write there floods/garbles the UART (thousands of lines
  // that only ever alternate bit3) and drowns out the far rarer, more useful
  // [FDC CMD] trace. Dedupe on everything EXCEPT bit3, so a genuine drive/reset/
  // side/density change still logs even while HLT happens to be mid-pulse.
  static uint8_t lastData = 0xFF; // no register write is 0xFF at reset, forces first log
  if ((data & ~0x08) != (lastData & ~0x08)) {
    lastData = data;
    Debug::log("[FDC SYS] data=%02X drv=%d reset=%d hlt(bit3)=%d side(bit4)=%d dden=%d pc=%04X",
               data, data & 3, (int)((data & 0x04) == 0), (int)((data & 0x08) != 0),
               (int)((data & 0x10) != 0), (int)((data & 0x20) == 0),
               Z80::getRegPC());
  }
#endif
  // Change active disk unit. Full 2-bit select (4 units), per the Karabas-Pro
  // dev manual RQ93 register (DRIVE bits 0-1). ZXMAK2's classic-Profi model
  // masked this to 1 bit (2 physical drives, WD1793.cs:227) and we used to
  // follow it on Profi — but that aliased C: onto A: (and D: onto B:), so a
  // TR-DOS "LIST C:" showed drive A's catalog and units 2/3 were unreachable.
  uint8_t new_drive = data & 0x3;
  if (ESPectrum::fdd.diskS != new_drive) {
    ESPectrum::fdd.diskS = new_drive;
    if (ESPectrum::fdd.disk[ESPectrum::fdd.diskS] != NULL &&
        ESPectrum::fdd.side &&
        ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->sides == 1)
      ESPectrum::fdd.side = 0;
    ESPectrum::fdd.sclConverted = false;
    // Fastmode is per-disk: re-evaluate it for the newly selected drive so a
    // raw image in one slot doesn't force a standard disk in another to slow.
    rvmWD1793UpdateFastmode(&ESPectrum::fdd);
  }

  if (!(data & 0x4)) {
    rvmWD1793Reset(&ESPectrum::fdd);
    profi_nodisk_reissue_cnt = 0;
    profi_shifted_fdc = false;
  }

  if (data & 0x8)
    ESPectrum::fdd.control |= kRVMWD177XTest;
  else
    ESPectrum::fdd.control &= ~kRVMWD177XTest;

  if (data & 0x10)
    ESPectrum::fdd.side = 0;
  else {
    if (ESPectrum::fdd.disk[ESPectrum::fdd.diskS] != NULL)
      ESPectrum::fdd.side =
          ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->sides == 1 ? 0 : 1;
    else
      ESPectrum::fdd.side = 1;
  }

  // RQ93 bit 5: ~DDEN (0=MFM double density, 1=FM single density)
  if (data & 0x20)
    ESPectrum::fdd.control &= ~kRVMWD177XDDEN;
  else
    ESPectrum::fdd.control |= kRVMWD177XDDEN;
}

// GMX ProfROM 0x0100-0x010F read tap: armed only while the SERVICE bank sits at
// 0x0000 with ROM actually mapped (ZXMAK2 MemoryScorpionProfRom256 gates on SYSEN;
// MAME taps 0x0100-0x010C when (bank & 3) == SYS && !romram). Recomputed on every
// romInUse change for Scorpion, so the hot-path test in peek8/fetchOpcode is one
// almost-always-false global load.
#if GMX_TRACE
// Scorpion GMX paging trace (-DGMX_TRACE=ON): every ROM-bank transition, GMX
// register write, magic reset and TR-DOS trap event, capped so the boot
// sequence fits the UART without stalling emulation. Shared with Z80_JLS.cpp.
uint32_t g_gmxTraceN = 0;
#endif

static inline void gmxTapUpdate() {
  // The legacy ProfROM 0x0100-0x010F read tap is DISABLED on GMX (hw trace
  // 2026-08-31): the v2.94 service monitor (plane 4 bank 2) checksums its whole
  // 16K with 1FFD D1 set — the CPI loop at 0x31B8 reads straight through
  // 0x0100-0x010F, and the tap flipped the plane to 0 mid-execution
  // (GMX_TRACE: "[GMX romU] 18->2 ... plane=0 pc=31B9"), landing the CPU in
  // plane 0's DATA bank — the striped-screen crash. The GMX firmware switches
  // planes exclusively via #7EFD D4-6 (same trace: every deliberate plane
  // change is a 7EFD write from the RAM thunks at E3FD/E429); the 0x010x tap
  // belongs to the ProfROM add-on for the Yellow/Green boards (ZXMAK2
  // MemoryScorpionProfRom256 — ZXMAK2 has no GMX machine at all, and MAME's
  // scorpiongmx only inherits the tap from scorpiontb). Keep gmxProfRomTap /
  // kProfPlaneMap for a future ProfROM romset — the arming condition there
  // was ((romInUse & 3) == 2) && !page0ram, on M1 ONLY (the data-read hook in
  // peek8 is what fired on the checksum; ZXMAK2 subscribes both, but ZXMAK2
  // never ran this firmware).
  g_gmx_tap = false;
#if GMX_IN_FLASH
  // GMX banks are stored deduplicated + as overlays over ROMs already in flash
  // (scorpion_gmx_banks.h). MemESP's overlay registry keys ONE overlay per base
  // pointer, and several GMX banks derive from the SAME base (plane 1 and plane 4
  // both patch the Sinclair 128K halves) — so the registration is DYNAMIC: every
  // romInUse change lands here (scorpionRomUpdate / gmxTapRecheck) and re-registers
  // the overlay of the bank now live at 0x0000. Only the live bank's pointer is
  // ever consulted, so a stale entry for a base that is not paged in is harmless.
  // For a raw bank this registers nullptr — a no-op. The table itself must be
  // touched only from Config.cpp (see gmxRegisterLiveOverlay's comment).
  if (g_scorp_gmx) gmxRegisterLiveOverlay(MemESP::romInUse);
#endif
}

// The 0xC000 RAM page from all three latches: 7FFD bits 0-2 (low3), 1FFD D4 (+8),
// and on GMX the #DFFD 3 extra bits (<<4) — 128 pages = 2 MB (MAME scorpiongmx).
// Bounds W/A like the Profi combine: never walk off the page strip.
static inline uint32_t scorpionC000Page(uint32_t low3) {
  uint32_t page = low3 | ((Ports::port1FFD & 0x10) >> 1);
  if (g_scorp_gmx) page |= (uint32_t)(Ports::portDFFDgmx & 0x07) << 4;
  uint32_t pages = ram_pages + butter_pages + psram_pages + swap_pages;
  if (page >= pages) page = low3;
  return page;
}

// Scorpion ROM select — MAME's hardware-derived function (sinclair/scorpion.cpp):
//   rom = 1FFD D1 ? 2 : ((dos << 1) | 7FFD D4)
// bank0/1 = BASIC-128/BASIC-48, bank2 = service monitor, bank3 = TR-DOS. Note the
// quirk that IS the hardware: in DOS with the 128 ROM selected (D4=0) the SERVICE
// page appears, not TR-DOS — normal TR-DOS software always runs with D4=1.
// GMX: the bank lands inside the live ProfROM plane (romInUse = plane*4 + bank),
// and 1FFD D2 hard-wires the DOS page at 0x0000 (overriding even RAM0) with the
// Beta interface forced on — MAME scorpiongmx scorpion_update_memory.
// The ONLY place Scorpion's rom bank is derived — callers: the #1FFD handler, the
// Scorpion arm of the #7FFD rom-select, check_trdos entry/exit, the .z80 loader.
// recoverPage0() already orders newSRAM > page0ram > rom[romInUse], which matches
// the hardware (RAM0 wins over the service override).
void Ports::scorpionRomUpdate() {
#if GMX_TRACE
  uint8_t gmxt_prev = MemESP::romInUse;
#endif
  if (g_scorp_gmx && (port1FFD & 0x04)) {
    MemESP::romInUse = (gmxPlane << 2) | 3;
    ESPectrum::trdos = true;   // Beta on; check_trdos holds DOS while D2 is set
    MemESP::ramCurrent[0] = MemESP::rom[MemESP::romInUse].direct();
    gmxTapUpdate();
#if GMX_TRACE
    if (MemESP::romInUse != gmxt_prev)
      GMXT("[GMX romU] %u->%u D2-hold 1FFD=%02X plane=%u pc=%04X",
           gmxt_prev, (unsigned)MemESP::romInUse, port1FFD, gmxPlane, Z80::getRegPC());
#endif
    return;
  }
  uint8_t bank = (port1FFD & 0x02) ? 2
               : ((((uint8_t)ESPectrum::trdos) << 1) | MemESP::romLatch);
  MemESP::romInUse = (g_scorp_gmx ? (gmxPlane << 2) : 0) | bank;
  MemESP::recoverPage0();
  gmxTapUpdate();
#if GMX_TRACE
  if (MemESP::romInUse != gmxt_prev)
    GMXT("[GMX romU] %u->%u 1FFD=%02X dos=%d rom14=%u plane=%u pc=%04X",
         gmxt_prev, (unsigned)MemESP::romInUse, port1FFD, (int)ESPectrum::trdos,
         (unsigned)MemESP::romLatch, gmxPlane, Z80::getRegPC());
#endif
}

// MAME scorpiontb prof_plane_map — the legacy ProfROM plane-switch table driven
// by reads of 0x0100/4/8/C from inside the service bank; clamps to planes 0-3
// even on GMX (the full 0-7 range is reachable only via #7EFD D4-6).
static const uint8_t kProfPlaneMap[16] = {
    0, 1, 2, 3,
    3, 3, 3, 2,
    2, 2, 0, 1,
    1, 0, 1, 0,
};

// Called from Z80Ops::peek8/fetchOpcode when g_gmx_tap is armed and the address
// is 0x0100-0x010F (ZXMAK2 subscribes the whole 16-byte window; the plane slot
// is addr bits 2-3). Out of line — the armed case is rare.
void Ports::gmxProfRomTap(uint16_t address) {
  uint8_t plane = kProfPlaneMap[(address & 0x0C) | (gmxPlane & 0x03)];
  if (plane != gmxPlane) {
    gmxPlane = plane;
    scorpionRomUpdate();
  }
}

void Ports::gmxTapRecheck() { gmxTapUpdate(); }

// ── Scorpion GMX port family (MAME sinclair/scorpion.cpp scorpiongmx) ────────
// Deliberately NOT IRAM: called from the RAM-resident Ports::output/input only
// while g_scorp_gmx, and the register file is not on any hot path — keeping the
// bodies in flash saves ~1 KB of the RAM code budget.
bool Ports::gmxPortWrite(uint16_t address, uint8_t data) {
  if ((address & 0x00FF) == 0) {
    // Port #00 global config: D5=BLKEXT (GMX register file off), D4=fixrom
    // (freeze the ProfROM plane), D3 arms the magic shift-register readout
    // 0x88|(D0-2) and, with fixrom off, pulses CPU reset — the GMX "magic
    // jump" into the boot ROM (MAME global_cfg_w).
    gmxPort00 = data;
#if GMX_TRACE
    GMXT("[GMX p00] %02X blkext=%d fixrom=%d magic=%d pc=%04X",
         data, (int)((data >> 5) & 1), (int)((data >> 4) & 1),
         (int)((data >> 3) & 1), Z80::getRegPC());
#endif
    if (data & 0x08) {
      gmxMagicShift = 0x88 | (data & 0x07);
      if (!(data & 0x10)) {
#if GMX_TRACE
        GMXT("[GMX p00] magic reset shift=%02X (CPU only)", gmxMagicShift);
#endif
        Z80::reset();   // CPU only — RAM/paging stay
      }
    }
    return true;
  }
  if (gmxPort00 & 0x20) return false;     // BLKEXT → register file off
  switch (address) {                      // full 16-bit decode (MAME mirror 0)
    case 0x78FD: {
      // RAM page at 0x8000 (CPU bank 2): page = value ^ 2, so 0 = the
      // default page 2. Full 7-bit page number (2 MB).
      gmxPort78FD = data & 0x7F;
      uint32_t pg = gmxPort78FD ^ 2;
      uint32_t pages = ram_pages + butter_pages + psram_pages + swap_pages;
      if (pg >= pages) pg = 2;
      MemESP::ramCurrent[2] = MemESP::ram[pg].sync(2);
      MemESP::ramContended[2] = false;
      LED::touchW(LED::RAM);
      return true;
    }
    case 0x7AFD: gmxScrollLo = data & 0xF0; return true;  // 640x200 v-scroll
    case 0x7CFD: gmxScrollHi = data & 0x3F; return true;
    case 0x7EFD: {
#if GMX_TRACE
      if (data != gmxPort7EFD)
        GMXT("[GMX 7EFD] %02X plane=%u gfx=%d turbo=%d pc=%04X",
             data, (unsigned)((data >> 4) & 7), (int)((data >> 3) & 1),
             (int)((data >> 7) & 1), Z80::getRegPC());
#endif
      gmxPort7EFD = data;
      // D7 turbo (7 MHz) — honored only while the USER has turbo on, same
      // policy as Pentagon-1024SL #EFF7 D4 (the GMX boot ROM flips it at
      // will and must not turbo a 3.5 MHz session).
      if (ESPectrum::multUser) {
        uint8_t want = (data & 0x80) ? ESPectrum::multUser : 0;
        if (want != ESPectrum::multiplicator) {
          ESPectrum::multiplicator = want;
          CPU::updateStatesInFrame();
        }
      }
      // D4-6 = ProfROM plane (28F400 A16-18), frozen by fixrom (port #00 D4)
      if (!(gmxPort00 & 0x10)) {
        uint8_t plane = (data >> 4) & 0x07;
        if (plane != gmxPlane) {
          gmxPlane = plane;
          scorpionRomUpdate();
        }
      }
      // D3 = gfx_ext 640x200x16 — applied in vblank (EndFrame), the driver
      // palette tables must never be rewritten mid-scanout (DS80 precedent)
      VIDEO::gmxExtRequest((data & 0x08) != 0);
      // D2 = magic_disabled, D1 = Vpp, D0 = EWR (28F400 flash write) — ignored
      return true;
    }
    case 0xDFFD:
      // 3 extra RAM-page bits for the 0xC000 window ((dffd&7)<<4 → 2 MB)
      portDFFDgmx = data & 0x07;
      MemESP::bankLatch = scorpionC000Page(MemESP::bankLatch & 0x07);
      MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3);
      MemESP::ramContended[3] = false;
      LED::touchW(LED::RAM);
      return true;
    default: return false;
  }
}

// GMX register read-backs (live state; the MAME magic-lock snapshots are not
// modelled — no GMX Magic/NMI button in this port).
bool Ports::gmxPortRead(uint16_t address, uint8_t* out) {
  if (gmxPort00 & 0x20) return false;     // BLKEXT → register file off
  if (address == 0x78FD) {
    // BRD1 (last #FE bit1) | RAM-at-0x8000 page | magic shift-register bit 0
    *out = (uint8_t)((port254 & 0x02) << 6) | (gmxPort78FD & 0x7F) | (gmxMagicShift & 0x01);
    gmxMagicShift >>= 1;
    return true;
  }
  if (address == 0x7AFD) {
    // BRD0 | the composed 0xC000 paging state: DFFD<<4 | 1FFD.D4<<3 | 7FFD 0-2
    *out = (uint8_t)((port254 & 0x01) << 7)
         | (uint8_t)((portDFFDgmx & 0x07) << 4)
         | (uint8_t)((port1FFD & 0x10) >> 1)
         | (uint8_t)(MemESP::bankLatch & 0x07);
    return true;
  }
  if (address == 0x7EFD) {
    // BRD2 | 1FFD.D0(RAM0)<<6 | BLKEXT<<5 | port00.D7<<4 | gfx<<3 | turbo<<2
    //      | videoLatch<<1 | pagingLock
    *out = (uint8_t)((port254 & 0x04) << 5)
         | (uint8_t)((port1FFD & 0x01) << 6)
         | (uint8_t)(gmxPort00 & 0x20)
         | (uint8_t)((gmxPort00 & 0x80) >> 3)
         | (uint8_t)(gmxPort7EFD & 0x08)
         | (uint8_t)((gmxPort7EFD & 0x80) >> 5)
         | (uint8_t)((MemESP::videoLatch & 1) << 1)
         | (uint8_t)(MemESP::pagingLock & 1);
    return true;
  }
  return false;
}

IRAM_ATTR void Ports::output(uint16_t address, uint8_t data) {
  int Audiobit;
#if SND_PORT_TRACE
  sndTraceWr[address & 0xFF]++;
  sndTraceLastVal[address & 0xFF] = data;
#endif
  if (Config::numPortWriteBP > 0 && Config::hasBreakPoint(address, Config::BP_PORT_WRITE))
    CPU::portBasedBP = true;
  uint8_t rambank = address >> 14;
#if FDD_PORT_TRACE
  // Unconditional probe — see the matching read-side comment in Ports::input.
  if (Z80Ops::isProfi) {
    uint8_t lo8f = address & 0xFF;
    if (lo8f == 0x1F || lo8f == 0x3F || lo8f == 0x5F || lo8f == 0x7F ||
        lo8f == 0x83 || lo8f == 0xA3 || lo8f == 0xC3 || lo8f == 0xE3 ||
        lo8f == 0xFF || lo8f == 0xBF) {
      bool has_any_disk_p = ESPectrum::fdd.disk[ESPectrum::fdd.diskS] != nullptr;
      bool skip_p = (MemESP::romInUse == 0 && !has_any_disk_p);
      uint16_t pcNow = Z80::getRegPC();
      // Same dedupe as the read-side probe — see comment in Ports::input. For
      // OUT (n),A the addr high byte IS the data byte, so a delay-loop counter
      // written repeatedly from the same pc rotates addr/data together; dedupe
      // on (pc, lo byte, decode state) and show the data range while it repeats.
      static uint16_t lastPcOut = 0xFFFF;
      static uint8_t lastLoOut = 0xFF, loDataOut = 0, hiDataOut = 0;
      static uint8_t lastCpmOut = 0xFF, lastRom14Out = 0xFF, lastTrdosOut = 0xFF, lastRomInUseOut = 0xFF;
      static bool lastDiskOut = false, lastSkipOut = false;
      static uint32_t repOut = 0;
      bool cpmNow = (portDFFD & 0x20) != 0;
      if (pcNow == lastPcOut && lo8f == lastLoOut && cpmNow == lastCpmOut &&
          MemESP::romLatch == lastRom14Out && ESPectrum::trdos == lastTrdosOut &&
          MemESP::romInUse == lastRomInUseOut && has_any_disk_p == lastDiskOut && skip_p == lastSkipOut) {
        repOut++;
        if (data < loDataOut) loDataOut = data;
        if (data > hiDataOut) hiDataOut = data;
      } else {
        if (repOut)
          Debug::log("[FDC OUT probe] pc=%04X lo=%02X data %02X..%02X (repeated x%u)",
                     lastPcOut, lastLoOut, loDataOut, hiDataOut, repOut);
        repOut = 0;
        loDataOut = hiDataOut = data;
        lastPcOut = pcNow; lastLoOut = lo8f; lastCpmOut = cpmNow; lastRom14Out = MemESP::romLatch;
        lastTrdosOut = ESPectrum::trdos; lastRomInUseOut = MemESP::romInUse;
        lastDiskOut = has_any_disk_p; lastSkipOut = skip_p;
        Debug::log("[FDC OUT probe] addr=%04X lo=%02X data=%02X cpm=%d rom14=%d trdos=%d romInUse=%d disk=%d skip=%d pc=%04X",
                   address, lo8f, data, cpmNow, MemESP::romLatch,
                   ESPectrum::trdos, MemESP::romInUse, has_any_disk_p, skip_p, pcNow);
      }
    }
  }

  // SPI-flash port probe (write side) — see matching comment in Ports::input.
  if (Z80Ops::isProfi) {
    uint8_t lo8spi = address & 0xFF;
    if (lo8spi == 0xC7 || lo8spi == 0x87 || lo8spi == 0xA7 || lo8spi == 0xE7 || lo8spi == 0x67) {
      static uint32_t spiOutCnt = 0;
      if (spiOutCnt < 200) {
        spiOutCnt++;
        Debug::log("[SPI-FLASH OUT] addr=%04X lo=%02X data=%02X cpm=%d rom14=%d ds80=%d pc=%04X",
                   address, lo8spi, data, (portDFFD >> 5) & 1, (int)MemESP::romLatch,
                   (portDFFD >> 7) & 1, Z80::getRegPC());
      }
    }
  }
#endif
  // Profi dynamic palette (#7E): per ZXMAK2 UlaProfi5XX.WritePortFE / hardware
  // docs, any OUT with (address & 0x0081) == 0 (CS: A0=0, A7=0) is a palette
  // write:
  //   index = (port254 XOR 0x0F) & 0x0F               (last BORDER nibble)
  //   color = ~(address >> 8), decoded GX2:0|RX2:0|BX2:1 (3-3-2).
  // GX0 (bit5 of color) is latched for PAL_DETECT regardless of DS80 state —
  // real hardware self-test can probe the palette IC before DS80 video mode is
  // engaged. The actual RGB store (now 3-3-3 via profi_bx0_latch, see
  // profiPaletteWrite) only applies once DS80 is active, to avoid corrupting
  // defaults from incidental #7E-pattern writes during BIOS startup.
  if (Z80Ops::isProfi && (address & 0x0081) == 0) {
    uint8_t index = (port254 ^ 0x0F) & 0x0F;
    uint8_t color = ~(uint8_t)(address >> 8);
    VIDEO::profi_gx0_latch = (color >> 5) & 1;
    if (portDFFD & 0x80)
      VIDEO::profiPaletteWrite(index, color);
  }

  if (Z80Ops::isByte && address >= 0xC000) {
    // вместо VIDEO::Draw(1, MemESP::ramContended[rambank]);
    // добавляем задержку через таблицу MemESP
    int delay = MemESP::getByteContention(address);
    VIDEO::Draw(delay, true);
  } else {
    // Early contention depends on ADDRESS only (contended memory?), not port type.
    // Wiki: ULA port non-contended addr = N:1,C:3; contended addr = C:1,C:3
    //       Non-ULA contended addr = C:1,C:1,C:1,C:1; non-contended = N:4
    // Matches Ports::input behavior for symmetry.
    VIDEO::Draw(1, MemESP::ramContended[rambank]); // I/O Contention (Early)
  }
  uint8_t a8 = (address & 0xFF);
  p_states = CPU::tstates;

  // «Байт»: any access to the Kempston-decoded port (#1F/#9F) toggles the
  // DD71 доп. ПЗУ overlay (the built-in test's switch stub at #387A is
  // IN A,(#9F); RET). In TR-DOS #1F belongs to the FDC. Side effect only —
  // the write itself has no other target here.
  if (Z80Ops::isByte && !ESPectrum::trdos && (a8 & 0x7F) == 0x1F)
    Config::byteTestRomToggle();

  // ZiFi NIC port: A0..A7 == 0xEF, A8..A15 selects register (0x00..0xC7)
  // 0xEFF7 (hi=0xEF > 0xC7) falls through to Pentagon mode16col handler below
  if (Config::zifi_enabled && a8 == 0xEF) {
    uint8_t zifi_hi = address >> 8;
    if (zifi_hi <= 0xC7) {
      ZiFi::write(zifi_hi, data);
      return;
    }
    if (zifi_hi >= 0xF8) { // 16550 UART window (#F8EF..#FFEF) — raw-UART drivers
      ZiFi::uart16550Write(zifi_hi, data);
      return;
    }
  }
  // ZX UNO register file (#FC3B address / #FD3B data) — Karabas-Pro's UART
  // bridge to its on-board ESP8266. Full 16-bit decode (as on the FPGA), bit8
  // picks the data port; bridges to the same ESP link as the #xxEF windows.
  if (Config::zifi_enabled && (address | 0x0100) == 0xFD3B) {
    ZiFi::unoUartWrite(address & 0x0100, data);
    return;
  }
  // Scorpion GMX port family — cold flash-resident dispatch, see gmxPortWrite.
  // Placed BEFORE the ULA and #7FFD blocks on purpose: port #00 is even (the
  // ULA branch would repaint the border with config bytes — the Scorpion PAL
  // decodes ULA as A5=1&A1=1&A0=0, so #00 never reaches it on hardware), and
  // #78FD/#7AFD/#7CFD/#7EFD have A15=0/A1=0 (the loose 7FFD gate would eat
  // them as paging writes).
  if (g_scorp_gmx && gmxPortWrite(address, data)) return;
  // MC146818 RTC (Pentagon/Profi "Mr Gluk" TimeKeeper):
  //   OUT (#DFF7), reg  → latch register index
  //   OUT (#BFF7), data → write selected register
  if ((Z80Ops::isPentagon || Z80Ops::isProfi)) {
#if RTC_PORT_TRACE
    if (a8 == 0xF7) {
      static uint32_t out_n = 0;
      if (++out_n <= 150 || (out_n & 0xFF) == 0)
        Debug::log("[RTC OUT] %04X <- %02X pc=%04X eff7=%02X n=%u",
                   address, data, Z80::getRegPC(), Ports::portEFF7, (unsigned)out_n);
    }
#endif
    // Register-select is latched even when the RTC is off, so a subsequent read
    // returns the right static value (RTC::readDisabled). Data writes only take
    // effect when enabled — disabled = "ports don't act" but still respond.
    if (address == 0xDFF7) { RTC::selectReg(data); return; }
    if (address == 0xBFF7) { if (Config::rtc_enabled) RTC::writeData(data); return; }
  }
  // Karabas-Pro's own native RTC ports (#FF/#BF AS, #DF/#9F DS) are handled
  // LATER in this function, after the Beta-128/FDC write switch — see the
  // read-side comment in Ports::input for why (FDC must get first refusal).

  if (address == 0xAFF7) {
    LED::touchW(LED::RAM);
    uint8_t prev = portAFF7;
    uint8_t d6 = data & 0b00111111; // limit it for 64 planes
    if (prev != d6) {
      portAFF7 = d6;
      if (!MemESP::pagingLock) {
        size_t zxPages = extendedZxRamPages();
        uint32_t page = MemESP::bankLatch + d6 * zxPages - prev * zxPages;
        uint32_t pages = ram_pages + butter_pages + psram_pages + swap_pages;
        if (page < pages) { // W/A: protection of incorrect page selection logic
          MemESP::bankLatch = page;
          MemESP::ramCurrent[3] = MemESP::ram[page].sync(3);
          MemESP::ramContended[3] =
              (Z80Ops::isPentagon || Z80Ops::isProfi) ? false : (page & 0x01 ? true : false);
        }
      }
    }
  }

  // Profi extended paging port 0xDFFD
  // bits [2:0]: upper RAM page group (combined with 0x7FFD bits[2:0] → 8 groups × 8 pages)
  // bit [4]: map bank0 to RAM page 0 (else ROM)
  // bit [5]: DOS ports / TR-DOS enable (handled by existing TR-DOS mechanism)
  // bit [6]: map bank2 to page 6
  // bit [7]: hires video mode — screen at RAM page 4/6 instead of 5/7
  if (Z80Ops::isProfi && address == 0xDFFD) {
    ++Ports::portdffd_cnt;
#if FDD_PORT_TRACE
    checkPagingStuck(Z80::getRegPC());
#endif
    LED::touchW(LED::RAM);
    // Per ZXMAK2 MemoryProfi1024: DFFD writes are NOT gated by paging lock.
    // norom (bit 4) clears lock unconditionally.
    {
      uint8_t prev_page0ram = MemESP::page0ram;
#if PROFI_PORT_TRACE
      static uint8_t prev_dffd = 0xFE;
      if (prev_dffd != data) {
        Debug::log("[DFFD] new=0x%02X DS80=%d CPM=%d NOROM=%d SCO=%d SCR=%d page2..0=%d pc=0x%04X rom14=%d trdos=%d",
                   data, (data >> 7) & 1, (data >> 5) & 1, (data >> 4) & 1,
                   (data >> 3) & 1, (data >> 6) & 1, data & 7, Z80::getRegPC(),
                   (int)MemESP::romLatch, (int)ESPectrum::trdos);
        prev_dffd = data;
      }
#endif
      portDFFD = data;
      MemESP::page0ram = bitRead(data, 4);
      if (MemESP::page0ram) MemESP::pagingLock = false; // norom → unlock
      if (MemESP::page0ram != prev_page0ram)
        MemESP::recoverPage0();
      // SCR (bit6): bank2 → page6 (else page2)
      uint8_t bank2_page = bitRead(data, 6) ? 6 : 2;
      MemESP::ramCurrent[2] = MemESP::ram[bank2_page].sync(2);
      // Re-apply bankLatch with new extended group offset
      uint32_t page = (MemESP::bankLatch & 0x7) + ((data & 0x7) << 3);
      uint32_t pages = ram_pages + butter_pages + psram_pages + swap_pages;
      if (page < pages) {
        MemESP::bankLatch = page;
        MemESP::ramContended[3] = false;
      }
      // SCO (bit3): per ZXMAK2 UpdateMapping —
      //   sco=0: MapRead4000 = RAM[5];       MapReadC000 = RAM[ramPage]  ← std 128K
      //   sco=1: MapRead4000 = RAM[ramPage];  MapReadC000 = RAM[7]       ← Profi extended
      if (bitRead(data, 3)) {
        MemESP::ramCurrent[1] = MemESP::ram[MemESP::bankLatch].sync(1);
        MemESP::ramCurrent[3] = MemESP::ram[7].sync(3);
      } else {
        MemESP::ramCurrent[1] = MemESP::ram[5].direct();
        MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3);
      }
#if PROFI_PORT_TRACE
      // Log when a DS80 video page (4/6/56/58) is mapped into the Z80 address space.
      {
        uint32_t bl = MemESP::bankLatch;
        if (bl == 4 || bl == 6 || bl == 56 || bl == 58) {
          bool vl = MemESP::videoLatch;
          bool sco = bitRead(data, 3);
          // slot: SCO=1 → bankLatch at 0x4000 (slot1); SCO=0 → bankLatch at 0xC000 (slot3)
          char slot = sco ? '1' : '3';
          // Is this the DISPLAY page (currently being rendered from)?
          bool disp = (!vl && (bl == 4 || bl == 56)) || (vl && (bl == 6 || bl == 58));
          Debug::log("[DFFD] bl=%u slot%c vl=%u %s PC=%04X",
              bl, slot, vl, disp ? "DISPLAY-PAGE!" : "write-buf", Z80::getRegPC());
        }
      }
#endif
      // bit7: hires mode switches screen pages 5/7 → 4/6; color attrs from pages 58/56
      if (data & 0x80) {
        VIDEO::grmem     = MemESP::videoLatch ? MemESP::ram[6].direct()  : MemESP::ram[4].direct();
        uint32_t clrPage = MemESP::videoLatch ? 58 : 56;
        uint32_t totPages = ram_pages + butter_pages + psram_pages + swap_pages;
        VIDEO::profi_clrmem = (clrPage < totPages) ? MemESP::ram[clrPage].direct() : nullptr;
        // Debug::log("[DFFD] DS80 on: clrPage=%u tot=%u clrmem=%p grmem=%p", clrPage, totPages, VIDEO::profi_clrmem, VIDEO::grmem);
        // DEFERRED: hdmi_set_profi_ds80_mode() writes conv_color[] which the HDMI
        // DMA reads in real time.  Calling it here (Z80 loop, core0, active scan)
        // races the DMA on core1 → TMDS corruption → picture disappears.
        // Set a flag; EndFrame() (always at vblank) will apply it safely.
        // Guard: only set pending if neither mode is already active/pending.
        extern volatile bool profi_ds80_active;
        if (!profi_ds80_active && !VIDEO::profi_ds80_activate_pending) {
            VIDEO::profi_ds80_deactivate_pending = false; // cancel any pending off
            VIDEO::profi_ds80_activate_pending   = true;
        } else if (profi_ds80_active) {
            // DS80 already active — cancel any spurious deactivation queued by a
            // preceding bit7=0 write in the same Z80 frame (e.g. sea-viewer does
            // OUT (#FD),0x00  ; "reset" portDFFD before reprogramming banks
            // OUT (#FD),0x80  ; re-enable DS80
            // Without this cancel, EndFrame would see deactivate_pending=true and
            // tear down DS80 for one frame → black flash / flicker.
            if (VIDEO::profi_ds80_deactivate_pending) {
                VIDEO::profi_ds80_deactivate_pending = false;
            }
        }
        VIDEO::updateBorderBrd();
      } else {
        VIDEO::grmem        = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();
        VIDEO::profi_clrmem = nullptr;
        // DEFERRED: same race condition — defer deactivation to EndFrame vblank.
        extern volatile bool profi_ds80_active;
        bool exiting_ds80 = profi_ds80_active || VIDEO::profi_ds80_activate_pending;
        if (exiting_ds80) {
            VIDEO::profi_ds80_activate_pending   = false; // cancel any pending on
            VIDEO::profi_ds80_deactivate_pending = true;
            // Reset border to white (standard ZX boot default) when leaving DS80
            VIDEO::borderColor = 7;
        }
        VIDEO::updateBorderBrd();
        // Fill framebuffer with BLACK (0) when leaving DS80.
        //
        // Why not WHITE (7)?  The deferred deactivation flag (profi_ds80_deactivate_pending)
        // means HDMI ISR is STILL in DS80 mode when this fill runs — EndFrame hasn't
        // processed the flag yet.  In DS80 mode, byte 7 = slot profi_pair_lookup[0][7]
        // = pair(black, white) → alternating pixels → fine vertical gray stripes on the
        // border areas (bytes 0..pad_l-1 and pad_l+256..xres-1 are NOT overwritten by the
        // DS80 scan-time renderer, so they stay at 7 until EndFrame clears them).
        //
        // Byte 0 is safe in both modes:
        //   DS80:     slot 0 = pair(0,0) = black/black → solid black ✓
        //   Standard: palette index 0 = BLACK ✓
        // The border scanner fires after EndFrame deactivates DS80 and writes the correct
        // border color (white/default), so the first full standard frame looks correct.
        if (exiting_ds80 && VIDEO::vga.frameBuffer) {
          for (int y = 0; y < (int)VIDEO::vga.yres; y++)
            if (VIDEO::vga.frameBuffer[y]) memset(VIDEO::vga.frameBuffer[y], 0, VIDEO::vga.xres);
        }
      }
    }
  }

  // Port #EFF7 — extended-feature register (per UnrealSpeccy emul.h):
  //   D0 (0x01) = EFF7_4BPP      — 4-bit-per-pixel mode
  //   D1 (0x02) = EFF7_512       — 512-pixel hires mode (Profi CP/M)
  //   D2 (0x04) = EFF7_LOCKMEM
  //   D3 (0x08) = EFF7_ROCACHE
  //   D4 (0x10) = EFF7_GIGASCREEN — MISNAMED in emul.h: on Pentagon-1024SL it
  //               is TURBO OFF (pentevo io.cpp: turbo(pEFF7&0x10 ? 1 : 2));
  //               handled in the dedicated #EFF7 paging handler further down
  //   D5 (0x20) = EFF7_HWMC      — hardware multicolor
  //   D6 (0x40) = EFF7_384       — 384-line video
  //   D7 (0x80) = EFF7_CMOS      — CMOS RTC enable
  if ((Z80Ops::isPentagon || Z80Ops::isProfi) && address == 0xEFF7) {
    // Debug::log("[EFF7] pc=0x%04X data=0x%02X (4BPP=%d 512=%d LOCK=%d GIGA=%d HWMC=%d CMOS=%d)",
    //            Z80::getRegPC(), data, !!(data & 0x01), !!(data & 0x02), !!(data & 0x04),
    //            !!(data & 0x10), !!(data & 0x20), !!(data & 0x80));
    portEFF7 = data;
    // (page0ram/notMore128 from bits 2-3 are handled by the dedicated #EFF7
    // paging handler further down — single owner, don't duplicate here.)
    // Pentagon 16col (EFF7 D0, speccy.info "Порт EFF7"). On Pentagon-1024SL
    // the bit is real hardware, so it is honored there unconditionally — the
    // menu's "16 colours" toggle only matters for the other Pentagons (where
    // it deliberately lets a user grant the mode to software written for an
    // SL). The 512 B decode LUT is allocated lazily on first guest enable;
    // Video Reset / a machine switch away from Pentagon frees it as before.
    if (Config::mode16col_onoff || Z80Ops::is1024) {
      bool want = (data & 0x01) != 0;
      if (want != VIDEO::mode16col_enabled) {
        if (want) {
          VIDEO::ensure16colLut();
          VIDEO::mode16colUpdatePlanes();
        }
        VIDEO::mode16col_enabled = want;
      }
    }
  }

  bool ia = Z80Ops::isALF;
  if (ia) {
    if (a8 == 0xFE) {
      newAlfBit = (data >> 3) & 1;
    }
    if (bitRead(address, 7) == 0 &&
        (address & 1) == 1) { // ALF ROM selector A7=0, A0=1
      bool cart = bitRead(data, 7);
      MemESP::romInUse = (data & 0b01111111);
      while (MemESP::romInUse >= 64)
        MemESP::romInUse -= 64; // rolling ROM
      if (cart && AlfCart::active()) {
        // Lazy SD cartridge: fault the selected 16K bank into the window on demand
        // (like wd1793 faults a sector). Cart ROM is only ever visible at page 0, so
        // binding just the selected bank suffices. Banks past the image = open bus.
        int b = MemESP::romInUse;
        MemESP::rom[b].assign_rom(b < AlfCart::bankCount()
                                    ? AlfCart::residentBank(b) : gb_rom_Alf_ep);
      } else if (cart) {
        // Cart selected but none mounted: empty drive (open bus), like TR-DOS w/o disk.
        MemESP::rom[MemESP::romInUse].assign_rom(gb_rom_Alf_ep);
      } else {
        // System ROM (gb_rom_Alf, 32KB = 2 banks in flash); banks 2+ → open-bus zeros.
        if (MemESP::ramCurrent[0] != gb_rom_Alf) {
          for (int i = 0; i < 64; ++i)
            MemESP::rom[i].assign_rom(i >= 2 ? gb_rom_Alf_ep
                                             : gb_rom_Alf + ((16 * i) << 10));
        }
      }
      MemESP::recoverPage0();
      // ALF uses incomplete decoding (A7=0, A0=1) for the bank latch, so the
      // same OUT also hits MB-02 FDC (#0F/#2F/#4F/#6F), DMA (#0B/#6B), Beta-128
      // and other A7=0 odd-port peripherals. Take the bank-select exclusively.
      ioContentionLate(MemESP::ramContended[rambank]);
      return;
    }
  }
  // IDE/HDD — NEMO scheme. Enabled on ANY machine when the user selects NEMO
  // (bus card, not machine-specific). Decoded BEFORE the ULA even-port branch
  // (NEMO register ports have A0=0). 16-bit data via A0 latch. On Profi the
  // SYSEN line keeps ESPectrum::trdos permanently asserted, so the !trdos rule
  // (authentic NEMO is outside TR-DOS) is bypassed there.
  if (IDE::scheme == IDE::NEMO && !(address & 6) && (Z80Ops::isProfi || !ESPectrum::trdos)) {
    if (address & 1) { LED::touchW(LED::IDE); IDE::write_latch(data); return; } // A0=1: high latch
    if ((address & 0x18) == 0x08 && (address & 0xE0) == 0xC0) {                // control
      LED::touchW(LED::IDE); IDE::write8(8, data); return;
    }
    if ((address & 0x18) == 0x10) {                                           // register window
      LED::touchW(LED::IDE);
      uint8_t reg = (address >> 5) & 7;
      if (reg == 0) IDE::write_data_low(data); else IDE::write8(reg, data);
      return;
    }
    // else: not an IDE sub-address — fall through (don't shadow AY/ULA etc.)
  }
  // ULA =======================================================================
  if ((address & 0x0001) == 0) {
    // KR580VI53 (8253 PIT) — Byte computer synthesizer at #8E/#AE/#CE (data)
    // and #EE (control). The Byte fully decodes its I/O ports (unlike the ZX
    // ULA's bare A0=0 decode), so timer writes must NOT fall through to the
    // border/beeper latch: the built-in ROM test's melody phase reprograms the
    // timer per note (OUT (#EE) + two OUT (C) data writes) and the border
    // would flicker red/white with beeper clicks on top of the melody.
    if (Z80Ops::isByte && (a8 & 0x9F) == 0x8E) {
      pitWrite(a8, data);
      VIDEO::Draw(3, !(Z80Ops::isPentagon || Z80Ops::isProfi)); // I/O Contention (Late)
      return;
    }
    // The Byte fully decodes its output ports: only #FE reaches the
    // border/beeper latch (that full decode is a documented Byte trait). The
    // built-in test relies on it — its OUT (0),A phase markers and the RAM-
    // error loop's OUT (C),A to #0F must not repaint the border: the page
    // documents the border staying yellow through the RAM test after a
    // successful ROM checksum.
    if (Z80Ops::isByte && a8 != 0xFE) {
      VIDEO::Draw(3, !(Z80Ops::isPentagon || Z80Ops::isProfi)); // I/O Contention (Late)
      return;
    }
    port254 = data;
    // BX0 (blue LSB of the 3:3:3 palette) is port #FE bit7 — latched here for
    // profiPaletteWrite() and for the PAL_DETECT read-back self-test (Ports::input).
    if (Z80Ops::isProfi)
      VIDEO::profi_bx0_latch = (data >> 7) & 1;
    // Border color
#if FDD_PORT_TRACE
    // Red border (=2) is a classic ZX error indicator. If PQDOS/TRDBOOT sets it
    // from an error handler, this PC pinpoints WHICH error the boot hits. Log
    // every distinct border-colour change on Profi so we see the error signal.
    if (Z80Ops::isProfi) {
      static uint8_t prevBorder = 0xFF;
      if ((data & 0x07) != prevBorder) {
        prevBorder = data & 0x07;
        Debug::log("[BORDER] col=%u pc=%04X romU=%u", data & 0x07, Z80::getRegPC(), (unsigned)MemESP::romInUse);
      }
    }
#endif
    // Compare the 3-bit colour only: borderColor stores data & 0x07, so an
    // unmasked compare fires on every beeper/MIC bit change (bits 3-4) and on
    // OTIR/OTDR garbage bytes — each false hit runs a full DrawBorder catch-up
    // and re-arms brdChange (whole-border repaint) for no visual change.
    // Found via FPGA48_2026.tap: its OTDR section writes arbitrary bytes to #FE.
    if (VIDEO::borderColor != (data & 0x07)) {
      VIDEO::brdChange = true;
      if (!(Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion))
        // VIDEO::Draw(0, false); // Flush video rendering without adding contention
        VIDEO::Draw(0, true); // Apply contention to align border change with ULA character cell
      VIDEO::DrawBorder();
      VIDEO::borderColor = data & 0x07;
      if (VIDEO::ulaplus_enabled)
        VIDEO::ulaPlusUpdateBorder();
      else
        VIDEO::updateBorderBrd();
    }
    if (Config::tape_player)
      Audiobit = Tape::tapeEarBit ? 255 : 0; // For tape player mode
    else
      // Beeper Audio
      Audiobit = speaker_values[((data >> 2) & 0x04) | (Tape::tapeEarBit << 1) |
                                ((data >> 3) & 0x01)];
    if (Audiobit != ESPectrum::lastaudioBit) {
      ESPectrum::BeeperGetSample();
      ESPectrum::lastaudioBit = Audiobit;
      LED::touchW(LED::BEEPER);
    }
    // AY
    // ========================================================================
    if ((ESPectrum::AY_emu) && ((address & 0x8002) == 0x8000)) {
      LED::touchW(LED::AY);
      ayPortWrite(address, data, true);     // A8 decode: old-TS second chip
      VIDEO::Draw(3, !(Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion)); // I/O Contention (Late)
      return;
    }
    VIDEO::Draw(3, !(Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion)); // I/O Contention (Late)
  } else {
    // ULA+ ports (odd addresses: 0xBF3B register select, 0xFF3B data)
    if (Config::ulaplus) {
      if (address == 0xBF3B) {
        LED::touchW(LED::ULAPLUS);
        VIDEO::ulaplus_reg = data;
        ioContentionLate(MemESP::ramContended[rambank]);
        return;
      }
      if (address == 0xFF3B) {
        LED::touchW(LED::ULAPLUS);
        uint8_t reg = VIDEO::ulaplus_reg;
        if ((reg & 0xC0) == 0x00) {
          // Palette group write
          VIDEO::ulaplus_palette[reg & 0x3F] = data;
          if (VIDEO::ulaplus_enabled) {
            VIDEO::ulaPlusUpdatePaletteEntry(reg & 0x3F);
            if ((reg & 0x3F) == (8 + VIDEO::borderColor))
              VIDEO::ulaPlusUpdateBorder();
          }
        } else if ((reg & 0xC0) == 0x40) {
          // Mode group write
          bool new_on = data & 0x01;
          if (new_on && !VIDEO::ulaplus_enabled) {
            VIDEO::ulaplus_enabled = true;
            VIDEO::flashing = 0;
            // Defer heavy AluByte/palette rebuild to EndFrame so it runs during
            // HDMI blanking and not from inside Z80 port-write context
            VIDEO::ulaplus_alubytes_dirty = true;
            VIDEO::ulaPlusUpdateBorder();
          } else if (!new_on && VIDEO::ulaplus_enabled) {
            VIDEO::ulaPlusDisable();
          }
        }
        ioContentionLate(MemESP::ramContended[rambank]);
        return;
      }
    }
    // Covox #FB: on real Profi this is an external DAC decoding only A7:0=#FB,
    // NOT gated by CPM — Profi CP/M games (Single Warrior) stream samples to #FB
    // with DFFD bit5 set. (Karabas-Pro gates its internal Covox by DOS=0&CPM=0,
    // but that manual doesn't apply to Profi.)
    int covox = Config::covox;
    if ((covox == 1 && a8 == 0xFB) || (covox == 2 && a8 == 0xDD)) {
      LED::touchW(LED::COVOX);
      ESPectrum::lastCovoxVal = data;
      ESPectrum::lastCovoxValR = data;
      ESPectrum::CovoxGetSample();
    }
    // SounDrive: five 8-bit DAC latches — #0F/#1F/#3F mix left, #4F/#5F right,
    // #FB both (Karabas-Pro manual p.36). Config::soundrive: 1=On, 2=Auto
    // (Profi only). The ports are shared with the WD1793: real hardware gates
    // SounDrive CS by DOS=0, and Profi CP/M periphery mode (DFFD bit5) decodes
    // the FDC there too — so the ports act as DACs only outside both modes.
    // Single Warrior loads its disk with CPM=1, then streams 7.6 kHz menu PCM
    // to #3F/#5F with CPM=0 and trdos=0. Stereo: left/right latch groups go to
    // the L/R covox buffers; return so the writes never reach the FDC block
    // (out_has_raw_disk would route them to WD1793 regs).
    else if ((Config::soundrive == 1 ||
              (Config::soundrive == 2 && Z80Ops::isProfi)) &&
             !ESPectrum::trdos && !(Z80Ops::isProfi && (portDFFD & 0x20))) {
      int8_t slot = -1;
      switch (a8) {
        case 0x0F: slot = 0; break;
        case 0x1F: slot = 1; break;
        case 0x3F: slot = 2; break;
        case 0x4F: slot = 3; break;
        case 0x5F: slot = 4; break;
        case 0xFB: slot = 5; break;
      }
      if (slot >= 0) {
        sndriveLatch[slot] = data;
        sndriveUsed |= (1 << slot);
        // Model the analog summing amplifier: each rail is the average of the
        // DACs actually driven on it, not their raw sum. Summing alone clips at
        // 255 even at rest (two idle DACs sit at ~128 each → ~256), which is the
        // harsh distortion 4-channel SounDrive music exhibits. Averaging over
        // the *used* DAC count keeps one-DAC-per-side programs at full scale
        // (no regression for Single Warrior: #3F left + #5F right) while two
        // DACs/side mix cleanly. The result can never exceed 255, so no clip.
        const uint8_t leftMask = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 5);
        const uint8_t rightMask = (1 << 3) | (1 << 4) | (1 << 5);
        int ln = __builtin_popcount(sndriveUsed & leftMask);
        int rn = __builtin_popcount(sndriveUsed & rightMask);
        if (ln < 1) ln = 1;
        if (rn < 1) rn = 1;
        int l = (sndriveLatch[0] + sndriveLatch[1] + sndriveLatch[2] + sndriveLatch[5]) / ln;
        int r = (sndriveLatch[3] + sndriveLatch[4] + sndriveLatch[5]) / rn;
        if (l > 255) l = 255;
        if (r > 255) r = 255;
        LED::touchW(LED::COVOX);
        ESPectrum::lastCovoxVal = l;
        ESPectrum::lastCovoxValR = r;
        ESPectrum::CovoxGetSample();
        ioContentionLate(MemESP::ramContended[rambank]);
        return;
      }
    }
    // ShamaZX MIDI Interface (SAM2695)
    // 0xA0CF = control port: TX data byte here
    // 0xA1CF = data port: write 0xFF/0x3F for init, read status (bit 6 = receiver full)
    if (Midi::enabled >= 2 && address == 0xA0CF) {
      Midi::send(data);
      return;
    }
    // General Sound — host-side data/command ports
    if (GS::enabled && !DivMMC::divide_mode) {
      if (a8 == 0xB3 || a8 == 0xBB) {
        LED::touchW(LED::GS);
        if (a8 == 0xB3) GS::hostWriteB3(data);
        else            GS::hostWriteBB(data);
        ioContentionLate(MemESP::ramContended[rambank]);
        return;
      }
      // NeoGS control port GSCTR (#33): reset / NMI / LED
      if (GS::neogs && a8 == 0x33) {
        LED::touchW(LED::GS);
        GS::hostWriteCtrl(data);
        ioContentionLate(MemESP::ramContended[rambank]);
        return;
      }
    }
    // Z80 DMA / zxnDMA port write: listen on both 0x0B and 0x6B
    if (Config::dma_mode && (a8 == 0x0B || a8 == 0x6B)) {
      LED::touchW(LED::DMA);
      Z80DMA::writePort(data);
      ioContentionLate(MemESP::ramContended[rambank]);
      return;
    }
    // Timex SCLD video mode register (port 0x00FF, bit 8 clear)
    // Skip when TR-DOS is active — port 0xFF is the Beta-128 system register
    if (Config::timex_video && !ESPectrum::trdos && a8 == 0xFF && !(address & 0x0100)) {
      LED::touchW(LED::TIMEX);
      VIDEO::timex_port_ff = data & 0x3F;
      VIDEO::timex_mode = data & 0x07;
      VIDEO::timex_hires_ink = (data >> 3) & 0x07;
      ioContentionLate(MemESP::ramContended[rambank]);
      return;
    }
    // SAA1099 Sound Chip
    // Ports: 0x00FF/0x01FF (original), 0x04FF/0x05FF (Light/Middle revisions)
    //        0x00FE/0x01FE (FPGA48all.tap and some other programs use a8=0xFE)
    // Accessible only when TR-DOS ROM is NOT mapped (DOS/ = 1).
    // Karabas-Pro manual: gate is "DOS=0" — for Profi this is the extended
    // periphery mode (CPM=1 AND ROM14=1). Other archs keep the TR-DOS gate.
    if (ESPectrum::SAA_emu && saaChip && !ESPectrum::trdos && (a8 == 0xFF) &&
        !(Z80Ops::isProfi && (portDFFD & 0x20) && MemESP::romLatch)) {
      LED::touchW(LED::SAA);
      if (address & 0x0100) {
        // Register select (bit 8 set): 0x01FF, 0x05FF, etc.
        // Generate samples before selectRegister — it advances external envelope clock
        if (Tape::tapeStatus != TAPE_LOADING) ESPectrum::SAAGetSample();
        saaChip->selectRegister(data);
        return;
      } else {
        // Data write (bit 8 clear): 0x00FF, 0x04FF, etc.
        if (Tape::tapeStatus != TAPE_LOADING) ESPectrum::SAAGetSample();
        saaChip->setRegisterData(data);
        return;
      }
    }
    // AY
    // ========================================================================
    if ((ESPectrum::AY_emu) && (Config::turbosound || Config::tsfm) && address == 0xFFFD) {
      // NedoPC way: chip latched by the DATA written to #FFFD. The full family
      // is #F8..#FF, not just #FF/#FE — TurboSound FM is 2 × YM2203 (an AY plus
      // an FM half each), and its manual (nedopc.com/TURBOSOUND/tfm-prg.zip,
      // §5.1) defines the "pseudo-registers" as %11111frc:
      //   c = chip number
      //   r = ready-poll mode, 0 = ON  → IN #FFFD returns the OPN STATUS byte
      //                                  (bit 7 = BUSY) instead of a register
      //   f = FM synthesis,   0 = ON
      // Classic TurboSound only ever writes #FF/#FE, i.e. r=1, which is why
      // plain-TS software never sees the status register. Xpeccy's
      // libxpeccy/sound/ayym.c TS_NEDOPC decodes the same `(val & 0xF8)==0xF8`.
      //
      // Without the status path a TSFM driver hangs the machine outright: its
      // register write is "wait for BUSY to clear, write the register number,
      // wait again, write the data" (manual §5.1), and IN #FFFD with the latch
      // parked at #F8 returned 0xFF — BUSY forever (hw 2026-08-07, TheLink
      // stuck at ZX PC C0BC in `IN (C) / JP M` initialising the FM chips).
      //
      // "Выбор псевдорегистра обрабатывается ПЛИС, до YM2203 он не доходит -
      // текущий регистр не меняется": the select is swallowed by the CPLD, so
      // it must NOT reach selectRegister — hence the early return. (An earlier
      // build let it through on the guess that hardware parks the latch out of
      // range; the manual says the previously selected register survives.)
      //
      // The chip mapping keeps this project's hw-tested #FF → chip 0 / #FE →
      // chip 1 convention (Xpeccy maps bit 0 the other way round); #F8/#F9 only
      // have to stay consistent with it.
      // #FF/#FE are classic TurboSound and stay under Config::turbosound;
      // the rest of the family (#F8..#FD, i.e. FM enable / ready-poll mode)
      // only exists on a TSFM board, so it needs Config::tsfm.
      if ((data & 0xF8) == 0xF8 && (data >= 0xFE || Config::tsfm)) {
        AySound::selected_chip  = (data & 0x01) ? 0 : 1;
        AySound::ts_status_read = !(data & 0x02);
        // The `f` bit is the CPLD's FM_DIS latch, one for the whole board. It
        // gates the FM DAC only — the FM registers stay writable either way.
        // The mute itself is applied per frame in the mixer, so catch the FM
        // buffer up first: the samples already generated under the old state
        // then belong to the frame they were produced in.
        if (Config::tsfm) {
          const bool fm_on = !(data & 0x04);
          if (fm_on != AySound::ts_fm_enabled) {
            if (Tape::tapeStatus != TAPE_LOADING) ESPectrum::AYGetSample();
            AySound::ts_fm_enabled = fm_on;
          }
        }
        LED::touchW(LED::AY);
        ioContentionLate(MemESP::ramContended[rambank]);
        return;
      }
    }
    if ((ESPectrum::AY_emu) && ((address & 0x8002) == 0x8000)) {
      LED::touchW(LED::AY);
      ayPortWrite(address, data, true);
      ioContentionLate(MemESP::ramContended[rambank]);
      return;
    }
    // MB-02+ ports: FDC (#0F/#2F/#4F/#6F), floppy control (#13), memory paging (#17)
    if (MB02::enabled) {
      uint8_t lo = address & 0xFF;
      if ((lo & 0x9F) == 0x0F) { // WD2797 registers
        FDDStep_MB02(false);
        uint8_t reg = (lo >> 5) & 3;
        rvmWD1793Write(&ESPectrum::mb02_fdd, reg, data);
        // If command register written and DMA transfer is pending, execute it now.
        // On real hardware DMA waits for DRQ from FDC; here we run the whole
        // sector transfer synchronously after the Read/Write Sector command.
        if (reg == 0 && Z80DMA::mb02_deferred && Z80DMA::transfer_active) {
            Z80DMA::executeTransfer();
        }
        ioContentionLate(MemESP::ramContended[rambank]);
        return;
      }
      if (lo == 0x13) { // Floppy control (motor/drive select — housekeeping)
        MB02::writePort13(data);
        return;
      }
      if (lo == 0x17) { // Memory paging (not disk access)
        MB02::writePort17(data);
        return;
      }
    }

    if (DivMMC::enabled) {
      uint8_t lo = address & 0xFF;
      if (lo == 0xE3) {
        LED::touchW(LED::SD);
        DivMMC::bank = data & (DIVMMC_NUM_BANKS - 1);
        if (data & 0x40) DivMMC::mapram = true;
        DivMMC::conmem = (data & 0x80) != 0;
        DivMMC::applyMapping();
        return;
      }
      if (DivMMC::divide_mode) {
        if ((lo & 0xE3) == 0xA3) {
          LED::touchW(LED::SD);
          uint8_t reg = (lo >> 2) & 0x07;
          DivMMC::ide_write(reg, data);
          return;
        }
      } else {
        if (lo == 0xEB) {
          LED::touchW(LED::SD);
          DivMMC::mmc_write(data);
          return;
        }
        if (lo == 0xE7) {
          LED::touchW(LED::SD);
          DivMMC::mmc_cs(data);
          return;
        }
      }
    }

    if (DivMMC::zc_enabled) {
      uint8_t lo = address & 0xFF;
      if (lo == 0x77) { LED::touchW(LED::ZCTRL); DivMMC::zc_write_config(data); return; }
      if (lo == 0x57) { LED::touchW(LED::ZCTRL); DivMMC::zc_write_data(data); return; }
    }

#if IDE_PORT_TRACE
    // Unconditional probe — see the matching read-side comment above.
    if (Z80Ops::isProfi && ((address & 0xFF) & 0x9F) == 0x8B) {
      Debug::log("[IDE OUT probe] addr=%04X data=%02X scheme=%d cpm=%d rom14=%d trdos=%d pc=%04X",
                 address, data, (int)IDE::scheme, (portDFFD & 0x20) != 0, MemESP::romLatch,
                 ESPectrum::trdos, Z80::getRegPC());
    }
#endif
    // IDE/HDD — PROFI scheme, per UnrealSpeccy MM_PROFI modified-ports section:
    //   Gate: ROM14=1 AND CPM=1 (same as UnrealSpeccy: p7FFD&0x10 && pDFFD&0x20).
    //   Port decode: (p1 & 0x9F)==0x8B; CS1=A6=1 for data/registers.
    //   16-bit latch: #xxCB(A5=0) → store HIGH byte in write_latch;
    //                 #xxEB(A5=1, reg=0) → write 16-bit: data|(latch<<8).
    //   CS3: #xxAB(A6=0,A5=1, reg=6) → ATA control register (SRST/nIEN).
    if (IDE::scheme == IDE::PROFI && Z80Ops::isProfi) {
      bool cpm = (portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
      // Same DOS&&!ROM14&&!CPM OR-term as the read side above — the SYS-ROM
      // self-test's HDD probe issues its ATA soft-reset (OUT #06AB,0x06/0x02)
      // in this exact state (hw-confirmed 2026-07-09).
      if ((cpm && rom14) || (dos && !rom14 && !cpm)) {
        uint8_t p1 = address & 0xFF;
        uint8_t reg = (address >> 8) & 7;
        if ((p1 & 0x9F) == 0x8B) {
#if IDE_PORT_TRACE
          Debug::log("[IDE WR] pc=%04X port=%02X reg=%d data=%02X",
                     Z80::getRegPC(), (unsigned)p1, reg, data);
#endif
          if (p1 & 0x40) {                           // CS1 (A6=1): data/registers
            LED::touchW(LED::IDE);
            if (!(p1 & 0x20)) {                      // A5=0 = #xxCB: HIGH byte latch
              IDE::write_latch(data);
              return;
            }
            // A5=1 = #xxEB: write register or 16-bit data
            if (reg == 0)                            // data register: combine with latch
              IDE::write_data_low(data);             // latch_write is HIGH byte
            else
              IDE::write8(reg, data);
            return;
          }
          // CS3 (A6=0) = #xxAB reg6: ATA device control (0x3F6, SRST/nIEN).
          // MBOOTHDD issues the ATA soft-reset via OUT (#06AB),A — port 0xAB has
          // A5=0, so do NOT gate on A5 (the old `p1&0x20` check dropped the reset).
          if (reg == 6) {
            LED::touchW(LED::IDE);
            IDE::write8(8, data);
            return;
          }
        }
      }
    }

    // PQ-DOS extended config ports #008B/#018B/#028B — see the read-side comment
    // above (Ports::input) for the CS formula (verified against karabas_pro.vhd)
    // and the "not yet wired" caveat. #028B is unconditional; #008B/#018B are
    // CPM/ROM14/DOS-gated.
    if (Z80Ops::isProfi) {
      if (address == 0x028B) {
        port028B = data;
        // Apply TURBO_MODE (bits 5-6) to the CPU turbo — ROMain forces 7 MHz
        // (OUT #028B,0x20) around heavy operations and clears it afterwards,
        // exactly like the real hardware latch. Other bits (HDD/FDC/sound
        // switches) remain unwired — see the read-side caveat.
        uint8_t turbo = (data >> 5) & 3;
        if (turbo != ESPectrum::multiplicator) {
          ESPectrum::multiplicator = turbo;
          CPU::updateStatesInFrame();
        }
#if PROFI_PORT_TRACE
        Debug::log("[8B OUT] #028B <- %02X pc=%04X", data, Z80::getRegPC());
#endif
        return;
      }
      bool cpm = (portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
#if PROFI_PORT_TRACE
      if (address == 0x008B || address == 0x018B)
        Debug::log("[8B OUT probe] addr=%04X data=%02X cpm=%d rom14=%d dos=%d pc=%04X",
                   address, data, cpm, rom14, dos, Z80::getRegPC());
#endif
      if ((cpm && rom14) || (dos && !rom14)) {
        if (address == 0x008B) {
          uint8_t prev = port008B;
          port008B = data;
          // ONROM (bit6): forced DOS signal — in the FPGA (karabas_pro.vhd
          // "TR-DOS FLAG" process) `or onrom='1'` sets dos_act every clock and
          // outranks every exit condition, including NOROM. Map the rising
          // edge here; while the bit stays set, Z80::check_trdos() suppresses
          // the PC>=0x4000 DOS exit. UNLOCK_128 (bit7) is consumed in the
          // 0x3Dxx automap trap. ROM0-5 / #018B RAM0-7 remain stub-stored —
          // dead signals in the real FPGA too (only rom0 feeds the
          // config-flash loader path, not applicable here).
          if ((data & 0x40) && !(prev & 0x40) && !ESPectrum::trdos) {
            ESPectrum::trdos = true;
            if (!MemESP::page0ram) {
              MemESP::romInUse = MemESP::romLatch ? 1 : 0; // f(DOS,ROM14)
              MemESP::ramCurrent[0] = MemESP::rom[MemESP::romInUse].direct();
            }
          }
          return;
        }
        if (address == 0x018B) { port018B = data; return; }
      }
      // PQDOS/RS232 serial ports #F3/#D3 (keyboard) and #B3/#93 (RS232) — the
      // keyboard driver writes command bytes here to init/select the AT/serial
      // keyboard (e.g. 0x40 at bank5 0x5426, and the 0x2780 resident driver).
      // pico-speccy's Beta-128 FDC decode uses (address & 0xe3), and these four
      // ports alias onto real FDC registers (#F3→SYS/DATA, #D3→SECTOR,
      // #B3→#A3, #93→#83), so without an exact-match intercept here the
      // keyboard command bytes leak into the WD1793 (drive-select / soft-reset /
      // side toggle → disk corruption). On real hardware #F3/#D3/#B3/#93 are the
      // UART channel, NEVER the FDC (which uses #FF/#BF/#3F/#83/#A3/#C3/#E3), so
      // matching the exact low byte leaves the real FDC ports untouched. Consume
      // the write (no UART emulation) — key DATA is served on the read side.
      {
        uint8_t lo8o = address & 0xFF;
        if (lo8o == 0xF3 || lo8o == 0xD3 || lo8o == 0xB3 || lo8o == 0x93) {
          // #F3 = VV51 command register (bit2 RxE arms the serial mouse; the
          // PQDOS keyboard init keeps it clear). #B3/#93 bit0 = hardware-INT
          // enable (hw_int.vhd decodes BOTH; note #B3 writes are shadowed by
          // the General Sound host port while GS is enabled — drivers using
          // #93, like the stock Profi ones, are unaffected). #D3 data writes
          // are dropped: the VV51 TX side isn't emulated (TxRDY/TxE stuck 1).
          if (lo8o == 0xF3) serialMouseCtl = data;
          else if (lo8o == 0xB3 || lo8o == 0x93) serialMouseIntEn = data & 0x01;
#if FDD_PORT_TRACE
          static uint16_t lastPc = 0xFFFF; static uint8_t lastData = 0, lastLo = 0;
          uint16_t pcn = Z80::getRegPC();
          if (pcn != lastPc || data != lastData || lo8o != lastLo) {
            lastPc = pcn; lastData = data; lastLo = lo8o;
            Debug::log("[PQKBD OUT] port=%02X data=%02X cpm=%d rom14=%d dos=%d pc=%04X",
                       lo8o, data, (int)cpm, (int)rom14, (int)dos, pcn);
          }
#endif
          return;
        }
      }
    }

    // Profi FDC stub: command write to WD1793 reg0 → arm the one-shot busy flag.
    // Only active when no disk at all is mounted; if any disk is present (TRD/SCL
    // included), route to the real FDC so the SYS ROM disk probe can succeed.
    bool out_has_raw_disk = ESPectrum::fdd.disk[ESPectrum::fdd.diskS] &&
        (ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsUDIFile ||
         ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsFDIFile ||
         ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsMBDFile ||
         ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsTD0File ||
         ESPectrum::fdd.disk[ESPectrum::fdd.diskS]->IsProFile);
    bool out_has_any_disk = ESPectrum::fdd.disk[ESPectrum::fdd.diskS] != nullptr;
    if (Z80Ops::isProfi && MemESP::romInUse == 0 && !out_has_any_disk
        && (address & 0xE3) == 0x03) {
      profi_fdc_busy = 1;
      ioContentionLate(MemESP::ramContended[rambank]);
      return;
    }

    // Profi CP/M mode: no-disk CMD write detection (must be BEFORE the
    // out_has_raw_disk gate below, which is skipped when no disk is present).
    //
    // When no disk is in the selected drive, out_has_raw_disk=false and the
    // FDC output block is never entered.  But DSKKE9A still issues CMD writes
    // and spins in the re-issue loop (CALL 0x40EA → JR 0x40D9 → OUT 0x1F)
    // at CPU speed, overflowing the stack into code and crashing.
    //
    // Fix: count consecutive no-disk CMD writes here.  After 4 we walk the
    // Z80 stack to find the original non-0x40DE return address, restore SP
    // and redirect PC to 0x40E1 (EI; RET) for a clean error return.
    if (Z80Ops::isProfi && (portDFFD & 0x20) &&
        !out_has_raw_disk &&
        (address & 0xE3) == 0x03 && ((address >> 5) & 0x3) == 0) {
      ++profi_nodisk_reissue_cnt;
      if (profi_nodisk_reissue_cnt >= 4) {
        profi_nodisk_reissue_cnt = 0;
        uint16_t sp = Z80::getRegSP();
        uint16_t found_addr = 0;
        for (int i = 0; i < 256 && sp < 0xFF00; i++, sp += 2) {
          uint16_t lo = MemESP::romPeek(sp >> 14, MemESP::ramCurrent[sp >> 14], (sp) & 0x3FFF);
          uint16_t hi = MemESP::romPeek((sp+1) >> 14, MemESP::ramCurrent[(sp+1) >> 14], (sp+1) & 0x3FFF);
          uint16_t frame = lo | (hi << 8);
          if (frame != 0x40DE) {
            found_addr = frame;
            break;
          }
        }
        if (found_addr) {
          ESPectrum::fdd.status = kRVMWD177XStatusNotReady | kRVMWD177XStatusSeek;
          ESPectrum::fdd.control |= kRVMWD177XINTRQ | kRVMWD177XFINTRQ;
          ESPectrum::fdd.stepState = kRVMWD177XStepIdle;
          Z80::setRegSP(sp);
          Z80::setRegPC(0x40E1);
          Debug::log("[FDC] Profi no-disk loop break (gate): drv=%d found_ret=0x%04X new_sp=0x%04X",
                     ESPectrum::fdd.diskS, found_addr, sp);
        } else {
          Debug::log("[FDC] Profi no-disk (gate): no non-0x40DE frame, sp=0x%04X",
                     Z80::getRegSP());
        }
      }
      ioContentionLate(MemESP::ramContended[rambank]);
      return;
    }

    // Check if TRDOS Rom is mapped, or a raw disk is loaded. Scorpion SYSEN
    // (1FFD D1, the service monitor) opens the FDC ports too — see the
    // matching read-side comment (ZXMAK2: "Ports active when DOSEN=1 or
    // SYSEN=1").
    if (ESPectrum::trdos || out_has_raw_disk ||
        (Z80Ops::isScorpion && (port1FFD & 0x02))) {

      // Profi CP/M mode: FDC data registers shift to 0x83/0xA3/0xC3/0xE3
      // UnrealSpeccy decode: (addr & 0x9F) == 0x83 → reg index = (addr >> 5) & 3
      //   0x83 → reg0 (CMD/STATUS), 0xA3 → reg1 (TRACK),
      //   0xC3 → reg2 (SECTOR),     0xE3 → reg3 (DATA)
      // 0xBF & 0x9F == 0x9F ≠ 0x83, so SYS port 0xBF falls through to switch below.
      // Profi CP/M shifted FDC (see matching read path): enable on CPM=1 alone,
      // not gated by ROM14. The Dos5 5.30 CP/M driver writes Type-I commands to
      // 0x83 (e.g. OUT (0x83),0x0C/0x1C at 0x864F/0x866C) with ROM14=1.
      // Same DOS&&!ROM14 boot-context OR-gate as the read path above (self-test
      // FDC register check at ROM 0x140C runs before CPM is ever toggled on).
      bool cpm83o = (portDFFD & 0x20), rom14_83o = MemESP::romLatch, dos83o = ESPectrum::trdos;
      uint8_t fr83o = (address >> 5) & 0x3;
      // ALL FOUR shifted registers are the WD1793 in the DOS&&!ROM14 SYS-ROM
      // context too — see the matching read-side comment for the full story
      // (the old fr 0/2 restriction came from misreading ROM 0x148D as an
      // #A3 write; it is OUT (0x3F),A. The restriction misrouted the boot
      // loader's OUT (#A3),track / OUT (#E3),seek-target into the SYS decode
      // below → spurious WD reset → track=0xFF → RDSEC RecordNotFound loop,
      // hw log 2026-07-09).
      if (Z80Ops::isProfi && ((address & 0x9F) == 0x83) &&
          (cpm83o || (dos83o && !rom14_83o))) {
        FDDStep(false);
        uint8_t fr = fr83o;
        // CMD write via shifted 0x83 → activate shifted-scheme status for IN(0x3F)
        if (fr == 0) profi_shifted_fdc = true;
        rvmWD1793Write(&ESPectrum::fdd, fr, data);
        // MUST return here (mirrors the read-side's early return): falling
        // through reaches the #EFF7 decode further down, which for Profi is
        // (address & 0xF008) == 0xE000 — a mask that all four shifted-FDC low
        // bytes (0x83/A3/C3/E3, bit3=0) satisfy whenever the Z80 accumulator
        // (which is BOTH the port's high byte AND the OUT data, since this is
        // OUT (n),A) has its top nibble = 0xE. The self-test's FDC round-trip
        // loop (ROM 0x140C) walks A=0xFF..0x01, so it hits e.g. 0xEFC3 —
        // spuriously matching #EFF7 too and clobbering page0ram from data
        // bit3, paging RAM#0 (zeroed) into the low 16K mid-self-test. Since
        // the self-test code itself lives in that page0 ROM, the CPU then
        // fetches all-zero NOPs from PC onward forever (hw-confirmed
        // 2026-07-08: PAGE0->RAM#0, PC stuck executing NOPs at ~0x1263).
        return;
      } else if (Z80Ops::isProfi && (address & 0xFF) == 0x3F &&
                 (((portDFFD & 0x20) && MemESP::romLatch) ||
                  (ESPectrum::trdos && !MemESP::romLatch && !(portDFFD & 0x20)))) {
        // Per manual "Порты FDD": in the ROM14=1 & CPM=1 (MBOOTHDD) scheme the
        // WD93 SYS register (RQ93) is at #3F — NOT the track register. The
        // MBOOTHDD loader selects drive/side/reset via OUT(#3F) (e.g. 0x1C=side0,
        // 0x0C=side1). #3F&0xe3==0x23 would otherwise land in the track-register
        // case and silently drop the side select → fdd.side stuck → side-compare
        // rejects the catalog on track0/side0 → "FDD Read Error".
        // Third OR-term (DOS=1, ROM14=0, CPM=0): the SYS-ROM self-test's own
        // FDD0:/FDD1: drive-detect routine (ROM 0x1432/0x1478, see matching
        // read-side comment above) writes drive/side/reset bits to #3F in
        // this exact state, before CP/M is ever toggled on — hw-confirmed
        // 2026-07-09 by disassembling karabas-pro's bios_pqdos.hex.
        // SYS register write (drive/side select) — housekeeping, not counted.
        FDDStep(true);
        profiFdcSysWrite(data);
      } else if (Z80Ops::isProfi &&
                 ((address & 0xFF) == 0x67 || (address & 0xFF) == 0x87 ||
                  (address & 0xFF) == 0xA7 || (address & 0xFF) == 0xC7 ||
                  (address & 0xFF) == 0xE7)) {
        // SPI-flash ports (#C7/#87/#A7/#E7/#67 per Karabas-Pro dev manual) —
        // reserved for the on-board flash chip regardless of CPM/ROM14/DS80.
        // #A7 aliases #BF (case 0xa3) and #67 aliases #7F DATA reg (case
        // 0x63) in the mask below; #E7 aliases #FF (case 0xe3). PQDOS's own
        // SPI-flash driver (bank0 ROM ~0x28xx) got its control/data writes
        // misrouted into the WD1793 (spurious drive/side/reset pulses) — hw
        // log 2026-07-09. Nothing to actually emulate here (no real SPI-flash
        // chip backing), just don't let it hit the FDC.
      } else switch (address & 0xe3) {

      case 0x03:
      case 0x23:
      case 0x43:
      case 0x63:
        FDDStep(false);
        // CMD write via normal path → deactivate shifted-scheme status
        if (((address >> 5) & 0x3) == 0) profi_shifted_fdc = false;
        // Profi CP/M: detect the DSKKE9A re-issue loop (CALL 0x40EA → JR 0x40D9).
        // The DSKKE9A disk driver uses an infinite re-issue loop: after issuing a
        // Seek command it immediately calls CALL 0x40EA which JRs back to re-issue
        // the OUT. On real Profi hardware the Z80 WAIT pin stretches each OUT until
        // the WD1793 finishes (or the head is at target), so only a handful of
        // iterations occur. Without WAIT emulation, the loop spins at CPU speed
        // (~85 K iterations/second), quickly overflowing the stack into code.
        //
        // FIX: when a no-disk CMD write is issued consecutively (re-issue loop),
        // count the re-issues. After MAX_REISSUES we:
        //  1. Walk the Z80 stack to find the first return address that is NOT 0x40DE
        //     (the CALL 0x40EA return address) — this is the frame that called the
        //     Seek path originally (e.g. 0x40AB, which checks SEEK_ERROR status).
        //  2. Restore SP to just below that frame so RET returns to it.
        //  3. Set PC = 0x40E1 (EI; RET) so interrupts are re-enabled and the
        //     original caller resumes.
        //  4. Leave WD status = NOT_READY | SEEK_ERROR so the caller detects failure.
        if (Z80Ops::isProfi && (portDFFD & 0x20)) {
          uint8_t fdc_reg = (address >> 5) & 0x3;
          if (fdc_reg == 0) {  // CMD register write
            bool no_disk = !ESPectrum::fdd.disk[ESPectrum::fdd.diskS];
            if (no_disk) {
              ++profi_nodisk_reissue_cnt;
              if (profi_nodisk_reissue_cnt >= 4) {
                profi_nodisk_reissue_cnt = 0;
                // Walk Z80 stack to find the first return address ≠ 0x40DE.
                // 0x40DE is the CALL 0x40EA return (pushed by the re-issue loop).
                // The first non-0x40DE frame is the original caller.
                uint16_t sp = Z80::getRegSP();
                uint16_t found_addr = 0;
                for (int i = 0; i < 256 && sp < 0xFF00; i++, sp += 2) {
                  uint16_t lo = MemESP::romPeek(sp >> 14, MemESP::ramCurrent[sp >> 14], (sp) & 0x3FFF);
                  uint16_t hi = MemESP::romPeek((sp+1) >> 14, MemESP::ramCurrent[(sp+1) >> 14], (sp+1) & 0x3FFF);
                  uint16_t frame = lo | (hi << 8);
                  if (frame != 0x40DE) {
                    found_addr = frame;
                    break;
                  }
                }
                if (found_addr) {
                  // Set WD to NOT_READY + SEEK_ERROR so status check at 0x40AE
                  // returns carry=1 (CP/M error) to the application.
                  // NOT_READY | SEEK_ERROR, BUSY=0
                  ESPectrum::fdd.status =
                      kRVMWD177XStatusNotReady | kRVMWD177XStatusSeek;
                  ESPectrum::fdd.control |= kRVMWD177XINTRQ | kRVMWD177XFINTRQ;
                  ESPectrum::fdd.stepState = kRVMWD177XStepIdle;
                  // sp points to the found_addr frame (break was hit before
                  // the for-loop's sp += 2 increment), so found_addr is at
                  // the top-of-stack.  RET will pop it and return there.
                  Z80::setRegSP(sp);
                  // EI + RET: re-enable interrupts and return to found_addr
                  Z80::setRegPC(0x40E1);
                  Debug::log("[FDC] Profi no-disk loop break: drv=%d found_ret=0x%04X new_sp=0x%04X",
                             ESPectrum::fdd.diskS, found_addr, sp);
                } else {
                  Debug::log("[FDC] Profi no-disk loop: no non-0x40DE frame found, sp=0x%04X",
                             Z80::getRegSP());
                }
                break;  // skip rvmWD1793Write
              }
            } else {
              profi_nodisk_reissue_cnt = 0;
            }
          }
        }
        rvmWD1793Write(&ESPectrum::fdd, ((address >> 5) & 0x3), data);
        break;
      case 0xa3:
        // Profi: port 0xBF (address & 0xe3 == 0xa3) is the RQ93 SYS register
        // only in ROM14=0 & CPM=1 (the BOOTFDD scheme). When ROM14=1 the address
        // #BF is reassigned to extended periphery, and the SYS register moves to
        // #3F (the ROM14=1 & CPM=1 / MBOOTHDD scheme, handled before this switch).
        if (Config::arch != A_PROFI || MemESP::romLatch)
          break;
        // SYS register write — housekeeping, not counted as disk access.
        FDDStep(true);
        profiFdcSysWrite(data);
        break;
      case 0xe3:
        // Port #FF (and the #FF-family: #E7/#EB/#EF/#F3/#F7/#FB that also satisfy
        // address&0xe3==0xe3) is the WD93 SYS register ONLY in the standard scheme
        // (CPM=0). Per manual "Порты FDD", in CP/M the SYS register moves to #BF
        // (ROM14=0) or #3F (ROM14=1), and the #FF-family belongs to extended
        // periphery — notably the PROFI IDE/HDD ports (#xxEB) probed by the HDD22
        // driver. Routing those to the FDC here issued a spurious soft-reset
        // (SYS bit2=0 → rvmWD1793Reset → track=0xFF), which corrupted the floppy
        // track register mid-boot and made MBOOTHDD mis-seek (530.pro hang).
        // So gate out CP/M mode: only the standard TR-DOS scheme uses #FF as SYS.
        if (Z80Ops::isProfi && (portDFFD & 0x20))
          break;
        // SYS register write (#FF: drive/side/motor select) — housekeeping,
        // recurs continuously while TR-DOS is paged in; not counted as access.
        FDDStep(true);
        profiFdcSysWrite(data);
        break;
      }
    }
    // Karabas-Pro's OWN native RTC ports (#FF/#BF AS, #DF/#9F DS) — placed here,
    // after the Beta-128/FDC write switch above, so FDC gets first refusal on
    // these addresses (same reasoning as the read-side handler in Ports::input;
    // see that comment for the full CS formula and the real-hardware trace that
    // showed the DOS=1&&ROM14=0 branch is required for PQDOS's boot-time RTC
    // format patch to ever reach these ports).
#if RTC_PORT_TRACE
    if (Z80Ops::isProfi) {
      uint8_t lo8t = address & 0xFF;
      if ((lo8t | 0x40) == 0xFF || (lo8t | 0x40) == 0xDF) {
        static uint32_t pout_n = 0;
        if (++pout_n <= 150 || (pout_n & 0x3FF) == 0)
          Debug::log("[RTC-AS/DS OUT probe] addr=%04X lo=%02X data=%02X cpm=%d rom14=%d trdos=%d pc=%04X n=%u",
                     address, lo8t, data, (portDFFD & 0x20) != 0, MemESP::romLatch,
                     ESPectrum::trdos, Z80::getRegPC(), (unsigned)pout_n);
      }
    }
#endif
    if (Z80Ops::isProfi) {
      bool cpm = (portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
      if ((cpm && rom14) || (dos && !rom14)) {
        uint8_t lo8 = address & 0xFF;
        if ((lo8 | 0x40) == 0xFF) { // #FF/#BF (AS)
          // Latch the register index even when the RTC is off (the FDC already
          // had its refusal in the switch above) so a following read returns the
          // right static value; the data write below is what's gated on enabled.
          RTC::selectReg(data);
#if RTC_PORT_TRACE
          Debug::log("[RTC-AS OUT] sel<-%02X pc=%04X", data, Z80::getRegPC());
#endif
          ioContentionLate(MemESP::ramContended[rambank]);
          return;
        }
        if ((lo8 | 0x40) == 0xDF) { // #DF/#9F (DS)
          if (Config::rtc_enabled) RTC::writeData(data); // off = swallow (no clock/NVRAM)
#if RTC_PORT_TRACE
          Debug::log("[RTC-DS OUT] sel=%02X <-%02X pc=%04X", RTC::dbgSel(), data, Z80::getRegPC());
#endif
          ioContentionLate(MemESP::ramContended[rambank]);
          return;
        }
      }
    }
    ioContentionLate(MemESP::ramContended[rambank]);
  }
  // Pentagon #EFF7 (page0ram/notMore128). The old loose Pentagon decode
  // (address & 0x1008)==0 (= A12=0 & A3=0) COLLIDES with low ports whose bit3
  // is 0 — hw-hit twice:
  //  - Profi CP/M FDC command port #83 (RDSEC 0x82/0x86 → OUT(0x83),A gives
  //    address 0x8283/0x8683) — clobbered page0ram mid-RDSEC, MBOOTHDD stack
  //    in page0 → wild jump (NOP-slide crash);
  //  - Z-Controller SD ports #0057/#0077 (Neo8Tracker's FAT driver runs FROM
  //    page0 RAM and streams sectors with OUT (C),A to BC=0x0057 — every data
  //    byte's bit3 flapped page0ram under the executing code → crash into
  //    screen memory).
  // Real Pentagon-1024SL software addresses the port as a full 16-bit #EFF7
  // (LD BC,#EFF7), so require the #EFF7 family on Pentagon too (same fix as
  // Profi's: A15-A12=0xE, A3=0 — keeps mirror decodes like #EFF7/#EFFF-#xEF7).
  bool eff7_decode = ((address & 0xF008) == 0xE000);
  if ((Z80Ops::isPentagon || Z80Ops::isProfi) && eff7_decode) { // EFF7
    // The #EFF7 page0-overlay / lock-disable (bits 2,3) is a Pentagon-1024SL (and
    // Profi) feature; a plain Pentagon 512/128 has no #EFF7 and must NOT respond to
    // it. Gating to is1024/isProfi keeps real hardware semantics and lets guest
    // software tell a 512 from a 1024SL by probing #EFF7 (a 512 leaves page0 = ROM)
    // before ever touching #7FFD bit5 (which would permanently lock a 512).
    if (!MemESP::pagingLock && (Z80Ops::is1024 || Z80Ops::isProfi)) {
      uint8_t prevPage0 = MemESP::page0ram;
      uint8_t prevSRAM = MemESP::newSRAM;
      uint8_t prevNotMore = MemESP::notMore128;
      MemESP::notMore128 = bitRead(data, 2);
      if (Z80Ops::is1024) {
        // Pentagon-1024SL v2.x: EFF7 D3 overlays the hidden CACHE page at
        // 0x0000-0x3FFF — the SAME separate memory the #FB/#7B trap maps
        // (Unreal's EFF7_ROCACHE), NOT RAM bank 0. Mapping ram[0] here
        // aliased the overlay with 7FFD bank 0 at #C000 — an aliasing real
        // hardware doesn't have. Neo8Tracker's FAT driver keeps its
        // workspace in the overlay while the tracker uses bank 0 through
        // #C000: with the false aliasing they silently corrupted each other
        // (wild jumps / DI+HALT during mount; worked in UnrealSpeccy).
        MemESP::newSRAM = bitRead(data, 3);
        if (MemESP::newSRAM != prevSRAM)
          MemESP::recoverPage0();
      } else {
        MemESP::page0ram = bitRead(data, 3);
        if (MemESP::page0ram != prevPage0)
          MemESP::recoverPage0();
      }
      // Pentagon-1024SL v2.x: EFF7 D4 = turbo OFF (1 = 3.5 MHz, 0 = the
      // machine's turbo clock). Unreal's emul.h calls the bit EFF7_GIGASCREEN
      // (historical name) but pentevo io.cpp implements it as
      // `turbo((pEFF7 & EFF7_GIGASCREEN) ? 1 : 2)` — it is the CPU speed
      // switch. TheLink's beam-locked multicolor effects (TUNNEL, MULBAR)
      // write 0x10 at entry / 0x00 at exit: the demo runs at 7 MHz but those
      // effects need cycle-exact 3.5 MHz raster timing (each writer iteration
      // is exactly 1792 T = 8 scanlines, and the doubled turbo INT window even
      // adds a second EI,RET interrupt = +33 T — hw-measured with MC7FFD_TRACE
      // 2026-08-14: 1-cell attr stripes at column 0). Honored only while the
      // USER has turbo on: D4=0 must not turbo a 3.5 MHz session — the Gluk
      // RTC clock loop rewrites EFF7 (D7 CMOS) with D4=0 all the time.
      // Immediate apply mid-frame follows the Profi #028B precedent above.
      if (Z80Ops::is1024 && ESPectrum::multUser) {
        uint8_t want = bitRead(data, 4) ? 0 : ESPectrum::multUser;
        if (want != ESPectrum::multiplicator) {
          ESPectrum::multiplicator = want;
          CPU::updateStatesInFrame();
        }
      }
      // Only flash the RAM paging LED on an actual paging change. #EFF7 is
      // shared with the CMOS-enable bit (D7): Gluk's RTC clock loop toggles D7
      // every update, which would otherwise blink the RAM LED with no real
      // paging activity.
      if (MemESP::page0ram != prevPage0 || MemESP::newSRAM != prevSRAM ||
          MemESP::notMore128 != prevNotMore)
        LED::touchW(LED::RAM);
    }
  }
  // Scorpion #1FFD (write-only): D0=1 → RAM page 0 at 0x0000 (r/w), D1=1 → service
  // monitor ROM (bank2) override, D4 → +8 on the 0xC000 RAM page (256K = 16 pages);
  // D3 (RS-232) and D5 (Centronics strobe) are ignored. Decode per MAME's PAL mask
  // (1FFD = 00xxxxxxxx1xxx01): A15=A14=0, A1=0, plus A5=1 so small-port OUT (n),A
  // probes with n<0x20 can't land here; the #7FFD side below stays as loose as the
  // rest of the codebase (A14=1 separates them — see the extracker note there).
  // NEVER gated by pagingLock: the 7FFD D5 lock freezes only the 7FFD latch on
  // real hardware, #1FFD stays live until reset.
  if (Z80Ops::isScorpion && ((address & 0xC002) == 0) && (address & 0x0020)) {
    LED::touchW(LED::RAM);
#if GMX_TRACE
    if (data != port1FFD)
      GMXT("[GMX 1FFD] %02X (addr=%04X) pc=%04X", data, address, Z80::getRegPC());
#endif
    // GMX 1FFD D2 (hard-wired DOS page) FALLING edge: on real hardware DOSEN
    // drops on the very next >=0x4000 read — MAME's beta_disable_r fires on ANY
    // read, so dos survives a D2 clear by at most one instruction when the
    // writer runs from RAM. Our DOS exit only runs at control-flow opcodes
    // checking the NEW PC, so a jump straight INTO ROM (<0x4000) closes the
    // window with trdos still latched — romInUse then decodes as
    // (dos<<1)|rom14 = the wrong bank (GMX_TRACE 2026-08-31:
    // "[GMX romU] 3->2 1FFD=00 dos=1" after the loader's D2 pulse at 0x5F4C —
    // the 9B-pattern striped-screen crash class). Every D2 writer in the GMX
    // firmware runs from RAM, so clear the latch right at the edge.
    if (g_scorp_gmx && (port1FFD & 0x04) && !(data & 0x04) &&
        Z80::getRegPC() >= 0x4000)
      ESPectrum::trdos = false;
    port1FFD = data;
    uint32_t page = scorpionC000Page(MemESP::bankLatch & 0x07);
    if (page != MemESP::bankLatch) {
      MemESP::bankLatch = page;
      MemESP::ramContended[3] = false;
      MemESP::ramCurrent[3] = MemESP::ram[page].sync(3);
    }
    MemESP::page0ram = data & 0x01;
    scorpionRomUpdate();   // D1 service / GMX D2 DOS / TR-DOS / romLatch + recoverPage0
    return;
  }
  // 128K, Pentagon
  // ==================================================================
  // ALF shares the 128K codepath but uses port #5F (A7=0, A0=1) for its ROM-bank
  // latch, handled earlier and returned. #7FFD RAM paging (A7=1) does not collide
  // with that, and 128K-only cart games need it (else they abort with "requires
  // 128K RAM"). The cart ROM in page0 is preserved: romInUse is gated by !ia below,
  // so recoverPage0() keeps it. Require A7=1 for ALF so the loose #7FFD decode
  // (which ignores A7) can't catch the A7=0 port region used by ALF peripherals.
  // The decode is deliberately the loose 128K/Pentagon one (A15=0 & A1=0),
  // i.e. exactly what the paging latch's clock gate does on those machines.
  // It means `OUT (n),A` with a small A pages the machine — ExTracker 3.07's
  // device probe at 0x6FAA/0x6FDE (`LD A,B / OUT (0x88),A`) drives #0188 and
  // does just that. That is honest Pentagon behaviour, not a bug here; see
  // the extracker-7ffd-loose-decode note before "fixing" it with A14.
  if ((!Z80Ops::is48) && ((address & 0x8002) == 0) &&
      (!Z80Ops::isScorpion || (address & 0x4000)) && // Scorpion: A14=1 → 7FFD, A14=0 is the 1FFD family (handled above)
      (!Z80Ops::isALF || (address & 0x0080))) { // 8002 !-> 7FFD
    ++Ports::port7ffd_cnt;
#if MC7FFD_TRACE
    mc7ffdTrace(data);
#endif
    LED::touchW(LED::RAM);
#if FDD_PORT_TRACE
    // Profi-only: normal 128K screen-flip demos legitimately hammer 7FFD from a
    // fixed PC forever, which would false-positive the watchdog on non-Profi.
    if (Z80Ops::isProfi) checkPagingStuck(Z80::getRegPC());
#endif
#if PROFI_PORT_TRACE
    if (Z80Ops::isProfi) {
      static uint8_t prev_7ffd = 0xFE;
      if (prev_7ffd != data) {
        Debug::log("[7FFD] new=0x%02X bank=%d videoLatch=%d romLatch=%d lock=%d pc=%04X",
                   data, data & 7, (data >> 3) & 1, (data >> 4) & 1, (data >> 5) & 1,
                   Z80::getRegPC());
        prev_7ffd = data;
      }
    }
#endif
    // 48K paging-lock gate (7FFD bit5). Mirror UnrealSpeccy io.cpp set_banks
    // entry: the lock is RE-EVALUATED on every write, not a sticky flag.
    // Pentagon-1024 (not notMore128) and Profi with NOROM(DFFD.4) bypass the
    // lock entirely, so a CP/M bank-switch routine that writes 7FFD with bit5
    // set is never silently dropped (caused level-load freezes / wrong banks).
    bool blocked = MemESP::pagingLock;
    if (blocked) {
      if (Z80Ops::is1024 && !MemESP::notMore128)
        blocked = false; // Pentagon-1024 unlocked
      else if (Z80Ops::isProfi && (portDFFD & 0x10))
        blocked = false; // Profi NOROM → paging always live
    }
    if (!blocked) {
      uint8_t D5 = bitRead(data, 5);
      if (Z80Ops::is1024) {
        MemESP::pagingLock = MemESP::notMore128 ? D5 : 0;
      } else {
        MemESP::pagingLock = D5;
      }
      uint32_t page = (data & 0x7);
      // Scorpion: page = (GMX #DFFD bits << 4) | ((1FFD D4) >> 1) | (7FFD bits
      // 0-2) — the extended-RAM bits live in the OTHER ports' latches,
      // recombined on every write of any of them.
      if (Z80Ops::isScorpion) {
        page = scorpionC000Page(data & 0x07);
      }
      if ((Z80Ops::is512 || Z80Ops::is1024) && !MemESP::notMore128 &&
          !MemESP::pagingLock) {
        uint8_t D6 = bitRead(data, 6);
        uint8_t D7 = bitRead(data, 7);
        if (D6)
          page += 8;
        if (D7)
          page += 16;
        if (Z80Ops::is1024 && D5)
          page += 32;
      }
      if (MEM_PG_CNT > 64) {
        uint32_t pPlus = page + portAFF7 * extendedZxRamPages();
        uint32_t pages = ram_pages + butter_pages + psram_pages + swap_pages;
        if (pPlus <
            pages) { // W/A: protection of incorrect page selection logic
          page = pPlus;
        }
      }
      // For Profi: combine 0x7FFD bits[2:0] with 0xDFFD group (bits[2:0]<<3)
      if (Z80Ops::isProfi) {
        uint32_t profi_page = (page & 0x7) + ((portDFFD & 0x7) << 3);
        uint32_t profi_pages = ram_pages + butter_pages + psram_pages + swap_pages;
        if (profi_page < profi_pages) page = profi_page;
      }
      if (MemESP::bankLatch != page) {
        MemESP::bankLatch = page;
        MemESP::ramContended[3] =
            (Z80Ops::isPentagon || Z80Ops::isProfi || Z80Ops::isScorpion) ? false : (page & 0x01 ? true : false);
      }
      // Profi SCO (DFFD bit3): bank1=ramPage (full 0..63), bank3=page7; else bank3=ramPage
      if (Z80Ops::isProfi && (portDFFD & 0x08)) {
        MemESP::ramCurrent[1] = MemESP::ram[MemESP::bankLatch].sync(1);
        MemESP::ramCurrent[3] = MemESP::ram[7].sync(3);
      } else {
        MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3);
      }
#if PROFI_PORT_TRACE
      if (Z80Ops::isProfi && (portDFFD & 0x80)) {
        uint32_t bl = MemESP::bankLatch;
        if (bl == 4 || bl == 6 || bl == 56 || bl == 58) {
          bool vl = MemESP::videoLatch; // current (not yet updated for bit3)
          bool sco = portDFFD & 0x08;
          char slot = sco ? '1' : '3';
          bool disp = (!vl && (bl == 4 || bl == 56)) || (vl && (bl == 6 || bl == 58));
          Debug::log("[7FFD] bl=%u slot%c vl=%u %s PC=%04X",
              bl, slot, vl, disp ? "DISPLAY-PAGE!" : "write-buf", Z80::getRegPC());
        }
      }
#endif
      { uint8_t prevLatch = MemESP::romLatch;
        MemESP::romLatch = bitRead(data, 4);
        if (Z80Ops::isProfi) {
          // Profi/Karabas: the ROM bank is a LIVE 2-bit function of
          // (DOS, ROM14) — FPGA memory.vhd: rom_page <= not(TRDOS) & ROM_BANK:
          //   DOS=1: ROM14=0 → bank0 (SYS),  ROM14=1 → bank1 (TR-DOS/PQDOS)
          //   DOS=0: ROM14=0 → bank2 (128K), ROM14=1 → bank3 (SOS/48K)
          // It used to be frozen while trdos=1 ("trdos path handled in
          // check_trdos"), which broke PQDOS's RST8 trampoline: bank1 code at
          // 0x3D38 writes 7FFD with ROM14=0 and expects the very next fetch
          // (0x3D40) to come from bank0 (SYS: POP AF; JP 0x0008 → the ROM
          // service dispatcher). We kept fetching bank1's bytes instead (an
          // LDIR that treats the service-call registers as copy params) →
          // garbage copy → boot fell into the 128K menu (hw-traced
          // 2026-07-09, [DOS MAP+] HL=0200 DE=0009 BC=0019).
          MemESP::romInUse = ESPectrum::trdos ? (MemESP::romLatch ? 1 : 0)
                                              : (MemESP::romLatch ? 3 : 2);
          MemESP::recoverPage0();
        } else if (Z80Ops::isScorpion) {
          // Live recompute like Profi's: the 1FFD D1 service override outranks
          // D4, and TR-DOS stays mapped while trdos is true.
          scorpionRomUpdate();
        } else if (!ia && !ESPectrum::trdos) {
          MemESP::romInUse = MemESP::romLatch;
        }
      }
      if (!ESPectrum::trdos) MemESP::recoverPage0();
      if (MemESP::videoLatch != bitRead(data, 3)) {
        MemESP::videoLatch = bitRead(data, 3);
        if (Z80Ops::isProfi && (portDFFD & 0x80)) {
          VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[6].direct() : MemESP::ram[4].direct();
          uint32_t clrPage = MemESP::videoLatch ? 58 : 56;
          uint32_t totPages = ram_pages + butter_pages + psram_pages + swap_pages;
          VIDEO::profi_clrmem = (clrPage < totPages) ? MemESP::ram[clrPage].direct() : nullptr;
#if PROFI_PORT_TRACE
          Debug::log("[DS80 FLIP] vl=%u dispPx=%u dispClr=%u PC=%04X",
              MemESP::videoLatch, MemESP::videoLatch ? 6u : 4u, clrPage, Z80::getRegPC());
          ds80_dbg_wr_cnt = 0;
          ds80_dbg_grmem  = VIDEO::grmem;
          ds80_dbg_clrmem = VIDEO::profi_clrmem;
#endif
        } else {
          VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();
          if (Z80Ops::isProfi) VIDEO::profi_clrmem = nullptr;
        }
        if (Config::gigascreen_onoff == 2) VIDEO::gigascreen_auto_countdown = 3;
        if (VIDEO::mode16col_enabled) VIDEO::mode16colUpdatePlanes();
      }
    }
#if PAGE_TRACE
    // Every 7FFD write with the state it LANDS IN. D4 (ROM select) is the one
    // that matters here: TR-DOS software that EI/HALTs needs ROM 1's
    // self-contained 0x0038 handler, and a write that fails to make romU
    // follow rom14 hands it the 128K ROM instead — whose handler trampolines
    // through a RAM stub at 0x5B00 the guest is entitled to have overwritten.
    // "BLOCKED" means pagingLock swallowed the write whole. Bounded so a
    // screen-flip demo hammering 7FFD cannot flood the UART.
    { static uint16_t pgw = 0;
      if (pgw < 300) { pgw++;
        Debug::log("[7FFD] w=%02X %s bank=%u rom14=%u romU=%u lock=%u dos=%u pc=%04X",
                   data, blocked ? "BLOCKED" : "ok", (unsigned)MemESP::bankLatch,
                   (unsigned)MemESP::romLatch, (unsigned)MemESP::romInUse,
                   (unsigned)MemESP::pagingLock, (unsigned)ESPectrum::trdos,
                   Z80::getRegPC()); } }
#endif
  }
}

// KR580VI53 (8253 PIT) register write — Byte computer synthesizer.
// Honors the control word's RW mode (1=LSB only, 2=MSB only, 3=LSB then MSB;
// 0 = counter-latch command, which must NOT disturb the running count) and the
// BCD bit: the Byte's built-in ROM test programs every melody note with
// control word #37/#77/#B7 = mode 3, BCD — a BCD count of "6902" is 6902
// decimal, not 0x6902, so counting it in binary played the dog waltz ~2
// octaves low and detuned the notes whose digits exceed 9 (#B6/#D8/#F5).
// Invalid BCD digits decrement through their face value on the real decade
// counters, so the nibble-weighted sum below matches hardware closely enough.
IRAM_ATTR void Ports::pitWrite(uint8_t a8, uint8_t data) {
  uint8_t synthPort = (a8 >> 5) & 3;
  if (synthPort == 3) {
    // Control register (0xEE) — parse 8253 control word
    uint8_t ch = (data >> 6) & 3;
    if (ch < 3) {
      uint8_t rw = (data >> 4) & 3;
      if (rw == 0)
        return; // counter-latch command: count keeps running
      PIT8253Channel &pit = pitChannels[ch];
      ESPectrum::PITGetSample(); // flush audio rendered with the old settings
      pit.active = false;
      pit.lsb_loaded = false;
      pit.counter = 0;
      pit.output = 0;
      pit.rw = rw;
      pit.bcd = (data & 1) != 0;
    }
    return;
  }
  // Data port (0x8E/0xAE/0xCE)
  PIT8253Channel &pit = pitChannels[synthPort];
  uint16_t raw = 0;
  bool load = false;
  switch (pit.rw) {
  case 1: // LSB only, MSB = 0
    raw = data;
    load = true;
    break;
  case 2: // MSB only, LSB = 0
    raw = (uint16_t)data << 8;
    load = true;
    break;
  default: // LSB then MSB (also rw==0 after reset: legacy behavior)
    if (!pit.lsb_loaded) {
      pit.lsb = data;
      pit.lsb_loaded = true;
    } else {
      raw = pit.lsb | ((uint16_t)data << 8);
      pit.lsb_loaded = false;
      load = true;
    }
    break;
  }
  if (load) {
    ESPectrum::PITGetSample();
    pit.count_value = pit.bcd
        ? ((raw >> 12) & 0xF) * 1000 + ((raw >> 8) & 0xF) * 100 +
          ((raw >> 4) & 0xF) * 10 + (raw & 0xF)
        : raw;
    pit.counter = 0;
    pit.output = 1;
    pit.active = pit.count_value >= 2;
  }
}

// KR580VI53 (8253 PIT) square wave generator
// PIT clock = CPU clock = 3.5 MHz (verified: divisor 5602 → 624.7 Hz)
// Mode 3: output toggles every count_value/2 PIT clock ticks
// Optimized: analytical high_count instead of tick-by-tick simulation
IRAM_ATTR void Ports::pitGenSound(uint8_t *buf, int bufsize) {
  const int TICKS = ESPectrum::audioAYDivider; // ~112 (3.5 MHz / 31.25 kHz)
  const int AMP = 28;

  while (bufsize-- > 0) {
    int mix = 0;
    for (int ch = 0; ch < 3; ch++) {
      PIT8253Channel &pit = pitChannels[ch];
      if (!pit.active || pit.count_value < 2)
        continue;

      int half = pit.count_value >> 1;
      int ticks_left = TICKS;
      int high = 0;

      // Advance analytically: loop only runs once per output toggle
      // (typically 1-2 times vs old 112 iterations)
      while (ticks_left > 0) {
        int until_toggle = half - pit.counter;
        if (until_toggle > ticks_left) {
          // No toggle in remaining ticks
          if (pit.output)
            high += ticks_left;
          pit.counter += ticks_left;
          ticks_left = 0;
        } else {
          // Toggle happens
          if (pit.output)
            high += until_toggle;
          ticks_left -= until_toggle;
          pit.counter = 0;
          pit.output ^= 1;
        }
      }
      mix += high * AMP / TICKS;
    }
    *buf++ = mix;
  }
}

IRAM_ATTR void Ports::ioContentionLate(bool contend) {
  if (contend) {
    VIDEO::Draw(1, true);
    VIDEO::Draw(1, true);
    VIDEO::Draw(1, true);
  } else {
    VIDEO::Draw(3, false);
  }
}

// DMA I/O: no contention, only side effects (border, AY, beeper)
IRAM_ATTR void Ports::dmaOutput(uint16_t address, uint8_t data) {
    if ((address & 0x0001) == 0) {
        // ULA port (0xFE): border + beeper
        port254 = data;
        if (VIDEO::borderColor != (data & 0x07)) {
            VIDEO::brdChange = true;
            VIDEO::DrawBorder();
            VIDEO::borderColor = data & 0x07;
            if (VIDEO::ulaplus_enabled)
                VIDEO::ulaPlusUpdateBorder();
            else
                VIDEO::updateBorderBrd();
        }
        int Audiobit;
        Audiobit = speaker_values[((data >> 2) & 0x04) | (Tape::tapeEarBit << 1) |
                                    ((data >> 3) & 0x01)];
        if (Audiobit != ESPectrum::lastaudioBit) {
            ESPectrum::BeeperGetSample();
            ESPectrum::lastaudioBit = Audiobit;
        }
    } else if ((ESPectrum::AY_emu) && ((address & 0x8002) == 0x8000)) {
        // AY. Same NedoPC-latch / old-TS-address decode as Ports::output, minus
        // the #FFFD latch write itself (a DMA burst to the register port is a
        // register stream, not a chip-select sequence).
        ayPortWrite(address, data, false);
    }
    // MB-02+ FDC: DMA writes to WD2797 data port (#6F)
    if (MB02::enabled) {
        uint8_t lo = address & 0xFF;
        if ((lo & 0x9F) == 0x0F) {
            for (int i = 0; i < 1000; i++) {
                rvmWD1793Step(&ESPectrum::mb02_fdd, 1);
                if (ESPectrum::mb02_fdd.control & kRVMWD177XDRQ) break;
            }
            rvmWD1793Write(&ESPectrum::mb02_fdd, (lo >> 5) & 3, data);
        }
    }
}

IRAM_ATTR uint8_t Ports::dmaInput(uint16_t address) {
    // DMA read from I/O: return port value without contention
    if ((address & 0x0001) == 0) {
        // ULA port: keyboard + ear
        return 0xFF; // no keys pressed
    }
    // MB-02+ FDC: DMA reads from WD2797 data port (#6F)
    if (MB02::enabled) {
        uint8_t lo = address & 0xFF;
        if ((lo & 0x9F) == 0x0F) {
            // Step FDC until DRQ is set or timeout/command complete
            bool got_drq = false;
            for (int i = 0; i < 1000; i++) {
                rvmWD1793Step(&ESPectrum::mb02_fdd, 1);
                if (ESPectrum::mb02_fdd.control & kRVMWD177XDRQ) { got_drq = true; break; }
                // If FDC command completed (INTRQ set, not busy) → no more data
                if ((ESPectrum::mb02_fdd.control & kRVMWD177XINTRQ) &&
                    !(ESPectrum::mb02_fdd.status & kRVMWD177XStatusBusy)) break;
            }
            if (!got_drq) {
                // No more data — abort DMA transfer
                Z80DMA::transfer_active = false;
                return 0xFF;
            }
            return rvmWD1793Read(&ESPectrum::mb02_fdd, (lo >> 5) & 3);
        }
    }
    return 0xFF;
}
