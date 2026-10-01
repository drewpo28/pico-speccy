// pico-speccy — Pentagon 512/1024SL ports. See Pentagon.h. FLASH-resident on purpose.
#include "Pentagon.h"

#include "speccy/z80/CPU.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/ESPectrum.h"
#include "ui/LEDIndicators.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
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
