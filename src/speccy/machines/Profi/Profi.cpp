// pico-speccy — Profi / Karabas-Pro ports. See Profi.h. FLASH-resident, as the
// Ports::inputImpl<true>/outputImpl<true> instance these blocks came from.
#include "Profi.h"

#include <string.h>
#include "speccy/z80/CPU.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/ESPectrum.h"
#include "speccy/devices/storage/IDE.h"
#include "ui/LEDIndicators.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/video/Video.h"
#include "speccy/z80/z80.h"
#include "speccy/devices/disk/wd1793.h"

#ifndef bitRead
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#endif

extern int ram_pages, butter_pages, psram_pages, swap_pages;
extern volatile bool profi_ds80_active;   // drivers (hdmi.c / vga.c)
#if FDD_PORT_TRACE
void checkPagingStuck(uint16_t pc);   // Ports.cpp
#endif

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

// CP/M DSKKE9A no-disk re-issue counter (Ports.cpp owns it: its FDC stub resets it).
extern int profi_nodisk_reissue_cnt;

void Profi::writeDFFD(uint8_t data) {
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
    Ports::portDFFD = data;
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
      if (!profi_ds80_active && !VIDEO::profi_ds80_activate_pending) {
          VIDEO::profi_ds80_deactivate_pending = false; // cancel any pending off
          VIDEO::profi_ds80_activate_pending   = true;
      } else if (profi_ds80_active) {
          // DS80 already active — cancel any spurious deactivation queued by a
          // preceding bit7=0 write in the same Z80 frame (e.g. sea-viewer does
          // OUT (#FD),0x00  ; "reset" Ports::portDFFD before reprogramming banks
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

bool Profi::ideRead(uint16_t address, uint8_t* out) {
  bool cpm = (Ports::portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
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
        { *out = rv; return true; }
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
        { *out = rv; return true; }
      }
#if IDE_PORT_TRACE
      Debug::log("[IDE RD] pc=%04X port=%02X reg=%d CS3 (unhandled)",
                 Z80::getRegPC(), (unsigned)p1, reg);
#endif
    }
  }
  return false;
}

bool Profi::ideWrite(uint16_t address, uint8_t data) {
  bool cpm = (Ports::portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
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
          return true;
        }
        // A5=1 = #xxEB: write register or 16-bit data
        if (reg == 0)                            // data register: combine with latch
          IDE::write_data_low(data);             // latch_write is HIGH byte
        else
          IDE::write8(reg, data);
        return true;
      }
      // CS3 (A6=0) = #xxAB reg6: ATA device control (0x3F6, SRST/nIEN).
      // MBOOTHDD issues the ATA soft-reset via OUT (#06AB),A — port 0xAB has
      // A5=0, so do NOT gate on A5 (the old `p1&0x20` check dropped the reset).
      if (reg == 6) {
        LED::touchW(LED::IDE);
        IDE::write8(8, data);
        return true;
      }
    }
  }
  return false;
}

bool Profi::extRead(uint16_t address, uint8_t* out) {
  if (address == 0x028B) {
    // TURBO_MODE (bits 5-6) is LIVE state, not a stored latch: ROMain's
    // status bar polls IN #028B & 0x60 every frame and redraws its
    // "Turbo:" field on change (rt 0x0592), and the FPGA hotkeys change
    // the same latch on real hardware. Reflect our CPU turbo
    // (multiplicator 0..3 = 3.5/7/14/28 MHz) so the guest sees the truth;
    // the remaining bits stay stub-stored (see the caveat above).
    uint8_t v = (Ports::port028B & ~0x60) | ((ESPectrum::multiplicator & 3) << 5);
#if PROFI_PORT_TRACE
    Debug::log("[8B IN] #028B -> %02X pc=%04X", v, Z80::getRegPC());
#endif
    { *out = v; return true; }                     // unconditional (no CPM/ROM14/DOS gate)
  }
  bool cpm = (Ports::portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
#if PROFI_PORT_TRACE
  if (address == 0x008B || address == 0x018B)
    Debug::log("[8B IN probe] addr=%04X cpm=%d rom14=%d dos=%d pc=%04X",
               address, cpm, rom14, dos, Z80::getRegPC());
#endif
  if ((cpm && rom14) || (dos && !rom14)) {
    if (address == 0x008B) { *out = Ports::port008B; return true; }
    if (address == 0x018B) { *out = Ports::port018B; return true; }
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
    if ((Ports::serialMouseCtl & 0x04) || Config::romSet != R_PROFI_PQ) {
      if (lo8 == 0xF3) {              // VV51 status register
        smTryBuildPacket();
        uint8_t st = (uint8_t)(0x05 | (sm_rxrdy ? 0x02 : 0x00)); // TxRDY+TxE | RxRDY
#if FDD_PORT_TRACE
        static uint16_t smLastPc = 0xFFFF; static uint8_t smLastSt = 0xFF;
        if (Z80::getRegPC() != smLastPc || st != smLastSt) {
          smLastPc = Z80::getRegPC(); smLastSt = st;
          Debug::log("[VV51 IN] F3 st=%02X ctl=%02X inten=%d pc=%04X",
                     st, Ports::serialMouseCtl, (int)Ports::serialMouseIntEn, smLastPc);
        }
#endif
        { *out = st; return true; }
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
      { *out = v; return true; }
    }
    bool hasKey = (Ports::pqkHead != Ports::pqkTail);
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
      { *out = hasKey ? 0x02 : 0x00; return true; }  // bit1 = key ready; never 0xFF (present)
    // #D3: pop next scancode
    if (!hasKey) { *out = 0x00; return true; }
    uint8_t s = Ports::pqkBuf[Ports::pqkTail];
    Ports::pqkTail = (Ports::pqkTail + 1) & 0x0F;
#if FDD_PORT_TRACE
    Debug::log("[PQKBD POP] scan=%02X", s);
#endif
    { *out = s; return true; }
  }
  return false;
}

bool Profi::extWrite(uint16_t address, uint8_t data) {
  if (address == 0x028B) {
    Ports::port028B = data;
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
    return true;
  }
  bool cpm = (Ports::portDFFD & 0x20), rom14 = MemESP::romLatch, dos = ESPectrum::trdos;
#if PROFI_PORT_TRACE
  if (address == 0x008B || address == 0x018B)
    Debug::log("[8B OUT probe] addr=%04X data=%02X cpm=%d rom14=%d dos=%d pc=%04X",
               address, data, cpm, rom14, dos, Z80::getRegPC());
#endif
  if ((cpm && rom14) || (dos && !rom14)) {
    if (address == 0x008B) {
      uint8_t prev = Ports::port008B;
      Ports::port008B = data;
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
      return true;
    }
    if (address == 0x018B) { Ports::port018B = data; return true; }
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
      if (lo8o == 0xF3) Ports::serialMouseCtl = data;
      else if (lo8o == 0xB3 || lo8o == 0x93) Ports::serialMouseIntEn = data & 0x01;
#if FDD_PORT_TRACE
      static uint16_t lastPc = 0xFFFF; static uint8_t lastData = 0, lastLo = 0;
      uint16_t pcn = Z80::getRegPC();
      if (pcn != lastPc || data != lastData || lo8o != lastLo) {
        lastPc = pcn; lastData = data; lastLo = lo8o;
        Debug::log("[PQKBD OUT] port=%02X data=%02X cpm=%d rom14=%d dos=%d pc=%04X",
                   lo8o, data, (int)cpm, (int)rom14, (int)dos, pcn);
      }
#endif
      return true;
    }
  }
  return false;
}

bool Profi::fdcNoDiskBreak(uint16_t address) {
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
          uint16_t lo = MemESP::dbgPeek(sp);
          uint16_t hi = MemESP::dbgPeek(sp + 1);
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
        return true;  // caller skips rvmWD1793Write
      }
    } else {
      profi_nodisk_reissue_cnt = 0;
    }
  }
  return false;
}

#if FDD_PORT_TRACE
void Profi::fdcInProbe(uint16_t address) {
  uint8_t lo8f = address & 0xFF;
  if (lo8f == 0x1F || lo8f == 0x3F || lo8f == 0x5F || lo8f == 0x7F ||
      lo8f == 0x83 || lo8f == 0xA3 || lo8f == 0xC3 || lo8f == 0xE3 ||
      lo8f == 0xFF || lo8f == 0xBF) {
    bool has_any_disk_p = ESPectrum::fdd.disk[ESPectrum::fdd.diskS] != nullptr;
    bool skip_p = (MemESP::romInUse == 0 && !has_any_disk_p);
    uint16_t pcNow = Z80::getRegPC();
    bool cpmNow = (Ports::portDFFD & 0x20) != 0;
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
#endif

#if FDD_PORT_TRACE
void Profi::fdcOutProbe(uint16_t address, uint8_t data) {
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
    bool cpmNow = (Ports::portDFFD & 0x20) != 0;
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
#endif

