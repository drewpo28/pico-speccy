/*

TS-Conf (ZX-Evolution TS-Configuration) machine core.

Re-derived for pico-speccy from the UnrealSpeccy fork bundled with the
zx-evo repository (GPL-2.0+, https://github.com/tslabs/zx-evo —
pentevo/unreal/Unreal/tsconf.{cpp,h}, io.cpp, memory.cpp), cross-checked
against the FPGA RTL in pentevo/fpga/current (the authority where they
disagree) and the TSconf datasheet (pentevo/docs/TSconf/tsconf_en.md).

Phase 1 scope — the machine core only:
  - 4 MB page model: four 16 KB windows, Page0..3 (#10AF..#13AF), MemConfig
    (#21AF) window-0 ROM/RAM mapping, #7FFD under LCK128.
  - The #nnAF register file. Video registers are STORED (with the *_d
    line-delay shadows the hardware has) but only VPage drives anything yet.
  - FRAME interrupt with programmable HSINT/VSINT position, IM2 vector #FF.
  - SysConfig ZCLK 3.5/7/14 MHz -> ESPectrum::multiplicator.
  - CRAM/SFILE storage (written via #nnAF only in this phase).
Phase 2 (2026-09-06) adds the DMA controller (RAM/BLT1/BLT2/FILL/CRAM/SFILE
and SPI via the Z-Controller; IDE is a warn-once stub), the LINE and DMA
interrupt sources with the hardware's priority/acknowledge rules, and the
W0_WE write protect. Still inert: the TSU, 16c/256c/text video modes, VDOS,
the read cache.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef TSCONF_H
#define TSCONF_H

#include <stdint.h>

class TsConf {
public:
    // #nnAF register numbers (write side unless TSR_*) — ports.inc / tsconf.h
    enum Reg : uint8_t {
        TSW_VCONF    = 0x00, TSW_VPAGE   = 0x01,
        TSW_GXOFFSL  = 0x02, TSW_GXOFFSH = 0x03,
        TSW_GYOFFSL  = 0x04, TSW_GYOFFSH = 0x05,
        TSW_TSCONF   = 0x06, TSW_PALSEL  = 0x07,
        TSW_BORDER   = 0x0F,
        TSW_PAGE0    = 0x10, TSW_PAGE1   = 0x11,
        TSW_PAGE2    = 0x12, TSW_PAGE3   = 0x13,
        TSW_FMADDR   = 0x15,
        TSW_TMPAGE   = 0x16, TSW_T0GPAGE = 0x17,
        TSW_T1GPAGE  = 0x18, TSW_SGPAGE  = 0x19,
        TSW_DMASAL   = 0x1A, TSW_DMASAH  = 0x1B, TSW_DMASAX = 0x1C,
        TSW_DMADAL   = 0x1D, TSW_DMADAH  = 0x1E, TSW_DMADAX = 0x1F,
        TSW_SYSCONF  = 0x20, TSW_MEMCONF = 0x21,
        TSW_HSINT    = 0x22, TSW_VSINTL  = 0x23, TSW_VSINTH = 0x24,
        TSW_DMALEN   = 0x26, TSW_DMACTR  = 0x27, TSW_DMANUM = 0x28,
        TSW_FDDVIRT  = 0x29, TSW_INTMASK = 0x2A, TSW_CACHECONF = 0x2B,
        TSW_T0XOFFSL = 0x40, TSW_T0XOFFSH = 0x41,
        TSW_T0YOFFSL = 0x42, TSW_T0YOFFSH = 0x43,
        TSW_T1XOFFSL = 0x44, TSW_T1XOFFSH = 0x45,
        TSW_T1YOFFSL = 0x46, TSW_T1YOFFSH = 0x47,
        TSR_STATUS   = 0x00, TSR_PAGE2 = 0x12, TSR_PAGE3 = 0x13,
        TSR_DMASTATUS = 0x27,
    };

    // The register file. Field names follow the reference TSPORTS_t; the *_d
    // pairs are the hardware's line-delayed shadows (a write lands in *_d and
    // is committed at the next line start — the Phase 3 rasterizer does the
    // commit; until then only vpage is consumed, committed immediately).
    struct Regs {
        // -- system --
        uint8_t  sysconf;      // b1:0 ZCLK (00=3.5 01=7 10=14), b2 CACHE
        uint8_t  cacheconf;
        uint8_t  memconf;      // b0 ROM128, b1 W0_WE, b2 !W0_MAP, b3 W0_RAM, b7:6 LCK128
        uint8_t  fmaddr;       // b3:0 window, b4 enable (stored; hooked in phase 2)
        uint8_t  fddvirt;      // stored; VDOS is phase 4+
        uint8_t  intmask;      // b0 FRAME, b1 LINE, b2 DMA
        uint8_t  hsint;        // FRAME INT pixel position (0..223 valid)
        uint16_t vsint;        // FRAME INT line position, 9 bit (0..319 valid)
        uint8_t  pwr_up;       // 0x40 until the first STATUS read (cold-boot flag)
        uint8_t  p7ffd;        // our #7FFD shadow (b3 SCR, b4 ROM128, b5 LOCK)
        // -- memory --
        uint8_t  page[4];      // Page0..3
        // -- video (stored; *_d = line-delay shadow) --
        uint8_t  vconf, vconf_d;
        uint8_t  vpage, vpage_d;
        uint8_t  tsconf, tsconf_d;
        uint8_t  palsel, palsel_d;
        uint8_t  border;       // full 8-bit CRAM index
        uint16_t g_xoffs, g_yoffs;
        bool     g_yoffs_updated; // GYOffs written since the last rendered line (Unreal g_yoffs_updated)
        uint16_t t0_xoffs, t0_yoffs, t1_xoffs, t1_yoffs;
        uint8_t  tmpage, t0gpage, t1gpage, sgpage;
        // -- dma (stored only in this phase) --
        uint8_t  dmalen, dmanum, dmactrl;
        uint32_t saddr, daddr; // 22-bit: L=a[7:0] (bit0 forced 0), H=a[13:8], X=a[21:14]

        uint8_t lck128()   const { return memconf >> 6; }
        bool    w0_we()    const { return memconf & 0x02; }
        bool    w0_map_n() const { return memconf & 0x04; }
        bool    w0_ram()   const { return memconf & 0x08; }
    };

    static Regs r;
    static uint16_t cram[256];   // RGB555 xRRrrrGGgggBBbbb
    static uint16_t sfile[256];  // 85 sprite descriptors x 3 words
    static uint32_t bankPhys(uint8_t bank, uint16_t off);   // physical address of a CPU-bank offset
    static void     wrGateRecalc();                          // recompute g_tsconf_wr / g_ts_bank_watch
    static uint32_t sfileGen;    // bumped on every SFILE write (core1 render snapshots, Video.cpp)
    static uint8_t  tsuSeen;     // TSConfig layer bits seen set since the last EndFrame (mode hysteresis)

    // Machine reset (tsinit() values). Cold=true additionally raises pwr_up.
    static void reset(bool cold);
    static void bindRoms();      // called from Config::requestMachine

    static uint8_t portRead(uint8_t reg);
    static void portWrite(uint8_t reg, uint8_t val);
    static void write7ffd(uint8_t val);
    static void fmWrite(uint16_t addr, uint8_t val); // FMAddr window (CPU write funnel)
    // CPU write funnel hook, entered only while g_tsconf_wr != 0: routes the
    // FMAddr window and applies the W0_WE protect. Returns true when the RAM
    // store must be SUPPRESSED (window 0 is RAM with W0_WE = 0).
    static bool cpuWriteGate(uint16_t addr, uint8_t val);
    // SysConfig ZCLK -> ESPectrum::multiplicator. The guest's register IS the
    // clock (there is no user turbo on a ZX-Evo); fromGuest=true shows the
    // same top-border toast as the Turbo hotkey when the effective clock moves.
    static void applyZclk(bool fromGuest = false);
    static void setBanks();      // recompute MemESP::ramCurrent[0..3]
    static void trdosTrap(uint8_t pcH); // check_trdos() replacement (Z80_JLS.cpp)

    // Interrupt controller (zint.v). Three latched sources: FRAME (the
    // programmable 32-clock window at VSINT/HSINT), LINE (every line start
    // while enabled), DMA (transaction end). intLine() is the level the CPU
    // samples (Z80Ops::isActiveINT); intAck() runs the acknowledge — it picks
    // the highest-priority pending source (FRAME > LINE > DMA), clears it and
    // returns its IM2 vector (#FF / #FD / #FB). frameIntRecalc() maps
    // (vsint,hsint) onto CPU::IntStart/IntEnd in scaled T-states.
    static void frameIntRecalc();
    static inline bool frameIntEnabled() { return r.intmask & 0x01; }
    static bool intLine();
    static uint8_t intAck();
    // INT-accept trace (PERF_TRACE builds): where the guest was when each
    // interrupt was TAKEN — Z80::interrupt() calls it right after intAck(),
    // before the push. Ring of TS_INT_RING_N in TsConf.cpp (ts_int_ring), dumped by
    // tools/memdump.gdb. Written for fishbone (hw 2026-09-09): a raster-split
    // title that runs SP-pointer tricks with interrupts enabled, so WHERE the
    // 10-byte interrupt frame lands is the whole question.
#if PERF_TRACE
    static void intTrace(uint16_t pc, uint16_t sp, uint8_t vect, bool halted);
    static void workHalt();
#else
    static inline void intTrace(uint16_t, uint16_t, uint8_t, bool) {}
    static inline void workHalt() {}
#endif
    // True while a LINE or DMA interrupt may fire: CPU::loop then runs the
    // rest of the frame instruction-checked instead of the unchecked slices
    // (only the FRAME window is checked otherwise).
    static bool needsCheckedFrame();
    // Earliest T-state (>= CPU::tstates, <= statesInFrame) at which intLine()
    // can become true — where a HALTed CPU may sleep to (CPU::loop Stage D).
    static uint32_t nextIntEvent();
    static void intEnableHook();   // EI/RETN/RETI: wake the unchecked slice if INT is up
    static void endFrame();      // frame-relative INT/DMA timestamps wrap here

    // DMA controller. A DMACtrl write runs the whole transaction at once;
    // DMA_ACT then stays up for the time the hardware would have needed, and
    // the DMA interrupt is raised when it drops.
    static void dmaStart(uint8_t ctrl);
    static uint8_t dmaStatus();  // DMAStatus register image (b7 = DMA_ACT)
    // Memory movement of one bulk transaction (RAM/BLT1/BLT2/FILL), register
    // file untouched — runs on core1 from the render queue (Video.cpp) or on
    // core0 when the queue is off.
    static void dmaExecBulk(uint8_t ctrl, uint32_t saddr, uint32_t daddr, uint8_t dmalen, uint8_t dmanum);
    static void dmaLineTick();   // per content line: DMA_ACT dropped → make sure the queued copy is done

    // 16 KB RAM page as a direct pointer, or nullptr when the page is not
    // POINTER-backed (degraded boot only — see the residency self-heal in
    // ESPectrum::setup). The pointer is the cached XIP alias, same as every
    // existing renderer uses.
    static uint8_t* pagePtr(uint32_t page);
    static const uint8_t* romPtr(uint8_t page); // 32-page ROM window (flash)

    // ZX-mode screen page for the renderer (VPage with #7FFD-SCR folded in).
    static void refreshGrmem();
};

// Nonzero while a TS-Conf write-side hook is armed — the CPU write funnel's
// predicted-not-taken gate (the g_ngs_zxdma pattern; see CPU.cpp). Bits:
// 0x10 | window = FMAddr enabled (TsConf::fmWrite), 0x20 = window 0 is RAM
// with W0_WE = 0 (writes below #4000 dropped). Zero for every other machine.
extern uint8_t g_tsconf_wr;
// Bit 0x40 of g_tsconf_wr: some CPU bank maps a page the core1 render queue
// may still read; g_ts_bank_watch has one bit per bank. A guest write into such
// a bank waits for the queued lines that read it (VIDEO::tsRenderDrainOverlap).
extern uint8_t g_ts_bank_watch;

#endif // TSCONF_H
