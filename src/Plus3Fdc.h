// Plus3Fdc — the board side of the ZX Spectrum +3 disk interface.
//
// Upd765 and DskImage are deliberately free of firmware dependencies so they can be
// tested on a host; this is where they meet FatFs, the Buffer pool and Ports. Shaped
// like MB02 / DivMMC / IDE: a namespace of statics, lazy, and costing nothing until a
// disk is actually mounted.
//
// Ports (Fuse machines/machines_periph.c plus3_memory_ports / upd765_ports):
//   #2FFD  (addr & 0xF002) == 0x2000   main status register, read only
//   #3FFD  (addr & 0xF002) == 0x3000   data register, read and write
//   #1FFD  D3                          motor for BOTH drives (writeAux)

#pragma once

#include <string>
#include <stdint.h>

#include "Upd765.h"

namespace Plus3Fdc {

// The controller. Public so the LED indicators and the FDD sound can read its activity
// counters, exactly as they read ESPectrum::fdd's.
extern Upd765 fdc;

// Bring the interface up for the running machine. Cheap when the machine is not a +3:
// it resets the controller struct and allocates nothing.
void init();
void reset();

// Advance seeks and the transfer watchdog once per frame, so a guest sitting in HALT
// still sees a seek complete. The port handlers pump it too.
void frameTick();
// Flush the FDD_PORT_TRACE command run (no-op unless that option is on).
void traceFlush();

uint8_t readStatus();          // IN  #2FFD
uint8_t readData();            // IN  #3FFD
void    writeData(uint8_t d);  // OUT #3FFD
void    writeAux(uint8_t d);   // OUT #1FFD — only D3 (motor) concerns the FDC

// Drive A: is unit 0, drive B: is unit 1.
bool          mount(uint8_t unit, const std::string& fname);
void          eject(uint8_t unit);
bool          mounted(uint8_t unit);
const std::string& fname(uint8_t unit);
void          setWriteProtect(uint8_t unit, bool wp);

// Write a blank, formatted +3 disk image (40 tracks, 9 x 512, ~190 KB single-sided).
bool createBlank(const std::string& path, bool doubleSided);

} // namespace Plus3Fdc
