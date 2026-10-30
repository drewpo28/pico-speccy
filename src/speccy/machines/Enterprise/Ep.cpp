// pico-speccy — Enterprise 64/128 glue. See Ep.h for the model and EpCore.h for
// the chips. FLASH code: the shared RAM-resident paths keep one test that sends an
// Enterprise access here.

#include "Ep.h"
#include "EpCore.h"

#include <string.h>

#include "app/Config.h"
#include "app/Buffer.h"
#include "app/Debug.h"
#include "speccy/core/MemESP.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/MemGates.h"   // g_atm_ro: bit n = drop writes to CPU window n
#include "speccy/video/Video.h"
#include "speccy/roms/ep/ep_roms.h"

namespace Ep {

uint8_t keys[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static EpCore::Nick s_nick;
static EpCore::Dave s_dave;
static const uint8_t* s_exos = nullptr;     // 32K: segments 0, 1
static const uint8_t* s_basic = nullptr;    // 16K: segment 4
static const uint8_t* s_fileio = nullptr;   // 16K: segment 0x10 (FILE:, EpFile.cpp)
static bool     s_logoLine = false;         // this frame showed a 256-colour PIXEL line
static int      s_logoFrames = 0;
static bool     s_ep64 = false;             // RAM = FC..FF only
static uint8_t* s_ff = nullptr;             // a 16K page of 0xFF for absent segments
static uint8_t  s_ro = 0, s_vram = 0;       // windows read-only / showing VRAM
static uint32_t s_lineT = Ep::T_PER_LINE;   // frame T of the next line end
static uint32_t s_frac = 0;                 // wait states: carried 1/57 T
static int      s_row = 0;                  // lines since the end of VSYNC
static bool     s_prevVs = false;

void bindRoms() {
    s_ep64 = (Config::romSet == R_EP64);
    s_exos  = s_ep64 ? gb_rom_ep_exos20 : gb_rom_ep_exos21;
    s_basic = s_ep64 ? gb_rom_ep_basic20 : gb_rom_ep_basic21;
    s_fileio = gb_rom_ep_fileio;
}

// ---------------------------------------------------------------------------
// Memory

static const uint8_t* ffPage() {
    if (!s_ff) {
        s_ff = (uint8_t*)Buffer::palloc(0x4000, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (s_ff) memset(s_ff, 0xFF, 0x4000);
    }
    return s_ff ? s_ff : s_exos;                // no memory for it: show ROM, still read-only
}

static void mapRo(int w, const uint8_t* p) {
    MemESP::plugPtr16(w, (uint8_t*)p);
    mem_desc_t::bank_dirty[w * 2] = &mem_desc_t::dirty_sink;
    mem_desc_t::bank_dirty[w * 2 + 1] = &mem_desc_t::dirty_sink;
    s_ro |= (uint8_t)(1u << w);
}

bool segIsRam(uint8_t seg) { return seg >= 0xF8 && !(s_ep64 && seg < 0xFC); }

uint8_t* segPtr(uint8_t seg) {
    if (segIsRam(seg)) return MemESP::ram[seg - 0xF8].direct();
    if (seg <= 1)      return (uint8_t*)(s_exos + (seg << 14));
    if (seg == 4)      return (uint8_t*)s_basic;
    if (seg == 0x10)   return (uint8_t*)s_fileio;
    return nullptr;
}

bool pcInFileRom(uint16_t pc) { return s_dave.page[pc >> 14] == 0x10; }

int logoFrames() { return s_logoFrames; }

static void mapWin(int w, uint8_t seg) {
    const uint8_t bit = (uint8_t)(1u << w);
    s_ro &= (uint8_t)~bit;
    s_vram &= (uint8_t)~bit;
    if (segIsRam(seg)) {
        MemESP::plugPtr16(w, MemESP::ram[seg - 0xF8].sync(w));
        if (seg >= 0xFC) s_vram |= bit;
    } else if (const uint8_t* p = segPtr(seg)) mapRo(w, p);
    else                  mapRo(w, ffPage());
    MemESP::ramContended[w * 2] = MemESP::ramContended[w * 2 + 1] = (s_vram & bit) != 0;
    g_atm_ro = s_ro;
}

static void pageCb(void*, int w, uint8_t seg) { mapWin(w, seg); }
static void irqCb(void*, bool st) { s_dave.setInt1(st); }

// ---------------------------------------------------------------------------
// Video

static inline uint8_t q7(uint8_t c) { return (uint8_t)((c & 0x3F) | ((c >> 1) & 0x40)); }

uint32_t slotRgb(uint8_t i) {
    // Back to a Nick colour: red's two upper bits from bits 0 and 3 (i bit 0, 3),
    // its dropped LSB set whenever red is on at all (so pure black stays black).
    uint8_t c = (uint8_t)((i & 0x3F) | ((i & 0x40) << 1));
    if (c & 0x09) c |= 0x40;
    return EpCore::colourRgb(c);
}

static void lineOut(const uint8_t* px) {
    auto& vga = VIDEO::vga;
    if (!vga.frameBuffer) return;
    const int xres = (int)vga.xres, yres = (int)vga.yres;
    // The EXOS picture: 27 text rows x 9 = 243 lines from line 38 after VSYNC (the
    // first is its status line), 40 slots from slot 11. A 240-row fb starts at the
    // picture's first line (the last 3, blank in a 9-line character row, fall off);
    // a 288-row one shows 24 lines of border above it.
    int top = 38 - (yres - 240) / 2;
    if (top < 0) top = 0;
    const int y = s_row - top;
    if (y < 0 || y >= yres) return;
    uint8_t* row = vga.frameBuffer[y];
    if (!row) return;
    int x0 = (EpCore::Nick::OUT_PX - xres) / 2, n = xres;
    int dx = 0;
    if (x0 < 0) { dx = -x0; x0 = 0; n = EpCore::Nick::OUT_PX; }
    // Overlays drawn after EndFrame (F8 stats, volume box, LED panel, notify banner)
    // own their rects: leave those bytes alone.
    int cx[8][2];
    const int nc = VIDEO::rowCarves(y, cx);
    for (int i = 0; i < n; i++) {
        const int x = dx + i;
        bool carved = false;
        for (int k = 0; k < nc && !carved; k++) carved = x >= cx[k][0] && x < cx[k][1];
        if (!carved) row[x ^ 2] = (uint8_t)(PAL_SLOT0 + q7(px[x0 + i]));
    }
}

static void lineRun() {
    uint8_t px[EpCore::Nick::OUT_PX];
    const int fl = s_nick.line(px);
    s_dave.tick(16);                            // 256 T at 4 MHz = 16 Dave cycles (250 kHz)
    const bool vs = (fl & EpCore::Nick::L_VSYNC) != 0;
    if (s_nick.displayEnabled && s_nick.videoMode == 1 && s_nick.colorMode == 3) s_logoLine = true;
    if (s_prevVs && !vs) s_row = 0;
    s_prevVs = vs;
    if (!vs) lineOut(px);
    if (s_row < 1000) s_row++;
}

void catchUp() {
    while (CPU::tstates >= s_lineT) {
        lineRun();
        s_lineT += T_PER_LINE;
    }
}

uint32_t nextLineT() { return s_lineT; }

void frameEnd() {
    catchUp();
    s_logoFrames = s_logoLine ? s_logoFrames + 1 : 0;
    s_logoLine = false;
    s_lineT -= CPU::statesInFrame;
}

void renderFrame() {}

bool intLine() { return s_dave.irq(); }

// ---------------------------------------------------------------------------
// Reset

void reset() {
    if (!s_exos) bindRoms();
    for (int i = 0; i < 4; i++) s_nick.vram[i] = MemESP::ram[4 + i].direct();
    s_nick.irqCb = irqCb;
    s_dave.pageCb = pageCb;
    s_nick.reset();
    s_dave.reset(true);
    fileReset();
    s_logoFrames = 0;
    s_logoLine = false;
    memcpy(s_dave.keys, keys, sizeof(keys));
    s_lineT = T_PER_LINE;
    s_frac = 0;
    s_row = 0;
    s_prevVs = false;
    MemESP::romInUse = 0;
    MemESP::p3special = 2;        // recoverPage0() keeps out of window 0
    MemESP::pagingLock = 0;
    for (int w = 0; w < 4; w++) mapWin(w, s_dave.page[w]);
    Debug::log("[EP] reset: %s, %uK RAM", s_ep64 ? "EP64 EXOS 2.0" : "EP128 EXOS 2.1", s_ep64 ? 64u : 128u);
}

// ---------------------------------------------------------------------------
// Wait states (ep128emu Ep128VM::videoMemoryWait*): a VRAM access waits for the
// next Nick slot boundary plus the RAM latency. Here a line is 256 T = 57 slots,
// so slot boundaries are where tstates * 57 is a multiple of 256; the arithmetic
// is in 1/57 T and the fraction is carried (ep128emu keeps half cycles).

static inline uint32_t t57() { return CPU::tstates * 57u + s_frac; }
static inline uint32_t up256(uint32_t v) { return (v + 255u) & ~255u; }

// `pre`: T before the sync point, `post`: T (x57) after it, `base`: the access's own T.
static inline void vramWait(uint32_t pre57, uint32_t post57, uint32_t base) {
    const uint32_t s = t57();
    const uint32_t end = up256(s + pre57) + post57;
    uint32_t extra = end - s;
    extra = extra > base * 57u ? extra - base * 57u : 0;
    s_frac += extra;
    CPU::tstates += s_frac / 57u;
    s_frac %= 57u;
}

void draw(unsigned int n, bool contended) {
    if (__builtin_expect(CPU::tstates >= s_lineT, 0)) catchUp();
    if (n == 3) {                               // a memory access (internal cycles pass n != 3)
        if (contended) vramWait(2 * 57, 57 + 82, 3);
        else if (s_dave.waitMode() == 0) CPU::tstates += 1;
    }
    CPU::tstates += n;
}

void drawOpcode(bool contended) {
    if (__builtin_expect(CPU::tstates >= s_lineT, 0)) catchUp();
    if (contended) vramWait(86, 142 + 81, 4);
    else if (s_dave.waitMode() <= 1) CPU::tstates += 1;
    CPU::tstates += 4;
}

// ---------------------------------------------------------------------------
// Ports (low byte only): Dave #A0..#BF, Nick #80..#8F.

uint8_t portIn(uint16_t address) {
    const uint8_t lo = (uint8_t)address;
    if (lo >= 0xA0 && lo <= 0xBF) {
        for (int i = 0; i < 10; i++) s_dave.keys[i] = keys[i] & inj[i];
        return s_dave.read(lo);
    }
    if ((lo & 0xF0) == 0x80) {
        catchUp();
        vramWait(0, 83, 0);
        return s_nick.readPort();
    }
    return 0xFF;
}

void portOut(uint16_t address, uint8_t data) {
    const uint8_t lo = (uint8_t)address;
    if (lo >= 0xA0 && lo <= 0xBF) { catchUp(); s_dave.write(lo, data); return; }
    if ((lo & 0xF0) == 0x80) {
        catchUp();
        vramWait(0, 83, 0);
        s_nick.writePort(lo, data);
    }
}

} // namespace Ep
