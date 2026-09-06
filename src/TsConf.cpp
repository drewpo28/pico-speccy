/*

TS-Conf (ZX-Evolution TS-Configuration) machine core — see TsConf.h for
scope and provenance. Register/paging semantics ported from the
UnrealSpeccy fork in tslabs/zx-evo (GPL-2.0+): io.cpp ts_ext_port_wr()
(the #nnAF switch), memory.cpp set_banks() MM_TSL (the paging model),
z80_main.inl (the FMAddr memory window), tsconf.cpp tsinit() (reset
values); INT semantics cross-checked against fpga/current/z80/zint.v.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include "TsConf.h"
#include <string.h>
#include "pico/time.h"
#include "CPU.h"
#include "Z80_JLS/z80.h"
#include "Config.h"
#include "MemESP.h"
#include "ESPectrum.h"
#include "Video.h"
#include "Debug.h"
#include "RTC.h"
#include "DivMMC.h"
#include "LEDIndicators.h"
#include "OSDMain.h"
#include "roms/tsconf/romTsBios.h"

TsConf::Regs TsConf::r;
uint16_t TsConf::cram[256];
uint16_t TsConf::sfile[256];

// Write-side gate (see TsConf.h): 0x10|window while FMAddr is enabled, 0x20
// while window 0 is write-protected RAM. Tested predicted-not-taken in the CPU
// write funnel (gsDmaPoke8) — the g_ngs_zxdma pattern; zero for every other
// machine, so the cost elsewhere is one byte-load-and-test per guest write.
uint8_t g_tsconf_wr = 0;

// FMAddr 512-byte windows latch the even byte here and commit the word on the
// odd address (reference temp.fm_tmp).
static uint8_t s_fm_tmp = 0;

static void tsUpdateWrGate() {
    uint8_t g = 0;
    if (Z80Ops::isTsconf) {
        if (TsConf::r.fmaddr & 0x10) g |= (TsConf::r.fmaddr & 0x0F) | 0x10;
        if (TsConf::r.w0_ram() && !TsConf::r.w0_we()) g |= 0x20;
    }
    g_tsconf_wr = g;
}

// ------------------------------------------------ INT / DMA state ----
// Frame-relative, in scaled CPU T-states (CPU::tstates units). The sources are
// evaluated LAZILY from CPU::tstates when the CPU samples the line or reads
// DMAStatus — there is no per-line hook, and none is needed: while LINE or DMA
// interrupts are armed CPU::loop runs instruction-checked, so intLine() is
// consulted after every instruction anyway.
static bool     s_frm_acked;    // FRAME taken this frame (int_frm cleared by the ack)
static bool     s_lin_pending;  // int_lin latch
static uint32_t s_lin_next;     // T of the next line start that raises LINE
static bool     s_dma_busy;     // DMA_ACT: transaction "in flight"
static uint32_t s_dma_end;      // T at which it completes
static bool     s_dma_pending;  // int_dma latch

static inline uint32_t tsLineT() {
    return (uint32_t)TSTATES_PER_LINE_PENTAGON << ESPectrum::multiplicator;
}
static inline uint32_t tsNextLineStart(uint32_t t) {
    const uint32_t lt = tsLineT();
    return (t / lt + 1) * lt;
}
// Leave an unchecked exec_nocheck() slice early so CPU::loop re-evaluates
// needsCheckedFrame() — called when LINE/DMA interrupts become possible mid-
// frame. stFrame == 0 means "HALTed" to the loop, hence the floor of 1.
static inline void tsWakeLoop() {
    if (CPU::stFrame > CPU::tstates) CPU::stFrame = CPU::tstates ? CPU::tstates : 1;
}

// ---------------------------------------------------------------- ROM ----

// 32-page ROM window. Only the 4 TS-BIOS pages are embedded (they are
// byte-identical to the first 64 KB of the real 512 KB flash); pages 4..31
// answer like unbonded flash. gb_rom_Alf_ep (the ALF open-bus 16 KB zero
// page) is reused as the filler — reads there are deterministic 0x00.
extern "C" const unsigned char gb_rom_Alf_ep[];

const uint8_t* TsConf::romPtr(uint8_t page) {
    page &= 0x1F;
    if (page < 4) return gb_rom_tsbios + ((uint32_t)page << 14);
    return gb_rom_Alf_ep;
}

void TsConf::bindRoms() {
    // Nothing to bind into MemESP::rom[] — window 0 gets flash pointers from
    // romPtr() directly, and rom[4] (TR-DOS, bound unconditionally by
    // requestMachine's tail) stays untouched for the Beta-128 path.
}

// -------------------------------------------------------------- paging ----

uint8_t* TsConf::pagePtr(uint32_t page) {
    mem_desc_t& d = MemESP::ram[page & (MEM_PG_CNT - 1)];
    return (d.memType() == mem_type_t::POINTER) ? d.direct() : nullptr;
}

void TsConf::refreshGrmem() {
    // ZX mode renders from VPage. Non-POINTER pages cannot happen after the
    // boot residency self-heal; fall back to page 5 so a degraded boot still
    // shows something rather than dereferencing null.
    uint8_t* p = pagePtr(r.vpage);
    if (!p) p = MemESP::ram[5].direct();
    VIDEO::grmem = p;
}

void TsConf::setBanks() {
    const uint32_t mask = MEM_PG_CNT - 1;

    MemESP::ramCurrent[1] = MemESP::ram[r.page[1] & mask].sync(1);
    MemESP::ramCurrent[2] = MemESP::ram[r.page[2] & mask].sync(2);
    MemESP::ramCurrent[3] = MemESP::ram[r.page[3] & mask].sync(3);

    // Window 0: reference memory.cpp set_banks() MM_TSL. In mapped mode the
    // page number's low bits come from the DOS signal (bit 1) and ROM128
    // (#7FFD bit 4, bit 0): Service/DOS/128/48.
    uint8_t p0;
    if (r.w0_map_n()) {
        p0 = r.page[0];
    } else {
        uint8_t rom128 = (r.p7ffd >> 4) & 1;
        p0 = (ESPectrum::trdos ? (rom128 ? 1 : 0) : (rom128 ? 3 : 2))
             | (r.page[0] & 0xFC);
    }
    if (r.w0_ram()) {
        // RAM at #0000. W0_WE=0 write protect is not modelled yet (phase 2);
        // writes land in the page.
        MemESP::ramCurrent[0] = MemESP::ram[p0 & mask].sync(0);
    } else {
        // ROM at #0000 — a flash pointer; MemESP::writebyte drops writes to
        // anything below the butter window, so ROM is naturally read-only.
        MemESP::ramCurrent[0] = (uint8_t*)romPtr(p0);
    }

    // Keep the shared latches coherent for the OSD/debugger and the
    // ram[MEM_PG_CNT + romLatch] ROM-indexing convention (newSRAM is never
    // set on TS-Conf, so romLatch is display-only here).
    MemESP::bankLatch = r.page[3] & mask;
    MemESP::videoLatch = (r.p7ffd >> 3) & 1;
    MemESP::romLatch = (r.p7ffd >> 4) & 1;
    MemESP::romInUse = 0;
    MemESP::ramContended[0] = MemESP::ramContended[1] = false;
    MemESP::ramContended[2] = MemESP::ramContended[3] = false;

    tsUpdateWrGate();   // W0_RAM / W0_WE live in memconf
    refreshGrmem();
}

void TsConf::trdosTrap(uint8_t pcH) {
    // check_trdos() replacement — reference z80_main.inl:185-215 +
    // memory.cpp:397-411 for MM_TSL. Entry (CF_SETDOSROM): PC at #3Dxx with
    // ROM128=1 (#7FFD bit 4) and ROM actually mapped in window 0. Exit
    // (CF_LEAVEDOSRAM): executing from RAM closes TR-DOS — windows 1..3 are
    // always RAM on TS-Conf, so any PC >= #4000 exits, and so does window 0
    // itself once W0_RAM is set. (Under VDOS there is no exit — phase 4.)
    if (!ESPectrum::trdos) {
        if (pcH == 0x3D && (r.p7ffd & 0x10) && !r.w0_ram()) {
            ESPectrum::trdos = true;
            setBanks();
        }
    } else {
        if (pcH >= 0x40 || r.w0_ram()) {
            ESPectrum::trdos = false;
            setBanks();
        }
    }
}

void TsConf::write7ffd(uint8_t val) {
    // 48-lock: bit 5 of the LATCHED value blocks further writes (no unlock
    // backdoor on TS-Conf — reference io.cpp:707). LCK128=11 clears bit 5
    // before latching, so 1024K mode can never lock.
    if (r.p7ffd & 0x20) return;

    switch (r.lck128()) {
        case 0: // 512K: Page3[4:0] = #7FFD[7:6],#7FFD[2:0]
            r.page[3] = ((val & 0xC0) >> 3) | (val & 0x07);
            break;
        case 1: // 128K
            r.page[3] = val & 0x07;
            break;
        case 2: // Auto (opcode-dependent on real hw) — treated as 512K.
                // Documented Phase-1 limitation: OUT (#FD),A should page 128K.
            r.page[3] = ((val & 0xC0) >> 3) | (val & 0x07);
            break;
        case 3: // 1024K: Page3[5:0] = #7FFD[5],#7FFD[7:6],#7FFD[2:0]
            r.page[3] = (val & 0x20) | ((val & 0xC0) >> 3) | (val & 0x07);
            val &= ~0x20;
            break;
    }
    r.p7ffd = val;
    r.vpage = r.vpage_d = (val & 0x08) ? 7 : 5;  // SCR bit — no line latch
    setBanks();
}

// --------------------------------------------------------------- ports ----

uint8_t TsConf::portRead(uint8_t reg) {
    switch (reg) {
        case TSR_STATUS: {
            // b6 pwr_up (self-clearing cold-boot flag), b2:0 VDAC id (0 = PWM).
            uint8_t v = r.pwr_up;
            r.pwr_up = 0;
            return v;
        }
        case TSR_PAGE2:     return r.page[2];
        case TSR_PAGE3:     return r.page[3];
        case TSR_DMASTATUS: return dmaStatus();
        default:            return 0xFF;
    }
}

void TsConf::portWrite(uint8_t reg, uint8_t val) {
    switch (reg) {
        // -- system --
        case TSW_SYSCONF:
            r.sysconf = val;
            // Writing CACHE copies it into all four CacheConfig bits
            // (datasheet); the cache itself is timing-only and not modelled.
            r.cacheconf = (val & 0x04) ? 0x0F : 0x00;
            applyZclk(true);
            break;
        case TSW_CACHECONF:
            r.cacheconf = val & 0x0F;
            break;
        case TSW_FDDVIRT:
            r.fddvirt = val & 0x8F;  // stored; VDOS is a later phase
            break;
        case TSW_INTMASK: {
            // zint.v: a source's latch is held at 0 while its mask bit is 0
            // ("writing 0 to a pending source resets it"); writing 1 leaves a
            // pending one alone and re-arms the source at its next event.
            const uint8_t old = r.intmask;
            r.intmask = val & 0x07;
            if (!(val & 0x02)) s_lin_pending = false;
            else if (!(old & 0x02)) s_lin_next = tsNextLineStart(CPU::tstates);
            if (!(val & 0x04)) s_dma_pending = false;
            if (needsCheckedFrame()) tsWakeLoop();
            frameIntRecalc();
            break;
        }
        case TSW_HSINT:
            r.hsint = val;
            frameIntRecalc();
            break;
        case TSW_VSINTL:
            r.vsint = (r.vsint & 0x100) | val;
            frameIntRecalc();
            break;
        case TSW_VSINTH:
            r.vsint = (r.vsint & 0xFF) | ((uint16_t)(val & 1) << 8);
            frameIntRecalc();
            break;

        // -- memory --
        case TSW_MEMCONF:
            r.memconf = val;
            // ROM128 is physically #7FFD bit 4 (reference writes it back).
            r.p7ffd = (r.p7ffd & ~0x10) | ((val & 1) << 4);
            setBanks();
            break;
        case TSW_PAGE0: r.page[0] = val; setBanks(); break;
        case TSW_PAGE1: r.page[1] = val; setBanks(); break;
        case TSW_PAGE2: r.page[2] = val; setBanks(); break;
        case TSW_PAGE3: r.page[3] = val; setBanks(); break;
        case TSW_FMADDR:
            r.fmaddr = val & 0x1F;
            tsUpdateWrGate();
            break;

        // -- video (stored; committed immediately until the phase-3
        //    rasterizer takes over the *_d line latch) --
        case TSW_VCONF:  r.vconf  = r.vconf_d  = val; break;
        case TSW_VPAGE:
            r.vpage = r.vpage_d = val;
            refreshGrmem();
            break;
        case TSW_TMPAGE:  r.tmpage  = val; break;
        case TSW_T0GPAGE: r.t0gpage = val; break;
        case TSW_T1GPAGE: r.t1gpage = val; break;
        case TSW_SGPAGE:  r.sgpage  = val; break;
        case TSW_BORDER:
            r.border = val;
            // Phase-1 border: reuse the beam-raced 3-bit border machine.
            // Exact whenever the cell lives in the default ZX bank (#F0-#F7 —
            // which is where OUT (#FE) puts it); other cells approximate to
            // their low 3 bits until the phase-3 renderer owns the border.
            if (VIDEO::borderColor != (val & 0x07)) {
                VIDEO::brdChange = true;
                VIDEO::DrawBorder();
                VIDEO::borderColor = val & 0x07;
                VIDEO::brd = VIDEO::border32[val & 0x07];
            }
            break;
        case TSW_TSCONF: r.tsconf = r.tsconf_d = val; break;
        case TSW_PALSEL:
            r.palsel = r.palsel_d = val;
            VIDEO::tsCramDirty = true;  // gpal re-points the 16 ZX slots
            break;
        case TSW_GXOFFSL: r.g_xoffs = (r.g_xoffs & 0x100) | val; break;
        case TSW_GXOFFSH: r.g_xoffs = (r.g_xoffs & 0xFF) | ((uint16_t)(val & 1) << 8); break;
        case TSW_GYOFFSL: r.g_yoffs = (r.g_yoffs & 0x100) | val; r.g_yoffs_updated = true; break;
        case TSW_GYOFFSH: r.g_yoffs = (r.g_yoffs & 0xFF) | ((uint16_t)(val & 1) << 8); r.g_yoffs_updated = true; break;
        case TSW_T0XOFFSL: r.t0_xoffs = (r.t0_xoffs & 0x100) | val; break;
        case TSW_T0XOFFSH: r.t0_xoffs = (r.t0_xoffs & 0xFF) | ((uint16_t)(val & 1) << 8); break;
        case TSW_T0YOFFSL: r.t0_yoffs = (r.t0_yoffs & 0x100) | val; break;
        case TSW_T0YOFFSH: r.t0_yoffs = (r.t0_yoffs & 0xFF) | ((uint16_t)(val & 1) << 8); break;
        case TSW_T1XOFFSL: r.t1_xoffs = (r.t1_xoffs & 0x100) | val; break;
        case TSW_T1XOFFSH: r.t1_xoffs = (r.t1_xoffs & 0xFF) | ((uint16_t)(val & 1) << 8); break;
        case TSW_T1YOFFSL: r.t1_yoffs = (r.t1_yoffs & 0x100) | val; break;
        case TSW_T1YOFFSH: r.t1_yoffs = (r.t1_yoffs & 0xFF) | ((uint16_t)(val & 1) << 8); break;

        // -- dma (registers stored; the engine is a later phase) --
        case TSW_DMASAL: r.saddr = (r.saddr & 0x3FFF00) | (val & 0xFE); break;
        case TSW_DMASAH: r.saddr = (r.saddr & 0x3FC0FF) | ((uint32_t)(val & 0x3F) << 8); break;
        case TSW_DMASAX: r.saddr = (r.saddr & 0x003FFF) | ((uint32_t)val << 14); break;
        case TSW_DMADAL: r.daddr = (r.daddr & 0x3FFF00) | (val & 0xFE); break;
        case TSW_DMADAH: r.daddr = (r.daddr & 0x3FC0FF) | ((uint32_t)(val & 0x3F) << 8); break;
        case TSW_DMADAX: r.daddr = (r.daddr & 0x003FFF) | ((uint32_t)val << 14); break;
        case TSW_DMALEN: r.dmalen = val; break;
        case TSW_DMANUM: r.dmanum = val; break;
        case TSW_DMACTR:
            r.dmactrl = val;
            dmaStart(val);
            break;
        default:
            break;
    }
}

// -------------------------------------------------- CPU write funnel ----

bool TsConf::cpuWriteGate(uint16_t addr, uint8_t val) {
    if (g_tsconf_wr & 0x10) fmWrite(addr, val);
    // W0_WE = 0 with RAM in window 0: the page reads as ROM (TS-BIOS's "boot
    // from VROM" maps a ROM image copied into RAM this way). The FMAddr
    // array still took the byte above — the hardware stores in parallel.
    return (g_tsconf_wr & 0x20) && addr < 0x4000;
}

// Guest write with FMAddr enabled — called from the CPU write funnel
// (gsDmaPoke8) BEFORE the normal store, which still proceeds (the hardware
// writes RAM and the FPGA array in parallel; reference z80_main.inl:108).
void TsConf::fmWrite(uint16_t addr, uint8_t val) {
    if (((addr >> 12) & 0x0F) != (r.fmaddr & 0x0F)) return;

    if (((addr >> 8) & 0x0F) == 0x04) {           // TSF_REGS: 0100 a[11:8]
        portWrite(addr & 0xFF, val);
        return;
    }
    // 512-byte word arrays at a[11:9]: 000 = CRAM, 001 = SFILE. Even byte is
    // latched; the odd address commits the 16-bit word.
    if (!(addr & 1)) {
        s_fm_tmp = val;
        return;
    }
    uint16_t w = ((uint16_t)val << 8) | s_fm_tmp;
    switch ((addr >> 9) & 0x07) {
        case 0:
            cram[(addr >> 1) & 0xFF] = w;
            VIDEO::tsCramDirty = true;
            break;
        case 1:
            sfile[(addr >> 1) & 0xFF] = w;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------- interrupts ----

void TsConf::frameIntRecalc() {
    // FRAME INT: a 32-CPU-clock pulse starting when the raster counters hit
    // (VSINT, HSINT) — zint.v. Expressed as the existing IntStart/IntEnd
    // window in scaled T-states. Out-of-range positions disable the source
    // (reference intctrl.frame_t = -1). A window straddling the frame end is
    // truncated at statesInFrame (documented Phase-1 limitation; the reset
    // position 2/0 and everything observed sit at the frame start).
    if (!Z80Ops::isTsconf) return;
    uint8_t m = ESPectrum::multiplicator;
    if (r.hsint > 223 || r.vsint > 319) {
        CPU::IntStart = 0;
        CPU::IntEnd = 0;
        return;
    }
    uint32_t t = ((uint32_t)r.vsint * TSTATES_PER_LINE_PENTAGON + r.hsint) << m;
    uint32_t len = 32u << m;
    CPU::IntStart = t;
    CPU::IntEnd = t + len;
    if (CPU::IntEnd > CPU::statesInFrame) CPU::IntEnd = CPU::statesInFrame;
}

// Poll the lazily-evaluated sources against the current T-state.
static void tsIntPoll() {
    const uint32_t t = CPU::tstates;
    if ((TsConf::r.intmask & 0x02) && t >= s_lin_next) {
        s_lin_pending = true;                 // latched until acknowledged
        s_lin_next = tsNextLineStart(t);
    }
    if (s_dma_busy && t >= s_dma_end) {
        s_dma_busy = false;
        if (TsConf::r.intmask & 0x04) s_dma_pending = true;
    }
}

// FRAME: the 32-clock window at (VSINT, HSINT), auto-expiring (intctr_fin in
// zint.v) and cleared by the acknowledge — same latetiming shift as the
// generic Z80Ops::isActiveINT so the two agree to the T-state.
static inline bool tsFrmActive() {
    if (!TsConf::frameIntEnabled() || s_frm_acked) return false;
    int32_t tmp = (int32_t)CPU::tstates + CPU::latetiming;
    if (tmp >= (int32_t)CPU::statesInFrame) tmp -= CPU::statesInFrame;
    return tmp >= CPU::IntStart && tmp < CPU::IntEnd;
}

bool TsConf::intLine() {
    tsIntPoll();
    return tsFrmActive() || s_lin_pending || s_dma_pending;
}

uint8_t TsConf::intAck() {
    // zint.v: int_sel picks by priority at the ack edge; int_frm clears on
    // any ack, int_lin only when FRAME is not pending, int_dma only when
    // neither is — i.e. exactly the source whose vector is driven.
    tsIntPoll();
    if (tsFrmActive())  { s_frm_acked = true;    return 0xFF; }
    if (s_lin_pending)  { s_lin_pending = false; return 0xFD; }
    if (s_dma_pending)  { s_dma_pending = false; return 0xFB; }
    return 0xFF;   // spurious (source dropped between sample and ack)
}

uint32_t TsConf::nextIntEvent() {
    tsIntPoll();
    const uint32_t now = CPU::tstates;
    if (tsFrmActive() || s_lin_pending || s_dma_pending) return now;
    uint32_t t = CPU::statesInFrame;
    if ((r.intmask & 0x02) && s_lin_next < t) t = s_lin_next;
    if (s_dma_busy && (r.intmask & 0x04) && s_dma_end < t) t = s_dma_end;
    if (frameIntEnabled() && !s_frm_acked) {
        // First T-state whose latetiming-shifted value enters [IntStart, IntEnd).
        int32_t ws = (int32_t)CPU::IntStart - CPU::latetiming;
        if (ws > (int32_t)now && (uint32_t)ws < t) t = (uint32_t)ws;
    }
    return t < now ? now : t;
}

// EI / RETN / RETI re-enabled interrupts inside an unchecked slice: if a source
// is already up, end the slice so CPU::loop can take the interrupt at the
// right instruction (Stage D runs unchecked between INT events).
void TsConf::intEnableHook() {
    if (intLine()) tsWakeLoop();
}

bool TsConf::needsCheckedFrame() {
    return (r.intmask & 0x02) || s_lin_pending ||
           ((r.intmask & 0x04) && (s_dma_busy || s_dma_pending));
}

void TsConf::endFrame() {
    const uint32_t f = CPU::statesInFrame;
    s_frm_acked = false;
    s_lin_next = (s_lin_next >= f) ? s_lin_next - f : 0;
    s_dma_end  = (s_dma_end  >= f) ? s_dma_end  - f : 0;
}

// ------------------------------------------------------------------ DMA ----
//
// Port of the reference dma_init/dma_next_burst/dma_* (tsconf.cpp) with the
// per-memory-cycle state machine collapsed: the whole transaction executes
// inside the DMACtrl write, and DMA_ACT + the DMA interrupt follow the time
// the hardware would have taken (per-word cost below, scaled to the CPU
// clock). Instant completion is the safe direction — software waits on
// DMA_ACT or the interrupt, and nothing can observe a half-written block.
//
// Addresses are 22-bit; bit 0 is forced even (word transfers). With S_ALGN /
// D_ALGN a block advances inside a 256/512-byte window (wrapping), and the
// REGISTER steps by the window size per block; without alignment the
// register follows the running address. That register update is what the
// next transaction starts from, so it is kept exactly as the reference does.

namespace {
struct DmaRam {
    uint32_t page = 0xFFFFFFFFu;
    uint8_t* ptr = nullptr;
    inline uint8_t* at(uint32_t a) {
        const uint32_t pg = a >> 14;
        if (pg != page) { page = pg; ptr = TsConf::pagePtr(pg); }
        return ptr ? ptr + (a & 0x3FFE) : nullptr;
    }
    inline uint16_t rd(uint32_t a) {
        const uint8_t* p = at(a);
        return p ? (uint16_t)(p[0] | (p[1] << 8)) : 0xFFFF;
    }
    inline void wr(uint32_t a, uint16_t v) {
        uint8_t* p = at(a);
        if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
    }
};
}

// Per-word cost in 3.5 MHz T-states, before the CPU-clock scaling: the DRAM
// controller serves one access per ~4 clocks at 28 MHz, so RAM→RAM (read +
// write) is ~2 T per word, one-sided transfers ~1 T, SPI is bound by the
// card's clock (~4 T per word at 14 MHz SCK).
static const uint8_t kDmaCostRam  = 2;
static const uint8_t kDmaCostOne  = 1;
static const uint8_t kDmaCostSpi  = 4;

#if PERF_TRACE
volatile uint32_t ts_dma_us = 0;      // wall time inside dmaStart per frame (PERF line)
volatile uint32_t ts_dma_words = 0;   // words moved per frame
#if PERF_HIST
uint32_t ts_dma_src_hist[256], ts_dma_dst_hist[256];   // words per physical page
#endif
#endif

void TsConf::dmaStart(uint8_t ctrl) {
#if PERF_TRACE
    const uint64_t dma_t0 = time_us_64();
#endif
    const bool     rw    = ctrl & 0x80;
    const uint8_t  dev   = ctrl & 0x07;
    const bool     asz   = ctrl & 0x08;
    const bool     dalgn = ctrl & 0x10;
    const bool     salgn = ctrl & 0x20;
    const bool     opt   = ctrl & 0x40;   // BLT2: saturate
    const uint32_t m1    = asz ? 0x3FFE00 : 0x3FFF00;
    const uint32_t m2    = asz ? 0x0001FF : 0x0000FF;
    const uint32_t asize = asz ? 512 : 256;
    const uint8_t  mode  = (uint8_t)((rw ? 8 : 0) | dev);
    enum { M_RAM = 0x01, M_SPIRAM = 0x02, M_IDERAM = 0x03, M_FILL = 0x04, M_BLT2 = 0x06,
           M_BLT1 = 0x09, M_RAMSPI = 0x0A, M_RAMIDE = 0x0B, M_CRAM = 0x0C, M_SFILE = 0x0D };

    uint32_t ss = r.saddr, dd = r.daddr;
    uint32_t len = (uint32_t)r.dmalen + 1;
    uint32_t num = r.dmanum;
    uint32_t words = 0;
    uint8_t cost = kDmaCostRam;
    DmaRam src, dst;
    uint16_t fill = 0;

    auto ss_inc = [&]() { ss = salgn ? ((ss & m1) | ((ss + 2) & m2)) : ((ss + 2) & 0x3FFFFF); };
    auto dd_inc = [&]() { dd = dalgn ? ((dd & m1) | ((dd + 2) & m2)) : ((dd + 2) & 0x3FFFFF); };

    switch (mode) {
        case M_RAM: case M_BLT1: case M_BLT2: case M_FILL: case M_CRAM: case M_SFILE:
        case M_SPIRAM: case M_RAMSPI:
            break;
        case M_IDERAM: case M_RAMIDE: {
            static bool warned = false;
            if (!warned) {
                warned = true;
                Debug::log("TsConf: IDE DMA (ctrl=%02X) not implemented — transaction dropped", ctrl);
            }
            return;   // reference: DMA_ST_NOP — DMA_ACT never rises
        }
        default:
            return;   // reserved device: no-op, like the reference
    }
    if (mode == M_FILL) {           // dma_fill: ONE source word, read up front
        fill = src.rd(ss);
        ss_inc();
        cost = kDmaCostOne;
    } else if (mode == M_CRAM || mode == M_SFILE) {
        cost = kDmaCostOne;
    } else if (mode == M_SPIRAM || mode == M_RAMSPI) {
        cost = kDmaCostSpi;
        LED::touchR(LED::ZCTRL);
    }

    // Words a run may cover from address `a` before it leaves its 16 KB page
    // or, with alignment on, wraps inside its 256/512-byte window.
    auto run = [&](uint32_t a, bool algn, uint32_t want) -> uint32_t {
        uint32_t n = (0x4000u - (a & 0x3FFFu)) >> 1;
        if (algn) { const uint32_t w = (asize - (a & (asize - 1))) >> 1; if (w < n) n = w; }
        return n < want ? n : want;
    };
    auto ss_add = [&](uint32_t n) { ss = salgn ? ((ss & m1) | ((ss + 2 * n) & m2)) : ((ss + 2 * n) & 0x3FFFFF); };
    auto dd_add = [&](uint32_t n) { dd = dalgn ? ((dd & m1) | ((dd + 2 * n) & m2)) : ((dd + 2 * n) & 0x3FFFFF); };
    const bool bulk = (mode == M_RAM || mode == M_BLT1 || mode == M_BLT2 || mode == M_FILL);

    for (;;) {
        // Bulk modes go run by run with direct pointers: a 320x240 256c screen
        // copy is ~38k words per frame, and the per-word rd()/wr() path cost
        // 0.6 us a word (TMNT: dma=16 ms of a 60 ms frame, hw 2026-09-06).
        for (uint32_t rem = len; rem; ) {
            uint32_t n = 1;
            if (bulk) {
                n = run(dd, dalgn, rem);
                if (mode != M_FILL) n = run(ss, salgn, n);
                uint8_t* dp = dst.at(dd);
                const uint8_t* sp = (mode != M_FILL) ? src.at(ss) : nullptr;
                if (!dp) {
                    // destination not POINTER-backed (degraded boot): swallow
                } else if (mode == M_FILL) {
                    if ((fill & 0xFF) == (fill >> 8)) memset(dp, fill & 0xFF, n * 2);
                    else for (uint32_t i = 0; i < n; i++) { dp[2*i] = (uint8_t)fill; dp[2*i+1] = (uint8_t)(fill >> 8); }
                } else if (!sp) {
                    memset(dp, 0xFF, n * 2);            // unbacked source reads 0xFFFF
                } else if (mode == M_RAM) {
                    // Hardware copies word by word ascending: overlapping regions
                    // propagate forwards, which memmove would not reproduce.
                    if (dp + n * 2 <= sp || sp + n * 2 <= dp) memcpy(dp, sp, n * 2);
                    else for (uint32_t i = 0; i < n * 2; i++) dp[i] = sp[i];
                } else if (mode == M_BLT1) {          // transparent copy: 0 pixels keep dst
                    if (asz) {                        // 256c: byte pixels
                        for (uint32_t i = 0; i < n * 2; i++) if (sp[i]) dp[i] = sp[i];
                    } else {                          // 16c: nibble pixels
                        for (uint32_t i = 0; i < n * 2; i++) {
                            const uint8_t sv = sp[i]; uint8_t dv = dp[i];
                            if (sv & 0xF0) dv = (dv & 0x0F) | (sv & 0xF0);
                            if (sv & 0x0F) dv = (dv & 0xF0) | (sv & 0x0F);
                            dp[i] = dv;
                        }
                    }
                } else {                              // M_BLT2: additive, optional saturation
                    if (asz) {
                        for (uint32_t i = 0; i < n * 2; i++) {
                            uint32_t v = (uint32_t)sp[i] + dp[i];
                            if (v > 0xFF && opt) v = 0xFF;
                            dp[i] = (uint8_t)v;
                        }
                    } else {
                        for (uint32_t i = 0; i < n * 2; i++) {
                            const uint8_t sv = sp[i], dv = dp[i];
                            uint32_t lo = (sv & 0xF) + (dv & 0xF), hi = (sv >> 4) + (dv >> 4);
                            if (opt) { if (lo > 0xF) lo = 0xF; if (hi > 0xF) hi = 0xF; }
                            dp[i] = (uint8_t)(((hi & 0xF) << 4) | (lo & 0xF));
                        }
                    }
                }
            } else switch (mode) {
                case M_CRAM: {
                    const uint8_t idx = (uint8_t)(dd >> 1);
                    cram[idx] = src.rd(ss);
                    VIDEO::tsCramDirty = true;
                    break;
                }
                case M_SFILE:
                    sfile[(uint8_t)(dd >> 1)] = src.rd(ss);
                    break;
                case M_SPIRAM: {               // Zc.Rd(0x10057) x2, low byte first
                    uint16_t v = DivMMC::zc_read_data();
                    v |= (uint16_t)DivMMC::zc_read_data() << 8;
                    dst.wr(dd, v);
                    break;
                }
                case M_RAMSPI: {
                    const uint16_t v = src.rd(ss);
                    DivMMC::zc_write_data((uint8_t)v);
                    DivMMC::zc_write_data((uint8_t)(v >> 8));
                    break;
                }
                default: break;
            }
#if PERF_TRACE && PERF_HIST
            if (mode != M_FILL && mode != M_SPIRAM) ts_dma_src_hist[(ss >> 14) & 0xFF] += n;
            if (mode != M_RAMSPI) ts_dma_dst_hist[(dd >> 14) & 0xFF] += n;
#endif
            if (mode != M_FILL && mode != M_SPIRAM) ss_add(n);
            if (mode != M_RAMSPI) dd_add(n);
            words += n;
            rem -= n;
        }
        // dma_next_burst
        if (salgn) { r.saddr = (r.saddr + asize) & 0x3FFFFF; ss = r.saddr; }
        else         r.saddr = ss;
        if (dalgn) { r.daddr = (r.daddr + asize) & 0x3FFFFF; dd = r.daddr; }
        else         r.daddr = dd;
        if (num) { num--; len = (uint32_t)r.dmalen + 1; }
        else break;
    }

    // DMA_ACT for the hardware's duration; the DMA interrupt is raised when it
    // drops (tsIntPoll). A transaction started while a previous one is still
    // "busy" simply supersedes it — the data is long written either way.
    s_dma_busy = true;
    s_dma_end = CPU::tstates + ((words * cost) << ESPectrum::multiplicator);
    if (needsCheckedFrame()) tsWakeLoop();
#if PERF_TRACE
    ts_dma_us += (uint32_t)(time_us_64() - dma_t0);
    ts_dma_words += words;
#endif
}

uint8_t TsConf::dmaStatus() {
    tsIntPoll();
    return s_dma_busy ? 0x80 : 0x00;
}

// ------------------------------------------------------------ CPU clock ----

void TsConf::applyZclk(bool fromGuest) {
    // ZCLK 00/01/10 = 3.5/7/14 MHz -> multiplicator 0/1/2 (11 is reserved —
    // treated as 14), capped by Config::tsconf_clk_cap for boards that cannot
    // keep 14 MHz. The register is authoritative — TS-BIOS sets it at boot from
    // its Setup, an .spg header carries it, games write it — so the user's
    // Alt+F2 pick is NOT folded in here: the hotkey acts as an override that
    // lasts until the guest's next SysConfig write (the hotkey handlers no
    // longer call this). Was "user pick is a floor" until 2026-09-06.
    uint8_t zclk = r.sysconf & 0x03;
    if (zclk == 3) zclk = 2;
    if (zclk > Config::tsconf_clk_cap) zclk = Config::tsconf_clk_cap;
    if (zclk != ESPectrum::multiplicator) {
        ESPectrum::multiplicator = zclk;
        CPU::updateStatesInFrame();  // calls frameIntRecalc() for TS-Conf
        static bool warned14 = false;
        if (zclk == 2 && !warned14) {
            warned14 = true;
            Debug::log("TsConf: guest selected 14 MHz — may overrun the frame budget");
        }
        if (fromGuest) {
            static const char* const mhz[3] = { " CPU: 3.5 MHz ", " CPU: 7 MHz ", " CPU: 14 MHz " };
            OSD::notify(mhz[zclk], LEVEL_INFO, 900);
        }
    }
}

// --------------------------------------------------------------- reset ----

void TsConf::reset(bool cold) {
    // tsinit() values.
    r.page[0] = 0; r.page[1] = 5; r.page[2] = 2; r.page[3] = 0;
    r.fmaddr = 0;
    r.intmask = 1;
    r.fddvirt = 0;
    r.sysconf = 0;       // 3.5 MHz
    r.memconf = 0;       // mapped mode, ROM, W0 read-only
    r.cacheconf = 0;
    r.hsint = 2;
    r.vsint = 0;
    r.p7ffd = 0;
    r.vpage = r.vpage_d = 5;
    r.vconf = r.vconf_d = 0;
    r.tsconf = r.tsconf_d = 0;
    r.palsel = r.palsel_d = 0x0F;   // gpal = 15 -> CRAM #F0-#FF (the ZX bank)
    r.border = 0xF0;
    r.g_xoffs = r.g_yoffs = 0;
    r.g_yoffs_updated = false;
    r.t0_xoffs = r.t0_yoffs = r.t1_xoffs = r.t1_yoffs = 0;
    r.tmpage = r.t0gpage = r.t1gpage = r.sgpage = 0;
    r.dmalen = r.dmanum = r.dmactrl = 0;
    r.saddr = r.daddr = 0;
    if (cold) {
        r.pwr_up = 0x40;
        // Real CRAM is undefined at power-up and TS-BIOS programs it via
        // FMAddr. Seed the ZX bank with the standard palette anyway so a
        // guest that skips CRAM init stays visible. RGB555: R=t>>10 G=t>>5
        // B=t, 5-bit channels.
        static const uint16_t zx555[16] = {
            0x0000, 0x0010, 0x4000, 0x4010, 0x0200, 0x0210, 0x4200, 0x4210,
            0x0000, 0x0018, 0x6000, 0x6018, 0x0300, 0x0318, 0x6300, 0x6318,
        };
        for (int i = 0; i < 256; i++) cram[i] = 0;
        for (int i = 0; i < 16; i++) cram[0xF0 + i] = zx555[i];
        for (int i = 0; i < 256; i++) sfile[i] = 0;
    }
    s_fm_tmp = 0;
    s_frm_acked = false;
    s_lin_pending = s_dma_pending = s_dma_busy = false;
    s_lin_next = 0;
    s_dma_end = 0;
    tsUpdateWrGate();
    setBanks();
    applyZclk();
    frameIntRecalc();
    VIDEO::tsCramDirty = true;
    // TS-BIOS validates its NVRAM config (Gluk cells #B0..#E7) at every START
    // and falls into the text-mode SETUP — invisible until phase 3 — when the
    // CRC fails. Re-check here on every reset: a no-op (logged "valid") when
    // the BIOS would accept the cells, its own defaults + CRC otherwise.
    RTC::tsBiosSeed();
}
