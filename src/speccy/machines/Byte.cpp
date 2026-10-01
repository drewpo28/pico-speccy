// pico-speccy — «Байт» ports. See Byte.h. FLASH-resident on purpose.
#include "Byte.h"

#include "speccy/z80/CPU.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/video/Video.h"

void Byte::ioContention(uint16_t address) {
  // вместо VIDEO::Draw(1, MemESP::ramContended[rambank]);
  // добавляем задержку через таблицу MemESP
  int delay = MemESP::getByteContention(address);
  VIDEO::Draw(delay, true);
}

void Byte::portWrite(uint8_t a8, uint8_t data) {
  if ((a8 & 0x9F) == 0x8E) Ports::pitWrite(a8, data);
  VIDEO::Draw(3, true); // I/O Contention (Late) — the Byte is contended
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
void Ports::pitWrite(uint8_t a8, uint8_t data) {
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
