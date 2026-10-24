// pico-speccy — SAM Coupe clocks on port #EF (SimCoupe Base/Clock.cpp).
//
// SAMBUS clock (high byte < #FE): 16 nibble registers, index = A15..A12 — 00..0B
// the time/date digits (ones, tens pairs: seconds, minutes, hours, day, month,
// year), 0C day of week, 0D control (bit 1 = hold), 0F mode. Dallas DS12887
// (high byte >= #FE): A8 = 0 selects the register, A8 = 1 reads/writes it —
// bank 0 (0..0D clock + control A-D, 0E..7F user RAM) and the DS17887 bank 1
// (register 0A bit 4; model/serial read-only, century at 48). Both keep their
// own SAMTIME (BCD or binary per control B) advanced by the wall clock, seeded
// from RTC::now() at reset — a guest write to a time register sticks until the
// next tick touches that digit, as on the chips. Not modelled: the DS17887's
// 8 KB extended RAM (reads #FF), alarms/periodic interrupts, battery backing
// (the register files are session-only).
#pragma once

#include <stdint.h>

namespace SamClock {
    void    reset();                       // seed both clocks from the RTC
    void    flush();                       // ESPectrum::loop: the Dallas user RAM to its file, debounced
    uint8_t in(uint16_t address);
    void    out(uint16_t address, uint8_t v);
}
