// pico-speccy — «Байт» (Byte) port handling that used to sit inline in the
// RAM-resident Ports::input/output: its DD10/DD11-driven I/O contention above
// #C000, the fully decoded output ports and the KR580VI53 (8253) synthesizer.
// FLASH-resident: see CLAUDE.md "Machine code must not live in the SHARED RAM hot
// paths". The PIT's per-sample generator (Ports::pitGenSound) is not here.
#pragma once
#include <stdint.h>

namespace Byte {

// Early I/O contention for a port address >= #C000 (the DD10/DD11 PROM table).
void ioContention(uint16_t address);

// OUT to anything but #FE: the PIT at #8E/#AE/#CE/#EE, everything else swallowed
// (the Byte decodes its ports fully). Always applies the late I/O contention.
void portWrite(uint8_t a8, uint8_t data);

}
