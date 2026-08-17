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
#include "CPU.h"
#include "Z80_JLS/z80.h"
#include "Config.h"
#include "MemESP.h"
#include "ESPectrum.h"
#include "Video.h"
#include "Debug.h"
#include "roms/tsconf/romTsBios.h"

TsConf::Regs TsConf::r;
uint16_t TsConf::cram[256];
uint16_t TsConf::sfile[256];

// Nonzero while TS-Conf runs with FMAddr mapping enabled: (fmaddr & 0x0F) | 0x10.
// Tested predicted-not-taken in the CPU write funnel (gsDmaPoke8) — the
// g_ngs_zxdma pattern; zero for every other machine, so the cost elsewhere is
// one byte-load-and-test per guest write.
uint8_t g_tsconf_fm = 0;

// FMAddr 512-byte windows latch the even byte here and commit the word on the
// odd address (reference temp.fm_tmp).
static uint8_t s_fm_tmp = 0;

static void tsUpdateFmGate() {
    g_tsconf_fm = (Z80Ops::isTsconf && (TsConf::r.fmaddr & 0x10))
                      ? ((TsConf::r.fmaddr & 0x0F) | 0x10) : 0;
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
        case TSR_DMASTATUS: return 0x00; // DMA not implemented yet — never busy
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
            applyZclk();
            break;
        case TSW_CACHECONF:
            r.cacheconf = val & 0x0F;
            break;
        case TSW_FDDVIRT:
            r.fddvirt = val & 0x8F;  // stored; VDOS is a later phase
            break;
        case TSW_INTMASK:
            r.intmask = val & 0x07;  // b1/b2 (LINE/DMA) stored, never fire yet
            frameIntRecalc();
            break;
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
            tsUpdateFmGate();
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
        case TSW_GYOFFSL: r.g_yoffs = (r.g_yoffs & 0x100) | val; break;
        case TSW_GYOFFSH: r.g_yoffs = (r.g_yoffs & 0xFF) | ((uint16_t)(val & 1) << 8); break;
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
        case TSW_DMACTR: {
            r.dmactrl = val;
            static bool warned = false;
            if (!warned) {
                warned = true;
                Debug::log("TsConf: DMA start (ctrl=%02X) — not implemented yet", val);
            }
            break;
        }
        default:
            break;
    }
}

// -------------------------------------------------- FMAddr memory window ----

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

uint8_t TsConf::im2Vector() {
    // FRAME = #FF; LINE (#FD) and DMA (#FB) come with their sources in a
    // later phase.
    return 0xFF;
}

// ------------------------------------------------------------ CPU clock ----

void TsConf::applyZclk() {
    // ZCLK 00/01/10 = 3.5/7/14 MHz -> multiplicator 0/1/2 (11 is reserved —
    // treated as 14). The user's turbo pick is a FLOOR, never a cap: the
    // guest's clock choice is deliberate (TS-BIOS writes it at boot), unlike
    // Pentagon-1024 #EFF7 D4 which only ever pulls the clock down.
    uint8_t zclk = r.sysconf & 0x03;
    if (zclk == 3) zclk = 2;
    if (zclk > Config::tsconf_clk_cap) zclk = Config::tsconf_clk_cap;
    uint8_t want = (zclk > ESPectrum::multUser) ? zclk : ESPectrum::multUser;
    if (want != ESPectrum::multiplicator) {
        ESPectrum::multiplicator = want;
        CPU::updateStatesInFrame();  // calls frameIntRecalc() for TS-Conf
        static bool warned14 = false;
        if (zclk == 2 && !warned14) {
            warned14 = true;
            Debug::log("TsConf: guest selected 14 MHz — may overrun the frame budget");
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
    tsUpdateFmGate();
    setBanks();
    applyZclk();
    frameIntRecalc();
    VIDEO::tsCramDirty = true;
}
