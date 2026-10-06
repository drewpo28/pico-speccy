// pico-speccy — Pentagon 512/1024SL ports. See Pentagon.h. FLASH-resident on purpose.
#include "Pentagon.h"

#include "speccy/z80/CPU.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/ESPectrum.h"
#include "ui/LEDIndicators.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/devices/disk/MB02.h"
#include "speccy/devices/storage/DivMMC.h"
#include "speccy/video/Video.h"
#include "speccy/z80/z80.h"

void Pentagon::eff7Video(uint8_t data) {
  // Debug::log("[EFF7] pc=0x%04X data=0x%02X (4BPP=%d 512=%d LOCK=%d GIGA=%d HWMC=%d CMOS=%d)",
  //            Z80::getRegPC(), data, !!(data & 0x01), !!(data & 0x02), !!(data & 0x04),
  //            !!(data & 0x10), !!(data & 0x20), !!(data & 0x80));
  Ports::portEFF7 = data;
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

void Pentagon::eff7Paging(uint8_t data) {
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

uint8_t Pentagon::hiddenRam(uint8_t p8) {
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
  return 0xFF;
}

// ── 16col whole-line path ─────────────────────────────────────────────────────
// The MainScreen branch for 16col costs an indirect Draw call on EVERY guest
// memory access (~45 ARM instructions per byte against ~12 on the fast path);
// the 32-column loop itself is nothing. So while #EFF7 D0 is set the content
// is rendered one whole line at a time at the END of its paper (VIDEO::
// col16DrawTick, the ATM/Evo shape) and the TS-Conf fast memory path runs.
// Same bytes as MainScreen's branch: pixels 0..3 = L(a) R(a) L(b) R(b), stored
// as bytes [2,3,0,1] (the AluByte x^2 convention the scanout ISR expects).
void Pentagon::col16RenderLine(uint32_t* dst, uint16_t off, const uint8_t* const planes[4],
                               const uint16_t* lut) {
  const uint8_t* pA = planes[0] + off;
  const uint8_t* pB = planes[1] + off;
  const uint8_t* pC = planes[2] + off;
  const uint8_t* pD = planes[3] + off;
  for (int j = 0; j < 32; j += 2) {
    uint32_t la = lut[pA[j]], lb = lut[pB[j]], lc = lut[pC[j]], ld = lut[pD[j]];
    dst[0] = lb | (la << 16);
    dst[1] = ld | (lc << 16);
    la = lut[pA[j + 1]]; lb = lut[pB[j + 1]]; lc = lut[pC[j + 1]]; ld = lut[pD[j + 1]];
    dst[2] = lb | (la << 16);
    dst[3] = ld | (lc << 16);
    dst += 4;
  }
}

extern int ram_pages, butter_pages, psram_pages, swap_pages;

bool Pentagon::col16FastMemOk() {
  // Every page must be a plain POINTER: an SPI-PSRAM or SD-swap page is served
  // per byte through the accessor window (ramCurrent[pg] == nullptr), which the
  // fast path would dereference.
  if (psram_pages != 0 || swap_pages != 0) return false;
  // DivMMC / MB-02: the automap is decided on every opcode fetch (preOpcFetch),
  // which the fast fetch skips — not merely "is it mapped right now".
  if (DivMMC::enabled || MB02::enabled) return false;
  // Page 0 is read as a raw pointer on the fast path, i.e. the overlay registry
  // is bypassed: refuse while any ROM this machine can map there (rom[0..4] —
  // ROM0/48K/Gluk/TR-DOS, romInUse selects among them mid-frame via the #3Dxx
  // trap) carries a registered overlay. Pentagon's bases + TR-DOS 5.04T are raw;
  // 5.03 / 5.04TM / 5.05D / 6.11e are overlays and keep the generic path.
  if (MemESP::overlayCount != 0) {
    for (int i = 0; i <= 4; i++) {
      const uint8_t* p = MemESP::rom[i].direct();
      if (p && MemESP::overlayFor(p)) return false;
    }
  }
  for (int pg = 0; pg < 4; pg++)
    if (MemESP::ramContended[pg]) return false;   // never on a Pentagon; cheap insurance
  return true;
}
