// pico-speccy — ZX Spectrum +3e: the "simple 8-bit" IDE port decode.
//
// The +3e is Garry Lancaster's replacement +3 ROM (v1.4 here, the "sm8" build). It
// carries IDEDOS, which drives a hard disk over the simple 8-bit interface. This
// header is only the address decode; the ATA device behind it is IDE.cpp and the
// wiring is in Ports.cpp. It lives here so tools/plus3e_ide_test.cpp can check the
// shipped arithmetic against the shipped ROM rather than against a copy of it.
//
// The map was read out of the ROM itself (bank 2, from ~0x24A3), not from a
// schematic. The low byte is always #EF and the ATA register number is carried by
// three bits of the HIGH byte — A13, A12 and A8:
//
//   #CEEF data    #CFEF error/features  #DEEF sec count  #DFEF sector
//   #EEEF cyl lo  #EFEF cyl hi          #FEEF dev/head   #FFEF command/status
//
// Evidence, all from the shipped bank 2:
//   0x24C0  LD BC,#FEEF / LD A,#A0 / OUT (C),A     — device+head, master
//   0x24ED  LD A,#EC / LD BC,#FFEF / OUT (C),A     — IDENTIFY DEVICE
//   0x2501  #EEEF=0, #DFEF=1, #DEEF=1, #EFEF=D,
//           then #FFEF=#20                         — cyl lo / sector / count / cyl hi,
//                                                    then READ SECTORS
//   0x252D  LD B,#CE ... 256 x INI                 — one sector is 256 bytes, i.e. the
//                                                    bus is 8 bits wide
//
// A9..A11 are NOT decoded: 0x278B drives the very same registers as #F0EF / #EFEF /
// #E0EF. A14 and A15 are 1 in every access the ROM makes, and requiring them is what
// keeps this decode off the bottom of ZiFi's #xxEF window; the two still overlap at
// the top, which is why the NIC is forced off while a +3e runs.

#pragma once

#include <stdint.h>

// Does this port access belong to the +3e IDE interface? The caller supplies the
// machine test (Config::isPlus3e()) — this header knows only about addresses.
static inline bool plus3eIdePort(uint16_t address) {
    return (address & 0x00FF) == 0x00EF && (address & 0xC000) == 0xC000;
}

// ATA register 0..7 for an address that plus3eIdePort() accepted.
static inline uint8_t plus3eIdeReg(uint16_t address) {
    return (uint8_t)(((address >> 11) & 4)    // A13 -> bit 2
                   | ((address >> 11) & 2)    // A12 -> bit 1
                   | ((address >> 8)  & 1));  // A8  -> bit 0
}
