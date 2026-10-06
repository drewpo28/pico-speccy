// pico-speccy — Scorpion-family ports. See Scorpion.h. FLASH-resident on purpose.
#include "Scorpion.h"

#include "speccy/z80/CPU.h"
#include "app/Config.h"
#include "app/ESPectrum.h"
#include "ui/LEDIndicators.h"
#include "speccy/core/MemESP.h"
#include "ui/OSDMain.h"
#include "speccy/core/Ports.h"
#include "speccy/z80/z80.h"
#include "speccy/video/Video.h"

extern int ram_pages, butter_pages, psram_pages, swap_pages;

// Flash, not RAM: Scorpion-family paging only (Scorpion/GMX/ProfROM/KAY/Phoenix),
// called from the RAM-resident output() on a paging write — inlined there it cost
// every machine the KAY and GMX arithmetic in SRAM.
uint32_t Scorpion::c000Page(uint32_t low3) {
  if (g_scorp_kay) {
    // Nemo KAY (UnrealSpeccy MM_KAY, z00m128/kay1024 README): 7FFD 0-2, then
    // 1FFD D4 = +8 (256K), 1FFD D7 = +16 (512K), 7FFD D7 = +32 (1024K). Phoenix
    // (ZXM-Phoenix, Unreal MM_PHOENIX) orders them differently and adds 1FFD D6:
    // 7FFD D7 -> bit 3, 1FFD D4 -> bit 4, 1FFD D7 -> bit 5, 1FFD D6 -> bit 6 (2 MB).
    uint32_t page = low3;
    if (g_scorp_kay == 4)
      page |= ((Ports::kay7FFDd7 & 0x80) >> 4) | (Ports::port1FFD & 0x50) |
              ((Ports::port1FFD & 0x80) >> 2);
    else {
      page |= (Ports::port1FFD & 0x10) >> 1;
      if (g_scorp_kay >= 3) page |= ((Ports::port1FFD & 0x80) >> 3) | ((Ports::kay7FFDd7 & 0x80) >> 2);
    }
    uint32_t pages = ram_pages + butter_pages + psram_pages + swap_pages;
    if (page >= pages) page = low3;
    return page;
  }
  uint32_t page = low3 | ((Ports::port1FFD & 0x10) >> 1);
  if (g_scorp_gmx) page |= (uint32_t)(Ports::portDFFDgmx & 0x07) << 4;
  // ZS-1024: 1FFD D6,D7 are two more page bits above D4 — 64 pages = 1 MB
  // (MAME scorpion_update_memory `(1ffd & 0xc0) >> 2`; ZXMAK2
  // MemoryScorpionProfRom1024 `sega |= (CMR1 & 0xC0) >> 5`). 256K boards leave
  // the bits unwired — modelled by NOT composing them there (the bounds W/A
  // below would catch stray writes anyway).
  if (g_scorp_1024) page |= (uint32_t)(Ports::port1FFD & 0xC0) >> 2;
  uint32_t pages = ram_pages + butter_pages + psram_pages + swap_pages;
  if (page >= pages) page = low3;
  return page;
}

// Nemo KAY / ZXM-Phoenix #1FFD write (decode in output()). FLASH, not RAM.
void Scorpion::kay1FFDWrite(uint8_t data) {
  LED::touchW(LED::RAM);
  Ports::port1FFD = data;
  uint32_t page = Scorpion::c000Page(MemESP::bankLatch & 0x07);
  if (page != MemESP::bankLatch) {
    MemESP::bankLatch = page;
    MemESP::ramContended[3] = false;
    MemESP::ramCurrent[3] = MemESP::ram[page].sync(3);
  }
  MemESP::page0ram = data & 0x01;
  Ports::kayTurboUpdate();
  Ports::scorpionRomUpdate();
}

void Scorpion::write1FFD(uint16_t address, uint8_t data) {
  (void)address;   // only the GMX_TRACE line reads it
  LED::touchW(LED::RAM);
#if GMX_TRACE
  // D4-only changes are skipped: the GMX loader's RAM sizing toggles D4 (the +8
  // page bit) hundreds of times per boot pass (pc=6A78 in the logs) and that
  // flood alone burns the whole 600-line budget before anything interesting
  // happens. D0 (RAM0), D1 (service ROM) and D2 (GMX DOS page) are what the
  // paging questions are about, so trip on those.
  if ((data & 0xEF) != (Ports::port1FFD & 0xEF))
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
  if (g_scorp_gmx && (Ports::port1FFD & 0x04) && !(data & 0x04) &&
      Z80::getRegPC() >= 0x4000)
    ESPectrum::trdos = false;
  Ports::port1FFD = data;
  uint32_t page = Scorpion::c000Page(MemESP::bankLatch & 0x07);
  if (page != MemESP::bankLatch) {
    MemESP::bankLatch = page;
    MemESP::ramContended[3] = false;
    MemESP::ramCurrent[3] = MemESP::ram[page].sync(3);
  }
  MemESP::page0ram = data & 0x01;
  Ports::scorpionRomUpdate();   // D1 service / GMX D2 DOS / TR-DOS / romLatch + recoverPage0
}

uint8_t Scorpion::turboPlusRead(uint16_t address) {
  const uint8_t want = (address & 0x4000) ? 1 : 0;   // scale 1<<turbo = 3.5 / 7
  if (want != ESPectrum::multiplicator) {
    ESPectrum::multiplicator = want;
    CPU::updateStatesInFrame();
    OSD::notifyClock(want ? " CPU: 7 MHz " : " CPU: 3.5 MHz ");
  }
  return 0xFF;
}

// ── .pss snapshot ─────────────────────────────────────────────────────────────

uint32_t Scorpion::ramPages() {
  if (g_scorp_gmx || g_scorp_kay == 4) return 128;
  if (g_scorp_1024 || g_scorp_kay == 3) return 64;
  return 16;
}

static constexpr uint8_t SNAP_VER = 1;

uint32_t Scorpion::snapSave(uint8_t* out) {
  uint32_t n = 0;
  out[n++] = SNAP_VER;
  out[n++] = Ports::kay7FFDd7;
  out[n++] = Ports::gmxPort00;
  out[n++] = Ports::gmxPort78FD;
  out[n++] = Ports::gmxPort7EFD;
  out[n++] = Ports::gmxScrollLo;
  out[n++] = Ports::gmxScrollHi;
  out[n++] = Ports::gmxPlane;
  out[n++] = Ports::gmxMagicShift;
  out[n++] = Ports::portDFFDgmx;
  out[n++] = Ports::smucSys;
  out[n++] = Ports::smucFdd;
  return n;
}

void Scorpion::snapLoad(const uint8_t* in, uint32_t n) {
  if (n < 12 || in[0] < 1) return;
  Ports::kay7FFDd7     = in[1];
  Ports::gmxPort00     = in[2];
  Ports::gmxPort78FD   = in[3];
  Ports::gmxPort7EFD   = in[4];
  Ports::gmxScrollLo   = in[5];
  Ports::gmxScrollHi   = in[6];
  Ports::gmxPlane      = in[7];
  Ports::gmxMagicShift = in[8];
  Ports::portDFFDgmx   = in[9];
  Ports::smucSys       = in[10];
  Ports::smucFdd       = in[11];
}

void Scorpion::snapRemap() {
  const uint32_t pages = ram_pages + butter_pages + psram_pages + swap_pages;
  uint32_t c000 = MemESP::bankLatch;
  if (c000 >= pages) c000 = MemESP::bankLatch & 7;
  MemESP::bankLatch = c000;
  MemESP::ramCurrent[3] = MemESP::ram[c000].sync(3);
  MemESP::ramContended[3] = false;
  if (g_scorp_gmx) {
    // #78FD pages the 0x8000 window (page = value ^ 2); 0 after a reset = page 2.
    uint32_t pg = Ports::gmxPort78FD ^ 2;
    if (pg >= pages) pg = 2;
    MemESP::ramCurrent[2] = MemESP::ram[pg].sync(2);
    MemESP::ramContended[2] = false;
    const uint8_t mult = (Ports::gmxPort7EFD & 0x80) ? 1 : 0;   // #7EFD D7 = 7 MHz
    if (mult != ESPectrum::multiplicator) {
      ESPectrum::multiplicator = mult;
      CPU::updateStatesInFrame();
    }
    VIDEO::gmxExtRequest((Ports::gmxPort7EFD & 0x08) != 0);
  }
  Ports::kayTurboUpdate();
  Ports::scorpionRomUpdate();   // romInUse from #1FFD / DOS / ROM latch / plane, page 0
}
