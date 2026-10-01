// pico-speccy — ZX Spectrum +3 ports. See Plus3.h. FLASH-resident on purpose.
#include "Plus3.h"

#include "ui/LEDIndicators.h"
#include "speccy/core/MemESP.h"
#include "Plus3Fdc.h"
#include "speccy/core/Ports.h"
#include "speccy/video/Video.h"

#ifndef bitRead
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#endif

bool Plus3::portWrite(uint16_t address, uint8_t data) {
  if ((address & 0xC002) == 0x4000) {
    ++Ports::port7ffd_cnt;
    LED::touchW(LED::RAM);
    if (MemESP::pagingLock) return true;
    MemESP::pagingLock = bitRead(data, 5);
    MemESP::bankLatch  = data & 0x07;
    MemESP::romLatch   = bitRead(data, 4);
    if (MemESP::videoLatch != bitRead(data, 3)) {
      MemESP::videoLatch = bitRead(data, 3);
      VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();
      VIDEO::gigascreenAutoFlip();          // Gigascreen Auto: the displayed page changed
    }
    MemESP::plus3Remap(Ports::port1FFD);
    return true;
  }
  if ((address & 0xF002) == 0x1000) {
    // D3 (both drive motors) and D4 (printer strobe) are NOT gated by the paging
    // lock — only the memory bits are — so the motor is driven before the lock test.
    Plus3Fdc::writeAux(data);
    if (MemESP::pagingLock) return true;
    Ports::port1FFD = data;
    MemESP::plus3Remap(data);
    return true;
  }
  // The FDC data register. #2FFD is read-only, so a write there is swallowed.
  if ((address & 0xF002) == 0x3000) { Plus3Fdc::writeData(data); return true; }
  if ((address & 0xF002) == 0x2000) return true;
  return false;
}
