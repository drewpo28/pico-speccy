// eLeMeNt ZX machine module: see Element.h. Flash, reached through one test in the
// RAM port path.
#include "speccy/machines/Element/Element.h"
#include <string.h>
#include <stdlib.h>

#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "app/Buffer.h"
#include "speccy/core/MemESP.h"
#include "speccy/machines/Element/ElmLcu.h"
#include "speccy/machines/Element/ElmHgfx.h"
#include "speccy/machines/Element/ElmSdma.h"
#include "speccy/machines/Element/ElmSid.h"
#include "speccy/machines/Element/ElmFlash.h"
#include "speccy/devices/storage/DivMMC.h"
#include "speccy/machines/TsConf/ZxEvoAvr.h"
#include "speccy/devices/sound/SAASound.h"
#include "ui/LEDIndicators.h"
#include "ui/OSDMain.h"
#include "speccy/devices/storage/RTC.h"
#include "speccy/z80/MemGates.h"
#include "app/Config.h"
#include "app/ESPectrum.h"
#include "speccy/core/Ports.h"
#include "speccy/devices/zifi/ZiFi.h"
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"

uint8_t g_elm_nodiv;                  // DivMMC.h: no automap entry in Zone 0 URAM
const uint8_t* g_elm_divrom;          // DivMMC.cpp: esxDOS from block 4 of the ROM Area (e_zxi_02D bit 0)

namespace Elm {

// Core stamp 2026-10-12, the revision the Reference describes. Binary fields, the
// year as two digits: NETHER.COD builds year<<9 | month<<5 | day from them and wants
// at least #32CB = 25/06/11.
static const uint8_t kStampYear = 26, kStampMonth = 10, kStampDay = 12;
static const uint8_t kInterfaceId = 0xE1;     // "hE1 for eLeMeNt ZX, board v1.x"

static uint8_t s_gIdx, s_eIdx, s_uIdx;
static uint8_t s_corePos;
// 1 KB, on the heap: only this machine pays for them.
// hrxc: the HRXC colour table as this frame's picture uses it (the Reference: the
// palette is taken at the start of the frame, a change during the display waits).
struct Regs { uint8_t g[256], e[256], u[256], win[256], hrxc[Hgfx::HRXC_BYTES]; };
static Regs* s_r;

// URAM. Four Zones of two 8K Areas, each with its own 16-bit page; #73 picks the Zone
// the other ports act on. Backed by the 128-page strip: Protect mode (g_zxi_010 = 2)
// keeps the ZX pages of the Pentagon 1024 out, Full (1) addresses the whole 2 MB.
enum { GM_ELM = 0x20 };               // g_atm_ro bit: Elm::memWrite wants the store
enum { LAYOUT_OFF = 0, LAYOUT_RAM = 2, LAYOUT_FLASH = 6, LAYOUT_HGFX = 8, LAYOUT_HGFX1 = 9, LAYOUT_LCU0 = 0xA, LAYOUT_LCU1 = 0xC, LAYOUT_FRAM = 0xE };
enum { PROTECT_BASE = 128, FULL_PAGES = 256 };
// Protect mode leaves URAM what the ZX memory of the personality (e_zxi_000) does not
// take: 128 pages up to 512 KB, 64 on the Pentagon 1024 (the Reference's table).
// HGFNICCC sets Pentagon 128 and streams 120 pages; with 64 its stream wrapped onto
// itself and the player waited for frame #820D. The ZX side stays a Pentagon 1024
// here, so the upper 64 pages sit in ram[96..127], which only divRAM would take.
static uint32_t protectPages() { return s_r->e[0x000] == 7 || s_r->e[0x000] == 8 ? 64 : 128; }
// g_zxi_010, or the obsolete e_zxi_001 = #FF ("former Ultimate mode"): FLASH6 of 2024 and the
// SetUp open their layout-6 / E windows with only that, so it counts as Full.
static uint8_t uramMode() { return s_r->g[0x010] ? s_r->g[0x010] : (s_r->e[0x001] == 0xFF ? 1 : 0); }
struct Zone { uint16_t page[2]; uint8_t par; };
static Zone s_z[4];
static uint8_t s_zone;
static uint8_t s_active;              // Zones currently over the legacy mapping
static uint8_t s_wpSlots, s_lcuSlots, s_hgfxSlots, s_flashSlots, s_framSlots; // 8K slot masks
static bool s_flashOpen;              // zone 0 shows the flash (layout 6)
// The SetUp: a long reset on the board maps block 7 of the ROM Area at #0000; its stub
// copies the code to #C000 and the first OUT of the relocated code (#7FFD, then
// e_zxi_030) is when the ROM is wanted no more - the block stays until the first port
// write after the reset, which the stub never makes.
static bool s_setupBoot, s_setupRom, s_setupOnce;
static bool s_powered;                // the first reset of the session is the power-up
static uint8_t s_keep[0x40];          // e_zxi_000..03F across a warm reset
static uint32_t s_onFrames;
// The FRAM's system counters (#3FEC PowerUp, #3FF0 Extra, #3FF4 Reset, #3FF8 Nmi,
// #3FFC OnTime, 32-bit LE): the SetUp's first screen and its "User level".
static void framCount(uint16_t off) {
    uint8_t* f = ElmFlash::fram();
    if (!f) return;
    uint32_t v = (uint32_t)f[off] | (uint32_t)f[off + 1] << 8 | (uint32_t)f[off + 2] << 16 | (uint32_t)f[off + 3] << 24;
    if (v == 0xFFFFFFFFu) return;
    v++;
    f[off] = (uint8_t)v; f[off + 1] = (uint8_t)(v >> 8); f[off + 2] = (uint8_t)(v >> 16); f[off + 3] = (uint8_t)(v >> 24);
    ElmFlash::framDirty();
}
static uint16_t s_slotPage[8];        // HGFX Area: its 8K step in the Zone
uint8_t* wrKind;
// What a store to a 256-byte page of the Z80 space means. Regions overlap in RAM
// terms: the store also lands in the memory under a device window.
enum { W_NONE, W_VRAM, W_REG, W_COL, W_PROTECT, W_LCU, W_CHUNKY, W_FDMA, W_LCU2, W_SDMA, W_HRXC, W_FLASH, W_FRAM };
static uint8_t* s_lcu;                // 8K, the registers at its start (reads are plain)

static void readHookUpdate();
static void windowsRebuild() {
    uint8_t* w = s_r->win;
    memset(w, 0, 256);
    // HGFX modes only: under HRC / HRXC #4000-#7BFF is their screen, plain RAM, and the
    // HGFX register page #5B sits in its pixels.
    if (Hgfx::mode >= 3 && !(Hgfx::g20 & 0x80)) {
        const uint8_t v = s_r->g[0x021];
        for (int i = 0; i < Hgfx::vramPages(); i++) w[(uint8_t)(v + i)] = W_VRAM;
        const uint8_t c = s_r->g[0x023];
        for (int i = 0; i < 3; i++) w[(uint8_t)(c + i)] = W_COL;
        w[s_r->g[0x022]] = W_REG;
    }
    // HRXC: its colour table sits at #7C00 of the shown screen, plain RAM; a write
    // before the picture starts still reaches this frame's palette.
    if (Hgfx::mode == 2) for (const uint8_t h : { 0x7C, 0xFC }) if (!w[h]) w[h] = W_HRXC;
    if (s_r->g[0x040] & 1) w[s_r->g[0x041]] = W_FDMA;
    if ((s_r->g[0x060] & 0x81) == 1) w[s_r->g[0x061]] = W_LCU2;
    if ((s_r->g[0x048] & 0x81) == 1) w[s_r->g[0x049]] = W_SDMA;
    for (int s = 0; s < 8; s++) {
        const uint8_t k = (s_lcuSlots >> s) & 1 ? W_LCU : (s_hgfxSlots >> s) & 1 ? W_CHUNKY
                        : (s_wpSlots >> s) & 1 ? W_PROTECT
                        : (s_flashSlots >> s) & 1 ? W_FLASH : (s_framSlots >> s) & 1 ? W_FRAM : W_NONE;
        if (k) memset(w + s * 32, k, 32);
    }
    bool any = false;
    for (int i = 0; i < 256 && !any; i++) any = w[i] != 0;
    if (any) g_atm_ro |= GM_ELM; else g_atm_ro &= (uint8_t)~GM_ELM;
    readHookUpdate();
}

// The HGFX COPY read latch: on while the VRAM window is mapped and the mode is COPY,
// tested once per generic guest read (CPU.cpp gsDmaPeek8). A window at #4000 or
// above also needs the fast read path off (Video.cpp tsFastMemRecalc), a window
// below it (HE2025: #2300) costs nothing there.
volatile uint8_t g_elm_vram_rd;
static uint16_t s_rdOff;
static uint8_t s_rdVal;              // the byte that read returned
static bool s_rdArmed, s_rdFast = true;
static void readHookUpdate() {
    const bool on = s_r && Hgfx::mode >= 3 && !(Hgfx::g20 & 0x80) && Hgfx::reg(Hgfx::REG_MODE) == 1;
    const bool fast = !on;   // COPY: every read of a window at #4000+ must reach memRead
    const bool was = g_elm_vram_rd != 0, wasFast = s_rdFast;
    g_elm_vram_rd = on;
    s_rdFast = fast;
    if (!on) s_rdArmed = false;
    if (was != on || wasFast != fast) VIDEO::tsFastMemRecalc();
}
bool fastReadOk() { return s_rdFast; }
void memRead(uint16_t a) {
    if (s_r->win[a >> 8] != W_VRAM) return;
    s_rdOff = (uint16_t)(a - ((uint16_t)s_r->g[0x021] << 8));
    s_rdVal = MemESP::readbyte(a);
    s_rdArmed = true;
}

// FastDMA: Programmer's Reference + FDMA-EN.TXT. Two register sets of 32 bytes. A
// transfer stops the CPU and runs at memory speed, independent of the CPU clock:
// CHUNKY.TXT gives about 0.48 T a byte at 3.5 MHz, taken as 0.5.
static uint8_t s_fd[64];
static uint32_t s_fdSrc[2], s_fdDst[2];   // running addresses, 8.8 in the floating modes
static bool s_fdBusy;
static uint8_t s_radHi;
static uint8_t s_scan;
static uint8_t s_lifo[32], s_lifoTop;     // #2B, 32 bytes, circular
static uint64_t s_clkLatch;               // e_zxi_0F3..0F8, latched by reading 0F3
static void cpuSpeed(uint8_t sel);
static uint8_t s_dac[5], s_dacUsed;
static bool s_scanHeld;
static bool s_radFlip;

static inline uint16_t fd16(const uint8_t* r, int o) { return (uint16_t)(r[o] | r[o + 1] << 8); }
static inline uint32_t fd24(const uint8_t* r, int o) { return r[o] | (uint32_t)r[o + 1] << 8 | (uint32_t)r[o + 2] << 16; }

static uint8_t* uramPtr(uint16_t page);
static int audioPos() { return (int)((CPU::tstates / ESPectrum::audioCOVOXDivider) >> ESPectrum::multiplicator); }
static void sdCatch() { ElmSdma::renderTo(audioPos()); }
static uint8_t s_sidIdx;
// ULA+ register port as the Extra group sees it: the generic ULA+ keeps its own copy
// only while Config::ulaplus is on.
static uint8_t s_ulapReg;
static inline uint8_t* uramByte(uint32_t a) {
    uint8_t* p = uramPtr((uint16_t)(a >> 13));
    return p ? p + (a & 0x1FFF) : nullptr;
}
static uint8_t fdRead(uint32_t a, bool uram) {
    if (uram) { const uint8_t* p = uramByte(a); return p ? *p : 0xFF; }
    return busRead((uint16_t)a);
}
// A DMA / FastDMA read: the wiki says both scroll HGFX in COPY mode, so their reads of
// the window arm the latch exactly like the CPU's.
uint8_t busRead(uint16_t a) {
    if (g_elm_vram_rd) memRead(a);
    return MemESP::readbyte(a);
}
void busWrite(uint16_t a, uint8_t v) {
    if (s_r && s_r->win[a >> 8] && memWrite(a, v)) return;
    MemESP::writebyte(a, v);
}
static void fdWrite(uint32_t a, bool uram, uint8_t v) {
    if (uram) { uint8_t* p = uramByte(a); if (p) *p = v; return; }
    busWrite((uint16_t)a, v);
}

static void fdmaRun(int set, bool wide24) {
    if (s_fdBusy) return;
    s_fdBusy = true;
    const uint8_t* r = s_fd + set * 0x20;
    const uint8_t par = r[0];
    const uint8_t par2 = wide24 ? r[0x10] : 0;
    const bool fill = par & 0x20, down = par & 4;
    const bool sfl = par & 8, dfl = par & 0x10;
    const bool suram = par2 & 2, duram = par2 & 4;
    uint32_t len = wide24 ? fd24(r, 0x17) : fd16(r, 1);
    if (!len) len = wide24 ? 0x400000u : 0x10000u;
    const uint32_t sbatch = fd16(r, 5) ? fd16(r, 5) : 0x10000u, dbatch = fd16(r, 0x0B) ? fd16(r, 0x0B) : 0x10000u;
    const uint32_t sskip = fd16(r, 7), dskip = fd16(r, 0x0D);
    uint32_t s = (par & 1) ? s_fdSrc[set] : (wide24 ? fd24(r, 0x11) : fd16(r, 3)) << (sfl ? 8 : 0);
    uint32_t d = (par & 2) ? s_fdDst[set] : (wide24 ? fd24(r, 0x14) : fd16(r, 9)) << (dfl ? 8 : 0);
    const uint8_t fv = wide24 ? r[0x11] : r[3];
    const uint32_t amask = wide24 ? 0xFFFFFFu : 0xFFFFu;
#if HOST_BUILD
    if (getenv("PICOSPECCY_ELMLOG"))
        Debug::log("[FDMA] par=%02X/%02X len=%X s=%X d=%X sb=%X ss=%X db=%X ds=%X fv=%02X hreg vp=%02X oy=%d", par, par2, len, s, d, sbatch, sskip, dbatch, dskip, fv, Hgfx::reg(0), Hgfx::reg(7) | Hgfx::reg(8) << 8);
#endif
    uint32_t sn = 0, dn = 0;
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t b = fill ? fv : fdRead((sfl ? s >> 8 : s) & amask, suram);
        fdWrite((dfl ? d >> 8 : d) & amask, duram, b);
        if (sfl) s = down ? s - sskip : s + sskip;
        else {
            s = down ? s - 1 : s + 1;
            if (++sn == sbatch) { sn = 0; s = down ? s - sskip : s + sskip; }
        }
        if (dfl) d = down ? d - dskip : d + dskip;
        else {
            d = down ? d - 1 : d + 1;
            if (++dn == dbatch) { dn = 0; d = down ? d - dskip : d + dskip; }
        }
    }
    s_fdSrc[set] = s; s_fdDst[set] = d;
    s_fdBusy = false;
    // The CPU is held for the transfer; the machine's time moves on in pieces, which
    // is what the raster machines want. An INT inside it waits (SaveINT).
    uint32_t t = (len >> 1) << ESPectrum::multiplicator;
    while (t) { const uint32_t n = t > 200 ? 200 : t; VIDEO::Draw(n, false); t -= n; }
}

static void fdmaWrite(uint8_t off, uint8_t v) {
    if (off >= 0x40) return;
    if (off >= 0x20 && !(s_r->g[0x040] & 2)) return;
    s_fd[off] = v;
    const int set = off >> 5;
    const uint8_t o = off & 0x1F;
    if (o == 3 || o == 4) s_fdSrc[set] = fd16(s_fd + set * 0x20, 3);
    if (o == 9 || o == 10) s_fdDst[set] = fd16(s_fd + set * 0x20, 9);
    // h00 also starts a transfer while h10 bit 0 holds the 24-bit mode: TETRIS (Zirin
    // 2026) writes h10 = 3 once, then starts each 32-row band of its photo by h00.
    if (o == 0) fdmaRun(set, s_fd[set * 0x20 + 0x10] & 1);
    else if (o == 0x10 && (v & 1)) fdmaRun(set, true);
}

// LCU2: the block over RAM at g_zxi_061. Reads are plain, so the results go to memory.
static uint8_t s_lcu2[ElmLcu::SIZE];
static void lcu2Publish() {
    const uint16_t base = (uint16_t)s_r->g[0x061] << 8;
    for (int i = 0; i < ElmLcu::SIZE; i++) MemESP::writebyte((uint16_t)(base + i), s_lcu2[i]);
}

static uint8_t* uramPtr(uint16_t page) {
    const uint8_t mode = uramMode();
    const uint32_t idx = mode == 1 ? (page & (FULL_PAGES - 1))
                                   : PROTECT_BASE + (page & (protectPages() - 1));
    uint8_t* p = MemESP::ram[idx >> 1].direct();
    return p ? p + (idx & 1) * 0x2000 : nullptr;
}

// SoundDMA reads protected URAM whatever the mode (the Reference: "works with protected
// URAM only"); the address counts from its first page.
static const uint8_t* sdPage(uint32_t page) {
    const uint32_t idx = PROTECT_BASE + (page & (protectPages() - 1));
    const uint8_t* p = MemESP::ram[idx >> 1].direct();
    return p ? p + (idx & 1) * 0x2000 : nullptr;
}

static uint16_t freePages() {
    const uint8_t mode = uramMode();
    return mode == 1 ? FULL_PAGES : mode == 2 ? protectPages() : 0;
}

static uint8_t* lcuBlock() {
    if (!s_lcu) {
        s_lcu = (uint8_t*)Buffer::palloc(0x2000, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (s_lcu) { memset(s_lcu, 0, 0x2000); ElmLcu::reset(s_lcu); }
    }
    return s_lcu;
}

static void legacyZone(uint8_t z) {
    switch (z) {
    case 0: MemESP::p3special = 0; MemESP::recoverPage0(); break;
    case 1: MemESP::plugPtr16(1, MemESP::ram[5].direct()); break;
    case 2: MemESP::plugPtr16(2, MemESP::ram[2].sync(2)); break;
    case 3: MemESP::plugPtr16(3, MemESP::ram[MemESP::bankLatch].sync(3)); break;
    }
}

// The one writer of a URAM slot. The reserved layouts leave the Zone on the legacy
// mapping. Layout 6 (zone 0) shows the FlashRAM of the ROM Area in 8 KB pages, its
// writes are the JEDEC commands (ElmFlash); layout E shows the FRAM in Area1.
void remap() {
    if (!s_r) return;
    const bool on = (s_r->g[0x000] & 1) && uramMode();
    uint8_t active = 0, wp = 0, lcu = 0, hg = 0, fl = 0, fr = 0;
    for (uint8_t z = 0; z < 4; z++) {
        const uint8_t layout = on ? s_z[z].par >> 4 : LAYOUT_OFF;
        const bool mapped = layout == LAYOUT_RAM || layout == LAYOUT_HGFX || layout == LAYOUT_HGFX1 ||
                            (z == 0 && (layout == LAYOUT_LCU0 || layout == LAYOUT_LCU1 ||
                                        layout == LAYOUT_FLASH || layout == LAYOUT_FRAM));
        if (!mapped) {
            if (s_active & (1u << z)) legacyZone(z);
            continue;
        }
        active |= 1u << z;
        if (z == 0) MemESP::p3special = 4;   // recoverPage0() keeps out of Zone 0
        for (uint8_t a = 0; a < 2; a++) {
            const uint8_t slot = z * 2 + a;
            const bool isLcu = (layout == LAYOUT_LCU0 && a == 0) || (layout == LAYOUT_LCU1 && a == 1);
            const bool isHg = layout == LAYOUT_HGFX || (layout == LAYOUT_HGFX1 && a == 1);
            const bool isFl = layout == LAYOUT_FLASH, isFr = layout == LAYOUT_FRAM && a == 1;
            // No page: the window starts at OffsetY. Layout 8 shows the buffer from there
            // over both Areas; layout 9 has one HGFX Area, and it starts at OffsetY too
            // (TETRIS writes each 32-row band of its photo to #2000 with OffsetY = row).
            const uint8_t hgStep = layout == LAYOUT_HGFX ? a : 0;
            uint8_t* p = isLcu ? lcuBlock()
                       : isHg && Hgfx::ensure() ? Hgfx::chunkyRead(hgStep * 0x2000u)
                       : isFl ? (ElmFlash::ensure() ? ElmFlash::page(s_r->e[0x00F], s_z[z].page[a]) : nullptr)
                       : isFr ? (ElmFlash::ensure() ? ElmFlash::fram() : nullptr)
                       : uramPtr(s_z[z].page[a]);
            if (isHg) { hg |= 1u << slot; s_slotPage[slot] = hgStep; }
            if (!p) { wp |= 1u << slot; p = (uint8_t*)MemESP::ram[0].direct(); }
            MemESP::ramCurrent[slot] = p;
            mem_desc_t::bank_dirty[slot] = &mem_desc_t::dirty_sink;
            if (isLcu) lcu |= 1u << slot;
            else if (!(s_z[z].par & (1u << a))) wp |= 1u << slot;
            else if (isFl) fl |= 1u << slot;
            else if (isFr) fr |= 1u << slot;
        }
    }
    const bool flashOpen = (active & 1) && (s_z[0].par >> 4) == LAYOUT_FLASH;
    if (s_flashOpen && !flashOpen) ElmFlash::windowClosed();   // FLASH6 is done: to the file now
    s_flashOpen = flashOpen;
    s_active = active;
    s_wpSlots = wp;
    s_lcuSlots = lcu;
    s_hgfxSlots = hg;
    s_flashSlots = fl;
    s_framSlots = fr;
    Hgfx::direct(hg != 0);
    // e_zxi_02D bit 7 clear: URAM in Zone 0 is on top, the divSD entry points do not trap
    g_elm_nodiv = (active & 1) && !(s_r->e[0x02D] & 0x80);
    windowsRebuild();
}

static void hrxcLatch() {
    if (Hgfx::mode == 2) memcpy(s_r->hrxc, VIDEO::grmem + Hgfx::HRXC_TABLE, Hgfx::HRXC_BYTES);
}

// KeyLayer: the screen #7FFD bit 3 shows is on top, the other shines through its key
// colour. g_zxi_002 bit 0 puts the first screen in page 6 instead of 5.
void frameEnd() {
    const uint8_t* a = MemESP::ram[(s_r->g[0x002] & 1) ? 6 : 5].direct();
    const uint8_t* b = MemESP::ram[7].direct();
    if (s_r->g[0x002] & 1) VIDEO::grmem = MemESP::videoLatch ? (uint8_t*)b : (uint8_t*)a;
    const bool key = s_r->g[0x003] == 1;
    Hgfx::keyTop = key ? (MemESP::videoLatch ? b : a) : nullptr;
    Hgfx::keyBottom = key ? (MemESP::videoLatch ? a : b) : nullptr;
    Hgfx::keyColour = s_r->g[0x004];
    Hgfx::radastan = (s_r->u[0x40] & 3) == 3;
    Hgfx::radOffset = (uint16_t)(s_r->u[0x41] | (s_radHi << 8));
    Hgfx::radPad = s_r->u[0x42];
    Hgfx::radBank = s_r->u[0x43] & 3;
    Hgfx::hrxcTable = s_r->hrxc;
    Hgfx::frameEnd();
    hrxcLatch();                          // the next frame starts with the table as it is now
    ElmSdma::frameEnd(ESPectrum::samplesPerFrame);
    ElmSid::frameEnd(ESPectrum::samplesPerFrame);
}

// SafeNMI: the return address of the last NMI is readable, bit 7 locks the button.
// Bit 0 (RAM write protection) is stored: the Reference does not say what it covers.
bool nmiBlocked() { return s_r && (s_r->g[0x080] & 0x80); }
void nmiTaken(uint16_t pc) {
    if (!s_r) return;
    framCount(0x1FF8);
    s_r->g[0x081] = (uint8_t)pc;
    s_r->g[0x082] = (uint8_t)(pc >> 8);
}

bool memWrite(uint16_t a, uint8_t v) {
    // A write elsewhere (a CALL's return address, a PUSH) leaves the latch armed: clearing
    // it there turned HE2025's sprite blits into ATTR dots on hardware (2026-11-02).
    if (a < 0x4000 && MemESP::divmmc_mapped) return false;   // the divMMC window is on top
    switch (s_r->win[a >> 8]) {
    case W_VRAM:
        // Plane mask 0 draws nothing and the store reaches RAM: HE2025 copies its start
        // code to #4000 under the open window that way and jumps to it.
        if (!Hgfx::reg(Hgfx::REG_MASK)) return false;
        // COPY takes its 8 pixels from the position the Z80 READ in the window (plus
        // the source offsets), one read per write (the wiki: the ROM scroll copies a
        // line up with LDIR, HE2025 replicates a pattern with an overlapping LDIR and
        // blits sprites with LD A,(HL) / LD (DE),A a row apart). The read hook
        // (memRead, from the generic read funnel while g_elm_vram_rd) arms the latch;
        // a write with nothing armed draws like ATTR.
        {
            const uint16_t off = (uint16_t)(a - ((uint16_t)s_r->g[0x021] << 8));
            bool wasRead = false;
            uint16_t src = 0xFFFF;
            // The copy needs the written byte to be the one read (HGFX-6EN: "The read value
            // must match the subsequently written value", else the write-only method). The
            // ROM's PRINT reads the screen byte and writes the new character row: HGFXTEXT's
            // INVERSE " COLOR " in random colours came out as a copy of the old grey.
            if (Hgfx::reg(Hgfx::REG_MODE) == 1) { wasRead = s_rdArmed && v == s_rdVal; src = s_rdOff; s_rdArmed = false; }
#if HOST_BUILD
            if (getenv("PICOSPECCY_ELMVLOG")) Debug::log("[VRAM] %04X=%02X g20=%02X m=%u rd=%d src=%04X pc=%04X oxy=%d,%d sxy=%d,%d vp=%02X", off, v, Hgfx::g20, Hgfx::reg(1), (int)wasRead, src, Z80::getRegPC(), (int16_t)(Hgfx::reg(5) | Hgfx::reg(6) << 8), (int16_t)(Hgfx::reg(7) | Hgfx::reg(8) << 8), (int16_t)(Hgfx::reg(9) | Hgfx::reg(10) << 8), (int16_t)(Hgfx::reg(11) | Hgfx::reg(12) << 8), Hgfx::reg(0));
#endif
            Hgfx::writeVram(off, v, wasRead, src);
        }
        // The store lands in RAM too: a read of the window returns what was written.
        // HGFXCUBE clears its buffer with #FF at #E000 and a Z80 DMA copy #E000 ->
        // #E001; swallowed, the copy spread the old 0 and drew nothing (HGFX-6EN: the
        // ROM's scroll reads back the bytes it wrote to the screen).
        return false;
    case W_REG: {
        const uint8_t r = (uint8_t)a;
#if HOST_BUILD
        if (getenv("PICOSPECCY_ELMLOG")) Debug::log("[HREG] %02X=%02X pc=%04X", r, v, Z80::getRegPC());
#endif
        Hgfx::writeReg(r, v);
        if (s_hgfxSlots && (r == Hgfx::REG_VP || r == Hgfx::REG_OY || r == Hgfx::REG_OY + 1)) remap();
        if (r == Hgfx::REG_MODE) readHookUpdate();
        return false;
    }
    case W_COL:
        Hgfx::writeColour((uint16_t)(a - ((uint16_t)s_r->g[0x023] << 8)), v);
        return false;
    case W_PROTECT:
        return true;
    case W_FLASH:                     // a JEDEC command byte; the store never lands as data
        ElmFlash::write((uint32_t)(s_r->e[0x00F] & 3) * ElmFlash::AREA +
                        (uint32_t)(s_z[0].page[(a >> 13) & 1] & 15) * ElmFlash::PAGE + (a & 0x1FFF), v);
        return true;
    case W_FRAM:
        ElmFlash::framDirty();
        return false;                 // the store goes to the FRAM under the window
    case W_LCU: {
        const uint16_t off = a & 0x1FFF;
        if (off < ElmLcu::SIZE) ElmLcu::write(s_lcu, (uint8_t)off, v);
        return true;
    }
    case W_FDMA:
        fdmaWrite((uint8_t)a, v);
        return false;
    case W_SDMA:
#if HOST_BUILD
        if (getenv("PICOSPECCY_ELMLOG")) Debug::log("[SDMA] %02X=%02X pc=%04X", (uint8_t)a, v, Z80::getRegPC());
#endif
        if ((uint8_t)a < ElmSdma::REG_BYTES) { sdCatch(); ElmSdma::write((uint8_t)a, v); }
        return false;
    case W_HRXC: {
        const uint8_t* p = MemESP::ramCurrent[a >> 13] + (a & 0x1FFF);
        const uint8_t* t = VIDEO::grmem + Hgfx::HRXC_TABLE;
        if (p >= t && p < t + Hgfx::HRXC_BYTES && (int)(CPU::tstates >> ESPectrum::multiplicator) < VIDEO::tStatesScreen)
            s_r->hrxc[p - t] = v;
        return false;
    }
    case W_LCU2:
        ElmLcu::write(s_lcu2, (uint8_t)a, v);
        lcu2Publish();
        return true;
    case W_CHUNKY:
        Hgfx::chunkyWrite(s_slotPage[a >> 13] * 0x2000u + (a & 0x1FFF), v);
        return true;
    }
    return false;
}

// The ZX ROMs come from the ROM Area e_zxi_00F selects: ROM0-3 = its blocks 0-3, so
// #7FFD bit 4 picks block 0/1 (FLASH128 lays ROM0/ROM1 in 0/2 and 1/3) and a +2A
// personality adds #1FFD bit 2 as the high bit. Without the image the Pentagon pair
// bound by Config::requestMachine stays.
static void romsBind() {
    if (!ElmFlash::ensure()) return;
    const uint8_t area = s_r->e[0x00F] & 3;
    for (uint8_t b = 0; b < 4; b++) {
        const uint8_t* p = ElmFlash::block(area, s_setupRom ? ElmFlash::BLOCK_SETUP : b);
        MemESP::registerOverlay(p, nullptr);          // a flat page, nothing to materialise
        MemESP::rom[b].assign_rom(p);
    }
    MemESP::recoverPage0();
}

// esxDOS: e_zxi_02D bit 0 = 1 takes the ROM from block 4 of the Area (the first 8 KB,
// FLASHESX writes an 8 KB image there), 0 = the "FPGA rescue" copy, our built-in one.
static void divRomApply() {
    const uint8_t area = s_r->e[0x00F] & 3;
    const bool fromFlash = (s_r->e[0x02D] & 1) && ElmFlash::ensure() && !ElmFlash::blockBlank(area, ElmFlash::BLOCK_ESXDOS);
    g_elm_divrom = fromFlash ? ElmFlash::block(area, ElmFlash::BLOCK_ESXDOS) : nullptr;
    DivMMC::setRom(g_elm_divrom);
}

void flush(bool force) {
    if (++s_onFrames >= 3000) { s_onFrames = 0; framCount(0x1FFC); }   // OnTime, minutes
    ElmFlash::flush(force);
}

// Machine > eLeMeNt ZX > SetUp (the board: reset held 1.5 s). Takes effect at the
// reset that follows; a blank block 7 shows what to flash.
bool enterSetup() {
    if (!ElmFlash::ensure()) return false;
    if (ElmFlash::blockBlank(s_r ? s_r->e[0x00F] & 3 : 0, ElmFlash::BLOCK_SETUP)) return false;
    s_setupBoot = true;
    return true;
}

static uint8_t coreIdNext(uint8_t* pos) {   // "eLeMeNt ZX", a 0, then again
    static const char kId[] = "eLeMeNt ZX";
    const uint8_t c = (uint8_t)kId[*pos];
    *pos = c ? (uint8_t)(*pos + 1) : 0;
    return c;
}

void reset() {
    s_gIdx = s_eIdx = s_uIdx = 0;
    s_corePos = 0;
    if (!s_r) s_r = (Regs*)tryMalloc(sizeof(Regs));
    if (!s_r) { Debug::log("[ELM] no heap for the ZXi registers"); return; }
    if (s_powered) memcpy(s_keep, s_r->e, sizeof s_keep);
    memset(s_r, 0, sizeof(Regs));
    wrKind = s_r->win;
    // Memory-register windows at their documented defaults.
    s_zone = 0;
    memset(s_z, 0, sizeof(s_z));
    s_active = s_wpSlots = s_lcuSlots = s_hgfxSlots = s_flashSlots = s_framSlots = 0;
    if (s_flashOpen) { s_flashOpen = false; ElmFlash::windowClosed(); }
    g_elm_nodiv = 0;        // the reset goes through esxDOS at #0000 again
    Hgfx::reset();
    s_rdArmed = false;
    Hgfx::direct(false);
    g_atm_ro = 0;
    if (s_lcu) ElmLcu::reset(s_lcu);
    ElmLcu::reset(s_lcu2);
    memset(s_fd, 0, sizeof(s_fd));
    if (!ElmSdma::init(sdPage, ESPectrum::Audio_freq)) Debug::log("[ELM] no heap for SoundDMA");
    if (!ElmSid::init(ESPectrum::Audio_freq)) Debug::log("[ELM] no heap for the SID");
    ElmSid::setParams(0);   // e_zxi_019 = 0: 6581, PAL
    ElmSid::mute(false);
    s_sidIdx = 0;
    s_ulapReg = 0;
    s_scan = 0; s_scanHeld = false;
    s_lifoTop = 0;
    memset(s_dac, 0, sizeof(s_dac)); s_dacUsed = 0;
    s_r->g[0x000] = 0x01;   // the internal device, not an MB03+
    s_r->g[0x021] = 0x40;   // HGFX videoram
    s_r->g[0x022] = 0x5B;   // HGFX registers
    s_r->g[0x023] = 0x5C;   // HGFX colour table
    s_r->g[0x041] = 0x5D;   // FastDMA
    s_r->g[0x049] = 0x5E;   // SoundDMA
    s_r->g[0x051] = 0x5F;   // eINT
    s_r->g[0x061] = 0x60;   // LCU2
    // The SetUp's own "Load defaults" table (setup-20260406, D3ED), two of ours on top:
    // e_000 = Pentagon 1024 (the owner's base personality, the table says 128/+2) and
    // e_02D bit 0 = esxDOS from the flash block when one is flashed (the table boots the
    // FPGA copy). LIFO and DMA on: the MB03+SD esxDOS build keeps its divMMC control
    // byte in the LIFO (IN A,(#2B) before OUT (#E3)) and hangs without it.
    static const uint8_t kDefaults[0x40] = {
        0x07, 0x01, 0x3F, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01,
        0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 };
    memcpy(s_r->e, kDefaults, sizeof kDefaults);
    // The FRAM's SetUp area (#2000 + N = e_zxi_N, 64 bytes) is what the board loads at
    // power-up; a blank FRAM (no file yet, or defaults never saved) keeps the table.
    // Power-up (the first reset of the session) loads the FRAM; a reset keeps the
    // registers as the programs left them, like the board (the SetUp exits by RST 0
    // and the Area / machine it saved are seen at the next power-up or F11 here).
    if (!s_powered) {
        if (ElmFlash::ensure()) {
            uint8_t* f = ElmFlash::fram();
            if (f[0x000] <= 9 && f[0x001] != 0xFF) memcpy(s_r->e, f, 0x40);
            else {                    // a fresh FRAM: our table as the saved settings, counters at 0
                memcpy(f, kDefaults, sizeof kDefaults);
                memset(f + 0x1C00, 0, 0x400);
                ElmFlash::framDirty();
            }
            framCount(0x1FEC);        // PowerUp
        }
        s_powered = true;
    } else {
        memcpy(s_r->e, s_keep, sizeof s_keep);
        framCount(0x1FF4);            // Reset
    }
    if (s_setupBoot) { s_setupBoot = false; s_setupRom = true; }
#if HOST_BUILD
    if (getenv("PICOSPECCY_ELM_SETUP") && !s_setupOnce) { s_setupOnce = true; s_setupRom = true; }
#endif
    if (s_setupRom) g_elm_nodiv = 1;   // esxDOS stays off the bus until the SetUp runs from RAM
    cpuSpeed(s_r->e[0x030]);
    ElmSid::mute(!(s_r->e[0x002] & 8));
    s_r->e[0x0FC] = kStampDay;
    s_r->e[0x0FD] = kStampMonth;
    s_r->e[0x0FE] = kStampYear;
    s_r->e[0x0FF] = kInterfaceId;
    romsBind();
    divRomApply();
}

static uint8_t gRead(uint8_t r) {
    switch (r) {
    case 0x01E:          // HGFX/HRC beam line, low / high
    case 0x01F: {
        const uint32_t line = (CPU::tstates >> ESPectrum::multiplicator) / VIDEO::tStatesPerLine;
        return r == 0x01E ? (uint8_t)line : (uint8_t)(line >> 8);
    }
    case 0x02F: return Hgfx::status();
    case 0x032: return ZxEvoAvr::scanPop();
    case 0x04A: sdCatch(); return ElmSdma::posHigh();
    case 0x011: return (uint8_t)freePages();
    case 0x012: return (uint8_t)(freePages() >> 8);
    default:
        return s_r->g[r];
    }
}

// The windows are RAM with a device listening: what was written before the mode was
// turned on is in memory, not in the device.
static void hgfxOpen() {
    uint8_t regs[Hgfx::REG_COUNT];
    const uint16_t rb = (uint16_t)s_r->g[0x022] << 8, cb = (uint16_t)s_r->g[0x023] << 8;
    for (int i = 0; i < Hgfx::REG_COUNT; i++) regs[i] = MemESP::readbyte((uint16_t)(rb + i));
    static uint8_t* cols;
    if (!cols) cols = (uint8_t*)Buffer::palloc(Hgfx::COLOURS, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!cols) return;
    for (int i = 0; i < Hgfx::COLOURS; i++) cols[i] = MemESP::readbyte((uint16_t)(cb + i));
    Hgfx::loadState(regs, cols);
    readHookUpdate();
}

static void gWrite(uint8_t r, uint8_t v) {
    switch (r) {
    case 0x011: case 0x012:           // free URAM pages
    case 0x01E: case 0x01F:           // beam
    case 0x02F:                       // HGFX status
    case 0x081: case 0x082:           // SafeNMI return address
        return;                       // read only
    case 0x000: case 0x010:           // URAM device / mode
        s_r->g[r] = v;
        remap();
        return;
    case 0x001: {                     // extra graphics mode: the windows open
        s_r->g[r] = v;
        const uint8_t was = Hgfx::mode;
        Hgfx::setMode(v);
        if (v >= 3 && was < 3 && Hgfx::ensure()) hgfxOpen();
        hrxcLatch();
        windowsRebuild();
        return;
    }
    case 0x030:                       // write 1: the RX FIFO is erased
        if ((v & 1) && Config::zifi_enabled)
            while (ZiFi::uart16550Read(0xFD) & 1) ZiFi::uart16550Read(0xF8);
        return;
    case 0x032: case 0x04A:
        return;                       // read only
    case 0x048:
        s_r->g[r] = v;
        if (!(v & 1)) { sdCatch(); ElmSdma::reset(); }
        windowsRebuild();
        return;
    case 0x040: case 0x041: case 0x049:
        s_r->g[r] = v;
        windowsRebuild();
        return;
    case 0x060: case 0x061:
        s_r->g[r] = v;
        windowsRebuild();
        if ((s_r->g[0x060] & 1)) lcu2Publish();
        return;
    case 0x003: case 0x004:
        s_r->g[r] = v;
        return;
    case 0x002:
        s_r->g[r] = v;
        Hgfx::rgbOrder = v & 4;
        Hgfx::paletteLost();
        return;
    case 0x020:
        s_r->g[r] = v;
        Hgfx::setG20(v);
        windowsRebuild();
        return;
    case 0x021: case 0x022: case 0x023:
        s_r->g[r] = v;
        windowsRebuild();
        return;
    default:
        s_r->g[r] = v;
    }
}

static void eWrite(uint8_t r, uint8_t v) {
    if (r >= 0xF3) return;            // counter, stamp, ID: read only
    if (r == 0xEF) return;            // FPGA ID: read only
    if (r == 0x30) { s_r->e[r] = v; cpuSpeed(v); return; }
    if (r == 0x00 && v != s_r->e[0]) {
        static bool once;
        if (!once) { once = true; Debug::log("[ELM] e_zxi_000 = %02X: only the URAM Protect size follows it", v); }
    }
    s_r->e[r] = v;
    if (r == 0x00 || r == 0x01) remap();
    if (r == 0x2D) { remap(); divRomApply(); }
    if (r == 0x0F && s_flashOpen) remap();   // the flash window follows the Area at once; the ROMs at the next reset (FLASH2A returns to BASIC between blocks)
    if (r == 0x02) ElmSid::mute(!(v & 8));
    if (r == 0x19) { ElmSid::renderTo(audioPos()); ElmSid::setParams(v & 3); }
}

static uint8_t uRead(uint8_t r) {
    switch (r) {
    case 0xFF: return coreIdNext(&s_corePos);
    case 0x0B: return s_r->u[0x0B];
    case 0x04:                        // the last scancode; KEYSTAT may have taken it already
        if (s_scanHeld) s_scanHeld = false;
        else if (const uint8_t c = ZxEvoAvr::scanPop()) s_scan = c;
        return s_scan;
    case 0x05: {                      // PEN, cleared by this read
        const uint8_t c = ZxEvoAvr::scanPop();
        if (c) { s_scan = c; s_scanHeld = true; }
        return c ? 1 : 0;
    }
    }
    return s_r->u[r];
}

// SounDrive and Czech DACs: the rail is the average of the DACs driven on it (Ports.cpp).
static void dacWrite(int slot, uint8_t v) {
    s_dac[slot] = v;
    s_dacUsed |= (uint8_t)(1 << slot);
    int ln = __builtin_popcount(s_dacUsed & 7), rn = __builtin_popcount(s_dacUsed & 0x18);
    if (ln < 1) ln = 1;
    if (rn < 1) rn = 1;
    ESPectrum::lastCovoxVal = (s_dac[0] + s_dac[1] + s_dac[2]) / ln;
    ESPectrum::lastCovoxValR = (s_dac[3] + s_dac[4]) / rn;
    ESPectrum::CovoxGetSample();
}

// #133B status / TX, #143B RX / speed. There is one UART, behind ZiFi; the FIFO fill
// bits (2..4) are not known here.
static uint8_t uartStatus() {
    if (!Config::zifi_enabled) return 0;
    const uint8_t lsr = ZiFi::uart16550Read(0xFD);
    return (uint8_t)((lsr & 1) | (lsr & 0x20 ? 0 : 2));
}

// e_zxi_030 / u_zxi_00B: the CPU clock. 20 MHz and the overclock modes run at our
// 14 MHz (deviation, recorded in the plan).
static void cpuSpeed(uint8_t sel) {
    const uint8_t want = sel >= 2 ? 2 : sel;
    const uint8_t old = ESPectrum::multiplicator;
    if (want == old) return;
    CPU::tstates = want > old ? CPU::tstates << (want - old) : CPU::tstates >> (old - want);
    ESPectrum::multiplicator = want;
    CPU::updateStatesInFrame();
    OSD::notifyClock(want == 2 ? " CPU: 14 MHz " : want ? " CPU: 7 MHz " : " CPU: 3.5 MHz ");
}

// The 48-bit master counter at 114.6875 MHz, i.e. 32.77 counts per 3.5 MHz T.
static uint64_t masterClock() {
    const uint64_t t = (CPU::global_tstates + CPU::tstates) >> ESPectrum::multiplicator;
    return t * 1146875 / 35000;
}

// RTC72421 at #0003-#0F03 (register = A11..A8): BCD nibbles of the host clock.
// Writes are dropped: the clock is the host's.
static uint8_t rtc72421(uint8_t reg) {
    int y, mo, d, h, mi, sec;
    if (!RTC::now(y, mo, d, h, mi, sec)) return 0;
    switch (reg) {
    case 0x0: return sec % 10;  case 0x1: return sec / 10;
    case 0x2: return mi % 10;   case 0x3: return mi / 10;
    case 0x4: return h % 10;    case 0x5: return h / 10;
    case 0x6: return d % 10;    case 0x7: return d / 10;
    case 0x8: return mo % 10;   case 0x9: return mo / 10;
    case 0xA: return y % 10;    case 0xB: return (y / 10) % 10;
    case 0xC: {                 // day of week, 0 = Sunday (Zeller)
        int m = mo, yy = y;
        if (m < 3) { m += 12; yy--; }
        return (uint8_t)((d + 13 * (m + 1) / 5 + yy + yy / 4 - yy / 100 + yy / 400 + 6) % 7);
    }
    case 0xF: return 4;         // CF: 24-hour
    }
    return 0;
}

static bool unoUart(uint8_t r) { return r >= 0xC6 && r <= 0xC9 && Config::zifi_enabled; }

static bool uramIn(uint8_t lo, uint8_t* out) {
    const Zone& z = s_z[s_zone];
    switch (lo) {
    case 0x07: *out = (uint8_t)z.page[0];        return true;
    case 0x27: *out = (uint8_t)(z.page[0] >> 8); return true;
    case 0x47: *out = (uint8_t)z.page[1];        return true;
    case 0x67: *out = (uint8_t)(z.page[1] >> 8); return true;
    case 0x53: *out = z.par;                     return true;
    case 0x73: *out = s_zone;                    return true;
    }
    return false;
}

static bool uramOut(uint8_t lo, uint8_t v) {
    Zone& z = s_z[s_zone];
    switch (lo) {
    case 0x07: z.page[0] = (z.page[0] & 0xFF00) | v;             break;
    case 0x27: z.page[0] = (z.page[0] & 0x00FF) | (uint16_t)(v << 8); break;
    case 0x47: z.page[1] = (z.page[1] & 0xFF00) | v;             break;
    case 0x67: z.page[1] = (z.page[1] & 0x00FF) | (uint16_t)(v << 8); break;
    case 0x53: z.par = v;                                        break;
    case 0x73: s_zone = v & 3;                                   return true;
    default:   return false;
    }
    remap();
    return true;
}

bool portIn(uint16_t a, uint8_t* out) {
    if (!s_r) return false;
    if (uramIn((uint8_t)a, out)) return true;
    if ((a & 0xF0FF) == 0x0003) {
        *out = (s_r->e[0x02B] & 1) ? rtc72421((uint8_t)(a >> 8)) : 0xFF;
        return true;
    }
    if ((uint8_t)a == 0x2B) {
        if (!(s_r->e[0x02F] & 1)) return false;
        s_lifoTop = (s_lifoTop - 1) & 31;
        *out = s_lifo[s_lifoTop];
        return true;
    }
    switch (a) {
    case 0x9EFD: *out = Ports::feLatch();            return true;   // #FE, readable
    case 0x9FFD: *out = VIDEO::timex_port_ff;        return true;   // #FF, readable
    case 0x133B: *out = uartStatus();                return true;
    case 0x143B: *out = Config::zifi_enabled ? ZiFi::uart16550Read(0xF8) : 0xFF; return true;
    case 0xFF3B:                                     // ULA+ Extra group: g_zxi_001
        if ((s_ulapReg & 0xC0) != 0x80) return false;
        *out = s_r->g[0x001];
        return true;
    case 0x6C3B: *out = s_sidIdx;                    return true;
    case 0x6D3B: ElmSid::renderTo(audioPos()); *out = ElmSid::read(s_sidIdx); return true;
    // TSFM mirror: #7F3B = #FFFD (select, status), #7E3B = #BFFD (data). The Reference
    // gives only the two numbers; Lanex's TFM player for MB03+ (TSFM.TAP) polls BUSY at #7F3B.
    case 0x7F3B: *out = Ports::input(0xFFFD);        return true;
    case 0x7C3B: *out = s_gIdx;                      return true;
    case 0x7D3B: *out = gRead(s_gIdx);               return true;
    case 0x783B: *out = s_eIdx;                      return true;
    case 0x793B:
        if (s_eIdx == 0xF3) s_clkLatch = masterClock();
        *out = s_eIdx >= 0xF3 && s_eIdx <= 0xF8 ? (uint8_t)(s_clkLatch >> ((s_eIdx - 0xF3) * 8))
                                                : s_r->e[s_eIdx];
        return true;
    case 0xFC3B: *out = s_uIdx;                      return true;
    case 0xFD3B:
        *out = unoUart(s_uIdx) ? ZiFi::unoUartRead(true) : uRead(s_uIdx);
        return true;
    }
    // #703B..#773B: the MB03+ answers here with #3x. It is not fitted.
    if ((a & 0xF8FF) == 0x703B) { *out = 0xFF; return true; }
    return false;
}

bool portOut(uint16_t a, uint8_t v) {
    if (!s_r) return false;
    if (s_setupRom) { s_setupRom = false; romsBind(); remap(); }   // the SetUp runs from RAM now; divSD back
#if HOST_BUILD
    if (getenv("PICOSPECCY_ELMLOG"))
        Debug::log("[ELM] out %04X=%02X pc=%04X sp=%04X z=%u idx=%02X/%02X", a, v, Z80::getRegPC(), Z80::getRegSP(), s_zone, s_gIdx, s_eIdx);
#endif
    if (uramOut((uint8_t)a, v)) return true;
    switch ((uint8_t)a) {
    case 0x0F: dacWrite(0, v); return true;
    case 0x1F: dacWrite(1, v); return true;
    case 0x3F: dacWrite(2, v); return true;
    case 0x4F: dacWrite(3, v); return true;
    case 0x5F: dacWrite(4, v); return true;
    }
    if ((a & 0xF0FF) == 0x0003) return true;
    if ((uint8_t)a == 0x2B && (s_r->e[0x02F] & 1)) {
        s_lifo[s_lifoTop] = v;
        s_lifoTop = (s_lifoTop + 1) & 31;
        return true;
    }
    switch (a) {
    case 0x133B: if (Config::zifi_enabled) ZiFi::uart16550Write(0xF8, v); return true;
    case 0x143B: s_r->g[0x030] = v;                  return true;
    case 0x6E3B: case 0x6F3B:                        // SAA1099
        if (ESPectrum::SAA_emu && saaChip) {
            LED::touchW(LED::SAA);
            ESPectrum::SAAGetSample();
            if (a & 0x0100) saaChip->selectRegister(v); else saaChip->setRegisterData(v);
        }
        return true;
    // ULA+ Extra group (10): the data byte is g_zxi_001, the extra graphics mode. A
    // proposal in the Reference ("not approved yet"); groups 00 and 01 stay generic.
    case 0xBF3B: s_ulapReg = v;                      return false;
    case 0xFF3B:
        if ((s_ulapReg & 0xC0) != 0x80) return false;
        gWrite(0x001, v & 0x3F);
        return true;
    case 0x6C3B: s_sidIdx = v;                       return true;
    case 0x6D3B: ElmSid::renderTo(audioPos()); ElmSid::write(s_sidIdx, v); return true;
    case 0x7F3B: Ports::output(0xFFFD, v);           return true;   // TSFM mirror
    case 0x7E3B: Ports::output(0xBFFD, v);           return true;
    case 0x7C3B: s_gIdx = v;                         return true;
    case 0x7D3B: gWrite(s_gIdx, v);                  return true;
    case 0x783B: s_eIdx = v;                         return true;
    case 0x793B: eWrite(s_eIdx, v);                  return true;
    case 0xFC3B:
        s_uIdx = v;
        if (v == 0xFF) s_corePos = 0;
        if (Config::zifi_enabled) ZiFi::unoUartWrite(false, v);
        return true;
    case 0xFD3B:
        if (unoUart(s_uIdx)) ZiFi::unoUartWrite(true, v);
        else if (s_uIdx == 0x41) {    // 14 bits: low byte, then high
            if (s_radFlip) s_radHi = v & 0x3F; else s_r->u[0x41] = v;
            s_radFlip = !s_radFlip;
        }
        else if (s_uIdx != 0xFF) {
            s_r->u[s_uIdx] = v;
            if (s_uIdx == 0x0B && (s_r->e[0x032] & 1)) cpuSpeed(v >> 6);
        }
        return true;
    }
    if ((a & 0xF8FF) == 0x703B) return true;
    if (a == 0xEFF7) VIDEO::timexHiresRequest(v & 2);   // and on to the generic decode
    return false;
}

}
