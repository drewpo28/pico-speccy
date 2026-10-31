// pico-speccy — Enterprise 64/128 glue. See Ep.h for the model and EpCore.h for
// the chips. FLASH code: the shared RAM-resident paths keep one test that sends an
// Enterprise access here.

#include "Ep.h"
#include "EpFdc.h"
#if HOST_BUILD
#include "speccy/core/Pss.h"
#endif
#include "app/SnapPack.h"
#include "EpTapeRec.h"
#include "speccy/devices/tape/Tape.h"
#include "ui/LEDIndicators.h"
#include "EpCore.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "app/Config.h"
#include "app/Buffer.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "speccy/core/MemESP.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"
#include "pico/time.h"
#include "speccy/z80/MemGates.h"   // g_atm_ro: bit n = drop writes to CPU window n, ATM_RO_EP = the EP fast path
#include "EpFast.h"
#include "speccy/video/Video.h"
#include "speccy/roms/ep/ep_roms.h"

#if PERF_TRACE && PERF_HIST
extern void pcprof_dump();   // PcProf.cpp (global: a declaration inside namespace Ep would not link)
#endif

namespace Ep {

uint8_t keys[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t joy[2] = {0xFF, 0xFF};

static EpCore::Nick s_nick;
static EpCore::Dave s_dave;
static const uint8_t* s_exos = nullptr;     // 32K: segments 0, 1
static const uint8_t* s_basic = nullptr;    // 16K: segment 4
static const uint8_t* s_fileio = nullptr;   // 16K: segment 0x10 (FILE:, EpFile.cpp)
static const uint8_t* s_exdos = nullptr;    // 32K: segments 0x20, 0x21 (EXDOS 1.3) while s_exdosOn
static bool     s_exdosOn = false;          // decided at reset: a disk is in a drive
static bool     s_ep64 = false;             // RAM = FC..FF only
static uint8_t* s_ff = nullptr;             // a 16K page of 0xFF for absent segments
static uint8_t  s_ro = 0, s_vram = 0;       // windows read-only / showing VRAM
#if PERF_TRACE || HOST_BUILD
Perf perf;
#define EPT_BEGIN(v) const uint32_t v = perfNow()
#define EPT_ADD(f, v) perf.f += perfNow() - (v)
#else
#define EPT_BEGIN(v)
#define EPT_ADD(f, v)
#endif
// The fast accessors' state (EpFast.h): the next line end, the wait-state fraction
// and the #BF wait flags. Written here, read inline from CPU.cpp / Z80_JLS.cpp.
uint32_t lineT = Ep::T_PER_LINE;
uint32_t frac57 = 0;
uint8_t  wait1Rd = 1, wait1Op = 1;
static int      s_row = 0;                  // lines since the end of VSYNC
static bool     s_prevVs = false;
static uint32_t s_daveT = 16;               // frame T at which the next Dave cycle ends
static uint8_t* s_aud = nullptr;            // AUD_CAP left samples, then AUD_CAP right
static constexpr int AUD_CAP = 640;
static int8_t   s_keysOvr = -1;             // KeysMode override, -1 = AUTO
static uint32_t s_fkPollUs = 0;             // last read of row 4 from RAM code (time_us_32)
static bool     s_fkPolled = false;

void bindRoms() {
    s_ep64 = (Config::romSet == R_EP64);
    s_exos  = s_ep64 ? gb_rom_ep_exos20 : gb_rom_ep_exos21;
    s_basic = s_ep64 ? gb_rom_ep_basic20 : gb_rom_ep_basic21;
    s_fileio = gb_rom_ep_fileio;
    s_exdos = gb_rom_ep_exdos13;
}

bool exdosOn() { return s_exdosOn; }

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
    if ((seg == 0x20 || seg == 0x21) && s_exdosOn) return (uint8_t*)(s_exdos + ((seg & 1) << 14));
    return nullptr;
}

bool pcInFileRom(uint16_t pc) { return s_dave.page[pc >> 14] == 0x10; }
uint8_t winSeg(int w) { return s_dave.page[w & 3]; }

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
    g_atm_ro = (uint8_t)(s_ro | ATM_RO_EP);
}

static void pageCb(void*, int w, uint8_t seg) { mapWin(w, seg); }
static void irqCb(void*, bool st) { s_dave.setInt1(st); }

// ---------------------------------------------------------------------------
// Video

static __attribute__((always_inline)) inline uint8_t q7(uint8_t c) { return (uint8_t)((c & 0x3F) | ((c >> 1) & 0x40)); }

uint32_t slotRgb(uint8_t i) {
    // Back to a Nick colour: red's two upper bits from bits 0 and 3 (i bit 0, 3),
    // its dropped LSB set whenever red is on at all (so pure black stays black).
    uint8_t c = (uint8_t)((i & 0x3F) | ((i & 0x40) << 1));
    if (c & 0x09) c |= 0x40;
    return EpCore::colourRgb(c);
}

// ---------------------------------------------------------------------------
// 2-colour PIXEL mode at its real 16 px a slot (EXOS's 80-column text): a frame that
// showed such a line is drawn through the packed-pair driver (VIDEO::samPairSync,
// shared with SAM mode 3) from the next frame on. A fb byte = two output pixels,
// each a pair-palette entry: 12 of them allocated sticky to the Nick colours the
// frame uses (a full table gives the nearest), 12..15 the ZX colours the overlays
// draw with. A colour first seen mid-frame is exact from the next frame on.
static constexpr int PAIR_DYN = 12;
struct PairState {
    alignas(4) uint8_t hi[EpCore::Nick::HI_PX];   // the line at 16 px a slot (word stores)
    uint8_t map[256];                  // Nick colour -> entry, 0xFF = not placed
    int16_t col[PAIR_DYN];             // Nick colour per entry, -1 = free
};
static PairState* s_ps = nullptr;      // heap, taken the first time a hi-res line shows
static bool       s_hiSeen = false;    // this frame showed a 2-colour PIXEL line

static void pairReset() {
    memset(s_ps->map, 0xFF, sizeof(s_ps->map));
    for (int i = 0; i < PAIR_DYN; i++) s_ps->col[i] = -1;
}

// A Nick colour the map has not placed yet: allocate or approximate. Cold (flash):
// it runs once per new colour per frame; the hit is the inline pairEntry below.
static __attribute__((noinline)) uint8_t pairMiss(uint8_t c) {
    int freeAt = -1;
    for (int i = 0; i < PAIR_DYN; i++) if (s_ps->col[i] < 0) { freeAt = i; break; }
    if (freeAt >= 0) { s_ps->col[freeAt] = c; return s_ps->map[c] = (uint8_t)freeAt; }
    const uint32_t want = EpCore::colourRgb(c);
    const int wr = (want >> 16) & 255, wg = (want >> 8) & 255, wb = want & 255;
    int best = 0; uint32_t bd = 0xFFFFFFFFu;
    for (int i = 0; i < PAIR_DYN; i++) {
        const uint32_t h = EpCore::colourRgb((uint8_t)s_ps->col[i]);
        const int dr = ((h >> 16) & 255) - wr, dg = ((h >> 8) & 255) - wg, db = (h & 255) - wb;
        const uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < bd) { bd = d; best = i; }
    }
    return s_ps->map[c] = (uint8_t)best;   // approximated: stays so until the next reset
}
static __attribute__((always_inline)) inline uint32_t pairEntry(const uint8_t* map, uint8_t c) {
    const uint8_t e = map[c];
    return e != 0xFF ? e : pairMiss(c);
}


// Nick colour -> framebuffer byte, as a table Nick applies itself (Nick::setOutMap):
// its slot output IS the framebuffer row's content. 256 B of heap beside the audio.
static uint8_t* s_fbMap = nullptr;

// The fb row this raster line lands on, or nullptr. The EXOS picture: 27 text rows
// x 9 = 243 lines from line 38 after VSYNC (the first is its status line), 40 slots
// from slot 11. A 240-row fb starts at the picture's first line (the last 3, blank
// in a 9-line character row, fall off); a 288-row one shows 24 lines of border.
static __attribute__((always_inline)) inline uint8_t* fbRow(int& y) {
    auto& vga = VIDEO::vga;
    if (!vga.frameBuffer) return nullptr;
    const int yres = (int)vga.yres;
    int top = 38 - (yres - 240) / 2;
    if (top < 0) top = 0;
    y = s_row - top;
    if (y < 0 || y >= yres) return nullptr;
    return vga.frameBuffer[y];
}

// The slow lines: a 2-colour PIXEL frame (pair driver) and rows carrying an overlay
// (F8 stats, volume box, LED panel, notify banner — their rects are left alone).
// `px` is the line in fb order, fbMap applied (lores) / `s_ps->hi` the hi-res line.
static void lineOutSlow(uint8_t* row, int y, const uint8_t* px, int nc, int (*cx)[2]) {
    const int xres = (int)VIDEO::vga.xres;
    const int x0 = (EpCore::Nick::OUT_PX - xres) / 2;   // >= 0: the fb is never wider than the line
    auto carved = [&](int x) { for (int k = 0; k < nc; k++) if (x >= cx[k][0] && x < cx[k][1]) return true; return false; };
    (void)y;
    if (VIDEO::sam_pair_live && s_ps) {
        // 2 output pixels a fb byte: hi-res pixels 2x and 2x+1 of the line
        const uint8_t* pl = &VIDEO::profi_pair_lookup[0][0];
        const uint8_t* map = s_ps->map;
        const uint8_t* h = s_ps->hi + 2 * x0;
#define EP_PAIR(k) ((uint32_t)pl[(pairEntry(map, h[(k) * 2]) << 4) | pairEntry(map, h[(k) * 2 + 1])])
        if (nc == 0) {
            uint32_t* d = (uint32_t*)row;              // x^2 order: pixels x+2, x+3, x, x+1
            for (int i = 0; i < xres; i += 4, h += 8)
                *d++ = EP_PAIR(2) | (EP_PAIR(3) << 8) | (EP_PAIR(0) << 16) | (EP_PAIR(1) << 24);
            return;
        }
        for (int x = 0; x < xres; x++, h += 2)
            if (!carved(x)) row[x ^ 2] = (uint8_t)EP_PAIR(0);
#undef EP_PAIR
        return;
    }
    // px is already in fb order with x0 a multiple of 4: byte i of the row = px[x0 + i]
    for (int i = 0; i < xres; i++)
        if (!carved(i ^ 2)) row[i] = px[x0 + i];
}

// One raster line is run in parts: the LPB slots right after the previous line ended
// (the LPB is read there on the hardware, and its IRQ bit raises INT1 at the line's
// START, a whole line before the old line-end run), up to the current slot before a
// Nick port write (a border or fixed-bias write lands on its slot), the rest at the
// line end. The buffers therefore outlive a part: s_px is the line's edge / slow copy.
static int  s_lSlot = 0;                       // the next slot to run
static int  s_lFlags = 0;
static bool s_lDirect = false;                 // Nick writes the row itself this line
static uint8_t* s_lRow = nullptr;
static int  s_lVisLo = 0, s_lVisHi = 0, s_lX0 = 0;
static uint8_t* s_px = nullptr;                // OUT_PX bytes, heap (the machine's own)

static void linePath() {
    const bool pair = VIDEO::sam_pair_live && s_ps;
    s_nick.hiOut = pair ? s_ps->hi : nullptr;
    int y = 0, cx[8][2];
    s_lRow = fbRow(y);
    const int nc = s_lRow ? VIDEO::rowCarves(y, cx) : 0;
    s_lDirect = s_lRow && nc == 0 && !pair;
    if (s_lDirect) {
        // Slots whose 8 px lie inside the row go straight in, the two edge slots
        // (a 360-wide row starts 4 px into slot 8) land in s_px.
        const int xres = (int)VIDEO::vga.xres;
        s_lX0 = (EpCore::Nick::OUT_PX - xres) / 2;             // 4 at 360, 24 at 320
        s_lVisLo = EpCore::Nick::FIRST + (s_lX0 + 7) / 8;
        s_lVisHi = EpCore::Nick::FIRST + (s_lX0 + xres) / 8;
    }
}

static void linePart(int to) {
    if (to <= s_lSlot) return;
    if (s_lSlot == 0) linePath();              // the line's first part decides its path
    if (s_lDirect) s_lFlags |= s_nick.line(s_lRow - s_lX0, s_px, s_lVisLo, s_lVisHi, s_lSlot, to);
    else           s_lFlags |= s_nick.line(s_px, nullptr, EpCore::Nick::FIRST, EpCore::Nick::LAST, s_lSlot, to);
    s_lSlot = to;
}

static void lineRun() {
    EPT_BEGIN(t0);
    linePart(EpCore::Nick::SLOTS);
    int y = 0;
    uint8_t* row = fbRow(y);
    if (s_lDirect) {
        const int xres = (int)VIDEO::vga.xres;
        const int lo = (s_lVisLo - EpCore::Nick::FIRST) * 8, hi = (s_lVisHi - EpCore::Nick::FIRST) * 8;
        if (s_px) {
            for (int p = s_lX0; p < lo; p += 4) __builtin_memcpy(s_lRow + (p - s_lX0), s_px + p, 4);
            for (int p = hi; p < s_lX0 + xres; p += 4) __builtin_memcpy(s_lRow + (p - s_lX0), s_px + p, 4);
        }
    } else if (row && s_px) {
        int cx[8][2];
        const int nc = VIDEO::rowCarves(y, cx);
        EPT_BEGIN(t1); lineOutSlow(row, y, s_px, nc, cx); EPT_ADD(out, t1);
    }
    const int fl = s_lFlags;
    s_lFlags = 0; s_lSlot = 0;
    if (fl & EpCore::Nick::L_HIRES) s_hiSeen = true;
    const bool vs = (fl & EpCore::Nick::L_VSYNC) != 0;
    if (s_prevVs && !vs) s_row = 0;
    s_prevVs = vs;
    if (s_row < 1000) s_row++;
    EPT_ADD(line, t0);
#if PERF_TRACE || HOST_BUILD
    perf.lines++;
#endif
}

// Dave runs on its own clock (250 kHz = 16 T a cycle): to the line ends, and to the
// exact T of every Dave register access, so a sampled-sound write lands in its cycle.
// Frame T from which a #B4 read needs Dave caught up: the first cycle that can move
// the status completes at s_daveT + (statusCycles - 1) * cycleT. Kept beside every
// move of s_daveT and every register write (a keyboard-polling game reads #B4
// ~1500 times a frame; before this each read ran Dave to its own T).
static uint32_t s_daveStatT = 0;
static bool s_b6Read = false;                // the guest polled #B6 this frame (a LOAD)
static __attribute__((always_inline)) inline void daveStatRecalc() {
    s_daveStatT = s_daveT + (s_dave.statusCycles() - 1) * (uint32_t)s_dave.cycleT();
}

static void daveTo(uint32_t t) {
    if (t < s_daveT) return;
    EPT_BEGIN(t0);
    const uint32_t ct = (uint32_t)s_dave.cycleT();
    const uint32_t n = (t - s_daveT) / ct + 1;
    s_dave.tick((int)n);
    s_daveT += n * ct;
    daveStatRecalc();
    EPT_ADD(dave, t0);
#if PERF_TRACE || HOST_BUILD
    perf.dcyc += n;
#endif
}

void catchUp() {
    while (CPU::tstates >= lineT) {
        lineRun();
        daveTo(lineT);
        lineT += T_PER_LINE;
        linePart(4);                            // the new line's LPB: mode, IRQ
    }
}

uint32_t nextLineT() { return lineT; }

uint32_t nextDaveIntT() { return s_dave.intArmed() ? s_daveStatT : 0xFFFFFFFFu; }
void davePoll() { if (CPU::tstates >= s_daveStatT) daveTo(CPU::tstates); }

// Fast tape load. EXOS times every tape pulse the same way (2.1 #EBCB half a period,
// #EBD9 a whole one; 2.0 is the same code 0x9B bytes lower): EI + HALT for one Dave
// channel 0 period, then a busy loop counting E until the #B6 edge(s), then a common
// tail at #EBFE. Everything above it (leader average, threshold, CRC, chunks, remote)
// is relative to E. So at that HALT the deck is run ahead to the edge, E gets what the
// loop would have counted, and the tail runs: real EXOS code on real pulses, no ROM
// patch, EPTE / ep128emu .tap / CSW / WAV (MP3 plays in real time). Recognised by its bytes, so another loader, or a RAM
// copy with other code, plays in real time. The STOP key takes the real loop.
bool tapeFastHalt() {
    if (Tape::tapeStatus != TAPE_LOADING || !Tape::fastLoadEnabled()) return false;
    const uint16_t pc = Z80::getRegPC();
    if (MemESP::readbyte(pc - 2) != 0xFB || MemESP::readbyte(pc - 1) != 0x76) return false;
    int edges, tail;
    if (MemESP::readbyte(pc) == 0x18 && MemESP::readbyte(pc + 1) == 0x14) { edges = 1; tail = 0x27; }
    else if (MemESP::readbyte(pc) == 0xDB && MemESP::readbyte(pc + 1) == 0xB5 &&
             MemESP::readbyte(pc + 2) == 0x0F && MemESP::readbyte(pc + 3) == 0xD2) { edges = 2; tail = 0x1E; }
    else return false;
    const uint16_t to = (uint16_t)(pc + tail);
    if (MemESP::readbyte(to) != 0x3E || MemESP::readbyte(to + 1) != 0x4E ||
        MemESP::readbyte(to + 2) != 0xD3 || MemESP::readbyte(to + 3) != 0xA7) return false;
    if (!(s_dave.keys[7] & 1)) return false;
    const uint32_t haltT = (((uint32_t)s_dave.reg[0] | ((uint32_t)(s_dave.reg[1] & 0x0F) << 8)) + 1) * 16;
    const auto measure = [&](uint8_t& e) {
        uint32_t dt = 0;
        for (int i = 0; i < edges; i++) {
            const uint32_t d = Tape::fastEdge();
            if (!d) return false;
            dt += d;
        }
        const uint32_t n = dt > haltT ? (dt - haltT) / 60 + 1 : 1;
        e = n > 255 ? 255 : (uint8_t)n;
        return true;
    };
    // Called from the byte reader (#EBB9, H = the shift register with its marker):
    // all bits but the last are taken here, with the CRC (#ED58: CCITT in HL'), so a
    // byte costs the guest one bit's worth of code. The last goes back the normal way.
    bool byteLoop = false;
    if (edges == 2) {
        const uint16_t sp = Z80::getRegSP();
        const uint16_t ret = (uint16_t)(MemESP::readbyte(sp) | (MemESP::readbyte((uint16_t)(sp + 1)) << 8));
        if (MemESP::readbyte(ret) == 0xBA && MemESP::readbyte(ret + 1) == 0xF5 &&
            MemESP::readbyte(ret + 2) == 0x9F && MemESP::readbyte(ret + 3) == 0xCD) {
            const uint16_t crc = (uint16_t)(MemESP::readbyte(ret + 4) | (MemESP::readbyte(ret + 5) << 8));
            static const uint8_t sig[] = {0xD9, 0x3E, 0x80, 0x20, 0x01, 0xAF, 0xAC, 0x17};
            byteLoop = true;
            for (unsigned i = 0; i < sizeof(sig); i++)
                if (MemESP::readbyte((uint16_t)(crc + i)) != sig[i]) byteLoop = false;
        }
    }
    uint8_t e = 0;
    if (!measure(e)) return false;
    if (byteLoop) {
        uint8_t h = Z80::getRegH();
        uint16_t c = Z80::getRegHLx();
        const uint8_t d = Z80::getRegD();
        while (!(h & 1)) {
            const bool bit = e < d;
            const bool fb = (((bit ? 0x80 : 0) ^ (c >> 8)) & 0x80) != 0;
            if (fb) c ^= 0x0810;
            c = (uint16_t)((c << 1) | (fb ? 1 : 0));
            h = (uint8_t)((h >> 1) | (bit ? 0x80 : 0));
            Tape::fastSkip(400);                // the deck runs on through the bit code
            if (!measure(e)) break;             // the guest no longer spends (measured)
        }
        Z80::setRegH(h);
        Z80::setRegHLx(c);
    }
    Z80::setRegE(e);
    Z80::setHalted(false);
    Z80::setIFF1(false);                // the handler at #0038 is a bare RET
    Z80::setIFF2(false);
    portOut(0xA7, 0x4F);                // what the loop does: the tail restarts channel 0
    Z80::setRegPC(to);
    CPU::tstates += 32;                 // the screen and the frame keep moving
    return true;
}

#if HOST_BUILD
static const char* s_pssAt = nullptr;
void hostPssPoll() { if (s_pssAt) { Debug::log("[EP] pss %s: %d", s_pssAt, (int)Pss::save(s_pssAt, "host")); s_pssAt = nullptr; } }
#endif

void frameEnd() {
#if PERF_TRACE && PERF_HIST
    { static int n; if (++n >= 50) { n = 0; ::pcprof_dump(); } }
#endif
    EPT_BEGIN(tfe);
    catchUp();
    daveTo(CPU::statesInFrame);
    s_daveT -= CPU::statesInFrame;
    daveStatRecalc();
    if (s_aud) {                                // a short frame: hold the last sample
        const int n = s_dave.outN;
        const uint8_t l = n ? s_aud[n - 1] : 0, r = n ? s_aud[AUD_CAP + n - 1] : 0;
        for (int i = n; i < AUD_CAP; i++) { s_aud[i] = l; s_aud[AUD_CAP + i] = r; }
    }
    // Tape SAVE: the frame's left output is the cassette signal (EpTapeRec.cpp).
    if (EpTapeRec::frame(s_aud, s_dave.outN, tapeRemoteUp(), s_b6Read)) {
        if (Tape::tapeStatus == TAPE_LOADING) Tape::Stop();    // the deck is recording, not playing
        s_dave.setTapeIn(0);                                   // no feedback under the signal
    }
    s_b6Read = false;
    s_dave.outN = 0;
    // The next frame's resolution: 16 px a slot if this one showed a hi-res line.
    bool want = s_hiSeen;
    s_hiSeen = false;
    if (want && !s_ps) {
        s_ps = (PairState*)tryMalloc(sizeof(PairState));
        if (s_ps) pairReset();
    }
    if (!s_ps) want = false;
    if (want && !VIDEO::sam_pair_live) pairReset();
    uint32_t rgb[PAIR_DYN];
    for (int i = 0; i < PAIR_DYN; i++)
        rgb[i] = (want && s_ps->col[i] >= 0) ? EpCore::colourRgb((uint8_t)s_ps->col[i]) : 0;
    VIDEO::samPairSync(want, rgb);
    // In a pair frame the hi-res buffer must carry RAW Nick colours (pairEntry keys
    // on them); the lores output is scratch then. Else Nick writes fb bytes.
    s_nick.setOutMap(VIDEO::sam_pair_live ? nullptr : s_fbMap);
    lineT -= CPU::statesInFrame;
    launchTick();
    EpFdc::frameEnd();
#if HOST_BUILD
    {   // Tests: PICOSPECCY_EP_SWAP="frame:/path" changes the disk in A: at that frame.
        static uint32_t n = 0; n++;
        if (const char* e = getenv("PICOSPECCY_EP_SWAP"))
            if (n == strtoul(e, nullptr, 10)) EpFdc::mount(0, strchr(e, ':') + 1);
        // PICOSPECCY_EP_PSS="frame:/path" writes a .pss at that frame.
        if (const char* e = getenv("PICOSPECCY_EP_PSS"))
            if (n == strtoul(e, nullptr, 10)) s_pssAt = strchr(e, ':') + 1;
    }
#endif
    EPT_ADD(fe, tfe);
#if PERF_TRACE || HOST_BUILD
    {
        static int n; static uint32_t tLast;
        const uint32_t now = perfNow();
        if (tLast) perf.frame += now - tLast;
        tLast = now;
        perf.dsteps = s_dave.steps;
        if (++n >= 50) {
            const Perf& p = perf;
            Debug::log("[EPPERF] /fr us: frame %u z80 %u (own %u) line %u (out %u) dave %u fe %u | slices %u chk %u halt %u lines %u dave cyc %u steps %u wr %u poll %u",
                       p.frame / 50, p.z80 / 50, (p.z80 - p.line - p.dave) / 50, p.line / 50, p.out / 50, p.dave / 50, p.fe / 50,
                       p.slices / 50, p.chk / 50, p.halt / 50, p.lines / 50, p.dcyc / 50, p.dsteps / 50, p.dwr / 50, p.dpoll / 50);
            memset(&perf, 0, sizeof perf); s_dave.steps = 0; n = 0;
        }
    }
#endif
}

void renderFrame() {}

bool intLine() { return s_dave.irq(); }
void intEnable() {
    if (s_dave.irq() && CPU::stFrame > CPU::tstates) CPU::stFrame = CPU::tstates ? CPU::tstates : 1;
}

bool audio(const uint8_t** l, const uint8_t** r) {
    if (!s_aud) return false;
    *l = s_aud; *r = s_aud + AUD_CAP;
    return true;
}

// ---------------------------------------------------------------------------
// Function keys

bool keysToGuest() {
    if (s_keysOvr >= 0) return s_keysOvr != 0;
    return s_fkPolled && (uint32_t)(time_us_32() - s_fkPollUs) < 2000000u;
}

KeysMode cycleKeysMode() {
    switch (s_keysOvr) {
        case KEYS_AUTO: s_keysOvr = keysToGuest() ? KEYS_OFF : KEYS_ON; break;   // always a change
        case KEYS_ON:   s_keysOvr = KEYS_OFF; break;
        default:        s_keysOvr = KEYS_AUTO; break;
    }
    return (KeysMode)s_keysOvr;
}

void clearKeysOverride() { s_keysOvr = -1; s_fkPolled = false; }

// ---------------------------------------------------------------------------
// Reset

void reset() {
    if (!s_exos) bindRoms();
    for (int i = 0; i < 4; i++) s_nick.vram[i] = MemESP::ram[4 + i].direct();
    s_nick.irqCb = irqCb;
    s_dave.pageCb = pageCb;
    // Nick has no reset line (ep128emu's Ep128VM::reset leaves it alone; a cold
    // reset only randomises its registers): on a WARM start EXOS finds its LPT's
    // first byte non-zero at FF:3900 and merely writes port 3 = CF, trusting the
    // LPT counter to be inside the LPT already — a Nick reset to lptCur = 0 then
    // walks segment FC from 0 for ever, no VSYNC block, no row to draw: a black
    // screen after F11 with EXOS alive (hw + host, 2026-10-30). Only the first
    // reset of the session resets the chip; a cold start on RAM garbage restarts
    // the LPT itself (0F / 4F / CF).
    static bool nickUp = false;
    if (!nickUp) { s_nick.reset(); nickUp = true; }
    s_dave.reset(true);
    if (!s_fbMap && (s_fbMap = (uint8_t*)tryMalloc(256)) != nullptr)
        for (int c = 0; c < 256; c++) s_fbMap[c] = (uint8_t)(PAL_SLOT0 + q7((uint8_t)c));
    s_nick.setOutMap(s_fbMap);                  // no heap: raw Nick colours (a wrong picture, no crash)
    if (!s_aud) s_aud = (uint8_t*)tryCalloc(2 * AUD_CAP);   // no heap: no sound
    if (!s_px) s_px = (uint8_t*)tryMalloc(EpCore::Nick::OUT_PX);
    s_lSlot = 0; s_lFlags = 0;
    s_dave.outL = s_aud;
    s_dave.outR = s_aud ? s_aud + AUD_CAP : nullptr;
    s_dave.outCap = s_aud ? AUD_CAP : 0;
    s_dave.outN = 0;
    s_daveT = (uint32_t)s_dave.cycleT();
    daveStatRecalc();
    fileReset();
    // EXDOS: the ROM is on the bus from this reset on when a disk is in a drive, and
    // then its A: is the default device; with no disk the machine boots as before
    // (FILE: default, EXOS scans two segments fewer).
    EpTapeRec::close();
    EpFdc::reset();
    // A TAPE launch boots WITHOUT EXDOS even with a disk in A: (it would make A: the
    // default device, and tape loaders chain their blocks with a plain LOAD "name" —
    // Amaurote died after its first chunk asking A: for AMAUROTE.BIN). The disk stays
    // mounted; the next plain reset (F11) puts EXDOS back on the bus.
    const bool tape = launchWantsTape();
    s_exdosOn = EpFdc::anyMounted() && !tape;
    fileSetDefault(!tape && (!s_exdosOn || launchWantsFile()));
    memcpy(s_dave.keys, keys, sizeof(keys));
    lineT = T_PER_LINE;
    frac57 = 0;
    wait1Rd = s_dave.waitMode() == 0; wait1Op = s_dave.waitMode() <= 1;
    s_row = 0;
    s_prevVs = false;
    MemESP::romInUse = 0;
    MemESP::p3special = 2;        // recoverPage0() keeps out of window 0
    MemESP::pagingLock = 0;
    for (int w = 0; w < 4; w++) mapWin(w, s_dave.page[w]);
    Debug::log("[EP] reset: %s, %uK RAM, EXDOS %s%s", s_ep64 ? "EP64 EXOS 2.0" : "EP128 EXOS 2.1", s_ep64 ? 64u : 128u,
               s_exdosOn ? "on" : "off", tape ? " (tape launch)" : "");
}

// ---------------------------------------------------------------------------
// Wait states (ep128emu Ep128VM::videoMemoryWait*): a VRAM access waits for the
// next Nick slot boundary plus the RAM latency. Here a line is 256 T = 57 slots,
// so slot boundaries are where tstates * 57 is a multiple of 256; the arithmetic
// is in 1/57 T and the fraction is carried (ep128emu keeps half cycles).

// VIDEO::Draw / Draw_Opcode: the internal cycles (addressOnBus, n != 3) and the
// generic accessor path (memory breakpoints set) — the inline twins in EpFast.h
// serve the ordinary accesses.
void draw(unsigned int n, bool contended) {
    if (n == 3) { tickRd(contended); return; }  // a memory access
    if (__builtin_expect(CPU::tstates >= lineT, 0)) catchUp();
    CPU::tstates += n;
}

void drawOpcode(bool contended) { tickOp(contended); }

// ---------------------------------------------------------------------------
// .pss: Nick, Dave, the glue's clocks. The segments and the FILE: channels are not
// here: RAM is the file's, an open FILE: channel is closed by a load.

uint32_t snapSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(1);
    auto io = [&](const void* p, uint32_t n) { o.raw(p, n); };
    s_nick.snapFields(io);
    s_dave.snapFields(io);
    o.u32(lineT); o.u32(frac57); o.u32(s_daveT);
    o.u16((uint16_t)s_row); o.u8(s_prevVs ? 1 : 0);
    o.u8((uint8_t)s_lSlot); o.u8((uint8_t)s_lFlags);
    o.u8(s_exdosOn ? 1 : 0);
    return o.n;
}

bool snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn i(in, n);
    if (i.u8() < 1) return false;
    auto io = [&](void* p, uint32_t len) { i.raw(p, len); };
    s_nick.snapFields(io);
    s_dave.snapFields(io);
    lineT = i.u32(); frac57 = i.u32(); s_daveT = i.u32();
    s_row = i.u16(); s_prevVs = i.u8() != 0;
    s_lSlot = i.u8(); s_lFlags = i.u8();
    s_exdosOn = i.u8() != 0;
    if (!i.ok) { reset(); return false; }
    launchAbort();
    s_nick.palRebuild();
    s_dave.snapDone();
    s_dave.outN = 0;
    for (int w = 0; w < 4; w++) mapWin(w, s_dave.page[w]);
    wait1Rd = s_dave.waitMode() == 0; wait1Op = s_dave.waitMode() <= 1;
    daveStatRecalc();
    if (s_lSlot) linePath();                    // mid-line: this run's row and buffers
    return true;
}

// ---------------------------------------------------------------------------
// Ports (low byte only): Dave #A0..#BF, Nick #80..#8F.

// A port access is 4 T (ep128emu doIn/doOut: 3 before the devices see it, 1 after);
// the RAM-resident core leaves the whole I/O cycle to Ports. A Nick port then waits
// from T+3 for the next slot + 1 T + the 0.363 us latency, which includes that last T.
static __attribute__((always_inline)) inline void nickPortWait() { vramWait(0, 57 + 83, 0); }

uint8_t portIn(uint16_t address) {
    CPU::tstates += 3;
    struct Tail { bool on = true; ~Tail() { if (on) CPU::tstates += 1; } } tail;
    const uint8_t lo = (uint8_t)address;
    if (lo >= 0xA0 && lo <= 0xBF) {
#if PERF_TRACE || HOST_BUILD
        perf.dpoll++;
#endif
        // Only a read whose value depends on Dave's clock (the interrupt status)
        // needs the catch-up: the keyboard rows, the page registers and the rest
        // are plain latches. A keyboard-polling loop reads #B5 ~1500 times a frame.
        if (lo == 0xB4) {
            if (CPU::tstates >= lineT) catchUp();
            if (CPU::tstates >= s_daveStatT) daveTo(CPU::tstates);   // else the latches are current
        } else if (lo == 0xB5) {
            if (s_dave.kbRow == 4 && segIsRam(s_dave.page[Z80::getRegPC() >> 14])) {
                s_fkPollUs = time_us_32(); s_fkPolled = true;
            }
            memcpy(s_dave.keys, keys, sizeof(keys));
            if (injOn) for (int r = 0; r < 10; r++) s_dave.keys[r] &= injKeys[r];
        } else if (lo == 0xB6) {
            s_b6Read = true;
            s_dave.keys[14] = joy[0];
            s_dave.keys[15] = joy[1];
            if (Tape::tapeStatus == TAPE_LOADING) { Tape::Read(); LED::touchR(LED::TAPE); }
            // The level is also SOUND (tape feedback): Dave is run to now first, so the
            // samples up to this edge carry the old level.
            if (Tape::tapeEarBit != s_dave.tapeIn) {
                if (s_dave.tapeFb) daveTo(CPU::tstates);
                s_dave.setTapeIn(Tape::tapeEarBit);
            }
        }
        return s_dave.read(lo);
    }
    if ((lo & 0xF0) == 0x10) return EpFdc::in(lo);
    if ((lo & 0xF0) == 0x80) {
        catchUp();
        nickPortWait(); tail.on = false;
        return s_nick.readPort();
    }
    return 0xFF;
}

void portOut(uint16_t address, uint8_t data) {
    CPU::tstates += 3;
    struct Tail { bool on = true; ~Tail() { if (on) CPU::tstates += 1; } } tail;
    const uint8_t lo = (uint8_t)address;
    if (lo >= 0xA0 && lo <= 0xBF) {
#if PERF_TRACE || HOST_BUILD
        perf.dwr++;
#endif
        // Timed registers: the sound (#A0-#AF), the interrupt latches (#B4) and
        // #BF (the Dave clock itself). The keyboard row (#B5) and the page
        // registers (#B0-#B3) are latches a later catch-up reads as they are.
        if (lo <= 0xAF || lo == 0xB4 || lo == 0xBF) {
            if (CPU::tstates >= lineT) catchUp();
            daveTo(CPU::tstates);
        }
#if HOST_BUILD
        // Tests: PICOSPECCY_DAVELOG=file logs each sound-register write as
        // "cycle reg value" (reg 0..0x0F, cycles since reset) for an offline diff
        // against ep128emu's Dave.
        static FILE* dl = [] { const char* f = getenv("PICOSPECCY_DAVELOG"); return f ? fopen(f, "w") : nullptr; }();
        if (dl && ((lo & 0x1F) < 0x10 || (lo & 0x1F) == 0x14 || (lo & 0x1F) == 0x1F))
            fprintf(dl, "%u %u %u\n", s_dave.cycles, lo & 0x1F, data);
#endif
        s_dave.write(lo, data);
        if (lo <= 0xAF || lo == 0xB4 || lo == 0xBF) daveStatRecalc();   // source / clock / latches moved
        if (lo == 0xB5) tapeRemote((uint8_t)(data >> 6));
        if (lo == 0xBF) { wait1Rd = s_dave.waitMode() == 0; wait1Op = s_dave.waitMode() <= 1; }
        return;
    }
    if ((lo & 0xF0) == 0x80) {
        catchUp();
        nickPortWait(); tail.on = false;
        catchUp();
        {   // the slots before this one keep the old value
            const uint32_t done = CPU::tstates + T_PER_LINE - lineT;
            int slot = (int)(done * 57u / T_PER_LINE);
            linePart(slot < EpCore::Nick::SLOTS ? slot : EpCore::Nick::SLOTS);
        }
        s_nick.writePort(lo, data);
        return;
    }
    if ((lo & 0xF0) == 0x10) EpFdc::out(lo, data);
}

} // namespace Ep
