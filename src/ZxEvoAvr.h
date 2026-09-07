#pragma once

#include <stdint.h>

// ZX-Evo AVR keyboard controller — the half of the Gluk RTC interface that is
// NOT an MC146818, live on TS-Conf only (reference: tslabs/zx-evo
// pentevo/avr/current rtc.c / version.c / ps2.c / config.c).
//
// The Evo's Gluk clock is served by the AVR, and it repurposes registers the
// real chip does not have:
//   0x0C write: b0 = clear the PS/2 log, b1 = toggle CAPS LED, b7 = toggle
//               EEPROM mode for 0xF0..0xFF; read adds SD detect (b3), NUM LED (b0)
//   0x0D read : PS/2 modifier statuses (lctrl rctrl lalt ralt lshift rshift f12)
//   0x0E read : lwin rwin menu
//   0xF0..0xFF: the "version extension" — a write to 0xF0 (or any 0xFx while
//               no interface owns the window) selects a TYPE, reads return that
//               type's 16-byte window: 0 = baseconf version, 1 = bootloader
//               version, 2 = PS/2 SCANCODE LOG (raw set-2 bytes, 16-entry ring,
//               0 = empty, 0xFF = overflow), 3 = modes register, 0x0E = the
//               configuration interface (pad mode / keymaps / autofire / protect /
//               command), 0x10 = SPI flash (stub). With reg C b7 set the same
//               window is a 4 KB EEPROM at (reg A << 4) | (index & 15).
//
// avrconf.spg — and anything else written against the Evo's own keyboard path —
// reads keys as PS/2 scancodes out of type 2 and never touches port #FE, so
// without this model it starts and then no key does anything (hw 2026-09-07).
namespace ZxEvoAvr {
    void    reset();                                   // AVR power-up state (TS-Conf cold reset)

    // Gluk register file hooks (RTC.cpp, TS-Conf only)
    uint8_t readExt(uint8_t idx, uint8_t regA);        // IN  reg 0xF0..0xFF
    void    writeExt(uint8_t idx, uint8_t v, uint8_t regA); // OUT reg 0xF0..0xFF
    void    writeRegC(uint8_t v);                      // OUT reg 0x0C
    uint8_t regCBits();                                // ORed into reg C reads
    uint8_t regD();                                    // IN  reg 0x0D
    uint8_t regE();                                    // IN  reg 0x0E

    // Keyboard feed (main.cpp process_kbd_report): HID usage codes in, PS/2
    // set-2 make/break sequences into the log. No-ops off TS-Conf.
    void    hidKey(uint8_t hid, bool down);
    void    hidModifiers(uint8_t now, uint8_t prev);
}
