// pico-speccy — MicroART ATM-Turbo 1 and ATM-Turbo 2+.
//
// References (the owner's link, atmturbo.nedopc.com/atmshem.htm, is not reachable
// from the build environment, so everything here is read out of two emulators that
// agree with each other): UnrealSpeccy (tslabs/zx-evo pentevo/unreal — memory.cpp
// MM_ATM450 / MM_ATM710, io.cpp, atm.cpp, drawers.cpp) and MAME sinclair/atm.cpp
// (ATM-Turbo 2+ only).
//
// Both boards: the 48K frame (224 T x 312 lines = 69888 T at 3.5 MHz), no
// contention, AY on #FFFD/#BFFD, Beta-128 TR-DOS, and a 16-entry palette that
// colours EVERY video mode (index = the ZX ink/paper nibble with BRIGHT as bit 3;
// the border is 4 bits too — BRIGHT comes from address line A3 of the #FE write,
// inverted). Video modes (ZX 256x192 / EGA 320x200x16 / hires 640x200 with an
// attribute per byte / 80x25 text — the last one ATM-Turbo 2+ only) read the
// screen page (5 or 7, #7FFD D3) plus the page four below it (1 or 3).
//
// ATM-Turbo 1 (Unreal MM_ATM450), 512 KB:
//   OUT (#FE): the LOW BYTE OF THE ADDRESS is latched (aFE): A7=0 = CP/M mode (RAM
//     page 0 at #0000, page 4 at #4000), A6..A5 = video mode (0 EGA, 1 hires, 3 ZX).
//   #7DFD (A15=0, A9=0, A1=0) palette write; #7FFD needs A9=1.
//   #FDFD (A15=1, A9=0, A1=0): D2..D0 = #C000 page bits 5..3, D3 with DOS = CPSYS.
//   IN from any port with A2=0 latches the low address byte (aFB); A7 = CPSYS,
//     which pages the SYSTEM ROM at #0000. #7FFD D5 clears it.
//   ROM page = CPSYS ? SYS : DOS ? TR-DOS : #7FFD D4 ? 48 : 128.
//
// ATM-Turbo 2+ (Unreal MM_ATM710, MAME atmtb2plus), 1 MB, ports in the DOS space only
// (TR-DOS active, or CP/M mode):
//   #xx77: data D2..D0 video mode (3 ZX, 0 EGA, 2 hires, 6 text), D3 turbo (7 MHz),
//     D5 frame INT enable; ADDRESS A8 = PEN (memory manager on), A9 = /CPM (0 keeps
//     the DOS signal and the DOS ports up), A14 = /PEN2 (0 = #FF writes the palette).
//   #xxF7: page register of window A15..A14 in set #7FFD D4. Data D7 = take the low
//     bits from #7FFD (RAM: D2..D0 of #7FFD; ROM: bit 0 = the DOS signal), D6 =
//     RAM (1) / ROM (0), D5..D0 = page number INVERTED.
//   PEN = 0 puts the LAST ROM page (the BIOS) in all four windows — the reset state.
//   IDE on the xx0F family (A7..A5 = register, A8 = the 16-bit high-byte latch).
#pragma once

#include <stdint.h>
#include "ArchRom.h"

// One ROM page of a bound romset (tools/rom_pack.py pack_atm -> roms/atm/atm_banks.h):
// base == nullptr is the all-0xFF page; overlay == nullptr means "base as is".
struct atm_rom_page_t { const unsigned char* base; const unsigned char* overlay; };

// Bit n set = CPU window n shows ROM: writes are dropped there (the pages may be
// flattened into butter PSRAM, which MemESP::writebyte's flash-pointer filter does
// not cover). Tested predicted-not-taken in the CPU write funnel (CPU.cpp
// gsDmaPoke8) — zero on every other machine.
extern uint8_t g_atm_ro;

namespace Atm {
    enum VMode : uint8_t { VM_ZX = 0, VM_EGA = 1, VM_HIRES = 2, VM_TEXT = 3 };

    extern bool     atm1;       // ATM-Turbo 1 board (else 2+), set by bindRoms
    extern uint8_t  p7ffd;
    // ATM-Turbo 1
    extern uint8_t  aFE, aFB, pFDFD;
    // ATM-Turbo 2+
    extern uint16_t a77;
    extern uint8_t  p77;
    extern uint8_t  pF7[8];
    extern uint8_t  pal[16];    // raw palette bytes as written (read back on ATM 2+ via #BF)
    extern bool     beta;       // the Beta-128 trap signal (DOSEN), separate from /CPM
    extern bool     palDirty;   // palette changed — applied at EndFrame

    // Record this romset's page table. The flattening into PSRAM happens on the first
    // reset() after Buffer::initPools (requestMachine runs before the pools exist).
    void bindRoms(RomsetIdx rs, const atm_rom_page_t* pages, uint8_t n);
    void reset();               // machine reset: register file + remap
    void remap();               // the one writer of MemESP::ramCurrent[0..3] on ATM

    // Port hooks, called early from Ports::output/input. true = consumed.
    bool portWrite(uint16_t address, uint8_t data);
    bool portRead(uint16_t address, uint8_t& v);
    void feWrite(uint16_t address);   // ATM1 #FE address latch (the ULA branch still runs)
    uint8_t feRead(uint8_t v);        // ATM1 #FE bit 7 PAL-detect quirk

    // check_trdos replacement (Z80_JLS.cpp).
    void trdosTrap(uint8_t pcH);

    // Frame INT gate (#xx77 D5 on the 2+; always open on the ATM1).
    inline bool intEnabled() { return atm1 || (p77 & 0x20); }

    VMode videoMode();          // live mode from the latches
    uint8_t borderBright();     // 8 when the border is BRIGHT (A3 = 0 at the #FE write)
    uint32_t palRgb(uint8_t i); // palette entry i as RGB888 (2 bits per channel)
    uint8_t romPageCount();
}
