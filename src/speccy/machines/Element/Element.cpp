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
#include "app/CodeOverlay.h"
#include "speccy/machines/Element/ElmSdma.h"
#include "speccy/machines/Element/ElmSid.h"
#include "speccy/machines/Element/ElmFlash.h"
#include "speccy/machines/Element/ElmTail.h"
#include "speccy/devices/storage/DivMMC.h"
#include "speccy/devices/gs/GS.h"         // g_ngs_zxdma, GS::zxDmaRead: splitRead's ROM wait path
#include "speccy/devices/disk/MB02.h"
#include "speccy/machines/TsConf/ZxEvoAvr.h"
#include "speccy/devices/sound/SAASound.h"
#include "ui/LEDIndicators.h"
#include "ui/OSDMain.h"
#include "ui/UiStage.h"
#include "speccy/devices/storage/RTC.h"
#include "speccy/z80/MemGates.h"
#include "app/Config.h"
#include "app/ESPectrum.h"
#include "speccy/core/Ports.h"
#include "speccy/devices/zifi/ZiFi.h"
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"
#include "speccy/core/Rzx.h"
#include "drivers/graphics/graphics.h"   // graphics_set_scanlines: the SetUp's CRT effect applied live

uint8_t g_elm_nodiv;                  // DivMMC.h: no automap entry in Zone 0 URAM
// Ports::input: e_zxi_010 bit 1 clear ("enable read port hFF" off, the SetUp's default) =
// an IN from #FF does not answer with the Timex register even with the Timex modes on,
// so the read falls through to the personality's floating bus (Reference: the read
// "does not work with some ZX Spectrum games, e.g. Arkanoid").
uint8_t g_elm_ff_off;
const uint8_t* g_elm_divrom;          // DivMMC.cpp: esxDOS from block 4 of the ROM Area (e_zxi_02D bit 0)
uint8_t g_elm_personality = 7;        // Config.h: e_zxi_000 as armed at the last reset

void elmFastSlotsUpdate();   // Video.cpp: g_z80_rd_fast / g_z80_wr_fast from the masks below

namespace Elm {

// Core stamp 2026-10-12, the revision the Reference describes. Binary fields, the
// year as two digits: NETHER.COD builds year<<9 | month<<5 | day from them and wants
// at least #32CB = 25/06/11.
static const uint8_t kStampYear = 26, kStampMonth = 10, kStampDay = 12;
static const uint8_t kInterfaceId = 0xE1;     // "hE1 for eLeMeNt ZX, board v1.x"

// ElmTail.h. The donor tag is ESPectrum's: the ZX page bits 5 and 7. Asked for at the
// first allocation (Elm::reset, after VIDEO::Init built the framebuffer for good).
void* tailAlloc(size_t n) {
    static uint8_t* s_tail;
    static size_t s_tailLeft;
    static bool s_asked;
    if (!s_asked) {
        s_asked = true;
        s_tailLeft = VIDEO::fbDonorSpare(0xA0u, &s_tail);
        if (s_tailLeft) Debug::log("[ELM] pages57 tail: %u B of SRAM at %p for the machine's tables", (unsigned)s_tailLeft, s_tail);
    }
    n = (n + 7) & ~(size_t)7;
    if (n <= s_tailLeft) {
        void* p = s_tail;
        s_tail += n;
        s_tailLeft -= n;
        return p;
    }
    return tryMalloc(n);
}

static uint8_t s_gIdx, s_eIdx, s_uIdx;
#if PERF_TRACE
extern uint32_t g_elm_io_cnt[6], g_elm_io_us[3], g_elm_fd_why[8], g_elm_fd_kind[8], g_elm_fd_src[8];
#endif
#if PERF_TRACE
}
uint8_t elmGIdxNow() { return Elm::s_gIdx; }
namespace Elm {   // the ZXi register index behind #7D3B reads (the IN histogram)
#endif
static uint8_t s_corePos;
// 1 KB, on the heap: only this machine pays for them.
// hrxc: the HRXC colour table as this frame's picture uses it (the Reference: the
// palette is taken at the start of the frame, a change during the display waits).
struct Regs { uint8_t g[256], e[256], u[256], win[256], hrxc[Hgfx::HRXC_BYTES * 2]; };   // hrxc: the shown screen's table, then the other one's
static Regs* s_r;

// URAM. Four Zones of two 8K Areas, each with its own 16-bit page; #73 picks the Zone
// the other ports act on. Backed by the 128-page strip: Protect mode (g_zxi_010 = 2)
// keeps the ZX pages of the Pentagon 1024 out, Full (1) addresses the whole 2 MB.
enum { GM_ELM = 0x20 };               // g_atm_ro bit: Elm::memWrite wants the store
enum { LAYOUT_OFF = 0, LAYOUT_RAM = 2, LAYOUT_FLASH = 6, LAYOUT_HGFX = 8, LAYOUT_HGFX1 = 9, LAYOUT_LCU0 = 0xA, LAYOUT_LCU1 = 0xC, LAYOUT_FRAM = 0xE };
// URAM in 8 KB pages: 256 on the 2 MB board, 512 with the AA-SRAM pack (MEM_PG_CNT is
// 16 KB pages; Config::wantedPages reads e_zxi_038 from the NVS at boot). The Protect
// pool follows the Reference's table: ZX RAM up to 512 KB -> 128 pages (384 with
// AA-SRAM), the Pentagon 1024 -> 64 (256); on the 4096 "there is no Protect mode" (the
// Reference): the pool is the whole URAM, like Full (AWORLD on 08 + AA-SRAM, hw 2026-11-03). The pool sits above the ZX pages, the divRAM is separate memory here.
static inline uint32_t fullPages() { return MEM_PG_CNT * 2; }
static inline bool aasram() { return MEM_PG_CNT >= 256; }
static void aasramWanted(bool want, const char* who);
static void divsdWanted(uint8_t e001, const char* who);
static void bootClassRegs(const uint8_t* e);
// Protect mode leaves URAM what the ZX memory of the personality (e_zxi_000) does not
// take: 128 pages up to 512 KB, 64 on the Pentagon 1024 (the Reference's table).
// HGFNICCC sets Pentagon 128 and streams 120 pages; with 64 its stream wrapped onto
// itself and the player waited for frame #820D. The ZX side stays a Pentagon 1024
// here, so the upper 64 pages sit in ram[96..127], which only divRAM would take.
static void protectPool(uint32_t* base, uint32_t* n) {
    const uint8_t p = s_r->e[0x000];
    // The Reference: "no Protect mode" on the 4096, the whole URAM - but numbered from the
    // same first page as the other personalities (poolIndex wraps): HGFXZOOM unpacks its
    // picture into pages 0-5 and only THEN writes e_zxi_000 = 05; started on 08 with the
    // pool at 0 it then read pages nobody wrote (stripes for the fox, hw 2026-11-13).
    if (p == 8)      { *n = fullPages();          *base = 128; }
    else if (p == 7) { *n = aasram() ? 256 : 64;  *base = 128; }   // base 256 with AA-SRAM until 2026-11-13: the same fault from an 07 start
    else             { *n = aasram() ? 384 : 128; *base = 128; }
}
static uint32_t protectPages() { uint32_t b, n; protectPool(&b, &n); return n; }
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
static const uint8_t kDefaults[0x40] = {
    0x07, 0x01, 0x3F, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 };

static bool s_setupBoot, s_setupRom, s_setupOnce;
static bool s_setupLive, s_resetWanted;   // a SetUp session is running / its RST 0 asked for a reset
static bool s_fromSetup;                  // the reset that follows is the SetUp's exit: validate its registers
static uint8_t s_preSetup[0x40];          // the registers before the SetUp session, for the revert
static bool s_preValid;
static bool s_dmaBoot, s_zifiBoot;        // what this boot came up with (reboot-class SetUp items)
static char s_pendingNote[200];           // a toast raised inside a reset would be wiped: shown a little later
static uint8_t s_pendingFrames;           // ... after the " CPU: x MHz " clock toast of the same reset has gone
static bool s_running;                    // frames have run: a reset now is a live one, not the boot
// Pentagon 4096 (personality 08, rdacomp's combined paging): the latches of the three
// extension ports and a guard for the #7FFD re-entry into the generic decode.
static uint8_t s_p4k1FFD, s_p4kDFFD, s_p4kFDFD, s_p4k7FFD;
static bool s_inGeneric;
static uint8_t s_gamaPage;            // Didaktik Gama 80: the 32 KB page at #8000 (i8255 PC0)
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
uint16_t slotPage[8];                 // HGFX Area: its 8K step in the Zone
volatile uint8_t g_elm_split;         // chunky windows across the SRAM half's edge + the ROM wait slots (Element.h)
static uint8_t s_chunkSplit;          // the chunky half of it (remap)
static uint8_t s_romWaitSlots;        // the ROM wait half (cpuSpeed)
uint8_t  g_elm_romWait;
uint32_t g_elm_stretchK, g_elm_stretchFrac, g_elm_stretchDebt, g_elm_shT;
uint8_t splitRead(uint16_t a) {
    if (!((s_chunkSplit >> (a >> 13)) & 1)) {
        // The OverClock ROM wait: the funnel's own tail after this gate (CPU.cpp gsDmaPeek8).
        VIDEO::Draw(1, false);
        if (g_ngs_zxdma && a < 0x4000 && !MemESP::page0ram && !MemESP::newSRAM && !MemESP::divmmc_mapped)
            return GS::zxDmaRead();
        return MemESP::readbyte(a);
    }
#if HOST_BUILD
    static uint32_t n; if (++n == 1 || n == 1000 || n == 100000) Debug::log("[ELM] split window reads: %u", (unsigned)n);
#endif
    return Hgfx::chunkyReadByte(slotPage[a >> 13] * 0x2000u + (a & 0x1FFF));
}
static inline bool isSplit(uint16_t a) { return (s_chunkSplit >> (a >> 13)) & 1; }
uint8_t* wrKind;
volatile uint8_t g_elm_rdmask = 0xFF, g_elm_wrmask = 0xFF;   // everything generic until the first remap
static uint8_t s_vramSlots;
extern volatile uint8_t g_elm_split, g_elm_vram_rd;           // slots holding a VRAM window page (windowsRebuildNow)
static void elmMasksUpdate() {
    uint8_t wr = 0x03, rd = 0;   // reads in slots 0-1: Video.cpp elmFastSlotsUpdate adds the divMMC / ZX-DMA terms
    if (s_r) for (int sl = 2; sl < 8; sl++) {
        const uint8_t* w = s_r->win + sl * 32;
        for (int i = 0; i < 32; i++) if (w[i]) { wr |= (uint8_t)(1u << sl); break; }
    }
    wr |= g_elm_split; rd |= g_elm_split;
    if (g_elm_vram_rd) rd |= s_vramSlots;
    g_elm_rdmask = rd; g_elm_wrmask = wr;
    ::elmFastSlotsUpdate();   // the loop's / the decoders' per-slot twins (Video.cpp)
}
static const uint8_t s_vramHi0 = 0x40;
const uint8_t* vramHi = &s_vramHi0;
// What a store to a 256-byte page of the Z80 space means. Regions overlap in RAM
// terms: the store also lands in the memory under a device window.
enum { W_NONE, W_VRAM, W_REG, W_COL, W_PROTECT, W_LCU, W_CHUNKY, W_FDMA, W_LCU2, W_SDMA, W_HRXC, W_FLASH, W_FRAM, W_COW };
static_assert(W_COW == WK_COW, "Element.h WK_COW");   // memWriteCold: no case - the store lands (false)
static_assert(W_CHUNKY == WK_CHUNKY && W_VRAM == WK_VRAM, "Element.h WK_*");
static uint8_t* s_lcu;                // 8K, the registers at its start (reads are plain)

static void readHookUpdate();
// Everything windowsRebuild reads. remap runs on every URAM page / zone write and on the
// HGFX VP / OffsetY writes under a chunky window - PF2026: ~35k a second on the bench,
// 4.7 % of core0 on hw in the 256-byte rebuild (PERF_HIST 2026-11-16) - and almost none
// of them change a window: the table is rebuilt only when this key moves.
struct WinKey { uint8_t mode, g20, regMode, act, wp, lcu, hg, fl, fr, g[9]; };
static WinKey s_winKey;
static bool s_winKeyValid;
static WinKey winKey() {
    WinKey k;
    k.mode = Hgfx::mode; k.g20 = Hgfx::g20; k.regMode = Hgfx::reg(Hgfx::REG_MODE);
    k.act = s_active; k.wp = s_wpSlots; k.lcu = s_lcuSlots; k.hg = s_hgfxSlots; k.fl = s_flashSlots; k.fr = s_framSlots;
    static const uint8_t gi[9] = { 0x021, 0x022, 0x023, 0x040, 0x041, 0x048, 0x049, 0x060, 0x061 };
    for (int i = 0; i < 9; i++) k.g[i] = s_r->g[gi[i]];
    return k;
}
static void windowsRebuildNow();
static void windowsRebuild() { s_winKey = winKey(); s_winKeyValid = true; windowsRebuildNow(); }
static void windowsRefresh() {
    const WinKey k = winKey();
    if (s_winKeyValid && !memcmp(&k, &s_winKey, sizeof k)) return;
    s_winKey = k; s_winKeyValid = true;
    windowsRebuildNow();
}
static void windowsRebuildNow() {
    uint8_t* w = s_r->win;
    memset(w, 0, 256);
    // HGFX modes only: under HRC / HRXC #4000-#7BFF is their screen, plain RAM, and the
    // HGFX register page #5B sits in its pixels.
    s_vramSlots = 0;
    if (Hgfx::mode >= 3 && !(Hgfx::g20 & 0x80)) {
        const uint8_t v = s_r->g[0x021];
        for (int i = 0; i < Hgfx::vramPages(); i++) { w[(uint8_t)(v + i)] = W_VRAM; s_vramSlots |= (uint8_t)(1u << ((uint8_t)(v + i) >> 5)); }
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
    // The ROM blocks are the flash image in butter PSRAM, which MemESP::writebyte's
    // flash-pointer filter takes for RAM: with Zone 0 on the legacy mapping, window 0
    // drops the store (bit 0, the ATM rule; the CPU hook lets the divMMC RAM and the
    // Pentagon page-0 RAM through). ula48.tap's bogus IM2 vector ran the ROM's
    // RAM-fill over the ROM0 block and the "48K" went dark (bench 2026-11-03).
    if (s_active & 1) g_atm_ro &= (uint8_t)~1; else g_atm_ro |= 1;
    readHookUpdate();
    elmMasksUpdate();
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
    if (was != on) elmMasksUpdate();
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
static void cpuSpeed(uint8_t sel, bool quiet = false);
static void eWrite(uint8_t r, uint8_t v);
static void extraTick();
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
// A run of c bytes inside one 8K slot of the Z80 space, read / stored as
// MemESP::readbyte / writebyte would do it byte by byte, when the slot is a plain page:
// those two are out-of-line FLASH copies here, and PF2026's ~350 FastDMA blits a frame
// (8 x 4 bytes each) spent ~10 % of core0 in them (PERF_HIST 2026-11-16). false = the
// slot needs the per-byte rules (breakpoints, accessor bank, divMMC / ROM / Next windows).
// memcpy from 4 bytes up: it stores words, and the destinations here are PSRAM pages,
// where the store, not the call, is the cost - an inline byte loop moved PF2026's 7.9 %
// from memcpy into the caller and made it slightly worse (hw 2026-11-16, zx28e).
static inline void cpy(uint8_t* d, const uint8_t* s, uint32_t n) {
    if (n >= 4) { memcpy(d, s, n); return; }
    for (uint32_t k = 0; k < n; k++) { d[k] = s[k]; __asm__ volatile(""); }
}
// The bytes at a as MemESP::readbyte would serve them, when the slot is a plain page.
static inline const uint8_t* rdPtr(uint16_t a) {
    const uint8_t* p = MemESP::ramCurrent[a >> 13];
    if (!p || Config::numMemReadBP || (a < 0x4000 && MemESP::divmmc_mapped) || isSplit(a)) return nullptr;
    return p + (a & 0x1FFF);
}
// A byte as the guest's generic read path serves it: a split chunky window byte by byte.
static inline uint8_t rdByte(uint16_t a) { return isSplit(a) ? splitRead(a) : MemESP::readbyte(a); }
static inline bool rdRun(uint16_t a, uint8_t* buf, uint32_t c) {
    const uint8_t* p = rdPtr(a);
    if (!p) return false;
    cpy(buf, p, c);
    return true;
}
static inline bool wrRun(uint16_t a, const uint8_t* buf, uint32_t c) {
    const uint8_t slot = a >> 13;
    uint8_t* p = MemESP::ramCurrent[slot];
    if (a < 0x4000 || Config::numMemWriteBP || ((MemESP::wr_overlay_mask >> slot) & 1) ||
        (uintptr_t)p < 0x11000000u) return false;
    cpy(p + (a & 0x1FFF), buf, c);
    *mem_desc_t::bank_dirty[slot] = true;
    return true;
}
static inline void rdBytes(uint16_t a, uint8_t* buf, uint32_t c) {
    if (!rdRun(a, buf, c)) for (uint32_t k = 0; k < c; k++) buf[k] = rdByte((uint16_t)(a + k));
}
static inline void wrBytes(uint16_t a, const uint8_t* buf, uint32_t c) {
    if (!wrRun(a, buf, c)) for (uint32_t k = 0; k < c; k++) MemESP::writebyte((uint16_t)(a + k), buf[k]);
}

// A DMA / FastDMA read: the wiki says both scroll HGFX in COPY mode, so their reads of
// the window arm the latch exactly like the CPU's.
uint8_t busRead(uint16_t a) {
    if (g_elm_vram_rd) memRead(a);
    return rdByte(a);
}
void busWrite(uint16_t a, uint8_t v) {
    if (s_r && s_r->win[a >> 8] && memWrite(a, v)) return;
    MemESP::writebyte(a, v);
}
// HGFXAGON copies ~7000 bytes of the window a frame by Z80 DMA in COPY mode: per byte
// that was busRead + busWrite + memWrite, three flash calls from the RAM byte loop.
// Here a piece stays inside one 256-byte page at both ends, so the page kinds hold;
// the latch ends disarmed, as the last write of the per-byte path leaves it.
uint32_t dmaCopy(uint16_t src, uint16_t dst, uint32_t n, uint32_t cycles) {
    if (!s_r || !Hgfx::reg(Hgfx::REG_MASK)) return 0;
    uint32_t c = n;
    if (c > 0x100u - (src & 0xFF)) c = 0x100u - (src & 0xFF);
    if (c > 0x100u - (dst & 0xFF)) c = 0x100u - (dst & 0xFF);
    const uint8_t sk = s_r->win[src >> 8];
    if (s_r->win[dst >> 8] != W_VRAM) return 0;
    if (!g_elm_vram_rd) {
        // Outside COPY mode the window takes plain bytes: HGFXCUBE clears its 6144 bytes
        // every frame with #E000 -> #E001. Read and store a byte at a time (the overlap
        // replicates), the HGFX side gets the piece as one byte run.
        if (sk != W_NONE && sk != W_VRAM) return 0;
        const uint32_t left = CPU::tstates < CPU::statesInFrame ? (CPU::statesInFrame - CPU::tstates) / (cycles ? cycles : 1) + 1 : 1;
        if (c > left) c = left;
        if ((src < 0x4000 || dst < 0x4000) && MemESP::divmmc_mapped) return 0;
        uint8_t buf[256];
        if (dst <= src || dst >= src + c) { rdBytes(src, buf, c); wrBytes(dst, buf, c); }
        else for (uint32_t k = 0; k < c; k++) {
            buf[k] = rdByte((uint16_t)(src + k));
            MemESP::writebyte((uint16_t)(dst + k), buf[k]);
        }
        Hgfx::writeVramRun((uint16_t)(dst - ((uint16_t)s_r->g[0x021] << 8)), buf, c);
#if PERF_TRACE
        g_elm_io_cnt[0] += c;
#endif
        uint32_t t = c * cycles;
        while (t) { const uint32_t q = t > 200 ? 200 : t; VIDEO::Draw(q, false); t -= q; }
        return c;
    }
    if (sk != W_VRAM) return 0;
    // Not past the frame's end: the caller closes the frame between pieces.
    const uint32_t left = CPU::tstates < CPU::statesInFrame ? (CPU::statesInFrame - CPU::tstates) / (cycles ? cycles : 1) + 1 : 1;
    if (c > left) c = left;
    if ((src < 0x4000 || dst < 0x4000) && MemESP::divmmc_mapped) return 0;
    const uint16_t base = (uint16_t)s_r->g[0x021] << 8;
    Hgfx::copyRun((uint16_t)(dst - base), (uint16_t)(src - base), c);
    // The stores land in RAM too, byte by byte: an overlapping copy replicates.
    if (dst <= src || dst >= src + c) { uint8_t buf[256]; rdBytes(src, buf, c); wrBytes(dst, buf, c); }
    else for (uint32_t k = 0; k < c; k++) MemESP::writebyte((uint16_t)(dst + k), rdByte((uint16_t)(src + k)));
    s_rdArmed = false;
#if PERF_TRACE
    g_elm_io_cnt[1] += c;
#endif
    uint32_t t = c * cycles;
    while (t) { const uint32_t q = t > 200 ? 200 : t; VIDEO::Draw(q, false); t -= q; }
    return c;
}
// PF2026 reads every sector from the SD data port #EB with the Z80 DMA: per byte that
// was dmaInput -> the whole Ports::input decode -> spiRead, then busWrite + writebyte
// (a fifth of core0 at 15-20 FPS, PERF_HIST 2026-11-16). Here a piece of the block's
// data phase goes from the sector buffer straight into the RAM page (mmc_read_bulk, the
// Next INIR batch's shape); the protocol bytes around it (R1, token, CRC) stay on the
// per-byte path. The piece keeps the per-byte path's time: port + memory cycles and the
// 1 T early I/O cycle input() draws, uncontended, and stops at the frame's end.
uint32_t dmaFromCard(uint16_t port, uint16_t dst, uint32_t n, uint32_t cycles) {
    if ((port & 0xFF) != 0xEB || !DivMMC::enabled || DivMMC::divide_mode) return 0;
    if (Config::numPortReadBP || Config::numMemWriteBP || dst < 0x4000) return 0;
    if (s_r && s_r->win[dst >> 8] != W_NONE) return 0;      // a window: busWrite per byte
    const uint8_t slot = dst >> 13;
    if ((MemESP::wr_overlay_mask >> slot) & 1) return 0;
    uint8_t* p = MemESP::ramCurrent[slot];
    if ((uintptr_t)p < 0x11000000u) return 0;              // ROM / accessor bank
    const uint32_t per = cycles + 1;
    uint32_t c = n;
    if (c > 0x100u - (dst & 0xFF)) c = 0x100u - (dst & 0xFF);
    if (CPU::tstates >= CPU::statesInFrame) return 0;
    const uint32_t left = (CPU::statesInFrame - CPU::tstates) / per;
    if (c > left) c = left;
    if (!c) return 0;
#if PERF_TRACE
    const uint32_t t0_ = time_us_32();
#endif
    const uint32_t k = DivMMC::mmc_read_bulk(p + (dst & 0x1FFF), c);
#if PERF_TRACE
    g_elm_io_us[0] += time_us_32() - t0_;
#endif
    if (!k) return 0;
    *mem_desc_t::bank_dirty[slot] = true;
    LED::touchR(LED::SD);
#if PERF_TRACE
    g_elm_io_cnt[2] += k;
#endif
    uint32_t t = k * per;
    while (t) { const uint32_t q = t > 200 ? 200 : t; VIDEO::Draw(q, false); t -= q; }
    return k;
}
// The 48 ROM's key wait at 28 MHz: no HALT, the whole frame is passes of
// WAIT-KEY1 (#15DE: CALL INPUT-AD / RET C / JR Z) -> INPUT-AD -> CALL-SUB -> KEY-INPUT
// (#10A8: BIT 3,(IY+2) / CALL NZ / AND A / BIT 5,(IY+1) / RET Z) - 45-59k
// instructions a frame for nothing (BASIC 48: 17 FPS against the Next's 51, whose
// NextIdle skips its own loop; hw 2026-11-16). Only the ISR can end it (KEYBOARD
// sets FLAGS bit 5), so once two passes meet at the return to #15E1 with Z set,
// C clear and the same registers, guest time moves on by whole passes to the
// slice end - NextPorts::pollSkip's arithmetic. The pass writes nothing but the
// same stack words each time. Called from Z80::check_trdos (RET to #15E1).
static const struct { uint16_t a; uint8_t n; uint8_t b[10]; } s_kwSig[] = {
    { 0x15DE, 6,  { 0xCD, 0xE6, 0x15, 0xD8, 0x28, 0xFA } },
    { 0x15E6, 9,  { 0xD9, 0xE5, 0x2A, 0x51, 0x5C, 0x23, 0x23, 0x18, 0x08 } },
    { 0x15F7, 10, { 0x5E, 0x23, 0x56, 0xEB, 0xCD, 0x2C, 0x16, 0xE1, 0xD9, 0xC9 } },
    { 0x162C, 1,  { 0xE9 } },
    { 0x10A8, 10, { 0xFD, 0xCB, 0x02, 0x5E, 0xC4, 0x1D, 0x11, 0xA7, 0xFD, 0xCB } },
    { 0x10B2, 3,  { 0x01, 0x6E, 0xC8 } },
};
static const uint8_t* s_kwPg[2];
static bool s_kwOk;
static uint32_t s_kwT, s_kwR, s_kwRegs;
#if PERF_TRACE
uint32_t g_elm_kw[2];   // skips, passes skipped
uint32_t g_elm_io_cnt[6];   // bytes a window: dmaCopy plain / COPY, dmaFromCard, FastDMA run / per byte, (5) spare
uint32_t g_elm_io_us[3];    // core0 time a window: dmaFromCard, fdmaRun (whole call), fdmaRun per-byte pieces
uint32_t g_elm_fd_why[8], g_elm_fd_kind[8], g_elm_fd_src[8];   // fd_src: fill / URAM / chunky window / plain page (4 = per-byte read fallback)   // FastDMA pieces a window: per-byte reason (1 src divMMC, 2 overlap, 3 dst low, 4 VRAM rule, 5 split chunky src, 6 other window), destination kind
#endif
void keyWaitSkip() {
    const uint16_t af = Z80::getRegAF();
    if (!(af & 0x40) || (af & 0x01)) return;   // Z set, C clear: no key, loop again
    if (!Z80::isIFF1() || Z80::getIM() != Z80::IM1 || Z80::isNMI() || Rzx::mode) return;
    if (Z80::getRegIY() != 0x5C3A || MemESP::divmmc_mapped || Config::numPcBP) return;
    if (MemESP::ramCurrent[0] != s_kwPg[0] || MemESP::ramCurrent[1] != s_kwPg[1]) {
        s_kwPg[0] = MemESP::ramCurrent[0]; s_kwPg[1] = MemESP::ramCurrent[1];
        s_kwOk = true;
        for (const auto& s : s_kwSig)
            for (int i = 0; i < s.n && s_kwOk; i++) s_kwOk = MemESP::readbyte((uint16_t)(s.a + i)) == s.b[i];
    }
    if (!s_kwOk) return;
    // Channel K's input routine, no ED-COPY due, no key yet.
    const uint16_t ch = (uint16_t)(MemESP::readbyte(0x5C51) | MemESP::readbyte(0x5C52) << 8);
    if ((MemESP::readbyte((uint16_t)(ch + 2)) | MemESP::readbyte((uint16_t)(ch + 3)) << 8) != 0x10A8) return;
    if ((MemESP::readbyte(0x5C3C) & 0x08) || (MemESP::readbyte(0x5C3B) & 0x20)) return;
    const uint32_t t = CPU::tstates, r = Z80::getFetchCounter();
    const uint32_t regs = Z80::getRegBC() ^ (uint32_t)Z80::getRegDE() << 16 ^ (uint32_t)Z80::getRegHL() << 5 ^
                          (uint32_t)Z80::getRegSP() << 11 ^ (uint32_t)Z80::getRegIX() << 21 ^
                          (uint32_t)Z80::getRegBCx() << 3 ^ (uint32_t)Z80::getRegDEx() << 9 ^ (uint32_t)Z80::getRegHLx() << 14;
    const uint32_t g = (uint32_t)(CPU::global_tstates + t);
    const uint32_t d = g - s_kwT, dR = r - s_kwR;
    const bool same = regs == s_kwRegs;
    s_kwT = g; s_kwR = r; s_kwRegs = regs;
    if (!same || d == 0 || d > 1024) return;
    if ((int32_t)t < CPU::IntEnd) return;
    uint32_t lim = CPU::statesInFrame - (uint32_t)CPU::IntEnd;
    if (CPU::stFrame && CPU::stFrame < lim) lim = CPU::stFrame;
    if (lim <= t) return;
    const uint32_t k = (lim - t) / d;
    if (!k) return;
    const uint32_t to = t + k * d;
    while (CPU::tstates < to) {
        uint32_t n = to - CPU::tstates;
        if (VIDEO::tStatesPerLine && n > VIDEO::tStatesPerLine) n = VIDEO::tStatesPerLine;
        VIDEO::Draw(n, false);
    }
    Z80::incRegR(k * dR);
    s_kwT = (uint32_t)(CPU::global_tstates + CPU::tstates); s_kwR = Z80::getFetchCounter();
#if PERF_TRACE
    g_elm_kw[0]++; g_elm_kw[1] += k;
#endif
}

void masksRefresh() { elmMasksUpdate(); }
static void fdWrite(uint32_t a, bool uram, uint8_t v) {
    if (uram) { uint8_t* p = uramByte(a); if (p) { if (Hgfx::g_cowOn) Hgfx::cowGuard(p, 1); *p = v; } return; }
    busWrite((uint16_t)a, v);
}

ELM_OVL_CODE static void fdmaRun(int set, bool wide24) {   // .elmovl: 2 400 pieces a frame on PF2026, 6.6 ms of flash code (hw 2026-11-18)
#if PERF_TRACE
    struct FdT_ { uint32_t t0; FdT_() : t0(time_us_32()) {} ~FdT_() { g_elm_io_us[1] += time_us_32() - t0; } } fdT_;
#endif
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
    // The plain transfer (no floating step, upwards) goes in pieces that stay inside one
    // batch and, at both ends, one 256-byte page of the Z80 space or one 8K page of the
    // URAM, so what a piece reads and where it lands is decided once. FDMATEST: 20.7k
    // bytes a frame into the VRAM window; Another World: 128k bytes a game frame, URAM ->
    // URAM and URAM -> the HGFX 7 chunky window, which the per-byte dispatcher below
    // (uramPtr, busWrite, memWrite, chunkyWrite for every byte) ran at 15-20 FPS.
    const bool plain = !sfl && !dfl && !down;
    // The blit: a plain page -> the chunky window, rows of dbatch bytes (each inside one
    // 256-byte page), the source one run inside one page. PF2026: ~600 a frame, par 03
    // len #20 db 8 ds #F8 (an 8x4 block), source the URAM stream paged into the Z80 space.
    // Decided once, the rows' window offsets taken up front, then one chunkyWriteRows;
    // anything else (or one row that does not qualify) takes the piece loop below.
    bool blit = false;
    if (plain && !fill && !suram && !duram && dbatch < 0x100 && len <= 0x100 && len % dbatch == 0 &&
        sbatch >= len && len / dbatch <= 32) {
        const uint32_t sa = s & amask;
        const uint8_t sslot = (sa >> 13) & 7;
        const uint8_t* sp = nullptr;
        if ((sa & 0xFF) + len <= 0x100 && !((s_hgfxSlots >> sslot) & 1) && s_r->win[(sa >> 8) & 0xFF] != W_VRAM &&
            !(sa < 0x4000 && MemESP::divmmc_mapped))
            sp = rdPtr((uint16_t)sa);
        if (sp) {
            const uint32_t nr = len / dbatch;
            uint32_t offs[32];
            uint32_t dd = d;
            bool ok = true;
            for (uint32_t k = 0; k < nr && ok; k++, dd += dbatch + dskip) {
                const uint32_t da = dd & amask;
                ok = (da & 0xFF) + dbatch <= 0x100 && s_r->win[(da >> 8) & 0xFF] == W_CHUNKY &&
                     !(da < 0x4000 && MemESP::divmmc_mapped);
                offs[k] = slotPage[(da >> 13) & 7] * 0x2000u + (da & 0x1FFF);
            }
            if (ok) {
#if PERF_TRACE
                const uint32_t tw_ = time_us_32();
#endif
                Hgfx::chunkyWriteRows(offs, sp, dbatch, nr);
#if HOST_BUILD
                {
                    static int chk = -1;
                    static uint32_t blits, bad;
                    if (chk < 0) chk = getenv("PICOSPECCY_ELM_BLITCHK") ? 1 : 0;
                    if (chk) {
                        bad += Hgfx::chunkyRowsCheck(offs, sp, dbatch, nr);
                        if ((++blits & 0xFFFF) == 0) Debug::log("[BLITCHK] %u blits, %u bad bytes", blits, bad);
                    }
                }
#endif
#if PERF_TRACE
                g_elm_io_us[2] += time_us_32() - tw_;
                g_elm_io_cnt[3] += len; g_elm_io_cnt[5]++;
                g_elm_fd_src[3] += nr; g_elm_fd_kind[W_CHUNKY] += nr;
#endif
                s += len;
                if (sbatch == len) s += sskip;
                d = dd;
                blit = true;
            }
        }
    }
    for (uint32_t i = 0; plain && !blit && i < len; ) {
        const uint32_t sa = s & amask, da = d & amask;
        uint8_t buf[256];
        uint32_t c = len - i;
        if (c > sizeof buf) c = sizeof buf;
        if (c > sbatch - sn) c = sbatch - sn;
        if (c > dbatch - dn) c = dbatch - dn;
        const uint32_t droom = duram ? 0x2000u - (da & 0x1FFF) : 0x100u - (da & 0xFF);
        if (c > droom) c = droom;
        if (!fill) {
            const uint32_t sroom = suram ? 0x2000u - (sa & 0x1FFF) : 0x100u - (sa & 0xFF);
            if (c > sroom) c = sroom;
        }
        // Where the bytes come from and go to in host memory, for the overlap test: a
        // destination that starts inside the source replicates, byte by byte.
        const uint8_t* sp = fill ? nullptr : suram ? uramByte(sa) : MemESP::ramCurrent[(sa >> 13) & 7] + (sa & 0x1FFF);
        uint8_t* dp = duram ? uramByte(da) : MemESP::ramCurrent[(da >> 13) & 7] + (da & 0x1FFF);
        const uint8_t kind = duram ? W_NONE : s_r->win[(da >> 8) & 0xFF];
        // The source in a chunky window: read through the HGFX buffer (the SRAM half,
        // a split window), never the slot's raw pointer. PF2026 moves its decoded video
        // from buffer 1 to the shown buffer this way, ~8 KB a frame into the VRAM
        // window in COPY mode, which the per-byte path below ran at ~2 ms a frame of
        // flash calls (hw PERF_HIST 2026-11-16).
        const bool schunk = !fill && !suram && ((s_hgfxSlots >> ((sa >> 13) & 7)) & 1);
        // COPY mode's read latch is per byte: only a read of the VRAM window arms it
        // (memRead), so a run from anywhere else, with nothing armed, is the plain
        // writes the per-byte path would have made.
        const bool svram = !fill && !suram && s_r->win[(sa >> 8) & 0xFF] == W_VRAM;
        bool fast = true;
#if PERF_TRACE
        uint8_t why = 0;
#define FD_SLOW(n) do { fast = false; if (!why) why = (n); } while (0)
#else
#define FD_SLOW(n) (fast = false)
#endif
        if (!fill && !suram && sa < 0x4000 && MemESP::divmmc_mapped) FD_SLOW(1);
        if (sp && dp && dp > sp && dp < sp + c) FD_SLOW(2);
        if (!duram) {
            if (da < 0x4000 && (MemESP::divmmc_mapped || kind == W_NONE)) FD_SLOW(3);   // ROM, divMMC: the generic rules
            // COPY mode, VRAM -> VRAM: one copyRun (every byte read arms the latch, every
            // write copies it - what dmaCopy does for the Z80 DMA); PF2026 moved ~930 B a
            // frame this way at 2.6 us a byte through fdWrite (hw 2026-11-18, 2.45 ms).
            if (kind == W_VRAM) { if (!Hgfx::reg(Hgfx::REG_MASK) || (g_elm_vram_rd && !svram && s_rdArmed)) FD_SLOW(4); }
            else if (kind == W_CHUNKY) { if (schunk) FD_SLOW(5); }
            else if (kind != W_NONE) FD_SLOW(6);
        }
#if PERF_TRACE
        if (!fast) g_elm_fd_why[why]++;
        g_elm_fd_kind[kind & 7]++;
#endif
#undef FD_SLOW
        if (fast) {
            // The source is read in place when it is a plain page (no copy into buf);
            // the overlap test above already sent a destination inside it per byte.
            const uint8_t* src = buf;
#if PERF_TRACE
            g_elm_fd_src[fill ? 0 : suram ? 1 : schunk ? 2 : 3]++;
#endif
            if (fill) memset(buf, fv, c);
            else if (suram) { if (sp) src = sp; else memset(buf, 0xFF, c); }
            else if (schunk) Hgfx::chunkyReadRun(slotPage[(sa >> 13) & 7] * 0x2000u + (sa & 0x1FFF), buf, c);
            else if (!(src = rdPtr((uint16_t)sa))) { rdBytes((uint16_t)sa, buf, c); src = buf;
#if PERF_TRACE
                g_elm_fd_src[4]++;
#endif
            }
            if (duram) { if (dp) { if (Hgfx::g_cowOn) Hgfx::cowGuard(dp, c); memmove(dp, src, c); } }
            else if (kind == W_CHUNKY) {
#if PERF_TRACE
                const uint32_t tw_ = time_us_32();
#endif
                Hgfx::chunkyWriteRun(slotPage[(da >> 13) & 7] * 0x2000u + (da & 0x1FFF), src, c);
#if PERF_TRACE
                g_elm_io_us[2] += time_us_32() - tw_;
#endif
            }
            else {
                const uint16_t vbase = (uint16_t)s_r->g[0x021] << 8;
                if (kind == W_VRAM) {
                    if (g_elm_vram_rd && svram) { Hgfx::copyRun((uint16_t)(da - vbase), (uint16_t)(sa - vbase), c); s_rdArmed = false; }
                    else Hgfx::writeVramRun((uint16_t)(da - vbase), src, c);
                }
                wrBytes((uint16_t)da, src, c);   // under the window too
            }
        } else {
#if PERF_TRACE
            const uint32_t tb_ = time_us_32();
#endif
            for (uint32_t k = 0; k < c; k++)
                fdWrite((da + k) & amask, duram, fill ? fv : fdRead((sa + k) & amask, suram));
#if PERF_TRACE
            g_elm_io_us[2] += time_us_32() - tb_;
#endif
        }
#if PERF_TRACE
        g_elm_io_cnt[fast ? 3 : 4] += c;
#endif
        s += c; d += c; i += c;
        if ((sn += c) == sbatch) { sn = 0; s += sskip; }
        if ((dn += c) == dbatch) { dn = 0; d += dskip; }
    }
    for (uint32_t i = 0; !plain && i < len; i++) {
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

static uint32_t poolIndex(uint16_t page) {   // Protect: the pool wraps like the board's page decode
    uint32_t b, n; protectPool(&b, &n);
    // The Pentagon 1024 REPORTS 64 (256) free pages, but a page number means the same
    // page as on the smaller personalities: the APP launcher loads PF2026 into pages
    // #5F-#77 on 07, the demo then writes e_zxi_000 = 05 and reads them back. With the
    // 64-page wrap it read pages nobody wrote (green bars for the title, bench 2026-11-14).
    if (s_r->e[0x000] == 7) n = aasram() ? 384 : 128;
    return (b + ((n & (n - 1)) ? page % n : (page & (n - 1)))) & (fullPages() - 1);
}
static uint8_t* uramPtr(uint16_t page) {
    const uint8_t mode = uramMode();
    const uint32_t idx = mode == 1 ? (page & (fullPages() - 1)) : poolIndex(page);
    uint8_t* p = MemESP::ram[idx >> 1].direct();
    return p ? p + (idx & 1) * 0x2000 : nullptr;
}

// SoundDMA reads protected URAM whatever the mode (the Reference: "works with protected
// URAM only"); the address counts from its first page.
static const uint8_t* sdPage(uint32_t page) {
    const uint32_t idx = poolIndex((uint16_t)page);
    const uint8_t* p = MemESP::ram[idx >> 1].direct();
    return p ? p + (idx & 1) * 0x2000 : nullptr;
}

static uint16_t freePages() {
    const uint8_t mode = uramMode();
    return (uint16_t)(mode == 1 ? fullPages() : mode == 2 ? protectPages() : 0);
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
    case 2: MemESP::plugPtr16(2, MemESP::ram[(g_elm_personality == 9 && (s_gamaPage & 1)) ? 1 : 2].sync(2)); break;
    case 3: MemESP::plugPtr16(3, MemESP::ram[(g_elm_personality == 9 && (s_gamaPage & 1)) ? 3 : MemESP::bankLatch].sync(3)); break;
    }
}

// Pentagon 4096 (personality 08): the Reference's "Combined memory variants" (rdacomp's
// Pentagon-4096 core): #7FFD bits 0-2 and 6-7 are the Pentagon 512's page bits 0-4, bit 5
// is the 4096K bit (page bit 7: the AA-SRAM half, absent here, so it is dropped - no
// lock either); page bits 5-6 come from #1FFD bits 4 and 7 (Kay/Scorpion), #DFFD bits
// 0-1 (Profi) or #FDFD bits 0-1 (ATM1), OR-ed; their bit 2 is the 4096K bit again.
// #1FFD bit 0 puts RAM page 0 at #0000 (Scorpion). The generic decode keeps the #7FFD
// side effects; the extension bits ride on its #AFF7 extended-page latch (32 pages per
// unit for a 512), re-applied by the generic #AFF7 handler on every change.
static void p4kExtra() {
    uint8_t extra = (uint8_t)(((s_p4k1FFD >> 4) & 1) | ((s_p4k1FFD >> 6) & 2) | (s_p4kDFFD & 3) | (s_p4kFDFD & 3));
    if (aasram()) extra |= (uint8_t)((((s_p4k7FFD >> 5) | (s_p4kDFFD >> 2) | (s_p4kFDFD >> 2)) & 1) << 2);   // the 4096K bit
    static uint8_t logged;
    if (logged < 8) { logged++; Debug::log("[ELM] P4096 7FFD=%02X 1FFD=%02X DFFD=%02X FDFD=%02X -> extra %u", s_p4k7FFD, s_p4k1FFD, s_p4kDFFD, s_p4kFDFD, extra); }
    if (Ports::portAFF7 != extra) Ports::output(0xAFF7, extra);
}
static bool p4kOut(uint8_t hi, uint8_t v) {
    if (hi == 0x7F) {
        if (s_inGeneric) return false;
        if (((s_p4k7FFD ^ v) & 0x20) && aasram()) { s_p4k7FFD = v; p4kExtra(); }
        s_p4k7FFD = v;
        s_inGeneric = true;
        Ports::output(0x7FFD, v & ~0x20);
        s_inGeneric = false;
#if HOST_BUILD
        if (getenv("PICOSPECCY_ELMLOG")) Debug::log("[ELM] P4096 7FFD=%02X -> bank %u aff7 %u", v, (unsigned)MemESP::bankLatch, Ports::portAFF7);
#endif
        return true;
    }
    if (hi == 0x1F) s_p4k1FFD = v;
    else if (hi == 0xDF) s_p4kDFFD = v;
    else if (hi == 0xFD) s_p4kFDFD = v;
    else return false;
    p4kExtra();
    const uint32_t ram0 = s_p4k1FFD & 1;
    if (MemESP::page0ram != ram0) { MemESP::page0ram = ram0; MemESP::recoverPage0(); }
    return true;
}

// Didaktik Gama 80 (personality 09): 80 KB as two 32 KB pages at #8000, picked by
// the i8255's port C bit 0 - OUT 127,x (control word, BSR for bit 0) or port 95
// (Reference, "Didaktik Gama 80KB memory"). Page 0 = the usual 2/0, page 1 = 1/3.
static void gamaPlug() {
    const bool alt = s_gamaPage & 1;
    MemESP::plugPtr16(2, MemESP::ram[alt ? 1 : 2].sync(2));
    MemESP::plugPtr16(3, MemESP::ram[alt ? 3 : 0].sync(3));
    remap();                            // URAM zones 2/3 back on top
}
static bool gamaOut(uint8_t lo, uint8_t v) {
    if (g_elm_personality != 9) return false;
    uint8_t page = s_gamaPage;
    if (lo == 0x7F) {                   // control word: BSR (bit 7 = 0) on bit 0 only
        if (v & 0x80) return true;      // a mode-set word: nothing to page
        if (((v >> 1) & 7) != 0) return true;
        page = v & 1;
    } else if (lo == 0x5F) page = v & 1;
    else return false;
    if (page != s_gamaPage) { s_gamaPage = page; gamaPlug(); }
    return true;
}

// The one writer of a URAM slot. The reserved layouts leave the Zone on the legacy
// mapping. Layout 6 (zone 0) shows the FlashRAM of the ROM Area in 8 KB pages, its
// writes are the JEDEC commands (ElmFlash); layout E shows the FRAM in Area1.
void remap() {
    if (!s_r) return;
    const bool on = (s_r->g[0x000] & 1) && uramMode();
    uint8_t active = 0, wp = 0, lcu = 0, hg = 0, fl = 0, fr = 0, split = 0;
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
            bool sp = false;
            uint8_t* p = isLcu ? lcuBlock()
                       : isHg && Hgfx::ensure() ? Hgfx::chunkyRead(hgStep * 0x2000u, slot < 2, &sp)
                       : isFl ? (ElmFlash::ensure() ? ElmFlash::page(s_r->e[0x00F], s_z[z].page[a]) : nullptr)
                       : isFr ? (ElmFlash::ensure() ? ElmFlash::fram() : nullptr)
                       : uramPtr(s_z[z].page[a]);
            if (isHg) { hg |= 1u << slot; slotPage[slot] = hgStep; if (sp) split |= 1u << slot; }
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
    s_chunkSplit = split;
    g_elm_split = split | s_romWaitSlots;
    if (Hgfx::g_cowOn) Hgfx::cowMark();  // the pair frame's captured screen in its new slots (the rebuild wiped the marks)
    elmMasksUpdate();
    Hgfx::direct(hg != 0);
    // e_zxi_02D bit 7 clear: URAM in Zone 0 is on top, the divSD entry points do not trap
    g_elm_nodiv = (active & 1) && !(s_r->e[0x02D] & 0x80);
    windowsRefresh();
    // A window across the edge of the SRAM half sent it back to PSRAM in this pass:
    // slots mapped before it point into freed memory. One more pass (nothing in SRAM now).
    if (Hgfx::chunkyStale()) remap();
}

static const uint8_t* s_hrxcSrc;      // the screen the table was latched from
// The screen not shown (KeyLayer's bottom one): its table is latched with the shown one's,
// or a fade written to both in one loop reads a step apart (HRXCIMGS: 31 colours for 16).
static const uint8_t* hrxcOther() {
    const uint8_t* b = MemESP::ram[7].direct();
    return VIDEO::grmem == b ? MemESP::ram[(s_r->g[0x002] & 1) ? 6 : 5].direct() : b;
}
static void hrxcLatch() {
    s_hrxcSrc = VIDEO::grmem;
    if (Hgfx::mode != 2) return;
    memcpy(s_r->hrxc, VIDEO::grmem + Hgfx::HRXC_TABLE, Hgfx::HRXC_BYTES);
    memcpy(s_r->hrxc + Hgfx::HRXC_BYTES, hrxcOther() + Hgfx::HRXC_TABLE, Hgfx::HRXC_BYTES);
}

// KeyLayer: the screen #7FFD bit 3 shows is on top, the other shines through its key
// colour. g_zxi_002 bit 0 puts the first screen in page 6 instead of 5.
void frameEnd() {
    s_running = true;
    const uint8_t* a = MemESP::ram[(s_r->g[0x002] & 1) ? 6 : 5].direct();
    const uint8_t* b = MemESP::ram[7].direct();
    if (s_r->g[0x002] & 1) VIDEO::grmem = MemESP::videoLatch ? (uint8_t*)b : (uint8_t*)a;
    const bool key = s_r->g[0x003] == 1;
    Hgfx::keyTop = key ? (MemESP::videoLatch ? b : a) : nullptr;
    Hgfx::keyBottom = key ? (MemESP::videoLatch ? a : b) : nullptr;
    Hgfx::keyColour = s_r->g[0x004];
    // GigaScreen for HRC is the owner's switch, as for the ZX screen: "On" in the menu
    // (SetUp 01 / 02) draws it 256 wide and mixed, Off keeps the 512 px pair
    // frame. Following g_zxi_008 alone blurred TOCHIHRC's 512 px title text (owner
    // 2026-11-14); the register only says what is mixed: 1 the two VRAMs, else two frames.
    const uint8_t gkind = s_r->g[0x008] == 1 ? 1 : 2;
    Hgfx::giga = Config::gigascreen_onoff == 1 ? gkind : 0;
    Hgfx::gigaAuto = Config::gigascreen_onoff == 2 ? gkind : 0;   // Auto: only while HRC flickers (Hgfx::hrcFlicker)
    Hgfx::gigaOther = MemESP::videoLatch ? a : b;
    Hgfx::radastan = (s_r->u[0x40] & 3) == 3;
    Hgfx::radOffset = (uint16_t)(s_r->u[0x41] | (s_radHi << 8));
    Hgfx::radPad = s_r->u[0x42];
    Hgfx::radBank = s_r->u[0x43] & 3;
    // The other screen was switched in during this frame (#7FFD bit 3): the picture drawn
    // below is that screen's, so is the table. WOWDEMO flips for every picture and each
    // has its own colours; with the table of the frame start every new picture was shown
    // for one frame in the previous one's colours (flicker, owner 2026-11-14).
    if (Hgfx::mode == 2 && VIDEO::grmem != s_hrxcSrc) hrxcLatch();
    Hgfx::hrxcTable = s_r->hrxc;
    Hgfx::frameEnd();
    hrxcLatch();                          // the next frame starts with the table as it is now
    ElmSdma::frameEnd(ESPectrum::samplesPerFrame);
    ElmSid::frameEnd(ESPectrum::samplesPerFrame);
    extraTick();
    if (s_pendingNote[0] && !--s_pendingFrames) { OSD::notify(s_pendingNote, LEVEL_WARN, 5000); s_pendingNote[0] = 0; }
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

bool memWriteCold(uint16_t a, uint8_t v) {
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
        const uint8_t* t2 = hrxcOther() + Hgfx::HRXC_TABLE;
        if ((int)(CPU::tstates >> ESPectrum::multiplicator) < VIDEO::tStatesScreen) {
            if (p >= t && p < t + Hgfx::HRXC_BYTES) s_r->hrxc[p - t] = v;
            else if (p >= t2 && p < t2 + Hgfx::HRXC_BYTES) s_r->hrxc[Hgfx::HRXC_BYTES + (p - t2)] = v;
        }
        return false;
    }
    case W_LCU2:
        ElmLcu::write(s_lcu2, (uint8_t)a, v);
        lcu2Publish();
        return true;
    case W_CHUNKY:
        Hgfx::chunkyWrite(slotPage[a >> 13] * 0x2000u + (a & 0x1FFF), v);
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

// The personality at the reset that is starting: the live register once the session
// is up (the SetUp writes it, then restarts), the FRAM's at power-up.
void armPersonality() {
    uint8_t p = kDefaults[0];
    if (s_powered && s_r) p = s_r->e[0x000];
    else if (ElmFlash::ensure()) {
        const uint8_t* f = ElmFlash::fram();
        if (f[0x000] <= 9 && f[0x001] != 0xFF) p = f[0x000];
    }
    if (p > 9 || p == 2 || p == 4) p = kDefaults[0];   // reserved values: the base machine
    if (g_elm_personality != p) Debug::log("[ELM] personality %02X at reset", p);
    g_elm_personality = p;
}

// What CPU::reset's own branches set for the stock machine of this personality. 08
// (Pentagon 4096) is a 512 on #7FFD plus the extension ports below, 09 (Didaktik Gama 80) a 48K.
void cpuFlags() {
    const uint8_t p = g_elm_personality;
    Z80Ops::isByte = false;
    Z80Ops::isProfi = false;
    Z80Ops::isP3 = (p == 3);
    Z80Ops::is48 = (p == 0 || p == 9);
    Z80Ops::is128 = (p == 1);                 // the +2A is not "128": its own paging and contention
    Z80Ops::isPentagon = (p >= 5);
    Z80Ops::is512 = (p == 6 || p == 8);       // 08: #7FFD bits 6-7 as the 512, the rest from p4kWrite
    Z80Ops::is1024 = (p == 7);
    Ports::getFloatBusData = Z80Ops::is48 ? &Ports::getFloatBusData48
                           : Z80Ops::is128 ? &Ports::getFloatBusData128
                           : &Ports::getFloatBusDataNone;   // +2A: none; Pentagon: none
    ESPectrum::target = Z80Ops::is48 ? MICROS_PER_FRAME_48
                      : (p == 1 || p == 3) ? MICROS_PER_FRAME_128 : MICROS_PER_FRAME_PENTAGON;
}

// Machine > eLeMeNt ZX > SetUp (the board: reset held 1.5 s). Takes effect at the
// reset that follows; a blank block 7 shows what to flash.
bool enterSetup() {
    if (!ElmFlash::ensure()) return false;
    if (ElmFlash::blockBlank(s_r ? s_r->e[0x00F] & 3 : 0, ElmFlash::BLOCK_SETUP)) return false;
    if (s_r) { memcpy(s_preSetup, s_r->e, sizeof s_preSetup); s_preValid = true; }
    s_setupBoot = true;
    return true;
}

// Shift+R / Shift+L in the SetUp: the registers are written, then RST 0. On the board
// that restarts the machine; here the ROM would run on at #0000 into a warm esxDOS
// (divMMC RAM intact, its browser state too) and the personality, which is applied at
// reset only, would not change. Log the state once and ask for a machine reset.
void rst0(uint16_t pc) {
    if (!s_setupLive || !s_r) return;
    s_setupLive = false;
    s_fromSetup = true;
    s_resetWanted = true;
    // What the SetUp SAVED is what it meant: every page stores its pick in the FRAM
    // (#2000 + N), but the live write can miss - SetUp 20260406's "Joystick/DE9
    // connector" page shares the Joystick page's handler (page table D44C: handler C7D7
    // for both), so it writes e_003 live and e_01A to the FRAM; Shift+L only fills the
    // FRAM. The board shows the saved values at its next power-up; here the restart is
    // that power-up (cold esxDOS, reboot-class items), so the registers follow the FRAM
    // now. Before the reset: armPersonality reads e_000 ahead of Elm::reset.
    if (ElmFlash::ensure()) {
        const uint8_t* f = ElmFlash::fram();
        if (f[0x000] <= 9 && f[0x001] != 0xFF) {
            for (int i = 0; i < 0x40; i++)
                if (s_r->e[i] != f[i]) Debug::log("[ELM] SetUp exit: e_%03X live %02X, saved %02X -> saved", i, s_r->e[i], f[i]);
            memcpy(s_r->e, f, 0x40);
            aasramWanted(f[0x038] & 1, "SetUp e_zxi_038");
            divsdWanted(f[0x001], "SetUp e_zxi_001");
            bootClassRegs(f);
        }
    }
    g_elm_nodiv = 1;                    // the rest of this frame: the ROM alone at #0000
    Debug::log("[ELM] SetUp RST 0 at %04X: e_000=%02X e_001=%02X e_00F=%02X e_02D=%02X divmmc map=%d con=%d -> machine reset",
               pc, s_r->e[0x000], s_r->e[0x001], s_r->e[0x00F], s_r->e[0x02D],
               (int)MemESP::divmmc_mapped, (int)DivMMC::conmem);
}
static uint8_t s_resetKind = 1;          // what s_resetWanted asks for: 1 restart, 2 warm
int takeReset() {
    if (!s_resetWanted) return 0;
    s_resetWanted = false;
    const int k = s_resetKind;
    s_resetKind = 1;
    return k;
}

// The MB-02+ card's 512 KB on this machine: butter PSRAM, allocated once, registered with
// the card (MB02::setExternalRam) so MB02::init never takes the URAM strip's pages.
static uint8_t* s_mb02Ram;
static bool mb02Ram() {
    // The divSD's 512 KB (64 banks, DivMMC::banks() on this machine) ARE the MB02+ RAM
    // on the board; with the banks in butter PSRAM they are one contiguous block, so the
    // card simply pages the same bytes. Without them (esxDOS off, or the swap-file
    // path) the card gets a block of its own.
    uint8_t* base = (DivMMC::enabled && DivMMC::use_psram && DivMMC::banks() == 64) ? DivMMC::bank_ptr[0] : nullptr;
    if (base) {
        if (s_mb02Ram != base) Debug::log("[ELM] MB-02+ RAM = the divSD's 512 KB @ %p (shared)", base);
        s_mb02Ram = base;
    } else if (!s_mb02Ram) {
        if (!Buffer::butterPoolReady()) return false;
        s_mb02Ram = (uint8_t*)Buffer::palloc(32u * 16384u, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!s_mb02Ram) { Debug::log("[ELM] no PSRAM for the MB-02+ 512 KB"); return false; }
        Debug::log("[ELM] MB-02+ RAM 512 KB @ %p (butter PSRAM, not the divSD's)", s_mb02Ram);
    }
    MB02::setExternalRam(s_mb02Ram);
    return true;
}

// Reboot-class: the DMA window and the ZiFi NIC are laid out at boot (the SetUp's
// restart reboots the board when either changed - rebootWanted()). Also called from
// rst0, BEFORE the caller asks rebootWanted(): from applyRegs alone (inside the reset
// that follows) the pick came one check too late and the board did not reboot.
static void bootClassRegs(const uint8_t* e) {
    const bool dma = e[0x02E] & 1;
    if ((Config::dma_mode != 0) != dma) { Config::dma_mode = dma ? 1 : 0; Config::save(); Debug::log("[ELM] SetUp e_zxi_02E: DMA %s at the next boot", dma ? "on" : "off"); }
    const bool uart = e[0x039] != 3;
    if ((Config::zifi_enabled != 0) != uart) { Config::zifi_enabled = uart ? 1 : 0; Config::save(); Debug::log("[ELM] SetUp e_zxi_039 = %02X: UART/ZiFi %s at the next boot", e[0x039], uart ? "on" : "off"); }
}

// Phase E: the SetUp's registers -> Config. Runtime only, never Config::save(): the
// SetUp is the Element's own configuration (FRAM), the NVS stays the user's. `live`
// runs the hooks the settings menu would (and the subsystem reconcile); at boot the
// setup() that follows allocates from Config by itself.
static void applyRegs(const uint8_t* e, bool live) {
    char what[112]; int n = 0; bool subsys = false;
    auto note = [&](const char* s) { if (n < (int)sizeof(what) - 12) n += snprintf(what + n, sizeof(what) - n, "%s ", s); };
    static const uint8_t kJoy[4] = { JOY_NONE, JOY_KEMPSTON, JOY_SINCLAIR2, JOY_SINCLAIR1 };   // 04 = the game's keys
    if (e[0x003] < 4 && Config::joystick != kJoy[e[0x003]]) { Config::joystick = kJoy[e[0x003]]; note("joystick"); }
    const bool timex = e[0x010] & 1;
    if (Config::timex_video != timex) {
        Config::timex_video = timex; note(timex ? "timex" : "timex-off");
        if (live && !timex) { VIDEO::timex_port_ff = 0; VIDEO::timex_mode = 0; VIDEO::timex_hires_ink = 0; }
    }
    const bool ulap = e[0x011] & 1;
    if (Config::ulaplus != ulap) {
        Config::ulaplus = ulap; note(ulap ? "ulaplus" : "ulaplus-off");
        if (live && !ulap && VIDEO::ulaplus_enabled) VIDEO::ulaPlusDisable();
    }
    const uint8_t gs = e[0x01D] == 0 ? 0 : e[0x01D] == 3 ? 2 : 1;   // 01/02 = on, 03 = auto detect
    if (Config::gigascreen_onoff != gs) { Config::gigascreen_onoff = gs; subsys = true; note(gs ? "gigascreen" : "gigascreen-off"); }
    const bool tsfm = e[0x002] & 2;
    if ((Config::tsfm != 0) != tsfm) { Config::tsfm = tsfm ? 1 : 0; subsys = true; note(tsfm ? "tsfm" : "tsfm-off"); }
    const bool saa = e[0x002] & 0x10;
    if (Config::SAA1099 != saa) { Config::SAA1099 = saa; subsys = true; note(saa ? "saa" : "saa-off"); }
    // Z-Controller: on the board a second SD interface beside the divSD; here its own
    // card (a window of the one SD card, or an image — Devices > Z-Controller) on
    // DivMMC's SPI machine, so it runs beside esxDOS (DivMMC.h, since 2026-11-03).
    const bool zc = e[0x02C] != 0;
    if (Config::zcontroller != zc) {
        Config::zcontroller = zc; note(zc ? "zc" : "zc-off");
        if (live) { if (zc) DivMMC::zc_init(); else DivMMC::zc_shutdown(); }
    }
    // divSD + MB02+ (e_001 = 02): the MB-02+ card beside esxDOS, its 512 KB from butter
    // PSRAM (mb02Ram), never the URAM strip. 00/01/03 = off.
    if (e[0x001] <= 3) {
        const bool mb = e[0x001] == 2;
        if (Config::mb02 != (mb ? 1 : 0)) {
            if (mb && !mb02Ram()) note("mb02-no-psram");
            else { Config::mb02 = mb ? 1 : 0; subsys = true; note(mb ? "mb02" : "mb02-off"); }
        }
    }
    // MonsterBlaster (bit 2) = the DAC ports here (#0F..#5F, dacWrite) AND the Covox
    // subsystem of the emulator; the SetUp's bit decides both.
    const bool covox = e[0x002] & 4;
    if ((Config::covox != 0) != covox) { Config::covox = covox ? 1 : 0; subsys = true; note(covox ? "covox" : "covox-off"); }
    // TSFM stereo: bit 1 = 0 ACB, 1 ABC (our ayConfig 1 / 0); the other layouts stay.
    if (Config::ayConfig == 0 || Config::ayConfig == 1) {
        const uint8_t ayc = (e[0x018] & 2) ? 0 : 1;
        if (Config::ayConfig != ayc) { Config::ayConfig = ayc; note(ayc ? "acb" : "abc"); }
    }
    // CRT/TV effect 1/2 .. 1/128 -> our four scan-line strengths (1/2 = darkest).
    const uint8_t sl = e[0x01E] == 0 ? 0 : e[0x01E] > 4 ? 4 : e[0x01E];
    if (Config::scanlines != sl) {
        Config::scanlines = sl; note("scanlines");
        if (live) graphics_set_scanlines(sl);
    }
    // RTC time zone: GMT+N, two's complement for the west.
    const signed char tz = (signed char)e[0x03E];
    if (tz >= -12 && tz <= 14 && Config::wifi_tz != tz) { Config::wifi_tz = tz; note("tz"); }
    if (n) Debug::log("[ELM] SetUp -> settings%s: %s", live ? "" : " (boot)", what);
    if (live && subsys) nm::Stage::reconcileQuiet();
    bootClassRegs(e);
}

// What the emulator cannot do that the SetUp offers: told once at the SetUp's exit, and
// the register (and FRAM) goes back to what it was before the session, so the SetUp
// keeps showing the truth rather than a wish (owner, 2026-11-04).
static void framPut(uint8_t i, uint8_t v) {
    uint8_t* f = ElmFlash::ensure() ? ElmFlash::fram() : nullptr;
    if (!f || f[i] == v) return;
    f[i] = v;
    ElmFlash::framDirty();
}
static void validateRegs() {
    const uint8_t* prev = s_preValid ? s_preSetup : kDefaults;
    char what[160]; int n = 0;
    // Told only when the pick was made in this session: a value the SetUp's own defaults
    // table carries (legacy DVI = 1) is put right without a word.
    auto revert = [&](uint8_t i, uint8_t v, const char* why) {
        if (s_r->e[i] == v) return;
        const bool picked = s_r->e[i] != prev[i] && s_r->e[i] != kDefaults[i];
        s_r->e[i] = v; framPut(i, v);
        if (!picked) return;
        if (n < (int)sizeof(what) - 24) n += snprintf(what + n, sizeof(what) - n, "%s%s", n ? ", " : "", why);
    };
    uint8_t* e = s_r->e;
    if (e[0x000] == 2 || e[0x000] == 4 || e[0x000] > 9) revert(0x000, prev[0x000] <= 9 && prev[0x000] != 2 && prev[0x000] != 4 ? prev[0x000] : kDefaults[0], "machine");
    if (!(e[0x002] & 1)) revert(0x002, (uint8_t)(e[0x002] | 1), "ULA audio off");
    if (e[0x003] > 3) revert(0x003, prev[0x003] <= 3 ? prev[0x003] : 1, "keyboard joystick");
    if (e[0x01A] != 0) revert(0x01A, 0, e[0x01A] == 1 ? "Sega pad" : "DE9 UART");
    if (e[0x01F] != 0) revert(0x01F, 0, "DVI");
    if (e[0x02C] == 2) revert(0x02C, 1, "SD slot 2");             // the Z-Controller's card is set in Devices
    if (e[0x02D] & 2) revert(0x02D, (uint8_t)(e[0x02D] & ~2), "SD swap");
    if (e[0x030] == 3 || e[0x030] == 4) revert(0x030, 2, "20/22 MHz");   // 14 or 28 MHz here
    if (e[0x030] == 7) revert(0x030, 6, "30 MHz");
    if (e[0x03F] != 0) revert(0x03F, 0, "Diag");
    if (!n) return;
    Debug::log("[ELM] SetUp: not supported here, kept as before: %s", what);
    snprintf(s_pendingNote, sizeof s_pendingNote, "SetUp: not here: %s", what);
    s_pendingFrames = 75;
}

void bootSettings() {
    if (Config::arch != A_ELM) return;
    if (!ElmFlash::ensure()) { Debug::log("[ELM] boot settings: no flash image / FRAM"); return; }
    uint8_t* f = ElmFlash::fram();
    if (Config::elm_pers_req <= 9 && f[0x001] != 0xFF) {   // Machine > eLeMeNt ZX picked while another machine ran
        Debug::log("[ELM] boot: personality %02X from the menu -> FRAM", Config::elm_pers_req);
        f[0x000] = Config::elm_pers_req;
        ElmFlash::framDirty();
    }
    if (Config::elm_pers_req != 0xFF) { Config::elm_pers_req = 0xFF; Config::save(); }
    // The options this firmware started sharing with the SetUp on 2026-11-04 (Covox, AY
    // stereo, scan lines, DMA, time zone, UART) come from the NVS the first time a FRAM
    // is seen: a SetUp table nobody edited must not switch the NIC on or move the clock.
    // Marked in the FRAM's counter area; from then on the FRAM is the one source.
    if (f[0x001] != 0xFF && f[0x1FE8] != 0xA5) {
        f[0x002] = (uint8_t)((f[0x002] & ~4) | (Config::covox ? 4 : 0));
        if (Config::ayConfig == 0 || Config::ayConfig == 1) f[0x018] = (uint8_t)((f[0x018] & ~2) | (Config::ayConfig == 0 ? 2 : 0));
        f[0x01E] = Config::scanlines > 4 ? 4 : Config::scanlines;
        f[0x02E] = (uint8_t)((f[0x02E] & ~1) | (Config::dma_mode ? 1 : 0));
        f[0x03E] = (uint8_t)Config::wifi_tz;
        f[0x039] = Config::zifi_enabled ? (f[0x039] == 3 ? 0 : f[0x039]) : 3;
        f[0x1FE8] = 0xA5;
        ElmFlash::framDirty();
        Debug::log("[ELM] FRAM seeded from the settings once (covox/ay/scanlines/dma/tz/uart)");
    }
    const uint8_t* e = f[0x001] != 0xFF ? f : kDefaults;
    Debug::log("[ELM] boot settings from %s: e_002=%02X e_003=%02X e_010=%02X e_011=%02X e_01D=%02X e_02C=%02X (config: joy %u timex %d ulap %d gs %u tsfm %u saa %d zc %d)",
               e == f ? "FRAM" : "defaults", e[0x002], e[0x003], e[0x010], e[0x011], e[0x01D], e[0x02C],
               Config::joystick, (int)Config::timex_video, (int)Config::ulaplus, Config::gigascreen_onoff, Config::tsfm, (int)Config::SAA1099, (int)Config::zcontroller);
    applyRegs(e, false);
    if (Config::mb02) MB02::init();      // setup()'s own MB02::init ran before the pools and refused here
    aasramWanted(e[0x038] & 1, "FRAM e_zxi_038");
    divsdWanted(e[0x001], "FRAM e_zxi_001");
    s_dmaBoot = Config::dma_mode != 0;
    s_zifiBoot = Config::zifi_enabled != 0;
    Debug::log("[ELM] URAM %u MB (%s), personality %02X at power-up", (unsigned)(MEM_PG_CNT / 64), aasram() ? "AA-SRAM" : "no AA-SRAM", e[0x000]);
}

// e_zxi_038 bit 0 = the AA-SRAM 2 MB pack: the page strip is laid out at boot, so the
// pick goes to the NVS and the next boot (the SetUp's restart reboots the board when
// the strip has to change - Elm::takeReset's caller).
static void aasramWanted(bool want, const char* who) {
    if (Config::elm_aasram == want) return;
    Config::elm_aasram = want;
    Config::save();
    Debug::log("[ELM] %s: AA-SRAM %s -> URAM %u MB at the next boot (now %u MB)", who, want ? "on" : "off",
               want ? 4u : 2u, (unsigned)(MEM_PG_CNT / 64));
}
// e_zxi_001: 00 none and 03 "SD only" run without the divSD memory interface (no esxDOS),
// 01 divSD and 02 divSD+MB02+ with it; FE (the flashers' rescue mode) and FF (obsolete)
// change nothing. esxDOS is boot-class here (its ROM, RAM and SD come up in setup()),
// so the pick goes to the NVS like the AA-SRAM and the SetUp's restart reboots.
static void divsdWanted(uint8_t e001, const char* who) {
    if (e001 != 0 && e001 != 1 && e001 != 2 && e001 != 3) return;
    const bool want = e001 == 1 || e001 == 2;
    if ((Config::esxdos != 0) == want) return;
    Config::esxdos = want ? 3 : 0;      // 3 = DivSD over the raw card, the Element's mode
    Config::save();
    Debug::log("[ELM] %s = %02X: esxDOS (divSD) %s at the next boot", who, e001, want ? "on" : "off");
}
bool rebootWanted() {
    return Config::wantedPages(A_ELM) != MEM_PG_CNT || (Config::esxdos != 0) != DivMMC::enabled ||
           (Config::dma_mode != 0) != s_dmaBoot || (Config::zifi_enabled != 0) != s_zifiBoot;
}

uint8_t personality() {
    if (s_r) return s_r->e[0x000];
    if (ElmFlash::ensure()) { const uint8_t* f = ElmFlash::fram(); if (f[0x001] != 0xFF && f[0x000] <= 9) return f[0x000]; }
    return kDefaults[0];
}

void setPersonality(uint8_t p) {
    if (!s_r || p > 9 || p == 2 || p == 4) return;
    Debug::log("[ELM] menu: personality %02X -> e_zxi_000 + FRAM, restart", p);
    eWrite(0x00, p);
    framPut(0x000, p);
    s_resetKind = 1; s_resetWanted = true;
}

void syncFromSettings() {
    if (!s_r) return;
    uint8_t w[0x40];
    memcpy(w, s_r->e, sizeof w);
    switch (Config::joystick) {                        // Cursor / Fuller / custom have no SetUp value: it stays
        case JOY_NONE:      w[0x003] = 0; break;
        case JOY_KEMPSTON:  w[0x003] = 1; break;
        case JOY_SINCLAIR2: w[0x003] = 2; break;
        case JOY_SINCLAIR1: w[0x003] = 3; break;
        default: break;
    }
    w[0x010] = (uint8_t)((w[0x010] & ~1) | (Config::timex_video ? 1 : 0));
    w[0x011] = (uint8_t)((w[0x011] & ~1) | (Config::ulaplus ? 1 : 0));
    w[0x01D] = Config::gigascreen_onoff == 0 ? 0 : Config::gigascreen_onoff == 2 ? 3 : (w[0x01D] == 1 || w[0x01D] == 2 ? w[0x01D] : 1);
    uint8_t a = w[0x002];
    a = (uint8_t)((a & ~2) | (Config::tsfm ? 2 : 0));
    a = (uint8_t)((a & ~4) | (Config::covox ? 4 : 0));
    a = (uint8_t)((a & ~0x10) | (Config::SAA1099 ? 0x10 : 0));
    w[0x002] = a;
    w[0x02C] = Config::zcontroller ? (w[0x02C] ? w[0x02C] : 1) : 0;
    if (Config::esxdos == 0) { if (w[0x001] == 1 || w[0x001] == 2) w[0x001] = 0; }
    else if (w[0x001] <= 3) w[0x001] = Config::mb02 ? 2 : 1;
    w[0x038] = (uint8_t)((w[0x038] & ~1) | (Config::elm_aasram ? 1 : 0));
    if (Config::ayConfig == 0 || Config::ayConfig == 1)
        w[0x018] = (uint8_t)((w[0x018] & ~2) | (Config::ayConfig == 0 ? 2 : 0));
    if (Config::scanlines == 0 || w[0x01E] <= 4) w[0x01E] = Config::scanlines > 4 ? 4 : Config::scanlines;
    w[0x02E] = (uint8_t)((w[0x02E] & ~1) | (Config::dma_mode ? 1 : 0));
    w[0x03E] = (uint8_t)Config::wifi_tz;
    w[0x039] = Config::zifi_enabled ? (w[0x039] == 3 ? 0 : w[0x039]) : 3;
    char what[120]; int n = 0;
    for (int i = 0; i < 0x40; i++) {
        if (w[i] == s_r->e[i]) continue;
        if (n < (int)sizeof(what) - 12) n += snprintf(what + n, sizeof(what) - n, "e_%03X=%02X ", i, w[i]);
        eWrite((uint8_t)i, w[i]);
        framPut((uint8_t)i, w[i]);
    }
    if (n) Debug::log("[ELM] settings -> SetUp: %s", what);
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
    if (!s_r) s_r = (Regs*)tailAlloc(sizeof(Regs));
    if (!s_r) { Debug::log("[ELM] no heap for the ZXi registers"); return; }
    if (s_powered) memcpy(s_keep, s_r->e, sizeof s_keep);
    memset(s_r, 0, sizeof(Regs));
    wrKind = s_r->win;
    vramHi = &s_r->g[0x021];
    // Memory-register windows at their documented defaults.
    s_zone = 0;
    memset(s_z, 0, sizeof(s_z));
    s_active = s_wpSlots = s_lcuSlots = s_hgfxSlots = s_flashSlots = s_framSlots = 0;
    s_gamaPage = 0;
    s_p4k1FFD = s_p4kDFFD = s_p4kFDFD = s_p4k7FFD = 0;
    if (s_flashOpen) { s_flashOpen = false; ElmFlash::windowClosed(); }
    g_elm_nodiv = 0;        // the reset goes through esxDOS at #0000 again
    Hgfx::reset();
    s_rdArmed = false;
    Hgfx::direct(false, true);
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
    s_setupLive = s_setupRom;
    s_resetWanted = false;
    if (s_setupRom) Debug::log("[ELM] SetUp ROM (block 7 of Area %u) boots at this reset", s_r->e[0x00F] & 3);
    cpuSpeed(s_r->e[0x030]);
    ElmSid::mute(!(s_r->e[0x002] & 8));
    g_elm_ff_off = !(s_r->e[0x010] & 2);
    ElmSid::setParams(s_r->e[0x019] & 3);   // the SetUp's model / clock outlives a reset like the register
    if (s_fromSetup) { s_fromSetup = false; if (s_running) validateRegs(); }
    if (s_running) applyRegs(s_r->e, true);   // the power-up values went in through bootSettings()
    s_r->e[0x0FC] = kStampDay;
    s_r->e[0x0FD] = kStampMonth;
    s_r->e[0x0FE] = kStampYear;
    s_r->e[0x0FF] = kInterfaceId;
    romsBind();
    divRomApply();
    windowsRebuild();       // no window yet; the ROM write gate on window 0
    Hgfx::arenaEnsure();    // the HGFX SRAM block, while the heap is still whole (once a session)
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
    if (r == 0x10) g_elm_ff_off = !(v & 2);
    if (r == 0x38) aasramWanted(v & 1, "e_zxi_038");
    if (r == 0x01) divsdWanted(v, "e_zxi_001");
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
    if (!(s_r->e[0x002] & 4)) return;      // SetUp: MonsterBlaster off
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

// e_zxi_030 / u_zxi_00B: the CPU clock. 3.5 / 7 / 14 by the multiplicator; 20 / 22 MHz on
// the 28 MHz frame with every T stretched, the OverClock modes at 28 with the ROM wait on 05 /
// 07 (Element.h; owner 2026-11-18). WOWDEMO and PF2026 write 03 then 05.
static void cpuSpeed(uint8_t sel, bool quiet) {
    sel &= 7;
    const uint8_t want = sel >= 3 ? 3 : sel;
    const uint32_t k = sel == 3 ? 26214u : sel == 4 ? 17873u : 0u;   // 28/20 - 1, 28/22 - 1 in 16.16
    const uint8_t wait = (sel == 5 || sel == 7) ? 1 : 0;
    const uint8_t old = ESPectrum::multiplicator;
    const bool same = want == old && k == g_elm_stretchK && wait == g_elm_romWait;
    if (k != g_elm_stretchK) { g_elm_stretchFrac = 0; g_elm_stretchDebt = 0; g_elm_shT = CPU::tstates; }
    g_elm_stretchK = k;
    if (wait != g_elm_romWait) {
        g_elm_romWait = wait;
        s_romWaitSlots = wait ? 0x03 : 0;
        g_elm_split = s_chunkSplit | s_romWaitSlots;
        elmMasksUpdate();
    }
    if (same) return;
    if (want != old) {
        CPU::tstates = want > old ? CPU::tstates << (want - old) : CPU::tstates >> (old - want);
        ESPectrum::multiplicator = want;
        CPU::updateStatesInFrame();
    }
    if (quiet) return;
    static const char* const kClock[8] = { " CPU: 3.5 MHz ", " CPU: 7 MHz ", " CPU: 14 MHz ", " CPU: 20 MHz ",
                                           " CPU: 22 MHz ", " CPU: 28 MHz ", " CPU: 28 MHz ", " CPU: 28 MHz " };
    OSD::notifyClock(kClock[sel]);
}

void leave() {
    Hgfx::arenaFree();
    g_elm_split = s_chunkSplit = s_romWaitSlots = 0;
    g_elm_romWait = 0;
    g_elm_stretchK = g_elm_stretchFrac = g_elm_stretchDebt = 0;
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
    // A new page for a Zone already mapped as plain URAM, its write protect unchanged:
    // only that slot's pointer moves (PF2026 pages URAM tens of thousands of times a
    // second). Anything else - a layout or protect change, a page with no memory - goes
    // through the full remap.
    if (lo != 0x53 && (s_active >> s_zone) & 1 && (z.par >> 4) == LAYOUT_RAM && (s_r->g[0x000] & 1) && uramMode()) {
        const uint8_t a = (lo == 0x47 || lo == 0x67) ? 1 : 0, slot = s_zone * 2 + a;
        const bool wp = !(z.par & (1u << a));
        uint8_t* p = uramPtr(z.page[a]);
        if (p && ((s_wpSlots >> slot) & 1) == wp) {
            MemESP::ramCurrent[slot] = p;
            mem_desc_t::bank_dirty[slot] = &mem_desc_t::dirty_sink;
            return true;
        }
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
    // A 48K personality has no RAM paging, but the ROM Area is still four blocks and
    // esxDOS reaches 48 BASIC by OUT #7FFD,#10 (FLASH48 fills all four blocks, so a
    // 48 set works whichever one the latch picks): bit 4 selects the block, the rest
    // of the write is dropped. The generic decode ignores #7FFD entirely on a 48K.
    if (g_elm_personality == 8 && (uint8_t)a == 0xFD && p4kOut(a >> 8, v)) return true;
    if (a == 0x7FFD) {
        if (!Z80Ops::is48) return false;
        MemESP::romLatch = (v >> 4) & 1;
        MemESP::romInUse = MemESP::romLatch;
        MemESP::recoverPage0();
        return true;
    }
    if (gamaOut((uint8_t)a, v)) return true;   // Gama 80: #7F / #5F page the upper 32 KB
    // divSD + MB02+: the WD2797 sits on #0F/#2F/#4F/#6F, the same low bytes as the
    // SounDrive DACs - with the card on, those writes are the floppy controller's
    // (hw 2026-11-04: BS-DOS's Restore/Read never reached the WD, "insert your system
    // disk" forever). The board's FPGA decides the same way by e_zxi_001 = 02.
    if (MB02::enabled && (((uint8_t)a & 0x9F) == 0x0F)) return false;
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
    if (a == 0xEFF7) {
        VIDEO::timexHiresRequest(v & 2);
        // e_zxi_032 bit 1 "enable Pentagon 1024 v2.2 turbo", personality 07: #EFF7 D4 is the
        // 1024SL's turbo switch (pentevo io.cpp: turbo(pEFF7 & 0x10 ? 1 : 2)) - 1 = 3.5 MHz,
        // 0 = turbo: the SetUp's CPU speed, 7 MHz when that is 3.5. Level, not edge, as
        // on the 1024SL. The generic handler's Alt+F2 policy is off on this machine
        // (Pentagon::eff7Paging). No toast: TheLink flips it around beam-raced effects.
        if (g_elm_personality == 7 && (s_r->e[0x032] & 2) && !MemESP::pagingLock)
            cpuSpeed((v & 0x10) ? 0 : (s_r->e[0x030] ? s_r->e[0x030] : 1), true);
    }                                                    // and on to the generic decode
    return false;
}

// ── the EXTRA and RESET buttons ────────────────────────────────────────────────
// EXTRA = Win+F11 (User Manual: "ZX + F11"), RESET = the hard-reset hot key. The
// board holds the CPU while a button is down (CPU::paused here, so the frame stays
// and the digits never reach the ZX keyboard). e_zxi_020 = what a short EXTRA press
// does, e_zxi_021 = 01 arms the Spectrum menu on a 1.5 s hold; RESET held 1.5 s =
// the SetUp firmware. The live registers change, not the FRAM: the SetUp is where
// the choice is stored.
static uint64_t s_extraT0, s_rstT0;
static bool s_extraHeld, s_extraMenu, s_rstHeld;
static uint8_t s_gsLast = 3;            // the GigaScreen mode a short press toggles back to
static const char* const kPersName[10] = {
    "ZX Spectrum 48", "ZX Spectrum 128/+2", nullptr, "ZX Spectrum +2A", nullptr,
    "Pentagon 128", "Pentagon 512", "Pentagon 1024", "Pentagon 4096", "Didaktik Gama 80" };
static const char* const kSpeedName[8] = {
    // At most 8 characters (owner 2026-11-18): the clock banner, the EXTRA notice, the fast
    // menu's rows. 05 = OverClock 28 with the ROM wait, 06 / 07 = the SetUp's "(RAM)" ones.
    "3.5 MHz", "7 MHz", "14 MHz", "20 MHz",
    "22 MHz", "28 MHz", "28M RAM", "30M RAM" };

uint8_t speed() { return s_r ? (uint8_t)(s_r->e[0x030] & 7) : ESPectrum::multiplicator; }
const char* speedName(uint8_t sel) { return kSpeedName[sel & 7]; }
void setSpeed(uint8_t sel) {
    sel &= 7;
    if (s_r) {
        s_r->e[0x030] = sel;
        framPut(0x030, sel);   // the SetUp's page and the next power-up follow
    }
    cpuSpeed(sel, true);
    ESPectrum::multUser = ESPectrum::multiplicator;
    Config::turbo = ESPectrum::multUser;
    Config::save();
    char m[40];
    snprintf(m, sizeof m, " CPU: %s ", kSpeedName[sel]);
    OSD::notifyClock(m, true);   // a keypress always answers (OSD::setTurbo)
    Debug::log("[ELM] hot key: CPU speed -> e_030=%02X", sel);
}

static void extraShort() {
    char m[64];
    switch (s_r->e[0x020]) {
    case 1: {                                                   // CPU speed: 3.5 -> 7 -> 14 -> 20
        const uint8_t v = (uint8_t)((s_r->e[0x030] + 1) & 3);
        eWrite(0x30, v);
        snprintf(m, sizeof m, "EXTRA: CPU %s", kSpeedName[v]);
        break;
    }
    case 2: {                                                   // Machine: the next personality, restart
        static const uint8_t kOrder[8] = { 0, 1, 3, 5, 6, 7, 8, 9 };
        int i = 0;
        while (i < 8 && kOrder[i] != s_r->e[0x000]) i++;
        const uint8_t v = kOrder[(i + 1) % 8];
        eWrite(0x00, v);
        s_resetKind = 1; s_resetWanted = true;
        snprintf(m, sizeof m, "EXTRA: %s", kPersName[v]);
        break;
    }
    case 3: {                                                   // GigaScreen on/off (back to the last mode)
        const uint8_t cur = s_r->e[0x01D];
        if (cur) s_gsLast = cur;
        const uint8_t v = cur ? 0 : s_gsLast;
        eWrite(0x1D, v);
        applyRegs(s_r->e, true);
        snprintf(m, sizeof m, "EXTRA: GigaScreen %s", v == 0 ? "off" : v == 3 ? "auto" : "on");
        break;
    }
    case 4:                                                     // Warm reset: RAM and the divSD stay
        s_resetKind = 2; s_resetWanted = true;
        snprintf(m, sizeof m, "EXTRA: warm reset");
        break;
    case 5: {                                                   // Joystick interface: the next one
        static const char* const kJoy[5] = { "none", "Kempston", "Sinclair left", "Sinclair right", "keyboard" };
        const uint8_t v = (uint8_t)((s_r->e[0x003] + 1) % 5);
        eWrite(0x03, v);
        applyRegs(s_r->e, true);
        snprintf(m, sizeof m, "EXTRA: joystick %s", kJoy[v]);
        break;
    }
    default: return;                                            // 00 none
    }
    Debug::log("[ELM] %s", m);
    OSD::notify(m, LEVEL_INFO, 1500);
}

// The speccy rainbow of the Spectrum menu: four slanted stripes in the bottom border,
// painted straight into the framebuffer every frame of the hold (the CPU is held, so
// nothing else repaints the border; the release asks for a border repaint). Only the
// standard border geometry: the HGFX / big modes keep the toast alone.
static void drawRainbow() {
    if (VIDEO::osd_in_paper || VIDEO::bandBorderMode() || VIDEO::ds80BandMode()) return;
    const int top = VIDEO::isFullBorder288() ? 48 : 24;
    const int y0 = top + 192, h = (int)OSD::scrH - y0;
    const int w = (int)OSD::scrW;
    if (h <= 0 || w < 128) return;
    static const uint8_t kInk[4] = { 2, 6, 4, 5 };            // red, yellow, green, cyan
    for (int r = 0; r < h; r++) {
        int x = w - 40 - r;                                   // the "/" slant of the logo
        for (int k = 0; k < 4; k++, x += 8) {
            const int xa = x < 0 ? 0 : x, xb = x + 8 > w ? w : x + 8;
            if (xb > xa) VIDEO::vga.fillRect(xa, y0 + r, xb - xa, 1, zxColor(kInk[k], 1));
        }
    }
}

static void buttonRelease() {
    CPU::paused = false;
    VIDEO::brdChange = true;                                  // repaint the border (the rainbow)
    VIDEO::brdnextframe = true;
}

static void extraTick() {
    if (!s_extraHeld) return;
    if (s_extraMenu) { drawRainbow(); return; }
    if (s_r->e[0x021] != 1 || time_us_64() - s_extraT0 < 1500000ull) return;
    s_extraMenu = true;
    Debug::log("[ELM] EXTRA held: Spectrum menu");
    OSD::notify("Spectrum menu: 1-8 = CPU speed", LEVEL_INFO, 30000);
}

int extraKey(uint16_t vk, bool down, bool gui, bool plain, uint16_t resetVk) {
    if (!s_r) return EK_NONE;
    if (vk == fabgl::VK_F11 && gui && !s_rstHeld) {           // EXTRA
        if (down) {
            if (!s_extraHeld) {
                s_extraHeld = true; s_extraMenu = false; s_extraT0 = time_us_64(); CPU::paused = true;
                framCount(0x1FF0);                                // the SetUp's "Extra" counter
            }
            return EK_TAKEN;
        }
    }
    if (vk == fabgl::VK_F11 && !down && s_extraHeld) {
        s_extraHeld = false;
        buttonRelease();
        if (s_extraMenu) { s_extraMenu = false; OSD::cancelNotify(); }
        else extraShort();
        return EK_TAKEN;
    }
    if (vk == resetVk && plain && !gui && !s_extraHeld) {     // RESET: short = reset, 1.5 s = SetUp
        if (down) {
            if (!s_rstHeld) { s_rstHeld = true; s_rstT0 = time_us_64(); CPU::paused = true; }
            return EK_TAKEN;
        }
        if (!s_rstHeld) return EK_NONE;
        s_rstHeld = false;
        buttonRelease();
        const bool setup = time_us_64() - s_rstT0 >= 1500000ull;
        Debug::log("[ELM] reset button %s", setup ? "held: SetUp" : "released: reset");
        return setup ? EK_SETUP : EK_RESET;
    }
    if (!s_extraHeld || !down) return EK_NONE;
    int n = -1;
    if (vk >= fabgl::VK_1 && vk <= fabgl::VK_8) n = vk - fabgl::VK_1;
    else if (vk >= fabgl::VK_KP_1 && vk <= fabgl::VK_KP_8) n = vk - fabgl::VK_KP_1;
    if (n < 0) return EK_NONE;
    if (!s_extraMenu) return EK_TAKEN;                          // the CPU is held: the key goes nowhere
    if (n >= 4 && !(s_r->e[0x031] & 1)) return EK_TAKEN;        // 5-8 need OverClock enabled
    eWrite(0x30, (uint8_t)n);
    char m[48];
    snprintf(m, sizeof m, "CPU speed: %s", kSpeedName[n]);
    Debug::log("[ELM] Spectrum menu: %s", m);
    OSD::notify(m, LEVEL_INFO, 1500);
    return EK_TAKEN;
}

}
