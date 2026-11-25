#pragma once
#include <stdint.h>

// ZX Spectrum Next board personality: which PCB the core believes it runs on
// (Config::next_board: 0 = KS1 / Issue 2, 1 = KS2 / Issue 4, 2 = KS3 / Issue 5).
//
// Sources (core 3.02.03, ~/github/ZX_Spectrum_Next_FPGA/cores/zxnext):
//   nextreg.txt NR 0x0F       board id 0 = Issue 2, 2 = Issue 4, 3 = Issue 5
//   zxnext.vhd gen_romram_*   NR 0x04: bit 7 ignored below Issue 5 (4 MB SRAM space)
//   zxnext.vhd gen_xper_*     NR 0xF0 (XDEV: DNA / XADC select), 0xF8-0xFA (XADC DRP):
//                             Issues 4 and 5 only, all zero on Issue 2/3
//   Xilinx UG480              XADC DRP map: 0x00 temperature, 0x01/02/06 VCCINT/AUX/
//                             BRAM, 0x20-0x23 max, 0x24-0x27 min, 0x40-0x4F config
// The XADC temperature is the RP2350's own die sensor (chipTempX10), the supply
// rails are their nominal values.
namespace NextBoard {
    uint8_t boardId();          // NR 0x0F value
    bool    issue5();           // KS3: 4 MB config-mode SRAM, NR 0x04 bit 7
    bool    xilinxPer();        // KS2/KS3: XDEV + XADC registers live
    void    reset(bool hard);   // NextReg::reset: NR 0xF0 back to select mode
    void    write(uint8_t r, uint8_t v);   // NR 0xF0 / 0xF8 / 0xF9 / 0xFA
    uint8_t read(uint8_t r);               // the same four
}
