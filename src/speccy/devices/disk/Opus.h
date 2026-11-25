// pico-speccy — Opus Discovery disk interface (Opus Supplies, 1984-86).
//
// Model: Fuse 1.6.0 peripherals/disk/opus.c (GPL-2.0+), the semantics ported; ROM =
// Opus 2.22 (8K, src/speccy/roms/opus.h).
//   - While paged: 8K ROM at 0x0000-0x1FFF, 2K RAM at 0x2000-0x27FF, the WD1770 at
//     0x2800-0x2803 (command/status, track, sector, data) and a 6821 PIA at
//     0x3000-0x3003 (port A: bit 1 drive 2, bit 4 side, bit 6 printer busy; port B:
//     printer data), all MEMORY-mapped; 0x3800-0x3FFF reads #FF.
//   - Paging is decided AFTER the M1 fetch (Fuse "late" check): the opcode at 0x0008
//     (RST 8), 0x0048 (the 48K ROM's IM1 handler) and 0x1708 (CLOSE#) is read from
//     the Spectrum ROM, then the Opus pages in; the opcode at 0x1748 is read from the
//     Opus ROM (a RET), then it pages out.
//   - The WD1770's DRQ drives the Z80 NMI: the ROM points HL at its byte handler
//     (#1886 read, #1895 write, #189F format) and 0x0066 is JP (HL). The main code
//     polls BUSY. Here the NMI is raised a few instructions after DRQ rises (status
//     poll / data byte), so the handler's RETN is always out first (an NMI is not
//     masked by an NMI in progress).
//   - 48K only: on a 128K the IM1 path through ROM 0 returns to ROM 0's 0x0048 (POP HL)
//     after ROM 1's 0x0048, the trap fires twice and the stack breaks.
// The controller and the drives are the SAM Coupe's (machines/Sam/SamFdc), .opd/.opu
// images (18 x 256 B sectors numbered from 0). Shares MemESP's page-0 window with
// Plus D / DivMMC / MB-02+, which are off while the Opus is on.
#pragma once

#include <stdint.h>

namespace Opus {
    extern bool enabled;
    extern bool paged;
    extern uint8_t nmiCnt;       // > 0: instructions until the DRQ NMI (exec loops)

    static inline bool trapPc(uint16_t pc) {
        return pc == 0x0008 || pc == 0x0048 || pc == 0x1708 || pc == 0x1748;
    }
    void m1Late(uint16_t pc);    // RAM-resident: after the fetch of a trapPc address
    void nmiTick();              // RAM-resident: nmiCnt reached 0 at an instruction start

    // The memory-mapped I/O (0x2800-0x3FFF while paged; CPU.cpp gsDmaPeek8/Poke8).
    uint8_t memRead(uint16_t addr);
    void    memWrite(uint16_t addr, uint8_t v);

    bool init();                 // Config::opus -> the interface (ROM + RAM buffers)
    void reset();                // machine reset: paged out, PIA + controller reset
    void frameEnd();
    bool machineOk(uint8_t arch, uint8_t romset);

    // F5 launch of an .opd/.opu: drive 1, the interface on (a 48K), reset, RUN typed
    // (the ROM loads the file "run").
    bool launchDisk(const char* path);
    void bootTick();

    static constexpr uint32_t SNAP_MAX = 12 + 2048;
    uint32_t snapSave(uint8_t* out);
    bool     snapLoad(const uint8_t* in, uint32_t n);
}

// 1 while the Opus is paged: the I/O window test in CPU.cpp's peek/poke paths.
extern uint8_t g_opus_io;
