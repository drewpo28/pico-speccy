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
#include "ZxEvoAvr.h"
#include "DivMMC.h"
#include "LEDIndicators.h"
#include "OSDMain.h"

// Hot path in SRAM: every #nnAF port access (TMNT: ~4000 a frame — DMA register
// programming + DMAStatus polls), every INT-source poll from CPU::loop's Stage D
// and the DMA copy itself. In flash they queued behind the PSRAM line fills of
// the core1 renderer and the DMA data on the ONE XIP port (929 ns/instr in the
// ship scene against 587 in a light one, hw 2026-09-07). ~3 KB of SRAM.
#ifndef TS_VIDEO_TRACE
#define TS_VIDEO_TRACE 0
#endif
#if TS_VIDEO_TRACE
// Capped trace of the video-mode registers a game may flip mid-frame, with the
// frame line and the writer's PC — for "which register did the game toggle
// around its INT handler" questions (Ninja Gaiden background flicker, 2026-09-07).
static uint32_t s_vtrace_left = 1u << 30;
static uint32_t s_vtrace_frame = 0;
#define TSVT(fmt, ...) do { if (s_vtrace_left) { s_vtrace_left--; \
    Debug::log("[TSVT] f%u L%03u pc=%04X " fmt, (unsigned)s_vtrace_frame, (unsigned)(CPU::tstates / tsLineT()), Z80::getRegPC(), ##__VA_ARGS__); } } while (0)
// Offsets: log only a CHANGE of the 9-bit value (a game rewrites them every frame).
#define TSVT_CHG(name, before, after) do { if ((before) != (after)) TSVT(name "=%03X", (unsigned)(after)); } while (0)
#else
#define TSVT(fmt, ...) do {} while (0)
#define TSVT_CHG(name, before, after) do { (void)(before); (void)(after); } while (0)
#endif
// Mode registers seen ENABLED at any point of the frame: a game may switch TSU
// layers / VConfig off around its INT handler and back before the frame ends,
// and the EndFrame-sampled mode must not follow that window (a whole frame
// without tiles otherwise — hw 2026-09-07). Consumed by VIDEO::tsVideoApplyPending.
uint8_t TsConf::tsuSeen = 0;

#if TSCONF_HOT_IN_RAM
#define TS_HOT __not_in_flash("tsconf")
#else
#define TS_HOT
#endif

TsConf::Regs TsConf::r;
uint16_t TsConf::cram[256];
uint16_t TsConf::sfile[256];
uint32_t TsConf::sfileGen = 0;

// Write-side gate (see TsConf.h): 0x10|window while FMAddr is enabled, 0x20
// while window 0 is write-protected RAM. Tested predicted-not-taken in the CPU
// write funnel (gsDmaPoke8) — the g_ngs_zxdma pattern; zero for every other
// machine, so the cost elsewhere is one byte-load-and-test per guest write.
uint8_t g_tsconf_wr = 0;
uint8_t g_ts_bank_watch = 0;
static uint8_t s_bank_phys[4];   // physical page per CPU bank (setBanks)

// FMAddr 512-byte windows latch the even byte here and commit the word on the
// odd address (reference temp.fm_tmp).
static uint8_t s_fm_tmp = 0;

static void tsUpdateWrGate() {
    uint8_t g = 0, wt = 0;
    if (Z80Ops::isTsconf) {
        if (TsConf::r.fmaddr & 0x10) g |= (TsConf::r.fmaddr & 0x0F) | 0x10;
        if (TsConf::r.w0_ram() && !TsConf::r.w0_we()) g |= 0x20;
        for (int b = 0; b < 4; b++)
            if ((b || TsConf::r.w0_ram()) && VIDEO::tsWatchedPage(s_bank_phys[b])) wt |= (uint8_t)(1u << b);
        if (wt) g |= 0x40;
    }
    g_ts_bank_watch = wt;
    g_tsconf_wr = g;
}
void TsConf::wrGateRecalc() { tsUpdateWrGate(); }
uint32_t TsConf::bankPhys(uint8_t bank, uint16_t off) {
    return ((uint32_t)s_bank_phys[bank & 3] << 14) | (off & 0x3FFF);
}

// ------------------------------------------------ INT / DMA state ----
// Frame-relative, in scaled CPU T-states (CPU::tstates units). The sources are
// evaluated LAZILY from CPU::tstates when the CPU samples the line or reads
// DMAStatus — there is no per-line hook, and none is needed: while LINE or DMA
// interrupts are armed CPU::loop runs instruction-checked, so intLine() is
// consulted after every instruction anyway.
static bool     s_frm_acked;    // int_frm cleared by the ack — for the CURRENT window only (see frameIntRecalc)
static uint16_t s_frm_vsint = 0xFFFF, s_frm_hsint = 0xFFFF;   // window position the latch belongs to
static bool     s_lin_pending;  // int_lin latch
static uint32_t s_lin_next;     // T of the next line start that raises LINE
static bool     s_dma_busy;     // DMA_ACT: transaction "in flight"
static uint32_t s_dma_end;      // T at which it completes
static bool     s_dma_pending;  // int_dma latch
#if PERF_TRACE
volatile uint32_t ts_int_frm = 0;   // FRAME INT acks per PERF window (a raster-split title acks 2 per frame)
#if PERF_TRACE
// FRAME accepts taken 32+ T after the window opened. Our window is 32 T at
// 3.5 MHz scaled by the clock (32 << m, i.e. 128 T at ZCLK 14) — IF zint.v's
// intctr counts Z80 clocks, hardware closes it after 32 T of the CURRENT
// clock, and every accept counted here is one real hardware would have
// dropped. Unverified against the RTL; that is what this counter is for.
volatile uint32_t ts_int_late = 0;
// FRAME windows armed at frame end and never acknowledged (counted in
// endFrame — a missed window stays put until the guest moves it, so the one
// at frame end is the one that was missed).
volatile uint32_t ts_int_miss = 0;
// The ring itself: oldest entry at ts_int_ring_w, newest at w-1.
#define TS_INT_FREEZE 0   // 1 = stop the ring on fishbone's failure signatures; 0 = always keep the last N accepts
#define TS_INT_RING_N 512   // accepts kept (power of two); 512 ~ 1.6 fishbone frames
struct TsIntRec { uint16_t pc, sp, ppc, vs; uint32_t t; uint8_t lat, src; };  // ppc = prev accept's pc; vs = VSINT
// t is the frame T-state (>> m) and MUST be 32-bit: a TS-Conf frame is 71680
// unscaled T, so a uint16 silently wrapped everything past line 292 (65536/224)
// and made healthy late-frame accepts read as "line 0" — a trace bug that
// false-fired the freeze trigger below (hw 2026-09-09).
TsIntRec ts_int_ring[TS_INT_RING_N];
uint16_t ts_int_ring_w = 0;
uint8_t  ts_int_frozen = 0;           // 1 = trigger fired, ring holds the transition (see intTrace)
static uint8_t  s_int_last_lat = 0;   // set by intAck for the record intTrace is about to write
static uint16_t s_int_prev_pc  = 0;
static uint32_t s_int_prev_t   = 0;   // previous accept's frame T (>> m), for the cadence trigger
static uint16_t s_int_run      = 0;   // consecutive accepts inside the music player
// How much of fishbone's deliberate 32-line interrupt-free window its frame
// work actually uses. The demo steps VSINT 32,33,...,319,0 and back to 32, so
// after the line-0 window there are 32 lines with no interrupt at all — that is
// where it runs the PT3 player, whose LD SP,HL / POP reads have no DI around
// them. Overrun that window and the next interrupt lands inside an SP trick:
// that is the whole failure (hw 2026-09-09, the ring caught one frame fitting
// and the next one not). Measured from the line-0 ack to the HALT that ends the
// frame work, in T-states at the current ZCLK; 32 lines = 32*224<<m.
uint32_t ts_work_max = 0, ts_work_min = 0xFFFFFFFF, ts_work_sum = 0, ts_work_cnt = 0;
// THE danger event, counted directly: an interrupt accepted inside the player
// (0x9400-0x9AD0) that is NOT the line-0 one. A healthy frame has exactly zero
// of these — the only accept that legitimately lands in the player is the vs==0
// window, which arrives one line after the player starts and hits it before the
// first LD SP,HL. Everything after that is supposed to run inside the demo's
// 32-line interrupt-free gap. Any other hit means an interrupt fell into an SP
// trick's window, which is what destroys the demo (hw 2026-09-09).
uint32_t ts_plyr_hit = 0;
uint32_t ts_miss_ei = 0, ts_miss_di = 0, ts_miss_halt = 0;
// ISR cost, measured from the FRAME ack to the EI that ends the handler. The
// demo takes 289 interrupts a frame, so this figure is multiplied by 289: an
// error of 16 T per interrupt is 5 raster lines a frame, which is exactly the
// scale that decides whether the player fits in its 32-line window. Hand-count
// from the disassembly of 8254-82AD is 19 (IM2 ack) + 355 (to the end of EI).
uint32_t ts_isr_min = 0xFFFFFFFF, ts_isr_max = 0, ts_isr_sum = 0, ts_isr_cnt = 0;
static uint32_t s_isr_t0 = 0;
static bool     s_isr_armed = false;
static uint32_t s_work_t0 = 0;
static bool     s_work_armed = false;
#endif
#endif

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

// 32-page ROM window. Only the 4 pages of the selected TS-BIOS set are bound
// (they are the first 64 KB of the real 512 KB flash — the ZX-Evo images differ
// from each other in page 2 alone, which is what Config::romSetTsconf picks);
// pages 4..31 answer like unbonded flash. gb_rom_Alf_ep (the ALF open-bus 16 KB
// zero page) is reused as the filler — reads there are deterministic 0x00.
extern "C" const unsigned char gb_rom_Alf_ep[];

// Set by bindRoms() before the machine runs; the filler covers the window before
// the first bind (a boot that never passes through requestMachine cannot happen,
// but a null here would be a hard fault instead of open bus).
static const uint8_t* s_rom_page[4] = { nullptr, nullptr, nullptr, nullptr };

const uint8_t* TsConf::romPtr(uint8_t page) {
    page &= 0x1F;
    if (page < 4 && s_rom_page[page]) return s_rom_page[page];
    return gb_rom_Alf_ep;
}

void TsConf::bindRoms(const uint8_t* const pages[4]) {
    // Window 0 is a RAW POINTER into flash (setBanks -> ramCurrent[0], read by the
    // TsFastMem path), so these four pages must be raw arrays — which is why the
    // TR-DOS and 128K-ROM0 overlay families were inverted to make the variants
    // TS-Conf needs their bases (tools/rom_pack.py). Nothing goes into MemESP::rom[]:
    // rom[4] (TR-DOS, bound by requestMachine's tail) stays for the Beta-128 path,
    // which TS-BIOS never uses — it carries its own TR-DOS in page 1.
    for (int i = 0; i < 4; ++i) s_rom_page[i] = pages[i];
}

// -------------------------------------------------------------- paging ----

// RAM unconditionally (36 bytes): tsuComposeLine calls this once per TILE —
// ~84 times per rendered line, plus once per sprite element — and it is on
// core1, where a flash fetch queues behind that same line's PSRAM tile reads.
// It stays out of the TSCONF_HOT_IN_RAM group on purpose: the renderer needs
// it whether or not the port/DMA code is resident.
uint8_t* __not_in_flash("tsconf_pageptr") TsConf::pagePtr(uint32_t page) {
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
    s_bank_phys[1] = (uint8_t)(r.page[1] & mask); s_bank_phys[2] = (uint8_t)(r.page[2] & mask); s_bank_phys[3] = (uint8_t)(r.page[3] & mask);

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
    s_bank_phys[0] = (uint8_t)(p0 & mask);
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

uint16_t TsConf::dbg_p7ffd = 0, TsConf::dbg_p7ffd_locked = 0;

void TsConf::write7ffd(uint8_t val) {
    dbg_p7ffd++;
    // 48-lock: bit 5 of the LATCHED value blocks further writes (no unlock
    // backdoor on TS-Conf — reference io.cpp:707). LCK128=11 clears bit 5
    // before latching, so 1024K mode can never lock.
    if (r.p7ffd & 0x20) { dbg_p7ffd_locked++; return; }

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
    // SCR bit — no line latch. A change of the displayed page is what arms
    // Gigascreen's Auto mode: every other machine bumps the countdown from its
    // own #7FFD videoLatch flip, and TS-Conf takes this handler INSTEAD of that
    // one (Ports.cpp early-delegates), so without this Auto never engaged here.
    const uint8_t scr_page = (val & 0x08) ? 7 : 5;
    if (r.vpage != scr_page) VIDEO::gigascreenAutoFlip();
    r.vpage = r.vpage_d = scr_page;
    setBanks();
}

// --------------------------------------------------------------- ports ----

TS_HOT uint8_t TsConf::portRead(uint8_t reg) {
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

TS_HOT void TsConf::portWrite(uint8_t reg, uint8_t val) {
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
        case TSW_INTMASK: TSVT("INTMASK=%02X", val);  {
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
        case TSW_HSINT: TSVT("HSINT=%02X", val);
            r.hsint = val;
            frameIntRecalc();
            break;
        case TSW_VSINTL: TSVT("VSINTL=%02X", val);
            r.vsint = (r.vsint & 0x100) | val;
            frameIntRecalc();
            break;
        case TSW_VSINTH: TSVT("VSINTH=%02X", val);
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
        case TSW_VCONF:  TSVT("VCONF=%02X", val); r.vconf  = r.vconf_d  = val; tsUpdateWrGate(); break;
        case TSW_VPAGE:
            TSVT("VPAGE=%02X", val);
            // The native way a TS program flips screens (the #7FFD SCR bit is the
            // 128K-compatible one) — Gigascreen Auto has to see it too.
            if (r.vpage != val) VIDEO::gigascreenAutoFlip();
            r.vpage = r.vpage_d = val;
            refreshGrmem();
            tsUpdateWrGate(); break;
        case TSW_TMPAGE:  TSVT("TMPAGE=%02X", val); r.tmpage  = val; tsUpdateWrGate(); break;
        case TSW_T0GPAGE: TSVT("T0GPAGE=%02X", val); r.t0gpage = val; tsUpdateWrGate(); break;
        case TSW_T1GPAGE: TSVT("T1GPAGE=%02X", val); r.t1gpage = val; tsUpdateWrGate(); break;
        case TSW_SGPAGE: TSVT("SGPAGE=%02X", val);  r.sgpage  = val; tsUpdateWrGate(); break;
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
        case TSW_TSCONF: TSVT("TSCONF=%02X", val); r.tsconf = r.tsconf_d = val; tsuSeen |= val & 0xE0; tsUpdateWrGate(); break;
        case TSW_PALSEL:
            r.palsel = r.palsel_d = val;
            VIDEO::tsCramDirty = true;  // gpal re-points the 16 ZX slots
            break;
        case TSW_GXOFFSL: r.g_xoffs = (r.g_xoffs & 0x100) | val; break;
        case TSW_GXOFFSH: r.g_xoffs = (r.g_xoffs & 0xFF) | ((uint16_t)(val & 1) << 8); break;
        case TSW_GYOFFSL: r.g_yoffs = (r.g_yoffs & 0x100) | val; r.g_yoffs_updated = true; break;
        case TSW_GYOFFSH: r.g_yoffs = (r.g_yoffs & 0xFF) | ((uint16_t)(val & 1) << 8); r.g_yoffs_updated = true; break;
        case TSW_T0XOFFSL: { r.t0_xoffs = (r.t0_xoffs & 0x100) | val; TSVT("T0X=%03X", (unsigned)r.t0_xoffs); break; }
        case TSW_T0XOFFSH: { r.t0_xoffs = (r.t0_xoffs & 0xFF) | ((uint16_t)(val & 1) << 8); TSVT("T0XH=%03X", (unsigned)r.t0_xoffs); break; }
        case TSW_T0YOFFSL: { const uint16_t o = r.t0_yoffs; r.t0_yoffs = (r.t0_yoffs & 0x100) | val; TSVT_CHG("T0Y", o, r.t0_yoffs); break; }
        case TSW_T0YOFFSH: { const uint16_t o = r.t0_yoffs; r.t0_yoffs = (r.t0_yoffs & 0xFF) | ((uint16_t)(val & 1) << 8); TSVT_CHG("T0Y", o, r.t0_yoffs); break; }
        case TSW_T1XOFFSL: { r.t1_xoffs = (r.t1_xoffs & 0x100) | val; TSVT("T1X=%03X", (unsigned)r.t1_xoffs); break; }
        case TSW_T1XOFFSH: { r.t1_xoffs = (r.t1_xoffs & 0xFF) | ((uint16_t)(val & 1) << 8); TSVT("T1XH=%03X", (unsigned)r.t1_xoffs); break; }
        case TSW_T1YOFFSL: { const uint16_t o = r.t1_yoffs; r.t1_yoffs = (r.t1_yoffs & 0x100) | val; TSVT_CHG("T1Y", o, r.t1_yoffs); break; }
        case TSW_T1YOFFSH: { const uint16_t o = r.t1_yoffs; r.t1_yoffs = (r.t1_yoffs & 0xFF) | ((uint16_t)(val & 1) << 8); TSVT_CHG("T1Y", o, r.t1_yoffs); break; }

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

TS_HOT bool TsConf::cpuWriteGate(uint16_t addr, uint8_t val) {
    if ((g_tsconf_wr & 0x40) && ((g_ts_bank_watch >> (addr >> 14)) & 1))
        VIDEO::tsRenderDrainOverlap(bankPhys((uint8_t)(addr >> 14), addr), 1);
    if (g_tsconf_wr & 0x10) fmWrite(addr, val);
    // W0_WE = 0 with RAM in window 0: the page reads as ROM (TS-BIOS's "boot
    // from VROM" maps a ROM image copied into RAM this way). The FMAddr
    // array still took the byte above — the hardware stores in parallel.
    return (g_tsconf_wr & 0x20) && addr < 0x4000;
}

// Guest write with FMAddr enabled — called from the CPU write funnel
// (gsDmaPoke8) BEFORE the normal store, which still proceeds (the hardware
// writes RAM and the FPGA array in parallel; reference z80_main.inl:108).
TS_HOT void TsConf::fmWrite(uint16_t addr, uint8_t val) {
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
            sfileGen++;
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
    //
    // The window is NOT once per frame. zint.v sets int_frm on every
    // int_start_frm strobe — i.e. every time the raster counters hit
    // (VSINT, HSINT) — and clears it on the ack or the 32-clock expiry; a
    // guest that moves VSINT inside the handler gets a second window in the
    // same frame. Ninja Gaiden does exactly that for its raster split (INT at
    // 272: T0X=0, VSINT:=94; INT at 94: T0X=X, VSINT:=272), and a per-frame
    // ack latch lost every other one — hw 2026-09-07, the background alternated
    // between the level's first frame and the scrolled one. So the ack latch
    // belongs to a window POSITION: moving the window re-arms it, and a running
    // unchecked slice is ended so CPU::loop re-slices against the new window.
    if (!Z80Ops::isTsconf) return;
    if (r.vsint != s_frm_vsint || r.hsint != s_frm_hsint) {
        s_frm_vsint = r.vsint; s_frm_hsint = r.hsint;
        s_frm_acked = false;
        tsWakeLoop();
    }
    uint8_t m = ESPectrum::multiplicator;
    if (r.hsint > 223 || r.vsint > 319) {
        CPU::IntStart = 0;
        CPU::IntEnd = 0;
        return;
    }
    // HSINT is measured from TS-Conf's own raster origin, which sits
    // TS_HSINT_RASTER_BIAS T-states before the point our Pentagon-derived
    // raster calls T=0. The ZX-Evo is Pentagon-compatible, so its RESET value
    // (hsint = 2) has to put the frame interrupt exactly where a Pentagon's
    // lands — otherwise no Pentagon software would keep its raster on an Evo.
    // Measured: "Across the Edge" executes an identical 1638 T between the
    // interrupt and its first OUT (#FE) on both machines, yet the interrupt
    // itself was taken at T=0 on Pentagon and T~6 on TS-Conf, and its border
    // split came out 8 px right (hw 2026-09-09).
    // 0 = the hardware formula, and it stays 0: three biases were tried on
    // 2026-09-09 against "Across the Edge" and none fixed its border on TS-Conf.
    //
    // What the RTL does say (video_sync.v:132,
    //   assign int_start_s = (hcount == {hint_beg, 1'b0}) && (vcount == vint_beg))
    // is that hcount counts 7 MHz pixel periods, so HSINT is in 3.5 MHz T-states
    // and our vsint*224 + hsint is right. It also fixes the one distance that
    // matters: with the reset hsint=2 the interrupt sits at hcount 4 of line 0,
    // paper (256x192) starts at vp_beg=80 / hp_beg=140, so hardware puts
    // (80*448 + 140 - 4) / 2 = 17988 T between the interrupt and the first paper
    // pixel. Ours is TS_SCREEN_PENTAGON (17983) - 2 = 17981, i.e. 7 T short —
    // which is where the empirically "right" bias of 7 came from. Applying it to
    // the INT position is WRONG though: that is the one direction that breaks,
    // because pos goes negative and the straddle truncation below cuts the
    // window from 32 T to 5 (interrupts lost wholesale, keyboard dead with them).
    // If this is retried, move the RASTER instead — tStatesScreen for TS-Conf —
    // and check the datasheet's own "Pentagon-128 compatibility / INT position"
    // section, which is an empty stub as of this writing.
    // Measured on "Across the Edge" (hw 2026-09-09), same effect on both
    // machines: the guest spends an identical 1638 T between the interrupt and
    // its first OUT (#FE), yet the interrupt was taken at T=0..3 on Pentagon and
    // T=6..9 on TS-Conf, and the border split came out 16 px right. Those 6 T
    // are 2 of window offset plus 4 of snapping: a HALTed Z80 samples INT only
    // on its 4 T NOP grid, the grid is aligned to the frame end (71680 % 4 == 0)
    // so its points are 0, 4, 8..., and a window opening at 2 is first seen at 4.
    // With the bias the window opens at 0 and both effects vanish together.
    // DO NOT make this larger than hsint: 7 was tried and BROKE THE MACHINE —
    // pos goes negative, wraps to 71675, and the straddle truncation below cuts
    // the window from 32 T to 5, so interrupts are lost wholesale and the
    // keyboard dies with them (TS-BIOS and TR-DOS read it from the handler).
    static constexpr int TS_HSINT_RASTER_BIAS = 0;
    int32_t pos = (int32_t)r.vsint * TSTATES_PER_LINE_PENTAGON + (int32_t)r.hsint - TS_HSINT_RASTER_BIAS;
    if (pos < 0) pos += (int32_t)(CPU::statesInFrame >> m);
    uint32_t t = (uint32_t)pos << m;
    // zint.v counts the pulse in ZPOS ticks — Z80 clocks at the CURRENT ZCLK —
    // so the window is 32 of OUR T-states at every clock, not 32 scaled ones.
    // It was `32u << m` (128 at 14 MHz, 4x the hardware). Nothing observed was
    // ever accepted past 32 (frmLate stayed 0 over every capture), so this is a
    // correctness fix, not a behaviour change — but it is the only length the
    // RTL supports. WATCH frmLate on a raster-split title (Ninja Gaiden): a
    // non-zero count there would mean the shorter window is dropping an
    // interrupt a long instruction used to straddle into.
    uint32_t len = 32u;
    CPU::IntStart = t;
    CPU::IntEnd = t + len;
    if (CPU::IntEnd > CPU::statesInFrame) CPU::IntEnd = CPU::statesInFrame;
}

// Poll the lazily-evaluated sources against the current T-state.
TS_HOT static void tsIntPoll() {
    const uint32_t t = CPU::tstates;
    if ((TsConf::r.intmask & 0x02) && t >= s_lin_next) {
        s_lin_pending = true;                 // latched until acknowledged
        s_lin_next = tsNextLineStart(t);
    }
    if (s_dma_busy && t >= s_dma_end) {
        s_dma_busy = false;
        VIDEO::tsRenderDrainDma();   // the guest may read the result from here on
        if (TsConf::r.intmask & 0x04) s_dma_pending = true;
    }
}

// FRAME: the 32-clock window at (VSINT, HSINT), auto-expiring (intctr_fin in
// zint.v) and cleared by the acknowledge — same latetiming shift as the
// generic Z80Ops::isActiveINT so the two agree to the T-state.
static inline bool tsFrmActive() {
    if (!TsConf::frameIntEnabled() || s_frm_acked) return false;
    // NO latetiming here: that is the ULA's Early/Late sampling shift, and
    // TS-Conf's interrupt comes from zint.v's own counter, not from a ULA.
    // The reference (Unreal ts_frame_int) applies no such shift either. It used
    // to add CPU::latetiming, i.e. Config::AluTiming, so a user running Late
    // timing moved every TS-Conf interrupt one T-state.
    int32_t tmp = (int32_t)CPU::tstates;
    if (tmp >= (int32_t)CPU::statesInFrame) tmp -= CPU::statesInFrame;
    return tmp >= CPU::IntStart && tmp < CPU::IntEnd;
}

TS_HOT bool TsConf::intLine() {
    tsIntPoll();
    return tsFrmActive() || s_lin_pending || s_dma_pending;
}

TS_HOT uint8_t TsConf::intAck() {
    // zint.v: int_sel picks by priority at the ack edge; int_frm clears on
    // any ack, int_lin only when FRAME is not pending, int_dma only when
    // neither is — i.e. exactly the source whose vector is driven.
    tsIntPoll();
    if (!tsFrmActive()) TSVT("INT ack frm=0 lin=%d dma=%d", (int)s_lin_pending, (int)s_dma_pending);   // FRAME acks are the norm — count them (PERF ts frmInt=) instead
    if (tsFrmActive())  { s_frm_acked = true;
#if PERF_TRACE
        ts_int_frm++;
        {   // how deep into the window the accept came (see ts_int_late)
            int32_t tmp = (int32_t)CPU::tstates;
            if (tmp >= (int32_t)CPU::statesInFrame) tmp -= CPU::statesInFrame;
            const int32_t lat = tmp - (int32_t)CPU::IntStart;
            s_int_last_lat = (uint8_t)(lat < 0 ? 0 : lat > 127 ? 127 : lat);
            if (lat >= 32) ts_int_late++;
        }
        if (r.vsint == 0) { s_work_t0 = CPU::tstates; s_work_armed = true; }   // the interrupt-free window opens here
        s_isr_t0 = CPU::tstates; s_isr_armed = true;
#endif
        return 0xFF; }
    if (s_lin_pending)  { s_lin_pending = false; return 0xFD; }
    if (s_dma_pending)  { s_dma_pending = false; return 0xFB; }
    return 0xFF;   // spurious (source dropped between sample and ack)
}

#if PERF_TRACE
// Called from CPU::loop the moment the guest goes HALTed (Stage D). Closes the
// frame-work measurement armed by the line-0 ack.
TS_HOT void TsConf::workHalt() {
    if (!s_work_armed) return;
    s_work_armed = false;
    uint32_t d = CPU::tstates - s_work_t0;
    if ((int32_t)d < 0) d += CPU::statesInFrame;
    if (d > ts_work_max) ts_work_max = d;
    if (d < ts_work_min) ts_work_min = d;
    ts_work_sum += d; ts_work_cnt++;
}

TS_HOT void TsConf::intTrace(uint16_t pc, uint16_t sp, uint8_t vect, bool halted) {
    // Freeze when the MAIN THREAD starts executing memory that holds nothing
    // but zeros, which is the FIRST visible step of fishbone's failure and
    // comes long before the ROM PC an earlier trigger keyed on. In this demo
    // 0x9ACF-0xBBBA is free RAM (the IM2 table at I=BE points every vector at
    // 0xBBBB, where the demo put its JP 0x8254; everything below is filler),
    // so a PC in there is always a NOP slide from a bad jump — and the slide
    // ends in that JP, entering the handler body as ordinary code.
    // Freezing on the ROM PC instead left all 256 records post-mortem: the
    // three dumps of 2026-09-09 were IDENTICAL down to the lat values
    // (w=196, lines 165..187), i.e. the failure is fully deterministic, so
    // the trigger can be moved earlier and earlier until it catches the
    // origin. ROM is kept as a fallback for a run that skips the slide.
    if (ts_int_frozen) { s_int_prev_pc = pc; s_int_last_lat = 0; return; }
    const uint32_t t = CPU::tstates >> ESPectrum::multiplicator;
    TsIntRec& r_ = ts_int_ring[ts_int_ring_w & (TS_INT_RING_N - 1)];
    r_.pc = pc; r_.sp = sp; r_.ppc = s_int_prev_pc; r_.t = t; r_.vs = r.vsint;
    r_.lat = (uint8_t)(s_int_last_lat | (halted ? 0x80 : 0));   // bit 7 = woke from HALT
    r_.src = vect;                                                // FF FRAME / FD LINE / FB DMA
    ts_int_ring_w = (uint16_t)((ts_int_ring_w + 1) & (TS_INT_RING_N - 1));
    // Primary trigger: the player is RUNNING AWAY. fishbone calls its PT3-style
    // player once per frame from the main loop (0x80B3), which is ~13 lines of
    // work, i.e. ~13 accepts land inside it. A run of 40+ consecutive accepts
    // in 0x9400-0x9AD0 means it is looping, and that happens ~1.5 frames before
    // anything else shows: by the time the PC reaches free RAM or ROM the ring
    // is already all post-mortem (three identical dumps proved it).
    // NB do NOT trigger on writes to 0x6000: the demo parks its stack on top of
    // the module's ASCII title (the player never reads it), so that word is
    // rewritten constantly and legitimately — a write breakpoint there fires on
    // the fourth T-state of the demo (hw 2026-09-09).
    if (pc >= 0x9400 && pc < 0x9AD0) {
        if (vect == 0xFF && r.vsint != 0) ts_plyr_hit++;   // an interrupt inside the player, outside the safe one
        if (s_int_run < 0xFFFF) s_int_run++;
    }
    else s_int_run = 0;
#if TS_INT_FREEZE
    if (s_int_run > 40) ts_int_frozen = 1;
#endif
#if TS_INT_FREEZE
    if (pc < 0x4000 || (pc >= 0x9B00 && pc < 0xBBBB)) ts_int_frozen = 1;   // fallbacks
#endif
    s_int_prev_t = t;
    s_int_prev_pc = pc;
    s_int_last_lat = 0;
}
#endif

TS_HOT uint32_t TsConf::nextIntEvent() {
    tsIntPoll();
    const uint32_t now = CPU::tstates;
    if (tsFrmActive() || s_lin_pending || s_dma_pending) return now;
    uint32_t t = CPU::statesInFrame;
    if ((r.intmask & 0x02) && s_lin_next < t) t = s_lin_next;
    if (s_dma_busy && (r.intmask & 0x04) && s_dma_end < t) t = s_dma_end;
    if (frameIntEnabled() && !s_frm_acked) {
        // First T-state whose latetiming-shifted value enters [IntStart, IntEnd).
        int32_t ws = (int32_t)CPU::IntStart;
        if (ws > (int32_t)now && (uint32_t)ws < t) t = (uint32_t)ws;
    }
    return t < now ? now : t;
}

// EI / RETN / RETI re-enabled interrupts inside an unchecked slice: if a source
// is already up, end the slice so CPU::loop can take the interrupt at the
// right instruction (Stage D runs unchecked between INT events).
TS_HOT void TsConf::intEnableHook() {
#if PERF_TRACE
    if (s_isr_armed) {   // the first EI after an accept is the handler's own (82AC)
        s_isr_armed = false;
        uint32_t d = CPU::tstates - s_isr_t0;
        if ((int32_t)d >= 0 && d < 4000) {
            if (d < ts_isr_min) ts_isr_min = d;
            if (d > ts_isr_max) ts_isr_max = d;
            ts_isr_sum += d; ts_isr_cnt++;
        }
    }
#endif
    // Two reasons to end the slice: a source is already up (take it at the right
    // instruction), or an INT event still lies AHEAD in this frame — the slice was
    // sized with IFF1 clear, i.e. to the frame end, and would run straight through
    // the window. Ninja Gaiden: the line-94 handler moves VSINT back to 272 and
    // re-enables with EI; without this the 272 window was never sampled (hw
    // 2026-09-07, frmInt=60 where the raster split needs 120).
    if (intLine() || nextIntEvent() < CPU::statesInFrame) tsWakeLoop();
}

TS_HOT bool TsConf::needsCheckedFrame() {
    return (r.intmask & 0x02) || s_lin_pending ||
           ((r.intmask & 0x04) && (s_dma_busy || s_dma_pending));
}

TS_HOT void TsConf::endFrame() {
#if TS_VIDEO_TRACE
    if (frameIntEnabled() && !s_frm_acked) {
        static uint32_t miss_budget = 200;
        if (miss_budget) { miss_budget--;
            TSVT("FRAME INT NOT TAKEN: IntStart=%u (L%u) iff1=%d im=%d halted=%d intmask=%02X", (unsigned)CPU::IntStart,
                 (unsigned)(CPU::IntStart / tsLineT()), (int)Z80::isIFF1(), (int)Z80::getIM(), (int)Z80::isHalted(), r.intmask); }
    }
    s_vtrace_frame++;
#endif
#if PERF_TRACE
    if (frameIntEnabled() && !s_frm_acked && CPU::IntEnd) {
        ts_int_miss++;
        // Split the miss by WHY, which is the whole question: with IFF1 clear
        // the guest had interrupts disabled and real hardware would have lost
        // the pulse too (its 32 clocks simply expire); with IFF1 SET we failed
        // to deliver a window the guest was waiting for, and that is ours.
        if (Z80::isIFF1()) ts_miss_ei++; else ts_miss_di++;
        if (Z80::isHalted()) ts_miss_halt++;
    }
#endif
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

// Bulk DMA through the core1 render queue: hw-REFUTED 2026-09-07 (TMNT ship
// scene: ~170 sprite blits per frame, each followed by a DMAStatus poll — every
// poll waited for the whole line backlog ahead of its blit, waitDma=24 ms/frame,
// cpu 18.5 → 36 ms). The queue path (VIDEO::tsPostDma / TsConf::dmaExecBulk on
// core1) is kept for a workload that does real work between DMACtrl and the
// DMA_ACT drop; it is off here.
static const bool kTsDmaOnCore1 = false;

#if PERF_TRACE
volatile uint32_t ts_dma_us = 0;      // wall time inside dmaStart per frame (PERF line)
volatile uint32_t ts_dma_words = 0;   // words moved per frame
volatile uint32_t ts_dma_words_ram = 0, ts_dma_words_blt = 0, ts_dma_words_fill = 0;   // by bulk mode
#if PERF_HIST
uint32_t ts_dma_src_hist[256], ts_dma_dst_hist[256];   // words per physical page
#endif
#endif

TS_HOT void TsConf::dmaStart(uint8_t ctrl) {
#if PERF_TRACE
    const uint64_t dma_t0 = time_us_64();
    extern volatile uint32_t ts_c1_wait_us;
    const uint32_t dma_w0 = ts_c1_wait_us;    // the overlap drain inside is reported as `wait`, not `dma`
#endif
    const bool     rw    = ctrl & 0x80;
    const uint8_t  dev   = ctrl & 0x07;
    const bool     asz   = ctrl & 0x08;
    const bool     dalgn = ctrl & 0x10;
    const bool     salgn = ctrl & 0x20;
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
    const bool bulk = (mode == M_RAM || mode == M_BLT1 || mode == M_BLT2 || mode == M_FILL);
    if (bulk) {
        // Memory movement in dmaExecBulk (on core1 behind the queued lines when
        // the TS-Conf render queue is on — a line then reads the memory the beam
        // would have seen — else right here). The register file and DMA_ACT are
        // derived arithmetically from what the run-by-run loop would leave:
        // with S/D_ALGN the register steps by the window per block, otherwise it
        // follows the running address; FILL reads ONE source word (ss + 2).
        const uint32_t blocks = num + 1;
        words = len * blocks;
        cost = (mode == M_FILL) ? kDmaCostOne : kDmaCostRam;
        if (mode == M_FILL) r.saddr = salgn ? ((r.saddr + asize * blocks) & 0x3FFFFF) : ((ss + 2) & 0x3FFFFF);
        else                r.saddr = salgn ? ((r.saddr + asize * blocks) & 0x3FFFFF) : ((ss + 2 * len * blocks) & 0x3FFFFF);
        r.daddr = dalgn ? ((r.daddr + asize * blocks) & 0x3FFFFF) : ((dd + 2 * len * blocks) & 0x3FFFFF);
        if (kTsDmaOnCore1 && VIDEO::tsRenderQueueOn()) {
            VIDEO::tsPostDma(ctrl, r.dmalen, r.dmanum, ss, dd);
        } else {
            // Synchronous: a queued line that reads the destination must render
            // first (conservative footprint: blocks x max(len, alignment window)).
            const uint32_t span = blocks * (dalgn ? (2 * len > asize ? 2 * len : asize) : 2 * len);
            VIDEO::tsRenderDrainOverlap(dd, span);
            dmaExecBulk(ctrl, ss, dd, r.dmalen, r.dmanum);
        }
#if PERF_TRACE
        if (mode == M_RAM) ts_dma_words_ram += words; else if (mode == M_FILL) ts_dma_words_fill += words; else ts_dma_words_blt += words;
#endif
    } else {
    // Synchronous modes read RAM (CRAM/SFILE/RAMSPI source) or write it
    // (SPIRAM): a queued bulk DMA must land first, and a SPIRAM write must not
    // overtake a queued line that reads its destination.
    VIDEO::tsRenderDrainDma();
    if (mode == M_SPIRAM && VIDEO::tsRenderOverlaps(dd, 2 * len * (num + 1) + asize * (num + 1))) VIDEO::tsRenderDrain();
    if (mode == M_CRAM || mode == M_SFILE) {
        cost = kDmaCostOne;
    } else if (mode == M_SPIRAM || mode == M_RAMSPI) {
        cost = kDmaCostSpi;
        LED::touchR(LED::ZCTRL);
    }

    for (;;) {
        for (uint32_t rem = len; rem; ) {
            const uint32_t n = 1;
            switch (mode) {
                case M_CRAM: {
                    const uint8_t idx = (uint8_t)(dd >> 1);
                    cram[idx] = src.rd(ss);
                    VIDEO::tsCramDirty = true;
                    break;
                }
                case M_SFILE:
                    sfile[(uint8_t)(dd >> 1)] = src.rd(ss);
                    sfileGen++;
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
            if (mode != M_SPIRAM) ss_inc();
            if (mode != M_RAMSPI) dd_inc();
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
    }   // !bulk

    // DMA_ACT for the hardware's duration; the DMA interrupt is raised when it
    // drops (tsIntPoll). A transaction started while a previous one is still
    // "busy" simply supersedes it — the data is long written either way.
#if TS_VIDEO_TRACE
    // Tile-animation element copies (RAM->RAM, both aligns, 2 words x 8 blocks)
    // come ~50 a frame and drowned the UART — whole lines were dropped, the
    // register trace with them (hw 2026-09-07). Skip those; cap the rest per frame.
    static uint32_t dma_trace_frame = 0xFFFFFFFFu, dma_trace_n = 0;
    if (dma_trace_frame != s_vtrace_frame) { dma_trace_frame = s_vtrace_frame; dma_trace_n = 0; }
    if (!(ctrl == 0x31 && r.dmalen == 1 && r.dmanum == 7) && mode != M_CRAM && mode != M_SFILE && dma_trace_n < 6) {
        dma_trace_n++;
        // collapse identical repeats
        static uint32_t last_key[3] = {0xFFFFFFFFu, 0, 0}, rep = 0;
        const uint32_t key[3] = { ((uint32_t)ctrl << 24) | ((uint32_t)r.dmalen << 8) | r.dmanum, r.saddr, r.daddr };
        if (key[0] == last_key[0] && key[1] == last_key[1] && key[2] == last_key[2]) rep++;
        else {
            if (rep) TSVT("  (previous DMA x%u)", (unsigned)(rep + 1));
            TSVT("DMA ctrl=%02X s=%06X d=%06X len=%u num=%u (end regs)", ctrl, (unsigned)r.saddr, (unsigned)r.daddr, (unsigned)r.dmalen, (unsigned)r.dmanum);
            last_key[0] = key[0]; last_key[1] = key[1]; last_key[2] = key[2]; rep = 0;
        }
    }
#endif
    s_dma_busy = true;
    s_dma_end = CPU::tstates + ((words * cost) << ESPectrum::multiplicator);
    if (needsCheckedFrame()) tsWakeLoop();
#if PERF_TRACE
    ts_dma_us += (uint32_t)(time_us_64() - dma_t0) - (ts_c1_wait_us - dma_w0);
    ts_dma_words += words;
#endif
}

// The bulk copy itself, run by run with direct pointers (a 320x240 256c screen
// copy is ~38k words per frame; the per-word rd()/wr() path cost 0.6 us a word —
// TMNT: dma=16 ms of a 60 ms frame, hw 2026-09-06). Register file untouched:
// local copies replay dma_next_burst so the running addresses match dmaStart's
// arithmetic. Safe on either core — reads only MemESP::ram descriptors.
TS_HOT void TsConf::dmaExecBulk(uint8_t ctrl, uint32_t saddr, uint32_t daddr, uint8_t dmalen, uint8_t dmanum) {
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
    enum { M_RAM = 0x01, M_FILL = 0x04, M_BLT2 = 0x06, M_BLT1 = 0x09 };

    uint32_t ss = saddr, dd = daddr, sreg = saddr, dreg = daddr;
    uint32_t len = (uint32_t)dmalen + 1;
    uint32_t num = dmanum;
    DmaRam src, dst;
    uint16_t fill = 0;
    if (mode == M_FILL) {           // dma_fill: ONE source word, read up front
        fill = src.rd(ss);
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

    for (;;) {
        for (uint32_t rem = len; rem; ) {
            uint32_t n = run(dd, dalgn, rem);
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
                // Four bytes a step: build a per-pixel "source non-zero" mask
                // without branches and merge — the byte-at-a-time version did a
                // read-modify-write per pixel into PSRAM through the XIP cache
                // (TMNT: ~170 sprite blits a frame). Unaligned 32-bit access is
                // fine on the M33; memcpy keeps it explicit.
                const uint32_t bytes = n * 2;
                uint32_t i = 0;
                // The destination is never READ for a group that is fully opaque
                // (one 32-bit store) or fully transparent (skipped); only a mixed
                // group falls back to per-pixel stores. A first cut read four
                // destination bytes per group for the merge and made the blits
                // SLOWER (dma 6.3 → 8.5 ms, hw 2026-09-07): the byte loop never
                // read dst, and a dst read is a PSRAM line fill through XIP.
                if (asz) {                        // 256c: byte pixels
                    for (; i + 4 <= bytes; i += 4) {
                        uint32_t sv; memcpy(&sv, sp + i, 4);
                        if (!sv) continue;
                        uint32_t t = (sv | (sv >> 4)) & 0x0F0F0F0Fu;
                        t |= (t >> 2) & 0x03030303u;
                        t = (t | (t >> 1)) & 0x01010101u;
                        if (t == 0x01010101u) { memcpy(dp + i, &sv, 4); continue; }
                        for (uint32_t k = i; k < i + 4; k++) if (sp[k]) dp[k] = sp[k];
                    }
                    for (; i < bytes; i++) if (sp[i]) dp[i] = sp[i];
                } else {                          // 16c: nibble pixels
                    for (; i + 4 <= bytes; i += 4) {
                        uint32_t sv; memcpy(&sv, sp + i, 4);
                        if (!sv) continue;
                        uint32_t t = (sv | (sv >> 2)) & 0x33333333u;
                        t = (t | (t >> 1)) & 0x11111111u;
                        if (t == 0x11111111u) { memcpy(dp + i, &sv, 4); continue; }
                        for (uint32_t k = i; k < i + 4; k++) {
                            const uint8_t s8 = sp[k]; uint8_t dv = dp[k];
                            if (s8 & 0xF0) dv = (dv & 0x0F) | (s8 & 0xF0);
                            if (s8 & 0x0F) dv = (dv & 0xF0) | (s8 & 0x0F);
                            dp[k] = dv;
                        }
                    }
                    for (; i < bytes; i++) {
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
#if PERF_TRACE && PERF_HIST
            if (mode != M_FILL) ts_dma_src_hist[(ss >> 14) & 0xFF] += n;
            ts_dma_dst_hist[(dd >> 14) & 0xFF] += n;
#endif
            if (mode != M_FILL) ss_add(n);
            dd_add(n);
            rem -= n;
        }
        // dma_next_burst
        if (salgn) { sreg = (sreg + asize) & 0x3FFFFF; ss = sreg; }
        if (dalgn) { dreg = (dreg + asize) & 0x3FFFFF; dd = dreg; }
        if (num) { num--; len = (uint32_t)dmalen + 1; }
        else break;
    }
}

TS_HOT void TsConf::dmaLineTick() {
    if (s_dma_busy && CPU::tstates >= s_dma_end) tsIntPoll();
}

// DMAStatus, with the busy-poll fast-forward. The data is long written (the
// copy ran inside the DMACtrl write), only DMA_ACT's guest-time window remains,
// and software waits it out in a tight `IN / BIT 7 / JR NZ` loop — TMNT's ship
// scene: ~170 blits a frame, 18k words x 8 T = ~147k T of polling, i.e. half
// the frame's guest time spent emulating that loop (hw 2026-09-07). When the
// SAME PC reads a busy status twice within a few dozen T-states, guest time is
// advanced straight to the DMA_ACT drop — or to the next interrupt event, so a
// LINE/FRAME/DMA interrupt due inside the window is still taken where it would
// have been — walking the video machine line by line like a HALT does
// (CPU::haltAdvanceTo). A loop that does anything else between polls is not
// tight and never triggers it.
static uint16_t s_poll_pc = 0xFFFF;
static uint32_t s_poll_t  = 0;
#if PERF_TRACE
volatile uint32_t ts_poll_ff = 0, ts_poll_ff_t = 0, ts_poll_reads = 0;   // fast-forwards, T skipped, DMAStatus reads
#endif
TS_HOT uint8_t TsConf::dmaStatus() {
    tsIntPoll();
    if (!s_dma_busy) { s_poll_pc = 0xFFFF; return 0x00; }
    const uint16_t pc = Z80::getRegPC();
    const uint32_t t  = CPU::tstates;
#if PERF_TRACE
    ts_poll_reads++;
#endif
#ifndef TS_POLL_FF
#define TS_POLL_FF 1
#endif
    if (TS_POLL_FF && pc == s_poll_pc && (uint32_t)(t - s_poll_t) < 64u) {
        uint32_t end = s_dma_end;
        if (Z80::isIFF1()) { const uint32_t e = nextIntEvent(); if (e < end) end = e; }
        if (end > CPU::statesInFrame) end = CPU::statesInFrame;
        if (end > t) {
#if PERF_TRACE
            ts_poll_ff++; ts_poll_ff_t += end - t;
#endif
#if TS_VIDEO_TRACE
            // Would this jump carry the guest over the FRAME INT window with
            // interrupts disabled? On hardware that only happens if the DI
            // section really spans the window; here the DMA_ACT duration is a
            // model, so a too-long DMA makes us lose an INT the hardware kept.
            if (!Z80::isIFF1() && frameIntEnabled() && !s_frm_acked &&
                (uint32_t)CPU::IntStart >= t && (uint32_t)CPU::IntStart < end)
                TSVT("POLL-FF over FRAME window with DI: t=%u end=%u IntStart=%u", (unsigned)t, (unsigned)end, (unsigned)CPU::IntStart);
#endif
            CPU::haltAdvanceTo(end);
        }
        tsIntPoll();
    }
    s_poll_pc = pc;
    s_poll_t  = CPU::tstates;
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
        ZxEvoAvr::reset();   // the AVR keyboard controller powers up with the board
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
        sfileGen++;
    }
    s_fm_tmp = 0;
    tsuSeen = 0;
#if TS_VIDEO_TRACE
    s_vtrace_left = 4000;  // re-arm per machine reset (an .spg load resets first); 400 went blind after ~8 s of INT acks
#endif
    s_frm_acked = false;
    s_frm_vsint = s_frm_hsint = 0xFFFF;
    s_lin_pending = s_dma_pending = s_dma_busy = false;
#if PERF_TRACE
    ts_int_frozen = 0; ts_int_ring_w = 0;                      // re-arm the INT-accept trigger per load
    s_int_prev_pc = 0; s_int_prev_t = 0; s_int_run = 0;
#endif
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
