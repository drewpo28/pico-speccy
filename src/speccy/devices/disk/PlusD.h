// pico-speccy — MGT Plus D disk / printer interface (Miles Gordon Technology, 1988).
//
// Model: Fuse 1.6.0 peripherals/disk/plusd.c (GPL-2.0+), the semantics ported.
//   - 8K G+DOS ROM at 0x0000-0x1FFF + 8K RAM at 0x2000-0x3FFF while paged in.
//   - Paged IN by the M1 fetch of 0x0008 (RST 8 hook codes), 0x003A (inside the
//     48K ROM's IM1 handler), 0x0066 (NMI = the snapshot button) and 0x028E
//     (KEY-SCAN), and by any IN from #E7; paged OUT by any OUT to #E7. Paged in at
//     reset on the 48K (G+DOS pages itself out into the 48K ROM's START/NEW); left
//     out on a 128K / +2, whose ROM 0 has other code at that address (PlusD::reset).
//   - WD1772 at #E3 (command/status) #EB (track) #F3 (sector) #FB (data); #EF control
//     (bits 0-1 drive: 01 = drive 1, 10 = drive 2; bit 6 printer strobe; bit 7 side),
//     #F7 printer data (write) / busy in bit 7 (read; no printer = never busy).
// The controller and the media are the SAM Coupe's (machines/Sam/SamFdc, one Regs
// per drive, shareRegs on a drive switch): the two MGT drives are the same objects,
// which is fine because the SAM machine and a Spectrum with Plus D never run together.
// The memory window is MemESP's page0 window (page0_lo/hi + divmmc_mapped), the one
// DivMMC and MB-02+ use; those two are kept off while the Plus D is on.
#pragma once

#include <stdint.h>

namespace PlusD {
    extern bool enabled;        // the interface is on the bus (Config::plusd and the RAM landed)
    extern bool paged;          // ROM + RAM visible at 0x0000-0x3FFF

    // The M1 addresses that page the interface in. Inline: tested on every fetch
    // below 0x0300 while the interface is on (Z80_JLS exec_nocheck / fetchOpcode).
    static inline bool trapPc(uint16_t pc) {
        return pc == 0x0008 || pc == 0x003A || pc == 0x0066 || pc == 0x028E;
    }
    void pageIn();               // RAM-resident: called from the fetch loop
    void pageOut();

    // Config::plusd -> the interface (allocates / frees the 8K RAM). False = no RAM.
    bool init();
    void reset();                // machine reset: controller reset, paged in on the 48K, disks stay
    void powerOn();              // power cycle (machine switch, launch): the 8K RAM zeroed, G+DOS gone

    // Ports: the low byte has A7-A5 and A1-A0 high ((port & 0xE3) == 0xE3, Ports.cpp).
    uint8_t portIn(uint8_t lo);
    void    portOut(uint8_t lo, uint8_t v);

    void frameEnd();             // motor timeout + write-back (SamFdc::frameEnd)

    // Whether the machine `arch`/`romset` can carry a Plus D (a 48K / 128K / +2 bus).
    bool machineOk(uint8_t arch, uint8_t romset);

    // A flat MGT image whose directory holds Spectrum (G+DOS) file types rather
    // than SAM ones. Reads the first directory sectors.
    bool looksPlusD(const char* path);

    // F5 / catalog launch of a Plus D disk: drive 1, the interface on (switching to a
    // 128K when the running machine cannot carry it), the 8K RAM cleared, reset. No
    // autorun: RUN typed by the user makes the boot ROM load +SYS and the disk's AUTO.
    bool launchDisk(const char* path);

    // .pss: paged flag, control register, the 8K RAM, the controller of each drive.
    static constexpr uint32_t SNAP_MAX = 4 + 8192;
    uint32_t snapSave(uint8_t* out);
    bool     snapLoad(const uint8_t* in, uint32_t n);
}
