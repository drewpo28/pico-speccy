// pico-speccy — DivIDE: the ATA port decode of the classic 8-bit-bus IDE card.
//
// The card this describes is the one the "+3 (divIDE)" romset talks to: Garry
// Lancaster's +3e ROM built for a divIDE interface (the `div` build of p3eroms)
// drives IDEDOS over these ports instead of over the +3e's own "simple 8-bit"
// #xxEF window (Plus3eIde.h). The device behind the decode is IDE.cpp and the
// wiring is in Ports.cpp; the decode lives here so tools/divide_ide_test.cpp can
// check the shipped arithmetic rather than a copy of it.
//
// Reference: Fuse `peripherals/ide/divide.c` (libretro/fuse-libretro mirror —
// the upstream site is not reachable from this environment), which is also what
// the Workbench author's setup guide targets. Fuse registers exactly two port
// windows, as {mask, value} pairs over the FULL 16-bit port:
//
//     { 0x00e3, 0x00a3, divide_ide_read, divide_ide_write }   // ATA registers
//     { 0x00ff, 0x00e3, NULL,            divide_control_write } // control, W/O
//
// so the high address byte is not decoded at all, and the register number is
// carried by A2/A3/A4 of the low byte — its own port_to_ide_register() is the
// straight table
//
//     #A3 data   #A7 error/features  #AB sector count  #AF sector
//     #B3 cyl lo #B7 cyl hi          #BB device/head   #BF command/status
//
// Two properties of this interface that the rest of the firmware depends on:
//
//  * THE DATA BUS IS 16 BITS (libspectrum LIBSPECTRUM_IDE_DATA16): the card has
//    the high-byte latch in hardware, so the guest pulls a whole 512-byte sector
//    through the ONE data port as 512 consecutive accesses — low, high, low...
//    That is IDE::read8(0)/write8(0) with IDE::eight_bit false, i.e. the default,
//    and it is why a divIDE image is a FULL-sector .hdf (HDF flags bit 0 clear)
//    where an IDEDOS image for the +3e's 8-bit interface is a half-sector one.
//
//  * NO EPROM AND NO AUTOMAP. A real divIDE also pages its own 8 KB EPROM plus
//    32 KB of RAM over 0x0000-0x3FFF, and Fuse models that (divxxx.c) — but only
//    when CONMEM is set or the EPROM is write-protected: `divxxx_refresh_page_state`
//    ignores the automap flag entirely while `divide_wp` is clear, and clear is
//    both Fuse's default (settings.dat) and what the physical jumper leaves it at.
//    The +3e-div ROM never writes the control port, so in the configuration this
//    romset reproduces the card is nothing but the taskfile above. Paging it in
//    would be actively wrong here: the driver lives in the machine's own ROM, and
//    divIDE's romcs would replace the very banks it runs from. (The full card —
//    EPROM, RAM, CONMEM/MAPRAM and the 0x3Dxx/entry-point automap — does exist in
//    this firmware, as esxDOS -> DivIDE in DivMMC.cpp. The two decode the same
//    ports, which is why only one of them may be live at a time.)

#pragma once

#include <stdint.h>

// Does this port access belong to the divIDE ATA taskfile? The caller supplies
// the "is the card fitted" test (IDE::portScheme) — this header knows only about
// addresses. Fuse decodes the low byte alone; so do we.
static inline bool divideIdePort(uint16_t address) {
    return (address & 0x00E3) == 0x00A3;
}

// ATA register 0..7 for an address that divideIdePort() accepted: A2..A4 of the
// low byte, which reproduces Fuse's port_to_ide_register() table exactly.
static inline uint8_t divideIdeReg(uint16_t address) {
    return (uint8_t)((address >> 2) & 0x07);
}

// The write-only control register (CONMEM / MAPRAM / EPROM bank). Nothing this
// romset runs writes it, and there is no memory behind it here — Ports.cpp
// swallows the write so it cannot reach another card's decode instead.
static inline bool divideCtrlPort(uint16_t address) {
    return (address & 0x00FF) == 0x00E3;
}
