#pragma once
#include <stdint.h>

// ZX Spectrum Next: a STUB of the FPGA configuration flash on the SPI bus (port
// #E7 = #7F, config mode only). Other FPGA cores (QL, ...) cannot run here, so
// nothing is stored: the part answers its ID and status (never busy), accepts
// erase and program and drops them, and reads as erased (#FF) — except the
// last programmed page, kept so the firmware's per-page verify passes. TBBLUE.FW's
// Select Core -> Flash therefore runs to its end instead of stopping on
// "Flash timeout!", with a "not supported" notice; starting a core says the same.
//
// Sources: zxnext.vhd 3300-3332 (#E7 slave select: #7F = flash, only while
// nr_03_config_mode or a power-on reset), serial/spi_master.vhd (the port is
// pipelined: an IN returns the byte the PREVIOUS transfer clocked in and starts
// a new transfer sending #FF), nextreg.txt NR 0x0F (Issue 2 = W25Q128JV, Issue
// 4/5 = MX25L25645G) and the parts' datasheets for the command set.
namespace NextFlash {
    void    select(bool on);       // #E7 write: #7F in config mode selects, anything else deselects
    bool    selected();
    void    out(uint8_t v);        // #EB write while selected
    uint8_t in();                  // #EB read while selected
    void    reset();               // machine reset: deselect
    void    coreBoot(uint8_t nr10);// NR 0x10 bit 7: the FPGA would reload from a slot (not supported)
}
