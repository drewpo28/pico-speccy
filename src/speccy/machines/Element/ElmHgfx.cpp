// HGFX renderer and planar engine: see ElmHgfx.h. Flash. core0 only queues what the guest
// writes and core1 runs the engine and draws the frame, the way the TS-Conf and Next
// renderers split the work; nothing reads the pixels back but the chunky windows, which
// take the direct path (docs/element-zx-plan.md).
#include "speccy/devices/gs/GS.h"
#include "speccy/machines/Element/ElmHgfx.h"
#include "speccy/machines/Element/Element.h"
#include <string.h>
#include <stdlib.h>
#include <algorithm>

#include "app/Buffer.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "speccy/core/MemESP.h"
#include "app/ESPectrum.h"
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"

volatile bool g_elm_c1_live;
size_t getFreeHeap(void);
extern "C" size_t getLargestAllocatable(void);   // OSDMain.cpp (host: host_stubs)

#if PERF_HIST
void pcprof_dump();
#endif
namespace Hgfx {

uint8_t mode, g20;
static uint8_t s_e20;               // g20 as the engine has reached it in the queue (core1)
bool rgbOrder, radastan;
uint8_t radBank, radPad, keyColour;
uint16_t radOffset;
const uint8_t* keyTop;
const uint8_t* keyBottom;
const uint8_t* hrxcTable;
uint8_t giga, gigaAuto;
const uint8_t* gigaOther;

// ZX and HRC: black has no bright, so key 0 takes colour 8 too (and back). HRXC: the
// key is one index of the 32, "determined by a colour index, not by a colour content"
// (Reference); the ZX rule there made 0, 8 and 24 transparent under key 16 (HRXCIMGS).
static inline bool keyIs(uint8_t c) {
    return c == keyColour || (mode != 2 && !(keyColour & 7) && !(c & 7));
}

static uint8_t* s_buf;              // 128K, PSRAM
// Buffer 1 in SRAM (h1Manage, at()): the half's offsets [s_h1Base - BUF1, +s_h1Size) live
// in s_h1 (the arena), everything else in s_buf. The range STARTS at #0000, the picture
// (48 KB), and runs as far as the arena goes (up to H1_END, the farthest a window of
// PF2026 reaches). A chunky window across its end is a SPLIT window (chunkyRead): the
// guest's reads of it go byte by byte through Elm::g_elm_split (CPU.cpp gsDmaPeek8,
// Elm::busRead), its writes went through at() anyway. PF2026 slides its two windows over
// the whole buffer (OffsetY 0..168 in steps of 4, ~13 mappings a frame, bench census
// 2026-11-16), so no edge inside the buffer is ever safe and the old rule - the half goes
// back to PSRAM on the first window across an edge - needed all 58 KB in ONE piece,
// which the eLeMeNt's heap does not have (the largest piece is the 56 KB overlay region,
// hw 2026-11-16, and 49 KB on a plain build). A split window needs the generic read
// path: below #4000 (level-1 fastmem serves #4000+ only) it always is; a window at
// #4000+ across the edge still sends the half back.
// H1_MIN: the hw's largest piece was 49 132 B against the picture's 49 152 (DVp2w, PERF
// without HIST, 2026-11-16): the arena takes what there is, whole rows, and the last rows
// of the picture stay in PSRAM. The row blocks (sramPromote) want the full 48 KB and fall
// back to their own mallocs with a smaller arena.
enum { H1_END = 0xE800, H1_MIN = 40 * 1024 };
static uint8_t* s_h1;
static uint32_t s_h1Base, s_h1Size;  // absolute offset of s_h1[0] in the 128 KB space, length
static uint8_t s_h1Edge;             // demotions by a window across an edge; 3 = this program does not fit
static uint8_t s_h1Retry;
static bool s_h1Stale;               // a window was mapped into it and it went back: remap again
static void h1Off(bool keep, const char* why = "");
void pairPushPoll();
extern volatile bool g_pairPushNow;
// ONE SRAM block for the whole eLeMeNt session (arenaEnsure, at Elm::reset while the heap
// is still whole): buffer 1's half in direct mode (s_h1) or the row blocks of either
// buffer otherwise (sramPromote), never both. Taken at the moment of need it was not
// there any more: 60 KB free when PF2026 ran against 72 KB wanted (hw 2026-11-16).
static uint8_t* s_arena;
static bool s_sramArena;             // the row blocks are slices of s_arena (not freed one by one)
// HGFX 1 is two 49152-byte buffers and COPY walks them every frame: while the heap has
// room they live in SRAM (the XIP port was the limit, core0's guest pages share it).
// Each is six blocks of 32 rows: 136 KB are free on this machine, but in regions and
// between small blocks, and neither 49 nor 24 KB in one piece got through on hardware.
// Off for the chunky windows, whose pointers assume the PSRAM layout.
enum { SRAM_SHIFT = 5, SRAM_ROWS = 1 << SRAM_SHIFT, SRAM_BLOCK = SRAM_ROWS * 256, SRAM_PER = HEIGHT / SRAM_ROWS,
       SRAM_N = SRAM_PER * 2, SRAM_KEEP = 16384 };
static uint8_t* s_sram[SRAM_N];     // buffer * SRAM_PER + y / SRAM_ROWS
static bool s_sramOn[2];
// Only what needs it pays, and at most ONE buffer (48 KB): a buffer moves when core1
// keeps falling behind, the one that costs more PSRAM transactions first. HGFXAGON
// copies a whole screen a frame: with both buffers in PSRAM that is ~30k line fills and
// write-backs = ~18 ms of core1, so 50 FPS needs the destination in SRAM; the source
// reads it then still takes from PSRAM are a third of that.
enum { SRAM_MAX = 1 };
static volatile uint32_t s_use[2];  // PSRAM transactions a buffer would cost: writes x2
static uint8_t s_lagFrames, s_lagWindow;
static uint8_t s_sramRetry;         // half-second windows before a refused move is tried again
// The hot path sits in the Z80 DMA window: always loaded on this machine.
#define ELM_HOT __attribute__((section(".time_critical.dmaloop"), noinline))
// The helpers of that path are forced inline: at -Os GCC left e16 / rowOf / zxRow / put8
// as functions in FLASH, reached from the RAM code through veneers, several per guest
// byte (objdump of the 2026-11-05 build), on the XIP port the PSRAM buffers share.
#define ELM_INL static inline __attribute__((always_inline))
static uint8_t* s_col;              // the colour table, PSRAM
static struct Hot {                 // read per pixel: heap
    uint8_t map[256], pidx[256], zxp[16], line[720], line2[720], slots[720];
    uint32_t pal[16], nib[16];      // nib: a nibble of pixel bits as a byte mask, left pixel low
    // core0 scratch for frameSetup / pairPalette: as locals they were 4 KB on top of the
    // pair driver's 2 KB, past the 8 KB core0 stack (HGFX 2 hung the board, 2026-11-01).
    uint32_t rgb[256], hist[256], h[256];
    // paletteProgram: which of the 168 slots an index holds (0xFF none), the colour each
    // slot was given, and which indices sit on a nearest colour rather than their own.
    uint32_t slotCol[168];
    uint8_t mapK[256], approx[256], claim[168];
    // HRXC's own pair table (pairLutUpdate): the pairs the last frame drew, counted.
    uint16_t cnt[256];
    uint8_t lut[256];
    bool lutOn;
    uint32_t colRgb[64];            // HAM: the table's first 64 colours as RGB, taken per frame (s_col is PSRAM)
}* s_hot;
static uint8_t s_cube[168];
// HAM8 colours (owner's choice 2, 2026-11-02): a median-cut palette of the picture on
// the 168 slots plus a light 4x4 ordered dither; the fixed 6x7x4 cube with a strong
// 2x2 dither stays as the fallback. The histogram (RGB 4:4:4 bins, every fourth row)
// is taken on core0 twice a second; the bin -> slot table fills on core1 as bins turn
// up (0xFF = not yet), so a new palette costs no full nearest-colour pass.
enum { HAM_BINS = 4096, HAM_DITHER = 10, HAM_MAX_MSE = 300, HAM_STICK = 3 * 16 * 16 };   // a slot keeps its colour within 16 levels a channel
static struct Ham {
    uint8_t lut[HAM_BINS];
    uint32_t rgb[168];                  // the colour of each slot
    uint8_t n;                          // slots in use, 0 = the cube
}* s_ham;
struct HamBox { uint16_t lo, hi; uint32_t w; uint8_t mn[3], mx[3]; };
static uint16_t* s_hamTmp;              // counts + bin order, then the boxes: PSRAM
static uint8_t s_hamHold;
static bool s_hamCubeOn;                // the slots hold the cube
static bool s_pair, s_zxPair, s_palValid, s_pairWant2;
// HRXC: the pairs of the last frame by TABLE INDEX (64 x 64, the bottom screen's are
// +32), heap, only while mode 2 is up. See pairLutUpdate.
static uint16_t* s_cnt2;
static uint8_t s_pairHold;
static uint8_t s_r[REG_COUNT];      // as the guest wrote them, core0
static uint8_t s_e[REG_COUNT];      // as the engine has reached them in the queue
static inline int16_t s16(uint8_t r) { return (int16_t)(s_r[r] | (s_r[r + 1] << 8)); }
ELM_INL int16_t e16(uint8_t r) { return (int16_t)(s_e[r] | (s_e[r + 1] << 8)); }
// The core1 queue: core0 writes s_qw, core1 s_qr, both free-running. In PSRAM, one
// frame of a heavy demo is ~8000 entries. Bits 31..29 kind, the rest as below.
enum : uint32_t { Q_N = 1u << 14, Q_VRAM = 0u << 29, Q_REG = 1u << 29, Q_CMD = 2u << 29, Q_DRAW = 3u << 29, Q_RUN = 4u << 29,
                 Q_BYTES = 5u << 29 };
static uint32_t* s_q;
static volatile uint32_t s_qw, s_qr, s_drawPosted, s_drawDone;
static bool s_direct;               // chunky windows are mapped: the engine stays on core0
// Consecutive COPY writes are one queue entry: HGFXAGON's 7800 a frame are ~200 runs.
// Two words: the run, then the offset the Z80 read from — HE2025 replicates a 2-byte
// pattern along a row with an overlapping LDIR (read at HL, write at HL+2), and the
// ROM's scroll copies a line up the same way; the pixels come from the READ's position.
static uint16_t s_runOff, s_runLen, s_runSrc;
// Consecutive plain writes (INK / ATTR) are one entry too: a header (offset, count,
// the read flag) and the bytes four to a word. FDMATEST blits 12 sprites a frame as 72
// FastDMA transfers of 288 bytes, 6 a row: 20.7k single entries overflowed the 16k
// queue every frame and core1 redid the row geometry per byte.
enum { BRUN_MAX = 32 };
static uint32_t s_bData[BRUN_MAX / 4];
static uint16_t s_bOff;
static uint8_t s_bLen;
static bool s_bRead;
static bool s_live, s_palDirty, s_palAll;
// Anything the picture depends on was written since the last frame was posted. A frame
// with nothing new is not drawn again where the picture is HGFX alone (no ZX layer under
// it, no HAM, no pair frame): Another World shows a new picture every third frame and
// the draw of 360x288 from PSRAM is most of a frame of core1. Every tenth frame is drawn
// anyway (a toast's carve going away, a video mode change).
static bool s_fbDirty = true;
static uint8_t s_cleanRun;
static uint64_t s_busyUntil;
// The current mode's geometry: pixels, bits per pixel, the second buffer.
static int s_w = WIDTH, s_h = HEIGHT, s_stride = WIDTH, s_bpp = 8;
// The pair frame's staged palette (pairPalette, mode 2): core1 pushes it at the start of
// the draw it belongs to (pairPendApply), which the Q_DRAW gate starts only with the beam
// below the picture or in blanking (beamBelowPicture).
static uint32_t s_pairPendPal[16];
static volatile bool s_pairPendValid;
static bool s_pairPendFull;
static inline bool beamBelowPicture() {
    const int beam = VIDEO::displayBeamRow();
    return beam < 0 || beam >= VIDEO::screenTopRows() + s_h;
}
// core1 cannot push it (hdmi_set_profi_ds80_mode keeps ~1 KB of locals, core1's stack is
// 2 KB: SIGBUS stackOvf on the first refresh, hw 2026-11-16 18:18): it raises
// g_pairPushNow at the draw's start and core0's line tick (VIDEO::tsDrawTick ->
// pairPushPoll) pushes within a line, the beam still below the picture. Pushed in
// core0 context only: frameSetup takes a push still due before it stages the next.
volatile bool g_pairPushNow;
#if PERF_TRACE
static uint32_t s_pPushPoll, s_pPushLate, s_pPushBeamMin = 999, s_pPushBeamMax, s_pPushCap;   // pushes: at the draw's start / late (frameSetup), beam row at the push
#endif
// core0 holds the beam watch: core1 only says it is at the gate (g_pairPushNow) and spins;
// core0, polling in every idle loop, pushes the palette the moment the beam is below the
// picture and releases core1 (g_pairPushNow = false). With core1 watching the beam and
// core0 pushing on a flag, the push landed 0.1-0.6 ms late and, on a core1 busy with the
// NeoGS, the 25 ms cap fired: "beam 1..240" at the push in a fade (hw 2026-11-16 19:25).
static volatile bool s_pairGo;           // core0 -> core1: the staged palette is in, draw now
void pairPushPoll() {
    if (!g_pairPushNow) return;
    if (!s_pairPendValid) { g_pairPushNow = false; s_pairGo = true; return; }
    if (!beamBelowPicture()) return;
    __sync_synchronize();
#if PERF_TRACE
    { const int b = VIDEO::displayBeamRow(); s_pPushPoll++;
      if (b >= 0) { if ((uint32_t)b < s_pPushBeamMin) s_pPushBeamMin = (uint32_t)b; if ((uint32_t)b > s_pPushBeamMax) s_pPushBeamMax = (uint32_t)b; } }
#endif
    s_pair = VIDEO::samPairSync(true, s_pairPendPal, s_pairPendFull ? s_pairPendPal + 12 : nullptr);
    s_pairPendValid = false;
    __sync_synchronize();
    s_pairGo = true;                     // core1 draws now
    g_pairPushNow = false;
}
// The hardware palette is APPEND-ONLY within a scene: a slot that pixels on screen refer
// to is never recoloured, a new colour takes a slot no drawn pixel uses. Any switch of a
// live slot recolours the frame on screen until the redraw, and no timing of that write
// can be made safe with one framebuffer: at once (noise, 1-2 frames), at any draw start
// (a draw from the top border is overtaken by the beam), at a draw start with the beam
// below the picture (the SD reads riding in core1's draw let the beam catch it) - all
// tried on PF2026, owner's recordings 2026-11-16. Which slots are live comes free from
// the LUT: it is filled lazily, bin by bin, by the pixels core1 draws, so the slots it
// holds are the slots on screen since it was last cleared. A colour with no near slot
// goes to a slot the LUT does not hold; with none left, to the nearest live slot (the
// one visible change, at a scene change).
static uint8_t s_hamSlotK[256];          // hardware slot -> HAM index k, 0xFF none
static void hamLiveSlots(bool* live) {    // live[k]: a drawn pixel maps to slot k
    for (int k = 0; k < 168; k++) live[k] = false;
    const uint8_t* lut = s_ham->lut;
    for (int b = 0; b < HAM_BINS; b++) { const uint8_t k = s_hamSlotK[lut[b]]; if (k != 0xFF) live[k] = true; }
}
static uint32_t s_b1 = BUF1;
static void sramOff(bool keep);

// 6144 bytes in every mode (HGFX-4EN: "enough for half of the HiRes screen only ... we
// help ourselves with an offset"). Sized by the mode it covered #4000-#9FFF in HGFX 3,
// and SHRTEXT's stack, code and text buffer drew as dots.
int vramPages() { return 6144 / 256; }

void setMode(uint8_t m) {
    s_fbDirty = true;
    drain();
    sramOff(true);
    const bool h1Was = s_h1 != nullptr;
    h1Off(false, "setMode");               // the mode clears the buffers below anyway
    s_h1Edge = 0;
    // Selecting a mode clears both buffers. The Reference does not say so; SAM2HGFX
    // is the evidence: it re-selects HGFX 1 before every picture and draws only
    // planes 0-3 (PlanarMask 0F), and its intro text in planes 2-7 is gone on the
    // owner's screenshots (2026-11-02). Every other program selects the mode once,
    // before it draws.
    if (m && s_buf) memset(s_buf, 0, BUF_SIZE);
    mode = m;
    static const struct { uint16_t w, h; uint8_t bpp; bool two; } k[10] = {
        { 256, 192, 8, true },  { 256, 192, 8, true },  { 256, 192, 8, true },
        { 256, 192, 8, true },                         // 3 HGFX 1
        { 512, 192, 8, false },                        // 4 HGFX 2
        { 512, 384, 4, false },                        // 5 HGFX 3
        { 720, 546, 2, false },                        // 6 HGFX 4
        { 720, 576, 2, false },                        // 7 HGFX 5
        { 360, 288, 4, true },                         // 8 HGFX 6
        { 360, 288, 8, false },                        // 9 HGFX 7
    };
    const auto& g = k[m < 10 ? m : 0];
    s_w = g.w; s_h = g.h; s_bpp = g.bpp;
    s_stride = g.w * g.bpp / 8;
    s_b1 = g.two ? BUF1 : 0;
    if (h1Was) Elm::remap();          // a window may have pointed into the freed SRAM half
}

// A pixel of a packed row: the high bits are the left pixel.
static inline uint8_t pxGet(const uint8_t* row, int x) {
    if (s_bpp == 8) return row[x];
    if (s_bpp == 4) return (row[x >> 1] >> (x & 1 ? 0 : 4)) & 15;
    return (row[x >> 2] >> (6 - (x & 3) * 2)) & 3;
}
static inline void pxPlanar(uint8_t* row, int x, uint8_t v, uint8_t m) {
    if (s_bpp == 8) { row[x] = planar(row[x], v, m); return; }
    if (s_bpp == 4) {
        const int sh = x & 1 ? 0 : 4;
        row[x >> 1] = planar(row[x >> 1], (uint8_t)(v << sh), (uint8_t)((m & 15) << sh));
    } else {
        const int sh = 6 - (x & 3) * 2;
        row[x >> 2] = planar(row[x >> 2], (uint8_t)(v << sh), (uint8_t)((m & 3) << sh));
    }
}

// Buffer 1's picture in SRAM (mode 3 under chunky windows, h1Manage): PF2026 rewrites
// ~19 KB of it a frame through FastDMA and core1 draws it, both against PSRAM (memcpy
// 8.5 % of core0, draw 15-20 ms; hw 2026-11-16). The 48 KB at absolute offsets
// [s_h1Base, +s_h1Size) live in s_h1 then, everything else stays in s_buf; every access
// to the 128 KB space goes through at(), so the two never disagree. bufN() stays the
// plain PSRAM base: its callers are the other modes, and a mode change returns the half.
ELM_INL uint8_t* at(uint32_t a) {
    if (s_h1 && a - s_h1Base < s_h1Size) return s_h1 + (a - s_h1Base);
    return s_buf + a;
}
ELM_INL uint8_t* bufN(int n) { return s_buf + (n ? s_b1 : 0); }
ELM_INL uint8_t* rowOf(int n, int y) {
    if (s_sramOn[n]) return s_sram[n * SRAM_PER + (y >> SRAM_SHIFT)] + (y & (SRAM_ROWS - 1)) * 256;
    return at((n ? s_b1 : 0) + (uint32_t)y * s_stride);
}
static inline uint32_t dstOff() { return ((s_r[REG_VP] >> 1) & 1) ? s_b1 : 0; }
static inline uint32_t srcOff() { return ((s_r[REG_VP] >> 2) & 1) ? s_b1 : 0; }
static inline uint8_t* dstBuf() { return bufN((s_r[REG_VP] >> 1) & 1); }
static inline uint8_t* srcBuf() { return bufN((s_r[REG_VP] >> 2) & 1); }
ELM_INL int dstE() { return (s_e[REG_VP] >> 1) & 1; }
ELM_INL int srcE() { return (s_e[REG_VP] >> 2) & 1; }
static inline uint64_t now() { return CPU::global_tstates + CPU::tstates; }

bool ensure() {
    if (!s_hot) {
        s_hot = (Hot*)tryMalloc(sizeof(Hot));
        if (s_hot) { memset(s_hot->mapK, 0xFF, sizeof s_hot->mapK); memset(s_hot->cnt, 0, sizeof s_hot->cnt); s_hot->lutOn = false; }
        if (s_hot) for (int n = 0; n < 16; n++)
            s_hot->nib[n] = ((n & 8) ? 0xFFu : 0) | ((n & 4) ? 0xFF00u : 0) | ((n & 2) ? 0xFF0000u : 0) | ((n & 1) ? 0xFF000000u : 0);
    }
    if (!s_buf) {
        s_buf = (uint8_t*)Buffer::palloc(BUF_SIZE + COLOURS, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (s_buf) { memset(s_buf, 0, BUF_SIZE + COLOURS); s_col = s_buf + BUF_SIZE; }
        else Debug::log("[ELM] no memory for the HGFX buffers");
    }
    if (s_buf && !s_q) s_q = (uint32_t*)Buffer::palloc(Q_N * sizeof(uint32_t), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    return s_buf && s_hot;
}

ELM_INL bool queued() { return s_q && !s_direct && g_elm_c1_live; }

// Everything queued has run and the last frame is on screen. Bounded: core1 never waits
// for core0, so only a dead core1 ends here.
ELM_INL void pushRaw(uint32_t e);
void drain();

// In and out of SRAM: the pixels travel. Only with the queue empty.
static void sramOff(bool keep) {
    if (!s_sramOn[0] && !s_sramOn[1]) return;
    drain();
    for (int n = 0; n < 2; n++) {
        if (!s_sramOn[n]) continue;
        s_sramOn[n] = false;
        for (int i = 0; i < SRAM_PER; i++) {
            uint8_t*& b = s_sram[n * SRAM_PER + i];
            if (keep) memcpy(s_buf + n * (uint32_t)BUF1 + i * (uint32_t)SRAM_BLOCK, b, SRAM_BLOCK);
            if (!s_sramArena) free(b);
            b = nullptr;
        }
        Debug::log("[ELM] HGFX buffer %d back to the heap, free %u", n, (unsigned)getFreeHeap());
    }
    s_sramArena = false;
}
// Back to PSRAM (keep = the SRAM bytes are the live ones). core1 may be drawing from it.
static void h1Off(bool keep, const char* why) {
    if (!s_h1) return;
    drain();
    if (keep) memcpy(s_buf + s_h1Base, s_h1, s_h1Size);
    s_h1 = nullptr;                       // the arena stays for the next time
    s_h1Retry = 50;                       // a second before another try (a window across the edge)
    Debug::log("[ELM] HGFX buffer 1 back to PSRAM (%s), heap free %u", why, (unsigned)getFreeHeap());
}
// Into the arena only where the row-block promotion (sramPromote) cannot work: chunky
// windows mapped (direct), mode 3. Called once a frame from frameEnd.
static uint32_t s_arenaSize;
static void h1Manage() {
    const bool want = s_direct && mode == 3 && s_buf && s_b1 == BUF1 && s_bpp == 8 && s_w == WIDTH && s_h == HEIGHT &&
                      s_stride == WIDTH && !s_sramOn[0] && !s_sramOn[1];
    if (!want) { if (s_h1) { h1Off(true, s_direct ? "mode" : "windows gone"); Elm::remap(); } return; }
    if (s_h1) return;
    if (s_h1Retry) { s_h1Retry--; return; }
    if (!s_arena) return;                 // no arena this session: the half stays in PSRAM
    // Windows across the arena's edges three times: this program's windows reach past what
    // the arena covers (PF2026 maps from #0000 and down to #E800 = 58 KB; the hw arena is the
    // 56 KB overlay region). Each round trip is a drain + a copy + a remap: stop, until the
    // mode or the machine changes.
    if (s_h1Edge >= 3) {
        if (s_h1Edge == 3) { s_h1Edge = 4; Debug::log("[ELM] HGFX buffer 1: windows reach past the arena, stays in PSRAM"); }
        return;
    }
    s_h1Size = s_arenaSize < H1_END ? s_arenaSize : H1_END;
    s_h1Base = BUF1;                      // the picture first; the tail only as far as the arena goes
    drain();                              // core1 draws from the PSRAM copy until here
    memcpy(s_arena, s_buf + s_h1Base, s_h1Size);
    s_h1 = s_arena;
    Elm::remap();                         // the mapped windows re-read their pointers (chunkyRead)
    Debug::log("[ELM] HGFX buffer 1 #%04X..#%04X in SRAM (the arena)", (unsigned)(s_h1Base - BUF1), (unsigned)(s_h1Base - BUF1 + s_h1Size));
}
static void sramPromote() {
    if (s_direct || s_h1 || !s_buf || s_bpp != 8 || s_w != WIDTH || s_h != HEIGHT || !s_b1) return;
    if (s_sramRetry) { s_sramRetry--; return; }
    if ((int)s_sramOn[0] + (int)s_sramOn[1] >= SRAM_MAX) return;
    const int n = s_sramOn[0] ? 1 : s_sramOn[1] ? 0 : s_use[1] > s_use[0];
    if (s_sramOn[n]) return;
    const size_t free0 = getFreeHeap();
    uint8_t** b = s_sram + n * SRAM_PER;
    if (s_arena && s_arenaSize >= SRAM_PER * SRAM_BLOCK) {
        drain();
        for (int i = 0; i < SRAM_PER; i++) {
            b[i] = s_arena + i * (uint32_t)SRAM_BLOCK;
            memcpy(b[i], s_buf + n * (uint32_t)BUF1 + i * (uint32_t)SRAM_BLOCK, SRAM_BLOCK);
        }
        s_sramArena = true;
        s_sramOn[n] = true;
        Debug::log("[ELM] HGFX buffer %d in SRAM (the arena)", n);
        return;
    }
    int got = 0;
    while (got < SRAM_PER && getFreeHeap() >= SRAM_BLOCK + SRAM_KEEP &&
           (b[got] = (uint8_t*)tryMalloc(SRAM_BLOCK)) != nullptr) got++;
    if (got < SRAM_PER) {
        for (int i = 0; i < got; i++) { free(b[i]); b[i] = nullptr; }
        s_sramRetry = 20;                   // the heap may have room in ten seconds
    } else {
        drain();
        for (int i = 0; i < SRAM_PER; i++)
            memcpy(b[i], s_buf + n * (uint32_t)BUF1 + i * (uint32_t)SRAM_BLOCK, SRAM_BLOCK);
        s_sramOn[n] = true;
    }
    Debug::log("[ELM] HGFX buffer %d %s: %d of %d blocks, heap free %u -> %u", n, got == SRAM_PER ? "in SRAM" : "stays in PSRAM",
               got, SRAM_PER, (unsigned)free0, (unsigned)getFreeHeap());
}

// The arena: the largest free piece up to H1_END bytes, at least H1_MIN (a buffer's six row
// blocks), leaving ARENA_KEEP of heap.
enum { ARENA_KEEP = 12 * 1024 };
static_assert(H1_END >= SRAM_PER * SRAM_BLOCK, "the arena holds a buffer's row blocks too");
void arenaEnsure() {
    if (s_arena || !ensure()) return;     // the per-pixel tables first: they want SRAM too
    const size_t f = getFreeHeap(), big = getLargestAllocatable();
    uint32_t want = H1_END;
    if (big < want + 64) want = big > 64 ? (uint32_t)((big - 64) & ~0xFFu) : 0;   // a malloc header, whole rows
    if (f < want + ARENA_KEEP) want = f > ARENA_KEEP ? (uint32_t)((f - ARENA_KEEP) & ~0xFFu) : 0;
#if HOST_BUILD
    // Bench: the hw heap's shape (PICOSPECCY_ELM_ARENA=57088 = the 56 KB overlay region).
    if (const char* e = getenv("PICOSPECCY_ELM_ARENA")) { const uint32_t cap = (uint32_t)atoi(e) & ~0xFFu; if (cap < want) want = cap; }
#endif
    if (want < H1_MIN) {
        Debug::log("[ELM] HGFX SRAM arena not taken: heap free %u, largest %u", (unsigned)f, (unsigned)big);
        return;
    }
    s_arena = (uint8_t*)tryMalloc(want);
    s_arenaSize = s_arena ? want : 0;
    Debug::log("[ELM] HGFX SRAM arena %s (%u B; heap free %u, largest %u before)", s_arena ? "taken" : "refused",
               (unsigned)want, (unsigned)f, (unsigned)big);
}
static void gsAhead(bool on);
void arenaFree() {
    gsAhead(false);
    if (!s_arena) return;
    sramOff(true);
    h1Off(true, "machine change");
    free(s_arena); s_arena = nullptr;
}
static inline void runFlush();
static void runFlushHot();
// The last frame's Q_DRAW: where it sits in the queue (frameEnd). Everything before it is
// engine work; nothing pushed after it while s_qw is still s_drawQw.
static uint32_t s_drawAt, s_drawQw;
// The NeoGS shares core1 with the draw (GS::pump between rows, rowPump): a 14-20 ms pair
// draw leaves it a third of its rate for that long, and with its default 2 ms backlog
// and 3.4 ms ring that was a sample hold every few ms (hw 2026-11-16, LIMSHOWM). Let the
// card run ELM_GS_AHEAD_MS ahead while the engine is on core1 (GS::setAhead).
enum { ELM_GS_AHEAD_MS = 24 };
static void gsAhead(bool on) {
    if (GS::enabled) GS::setAhead(on ? ELM_GS_AHEAD_MS : 0);   // setAhead lives in the GS overlay
}
static void drainWait(bool engineOnly) {
    if (!g_elm_c1_live) return;
    runFlush();
    const uint64_t t0 = time_us_64();
    for (;;) {
        const uint32_t qr = s_qr, qw = s_qw;
        if (qr == qw && s_drawDone == s_drawPosted) return;
        if (g_pairPushNow) pairPushPoll();   // core1 just started a draw that waits for its palette
        // Only the frame's draw left (waiting for the beam, or drawing): the engine's
        // queued work is done.
        if (engineOnly && qw == s_drawQw && (int32_t)(qr - s_drawAt) >= 0) return;
        if (time_us_64() - t0 > 200000) {
            Debug::log("[ELM] core1 queue stuck, back to core0");
            g_elm_c1_live = false;
            gsAhead(false);
            s_qr = s_qw; s_drawDone = s_drawPosted;
            return;
        }
    }
}
void drain() { drainWait(false); }

#if PERF_TRACE
static uint32_t s_pWaitUs, s_pPush, s_pDrop;          // core0
static uint32_t s_pPidxChg, s_pPalChg, s_pFullFlip, s_pLutPush;   // the pair frame: what moved per 60 frames
static volatile uint32_t s_pDrawUs, s_pDraws, s_pExecUs, s_pExecN, s_pCmdUs, s_pSkips;   // core1
#endif

ELM_INL void pushRaw(uint32_t e) {
#if PERF_TRACE
    s_pPush++;
    if (s_qw - s_qr >= Q_N) {
        const uint32_t t0 = time_us_32();
        while (s_qw - s_qr >= Q_N) { }
        s_pWaitUs += time_us_32() - t0;
    }
#endif
    while (s_qw - s_qr >= Q_N) { }
    s_q[s_qw & (Q_N - 1)] = e;
    __sync_synchronize();
    s_qw = s_qw + 1;
}
// A two-word entry lands whole: core1 sees s_qw move once.
ELM_INL void pushRaw2(uint32_t e, uint32_t e2) {
#if PERF_TRACE
    s_pPush++;
#endif
    while (s_qw - s_qr >= Q_N - 1) { }
    s_q[s_qw & (Q_N - 1)] = e;
    s_q[(s_qw + 1) & (Q_N - 1)] = e2;
    __sync_synchronize();
    s_qw = s_qw + 2;
}
// A whole byte run lands at once, like the two-word entry.
ELM_INL void pushBytes() {
    const uint32_t n = s_bLen, words = (n + 3) >> 2;
    s_bLen = 0;
    const uint8_t* b = (const uint8_t*)s_bData;
    if (n == 1) { pushRaw(Q_VRAM | s_bOff | (uint32_t)b[0] << 16 | (s_bRead ? 1u << 24 : 0)); return; }
#if PERF_TRACE
    s_pPush++;
#endif
    while (Q_N - (s_qw - s_qr) < words + 1) { }
    s_q[s_qw & (Q_N - 1)] = Q_BYTES | s_bOff | (n - 1) << 16 | (s_bRead ? 1u << 24 : 0);
    for (uint32_t w = 0; w < words; w++) s_q[(s_qw + 1 + w) & (Q_N - 1)] = s_bData[w];
    __sync_synchronize();
    s_qw = s_qw + words + 1;
}
ELM_HOT static void runFlushHot() {
    if (s_runLen) { pushRaw2(Q_RUN | s_runOff | (uint32_t)s_runLen << 16, s_runSrc); s_runLen = 0; }
    if (s_bLen) pushBytes();
}
ELM_INL void runFlush() { if (s_runLen | s_bLen) runFlushHot(); }
ELM_INL void byteAppend(uint16_t off, uint8_t v, bool wasRead) {
    if (s_bLen && s_bLen < BRUN_MAX && off == (uint16_t)(s_bOff + s_bLen) && wasRead == s_bRead) {
        ((uint8_t*)s_bData)[s_bLen++] = v;
        return;
    }
    runFlush();
    s_bOff = off; s_bRead = wasRead; s_bLen = 1;
    ((uint8_t*)s_bData)[0] = v;
}
static inline void push(uint32_t e) { runFlush(); pushRaw(e); }

void reset() {
    const bool was = s_live;
    s_fbDirty = true;
    drain();
    // The planes a program does not touch must not show the last program's picture
    // (HGFXAGON after Tetris).
    sramOff(false);
    h1Off(false, "reset"); s_h1Retry = 0; s_h1Edge = 0;
    s_sramRetry = 0; s_lagFrames = s_lagWindow = 0;
    free(s_ham); s_ham = nullptr;          // 4.8 KB of heap only while a program uses HAM
    s_hamHold = 0; s_hamCubeOn = false;
    if (s_buf) memset(s_buf, 0, BUF_SIZE);
    mode = 0; g20 = 0; s_e20 = 0; rgbOrder = false;
    memset(s_r, 0, sizeof(s_r));
    s_r[REG_MASK] = 0xFF;
    memcpy(s_e, s_r, sizeof(s_e));
    s_busyUntil = 0;
    if (was) { s_live = false; VIDEO::paper_hold = false; VIDEO::applyPalette(); }
}

void loadState(const uint8_t* regs, const uint8_t* colours) {
    if (!s_buf) return;
    drain();
    memcpy(s_r, regs, REG_COUNT);
    memcpy(s_e, regs, REG_COUNT); s_e20 = g20;
    memcpy(s_col, colours, COLOURS);
    s_palDirty = true; s_fbDirty = true;
}

static inline bool inWindow(int x, int y) {
    if (!(s_e[REG_VP] & 8)) return true;
    const int wx = e16(REG_WX), wy = e16(REG_WY), ww = e16(REG_WW), wh = e16(REG_WH);
    // A window off the screen limits nothing: HGFXSPRI sets the bit with such values
    // every second frame and would blink (a guess, the Reference does not say).
    if (ww <= 0 || wh <= 0 || wx >= s_w || wy >= s_h || wx + ww <= 0 || wy + wh <= 0) return true;
    return x >= wx && y >= wy && x < wx + ww && y < wy + wh;
}

ELM_INL uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
ELM_INL void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

// The 8 pixels of a write at once, 8 bits per pixel, all on screen, no window.
ELM_INL void put8(uint8_t* d, uint32_t lo, uint32_t hi, uint32_t bl, uint32_t bh, uint8_t m) {
    const uint32_t m8 = m * 0x01010101u, ml = bl & m8, mh = bh & m8;
    if (ml == ~0u) wr32(d, lo); else if (ml) wr32(d, (rd32(d) & ~ml) | (lo & ml));
    if (mh == ~0u) wr32(d + 4, hi); else if (mh) wr32(d + 4, (rd32(d + 4) & ~mh) | (hi & mh));
}

ELM_HOT static void execVram(uint16_t off, uint8_t v, bool wasRead, uint16_t src) {
    if (!s_buf) return;
    // Bytes per line follow the mode's width (Nether Earth steps 64 in HGFX 2), and
    // only HGFX 1 has the ZX layout.
    const int bpl = s_w / 8;
    const bool linear = (s_e20 & 1) || s_w != WIDTH;
    const int row = linear ? off / bpl : zxRow(off);
    const int col = (off % bpl) * 8;
    const int y = row + e16(REG_OY);
    if (y < 0 || y >= s_h) return;
    int x = col + e16(REG_OX);
    uint8_t* d = rowOf(dstE(), y);
    const uint8_t m = s_e[REG_MASK], pm = s_e[REG_MODE];
    const bool fast = s_bpp == 8 && x >= 0 && x + 8 <= s_w && !(s_e[REG_VP] & 8);
    if (fast) s_use[dstE()] = s_use[dstE()] + 16;
    // COPY moves the 8 pixels the Z80's read latched: from the READ's position plus the
    // source offsets (the wiki, register 01). A write that follows no read draws like ATTR.
    if (pm == 1 && wasRead) {
        if (src == 0xFFFF) src = off;
        const int srow = linear ? src / bpl : zxRow(src);
        const int sy = srow + e16(REG_SY);
        if (sy < 0 || sy >= s_h) return;
        int sx = (src % bpl) * 8 + e16(REG_SX);
        const uint8_t* s = rowOf(srcE(), sy);
        if (fast && sx >= 0 && sx + 8 <= s_w) {
            put8(d + x, rd32(s + sx), rd32(s + sx + 4), ~0u, ~0u, m);
            return;
        }
        // A source pixel past either end of the line reads 0: Nether Earth's title scroller
        // (COPY with source X +3) clears its right edge that way; skipping the pixel left the
        // bits the INK column had set there, and the scroll smeared them over the whole band.
        for (int i = 0; i < 8; i++, x++, sx++) {
            if (x < 0 || x >= s_w || !inWindow(x, y)) continue;
            pxPlanar(d, x, sx >= 0 && sx < s_w ? pxGet(s, sx) : 0, m);
        }
        return;
    }
    const uint8_t ink = s_e[REG_INDEX], paper = s_e[REG_PAPER];
    if (fast) {
        const uint32_t bl = s_hot->nib[v >> 4], bh = s_hot->nib[v & 15];
        const uint32_t i8 = ink * 0x01010101u;
        if (pm == 0) { put8(d + x, i8, i8, bl, bh, m); return; }
        const uint32_t p8 = paper * 0x01010101u;
        put8(d + x, (i8 & bl) | (p8 & ~bl), (i8 & bh) | (p8 & ~bh), ~0u, ~0u, m);
        return;
    }
    for (int i = 0; i < 8; i++, x++, v <<= 1) {
        if (x < 0 || x >= s_w) continue;
        const bool on = v & 0x80;
        if (!on && pm == 0) continue;
        if (!inWindow(x, y)) continue;
        pxPlanar(d, x, on ? ink : paper, m);
    }
}

#if PERF_TRACE
static volatile uint32_t s_pRuns, s_pRunBytes;         // core1
#endif

// A run of plain writes: the row, the masks and the colours once per row chunk, then
// two word stores a byte.
ELM_HOT static void execBytes(uint16_t off, const uint8_t* p, uint32_t n, bool wasRead) {
    if (!s_buf) return;
    const uint8_t pm = s_e[REG_MODE];
    if (s_bpp != 8 || (s_e[REG_VP] & 8) || (pm == 1 && wasRead)) {
        for (uint32_t i = 0; i < n; i++) execVram((uint16_t)(off + i), p[i], wasRead, 0xFFFF);
        return;
    }
#if PERF_TRACE
    s_pRuns = s_pRuns + 1; s_pRunBytes = s_pRunBytes + n;
#endif
    const int bpl = s_w / 8;
    const bool linear = (s_e20 & 1) || s_w != WIDTH;
    const uint8_t m = s_e[REG_MASK];
    const uint32_t i8 = s_e[REG_INDEX] * 0x01010101u, p8 = s_e[REG_PAPER] * 0x01010101u;
    const uint32_t* nib = s_hot->nib;
    while (n) {
        const uint32_t col = off % (uint32_t)bpl, room = (uint32_t)bpl - col, c = n < room ? n : room;
        const int row = linear ? off / bpl : zxRow(off);
        const int y = row + e16(REG_OY), x = (int)col * 8 + e16(REG_OX);
        if (y >= 0 && y < s_h && x >= 0 && x + (int)c * 8 <= s_w) {
            s_use[dstE()] = s_use[dstE()] + 16 * c;
            uint8_t* d = rowOf(dstE(), y) + x;
            if (pm == 0) {
                for (uint32_t i = 0; i < c; i++, d += 8) {
                    const uint8_t v = p[i];
                    if (v) put8(d, i8, i8, nib[v >> 4], nib[v & 15], m);
                }
            } else {
                for (uint32_t i = 0; i < c; i++, d += 8) {
                    const uint32_t bl = nib[p[i] >> 4], bh = nib[p[i] & 15];
                    put8(d, (i8 & bl) | (p8 & ~bl), (i8 & bh) | (p8 & ~bh), ~0u, ~0u, m);
                }
            }
        } else for (uint32_t i = 0; i < c; i++) execVram((uint16_t)(off + i), p[i], wasRead, 0xFFFF);
        off = (uint16_t)(off + c); p += c; n -= c;
    }
}

// A run of COPY writes: n bytes from off, a row at a time. In the ZX layout too: the
// 32 bytes of an aligned group share the row (zxRow keeps bits 5+), so the per-byte
// path, 7800 iterations a frame on HGFXAGON, was 9 ms of core1 for the same bytes.
__attribute__((optimize("no-tree-loop-distribute-patterns"))) ELM_HOT static void execRun(uint16_t off, uint32_t n, uint16_t src) {
    const int bpl = s_w / 8;
    const bool linear = (s_e20 & 1) || s_w != WIDTH;
#if PERF_TRACE
    s_pRuns = s_pRuns + 1; s_pRunBytes = s_pRunBytes + n;
#endif
    while (n) {
        // A chunk stays inside one row at both ends. An overlapping copy (HE2025: the
        // source two bytes behind the destination) is right word by word, forward, as
        // the chip does it 8 pixels at a time: a step is never shorter than 8 bytes here.
        const uint32_t room = (uint32_t)(bpl - off % bpl), sroom = (uint32_t)(bpl - src % bpl);
        const uint32_t c = n < room ? (n < sroom ? n : sroom) : (room < sroom ? room : sroom);
        const int row = linear ? off / bpl : zxRow(off), srow = linear ? src / bpl : zxRow(src);
        const int col = (off % bpl) * 8, y = row + e16(REG_OY), sy = srow + e16(REG_SY);
        int x = col + e16(REG_OX), sx = (src % bpl) * 8 + e16(REG_SX);
        const int px = (int)c * 8;
        if (s_bpp == 8 && !(s_e[REG_VP] & 8) && y >= 0 && y < s_h && sy >= 0 && sy < s_h &&
            x >= 0 && x + px <= s_w && sx >= 0 && sx + px <= s_w) {
            s_use[dstE()] = s_use[dstE()] + px * 2;    // a write = the fill and the write-back
            s_use[srcE()] = s_use[srcE()] + px;
            uint8_t* d = rowOf(dstE(), y) + x;
            const uint8_t* q = rowOf(srcE(), sy) + sx;
            const uint8_t m = s_e[REG_MASK];
            if (m == 0xFF) for (int i = 0; i < px; i += 4) wr32(d + i, rd32(q + i));
            else {
                const uint32_t m8 = m * 0x01010101u;
                for (int i = 0; i < px; i += 4)
                    wr32(d + i, (rd32(d + i) & ~m8) | (rd32(q + i) & m8));
            }
        } else for (uint32_t i = 0; i < c; i++) execVram((uint16_t)(off + i), 0, true, (uint16_t)(src + i));
        off = (uint16_t)(off + c); src = (uint16_t)(src + c); n -= c;
    }
}

ELM_HOT void writeVram(uint16_t off, uint8_t v, bool wasRead, uint16_t src) {
    s_fbDirty = true;
    if (src == 0xFFFF) src = off;
    if (queued()) {
        if (wasRead && s_r[REG_MODE] == 1) {        // the byte does not matter, COPY takes the cache
            if (s_runLen && s_runLen < 0x1FFF && off == (uint16_t)(s_runOff + s_runLen) &&
                src == (uint16_t)(s_runSrc + s_runLen)) { s_runLen++; return; }
            runFlush();
            s_runOff = off; s_runSrc = src; s_runLen = 1;
            return;
        }
        byteAppend(off, v, wasRead);
    } else execVram(off, v, wasRead, src);
}

// core0: n COPY writes in a row (Z80 DMA inside the window), the same as n writeVram
// calls with wasRead and the mode on COPY.
void copyRun(uint16_t off, uint16_t src, uint32_t n) {
    s_fbDirty = true;
    if (!queued()) { for (uint32_t i = 0; i < n; i++) execVram((uint16_t)(off + i), 0, true, (uint16_t)(src + i)); return; }
    while (n) {
        if (!(s_runLen && s_runLen < 0x1FFF && off == (uint16_t)(s_runOff + s_runLen) && src == (uint16_t)(s_runSrc + s_runLen))) {
            runFlush();
            s_runOff = off; s_runSrc = src;
        }
        const uint32_t c = n < 0x1FFFu - s_runLen ? n : 0x1FFFu - s_runLen;
        s_runLen = (uint16_t)(s_runLen + c);
        off = (uint16_t)(off + c); src = (uint16_t)(src + c); n -= c;
    }
}

// core0: n plain bytes in a row of the window (FastDMA), never a COPY.
ELM_HOT void writeVramRun(uint16_t off, const uint8_t* p, uint32_t n) {
    s_fbDirty = true;
    if (queued()) for (uint32_t i = 0; i < n; i++) byteAppend((uint16_t)(off + i), p[i], false);
    else for (uint32_t i = 0; i < n; i++) execVram((uint16_t)(off + i), p[i], false, 0xFFFF);
}

// The whole-buffer commands: instant here, busy for their documented time.
static void execCmd(uint8_t c) {
    if (!s_buf || (c != 1 && c != 2)) return;
    const uint8_t m = s_e[REG_MASK];
    const int dn = dstE(), sn = srcE();
    const uint32_t m8 = m * 0x01010101u;         // every stride is a multiple of 4
    if (c == 1 && (dn == sn || !s_b1)) return;
    s_use[dn] = s_use[dn] + (uint32_t)s_stride * s_h * 2;
    if (c == 1) s_use[sn] = s_use[sn] + (uint32_t)s_stride * s_h;
    for (int y = 0; y < s_h; y++) {
        uint8_t* d = rowOf(dn, y);
        if (c == 1) {
            const uint8_t* q = rowOf(sn, y);
            if (m == 0xFF) memcpy(d, q, (size_t)s_stride);
            else for (int i = 0; i < s_stride; i += 4) wr32(d + i, (rd32(d + i) & ~m8) | (rd32(q + i) & m8));
        } else if (m == 0xFF) memset(d, 0, (size_t)s_stride);
        // A word that is clear already is not stored: its PSRAM line stays clean and
        // costs no write-back (a sprite demo clears a mostly black screen every frame).
        else for (int i = 0; i < s_stride; i += 4) {
            const uint32_t w = rd32(d + i);
            if (w & m8) wr32(d + i, w & ~m8);
        }
    }
}

// g_zxi_020 travels the queue like a register: HE2025 flips its layout bit (ZX / linear
// rows) between drawing phases thousands of times a run, and core1 executing the
// previous phase's writes under the new layout put 8-pixel dashes on wrong rows.
void setG20(uint8_t v) {
    s_fbDirty = true;
    g20 = v;
    if (queued()) push(Q_REG | (uint32_t)REG_COUNT << 8 | v); else s_e20 = v;
}

void writeReg(uint8_t r, uint8_t v) {
    if (r >= REG_COUNT) return;
    s_fbDirty = true;
    s_r[r] = v;
    if (r == REG_CMD && (v == 1 || v == 2))
        s_busyUntil = now() + ((uint64_t)(v == 1 ? 6144 : 3500) << ESPectrum::multiplicator);
    if (queued()) {
        push(Q_REG | (uint32_t)r << 8 | v);
        if (r == REG_CMD) push(Q_CMD | v);
    } else {
        s_e[r] = v;
        if (r == REG_CMD) execCmd(v);
    }
}

// Direct mode outlives the window by DIRECT_HOLD frames. PF2026 maps its chunky window
// ~9 times a second; each time ~30 register writes, 16 byte runs and 2 COPY runs sat
// behind the frame's Q_DRAW (bench census), so the move to core0 waited out the beam
// (core1Pump holds Q_DRAW up to 25 ms) and the ~14 ms draw: 7-8 % of core0 (hw PERF_HIST
// 2026-11-16). Its engine work is ~0.7 ms a frame, cheaper on core0 than the waits.
enum { DIRECT_HOLD = 50 };
static bool s_directWant;
static uint8_t s_directHold;
void direct(bool on, bool now) {
    s_directWant = on;
    if (!on) {
        if (now) { s_direct = false; s_directHold = 0; }
        return;                              // frameEnd ends the hold
    }
    s_directHold = DIRECT_HOLD;
    if (s_direct) return;
    // The engine moves to core0: what core1 still holds of it runs first, or a later
    // chunky store lands under an earlier queued one. The frame's draw only reads the
    // buffers, and in direct mode it runs beside core0's stores anyway: not waited for.
    // sramOff drains in full when it has blocks to move.
    drainWait(true);
    sramOff(true);
    s_direct = true;
}

uint8_t reg(uint8_t r) { return r < REG_COUNT ? s_r[r] : 0; }

void writeColour(uint16_t off, uint8_t v) {
    if (s_col && off < COLOURS && s_col[off] != v) { s_col[off] = v; s_palDirty = true; }
}

uint8_t status() { return now() < s_busyUntil ? 1 : 0; }

static inline uint32_t chunkyAbs(uint32_t off) {
    return srcOff() + ((off + (uint32_t)(uint16_t)s16(REG_OY) * s_stride) & (BUF_SIZE - 1));
}
uint8_t* chunkyRead(uint32_t off, bool lowSlot, bool* split) {
    sramOff(true);
    *split = false;
    if (!s_buf) return nullptr;
    const uint32_t a = chunkyAbs(off);
    if (s_h1) {
        // The guest reads the window as one raw 8 KB run: wholly inside the SRAM half or
        // wholly outside it. Across the edge: a split window (its reads go through
        // chunkyReadByte) where the generic read path serves the slot, else the half
        // goes back to PSRAM and Elm::remap maps every slot again (an earlier slot of
        // this pass may point in).
        const uint32_t r = a - s_h1Base;
        const bool in = r < s_h1Size, across = in ? r + 0x2000u > s_h1Size : (a < s_h1Base && a + 0x2000u > s_h1Base);
        if (across) {
            if (lowSlot) { *split = true; return in ? s_h1 + r : s_buf + a; }   // the raw pointer serves a fetch only
            h1Off(true, in ? "window across the end" : "window across the start"); s_h1Stale = true; s_h1Edge++;
        } else if (in) return s_h1 + r;
    }
    return s_buf + a;
}
// A byte of a split window (Elm::splitRead), and a run of the window for the FastDMA.
uint8_t chunkyReadByte(uint32_t off) { return s_buf ? *at(chunkyAbs(off)) : 0xFF; }
void chunkyReadRun(uint32_t off, uint8_t* p, uint32_t n) {
    if (!s_buf) { memset(p, 0xFF, n); return; }
    uint32_t o = (off + (uint32_t)(uint16_t)s16(REG_OY) * s_stride) & (BUF_SIZE - 1);
    const uint32_t base = srcOff();
    while (n) {
        uint32_t c = n < BUF_SIZE - o ? n : BUF_SIZE - o;
        const uint32_t a = base + o;
        if (s_h1) {
            if (a < s_h1Base) { if (c > s_h1Base - a) c = s_h1Base - a; }
            else if (a - s_h1Base < s_h1Size && c > s_h1Base + s_h1Size - a) c = s_h1Base + s_h1Size - a;
        }
        memcpy(p, at(a), c);
        p += c; n -= c; o = (o + c) & (BUF_SIZE - 1);
    }
}
bool chunkyStale() { const bool v = s_h1Stale; s_h1Stale = false; return v; }

// Out of the flash: a Z80 loop that stores a pixel at a time (HGFXZOOM's rotating fox,
// LD (HL),A per pixel) came here through two flash calls a byte.
ELM_HOT void chunkyWrite(uint32_t off, uint8_t v) {
    if (!s_buf) return;
    s_fbDirty = true;
    if ((s_r[REG_VP] & 0x10) && !v) return;
    uint8_t* d = at(dstOff() + ((off + (uint32_t)(uint16_t)s16(REG_OY) * s_stride) & (BUF_SIZE - 1)));
    *d = planar(*d, v, s_r[REG_MASK]);
}

void chunkyWriteRun(uint32_t off, const uint8_t* p, uint32_t n) {
    if (!s_buf) return;
    s_fbDirty = true;
    const uint8_t m = s_r[REG_MASK];
    const bool skip0 = s_r[REG_VP] & 0x10;
    uint32_t o = (off + (uint32_t)(uint16_t)s16(REG_OY) * s_stride) & (BUF_SIZE - 1);
    const uint32_t base = dstOff();
    while (n) {
        uint32_t c = n < BUF_SIZE - o ? n : BUF_SIZE - o;             // the buffer wraps
        const uint32_t a = base + o;
        if (s_h1) {                                                   // a piece stays on one side of the SRAM half's edges
            if (a < s_h1Base) { if (c > s_h1Base - a) c = s_h1Base - a; }
            else if (a - s_h1Base < s_h1Size && c > s_h1Base + s_h1Size - a) c = s_h1Base + s_h1Size - a;
        }
        uint8_t* d = at(a);
        if (m == 0xFF && !skip0) memcpy(d, p, c);   // word stores into the PSRAM buffer (a byte loop was slower, hw 2026-11-16)
        else for (uint32_t i = 0; i < c; i++) { if (skip0 && !p[i]) continue; d[i] = planar(d[i], p[i], m); }
        p += c; n -= c; o = (o + c) & (BUF_SIZE - 1);
    }
}

// Hardware slots: 0..15 stay the ZX colours (the layer under index 0), the UI block
// and the HDMI Data Island words are out, the rest takes the table's colours.
static uint8_t kSlots(int i) { return (uint8_t)(i < 136 ? 16 + i : i < 152 ? 168 + i - 136 : 200 + i - 152); }

static inline uint32_t tableRgb(const uint8_t* c) {
    return rgbOrder ? ((uint32_t)c[0] << 16 | c[1] << 8 | c[2]) : ((uint32_t)c[2] << 16 | c[1] << 8 | c[0]);
}
static inline uint32_t dist(uint32_t a, uint32_t b) {
    const int dr = (int)(a >> 16) - (int)(b >> 16), dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255),
              db = (int)(a & 255) - (int)(b & 255);
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

// The 256 indices on 168 slots. An index keeps its slot while it can: a colour change is
// written into that slot, so the picture already in the framebuffer takes the new colour
// at once, as on the real HGFX, where the table is applied at display time. Handing the
// slots out again in the order the colours first appear (the old rule) renumbered them
// on every step of a fade: core1 draws the next frame later, and until then the old
// pixels showed under the new numbers (Nether Earth's title fade, "negative" frames).
// all: the slots were rewritten under us (cube, pair frame, applyPalette).
static void paletteProgram(bool all) {
    Hot* H = s_hot;
    uint32_t* rgb = H->rgb;
    for (int i = 0; i < 256; i++) rgb[i] = tableRgb(s_col + i * 3);
    memset(H->claim, 0, sizeof H->claim);
    auto take = [&](int k, uint32_t c) {
        H->claim[k] = 1;
        if (all || H->slotCol[k] != c) { H->slotCol[k] = c; VIDEO::elmPaletteSet(kSlots(k), c); }
    };
    // An index on its own colour keeps the slot; a second index on that slot shares it if
    // the colours still match, otherwise it moves.
    for (int i = 0; i < 256; i++) {
        const uint8_t k = H->mapK[i];
        if (k == 0xFF || H->approx[i]) { H->mapK[i] = 0xFF; continue; }
        if (!H->claim[k]) take(k, rgb[i]);
        else if (H->slotCol[k] != rgb[i]) H->mapK[i] = 0xFF;
    }
    int freeK = 0;
    bool folded = false;
    for (int i = 0; i < 256; i++) {
        if (H->mapK[i] != 0xFF) { H->approx[i] = 0; continue; }
        int hit = -1;
        for (int k = 0; k < 168 && hit < 0; k++) if (H->claim[k] && H->slotCol[k] == rgb[i]) hit = k;
        if (hit < 0) {
            while (freeK < 168 && H->claim[freeK]) freeK++;
            // Out of slots: the sticky rule keeps a slot for every index that once had a
            // colour of its own, also after a fade brought them to the same colour, and
            // nothing gave those back (Tetris: 152 colours, up to 126 doubles; the text
            // and block colours 192..255 then sat on the nearest grey until a reset).
            // Fold the doubles once, then look again.
            if (freeK == 168 && !folded) {
                folded = true;
                for (int k = 1; k < 168; k++) {
                    if (!H->claim[k]) continue;
                    int j = 0;
                    while (j < k && !(H->claim[j] && H->slotCol[j] == H->slotCol[k])) j++;
                    if (j == k) continue;
                    for (int m = 0; m < 256; m++) if (H->mapK[m] == k) H->mapK[m] = (uint8_t)j;
                    H->claim[k] = 0;
                }
                freeK = 0;
                while (freeK < 168 && H->claim[freeK]) freeK++;
            }
            if (freeK < 168) { hit = freeK; take(hit, rgb[i]); }
        }
        H->approx[i] = 0;
        if (hit < 0) {                   // more colours than slots: the nearest
            uint32_t best = ~0u;
            for (int k = 0; k < 168; k++) {
                if (!H->claim[k]) continue;
                const uint32_t d = dist(H->slotCol[k], rgb[i]);
                if (d < best) { best = d; hit = k; }
            }
            H->approx[i] = 1;
        }
        H->mapK[i] = (uint8_t)hit;
    }
#if HOST_BUILD
    if (getenv("PICOSPECCY_ELMPAL")) {
        int cl = 0, ap = 0, dup = 0;
        for (int k = 0; k < 168; k++) { if (!H->claim[k]) continue; cl++; for (int j = 0; j < k; j++) if (H->claim[j] && H->slotCol[j] == H->slotCol[k]) { dup++; break; } }
        for (int i = 0; i < 256; i++) ap += H->approx[i];
        static int s_cl = -1, s_ap = -1, s_dup = -1;
        if (cl != s_cl || ap != s_ap || dup != s_dup) { s_cl = cl; s_ap = ap; s_dup = dup; Debug::log("[ELMPAL] slots %d dup %d approx %d", cl, dup, ap); }
    }
#endif
    for (int i = 0; i < 256; i++) H->map[i] = kSlots(H->mapK[i]);
}

// The ZX layer of one row, colours 0..15.
static void zxLine(const uint8_t* scr, int y, uint8_t* out) {
    const uint32_t b = ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2);
    const uint8_t* attr = scr + 0x1800 + (y >> 3) * 32;
    const bool flash = VIDEO::flashing != 0;
    for (int c = 0; c < 32; c++) {
        const uint8_t a = attr[c];
        uint8_t bits = scr[b + c];
        if (flash && (a & 0x80)) bits = (uint8_t)~bits;
        const uint8_t br = (a & 0x40) ? 8 : 0;
        const uint32_t i8 = ((a & 7) | br) * 0x01010101u, p8 = (((a >> 3) & 7) | br) * 0x01010101u;
        const uint32_t bl = s_hot->nib[bits >> 4], bh = s_hot->nib[bits & 15];
        wr32(out, (i8 & bl) | (p8 & ~bl));
        wr32(out + 4, (i8 & bh) | (p8 & ~bh));
        out += 8;
    }
}

// HRC / HRXC: 64 bytes a line, one attribute per 8x4. HRC gives ZX colours 0..15; HRXC
// the 16 INK (0..15) and 16 PAPER (16..31) of its table.
static void hrLine(const uint8_t* scr, int y, bool indexed, uint8_t* out) {
    const uint8_t* bm = scr + (uint32_t)y * 64;
    const uint8_t* at = scr + 0x3000 + (y >> 2) * 64;
    const bool flash = VIDEO::flashing != 0;
    for (int c = 0; c < 64; c++) {
        const uint8_t a = at[c];
        uint8_t bits = bm[c], ink, pap;
        if (indexed) { ink = a & 15; pap = (uint8_t)(16 | (a >> 4)); }
        else {
            if (flash && (a & 0x80)) bits = (uint8_t)~bits;
            const uint8_t br = (a & 0x40) ? 8 : 0;
            ink = (a & 7) | br; pap = ((a >> 3) & 7) | br;
        }
        for (int i = 0; i < 8; i++, bits <<= 1) *out++ = (bits & 0x80) ? ink : pap;
    }
}

// The pair frame has 16 colours, four of them the ZX 12..15. `hist` picks the other
// twelve, `rgbOf` maps every source index to the nearest.
static void pairLutUpdate();
// The picture rows of the framebuffer in one slot: the pair driver reads the bytes as
// pairs the moment it is switched on, and the ZX palette the moment it is off, so the
// frame between the switch and core1's first redraw showed the old bytes through the
// wrong table (LIMSHOWM between pictures: a green block, then coloured blocks; owner's
// recording 2026-11-16 17:14). Black in the table about to be live instead.
static volatile bool s_pairStale = true;   // the pair FB no longer holds the last draw (cleared, repainted over)
static void pairFbClear(uint8_t slot) {
    const int xres = VIDEO::vga.xres, x0 = (xres - WIDTH) / 2, y0 = VIDEO::screenTopRows();
    for (int y = 0; y < HEIGHT; y++) memset(VIDEO::vga.frameBuffer[y0 + y] + x0, slot, WIDTH);
    s_pairStale = true;
}
static void pairOff() {
    s_pairPendValid = false; g_pairPushNow = false;
    if (!s_pair) return;
    drain();
    pairFbClear(0);
    s_pair = false; VIDEO::samPairSync(false, nullptr);
}
static void pairPalette(const uint32_t* hist, const uint32_t* rgbOf, int n) {
    uint32_t pal[16]; uint32_t* h = s_hot->h;
    memcpy(h, hist, n * sizeof(uint32_t));
    bool full = false;
    const bool had = s_pair;
    if (mode == 2) {
        // HRXC: a slot belongs to a TABLE INDEX for as long as the index is in use. A fade
        // changes the colours every frame, never the pixels: the slot keeps its index and
        // takes the new colour, the pixels drawn through it are that index's, nothing on
        // screen changes slot. Dealing slots by colour (frequency order, then nearest-to-
        // last-frame) moved indices between slots as the fading colours crossed and
        // folded, and the frame already on screen showed those pixels through the new
        // owner's colour for a frame: speckles all over LIMSHOWM's fades (owner's
        // recordings 2026-11-16 17:14 / 17:27). Indices beyond the slots map to the nearest
        // slot colour, no slot moves for them. The 12 / 16 question is sticky too (the
        // count crossing 12 mid-fade sent slots 12-15 back to the ZX brights).
        static uint8_t s_slotIdx[16];      // slot -> table index, 0xFF free
        static bool s_pairFull;
        // The palette and the bytes drawn through it must reach the screen TOGETHER (the
        // Kolbass rule, tsconf.md): pushed at once from here, a pixel whose index moved to
        // another slot showed through its OLD slot - now owned by an index a fade-in had
        // brightened - until the 18 ms redraw reached it (speckles over the dark picture,
        // hw 2026-11-16; the counters: 60-136 pidx moves in a fade-in's first 60 frames);
        // pushed a frame late it broke the fade-out instead. So the palette is staged and
        // core1 pushes it right before the draw it belongs to, a draw that starts with
        // the beam below the picture (core1Pump's gate) and, a cell at a time
        // (pairRowFast), finishes well inside the beam's lead.
        if (!had) { memset(s_slotIdx, 0xFF, sizeof s_slotIdx); s_pairFull = false; s_pairPendValid = false; g_pairPushNow = false; }
        else if (s_pairPendValid) {          // still due (nobody took it): now, before the next is staged
#if PERF_TRACE
            s_pPushLate++;
#endif
            pairPushPoll();
        }
        int m = 0;
        for (int k = 0; k < n; k++) if (h[k]) m++;
        full = m > 12 || (had && s_pairFull);
        s_pairFull = full;
        const int slots = full ? 16 : 12;
        for (int i = 0; i < 16; i++) {
            if (i >= slots) { s_slotIdx[i] = 0xFF; pal[i] = VIDEO::zxRgb((uint8_t)i); continue; }
            const uint8_t k = s_slotIdx[i];
            if (k != 0xFF && k < n && h[k]) pal[i] = rgbOf[k];                 // the owner, its colour of this frame
            else { if (k != 0xFF) s_slotIdx[i] = 0xFF; pal[i] = had ? s_hot->pal[i] : 0; }   // free: keeps its colour
        }
        // New indices, the heaviest first, into free slots: the one whose old colour is nearest.
        bool owned[64];
        for (int k = 0; k < n; k++) owned[k] = false;
        for (int i = 0; i < slots; i++) if (s_slotIdx[i] != 0xFF) owned[s_slotIdx[i]] = true;
        for (;;) {
            int best = -1;
            for (int k = 0; k < n; k++) if (h[k] && !owned[k] && (best < 0 || h[k] > h[best])) best = k;
            if (best < 0) break;
            int fs = -1; uint32_t bd = ~0u;
            for (int i = 0; i < slots; i++) if (s_slotIdx[i] == 0xFF) { const uint32_t d = dist(pal[i], rgbOf[best]); if (d < bd) { bd = d; fs = i; } }
            if (fs < 0) break;                                                  // more indices than slots: the rest map to the nearest
            s_slotIdx[fs] = (uint8_t)best; pal[fs] = rgbOf[best]; owned[best] = true;
        }
        memcpy(s_hot->pal, pal, sizeof(pal));
        // An index without a slot (the pictures have up to 32 colours on 16 slots) maps to
        // the nearest slot colour - with hysteresis: it keeps last frame's slot unless a
        // new one is clearly nearer. A fade-in from black separates the colours by coarse
        // 3-bit steps and the nearest choice flipped on near-ties: 136 pidx changes in 60
        // frames, 21 pair-LUT re-deals, speckles (hw counters 2026-11-16 17:44); the
        // fade-out, the same colours converging, had 0.
        for (int k = 0; k < n; k++) {
            uint32_t bd = ~0u; int b = 0;
            for (int i = 0; i < 16; i++) { const uint32_t d = dist(pal[i], rgbOf[k]); if (d < bd) { bd = d; b = i; } }
            if (had && s_hot->pidx[k] < 16) {
                const uint32_t dk = dist(pal[s_hot->pidx[k]], rgbOf[k]);
                if (dk <= bd * 2 + 3 * 12 * 12) b = s_hot->pidx[k];
            }
            s_hot->pidx[k] = (uint8_t)b;
        }
        for (int i = 0; i < slots; i++) if (s_slotIdx[i] != 0xFF) s_hot->pidx[s_slotIdx[i]] = (uint8_t)i;   // the owner wins over an equal colour
#if PERF_TRACE
        { static uint8_t pp[64]; static uint32_t pq[16]; static bool pf;
          for (int k = 0; k < n && k < 64; k++) { if (pp[k] != s_hot->pidx[k]) s_pPidxChg++; pp[k] = s_hot->pidx[k]; }
          for (int i = 0; i < 16; i++) { if (pq[i] != pal[i]) s_pPalChg++; pq[i] = pal[i]; }
          if (pf != full) s_pFullFlip++; pf = full; }
#endif
        pairLutUpdate();                 // with the new slots; the driver gets both with the palette
        if (!had) {                      // the first frame: nothing on screen to keep consistent
            drain(); pairFbClear(VIDEO::profi_pair_lookup[0][0]);   // black pair until core1 draws
            s_pair = VIDEO::samPairSync(true, pal, full ? pal + 12 : nullptr);
        } else {                         // staged: core1 pushes it right before the draw it belongs to (pairPendApply)
            memcpy(s_pairPendPal, pal, sizeof pal); s_pairPendFull = full;
            __sync_synchronize();
            s_pairPendValid = true;
        }
        return;
    }
    // A small table (HRXC: 32 entries, WOWDEMO uses 16 a picture) with more than twelve
    // colours in use: the twelve most frequent ones dropped whole hues, and the nearest
    // left for skin and hair were the ZX bright white and yellow (owner 2026-11-14).
    // Merge instead: the colour that costs least (its weight x the distance to the
    // nearest one staying, the four ZX ones included) goes, its weight to the neighbour.
    // HRXC (owner 2026-11-14): all sixteen pair colours are the picture's when it uses
    // more than twelve; the ZX 12..15 the overlays count on come back with a calmer one.
    const int room = mode == 2 ? 16 : 12;
    if (n <= 64) {
        // The same colour under several indices is one colour (HRXC: INK and PAPER
        // halves, two screens): without this the merge below ran every frame. Not HRC:
        // its pair index IS the ZX index, and black 0 / 8 folded moved the rest.
        if (mode != 1) for (int k = 1; k < n; k++) {
            if (!h[k]) continue;
            for (int j = 0; j < k; j++) if (h[j] && rgbOf[j] == rgbOf[k]) { h[j] += h[k]; h[k] = 0; break; }
        }
        int used = 0;
        for (int k = 0; k < n; k++) if (h[k]) used++;
        while (used > room) {
            int drop = -1, into = -1; uint64_t cost = ~0ull;
            for (int k = 0; k < n; k++) {
                if (!h[k]) continue;
                uint32_t bd = ~0u; int b = -1;
                for (int j = 0; j < n; j++) {
                    if (j == k || !h[j]) continue;
                    const uint32_t d = dist(rgbOf[j], rgbOf[k]);
                    if (d < bd) { bd = d; b = j; }
                }
                if (room == 12) for (int z = 12; z < 16; z++) {
                    const uint32_t d = dist(VIDEO::zxRgb((uint8_t)z), rgbOf[k]);
                    if (d < bd) { bd = d; b = -1; }
                }
                const uint64_t c = (uint64_t)bd * h[k];
                if (c < cost) { cost = c; drop = k; into = b; }
            }
            if (drop < 0) break;
            if (into >= 0) h[into] += h[drop];
            h[drop] = 0;
            used--;
        }
    }
    if (mode == 1) {                     // HRC: the ZX colours, pair index = ZX index
        for (int i = 0; i < 12; i++) {
            int best = 0;
            for (int k = 1; k < n; k++) if (h[k] > h[best]) best = k;
            pal[i] = rgbOf[best];
            h[best] = 0;
        }
        for (int i = 12; i < 16; i++) pal[i] = VIDEO::zxRgb((uint8_t)i);
    } else {
        // The colours that stay, the heaviest first, then each into the slot whose
        // colour of the previous frame is nearest. The palette is pushed before the
        // new pixels are drawn, so for a moment the old picture is shown through it:
        // in frequency order every picture of a video renumbered the slots and that
        // moment was a flash of wrong colours (owner on the host, 2026-11-14).
        uint32_t want[16]; int m = 0;
        for (; m < room; m++) {
            int best = -1;
            for (int k = 0; k < n; k++) if (h[k] && (best < 0 || h[k] > h[best])) best = k;
            if (best < 0) break;
            want[m] = rgbOf[best];
            h[best] = 0;
        }
        // Sticky while the pair frame is up: a fade folds colours into each other, the
        // count crosses 12 and slots 12-15 went back to the ZX bright white / yellow /
        // cyan / magenta under pixels still drawn through them - core1's 18 ms redraw
        // against the beam showed them as flashes in LIMSHOWM's fades (hw 2026-11-16).
        static bool s_pairFull;
        const bool had = s_pair;
        if (!had) s_pairFull = false;
        full = m > 12 || (had && s_pairFull);
        s_pairFull = full;
        const int slots = full ? 16 : 12;
        for (int i = 0; i < 16; i++) pal[i] = had && i < slots ? s_hot->pal[i] : i < 12 ? 0 : VIDEO::zxRgb((uint8_t)i);
        uint16_t usedS = 0, usedC = 0;
        for (int a = 0; a < m; a++) {
            uint32_t bd = ~0u; int bs = 0, bc = 0;
            for (int c = 0; c < m; c++) {
                if (usedC & (1u << c)) continue;
                for (int sl = 0; sl < slots; sl++) {
                    if (usedS & (1u << sl)) continue;
                    const uint32_t d = had ? dist(s_hot->pal[sl], want[c]) : (uint32_t)(sl != c);
                    if (d < bd) { bd = d; bs = sl; bc = c; }
                }
            }
            usedS |= (uint16_t)(1u << bs); usedC |= (uint16_t)(1u << bc);
            pal[bs] = want[bc];
        }
    }
    memcpy(s_hot->pal, pal, sizeof(pal));
    for (int k = 0; k < n; k++) {
        uint32_t bd = ~0u; int b = 0;
        for (int i = 0; i < 16; i++) { const uint32_t d = dist(pal[i], rgbOf[k]); if (d < bd) { bd = d; b = i; } }
        s_hot->pidx[k] = (uint8_t)b;
    }
    if (mode == 2) pairLutUpdate();      // with the new slots, before the driver gets both
    if (!s_pair) { drain(); pairFbClear(VIDEO::profi_pair_lookup[0][0]); }   // black pair until core1 draws
    s_pair = VIDEO::samPairSync(true, pal, full ? pal + 12 : nullptr);
}

// g_zxi_020 bit 1: the border shows HGFX colour index 0..7 (the #FE value) instead of
// the ZX colour. HGFX 4/5 fill the frame and have no border; the 4-colour pair frames
// know only their own indices.
static inline bool indexedBorder() { return s_live && s_hot && mode >= 3 && (g20 & 2); }

uint8_t pairBorder(uint8_t zx) {
    if (!s_hot) return zx;
    if (indexedBorder() && !s_zxPair) return s_hot->pidx[(zx & 7) & (mode >= 6 && mode <= 7 ? 3 : 7)];
    if (!s_zxPair) return s_hot->zxp[zx & 15];
    return zx & 15;
}

int borderSlot(uint8_t zx) {
    return indexedBorder() && !s_pair ? s_hot->map[zx & 7] : -1;
}

// The border word follows the palette: a colour change of index 0..7 moves no slot,
// but a full reprogram or a pair switch can. force: the extra video just ended, and a
// pair frame (HRC) left brd on a pair-table entry (a brown border after HRC).
static void borderFollow(bool force = false) {
    static bool s_was;
    const bool on = indexedBorder();
    if (!on && !s_was && !force) return;
    s_was = on;
    const uint32_t old = VIDEO::brd;
    if (!on && VIDEO::ulaplus_enabled) VIDEO::ulaPlusUpdateBorder(); else VIDEO::updateBorderBrd();
    if (VIDEO::brd != old) VIDEO::brdChange = VIDEO::brdnextframe = true;
}

static inline void putCarved(uint8_t* row, int fx, uint8_t v, int nc, int (*cx)[2]) {
    for (int k = 0; k < nc; k++) if (fx >= cx[k][0] && fx < cx[k][1]) return;
    row[fx ^ 2] = v;
}

// core1 between picture rows: the background SD read (sdcard.c sd_async_pump) posted by
// DivMMC's c17 path. A whole HGFX frame is one ~14 ms call on core1; pumped only at the
// top of render_core's loop, a posted sector waited past DivMMC's 3 ms and core0 read it
// itself. The Next pumps between its line batches the same way. One byte test otherwise.
#if PERF_TRACE
static uint32_t s_pRowPump, s_pRowSkip;   // row pumps given to the NeoGS / withheld (beam too close)
#endif
#if !HOST_BUILD
#include "pico/platform.h"           // get_core_num
extern "C" volatile uint8_t g_sd_as_state;
extern "C" void sd_async_pump(void);
// ...and the NeoGS: its GS-Z80 runs on core1 off the wall clock (GS::pump in main.cpp's
// loop), and a whole frame drawn in one core1Pump call starved it for the draw's length
// (11-18 ms: one catch-up burst a frame, "the NeoGS sounds terrible", LIMSHOWM, owner
// 2026-11-16). A pump every row keeps it fed (rowPump below).
static inline void gsPump() {
    if (GS::enabled && get_core_num() == 1) GS::pump();
}
static inline void gatePump() {          // core1 waiting at the palette gate: the SD and the NeoGS go on
    if (__builtin_expect(g_sd_as_state == 1, 0)) sd_async_pump();
    gsPump();
}
// fy: the fb row about to be drawn. A pump every 8 rows (split23) fed the card 4000 T a
// call (GS::pump's chunk) for 0.5 ms of wall clock: a third of its rate through the draw
// (owner 2026-11-16: "still not clean"). Every row now, but only while the draw leads the
// beam by ROW_LEAD rows: the card eats core1 time the row needs, and a draw the beam
// catches is the sand again. Beam below the row (or blanking): it wrapped, plenty of lead.
static inline void rowPump(int fy) {
    if (__builtin_expect(g_sd_as_state == 1, 0) && get_core_num() == 1) sd_async_pump();
    enum { ROW_LEAD = 24 };
    const int beam = VIDEO::displayBeamRow();
    if (beam < 0 || beam >= fy || fy - beam >= ROW_LEAD) {
#if PERF_TRACE
        s_pRowPump++;
#endif
        gsPump();
    }
#if PERF_TRACE
    else s_pRowSkip++;
#endif
}
#else
static inline void gsPump() {}
static inline void gatePump() {}
static inline void rowPump(int) {}
#endif
// One fb row from 2 output px per byte, colours in l[].
static void outPair(int fy, int x0, const uint8_t* l, int bytes, const uint8_t* lut = &VIDEO::profi_pair_lookup[0][0]) {
    rowPump(fy);
    uint8_t* row = VIDEO::vga.frameBuffer[fy];
    int cx[8][2];
    const int nc = VIDEO::rowCarves(fy, cx);
    for (int x = 0; x < bytes; x++)
        putCarved(row, x0 + x, lut[l[x * 2] * 16 + l[x * 2 + 1]], nc, cx);
}

// HRC / HRXC pair frame a CELL at a time (8 px = 4 fb bytes): the cell's INK / PAPER give
// four pair bytes through the driver table, the 8 pixel bits pick one per pair, one word
// store. The per-pixel way (hrLine -> pidx -> outPair, 512 pixels a row through
// putCarved) took core1 18 ms a frame (hw 2026-11-16), longer than the beam's sweep.
// KeyLayer: a cell whose INK and PAPER are both the key is the bottom screen's cell, one
// with neither is the top one's; a cell with one of them keeps the per-pixel merge.
#if PERF_TRACE
static uint32_t s_pCellTop, s_pCellBot, s_pCellMix;   // KeyLayer cells per 60 frames: top screen / bottom / mixed
#endif
// In the RAM window (ELM_HOT): on core1 flash code runs through the XIP cache core0 keeps
// busy, and this loop from flash took 21 ms a frame (hw 2026-11-16), MORE than the per-pixel
// path it replaced.
ELM_HOT void pairRowFast(const uint8_t* scr, const uint8_t* scr2, int y, bool indexed, const uint8_t* lut, int fy, int x0) {
    rowPump(fy);
    Hot* H = s_hot;
    uint8_t* row = VIDEO::vga.frameBuffer[fy];
    int cx[8][2];
    const int nc = VIDEO::rowCarves(fy, cx);
    const uint8_t* bm = scr + (uint32_t)y * 64;
    const uint8_t* at = scr + 0x3000 + (y >> 2) * 64;
    const uint8_t* bm2 = scr2 ? scr2 + (uint32_t)y * 64 : nullptr;
    const uint8_t* at2 = scr2 ? scr2 + 0x3000 + (y >> 2) * 64 : nullptr;
    const bool flash = VIDEO::flashing != 0;
    const uint8_t own = indexed ? 32 : 0;
    const bool fast = !nc && !(x0 & 3);
    const uint8_t* pidx = H->pidx;
    uint16_t* cnt2 = indexed ? s_cnt2 : nullptr;
    for (int c = 0; c < 64; c++) {
        uint8_t bits = bm[c];
        const uint8_t a = at[c];
        uint8_t ink, pap;
        if (indexed) { ink = a & 15; pap = (uint8_t)(16 | (a >> 4)); }
        else {
            if (flash && (a & 0x80)) bits = (uint8_t)~bits;
            const uint8_t br = (a & 0x40) ? 8 : 0;
            ink = (a & 7) | br; pap = ((a >> 3) & 7) | br;
        }
        uint8_t b0, b1, b2, b3;
        bool plain = true;
        if (scr2) {
            const bool kI = keyIs(ink), kP = keyIs(pap);
            if (kI || kP) {
                uint8_t bits2 = bm2[c], ink2, pap2;
                const uint8_t a2 = at2[c];
                if (indexed) { ink2 = (a2 & 15) + own; pap2 = (uint8_t)((16 | (a2 >> 4)) + own); }
                else {
                    if (flash && (a2 & 0x80)) bits2 = (uint8_t)~bits2;
                    const uint8_t br2 = (a2 & 0x40) ? 8 : 0;
                    ink2 = (a2 & 7) | br2; pap2 = ((a2 >> 3) & 7) | br2;
                }
                if (kI && kP) { bits = bits2; ink = ink2; pap = pap2; }
                else {
                    // Mixed: a pixel is the top screen's INK / PAPER, or the bottom screen's
                    // where the top one is the key. ix[]: the four candidates by table index,
                    // sel[top bit][bottom bit] picks one; the pairs then go as below.
                    plain = false;
                    const uint8_t ix[4] = { pap, ink, pap2, ink2 };
                    const uint8_t sel[2][2] = { { (uint8_t)(kP ? 2 : 0), (uint8_t)(kP ? 3 : 0) }, { (uint8_t)(kI ? 2 : 1), (uint8_t)(kI ? 3 : 1) } };
                    uint8_t sl[4];
                    for (int i = 0; i < 4; i++) sl[i] = indexed ? pidx[ix[i]] : ix[i];
                    uint8_t out[4];
                    for (int i = 0; i < 4; i++) {
                        const int sh = 6 - 2 * i;
                        const uint8_t l = sel[(bits >> (sh + 1)) & 1][(bits2 >> (sh + 1)) & 1], r = sel[(bits >> sh) & 1][(bits2 >> sh) & 1];
                        if (cnt2) cnt2[(ix[l] & 63) << 6 | (ix[r] & 63)]++;
                        out[i] = lut[sl[l] * 16 + sl[r]];
                    }
                    b0 = out[0]; b1 = out[1]; b2 = out[2]; b3 = out[3];
#if PERF_TRACE
                    s_pCellMix++;
#endif
                }
#if PERF_TRACE
                if (plain) s_pCellBot++;
#endif
            }
#if PERF_TRACE
            else s_pCellTop++;
#endif
        }
        if (plain) {
            if (cnt2) {                  // the pairs of this cell by table index (pairLutUpdate)
                const uint32_t hi = bits & 0xAA, lo = (bits & 0x55) << 1;   // left / right pixel of each pair, aligned
                const uint32_t v11 = hi & lo, v10 = hi & ~lo, v01 = ~hi & lo & 0xAA;
                const uint32_t n11 = (v11 >> 7) + ((v11 >> 5) & 1) + ((v11 >> 3) & 1) + ((v11 >> 1) & 1);
                const uint32_t n10 = (v10 >> 7) + ((v10 >> 5) & 1) + ((v10 >> 3) & 1) + ((v10 >> 1) & 1);
                const uint32_t n01 = (v01 >> 7) + ((v01 >> 5) & 1) + ((v01 >> 3) & 1) + ((v01 >> 1) & 1);
                const uint32_t i6 = ink & 63, p6 = pap & 63;
                cnt2[i6 << 6 | i6] += n11; cnt2[i6 << 6 | p6] += n10;
                cnt2[p6 << 6 | i6] += n01; cnt2[p6 << 6 | p6] += 4 - n11 - n10 - n01;
            }
            const uint8_t A = indexed ? pidx[ink] : ink, B = indexed ? pidx[pap] : pap;
            const uint8_t t[4] = { lut[B * 16 + B], lut[B * 16 + A], lut[A * 16 + B], lut[A * 16 + A] };   // code = left << 1 | right, 1 = INK
            b0 = t[bits >> 6]; b1 = t[(bits >> 4) & 3]; b2 = t[(bits >> 2) & 3]; b3 = t[bits & 3];
        }
        if (fast) *(uint32_t*)(row + x0 + c * 4) = b2 | (uint32_t)b3 << 8 | (uint32_t)b0 << 16 | (uint32_t)b1 << 24;
        else {
            putCarved(row, x0 + c * 4, b0, nc, cx); putCarved(row, x0 + c * 4 + 1, b1, nc, cx);
            putCarved(row, x0 + c * 4 + 2, b2, nc, cx); putCarved(row, x0 + c * 4 + 3, b3, nc, cx);
        }
    }
}

// The pair driver has fewer slots than 16 x 16 pairs (250, 210 with HDMI audio), and the
// shared table gives up fixed ones: right pixel 8 after a left 0..5, bright on another
// bright. A photo in 16 colours uses those like any other pair (HRXCIMGS: dark dots, the
// right pixel of a pair in the colour of slot 0; owner 2026-11-14) but not all 240 mixed
// pairs, so HRXC deals the slots by the picture: the pairs the last frame drew keep
// theirs, the rarest go to the nearest pair that stays. The driver shows a shared slot
// in the colours of its FIRST pair in table order, hence a target before the pair.
// The (c, c) slots stay where the shared table has them (overlays, border).
static void pairLutUpdate() {
    Hot* H = s_hot;
    const uint8_t* std = &VIDEO::profi_pair_lookup[0][0];
    if (!s_cnt2) s_cnt2 = (uint16_t*)tryCalloc(8192);
    if (!s_pair || !s_cnt2) { H->lutOn = false; if (s_cnt2) memset(s_cnt2, 0, 8192); return; }
    // Counted by table index and brought to slots HERE, through the palette of the frame
    // about to be drawn: kept by slot they were a frame late whenever the slots were
    // dealt again, and in a fade that is most frames - single pixels in a neighbour's
    // colour for one frame (owner 2026-11-14, LIMSHOWM; 9 such frames a picture).
    memset(H->cnt, 0, sizeof H->cnt);
    for (int i = 0; i < 4096; i++) {
        if (!s_cnt2[i]) continue;
        uint16_t& c = H->cnt[H->pidx[i >> 6] << 4 | H->pidx[i & 63]];
        const uint32_t v = (uint32_t)c + s_cnt2[i];
        c = (uint16_t)(v > 0xFFFF ? 0xFFFF : v);
    }
    const uint8_t* cur = H->lutOn ? H->lut : std;
    // Scratch in Hot::h (pairPalette's, free here): 1 KB more of core0 stack is too much.
    uint8_t* first = (uint8_t*)H->h;     // slot -> the pair that owns it
    uint8_t* fr = first + 256; uint8_t* lut = fr + 256;
    bool* seen = (bool*)(lut + 256);
    bool any = false, bad = false;
    memset(seen, 0, 256);
    for (int p = 0; p < 256; p++) if (!seen[cur[p]]) { seen[cur[p]] = true; first[cur[p]] = (uint8_t)p; }
    for (int p = 0; p < 256; p++) if (H->cnt[p]) { any = true; if (first[cur[p]] != p) bad = true; }
    if (!any) return;                    // nothing drawn yet (the census is zeroed by the draw, kept over skipped ones)
    // The same set of pairs as the last deal keeps the table: a fade changes the palette
    // every frame, not the pixels, and the nearest-surviving-pair choice below follows the
    // palette, so the table was dealt again on every step and the pixels whose slot
    // moved showed through the wrong one until core1's 18 ms redraw caught up (speckles
    // in the dark foreground of LIMSHOWM's fades, owner's recording 2026-11-16 17:14).
    static uint32_t s_lutSig[8];
    uint32_t sig[8] = { 0 };
    for (int p = 0; p < 256; p++) if (H->cnt[p]) sig[p >> 5] |= 1u << (p & 31);
    if (H->lutOn && !memcmp(sig, s_lutSig, sizeof sig)) return;
    memcpy(s_lutSig, sig, sizeof sig);
    if (bad) {
        int nf = 0;
        bool* drop = (bool*)first;       // `first` is done with
        memset(seen, 0, 256); memset(drop, 0, 256);
        for (int c = 0; c < 16; c++) seen[std[c * 17]] = true;
        for (int p = 0; p < 256; p++) if (!seen[std[p]]) { seen[std[p]] = true; fr[nf++] = std[p]; }
        for (int d = 240 - nf; d > 0; d--) {
            int m = -1;
            for (int p = 255; p > 0; p--) if (p % 17 && !drop[p] && (m < 0 || H->cnt[p] < H->cnt[m])) m = p;
            drop[m] = true;
        }
        int k = 0;
        for (int p = 0; p < 256; p++) {
            if (!(p % 17)) { lut[p] = std[p]; continue; }
            if (!drop[p]) { lut[p] = fr[k++]; continue; }
            uint32_t bd = ~0u; int b = 0;
            for (int q = 0; q < p; q++) {
                if (drop[q]) continue;
                const uint32_t dd = dist(H->pal[p >> 4], H->pal[q >> 4]) + dist(H->pal[p & 15], H->pal[q & 15]);
                if (dd < bd) { bd = dd; b = q; }
            }
            lut[p] = lut[b];
        }
        if (!H->lutOn || memcmp(lut, H->lut, 256)) {
            memcpy(H->lut, lut, 256);
            H->lutOn = true;
            VIDEO::samPairLut(H->lut);
#if PERF_TRACE
            s_pLutPush++;
#endif
        }
    }
}
// One fb row of palette slots.
static void outDirect(int fy, int x0, const uint8_t* slots, int n) {
    rowPump(fy);
    uint8_t* row = VIDEO::vga.frameBuffer[fy];
    int cx[8][2];
    const int nc = VIDEO::rowCarves(fy, cx);
    if (nc || (x0 & 3)) { for (int x = 0; x < n; x++) putCarved(row, x0 + x, slots[x], nc, cx); return; }
    uint32_t* d = (uint32_t*)(row + x0);
    for (int x = 0; x + 3 < n; x += 4)
        *d++ = slots[x + 2] | (uint32_t)slots[x + 3] << 8 | (uint32_t)slots[x] << 16 | (uint32_t)slots[x + 1] << 24;
}

// FILL: a 0 repeats the last non-zero pixel from the left.
static void fillLine(uint8_t* p, int n) {
    uint8_t last = 0;
    for (int x = 0; x < n; x++) { if (p[x]) last = p[x]; else p[x] = last; }
}

// HAM8, as on the Amiga: the top two bits say what the low six do: 00 a colour of the
// first 64, 01 blue, 10 red, 11 green. The result goes through the fixed colour cube.
// The cube's dither: 4x4 ordered, a threshold in 1/16 of a level on every channel
// (the old 2x2 at a fixed +-18 left steps on the 85-wide blue levels, owner 2026-11-02).
static const uint8_t kBayer4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static inline uint8_t cubeSlot(uint32_t rgb, int x, int y) {
    const int t = kBayer4[(y & 3) * 4 + (x & 3)] * 2 + 1;          // (0..15 + 0.5) / 16, in 1/32
    const auto lvl = [t](int v, int n) { const int l = (v * (n - 1) * 32 + t * 255) / (255 * 32); return l < n - 1 ? l : n - 1; };
    return s_cube[lvl((int)(rgb >> 16), 6) * 28 + lvl((int)((rgb >> 8) & 255), 7) * 4 + lvl((int)(rgb & 255), 4)];
}
// One row of HAM to RGB.
template <typename F> static inline void hamWalk(const uint8_t* p, int n, F f) {
    uint32_t c = 0;
    for (int x = 0; x < n; x++) {
        const uint8_t b = p[x], v = (uint8_t)((b & 0x3F) << 2);
        switch (b >> 6) {
        case 0: c = tableRgb(s_col + (b & 0x3F) * 3); break;
        case 1: c = (c & 0xFFFF03) | (v);                  break;
        case 2: c = (c & 0x03FFFF) | ((uint32_t)v << 16);  break;
        case 3: c = (c & 0xFF03FF) | ((uint32_t)v << 8);   break;
        }
        f(x, c);
    }
}
static inline uint32_t rgbBin(int r, int g, int b) { return (uint32_t)((r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)); }
static uint8_t hamNearest(uint32_t bin) {
    const uint32_t c = ((bin >> 8) * 17) << 16 | (((bin >> 4) & 15) * 17) << 8 | ((bin & 15) * 17);
    uint32_t bd = ~0u; int best = 0;
    for (int k = 0; k < s_ham->n; k++) {
        const uint32_t d = dist(s_ham->rgb[k], c);
        if (d < bd) { bd = d; best = k; }
    }
    return kSlots(best);
}
// 0..255 of a dithered channel: USAT on the M33 (one instruction for the two compares).
static inline uint32_t sat8(int v) {
#if HOST_BUILD
    return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
#else
    uint32_t r; __asm__("usat %0, #8, %1" : "=r"(r) : "r"(v)); return r;
#endif
}
// PF2026's HAM video: core1 drew 256x192 in 18-20 ms, ~400 ns a pixel (hw 2026-11-16),
// and the frame could not be shorter than that. Per pixel here: one switch, three
// saturating adds, the bin, one LUT load; the table colours come from Hot::colRgb
// (renderFrame's per-frame copy, SRAM) instead of three byte loads from s_col in PSRAM.
static void hamLine(const uint8_t* p, int n, int y, uint8_t* slots) {
    if (!s_ham || !s_ham->n) { hamWalk(p, n, [&](int x, uint32_t c) { slots[x] = cubeSlot(c, x, y); }); return; }
    static const int8_t bay[16] = { -8, 0, -6, 2, 4, -4, 6, -2, -5, 3, -7, 1, 7, -1, 5, -3 };   // 4x4 Bayer, x HAM_DITHER/8
    const int8_t* row = bay + (y & 3) * 4;
    int t[4];
    for (int k = 0; k < 4; k++) t[k] = row[k] * HAM_DITHER / 8;
    uint8_t* lut = s_ham->lut;
    const uint32_t* col = s_hot->colRgb;
    uint32_t c = 0;
    for (int x = 0; x < n; x++) {
        const uint32_t b = p[x], v = (b & 0x3F) << 2;
        switch (b >> 6) {
        case 0:  c = col[b & 0x3F];                   break;
        case 1:  c = (c & 0xFFFF03) | v;              break;
        case 2:  c = (c & 0x03FFFF) | (v << 16);      break;
        default: c = (c & 0xFF03FF) | (v << 8);       break;
        }
        const int tt = t[x & 3];
        const uint32_t bin = (sat8((int)(c >> 16) + tt) >> 4) << 8 | (sat8((int)((c >> 8) & 255) + tt) >> 4) << 4 |
                             (sat8((int)(c & 255) + tt) >> 4);
        uint8_t s = lut[bin];
        if (__builtin_expect(s == 0xFF, 0)) s = lut[bin] = hamNearest(bin);
        slots[x] = s;
    }
}

// The same for a row already in RGB.
static void rgbLine(const uint32_t* c, int n, int y, uint8_t* slots) {
    if (!s_ham || !s_ham->n) { for (int x = 0; x < n; x++) slots[x] = cubeSlot(c[x], x, y); return; }
    static const int8_t bay[16] = { -8, 0, -6, 2, 4, -4, 6, -2, -5, 3, -7, 1, 7, -1, 5, -3 };
    const int8_t* row = bay + (y & 3) * 4;
    uint8_t* lut = s_ham->lut;
    for (int x = 0; x < n; x++) {
        const int t = row[x & 3] * HAM_DITHER / 8;
        const auto cl = [t](int v) { v += t; return v < 0 ? 0 : v > 255 ? 255 : v; };
        const uint32_t b = rgbBin(cl((int)(c[x] >> 16)), cl((int)((c[x] >> 8) & 255)), cl((int)(c[x] & 255)));
        uint8_t v = lut[b];
        if (v == 0xFF) v = lut[b] = hamNearest(b);
        slots[x] = v;
    }
}

// HRC under GigaScreen (g_zxi_008 = 1 two VRAMs, 2 two frames; TOCHIHRC: a fixed 1-pixel
// chequer of INK and PAPER whose attributes change every frame). The pair frame has 16
// colours and no blend, so the picture goes out 256 wide: a pixel is the mean of its
// two HRC pixels now and of the two it is mixed with, on the HAM palette. The frame
// before is kept as INK << 4 | PAPER a pixel in the HGFX buffers, idle below mode 3.
static bool s_gigaPrev;                 // the kept frame is a frame of this picture
static inline bool hrcGiga() { return mode == 1 && giga; }
static void hrcRow(const uint8_t* scr, int y, uint8_t* out) {
    hrLine(scr, y, false, out);
    if (keyBottom && scr != keyBottom) {
        hrLine(keyBottom, y, false, s_hot->line2);
        for (int x = 0; x < 512; x++) if (keyIs(out[x])) out[x] = s_hot->line2[x];
    }
}
// keep: this row becomes the frame before (the render, not the histogram pass).
static void hrcMixRow(int y, uint32_t* out, bool keep) {
    Hot* H = s_hot;
    uint32_t zx[16];
    for (int i = 0; i < 16; i++) { const uint32_t c = VIDEO::zxRgb((uint8_t)i); zx[i] = (c & 0xFF00FF) | (c & 0xFF00) << 12; }   // G apart: sums of four fit
    const auto mix = [&](uint32_t a) { return ((a & 0x3FF03FF) >> 2 & 0xFF00FF) | ((a >> 22) & 0xFF) << 8; };
    hrcRow(keyTop ? keyTop : VIDEO::grmem, y, H->line);
    uint8_t* prev = s_buf + (uint32_t)y * WIDTH;
    if (giga == 1 && gigaOther) {
        hrLine(gigaOther, y, false, H->slots);
        for (int x = 0; x < WIDTH; x++)
            out[x] = mix(zx[H->line[x * 2]] + zx[H->line[x * 2 + 1]] + zx[H->slots[x * 2]] + zx[H->slots[x * 2 + 1]]);
        return;
    }
    for (int x = 0; x < WIDTH; x++) {
        const uint8_t l = H->line[x * 2], r = H->line[x * 2 + 1];
        const uint8_t pv = s_gigaPrev ? prev[x] : (uint8_t)(l << 4 | r);
        out[x] = mix(zx[l] + zx[r] + zx[pv >> 4] + zx[pv & 15]);
        if (keep) prev[x] = (uint8_t)(l << 4 | r);
    }
}

// Gigascreen "Auto" for HRC: mix only while the picture flickers. A cell flickers when
// its attribute differs from the frame before and equals the one before that (TOCHIHRC
// swaps attributes every frame; a #7FFD bit 3 flip every frame reads the same way, the
// shown screen is what is compared). On after 4 such frames in a row with 48+ cells
// (1.5 % of 3072), off half a second after the last one: a still page keeps its 512 px.
// The two attribute frames live in the second HGFX buffer, idle below mode 3.
static bool hrcFlicker() {
    static uint8_t s_run, s_hold;
    const uint8_t* at = (keyTop ? keyTop : VIDEO::grmem) + 0x3000;
    uint8_t* p1 = s_buf + 0x10000; uint8_t* p2 = p1 + 0xC00;
    int f = 0;
    for (int i = 0; i < 3072; i++) f += (at[i] != p1[i]) & (at[i] == p2[i]);
    memcpy(p2, p1, 3072); memcpy(p1, at, 3072);
    if (f >= 48) { if (s_run < 4) s_run++; if (s_run == 4) s_hold = 25; }
    else { s_run = 0; if (s_hold) s_hold--; }
    return s_hold != 0;
}

// Median cut over the picture's bins: split the box with the most weight x extent
// along its longest axis at the weighted median, 168 boxes, each slot the box's
// weighted mean. false: no memory (the cube then).
static bool hamProgram(bool force) {
    if (!s_ham && (s_ham = (Ham*)tryMalloc(sizeof(Ham))) != nullptr) s_ham->n = 0;
    if (s_ham && !s_hamTmp)
        s_hamTmp = (uint16_t*)Buffer::palloc(HAM_BINS * 2 * sizeof(uint16_t) + 168 * sizeof(HamBox),
                                             Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!s_ham || !s_hamTmp) { drain(); free(s_ham); s_ham = nullptr; return false; }
    uint16_t* h = s_hamTmp;
    uint16_t* idx = h + HAM_BINS;
    HamBox* box = (HamBox*)(idx + HAM_BINS);
    memset(h, 0, HAM_BINS * sizeof(uint16_t));
    const int n0 = s_r[REG_VP] & 1;
    if (hrcGiga()) for (int y = 0; y < HEIGHT; y += 4) {
        uint32_t* c = s_hot->hist;       // core0 scratch; the frame is drained (frameEnd)
        hrcMixRow(y, c, false);
        for (int x = 0; x < WIDTH; x++) {
            uint16_t& v = h[rgbBin((int)(c[x] >> 16), (int)((c[x] >> 8) & 255), (int)(c[x] & 255))];
            if (v != 0xFFFF) v++;
        }
    } else
    for (int y = 0; y < s_h; y += 4)
        hamWalk(rowOf(n0, y), WIDTH, [h](int, uint32_t c) {
            uint16_t& v = h[rgbBin((int)(c >> 16), (int)((c >> 8) & 255), (int)(c & 255))];
            if (v != 0xFFFF) v++;
        });
    int n = 0;
    for (int b = 0; b < HAM_BINS; b++) if (h[b]) idx[n++] = (uint16_t)b;
    if (!n) return false;
    const auto ch = [](uint16_t b, int a) { return a == 0 ? b >> 8 : a == 1 ? (b >> 4) & 15 : b & 15; };
    const auto fit = [&](HamBox& x) {
        x.w = 0;
        for (int a = 0; a < 3; a++) { x.mn[a] = 15; x.mx[a] = 0; }
        for (int i = x.lo; i < x.hi; i++) {
            x.w += h[idx[i]];
            for (int a = 0; a < 3; a++) {
                const uint8_t v = (uint8_t)ch(idx[i], a);
                if (v < x.mn[a]) x.mn[a] = v;
                if (v > x.mx[a]) x.mx[a] = v;
            }
        }
    };
    int nb = 1;
    box[0].lo = 0; box[0].hi = (uint16_t)n; fit(box[0]);
    while (nb < 168) {
        int pick = -1; uint32_t score = 0; int axis = 0;
        for (int k = 0; k < nb; k++) {
            if (box[k].hi - box[k].lo < 2) continue;
            for (int a = 0; a < 3; a++) {
                const uint32_t sc = box[k].w * (uint32_t)(box[k].mx[a] - box[k].mn[a]);
                if (sc > score) { score = sc; pick = k; axis = a; }
            }
        }
        if (pick < 0) break;
        HamBox& b = box[pick];
        std::sort(idx + b.lo, idx + b.hi, [&](uint16_t p, uint16_t q) { return ch(p, axis) < ch(q, axis); });
        uint32_t acc = 0; int cut = b.lo;
        while (cut < b.hi - 1 && (acc += h[idx[cut]]) * 2 < b.w) cut++;
        cut++;                           // [lo, cut) and [cut, hi), both non-empty
        if (cut >= b.hi) cut = b.hi - 1;
        HamBox& c = box[nb++];
        c.lo = (uint16_t)cut; c.hi = b.hi; b.hi = (uint16_t)cut;
        fit(b); fit(c);
    }
    uint32_t pal[168];
    uint64_t err = 0, tw = 0;
    for (int k = 0; k < nb; k++) {
        uint32_t s3[3] = { 0, 0, 0 }, w = 0;
        for (int i = box[k].lo; i < box[k].hi; i++) {
            const uint32_t wt = h[idx[i]];
            for (int a = 0; a < 3; a++) s3[a] += wt * (uint32_t)(ch(idx[i], a) * 17);
            w += wt;
        }
        pal[k] = (s3[0] / w) << 16 | (s3[1] / w) << 8 | (s3[2] / w);
        for (int i = box[k].lo; i < box[k].hi; i++) {
            const uint16_t b = idx[i];
            err += (uint64_t)h[b] * dist(pal[k], (uint32_t)(ch(b, 0) * 17) << 16 | (uint32_t)(ch(b, 1) * 17) << 8 | (uint32_t)(ch(b, 2) * 17));
        }
        tw += w;
    }
    // A picture the 168 colours do not hold (a smooth HAM gradient) keeps the cube: its
    // steps show as bands without a full dither. Mean squared error per pixel, RGB.
    const uint32_t mse = tw ? (uint32_t)(err / tw) : 0;
    const bool useCube = mse > HAM_MAX_MSE;
    static int8_t s_said = -1;
    if (s_said != (int8_t)useCube) { s_said = (int8_t)useCube; Debug::log("[ELM] HAM: %s (mse %u, %d bins)", useCube ? "dithered cube" : "picture palette", (unsigned)mse, n); }
    if (useCube) { if (s_ham->n) { drain(); s_ham->n = 0; } return false; }
    drain();                             // core1 maps through the table being rewritten
    static bool s_slotKInit;
    if (!s_slotKInit) { s_slotKInit = true; memset(s_hamSlotK, 0xFF, sizeof s_hamSlotK); for (int k = 0; k < 168; k++) s_hamSlotK[kSlots(k)] = (uint8_t)k; }
    uint32_t* nrgb = s_hot->rgb;         // core0 scratch (frameSetup's pair path is not this one)
    uint8_t* ord = (uint8_t*)s_hot->h; bool* live = (bool*)(ord + 168); bool* taken = live + 168;
    const int on = s_ham->n;
    for (int k = 0; k < 168; k++) { nrgb[k] = k < on ? s_ham->rgb[k] : 0; taken[k] = false; }
    if (on) hamLiveSlots(live); else for (int k = 0; k < 168; k++) live[k] = false;
    for (int j = 0; j < nb; j++) ord[j] = (uint8_t)j;
    std::sort(ord, ord + nb, [&](uint8_t a, uint8_t b) { return box[a].w > box[b].w; });
    int nn = on, written = 0, moved = 0;
    for (int i = 0; i < nb; i++) {
        const int j = ord[i];
        // Near enough to a slot we have (live or not): that slot serves the colour.
        int best = -1; uint32_t bd = ~0u;
        for (int k = 0; k < nn; k++) { const uint32_t d = dist(pal[j], nrgb[k]); if (d < bd) { bd = d; best = k; } }
        if (best >= 0 && bd <= HAM_STICK) { taken[best] = true; continue; }
        // A slot nobody on screen uses: an unused one first, then a free one, then the
        // nearest live slot not yet claimed by this pass.
        int k = -1;
        for (int c = 0; c < nn && k < 0; c++) if (!live[c] && !taken[c]) k = c;
        if (k < 0 && nn < 168) k = nn++;
        if (k < 0) { bd = ~0u; for (int c = 0; c < nn; c++) if (!taken[c]) { const uint32_t d = dist(pal[j], nrgb[c]); if (d < bd) { bd = d; k = c; } } if (k >= 0) moved++; }
        if (k < 0) continue;
        nrgb[k] = pal[j]; taken[k] = true; live[k] = true; written++;
        VIDEO::elmPaletteSet(kSlots(k), pal[j]);
    }
    if (!written && !force) return true;
#if HOST_BUILD
    if (getenv("PICOSPECCY_ELMLOG")) Debug::log("[ELM] HAM refresh: %d colours, %d slots written, %d live slots moved, %d in use", nb, written, moved, nn);
#endif
    memcpy(s_ham->rgb, nrgb, nn * sizeof(uint32_t));
    s_ham->n = (uint8_t)nn;
    memset(s_ham->lut, 0xFF, sizeof s_ham->lut);
    s_palAll = true;                     // the slots are HAM's now
    return true;
}

static void cubeProgram() {
    s_palAll = true;                     // the slots are the cube's now
    int k = 0;
    for (int r = 0; r < 6; r++) for (int g = 0; g < 7; g++) for (int b = 0; b < 4; b++, k++) {
        const uint8_t s = kSlots(k);
        s_cube[k] = s;
        VIDEO::elmPaletteSet(s, (uint32_t)(r * 255 / 5) << 16 | (uint32_t)(g * 255 / 6) << 8 | (b * 255 / 3));
    }
}

static bool wantPairFor2() {             // HGFX 2: text or a picture (owner's rule)
    uint32_t* hist = s_hot->hist; memset(hist, 0, 256 * sizeof(uint32_t));
    const uint8_t* buf = bufN(s_r[REG_VP] & 1);
    uint32_t total = 0;
    for (int y = 0; y < s_h; y += 8) {
        const uint8_t* p = buf + (uint32_t)y * s_stride;
        for (int x = 0; x < s_w; x += 3) { hist[p[x]]++; total++; }
    }
    uint32_t top = 0;
    for (int i = 0; i < 16; i++) {
        int b = 0;
        for (int k = 1; k < 256; k++) if (hist[k] > hist[b]) b = k;
        top += hist[b]; hist[b] = 0;
    }
    return top * 100 >= total * 97;
}

// Everything the pair draw reads, as one word: both screens (6912 B each), pidx, the LUT,
// the layout, FLASH when the attributes carry it (HRC; HRXC is indexed). ~3.5k words.
ELM_HOT static uint32_t pairSig(const uint8_t* scr, const uint8_t* scr2, bool idx, const uint8_t* lut, int x0, int y0) {
    uint32_t h = 0x811C9DC5u ^ (uint32_t)x0 ^ (uint32_t)y0 << 8 ^ (uint32_t)keyColour << 16 ^ (idx ? 1u << 24 : 0) ^ (uint32_t)(uintptr_t)lut;
    if (!idx && VIDEO::flashing) h ^= 1u << 25;
    h ^= (uint32_t)(uintptr_t)scr ^ (uint32_t)(uintptr_t)scr2 << 4;
    const uint32_t* w = (const uint32_t*)scr;
    for (int i = 0; i < 6912 / 4; i++) h = (h ^ w[i]) * 0x9E3779B1u;
    if (scr2) { w = (const uint32_t*)scr2; for (int i = 0; i < 6912 / 4; i++) h = (h ^ w[i]) * 0x9E3779B1u; }
    w = (const uint32_t*)s_hot->pidx;
    for (int i = 0; i < 64 / 4; i++) h = (h ^ w[i]) * 0x9E3779B1u;
    if (lut == s_hot->lut) { w = (const uint32_t*)lut; for (int i = 0; i < 256 / 4; i++) h = (h ^ w[i]) * 0x9E3779B1u; }
    return h;
}

static void renderFrame() {
    const int xres = VIDEO::vga.xres, yres = VIDEO::vga.yres, x0 = (xres - WIDTH) / 2, y0 = VIDEO::screenTopRows();
    Hot* H = s_hot;
    const uint8_t* zx0 = keyTop ? keyTop : VIDEO::grmem;
    if (radastan && !mode) {
        for (int y = 0; y < HEIGHT; y++) {
            const uint32_t line = (y >> 1) * (64u + radPad);
            uint8_t* o = H->slots;
            for (int x = 0; x < 64; x++) {
                const uint8_t b = VIDEO::grmem[(radOffset + line + x) & 0x3FFF];
                const uint8_t h = (uint8_t)(16 + (b >> 4)), l = (uint8_t)(16 + (b & 15));
                *o++ = h; *o++ = h; *o++ = l; *o++ = l;
            }
            outDirect(y0 + y, x0, H->slots, WIDTH);
        }
        return;
    }
    if (mode == 1 || mode == 2) {
        const bool idx = mode == 2;
        const uint8_t* lut = idx && H->lutOn ? H->lut : &VIDEO::profi_pair_lookup[0][0];
        if (hrcGiga() && !s_pair) {
            for (int y = 0; y < HEIGHT; y++) {
                hrcMixRow(y, H->rgb, true);
                rgbLine(H->rgb, WIDTH, y, H->slots);
                outDirect(y0 + y, x0, H->slots, WIDTH);
            }
            s_gigaPrev = true;
            return;
        }
        if (s_pair) {
            // The gate: core0 pushed the staged palette with the beam below the picture and
            // set s_pairGo (pairPushPoll); the Q_DRAW gate in core1Pump holds the draw until
            // then (spinning on the NeoGS and the SD pump, which live on this core too).
            s_pairGo = false;
            // Nothing the draw reads has changed since the last one: the FB still holds it.
            // The draw is ~14 ms of core1 a frame, and LIMSHOWM holds a picture for seconds and
            // fades it with the palette alone (hw 2026-11-16: `pidx 0 pal 0 ... draw x60`, the
            // NeoGS at 14 of 20 MHz underneath). At most PAIR_CLEAN frames in a row, as the HGFX
            // modes' s_cleanRun: whatever paints over the FB (OSD, notify) is gone within 10.
            const uint32_t sig = pairSig(zx0, keyBottom, idx, lut, x0, y0);
            static uint32_t s_sig; static uint8_t s_clean;
            enum { PAIR_CLEAN = 9 };
#if HOST_BUILD
            static const bool noSkip = getenv("PICOSPECCY_ELM_NOSKIP") != nullptr;   // bench: every frame drawn, for a shot-by-shot comparison
            if (noSkip) s_pairStale = true;
#endif
            if (sig == s_sig && !s_pairStale && s_clean < PAIR_CLEAN) {
                s_clean++;
#if PERF_TRACE
                s_pSkips = s_pSkips + 1;
#endif
                return;
            }
            s_sig = sig; s_clean = 0; s_pairStale = false;
            if (idx && s_cnt2) memset(s_cnt2, 0, 8192);   // the census of THIS draw (pairLutUpdate)
            for (int y = 0; y < HEIGHT; y++) pairRowFast(zx0, keyBottom, y, idx, lut, y0 + y, x0);
        }
        return;
    }
    if (!mode) {                         // KeyLayer over the ZX screen
        for (int y = 0; y < HEIGHT; y++) {
            zxLine(zx0, y, H->line);
            zxLine(keyBottom, y, H->line2);
            for (int x = 0; x < WIDTH; x++) if (keyIs(H->line[x])) H->line[x] = H->line2[x];
            outDirect(y0 + y, x0, H->line, WIDTH);
        }
        return;
    }
    const uint8_t* buf = bufN(s_e[REG_VP] & 1);
    const uint8_t feature = s_e[REG_FEATURE];
    if (feature == 2 && s_col) for (int i = 0; i < 64; i++) H->colRgb[i] = tableRgb(s_col + i * 3);   // hamLine's copy
    const bool under = !(s_e20 & 4) && s_w <= 512 && s_h == HEIGHT;
    const bool fullScreen = s_w >= 360 && s_w != 512;
    if (fullScreen) {
        // 360x288 and the PAL modes fill the framebuffer; a smaller one shows the middle.
        const int rows = s_h > 300 ? yres : s_h > yres ? yres : s_h;
        const int srcRowStep2 = s_h > 300 ? 2 : 1;
        const int ytop = s_h > 300 ? 0 : (s_h - rows) / 2;
        for (int r = 0; r < rows; r++) {
            const uint8_t* p = buf + (uint32_t)(ytop + r) * srcRowStep2 * s_stride;
            if (s_h > 300 && (ytop + r) * 2 >= s_h) break;
            if (s_w == 720) {
                if (s_pair) {
                    const int skip = (720 - xres * 2) / 2;
                    for (int x = 0; x < xres * 2; x++) H->line[x] = H->pidx[pxGet(p, x + skip)];
                    outPair(r, 0, H->line, xres);
                } else {
                    for (int x = 0; x < xres; x++) H->slots[x] = H->map[pxGet(p, (x * 2) + (720 - xres * 2) / 2)];
                    outDirect(r, 0, H->slots, xres);
                }
            } else {
                const int skip = (360 - xres) / 2;
                if (s_bpp == 8) for (int x = 0; x < xres; x++) H->slots[x] = H->map[p[x + skip]];
                else for (int x = 0; x < xres; x++) H->slots[x] = H->map[pxGet(p, x + skip)];
                outDirect(r, 0, H->slots, xres);
            }
        }
        return;
    }
    for (int y = 0; y < HEIGHT; y++) {
        const uint8_t* p = rowOf(s_e[REG_VP] & 1, s_h > HEIGHT ? y * 2 : y);
        if (s_bpp != 8) {                // HGFX 3: 16 colours, every second row of 384
            if (s_pair) {
                for (int x = 0; x < 512; x++) H->line[x] = H->pidx[pxGet(p, x)];
                outPair(y0 + y, x0, H->line, WIDTH);
            } else {
                for (int x = 0; x < WIDTH; x++) H->slots[x] = H->map[pxGet(p, x * 2)];
                outDirect(y0 + y, x0, H->slots, WIDTH);
            }
            continue;
        }
        if (s_pair) {                    // HGFX 2 as text
            memcpy(H->line2, p, WIDTH * 2);
            if (feature == 1) fillLine(H->line2, 512);
            for (int x = 0; x < 512; x++) H->line[x] = H->pidx[H->line2[x]];
            outPair(y0 + y, x0, H->line, WIDTH);
            continue;
        }
        const uint8_t* q = p;
        if (s_w > WIDTH) {               // 512 px on 256: the left pixel, or the right over 0
            for (int x = 0; x < WIDTH; x++) H->line2[x] = p[x * 2] ? p[x * 2] : p[x * 2 + 1];
            q = H->line2;
        } else if (feature == 1) {
            memcpy(H->line2, p, WIDTH);
            q = H->line2;
        }
        if (feature == 1) fillLine(const_cast<uint8_t*>(q), WIDTH);
        if (feature == 2) { hamLine(q, WIDTH, y, H->slots); outDirect(y0 + y, x0, H->slots, WIDTH); continue; }
        if (under) {
            zxLine(VIDEO::grmem, y, H->line);
            // Four pixels of index 0 are four pixels of the ZX layer as they stand.
            for (int x = 0; x < WIDTH; x += 4) {
                if (!rd32(q + x)) { wr32(H->slots + x, rd32(H->line + x)); continue; }
                for (int k = x; k < x + 4; k++) H->slots[k] = q[k] ? H->map[q[k]] : H->line[k];
            }
        } else {
            for (int x = 0; x < WIDTH; x++) H->slots[x] = H->map[q[x]];
        }
        outDirect(y0 + y, x0, H->slots, WIDTH);
    }
}

// The palette a frame needs, or a change of output.
static void frameSetup() {
    uint32_t* rgb = s_hot->rgb; uint32_t* hist = s_hot->hist;   // wantPairFor2 uses hist first, done by then
    const bool hr = mode == 1 || mode == 2;
    bool wantPair = (hr && !hrcGiga()) || mode == 5 || mode == 6 || mode == 7;
    if (!hrcGiga()) s_gigaPrev = false;
    if (mode == 4) {
        if (s_pairHold) s_pairHold--;
        else { wantPair = wantPairFor2(); s_pairWant2 = wantPair; s_pairHold = 25; }
        wantPair = s_pairWant2;
    }
    if (mode != 2 && s_hot->lutOn) { s_hot->lutOn = false; VIDEO::samPairLut(nullptr); }
    if (mode != 2 && s_cnt2) { drain(); free(s_cnt2); s_cnt2 = nullptr; }
    if (hr && mode == 1 && wantPair) {
        for (int i = 0; i < 256; i++) { rgb[i] = 0; hist[i] = 0; }
        for (int i = 0; i < 12; i++) { rgb[i] = VIDEO::zxRgb((uint8_t)i); hist[i] = (uint32_t)(100 - i); }
        pairPalette(hist, rgb, 12);
        s_zxPair = true;
        return;
    }
    s_zxPair = false;
    if (wantPair) {
        int n = 256;
        memset(hist, 0, 256 * sizeof(uint32_t));
        if (mode == 2) {
            // KeyLayer: "every screen uses its own colour palette" (Reference), the
            // bottom one's 32 are indices 32..63 here. HRXCIMGS loads the two screens
            // one after the other; through the top table the bottom one showed in the
            // new picture's colours before its own table came (owner 2026-11-14).
            n = keyBottom ? 64 : 32;
            const uint8_t* ct = hrxcTable ? hrxcTable : VIDEO::grmem + HRXC_TABLE;
            for (int i = 0; i < 32; i++) rgb[i] = tableRgb(ct + i * 3);
            if (keyBottom) {               // latched with the top one's (Element.cpp), right behind it
                const uint8_t* cb = hrxcTable ? hrxcTable + HRXC_BYTES : keyBottom + HRXC_TABLE;
                for (int i = 0; i < 32; i++) rgb[32 + i] = tableRgb(cb + i * 3);
            }
            // Every cell of every screen shown: under KeyLayer the top one may be INK over a
            // key PAPER and the bottom one bring colours the top never uses, and a colour
            // met in one cell of three could miss the sample (HRXCIMGS: black dots where
            // the absent colour went to its nearest, owner 2026-11-14).
            for (const uint8_t* scr : { keyTop ? keyTop : (const uint8_t*)VIDEO::grmem, keyBottom }) {
                if (!scr) continue;
                const uint8_t* at = scr + 0x3000;
                uint32_t* hs = hist + (scr == keyBottom ? 32 : 0);
                for (int i = 0; i < 3072; i++) { hs[at[i] & 15]++; hs[16 | (at[i] >> 4)]++; }
            }
            if (keyBottom) hist[keyColour & 31] = 0;   // the key is never seen
        } else {
            n = mode == 4 ? 256 : mode == 5 ? 16 : 4;
            for (int i = 0; i < n; i++) { rgb[i] = tableRgb(s_col + i * 3); hist[i] = (uint32_t)(n - i); }
            if (mode == 4) {
                const uint8_t* buf = bufN(s_r[REG_VP] & 1);
                memset(hist, 0, 256 * sizeof(uint32_t));
                for (int y = 0; y < s_h; y += 8) for (int x = 0; x < s_w; x += 3) hist[buf[(uint32_t)y * s_stride + x]]++;
                // The indexed border is a third of the picture's area and needs its own
                // colour among the 12 (the samples above are 4104).
                if (indexedBorder()) hist[VIDEO::borderColor & 7] += 2000;
            }
        }
        pairPalette(hist, rgb, n);
        if (s_pair) { for (int i = 0; i < 16; i++) {
            uint32_t bd = ~0u; const uint32_t z = VIDEO::zxRgb((uint8_t)i);
            for (int k = 0; k < 16; k++) { const uint32_t d = dist(s_hot->pal[k], z); if (d < bd) { bd = d; s_hot->zxp[i] = (uint8_t)k; } }
        } return; }
    }
    if (s_pair) { pairOff(); s_palDirty = true; s_palAll = true; }
    if (radastan && !mode) {
        for (int i = 0; i < 16; i++) {
            const uint8_t g = VIDEO::ulaplus_palette[radBank * 16 + i];
            const uint32_t r3 = (g >> 2) & 7, g3 = g >> 5, b2 = g & 3;
            VIDEO::elmPaletteSet((uint8_t)(16 + i), (r3 * 255 / 7) << 16 | (g3 * 255 / 7) << 8 | (b2 * 255 / 3));
        }
        return;
    }
    if (hrcGiga() || (mode >= 3 && s_r[REG_FEATURE] == 2)) {   // HAM, HRC mixed: a palette of the picture, refreshed twice a second
        const bool need = s_palDirty || !s_palValid;
        if (need || !s_hamHold) {
            s_hamHold = 25;
            s_palDirty = false; s_palValid = true;
            if (hamProgram(need)) s_hamCubeOn = false;
            else if (need || !s_hamCubeOn) { cubeProgram(); s_hamCubeOn = true; }
        } else s_hamHold--;
        return;
    }
    if (s_palDirty || !s_palValid) {
        s_palDirty = false; s_palValid = true;
        if (mode) { paletteProgram(s_palAll); s_palAll = false; }
    }
}

void frameEnd() {
    if (s_direct && !s_directWant && s_directHold && !--s_directHold) s_direct = false;
    const bool want = (mode >= 1 || keyBottom || radastan) && ensure();
    if (want != s_live) {
        s_live = want;
        VIDEO::paper_hold = want;
        s_palDirty = true; s_palValid = false; s_palAll = true;
        if (!want) {
            // The 48 KB go back to the heap the moment the program leaves the extra
            // video; a return to it earns them again through the lag rule.
            sramOff(true);
            s_sramRetry = 0; s_lagFrames = s_lagWindow = 0;
            if (s_pair) pairOff();
            if (s_cnt2) { drain(); free(s_cnt2); s_cnt2 = nullptr; }
            VIDEO::applyPalette();
            VIDEO::brdChange = VIDEO::brdnextframe = true;
            borderFollow(true);
        }
        Debug::log("[ELM] extra video %s (mode %u key %d rad %d)", want ? "on" : "off", mode, keyBottom != nullptr, (int)radastan);
    }
#if PERF_TRACE && PERF_HIST
    // Without the extra video the [HGFX] line below never runs: the PC sampler still
    // dumps once a second (plain BASIC at 28 MHz, 2026-11-16).
    if (!s_live) { static uint8_t n; if (++n >= 60) { n = 0; ::pcprof_dump(); } }
#endif
    if (!s_live) { if (s_h1) { h1Off(true, "video off"); Elm::remap(); } return; }
    h1Manage();
    if (mode == 1 && !giga && gigaAuto && hrcFlicker()) giga = gigaAuto;
    if (s_palDirty || !s_palValid) s_fbDirty = true;
    const bool was = s_pair;
    if (s_pair || mode == 4 || hrcGiga()) drain();   // the pair tables are rewritten under the frame; the HRC mix shares scratch
    frameSetup();
    // The pair frame's border is a slot of the picture's palette, and a new palette deals
    // the slots again: the border stayed on its old slot in whatever colour came there
    // (HRXCIMGS: blue, green, beige for BORDER 0; owner 2026-11-14).
    static uint8_t s_brdPair = 0xFF;
    const uint8_t bp = s_pair ? pairBorder(VIDEO::borderColor) : 0xFF;
    if (was != s_pair || bp != s_brdPair) { VIDEO::updateBorderBrd(); VIDEO::brdChange = VIDEO::brdnextframe = true; }
    s_brdPair = bp;
    borderFollow();
    if (!s_q) { renderFrame(); return; }
    g_elm_c1_live = true;
    gsAhead(true);                          // every frame: the card may have come up after us
    if (s_drawDone != s_drawPosted) s_lagFrames++;
    if (++s_lagWindow == 30) {              // half a second, most of it behind
#if HOST_BUILD
        static const bool forceSram = getenv("PICOSPECCY_ELM_SRAM") != nullptr;   // bench: the hw lag rule never fires here
        if (forceSram) s_lagFrames = 15;
#endif
        if (s_lagFrames >= 15) sramPromote();
        s_lagWindow = s_lagFrames = 0;
        s_use[0] = s_use[1] = 0;
    }
    {
        const bool alone = mode >= 3 && !s_pair && s_r[REG_FEATURE] != 2 &&
                           ((s_w >= 360 && s_w != 512) || (g20 & 4));
        if (alone && !s_fbDirty && s_cleanRun < 9) { s_cleanRun++; runFlush(); return; }
        s_fbDirty = false; s_cleanRun = 0;
    }
    // core1 still on the last frame: the machine waits for it. Dropping the frame instead
    // let core0 run ten frames ahead and showed one in ten (HGFXAGON: 5 of 60, in jerks).
    if (s_drawDone != s_drawPosted) {
#if PERF_TRACE
        const uint32_t t0 = time_us_32();
        s_pDrop++;
#endif
        const uint64_t lim = time_us_64() + 100000;
        while (s_drawDone != s_drawPosted && time_us_64() < lim) { if (g_pairPushNow) pairPushPoll(); }
#if PERF_TRACE
        s_pWaitUs += time_us_32() - t0;
#endif
    }
    s_drawPosted = s_drawPosted + 1;
    runFlush();
    s_drawAt = s_qw;
    pushRaw(Q_DRAW);
    s_drawQw = s_qw;
#if PERF_TRACE
    static uint32_t n;
    if (++n == 60) {
        n = 0;
        Debug::log("[HGFX] 60f: q=%u/fr wait=%uus/fr drop=%u | c1 engine %uus/fr (%u, cmd %uus) draw %uus x%u skip %u fill=%u | runs %u/fr %uB sram=%u%u | pair pidx %u pal %u full %u lut %u",
                   s_pPush / 60, s_pWaitUs / 60, s_pDrop, s_pExecUs / 60, s_pExecN / 60, s_pCmdUs / 60,
                   s_pDraws > s_pSkips ? s_pDrawUs / (s_pDraws - s_pSkips) : 0, s_pDraws - s_pSkips, s_pSkips, s_qw - s_qr,
                   s_pRuns / 60, s_pRunBytes / 60, (unsigned)s_sramOn[0], (unsigned)s_sramOn[1],
                   s_pPidxChg, s_pPalChg, s_pFullFlip, s_pLutPush);
        if (s_pCellTop | s_pCellBot | s_pCellMix) Debug::log("[HGFX] pair cells/fr: top %u bottom %u mixed %u | push at draw %u late %u cap %u beam %u..%u (top %d) | gs row pumps %u skipped %u",
                                                             s_pCellTop / 60, s_pCellBot / 60, s_pCellMix / 60, s_pPushPoll, s_pPushLate, s_pPushCap, s_pPushBeamMin, s_pPushBeamMax, VIDEO::screenTopRows(),
                                                             s_pRowPump / 60, s_pRowSkip / 60);
        s_pPidxChg = s_pPalChg = s_pFullFlip = s_pLutPush = 0; s_pCellTop = s_pCellBot = s_pCellMix = 0;
        s_pPushPoll = s_pPushLate = s_pPushCap = 0; s_pPushBeamMin = 999; s_pPushBeamMax = 0; s_pRowPump = s_pRowSkip = 0;
#if PERF_HIST
        ::pcprof_dump();   // core0 PC sampler (PcProf.cpp): only the Next and the EP dumped it
#endif
        s_pPush = s_pWaitUs = s_pDrop = 0;
        s_pDrawUs = s_pDraws = s_pSkips = s_pExecUs = s_pExecN = s_pRuns = s_pRunBytes = s_pCmdUs = 0;
    }
#endif
}

void repaint() { if (s_live) { drain(); s_pairStale = true; frameSetup(); renderFrame(); } }

// core1: a bounded batch, then back to the loop for the other pumps.
ELM_HOT void core1Pump() {
#if PERF_TRACE
    const uint32_t t0 = time_us_32(); uint32_t draw = 0, cnt = 0;
#endif
    for (int n = 0; n < 256 && s_qr != s_qw; n++) {
        __sync_synchronize();
        const uint32_t e = s_q[s_qr & (Q_N - 1)];
        // One buffer, and a frame takes a third of a sweep: it starts with the beam off
        // the picture and stays ahead of it, or moving sprites tear at the beam. The
        // queue waits, not the core; a display without a beam position passes at once.
        if ((e & (7u << 29)) == Q_DRAW) {
            static uint32_t s_waitT;
            const int top = VIDEO::screenTopRows(), beam = VIDEO::displayBeamRow();
            const uint32_t now32 = time_us_32();
            // A staged pair palette: the draw must start with the beam BELOW the picture
            // (pairPendApply pushes it first), not merely off it.
            // A staged pair palette: core0 pushes it with the beam below the picture and
            // raises s_pairGo (pairPushPoll); until then the draw waits here, the pump
            // returns to its other work (NeoGS, SD) in between. 30 ms cap.
            bool hold = beam >= top && beam < top + s_h;
            if (s_pairPendValid && !s_pairGo) {
                // Spin here, feeding the NeoGS and the SD pump, until core0 has pushed the
                // palette (beam below the picture) - leaving the pump instead cost one GS
                // slice of latency and the draw then started inside the picture (hw
                // 2026-11-16 19:34: "late 28 cap 28", the speckles back).
                g_pairPushNow = true;
                const uint32_t t0 = time_us_32();
                while (s_pairPendValid && !s_pairGo && time_us_32() - t0 < 30000) gatePump();
#if PERF_TRACE
                if (s_pairPendValid && !s_pairGo) s_pPushCap++;
#endif
                g_pairPushNow = false;
                hold = false;            // core0 pushed with the beam below the picture: draw now
            }
            if (hold) {
                if (!s_waitT) s_waitT = now32 | 1;
                if (now32 - s_waitT < 25000) break;
            }
            s_waitT = 0;
        }
        switch (e & (7u << 29)) {
        case Q_VRAM: execVram((uint16_t)e, (uint8_t)(e >> 16), e & (1u << 24), 0xFFFF); break;
        case Q_REG:  if ((uint8_t)(e >> 8) == REG_COUNT) s_e20 = (uint8_t)e; else s_e[(uint8_t)(e >> 8)] = (uint8_t)e; break;
        case Q_CMD:  {
#if PERF_TRACE
            const uint32_t c0 = time_us_32();
#endif
            execCmd((uint8_t)e);
#if PERF_TRACE
            s_pCmdUs = s_pCmdUs + (time_us_32() - c0);
#endif
            break;
        }
        case Q_BYTES: {                     // the bytes follow, four to a word
            const uint32_t nb = ((e >> 16) & 31) + 1, words = (nb + 3) >> 2;
            uint32_t tmp[BRUN_MAX / 4];
            for (uint32_t w = 0; w < words; w++) tmp[w] = s_q[(s_qr + 1 + w) & (Q_N - 1)];
#if HOST_BUILD
            // Bench self-check (PICOSPECCY_ELM_CHECK): the run against the per-byte path.
            static const bool check = getenv("PICOSPECCY_ELM_CHECK") != nullptr;
            static uint8_t* snap; static uint32_t s_chk, s_bad;
            const int cn = dstE(); const size_t rb = (size_t)s_stride, all = rb * (size_t)s_h;
            if (check && !snap) snap = (uint8_t*)malloc(0x20000 * 2);
            if (check) for (int y = 0; y < s_h; y++) memcpy(snap + y * rb, rowOf(cn, y), rb);
#endif
            execBytes((uint16_t)e, (const uint8_t*)tmp, nb, e & (1u << 24));
#if HOST_BUILD
            if (check) {
                uint8_t* after = snap + 0x20000;
                for (int y = 0; y < s_h; y++) { memcpy(after + y * rb, rowOf(cn, y), rb); memcpy(rowOf(cn, y), snap + y * rb, rb); }
                for (uint32_t i = 0; i < nb; i++) execVram((uint16_t)((uint16_t)e + i), ((const uint8_t*)tmp)[i], e & (1u << 24), 0xFFFF);
                bool same = true;
                for (int y = 0; y < s_h && same; y++) same = !memcmp(after + y * rb, rowOf(cn, y), rb);
                s_chk++;
                if (!same && s_bad++ < 20) Debug::log("[ELMCHK] MISMATCH off=%04X n=%u mode=%u pm=%u m=%02X bpp=%d w=%d", (unsigned)(uint16_t)e, (unsigned)nb, mode, s_e[REG_MODE], s_e[REG_MASK], s_bpp, s_w);
                if (!(s_chk & 0x3FFF)) Debug::log("[ELMCHK] %u runs checked, %u bad", (unsigned)s_chk, (unsigned)s_bad);
                (void)all;
            }
#endif
            s_qr = s_qr + words;
            break;
        }
        case Q_RUN:  {                       // two words: the second is the read offset
            s_qr = s_qr + 1;
            execRun((uint16_t)e, (e >> 16) & 0x1FFF, (uint16_t)s_q[s_qr & (Q_N - 1)]);
            break;
        }
        default: {
#if PERF_TRACE
            const uint32_t d0 = time_us_32();
#endif
            renderFrame();
            s_use[s_e[REG_VP] & 1] = s_use[s_e[REG_VP] & 1] + WIDTH * HEIGHT;
#if PERF_TRACE
            draw += time_us_32() - d0;
            s_pDraws = s_pDraws + 1;
#endif
            s_drawDone = s_drawDone + 1;
            break;
        }
        }
        s_qr = s_qr + 1;
#if PERF_TRACE
        cnt++;
#endif
    }
#if PERF_TRACE
    if (cnt) {
        s_pDrawUs = s_pDrawUs + draw;
        s_pExecUs = s_pExecUs + (time_us_32() - t0 - draw);
        s_pExecN = s_pExecN + cnt;
    }
#endif
}
void paletteLost() { s_palDirty = true; s_palAll = true; }

}

// The CPU's write hook (CPU.cpp, a window with a kind): the chunky window is taken here,
// in RAM; every other kind is Element.cpp's memWriteCold in flash.
namespace Elm {
ELM_HOT bool memWrite(uint16_t a, uint8_t v) {
    const uint8_t k = wrKind[a >> 8];
    if (a < 0x4000 && MemESP::divmmc_mapped) return false;   // the divMMC window is on top
    if (k == WK_CHUNKY) {
        Hgfx::chunkyWrite(slotPage[a >> 13] * 0x2000u + (a & 0x1FFF), v);
        return true;
    }
    // The VRAM window outside COPY mode (no read latch to consult): HGFXCUBE fills its
    // faces a byte at a time. The store lands in RAM too (false).
    if (k == WK_VRAM && !g_elm_vram_rd && Hgfx::s_r[Hgfx::REG_MASK]) {
#if HOST_BUILD
        static const bool vlog = getenv("PICOSPECCY_ELMVLOG") != nullptr;
        if (vlog) return memWriteCold(a, v);
#endif
        Hgfx::writeVram((uint16_t)(a - ((uint16_t)*vramHi << 8)), v, false, 0xFFFF);
        return false;
    }
    return memWriteCold(a, v);
}
}
