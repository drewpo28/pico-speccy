/*

pico-speccy: ZX Spectrum Next scanline video renderer

*/

#include "NextVideo.h"
#include "speccy/z80/z80.h"
#include "NextFramePacing.h"



#include <string.h>
#include <stdlib.h>
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"
#include "app/ESPectrum.h"
#include "speccy/core/MemESP.h"
#include "NextReg.h"
#include "app/Debug.h"
#include "speccy/z80/MemGates.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include "pico/time.h"
#include "hardware/timer.h"
#include "app/TryAlloc.h"
#include "app/Buffer.h"
#include "app/Config.h"
#include "ui/LEDIndicators.h"
#include "speccy/devices/Z80DMA.h"

// Hot render path in SRAM, like the rest of the video code
#undef IRAM_ATTR
#define IRAM_ATTR __not_in_flash("nextvid")

#pragma GCC optimize("O3")

uint16_t (*NEXTVID::rawPal)[256] = nullptr;
// Palette writes never wait for core1 either (TX-1696: ~70 a frame plus the
// copper's, 10 ms/frame of core0 waiting, hw 2026-10-09). rawPal/lut are the
// RENDERER's copy; core0 keeps cRaw (what the guest reads back) and logs every
// entry {pal, idx, raw} into palLog; a posted line carries the log position and
// the renderer applies the log up to it (palApply) before composing the line.
static uint16_t (*cRaw)[256] = nullptr;      // [8][256] core0's view
static uint32_t* palLog = nullptr;
#if NEXT_TRACE
extern volatile uint32_t g_nx_logapply_spr, g_nx_logapply_pal;   // defined with g_nextvid_layer_us
#endif           // [PAL_LOG] pal << 24 | idx << 16 | raw
static constexpr uint32_t PAL_LOG = 512;
static volatile uint32_t pal_w = 0, pal_app = 0;
uint8_t  (*NEXTVID::lut)[256] = nullptr;
uint8_t* NEXTVID::remap = nullptr;
bool     NEXTVID::palHasPrio[8];

uint8_t NEXTVID::clip[4][4];
uint8_t NEXTVID::clipIdx[4];

uint8_t NEXTVID::palIdx = 0;
bool    NEXTVID::pal9Second = false;
uint8_t NEXTVID::pal9First = 0;
uint8_t NEXTVID::palCtrl = 0;

uint8_t* NEXTVID::lineBuf = nullptr;
uint8_t* NEXTVID::gigsPrev = nullptr;
uint8_t  NEXTVID::gigsBlend[256];
// 0 = off, 1 = blend with the previous frame, 2 = only record this frame (the
// first frame after the buffer came up, or Auto between flips)
static uint8_t s_gigs = 0;
// Gigascreen prev for the BORDER: one nibble per fb pixel, xres/2 bytes a row
// over the whole fb (paper columns unused). The paper keeps NEXTVID::gigsPrev.
// Across the Edge gigascreens its border too (per-line #FE colours alternating
// per frame): on a Pentagon the shared Gigascreen blends the whole row, here
// the border flickered unblended (owner, 2026-10-20).
static uint8_t* s_gigsPrevBrd = nullptr;
static bool    s_gigsSeed = true;
uint8_t* NEXTVID::pairMap = nullptr;
uint8_t* NEXTVID::hiresRow = nullptr;

// ── 640-wide frames (80-column tilemap) through the pair driver ──────────────
// The tilemap is rendered twice, once per phase of the 640-pixel grid (even
// source pixels into nxT, odd into nxT2); a row whose two phases differ is
// mixed twice (lineBuf = left pixels, p80Line2 = right pixels) and every fb
// byte becomes the pair (left, right) of 16 colours. The 16 are picked per
// frame from what the frame actually shows (p80Hist, counted by the renderer)
// and kept in VIDEO::profi_palette_live. Everything else of the frame (Layer 2
// 320, ULA, sprites, border) is 320-wide and comes out as solid pairs.
// One block, allocated when the mode first comes on, SRAM only (read per pixel):
//   p80Line2 384 | p80Map16 256 | p80Solid 256 | nxT2 384x2 | nxUs 384x2 | nxL2nd 384x2 | p80Hist 256x4
// Layer 2 640x256x4bpp goes the same way (SymbOS ZNX): the left nibble of a
// byte into nxL, the right one into nxL2nd, and the row is mixed twice.
bool     NEXTVID::p80Live = false;
uint8_t* NEXTVID::l2Band = nullptr;
static uint8_t*  p80Blk   = nullptr;
// The renderer's per-row scratch (p80Line2 / nxT2 / nxUs and the layer lines)
// belongs to the core that renders the row: see NxCtx below. These are the
// shared buffers core1 always uses and core0 uses while it does not help.
static uint8_t*  p80Line2b = nullptr;
static uint8_t*  p80Map16 = nullptr;   // palette index -> pair colour 0..15
static uint8_t*  p80Solid = nullptr;   // palette index -> pair slot (c, c)
static uint16_t* nxT2b    = nullptr;
static uint16_t* nxUsb    = nullptr;   // nxU before the first mix (nxUlaMerge works in place)
static uint16_t* nxL2ndb  = nullptr;   // Layer 2 640x256: the right pixel of each byte
static uint32_t* p80Hist  = nullptr;   // palette index -> pixels this frame (renderers write, core0 reads)
static uint8_t   p80Cur[16];           // palette index shown in each pair colour (0xFF = unused)
static void nxCtxRefresh();
static constexpr unsigned P80_BLK = 384 + 256 + 256 + 384 * 2 * 3 + 256 * 4;

static inline bool nxIsSram(const void* p) {
    const uintptr_t a = (uintptr_t)p;
    return a >= 0x20000000u && a < 0x20082000u;
}

// A per-pixel table: SRAM or nothing (HOT_SRAM falls back to butter PSRAM, where
// the per-pixel reads would cost what the band / the pair mode exists to save).
static uint8_t* nxSramBlock(unsigned bytes) {
    uint8_t* p = (uint8_t*)Buffer::palloc(bytes, Buffer::NEED_POINTER | Buffer::HOT_SRAM);
    if (p && !nxIsSram(p)) { Buffer::pfree(p); p = nullptr; }
    return p;
}

static bool p80Alloc() {
    if (p80Blk) return true;
    p80Blk = nxSramBlock(P80_BLK);
    if (!p80Blk) return false;
    p80Line2b = p80Blk;
    p80Map16 = p80Line2b + 384;
    p80Solid = p80Map16 + 256;
    nxT2b    = (uint16_t*)(p80Solid + 256);
    nxUsb    = nxT2b + 384;
    nxL2ndb  = nxUsb + 384;
    p80Hist  = (uint32_t*)(nxL2ndb + 384);
    nxCtxRefresh();
    return true;
}
uint8_t* NEXTVID::sprPatterns = nullptr;
uint8_t  (*NEXTVID::sprAttr)[5] = nullptr;
uint16_t NEXTVID::sprPatWrite = 0;
uint8_t  NEXTVID::sprAttrSlot = 0;
uint8_t  NEXTVID::sprAttrByte = 0;
uint8_t  NEXTVID::sprFlags = 0;
uint8_t  NEXTVID::sprNrSlot = 0;
NEXTVID::SprRender* NEXTVID::sprR = nullptr;
uint32_t* NEXTVID::sprLog = nullptr;
// sprLog positions, free-running: core0 writes spr_w, the renderer spr_app.
// 4096 (was 1024): Atic Atac Remake writes ~500 attribute bytes a frame and the
// renderer applies the log a line at a time, so with core0 a frame ahead the
// log held two frames at most and overflowed 19 times a second — each a full
// c1Drain of up to 7 ms (3.5 ms of core0 a frame, hw 2026-10-19). Then 32768: the
// game streams sprite PATTERNS through port 0x5B by DMA, 5-6k log entries a
// frame, and 4096 overflowed every frame (15 drains/s). PSRAM, 128 KB.
static constexpr uint32_t SPR_LOG = 32768;
static volatile uint32_t spr_w = 0, spr_app = 0;
// core0's OWN copy of the sprite render state, with its own log position
// (the row helper, c0Help): the state is composed per row from the log applied
// up to the row's position, so with one shared copy core0 could take a row
// beside core1 only when it needed nothing applied. Atic Atac Remake writes
// sprite attributes all through the frame: every queued row needed the log,
// core0 rendered 0-5 rows a frame and core1 did 25 ms of render alone (hw
// 2026-10-19; the 1024-entry log's full drains had hidden it). Lives in the
// helper's SRAM block; (re)built from sprAttr (the log at spr_w) when it is
// taken and after a log overflow. Pattern bytes are applied by both cores.
static NEXTVID::SprRender* s_sprR0 = nullptr;
static volatile uint32_t   spr_app0 = 0;
static volatile uint8_t    s_sprR0Valid = 0;
// Log position just past the last PATTERN byte written. Patterns are ONE
// memory for both cores, so a core may take a row only if no pattern entry
// lies between the other core's log position and the row's own (else the
// other core's rows in flight would show the newer patterns: Atic Atac
// Remake scrolls its ticker by rewriting the sprite patterns every frame
// and the text jumped a frame ahead for one frame, hw 2026-10-19).
static volatile uint32_t   s_sprPatW = 0;
// The test is on the ROW's own last pattern (NextLineState::sprPatW), not the
// live s_sprPatW: the ticker writes patterns all frame long, so the live value
// is nearly always past the row and an "after the row" escape let pattern
// entries BETWEEN the other core and the row through (the jump came back,
// hw 2026-10-20).
static inline bool nxSprPatClear(uint32_t otherPos, uint32_t rowPatW) {
    return (int32_t)(rowPatW - otherPos) <= 0;
}
static inline bool nxOnCore0() { return (*(const uint32_t*)SIO_BASE & 1) == 0; }

uint8_t* NEXTVID::copperMem = nullptr;
uint16_t NEXTVID::copIndex = 0;
uint16_t NEXTVID::copPC = 0;
uint8_t  NEXTVID::copCtrl = 0;
bool     NEXTVID::cop16Second = false;
uint8_t  NEXTVID::cop16First = 0;

uint32_t NEXTVID::lineIdx = 0;
uint8_t NEXTVID::contend = 0;
int16_t NEXTVID::contLine = 0;
int16_t NEXTVID::contCol = 0;
uint8_t NEXTVID::contWait[8] = {6, 5, 4, 3, 2, 1, 0, 0};
uint32_t NEXTVID::totalLines = 311;
uint32_t& NEXTVID::nextLineTstate = VIDEO::ts_line_t;   // see NextVideo.h
int  NEXTVID::firstVisibleLine = 0;
int  NEXTVID::paperTopRow = 0;
int  NEXTVID::xPaperOff = 32;
bool NEXTVID::skipFrame = false;
static NextFramePacing framePacing;

// Everything the scanline renderer reads that the guest (or the copper) can
// change between two lines, captured by core0 when the line is posted. What is
// NOT here is read live by whichever core renders, and its writers drain the
// core1 queue first: the palettes (lut/rawPal/palHasPrio), sprite attributes
// and patterns, the Timex hi-res LUT and pair map, the framebuffer rows.
// Guest RAM (ULA screen, Layer 2, tilemap) is read live WITHOUT a drain — core1
// lags core0 by a few lines, the same race the beam has against a program
// drawing into the visible page (the TS-Conf rule).
struct NextLineState {
    const uint8_t* grmem;
    uint32_t sprSeq;         // sprite attribute log position (NEXTVID::sprLog)
    uint32_t palSeq;         // palette log position (palLog)
    uint32_t sprPatW;        // s_sprPatW at posting: just past the last PATTERN entry the row needs
    uint8_t clip[4][4];
    uint8_t r12, r14, r15, r16, r17, r2F, r30, r31, r32, r33, r42, r4A, r4B, r4C, r68, r69, r6B, r6C, r6E, r6F, r70, r71;
    uint8_t p123B, palCtrl, border, flashing, hires, osd;
    uint8_t gigs;            // Gigascreen this frame (s_gigs)
    uint8_t attOvr;          // 1 = attr[] replaces the ULA attribute row (DMA multicolour)
    uint8_t r26, r27;        // ULA scroll X / Y (keeps the struct word-aligned)
    uint8_t r6A;             // LoRes: Radastan, display-file XOR, palette offset
    uint8_t bmpOvr;          // 1 = s_bmpSh row y replaces the ULA bitmap line (captured at the beam)
    uint8_t _pad6A[2];
    // Border colour changes INSIDE the row (OUT #FE mid-line): segment i ends
    // at fb x 2*brd.t[i] (T-states into the row, cell-aligned) in colour nibble i of brd.c;
    // the rest of the row is `border`. n = 0 for an ordinary row (every byte
    // zero, so it still dedups). 15 runs: a colour-bar test changes #FE eight
    // times a row, and OUT (C),r every 12 T fits ~15 into the 180-T window.
    // 24 bytes: 64 queued states = 1.5 KB of heap.
    struct { uint8_t t[15]; uint8_t n; uint8_t c[8]; } brd;
    // The ULA attribute row as the per-scanline DMA left it (Z80DMA's write-
    // order shadow, see captureAttrAfterTransfer): copied when the line is
    // posted, because core1 reads guest RAM later, when the charrow already
    // holds its LAST row (NaPICu: 8x8 blocks instead of 8x1 art, hw 2026-10-08).
    uint8_t attr[32];
};
// ── Who renders a row: the renderer's per-row state, one copy per core ──────
// Rows are rendered by core1 from the queue and, while it waits for core1 (a
// full queue, a drain), by core0 too (nxHelp): two rows at once. Everything a
// row writes while it is composed lives in the core's NxCtx. core1 always uses
// the shared buffers (the Next heap block, P80Blk, l2Band); core0 uses them too
// (it only ever renders with the queue drained) unless it has its own set
// (s_c0Blk, taken while a 640-wide pair frame is live — the mode the split is
// for). The band cache state belongs to the band buffer, not to the core: core0
// rendering a copper row on the shared band must see what core1 left in it.
// buf nullptr = l2Band. The buffer is NX_L2_BAND bytes: the 320 x 8 column band,
// then the 256-entry Layer 2 colour LUT (lutKey = pal | palOfs << 8 | transp << 16,
// lutGen = s_palGen[pal] when it was built).
// gen / fillJob / pend*: the two cores' bands are shared read-only (see
// nxL2BandLoad): gen is odd while the band is being written, fillJob is the
// queue position of the row that filled it, pend* say what is being written.
struct NxBand { uint8_t* buf; uint32_t key; int row, lastRow; uint32_t lutKey; uint16_t lutGen;
                volatile uint32_t gen; uint32_t fillJob; uint32_t pendKey; int pendRow; uint32_t pendJob; };
static constexpr unsigned NX_L2_BAND = 320 * 8 + 512;
struct NxCtx {
    const NextLineState* ls;   // the line being rendered
    // Per-layer RGB line buffers of the compositor (RenderLayers), [384] each.
    uint16_t *u, *t, *l, *s;
    uint16_t *t2, *us;         // 640 pair frame: the tilemap's odd phase, nxU before the first mix
    uint16_t *l2nd;            // 640 pair frame: Layer 2 640x256's right pixels
    uint8_t  *line, *line2;    // lineBuf (left pixels), p80Line2 (right pixels)
    NxBand*  band;
    int      dx0, dx1;         // this row's two phases differ in [dx0, dx1) (line2 valid there)
    bool     rowDiff;
    // The line's Layer 2 / sprite renderers wrote at least one opaque pixel into
    // nxL / nxS. A layer that is enabled but empty on this row is dropped from the
    // mix (NXModPlayer: L2 on, all transparent, no sprites — mix 8.8 of 24 ms of
    // core1 a frame, hw 2026-10-10).
    bool     rowL2, rowSpr;
    int16_t  sprX0, sprX1;     // fb x span [x0, x1) of this row's opaque sprite pixels (rowSpr)
    bool     sample;           // NEXT_TRACE: this row is timed
};
static NxBand s_band[2] = { { nullptr, 0xFFFFFFFFu, -1000, 0, 0xFFFFFFFFu, 0 }, { nullptr, 0xFFFFFFFFu, -1000, 0, 0xFFFFFFFFu, 0 } };
static NxCtx  s_nxc[2];
static uint16_t* nxUb = nullptr;       // the shared layer lines (Next heap block)
// The renderer's context: SIO CPUID is 0 / 1. const: one load per function, the
// compiler keeps the pointer (a core never changes under a running function).
static inline __attribute__((always_inline, const)) NxCtx* nxCtx() {
    return &s_nxc[*(const uint32_t*)SIO_BASE & 1];
}
#define LS        (nxCtx()->ls)
#define nxU       (nxCtx()->u)
#define nxT       (nxCtx()->t)
#define nxL       (nxCtx()->l)
#define nxS       (nxCtx()->s)
#define nxT2      (nxCtx()->t2)
#define nxUs      (nxCtx()->us)
#define nxL2nd    (nxCtx()->l2nd)
#define p80Line2  (nxCtx()->line2)
#define s_rowDiff (nxCtx()->rowDiff)
#define s_dx0     (nxCtx()->dx0)
#define s_dx1     (nxCtx()->dx1)
#define nxRowL2   (nxCtx()->rowL2)
#define nxRowSpr  (nxCtx()->rowSpr)
#define nxSprX0   (nxCtx()->sprX0)
#define nxSprX1   (nxCtx()->sprX1)
#if NEXT_TRACE
#define nx_layer_sample (nxCtx()->sample)
#endif
static void nxCtxShared(NxCtx& c) {
    c.u = nxUb; c.t = nxUb ? nxUb + 384 : nullptr; c.l = nxUb ? nxUb + 768 : nullptr; c.s = nxUb ? nxUb + 1152 : nullptr;
    c.t2 = nxT2b; c.us = nxUsb; c.l2nd = nxL2ndb; c.line = NEXTVID::lineBufShared(); c.line2 = p80Line2b;
    c.band = &s_band[0];
}
static uint8_t* s_c0Blk = nullptr;     // core0's own row scratch (nxHelp), or nullptr
static void nxCtxRefresh() {
    nxCtxShared(s_nxc[1]);
    if (!s_c0Blk) nxCtxShared(s_nxc[0]);
}
static_assert(sizeof(NextLineState) % 4 == 0, "compared a word at a time");
// memcmp is libc in FLASH; 12 words compared inline instead, once per line.
static inline __attribute__((always_inline)) bool nxSameState(const NextLineState& a, const NextLineState& b) {
    const uint32_t* x = (const uint32_t*)&a; const uint32_t* y = (const uint32_t*)&b;
    uint32_t d = 0;
    for (unsigned i = 0; i < sizeof(NextLineState) / 4; i++) d |= x[i] ^ y[i];
    return d == 0;
}   // the line being rendered (one renderer at a time)

IRAM_ATTR void NEXTVID::captureLine(NextLineState& st) {
    const uint8_t* r = NextReg::reg;
    st.grmem = VIDEO::grmem;
    memcpy(st.clip, clip, sizeof st.clip);
    st.r12 = r[0x12];
    st.r14 = r[0x14];
    st.r15 = r[0x15];
    st.r16 = r[0x16];
    st.r17 = r[0x17];
    st.r2F = r[0x2F];
    st.r30 = r[0x30];
    st.r31 = r[0x31];
    st.r32 = r[0x32];
    st.r33 = r[0x33];
    st.r42 = r[0x42];
    st.r4A = r[0x4A];
    st.r4B = r[0x4B];
    st.r4C = r[0x4C];
    st.r68 = r[0x68];
    st.r69 = r[0x69];
    st.r6B = r[0x6B];
    st.r6C = r[0x6C];
    st.r6E = r[0x6E];
    st.r6F = r[0x6F];
    st.r70 = r[0x70];
    st.r71 = r[0x71];
    st.p123B = NextReg::port123B;
    st.palCtrl = palCtrl;
    st.border = VIDEO::borderColor;
    st.flashing = VIDEO::flashing;
    st.hires = VIDEO::timex_hires_live ? 1 : (p80Live ? 2 : 0);
    st.osd = VIDEO::OSD;
    st.gigs = s_gigs;
    st.attOvr = 0;
    st.r26 = r[0x26];
    st.r6A = r[0x6A];
    st.bmpOvr = 0;
    st._pad6A[0] = st._pad6A[1] = 0;
    st.sprSeq = spr_w;
    st.sprPatW = s_sprPatW;
    st.palSeq = pal_w;
    st.r27 = r[0x27];
    uint32_t* w = (uint32_t*)&st.brd;   // no libc memset on this path (flash)
    for (unsigned i = 0; i < sizeof st.brd / 4; i++) w[i] = 0;
    w = (uint32_t*)st.attr;
    for (unsigned i = 0; i < sizeof st.attr / 4; i++) w[i] = 0;
}

// The DMA attribute shadow for this row, if the per-scanline DMA wrote one
// into the page on show (Pentagon applies the same rule in MainScreen_Blank).
// Only reached while the shadow exists (a DMA into an attribute area).
__attribute__((noinline)) static void nxAttrShadow(NextLineState& st, int row, int paperTop) {
    const int y = row - paperTop;
    if (y < 0 || y >= 192 || !Z80DMA::dma_attr_valid[y]) return;
    if (Z80DMA::dma_attr_page[y >> 3] != st.grmem) return;
    if ((st.r69 & 0x43) && !(st.r69 & 0x40)) return;   // Timex screen 1 / hi-colour: other bytes
    const uint32_t* src = (const uint32_t*)&Z80DMA::dma_attr_shadow[y * 32];
    uint32_t* d = (uint32_t*)st.attr;
    for (int i = 0; i < 8; i++) d[i] = src[i];
    st.attOvr = 1;
}

// ---- per-T border (OUT #FE mid-line) ----
// The ULA paints the border with the colour of the moment, so a program that
// rewrites #FE inside a line splits that line into coloured runs (the 48K
// border tests, rainbow demos). VIDEO::DrawBorder() is called on every colour
// change with the OLD colour still in VIDEO::borderColor: borderMark() keeps
// {T of the change, old colour} in a small ring, and emitRow turns the changes
// inside the row's T window into segments. The window comes from the classic
// beam renderer's border anchor (VIDEO::tStatesBorder, hw-verified incl. its
// pipeline correction): fb x of raster line L shows T = L*tpl + s_bcol + x/2.
#define NX_BRD_N 32
static uint32_t s_brdT[NX_BRD_N];
static uint8_t  s_brdC[NX_BRD_N];
static uint8_t  s_brdHead = 0, s_brdCnt = 0;
static int16_t  s_bcol = 0;
// The border is latched in cells of s_bstep T-states — 4 (8 px) on the 48K /
// 128K / +3 ULA, 1 on Pentagon — the classic renderer's brdcol_step. A cell
// that STARTS at or before the change still shows the old colour
// (VIDEO::TopBorder paints every cell with lastBrdTstate <= tstates first).
static uint8_t  s_bstep = 4;
// T-states of side border a wider fb adds left of the 320-wide one (10 at 360):
// the cell grid stays aligned to the 320 layout's x 0, i.e. to the raster.
static int16_t  s_bpad = 0;

void NEXTVID::borderMark() {
    if (s_brdCnt == NX_BRD_N) { s_brdHead = (s_brdHead + 1) % NX_BRD_N; s_brdCnt--; }
    const uint8_t i = (s_brdHead + s_brdCnt) % NX_BRD_N;
    s_brdT[i] = CPU::tstates;
    s_brdC[i] = VIDEO::borderColor & 7;
    s_brdCnt++;
}

// Flash: runs only while the ring holds changes (a few per row at most).
__attribute__((noinline)) static void nxBorderSegs(NextLineState& st, int row, int fvLine) {
    const uint8_t m = ESPectrum::multiplicator;
    const int32_t t0 = ((row + fvLine) * (int32_t)VIDEO::tStatesPerLine + s_bcol) * (1 << m);
    const int32_t t1 = t0 + ((int32_t)VIDEO::vga.xres / 2) * (1 << m);
    while (s_brdCnt && (int32_t)s_brdT[s_brdHead] < t0) {   // ended before this row
        s_brdHead = (s_brdHead + 1) % NX_BRD_N; s_brdCnt--;
    }
    uint8_t n = 0, k = 0;
    for (; k < s_brdCnt; k++) {
        const uint8_t i = (s_brdHead + k) % NX_BRD_N;
        const int32_t t = (int32_t)s_brdT[i];
        if (t >= t1) break;
        if (n < 15) {
            const int32_t rel = (t - t0) >> m;   // >= 0: earlier changes were dropped
            // cell grid of the 320 layout (rel - s_bpad may be negative: +16
            // keeps the division a floor for both steps, 4 and 1)
            const int32_t g = rel - s_bpad + 16;
            st.brd.t[n] = (uint8_t)((g / s_bstep + 1) * s_bstep - 16 + s_bpad);
            st.brd.c[n >> 1] |= (uint8_t)(s_brdC[i] << ((n & 1) * 4));
            n++;
        }
    }
    st.brd.n = n;
    // colour after the last in-row change: the one in force until the next
    // change (already recorded if it happened before this row was posted)
    st.border = (k < s_brdCnt) ? s_brdC[(s_brdHead + k) % NX_BRD_N] : (VIDEO::borderColor & 7);
}

// ---- ULA screen flips mid-frame (#7FFD bit 3 / NR 0x69 bit 6) ----
// The ULA fetches a row's pixels and attributes from the page in force while
// the beam crosses its paper, but a row is POSTED at the end of its raster
// line — after a flip in the right border has already moved VIDEO::grmem.
// Insult (Code Busters, Pentagon): 16 PUSHes = 32 attribute bytes into the
// OFF-screen page, then OUT (#7FFD) XOR #0A (screen 5<->7, bank 7<->5) every
// 448 T = two lines; posting with the new page gave half the rows the colours
// of the next pair ("multicolour wrong", hw 2026-10-20, right on Pentagon
// whose MainScreen reads per column). Same shape as the #FE ring above: a ring
// of {T, page shown BEFORE the flip}, resolved per row at mid-paper.
#define NX_FLIP_N 8
static uint32_t s_flipT[NX_FLIP_N];
static uint8_t  s_flipV[NX_FLIP_N];   // page shown before the flip: 5 or 7
static uint8_t  s_flipHead = 0, s_flipCnt = 0;
static uint8_t  s_flipFrame = 0, s_flipPrev = 0;   // flips this frame / last frame

void NEXTVID::screenFlipMark() {
    if (s_flipCnt == NX_FLIP_N) { s_flipHead = (s_flipHead + 1) % NX_FLIP_N; s_flipCnt--; }
    const uint8_t i = (s_flipHead + s_flipCnt) % NX_FLIP_N;
    s_flipT[i] = CPU::tstates;
    s_flipV[i] = MemESP::videoLatch ? 7 : 5;
    s_flipCnt++;
    if (s_flipFrame < 255) s_flipFrame++;
}

// The page this row's paper was fetched from: flips before the row's mid-paper
// T belong to it (and to every later row — drop them), the first flip after it
// says which page was still on show.
static void nxScreenFlipRow(NextLineState& st, int row, int fvLine) {
    const uint8_t m = ESPectrum::multiplicator;
    const int32_t tm = ((row + fvLine) * (int32_t)VIDEO::tStatesPerLine + s_bcol
                        + (int32_t)VIDEO::vga.xres / 4) * (1 << m);
    while (s_flipCnt && (int32_t)s_flipT[s_flipHead] < tm) {
        s_flipHead = (s_flipHead + 1) % NX_FLIP_N; s_flipCnt--;
    }
    if (s_flipCnt) st.grmem = MemESP::ram[s_flipV[s_flipHead]].direct();
}

// ---- the attribute row at the BEAM (8x1 attribute multicolour) ----
// A row is posted at the END of its raster line and core1 reads its 32
// attribute bytes from RAM later still, while the ULA read them as the beam
// crossed the paper. A multicolour writer rewrites the same 32 bytes every
// line, behind the beam (Shock by ESI, the "multicolor" part: every paper row
// came out in some later line's colours on the Next, hw 2026-10-23; the
// Pentagon's MainScreen reads per column and never saw it). So the line clock
// has TWO boundaries per line: the paper start, where nxPaperStart() copies the
// row's attribute bytes (s_capAttr), and the line end, where the row is posted
// — nxAttrBeam() then gives the row that copy instead of the live RAM.
//
// One snapshot per row cannot be beam-exact: a writer running AHEAD of the
// beam inside the row's own paper is shown one line late. The classic writers
// (PUSH / LD (HL) races) run behind it, which the paper-start snapshot gets
// right, and the mid-paper point the flip ring uses would get half wrong.
//
// Gated, because a copy makes every row a new queue state (nxSameState), i.e.
// 64 rows in flight instead of 320: the copy is used while attributes CHANGE
// within a character row (two paper-start captures of adjacent lines of the
// same cell row differ, or the capture differs from RAM at the row's post),
// for the rest of that frame and the next two, and whenever the screen flips
// inside the frame (Insult: PUSHes into the off-screen page, the old
// post-time snapshot's case — the page on show at the paper start is the same
// page). A game that writes attributes once a frame trips it only when that
// write lands while the beam is inside the cell row, one frame at a time.
//
// Once a racing writer is detected the capture is PER CELL GROUP: 16 boundaries
// across the paper, 8 T apart, each copying the two cells the beam is about to
// read (NX_CAP_GROUPS). Shock's writer is a PUSH racer going right to left that
// also writes the next cell row ahead of the beam, so within one line some cells
// are written before the beam reaches them and others after: measured on the
// host against exact beam sampling, one point per line is 2.6 % of cells wrong
// on the 48K raster, 16 points 0.01 % (`shock_sim`, session scratch 2026-10-23).
// Idle titles pay one extra boundary per line and a 32-byte copy.
//
// The BITMAP line travels the same way while the writer is on, through a
// per-line shadow (`s_bmpSh`, 192 x 32 B, PSRAM, taken on first use) rather
// than the state: Shock rewrites its scroller's 14 bitmap lines right after the
// beam has passed them (for the NEXT frame), and a row core1 happened to read
// after that rewrite showed the next frame's scroll position — a scroller that
// twitched on the Next and not on a Pentagon (owner, 2026-10-23). The shadow is
// single-buffered: row y is rewritten one frame later, and core1 never lags a
// whole frame behind (the state slots cap it at 63 rows).
#define NX_CAP_GROUPS 16
static uint8_t  s_capAttr[32];
static const uint8_t* s_capGrmem = nullptr;
static int16_t  s_capY = -1;            // paper row s_capAttr holds, -1 = none
static uint8_t* s_bmpSh = nullptr;      // [192][32] bitmap lines as the beam read them
static int16_t  s_bmpCapY = -1;         // paper row whose bitmap line was captured this line
static uint8_t  s_bmpShTried = 0;       // one palloc attempt per frame
static uint8_t  s_capPhase = 0;         // next boundary: g < NX_CAP_GROUPS = capture point g of this line, else its end
static uint8_t  s_mcSeen = 0;           // attributes changed within a cell row this frame
static uint8_t  s_mcActive = 0;         // frames the copy stays on after the last such change
static int16_t  s_pStartT = 64;         // T into a raster line where the paper begins (beside s_bcol)

static inline __attribute__((always_inline)) bool nxWords8Differ(const uint8_t* a, const uint8_t* b) {
    const uint32_t* x = (const uint32_t*)a; const uint32_t* y = (const uint32_t*)b;
    uint32_t d = 0;
    for (int i = 0; i < 8; i++) d |= x[i] ^ y[i];
    return d != 0;
}

// Arms nextLineTstate for the current lineIdx / s_capPhase. Flash: once per
// boundary (the .nxovl window is full).
__attribute__((noinline)) void NEXTVID::nxLineArm() {
    const uint8_t m = ESPectrum::multiplicator;
    if (lineIdx >= totalLines) { nextLineTstate = 0xFFFFFFFF; return; }
    const uint32_t tl = (uint32_t)VIDEO::tStatesPerLine << m;
    nextLineTstate = s_capPhase >= NX_CAP_GROUPS
        ? (lineIdx + 1) * tl
        : lineIdx * tl + ((uint32_t)(s_pStartT + 8 * s_capPhase) << m);
}

// Flash: the capture boundaries of a raster line (ScanlineWork, phase < NX_CAP_GROUPS).
__attribute__((noinline)) void NEXTVID::nxPaperStart() {
    const uint8_t g = s_capPhase;
    const int y = (int)lineIdx - firstVisibleLine - paperTopRow;
    const uint8_t* r = NextReg::reg;
    if (g == 0) {
        if (y < 0 || y >= 192 || (r[0x69] & 0x07) || (r[0x68] & 0x80) || !VIDEO::grmem) {
            s_capY = -1; s_capPhase = NX_CAP_GROUPS; nxLineArm(); return;
        }
        const uint8_t* src = VIDEO::grmem + VIDEO::offAtt[y];
        if ((y & 7) && s_capY == y - 1 && s_capGrmem == VIDEO::grmem && nxWords8Differ(src, s_capAttr))
            s_mcSeen = 1;                // the same 32 bytes changed between two lines of one cell row
        uint32_t* d = (uint32_t*)s_capAttr; const uint32_t* s = (const uint32_t*)src;
        for (int i = 0; i < 8; i++) d[i] = s[i];   // the row at the paper start; groups refine it
        s_capY = (int16_t)y;
        s_capGrmem = VIDEO::grmem;
        s_bmpCapY = -1;
        const bool racing = s_mcSeen || s_mcActive || s_flipFrame >= 2 || s_flipPrev >= 2;
        if (racing) {
            if (!s_bmpSh && !s_bmpShTried) {
                s_bmpShTried = 1;
                s_bmpSh = (uint8_t*)Buffer::palloc(192 * 32, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
                if (!s_bmpSh) Debug::log("[NXV] no PSRAM for the ULA bitmap shadow (6 KB)\n");
            }
            if (s_bmpSh) {
                uint32_t* bd = (uint32_t*)(s_bmpSh + y * 32); const uint32_t* bs = (const uint32_t*)(VIDEO::grmem + VIDEO::offBmp[y]);
                for (int i = 0; i < 8; i++) bd[i] = bs[i];
                s_bmpCapY = (int16_t)y;
            }
        }
        s_capPhase = racing ? 1 : NX_CAP_GROUPS;
        nxLineArm();
        return;
    }
    // group g: cells 2g, 2g+1 as the beam reaches them (attributes and pixels)
    if (s_capY == y && s_capGrmem == VIDEO::grmem) {
        *(uint16_t*)&s_capAttr[2 * g] = *(const uint16_t*)(VIDEO::grmem + VIDEO::offAtt[y] + 2 * g);
        if (s_bmpCapY == y)
            *(uint16_t*)(s_bmpSh + y * 32 + 2 * g) = *(const uint16_t*)(VIDEO::grmem + VIDEO::offBmp[y] + 2 * g);
    }
    s_capPhase = (uint8_t)(g + 1);
    nxLineArm();
}

// At the row's post: the attribute bytes the beam read, when a writer is racing it.
static void nxAttrBeam(NextLineState& st, int row, int paperTop) {
    if (st.attOvr || (st.r69 & 0x07)) return;   // DMA shadow first; Timex modes: other bytes
    const int y = row - paperTop;
    if (y < 0 || y >= 192) return;
    const uint8_t* live = st.grmem + VIDEO::offAtt[y];
    const bool mc = s_mcSeen || s_mcActive || s_flipFrame >= 2 || s_flipPrev >= 2;
    if (s_capY == y && s_capGrmem == st.grmem) {
        const bool moved = nxWords8Differ(s_capAttr, live);   // written inside this line's paper
        if (moved) s_mcSeen = 1;
        if (!(mc || moved)) return;
        const uint32_t* s = (const uint32_t*)s_capAttr; uint32_t* d = (uint32_t*)st.attr;
        for (int i = 0; i < 8; i++) d[i] = s[i];
        if (s_bmpCapY == y) st.bmpOvr = 1;   // the bitmap line as the beam read it (s_bmpSh)
    } else {
        if (!mc) return;                 // no capture for this row (page flipped mid-paper): the post-time bytes
        memcpy(st.attr, live, sizeof st.attr);
    }
    st.attOvr = 1;
}

// Everything a posted row needs beyond captureLine(): the page at its paper
// time, the #FE segments, the DMA attribute shadow, the attribute copy. One
// flash call from the three posting sites (the .nxovl window is full).
__attribute__((noinline)) static void nxRowFixups(NextLineState& st, int row, int fv, int pt) {
    if (s_flipCnt) nxScreenFlipRow(st, row, fv);
    if (s_brdCnt) nxBorderSegs(st, row, fv);
    if (Z80DMA::dma_attr_valid) nxAttrShadow(st, row, pt);
    nxAttrBeam(st, row, pt);
}

// ---- the core1 line queue (NEXT_C1_*) ----
// Jobs are {row, state slot}; a state slot is written only when the captured
// inputs differ from the last posted line's, and reused only once core1 has
// consumed the last job naming it. Free-running indices, one writer each.
#define NEXT_C1_RING   320
#define NEXT_C1_STATES 64
static uint32_t*      nx_ring = nullptr;     // [NEXT_C1_RING]  row | slot << 16
static NextLineState* nx_st = nullptr;       // [NEXT_C1_STATES]
static uint32_t*      nx_st_job = nullptr;   // [NEXT_C1_STATES] nx_w after the last job naming the slot
static uint32_t       nx_cur = 0;
static bool           nx_have = false;
static volatile uint32_t nx_w = 0, nx_r = 0;
// Occupancy of the line ring, sampled at every publish. This is the number that
// decides whether NEXT_C1_RING can shrink (it is heap: 320 x 4 B + the state
// slots): a max well below the size means core1 keeps up line by line, a max
// pinned at the size means the poster is waiting on a full ring — which
// haltAdvanceTo always does after a HALT, because it posts a whole frame at
// once. Printed once a second on the NEXT_TRACE `NEXT cpu:` line.
#if NEXT_TRACE
volatile uint32_t g_nx_ring_max = 0, g_nx_ring_now = 0;
volatile uint32_t g_nx_ring_size = NEXT_C1_RING;   // one source of truth for the trace
#endif
static NextLineState  nx_sync;               // the queue-off path
// Mid-line palette writes by the copper (copper.vhd WAIT H, 8-pixel steps):
// recorded between the paper's left edge and the line end with the fb x they
// take effect at; the row is then drawn in segments (renderRowSegments).
struct NxPalDelta { uint16_t x; uint8_t pal, idx; uint16_t old, neu; };
static constexpr int NX_PAL_DELTAS = 32;
static NxPalDelta s_pd[NX_PAL_DELTAS];
static uint8_t  s_pdn = 0;
static bool     s_pdRec = false;       // ScanlineWork is inside the line's copper span
// A row with copper palette writes goes to the queue with its deltas (segPost):
// copied into a pool beside the ring, the ring slot names them (nx_segOf: pool
// start << 6 | count, 0 = an ordinary row). Such a row is rendered by ONE core
// with the other idle (it rolls the shared palette back and forward), so the
// claim waits for the other core and blocks the next claim (nx_segRun) — far
// cheaper than the old full drain on core0 (TX-1696: ~14 rows a frame, 10 ms
// of core0 a frame waiting, hw 2026-10-13). Pool full = the old drain path.
// Sized so that a whole ring of segment rows fits (2026-10-20: a scene with
// ~71 copper rows a frame overflowed 32 records twice a frame, and each overflow
// was a full drain, ~7 ms of core0). Pool start is 10 bits in nx_segOf (<< 6).
static constexpr unsigned NX_SEG_POOL = 1024;     // deltas, power of two, <= 1024
static constexpr unsigned NX_SEG_Q    = NEXT_C1_RING;   // posted segment rows in flight
static_assert(NX_SEG_POOL <= 1024 && (NX_SEG_POOL & (NX_SEG_POOL - 1)) == 0, "nx_segOf packs start << 6");
struct NxSegRec { uint32_t job, end; };
static NxPalDelta* s_segPool = nullptr;           // [NX_SEG_POOL]  (queue heap block)
static uint16_t*   nx_segOf  = nullptr;           // [NEXT_C1_RING]
static uint8_t*    nx_mapOf  = nullptr;           // [NEXT_C1_RING] post-time map-row copy, 0xFF none
static NxSegRec*   s_segQ    = nullptr;           // [NX_SEG_Q]
static uint32_t    s_segW = 0, s_segRel = 0, s_segQh = 0, s_segQt = 0;
static uint16_t    s_segPost = 0;                 // emitRow: the row being posted carries this
static volatile uint8_t  nx_segRun = 0;           // a segment row is being rendered
static uint16_t          s_jobSeg[2] = { 0, 0 };  // per core: the claimed job's segment word
static int32_t  s_copT = 0;            // copper clock, 28 MHz = 1/4 hc_ula, in the line being run
static int16_t  s_copXBase = 0;        // fb x of hc_ula 0 for the line being run
static const NextLineState* s_preCap = nullptr;   // emitRow: state already captured
static bool     s_lineReady = false;   // RenderLine: lineBuf already composed
// Tested by core1's render_core loop on every machine: one byte in .data.
extern "C" volatile bool g_next_c1_live = false;

// Two renderers on one queue. nx_c = the next job to claim; a claimed job is
// rendered by the core that claimed it and nx_r (the release point every waiter
// tests) only moves past it once it is done: nx_r = the oldest job still being
// rendered, or nx_c when neither core renders. Rows are composed with the logs
// (sprite attributes, palette) applied up to the row's capture position, and
// the logs only move forward — so a row may be claimed while the other core
// renders only if it needs nothing applied (the common case: a whole frame of
// rows on one log position); otherwise it waits for the other core. The claim
// applies the logs itself, under the lock, before anyone can read them.
// The lock is taken without masking IRQs: core1's HDMI line ISR must never wait
// for it, and no ISR touches the queue.
static volatile uint32_t nx_c = 0;
static volatile uint8_t  nx_busy[2] = { 0, 0 };
static volatile uint32_t nx_jobAt[2] = { 0, 0 };   // the job index each core renders
static spin_lock_t*      nx_lock = nullptr;
#if NEXT_TRACE
extern volatile uint32_t g_nextvid_us;
volatile uint32_t g_nx_c0_us = 0, g_nx_c0_rows = 0, g_nx_c1_rows = 0, g_nx_claim_blk = 0;
#endif

static void nxQueueDead(const char* why) {
    Debug::log("[NXC1] core1 render queue %s (r=%u c=%u w=%u) - rendering on core0", why,
               (unsigned)nx_r, (unsigned)nx_c, (unsigned)nx_w);
    g_next_c1_live = false;
    nx_c = nx_w;
    nx_r = nx_w;
}

__attribute__((noinline)) IRAM_ATTR bool NEXTVID::rowClaim(unsigned core, uint32_t& job) {
    if (nx_c == nx_w) return false;
    spin_lock_unsafe_blocking(nx_lock);
    const uint32_t c = nx_c;
    bool ok = false;
    if (c != nx_w && g_next_c1_live) {
        __dmb();
        const uint32_t j = nx_ring[c % NEXT_C1_RING];
        const NextLineState& st = nx_st[j >> 16];
        const uint16_t seg = nx_segOf ? nx_segOf[c % NEXT_C1_RING] : 0;
        if (nx_segRun) {
            ok = false;                       // the other core rolls the palette back right now
        } else if (core == 0 && st.hires == 1) {
            ok = false;                       // Timex hi-res: one shared row scratch (hiresRow)
        } else if (!nx_busy[core ^ 1]) {
            sprApply(st.sprSeq);              // nobody renders: move the logs on
            palApply(st.palSeq);
            if (sprCur()->dirty) resolveSprites();
            ok = true;
        } else {
            // the sprite ATTRIBUTES are per core (core0's copy must exist), the
            // palette and the sprite PATTERNS are shared: see nxSprPatClear
            ok = !seg && (int32_t)(st.palSeq - pal_app) <= 0 && (core == 1 || s_sprR0Valid) &&
                 nxSprPatClear(core == 0 ? spr_app : spr_app0, st.sprPatW);
        }
        if (ok) {
            s_jobSeg[core] = seg;
            if (seg) nx_segRun = 1;
            job = j;
            nx_jobAt[core] = c;
            nx_busy[core] = 1;
            nx_c = c + 1;
        }
#if NEXT_TRACE
        else g_nx_claim_blk = g_nx_claim_blk + 1;
#endif
    }
    spin_unlock_unsafe(nx_lock);
    return ok;
}

__attribute__((noinline)) IRAM_ATTR void NEXTVID::rowRelease(unsigned core) {
    __dmb();                                  // the row is written before it is released
    spin_lock_unsafe_blocking(nx_lock);
    nx_busy[core] = 0;
    if (s_jobSeg[core]) { s_jobSeg[core] = 0; nx_segRun = 0; }
    const uint32_t r = nx_busy[core ^ 1] ? nx_jobAt[core ^ 1] : nx_c;
    if ((int32_t)(r - nx_r) > 0) nx_r = r;
    spin_unlock_unsafe(nx_lock);
}

#if NEXT_TRACE
static volatile uint8_t s_frameSeq;   // walks the layer-timing sample across the rows
#endif
__attribute__((noinline)) IRAM_ATTR void NEXTVID::renderJob(uint32_t j) {
    LS = &nx_st[j >> 16];
    if (const uint16_t seg = s_jobSeg[*(const uint32_t*)SIO_BASE & 1]) {
        segCompose((int)(j & 0xFFFF), seg >> 6, NX_SEG_POOL - 1, seg & 63, true);
        return;
    }
#if NEXT_TRACE
    // every 16th job, shifted by one each frame: 240 rows a frame keep a plain
    // /16 counter on the same rows every frame (and rows 0, 16, 32... are
    // always the pair frame's histogram rows)
    static uint32_t s_sampleN = 0;
    if (((++s_sampleN + s_frameSeq) & 15) == 0) {
        const uint32_t t0 = timer_hw->timerawl;   // one load; time_us_64 is a retry loop
        nx_layer_sample = true;
        RenderLine((int)(j & 0xFFFF));
        nx_layer_sample = false;
        g_nextvid_us += (timer_hw->timerawl - t0) * 16;
    } else
#endif
    RenderLine((int)(j & 0xFFFF));
}

// core0, waiting for core1: render the oldest queued row here. Only with its
// own row scratch (s_c0Blk) — the shared one is core1's while the queue runs.
// Flash: once per row at most, and not a frame of the .nxovl window on core0's
// wait paths (a shared-VMA frame there has crashed GDB's unwinder).
__attribute__((noinline)) bool NEXTVID::c0Help() {
    if (!s_c0Blk || !g_next_c1_live) return false;
    if (!s_sprR0Valid) sprR0Sync();
    uint32_t j;
    if (!rowClaim(0, j)) return false;
#if NEXT_TRACE
    const uint64_t t0 = time_us_64();
#endif
    renderJob(j);
    rowRelease(0);
#if NEXT_TRACE
    g_nx_c0_us = g_nx_c0_us + (uint32_t)(time_us_64() - t0);
    g_nx_c0_rows = g_nx_c0_rows + 1;
#endif
    return true;
}

// core0's own row scratch: lineBuf 384 | 4 layer lines 4 x 768 | nxT2 768 |
// nxUs 768 | nxL2nd 768 | p80Line2 384, and the Layer 2 band (2560) as a SECOND block: one
// 7.9 KB piece was often not there on a fragmented heap (trace builds, NEXTEST:
// "no SRAM" and core1 rendered alone). SRAM or nothing (read per pixel).
static constexpr unsigned NX_C0_LINES = 384 + 4 * 768 + 768 + 768 + 768 + 384;
static constexpr unsigned NX_C0_BAND  = NX_L2_BAND;
static constexpr unsigned NX_C0_SPR   = (sizeof(NEXTVID::SprRender) + 3) & ~3u;   // core0's sprite state copy
static uint8_t* s_c0Band = nullptr;
static uint32_t s_c0Retry = 0;   // frames until the next allocation attempt
// Plain heap with a 3 KB margin, not HOT_SRAM's 8 KB: a trace build at NEXTEST
// had ~13 KB left after the 640 tables — never enough for the 8 KB rule. Taken
// at the first Next frame in every mode and kept across menu visits (it was
// freed on each, and a fragmented heap then had no room to take it back).
extern "C" size_t getLargestAllocatable(void);
static uint8_t* nxC0Sram(unsigned n) {
    if (getLargestAllocatable() < n + 3072) return nullptr;
    uint8_t* b = (uint8_t*)tryMalloc(n);
    if (b && !nxIsSram(b)) { free(b); b = nullptr; }
    return b;
}
static void nxHelperAlloc() {
    if (s_c0Blk || !g_next_c1_live) return;
    if (s_c0Retry) { s_c0Retry--; return; }
    uint8_t* b = nxC0Sram(NX_C0_LINES + NX_C0_SPR);
    uint8_t* bb = b ? nxC0Sram(NX_C0_BAND) : nullptr;
    if (!bb) {
        free(b);
        s_c0Retry = 50;                  // ~1 s: the first try after a menu visit often finds a fragmented heap
        Debug::log("[NXC1] core0 row helper: no SRAM (%u + %u B)", NX_C0_LINES + NX_C0_SPR, NX_C0_BAND);
        return;
    }
    NxCtx& c = s_nxc[0];
    c.line  = b;
    c.u     = (uint16_t*)(b + 384);
    c.t = c.u + 384; c.l = c.t + 384; c.s = c.l + 384;
    c.t2    = c.s + 384;
    c.us    = c.t2 + 384;
    c.l2nd  = c.us + 384;
    c.line2 = (uint8_t*)(c.l2nd + 384);
    s_band[1].buf = bb;
    s_band[1].key = 0xFFFFFFFFu; s_band[1].row = -1000; s_band[1].lastRow = 0;
    s_band[1].lutKey = 0xFFFFFFFFu;
    c.band = &s_band[1];
    s_c0Band = bb;
    s_sprR0 = (NEXTVID::SprRender*)(b + NX_C0_LINES);
    s_sprR0Valid = 0;                // built from sprAttr at the first row
    s_c0Blk = b;                     // last: c0Help tests it
    Debug::log("[NXC1] core0 row helper on, %u B @%08X + %u B @%08X", NX_C0_LINES + NX_C0_SPR,
               (unsigned)(uintptr_t)b, NX_C0_BAND, (unsigned)(uintptr_t)bb);
}

void NEXTVID::helperFree() {
    if (!s_c0Blk) return;
    c1Drain();                       // core0 renders nothing outside c0Help: free to swap
    uint8_t* b = s_c0Blk;
    s_c0Blk = nullptr;
    s_sprR0Valid = 0; s_sprR0 = nullptr;
    nxCtxShared(s_nxc[0]);
    s_band[1].buf = nullptr;
    free(b);
    free(s_c0Band);
    s_c0Band = nullptr;
    s_c0Retry = 0;
    Debug::log("[NXC1] core0 row helper off");
}

// core0: wait for core1 to finish every posted line. Bounded, like the TS-Conf
// drain: a wedged core1 must not take core0 with it.
#if NEXT_TRACE
// Perf baseline (NEXT perf: line): how often and how long core0 waits for
// core1's line queue, and how long core1 spends rendering.
volatile uint32_t g_nx_drain_n = 0, g_nx_drain_us = 0, g_nx_c1_us = 0;
// c1DrainRange waits by reason (0 L2, 1 map, 2 defs, 3 ULA/LoRes): count, us,
// lines waited for; and the 4 commonest DMA targets (bank << 16 | o0 hi | o1 hi).
volatile uint32_t g_nx_dmaw[4][3] = {}, g_nx_dmaw_key[4][2] = {}, g_nx_dmaw_sh = 0;
volatile uint32_t g_nx_drain_src[8] = {};    // waits per source: c1Drain's src, 4 = DMA range, 5 = sprite log full
volatile uint32_t g_nx_drain_ra = 0, g_nx_drain_ra_us = 0, g_nx_drain_ra_n = 0;   // caller of the longest wait
#endif
// core0 is spinning in c1Drain right now (waiting for the whole row queue; the
// other wait, nxWaitUntil, only happens with the queue near full, which
// sd_async_render_behind tests directly — the flag would not fit .nxovl):
// read by sdcard.c, through sd_async_render_behind(), to stop a speculative SD
// read-ahead that would keep core1 off the rows core0 is waiting for.
volatile uint8_t g_nx_c0wait = 0;
IRAM_ATTR void NEXTVID::c1Drain(uint8_t src) {
    (void)src;
    if (nx_r == nx_w) return;
    const uint64_t t0 = time_us_64();
    uint64_t tw = t0;
    g_nx_c0wait = 1;
    while (nx_r != nx_w) {
        if (s_c0Blk && c0Help()) { tw = time_us_64(); continue; }   // c0Help is flash: no call without a helper   // a row done here is a row core1 does not owe
        if (time_us_64() - tw > 100000) { nxQueueDead("stuck"); break; }
        tight_loop_contents();
    }
    g_nx_c0wait = 0;
#if NEXT_TRACE
    g_nx_drain_n = g_nx_drain_n + 1;
    g_nx_drain_src[src & 7] = g_nx_drain_src[src & 7] + 1;
    const uint32_t dt = (uint32_t)(time_us_64() - t0);
    g_nx_drain_us = g_nx_drain_us + dt;
    {
        const uint32_t ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
        if (ra == g_nx_drain_ra) g_nx_drain_ra_n = g_nx_drain_ra_n + 1;
        if (dt > g_nx_drain_ra_us) { g_nx_drain_ra_us = dt; if (ra != g_nx_drain_ra) g_nx_drain_ra_n = 1; g_nx_drain_ra = ra; }
    }
#endif
}

volatile uint32_t g_nx_qwait_us = 0, g_nx_qwait_max = 0;   // core0 waiting for queue room / a slot
volatile uint32_t g_nx_autoskip = 0;                        // frames not drawn: the renderer was behind (EndFrame)
static constexpr uint32_t NX_AUTOSKIP_US = 1000;
static void __not_in_flash_func(nxWaitUntil)(uint32_t w1) {
    if ((int32_t)(nx_r - w1) >= 0) return;
    const uint64_t t0 = time_us_64();
    uint64_t tw = t0;
    while ((int32_t)(nx_r - w1) < 0) {
        if (s_c0Blk && NEXTVID::c0Help()) { tw = time_us_64(); continue; }
        if (time_us_64() - tw > 100000) { nxQueueDead("stuck"); return; }
        tight_loop_contents();
    }
    const uint32_t d = (uint32_t)(time_us_64() - t0);
    g_nx_qwait_us = g_nx_qwait_us + d;
    if (d > g_nx_qwait_max) g_nx_qwait_max = d;
}

// Guest RAM a posted line reads, as (16K bank, offset range) tests: the
// tilemap row and tile definitions, the ULA / LoRes row, Layer 2 (whole
// banks). Conservative where the exact bytes would cost more than they save.
static bool nxRangeHit(uint8_t b, uint32_t a0, uint32_t a1, uint8_t bank, uint16_t o0, uint16_t o1) {
    return b == bank && a0 <= o1 && a1 >= o0;
}
// what a posted line reads in [o0, o1]: 0 nothing, 1 Layer 2, 2 tilemap map row,
// 3 tile definitions, 4 ULA / LoRes (the NEXT dmaw trace splits the waits by it)
static int nxLineReads(const NextLineState& st, int row, int paperTop, uint8_t bank, uint16_t o0, uint16_t o1,
                       bool withMap = true, bool noDefs = false) {
    // Layer 2 (NR 0x12, 3 banks, 5 in 320/640 modes)
    if ((st.p123B & 0x02) || (st.r69 & 0x80)) {
        const uint8_t l2 = st.r12 & 0x7F, n = (st.r70 & 0x30) ? 5 : 3;
        if (bank >= l2 && bank < l2 + n) return 1;
    }
    // tilemap: this line's map row + the definitions
    if (st.r6B & 0x80) {
        const int ty = row - (paperTop - 32);
        if (ty >= 0 && ty < 256) {
            const uint8_t mb = (st.r6E & 0x80) ? 7 : 5, db = (st.r6F & 0x80) ? 7 : 5;
            const int rowBytes = ((st.r6B & 0x40) ? 80 : 40) * ((st.r6B & 0x20) ? 1 : 2);
            const uint8_t sy = (uint8_t)(ty + st.r31);
            const uint32_t m0 = ((uint32_t)(st.r6E & 0x3F) << 8) + (uint32_t)(sy >> 3) * rowBytes;
            if (!withMap) {}
            else if (m0 + rowBytes > 0x4000) { if (mb == bank) return 2; }   // wraps: whole bank
            else if (nxRangeHit(mb, m0, m0 + rowBytes - 1, bank, o0, o1)) return 2;
            const uint32_t d0 = (uint32_t)(st.r6F & 0x3F) << 8;
            const uint32_t dl = (st.r6B & 0x08) ? 0x800 : (st.r6B & 0x02) ? 0x4000 : 0x2000;   // text / 512 / 256 tiles
            if (noDefs) {}
            else if (d0 + dl > 0x4000) { if (db == bank) return 3; }
            else if (nxRangeHit(db, d0, d0 + dl - 1, bank, o0, o1)) return 3;
        }
    }
    // ULA / LoRes: paper rows only, unless the ULA is off
    const int y = row - paperTop;
    if (y >= 0 && y < 192 && !(st.r68 & 0x80)) {
        const uint8_t ub = (st.grmem == MemESP::ram[7].direct()) ? 7 : 5;
        if (ub != bank) return 0;
        if (st.r15 & 0x80) {
            const uint8_t ly = (uint8_t)((y + st.r33) % 192);
            if (st.r6A & 0x20) {                       // Radastan: 64 bytes a row pair
                const uint32_t a = (((st.r69 ^ (st.r6A >> 4)) & 1) ? 0x2000 : 0) + (uint32_t)(ly >> 1) * 64;
                return nxRangeHit(ub, a, a + 63, bank, o0, o1) ? 4 : 0;
            }
            const uint8_t ry = ly >> 1;
            const uint32_t a = ry < 48 ? (uint32_t)ry * 128 : 0x2000 + (uint32_t)(ry - 48) * 128;
            return nxRangeHit(ub, a, a + 127, bank, o0, o1) ? 4 : 0;
        }
        if (nxRangeHit(ub, 0, 0x1AFF, bank, o0, o1)) return 4;
        if ((st.r69 & 0x03) || st.hires == 1) return nxRangeHit(ub, 0x2000, 0x3AFF, bank, o0, o1) ? 4 : 0;
    }
    return 0;
}

// ── Tilemap map-row shadows (copy-on-write for the batched DMA) ──────────────
// TX-1696 DMAs map rows (bank 5 0x1A00-0x1CFF) right behind the raster, ~15
// times a frame; the posted lines still to be rendered read the OLD row. Waiting
// for them (c1DrainRange) cost core0 ~9 ms a frame at 576p (hw 2026-10-13). So
// the old row is copied here before the write instead: a line whose job index
// is <= `until` reads the copy (the oldest copy that covers it), later lines
// read guest RAM. An entry is free once nx_r has passed `until`. Writer core0,
// readers both renderers (renderJob only: nx_busy set). Publication order:
// live = 0, fields + data, live = 1; a reader reads key/until, then live.
// Headers and row data apart: the renderer scans every header per tilemap line,
// and 32 of them 172 B apart in PSRAM were 32 XIP line fills a line; the data
// (160 B a row, PSRAM) is read once per hit. 128 entries: Atic Atac Remake
// rewrites 3-4 map rows per DMA, ~36 DMAs a frame, and the copies live until
// the last queued line that reads them is rendered — 32 ran out and the DMA
// then waited for core1 (158 waits/s, 1.1 ms of core0 a frame, hw 2026-10-19).
struct NxMapShadow {           // 8 B: 192 of them in the hot SRAM block
    volatile uint32_t until;      // newest job index that reads this copy
    volatile uint16_t key;        // nxMapKey: bank 7 << 15 | offset in the bank (< 0x4000)
    volatile uint8_t  live;
    uint8_t  n;                   // row bytes (<= 160)
};
static_assert(sizeof(NxMapShadow) == 8, "map shadow header packs to 8 bytes");
static inline uint16_t nxMapKey(uint8_t bank, uint32_t off) {
    return (uint16_t)(((uint32_t)(bank & 2) << 14) | (off & 0x3FFF));   // bank 5 -> 0, 7 -> 0x8000
}
static constexpr int NX_MAP_SH = 192;
static constexpr int NX_MAP_SH_ROW = 160;
// Key bit 14 (offsets are < 0x4000) marks a POST-TIME copy (nxMapSnapPost): never
// matched by the per-key scan, reached only through nx_mapOf[job].
static constexpr uint16_t NX_MAP_POST = 0x4000;
static NxMapShadow* s_mapSh = nullptr;            // [NX_MAP_SH] headers (the hot SRAM block, Reset)
static uint8_t (*s_mapShData)[NX_MAP_SH_ROW] = nullptr;   // [NX_MAP_SH] row copies (queue heap block)
static volatile uint8_t s_mapShLive = 0;          // any entry possibly live (renderer fast test)
static volatile int     s_mapShTop = 0;           // entries ever used (Take fills from the bottom): scan bound

// the tilemap map row a posted line reads: false = none (tilemap off / out of range)
static bool nxLineMapRow(const NextLineState& st, int row, int paperTop, uint8_t& bank, uint32_t& m0, int& rowBytes) {
    if (!(st.r6B & 0x80)) return false;
    const int ty = row - (paperTop - 32);
    if (ty < 0 || ty >= 256) return false;
    bank = (st.r6E & 0x80) ? 7 : 5;
    rowBytes = ((st.r6B & 0x40) ? 80 : 40) * ((st.r6B & 0x20) ? 1 : 2);
    const uint8_t sy = (uint8_t)(ty + st.r31);
    m0 = ((uint32_t)(st.r6E & 0x3F) << 8) + (uint32_t)(sy >> 3) * rowBytes;
    return m0 + rowBytes <= 0x4000;
}

// renderer: the copy of this map row the current job must read, or `live`
__attribute__((noinline)) static const uint8_t* nxMapRowFor(uint8_t bank, uint32_t off, const uint8_t* live) {
    const unsigned core = *(const uint32_t*)SIO_BASE & 1;
    if (!nx_busy[core] || !s_mapShData) return live;
    const uint32_t job = nx_jobAt[core];
    const uint16_t key = nxMapKey(bank, off);
    if (nx_mapOf) {                       // the row as it was at this line's raster time
        const uint8_t pk = nx_mapOf[job % NEXT_C1_RING];
        if (pk < NX_MAP_SH && s_mapSh[pk].live && s_mapSh[pk].key == (uint16_t)(key | NX_MAP_POST))
            return s_mapShData[pk];
    }
    const NxMapShadow* best = nullptr;
    uint32_t bestUntil = 0;
    const int top = s_mapShTop;
    for (int k = 0; k < top; k++) {
        const NxMapShadow& e = s_mapSh[k];
        const uint32_t u = e.until, kk = e.key;
        __dmb();
        if (!e.live || kk != key || (int32_t)(u - job) < 0) continue;
        if (!best || (int32_t)(u - bestUntil) < 0) { best = &e; bestUntil = u; }
    }
    return best ? s_mapShData[best - s_mapSh] : live;
}

// core0: the copy of map row (bank, off) posted job `job` will read, or `live`
static const uint8_t* nxMapRowForJob(uint8_t bank, uint32_t off, const uint8_t* live, uint32_t job) {
    if (!s_mapShData || !s_mapShLive) return live;
    const uint16_t key = nxMapKey(bank, off);
    if (nx_mapOf) {
        const uint8_t pk = nx_mapOf[job % NEXT_C1_RING];
        if (pk < NX_MAP_SH && s_mapSh[pk].live && s_mapSh[pk].key == (uint16_t)(key | NX_MAP_POST))
            return s_mapShData[pk];
    }
    const NxMapShadow* best = nullptr;
    uint32_t bestUntil = 0;
    const int top = s_mapShTop;
    for (int k = 0; k < top; k++) {
        const NxMapShadow& e = s_mapSh[k];
        if (!e.live || e.key != key || (int32_t)(e.until - job) < 0) continue;
        if (!best || (int32_t)(e.until - bestUntil) < 0) { best = &e; bestUntil = e.until; }
    }
    return best ? s_mapShData[best - s_mapSh] : live;
}

// core0: does posted line (st, row, job) draw a tile whose definition lies in
// [o0, o1] of `bank`? Every tilemap line "reads" the whole definition area, so
// the coarse test makes a DMA into ONE animated tile wait for the whole queue
// (TX-1696: 20-50 such waits a second at 576p, hw 2026-10-13). Conservative:
// any doubt (wrapping map row, text attribute modes) answers true.
static bool nxLineUsesTiles(const NextLineState& st, int row, int paperTop, uint32_t job,
                            uint8_t bank, uint16_t o0, uint16_t o1) {
    uint8_t mb; uint32_t m0; int n;
    if (!nxLineMapRow(st, row, paperTop, mb, m0, n)) return true;
    const uint8_t db = (st.r6F & 0x80) ? 7 : 5;
    if (db != bank) return false;
    const bool text = st.r6B & 0x08, noAttr = st.r6B & 0x20, m512 = st.r6B & 0x02;
    const uint32_t d0 = (uint32_t)(st.r6F & 0x3F) << 8;
    const uint32_t tsz = text ? 8 : 32;
    if (o1 < d0) return false;
    const uint32_t lo = (o0 > d0 ? o0 - d0 : 0) / tsz, hi = (uint32_t)(o1 - d0) / tsz;   // tile numbers written
    const uint8_t* live = MemESP::ram[mb].direct() + m0;
    const uint8_t* e = nxMapRowForJob(mb, m0, live, job);
    const int es = noAttr ? 1 : 2, cnt = n / es;
    for (int i = 0; i < cnt; i++) {
        uint32_t t = e[i * es];
        if (m512 && !noAttr) t |= (uint32_t)(e[i * es + 1] & 1) << 8;
        else if (m512) return true;          // 512 tiles without attributes: bit 8 from NR 0x6C
        if (t >= lo && t <= hi) return true;
    }
    return false;
}

// core0: copy map row (bank, m0) for the posted jobs up to `until`. false = no
// free entry (the caller waits instead).
static int nxMapShadowAlloc(uint8_t bank, uint32_t m0, int n, uint32_t until, uint16_t key) {
    int freeK = -1;
    for (int k = 0; k < NX_MAP_SH; k++) {
        NxMapShadow& e = s_mapSh[k];
        if (e.live && (int32_t)(nx_r - e.until) > 0) e.live = 0;       // all its lines rendered
        if (e.live && e.key == key && e.until == until) return k;      // already copied for them
        if (!e.live && freeK < 0) freeK = k;
    }
    if (freeK < 0) return -1;
    NxMapShadow& e = s_mapSh[freeK];
    if (freeK >= s_mapShTop) s_mapShTop = freeK + 1;
    e.live = 0;
    __dmb();
    e.key = key; e.until = until; e.n = (uint8_t)n;
    memcpy(s_mapShData[freeK], MemESP::ram[bank].direct() + m0, n);
    __dmb();
    e.live = 1;
    s_mapShLive = 1;
    return freeK;
}
static bool nxMapShadowTake(uint8_t bank, uint32_t m0, int n, uint32_t until) {
    return nxMapShadowAlloc(bank, m0, n, until, nxMapKey(bank, m0)) >= 0;
}

// core0, posting job `job`: give it the map row it reads AS IT IS NOW (its raster
// time). core1 renders a line later than that, and a guest that clears and
// redraws the whole tilemap once a frame (International Karate+'s menu: NR 0x1F
// wait for line 208, LDIR over #4000-#49FF, then the text) had the rows read
// after the clear — only the title, drawn first, survived (hw 2026-10-20). The
// copy taken for the previous line of the same tile row is reused while the
// row is unchanged (its `until` bumped), so a tile row costs one copy a frame.
static int      s_mapPostK = -1;
static uint32_t s_mapPostUntil = 0;
__attribute__((noinline)) static void nxMapSnapPost(const NextLineState& st, int row, int paperTop, uint32_t job) {
    uint8_t& of = nx_mapOf[job % NEXT_C1_RING];
    of = 0xFF;
    uint8_t bank; uint32_t m0; int n;
    if (!nxLineMapRow(st, row, paperTop, bank, m0, n)) return;
    const uint16_t key = (uint16_t)(nxMapKey(bank, m0) | NX_MAP_POST);
    const uint8_t* live = MemESP::ram[bank].direct() + m0;
    if (s_mapPostK >= 0) {
        NxMapShadow& e = s_mapSh[s_mapPostK];
        if (e.live && e.key == key && e.until == s_mapPostUntil && e.n == n
            && !memcmp(s_mapShData[s_mapPostK], live, n)) {
            e.until = job;                    // core0 only; a reader of `until` is fine either way
            s_mapPostUntil = job;
            of = (uint8_t)s_mapPostK;
            return;
        }
    }
    const int k = nxMapShadowAlloc(bank, m0, n, job, key);
    s_mapPostK = k;
    s_mapPostUntil = job;
    if (k >= 0) of = (uint8_t)k;              // none free: the line reads guest RAM, as before
}

// core0: the DMA is about to write [o0, o1] of `bank`. If the only posted lines
// that read it read it as a tilemap MAP ROW, copy those rows and return true
// (no wait). Anything else (Layer 2, tile definitions, ULA, no free entry):
// false, the caller waits as before.
static bool nxMapShadowCover(uint8_t bank, uint16_t o0, uint16_t o1, int paperTop) {
    if (!s_mapShData) return false;
    const uint32_t r = nx_r;
    // pass 1: is every hit a map-row hit? (newest job first, as c1DrainRange)
    struct { uint32_t m0, until; int n; } rows[16];
    int nr = 0;
    // Per line STATE (a few per queue), once per call: can a row of this state
    // read [o0, o1] as anything but its map row? 0 = no (Layer 2 elsewhere,
    // definitions out of range, ULA off or in another bank), 1 = maybe (the
    // full per-row test). The queue holds up to 300 rows and this ran the full
    // test on each for every DMA block (Atic Atac Remake: ~36 blocks a frame,
    // ~25 us each, hw 2026-10-19).
    uint8_t sv[NEXT_C1_STATES];
    for (int k = 0; k < NEXT_C1_STATES; k++) sv[k] = 0xFF;
    for (uint32_t i = nx_w; i != r; ) {
        i--;
        if ((int32_t)(nx_r - i) > 0) break;
        const uint32_t j = nx_ring[i % NEXT_C1_RING];
        const NextLineState& st = nx_st[j >> 16];
        const int row = (int)(j & 0xFFFF);
        uint8_t& v = sv[j >> 16];
        if (v == 0xFF) {
            v = 0;
            if ((st.p123B & 0x02) || (st.r69 & 0x80)) {
                const uint8_t l2 = st.r12 & 0x7F, nb = (st.r70 & 0x30) ? 5 : 3;
                if (bank >= l2 && bank < l2 + nb) v = 1;
            }
            if (st.r6B & 0x80) {
                const uint8_t db = (st.r6F & 0x80) ? 7 : 5;
                const uint32_t d0 = (uint32_t)(st.r6F & 0x3F) << 8;
                const uint32_t dl = (st.r6B & 0x08) ? 0x800 : (st.r6B & 0x02) ? 0x4000 : 0x2000;
                const bool defsHit = (d0 + dl > 0x4000) ? (db == bank) : nxRangeHit(db, d0, d0 + dl - 1, bank, o0, o1);
                if (defsHit && !(o1 < d0)) v = 1;   // o1 < d0: nxLineUsesTiles says no for every row
            }
            if (!(st.r68 & 0x80)) {
                const uint8_t ub = (st.grmem == MemESP::ram[7].direct()) ? 7 : 5;
                if (ub == bank) v = 1;
            }
        }
        if (v) {
            // 512-tile mode makes the definitions "the whole bank" (nxLineReads), so
            // every line read a map-row DMA as a definitions write and the copy was
            // taken only with an empty queue (Atic Atac Remake: 340 waits/s, 2.5 ms
            // of core0 a frame, hw 2026-10-19) — ask which tiles the line draws, as
            // c1DrainRange does (o1 < d0 answers at once).
            int why = nxLineReads(st, row, paperTop, bank, o0, o1, false);
            if (why == 3 && !nxLineUsesTiles(st, row, paperTop, i, bank, o0, o1))
                why = nxLineReads(st, row, paperTop, bank, o0, o1, false, true);
            if (why) return false;
        }
        uint8_t mb; uint32_t m0; int n;
        if (!nxLineMapRow(st, row, paperTop, mb, m0, n)) continue;
        if (mb != bank || !nxRangeHit(mb, m0, m0 + n - 1, bank, o0, o1)) continue;
        int k = 0;
        for (; k < nr; k++) if (rows[k].m0 == m0) break;
        if (k == nr) {
            if (nr == 16) return false;
            rows[nr++] = { m0, i, n };                // i: the newest job reading it
        }
    }
    for (int k = 0; k < nr; k++)
        if (!nxMapShadowTake(bank, rows[k].m0, rows[k].n, rows[k].until)) return false;
    return true;
}

// core0, before guest RAM [o0, o1] of 16K bank `bank` is overwritten faster than
// core1 can follow (the batched DMA): wait only for the posted lines that read it
// — the newest such job — instead of the whole queue (TX-1696 rewrites ~24
// tilemap rows a frame by DMA; a full drain each cost 2.5-3.4 ms, hw 2026-10-09).
uint8_t NEXTVID::c1OldestL2Bank() {
    if (!g_next_c1_live) return 0xFF;
    const uint32_t r = nx_r;
    if (nx_w == r) return 0xFF;
    const uint32_t j = nx_ring[r % NEXT_C1_RING];
    const NextLineState& st = nx_st[j >> 16];
    if (!((st.p123B & 0x02) || (st.r69 & 0x80))) return 0xFF;   // Layer 2 off on that line
    return (uint8_t)(st.r12 & 0x7F);
}

void NEXTVID::c1DrainRange(uint8_t bank, uint16_t o0, uint16_t o1) {
    if (!g_next_c1_live) return;
    if (nxMapShadowCover(bank, o0, o1, paperTopRow)) {
#if NEXT_TRACE
        g_nx_dmaw_sh = g_nx_dmaw_sh + 1;
#endif
        return;
    }
    const uint32_t r = nx_r;
    for (uint32_t i = nx_w; i != r; ) {
        i--;
        if ((int32_t)(nx_r - i) > 0) return;   // core1 already past it
        const uint32_t j = nx_ring[i % NEXT_C1_RING];
        int why = nxLineReads(nx_st[j >> 16], (int)(j & 0xFFFF), paperTopRow, bank, o0, o1);
        if (why == 3 && !nxLineUsesTiles(nx_st[j >> 16], (int)(j & 0xFFFF), paperTopRow, i, bank, o0, o1))
            why = nxLineReads(nx_st[j >> 16], (int)(j & 0xFFFF), paperTopRow, bank, o0, o1, true, true);
        if (why) {
#if NEXT_TRACE
            const uint32_t t0 = timer_hw->timerawl;
            const uint32_t lines = i + 1 - nx_r;
#endif
            nxWaitUntil(i + 1);
#if NEXT_TRACE
            const uint32_t dt = timer_hw->timerawl - t0;
            g_nx_drain_n = g_nx_drain_n + 1;
            g_nx_drain_src[4] = g_nx_drain_src[4] + 1;
            g_nx_drain_us = g_nx_drain_us + dt;
            g_nx_dmaw[(why - 1) & 3][0]++;                    // 0 L2, 1 map, 2 defs, 3 ULA/LoRes
            g_nx_dmaw[(why - 1) & 3][1] += dt;
            g_nx_dmaw[(why - 1) & 3][2] += lines;
            const uint32_t key = ((uint32_t)bank << 16) | (o0 & 0xFF00) | (o1 >> 8);
            int k = 0;
            for (; k < 4; k++) if (g_nx_dmaw_key[k][0] == key || g_nx_dmaw_key[k][0] == 0) break;
            if (k < 4) { g_nx_dmaw_key[k][0] = key; g_nx_dmaw_key[k][1]++; }
#endif
            return;
        }
    }
}
// Palette index 16 is never produced (buildRemap skips it: 0..15 ZX colours,
// 17..183 the cube), so it marks "Timex hi-res pixel here" in lineBuf.
static constexpr uint8_t HIRES_MARK = 16;
static_assert(HIRES_MARK == 0x10, "RenderLine's word test spells it 0x10101010");
// A hi-res lores pixel with ONE opaque half: the ULA wins the mix (this mark),
// the other half shows what the mix gives with the ULA transparent there
// (nxHiresUnder). hiresRow holds bit 7 = the opaque half is the LEFT one,
// bits 3:0 its ZX colour. 0xF0 is a sync slot in the fb palette: never an index.
static constexpr uint8_t HIRES_MARK2 = 0xF0;
static_assert(!NEXTVID::isSlot(HIRES_MARK2) && HIRES_MARK2 > 16, "HIRES_MARK2 must be a free index");
#if NEXT_TRACE
uint32_t g_nextvid_frames = 0; // completed frames with composition enabled
#endif

// ===== palette machinery =====


// Nearest usable CLUT index for an RGB333 colour (remap[512]). Usable candidates:
//  - isSlot(): the colour lattice (NextVideo.h slotRgb), programmed by
//    VIDEO::Reset / applyPalette. NEVER 184-199 / 216-239 on HDMI: those hold
//    audio data-island / guard-band TMDS symbols (a pixel with such a value
//    emits CONTROL symbols mid-line — hw: yellow vertical stripes); 240+ is
//    sync; 152-167 is the UI block the menu repaints.
//  - 0..15: the ZX palette entries — exact black/white/primaries. Black is
//    deliberately not in the lattice: it is ZX colour 0.
// Distances are computed in 8-bit RGB with rough luma weights.
void NEXTVID::buildRemap() {
    c1Drain();
    static const uint8_t zxlvl[2] = { 205, 255 };
    for (int i = 0; i < 512; i++) {
        int tr = ((i >> 6) & 7) * 255 / 7;
        int tg = ((i >> 3) & 7) * 255 / 7;
        int tb = (i & 7) * 255 / 7;
        int best = 0;
        // max d = 255^2 * 10 < 2^32 — plain uint32 arithmetic (a 64-bit
        // sentinel via `long` was UB on 32-bit ARM: bestd == 0, nothing ever
        // beat it and the whole remap collapsed to index 17 — hw: solid
        // crimson screen whatever the OS drew)
        uint32_t bestd = 0xFFFFFFFFu;
        for (int c = 0; c < 16; c++) {           // ZX colours
            int l = zxlvl[(c >> 3) & 1];
            int cr = (c & 2) ? l : 0, cg = (c & 4) ? l : 0, cb = (c & 1) ? l : 0;
            int dr = tr - cr, dg = tg - cg, db = tb - cb;
            uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 6 + db * db);
            if (d < bestd) { bestd = d; best = c; }
        }
        for (int j = 17; j < 216; j++) {          // colour lattice
            if (!isSlot(j)) continue;
            const uint32_t rgb = slotRgb(j);
            int cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;
            int dr = tr - cr, dg = tg - cg, db = tb - cb;
            uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 6 + db * db);
            if (d < bestd) { bestd = d; best = j; }
        }
        remap[i] = (uint8_t)best;
    }
}

// Timex hi-res runs the driver in packed-pair mode: every framebuffer byte is
// a (left, right) pair of the 16 ZX colours. The ULA's 512 pixels are exact
// (VIDEO's timex_hr_lut); every other layer's palette index — 0..15 the ZX
// colours themselves, the rest the colour lattice (buildRemap) — becomes the solid
// pair of its nearest ZX colour, the same limit every pair mode has.
void NEXTVID::rebuildPairMap() {
    if (!pairMap) return;
    c1Drain();
    static const uint8_t zxlvl[2] = { 205, 255 };
    for (int i = 0; i < 256; i++) {
        int tr, tg, tb;
        if (i < 16) {
            const int l = zxlvl[(i >> 3) & 1];
            tr = (i & 2) ? l : 0; tg = (i & 4) ? l : 0; tb = (i & 1) ? l : 0;
        } else if (isSlot(i)) {
            const uint32_t rgb = slotRgb(i);
            tr = (rgb >> 16) & 0xFF; tg = (rgb >> 8) & 0xFF; tb = rgb & 0xFF;
        } else {
            tr = tg = tb = 0;                     // never produced
        }
        int best = 0; uint32_t bestd = 0xFFFFFFFFu;
        for (int c = 0; c < 16; c++) {
            const int l = zxlvl[(c >> 3) & 1];
            const int dr = tr - ((c & 2) ? l : 0), dg = tg - ((c & 4) ? l : 0), db = tb - ((c & 1) ? l : 0);
            const uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 6 + db * db);
            if (d < bestd) { bestd = d; best = c; }
        }
        pairMap[i] = VIDEO::profi_pair_lookup[best][best];
    }
}

// ── 640-wide pair frames: core0 side (flash) ─────────────────────────────────
// RGB of a palette index as the renderer emits it: 0..15 the ZX colours, the
// rest the colour lattice.
static uint32_t nxIdxRgb(int i) {
    if (i < 16) {
        const int l = (i & 8) ? 255 : 205;
        return ((i & 2) ? (uint32_t)l << 16 : 0) | ((i & 4) ? (uint32_t)l << 8 : 0) | ((i & 1) ? (uint32_t)l : 0);
    }
    return NEXTVID::isSlot(i) ? NEXTVID::slotRgb(i) : 0;
}

static uint32_t nxRgbDist(uint32_t a, uint32_t b) {
    const int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
    const int dg = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF);
    const int db = (int)(a & 0xFF) - (int)(b & 0xFF);
    return (uint32_t)(dr * dr * 3 + dg * dg * 6 + db * db);
}

// p80Cur -> the 16 RGB colours, map16 (every palette index -> its nearest pair
// colour) and solid (-> the pair slot (c, c)). Callers drain core1 first.
static void p80BuildTables(uint32_t pal[16]) {
    for (int k = 0; k < 16; k++) pal[k] = p80Cur[k] == 0xFF ? 0 : nxIdxRgb(p80Cur[k]);
    for (int i = 0; i < 256; i++) {
        int best = 0;
        uint32_t bestd = 0xFFFFFFFFu;
        const uint32_t c = nxIdxRgb(i);
        for (int k = 0; k < 16; k++) {
            if (p80Cur[k] == 0xFF) continue;
            if (p80Cur[k] == i) { best = k; break; }
            const uint32_t d = nxRgbDist(c, pal[k]);
            if (d < bestd) { bestd = d; best = k; }
        }
        p80Map16[i] = (uint8_t)best;
        p80Solid[i] = VIDEO::profi_pair_lookup[best][best];
    }
}

// The frame's 16 colours: the most shown palette indices, counted over
// P80_WINDOW frames (core1 renders behind core0, so one frame's count is a
// different slice of the picture every time — hw 2026-10-11: the 16 changed
// almost every frame and the whole screen flickered).
// The pair table folds (ink 0..5, paper 8) into (ink, 0) and, with HDMI audio
// on, (bright ink, other bright paper) into paper - 8 (init_profi_pair_lookup's
// slot budget). So the first pick goes out as 8 families, family f in pair
// colours f and f + 8: the 8 most shown colours at 0..7 (exact among
// themselves), the rest as partners of the nearest head at f + 8, so a
// folded pair shows a near colour instead of an arbitrary one. Up to 8
// colours on screen never fold at all — a two-colour text screen must not.
// Afterwards a colour stays in its slot for as long as it is picked (a current
// colour counts double), and a newcomer takes the slot of an evicted one whose
// family partner is nearest to it — the rest of the screen does not change.
static constexpr int P80_WINDOW = 8;
static bool s_p80Full = true;          // next pick lays the families out afresh
static bool s_tall = false;            // the frame shows a 320x256 window (computeGeometry)
static int  s_p80Frames = 0;

static bool p80Choose() {
    const bool full = s_p80Full;
    if (++s_p80Frames < (full ? 2 : P80_WINDOW)) return false;
    s_p80Frames = 0;
    // read in place (core1 keeps counting meanwhile — harmless), zeroed below
    const uint32_t* const cnt = p80Hist;
    uint32_t total = 0;
    for (int i = 0; i < 256; i++) total += cnt[i];
    if (!total) return false;
    bool cur[256] = {};
    if (!full) for (int k = 0; k < 16; k++) if (p80Cur[k] != 0xFF) cur[p80Cur[k]] = true;
    uint8_t pick[16]; int n = 0;
    bool taken[256] = {};
    for (; n < 16; n++) {
        int best = -1; uint32_t bs = 0;
        for (int i = 0; i < 256; i++) {
            if (taken[i] || !cnt[i] || !(i < 16 || NEXTVID::isSlot(i))) continue;
            const uint32_t sc = cur[i] ? cnt[i] * 2 : cnt[i];
            if (sc > bs) { bs = sc; best = i; }
        }
        if (best < 0) break;
        taken[best] = true; pick[n] = (uint8_t)best;
    }
    int distinct = 0; for (int i = 0; i < 256; i++) distinct += cnt[i] != 0;
    for (int i = 0; i < 256; i++) p80Hist[i] = 0;
    if (!n) return false;
    if (!full) {
        // newcomers into the slots of evicted colours; slots of colours no
        // longer shown at all stay as they are (nothing maps to them)
        bool keep[16] = {};
        for (int k = 0; k < 16; k++) keep[k] = p80Cur[k] != 0xFF && taken[p80Cur[k]];
        bool changed = false;
        for (int j = 0; j < n; j++) {
            if (cur[pick[j]]) continue;
            const uint32_t c = nxIdxRgb(pick[j]);
            int best = -1; uint32_t bd = 0xFFFFFFFFu;
            for (int k = 0; k < 16; k++) {
                if (keep[k]) continue;
                const uint8_t pt = p80Cur[k ^ 8];
                const uint32_t d = pt == 0xFF ? 0x7FFFFFFFu : nxRgbDist(c, nxIdxRgb(pt));
                if (best < 0 || d < bd) { bd = d; best = k; }
            }
            if (best < 0) break;
            p80Cur[best] = pick[j];
            keep[best] = true;
            changed = true;
        }
        return changed;
    }
    // families: the 8 most shown colours are the family heads (pair colours
    // 0..7 — every combination among them has its own slot, so up to 8
    // colours on screen are EXACT); only the picks past the 8th become
    // partners (f + 8), each of the nearest head without one. A head without
    // a partner repeats itself in f + 8.
    // The first cut paired every head with its closest colour, most shown
    // first: on a two-colour screen that put the TEXT colour at 8, the
    // partner of the paper at 0, and the (ink 0..5, paper 8) -> (ink, 0) fold
    // erased every "paper left, ink right" pair — half the ink pixels of every
    // glyph (ZXZVM's 80-column text, hw 2026-10-19).
    uint8_t out[16]; for (int k = 0; k < 16; k++) out[k] = 0xFF;
    const int heads = n < 8 ? n : 8;
    for (int f = 0; f < heads; f++) { out[f] = pick[f]; out[f + 8] = pick[f]; }
    bool hasPartner[8] = {};
    for (int j = 8; j < n; j++) {
        const uint32_t c = nxIdxRgb(pick[j]);
        int best = -1; uint32_t bd = 0xFFFFFFFFu;
        for (int f = 0; f < heads; f++) {
            if (hasPartner[f]) continue;
            const uint32_t d = nxRgbDist(c, nxIdxRgb(out[f]));
            if (d < bd) { bd = d; best = f; }
        }
        if (best < 0) break;
        hasPartner[best] = true;
        out[best + 8] = pick[j];
    }
    for (int k = 0; k < 16; k++) p80Cur[k] = out[k];
    s_p80Full = false;
    Debug::log("[NXV] 640: %d colours on screen, 16 picked", distinct);
    return true;
}

// The framebuffer through a byte table, in place. Mode / palette switches of
// the pair frame re-encode what is already on screen instead of leaving it to
// the next frame's rows: until a row is re-rendered the beam shows it through
// the NEW driver tables, and old bytes there are another picture — a frame of
// wrong colours (the whole desktop red after the first 16-colour pick) or of
// garbage at every 320 <-> 640 switch (hw 2026-10-11, NextBench). core0, with
// the queue drained. ~0.3 ms for a 320x240 fb; only on a change. Flash.
__attribute__((noinline)) static void p80RemapFb(const uint8_t* tab) {
    if (!VIDEO::vga.frameBuffer) return;
    const int xres = (int)VIDEO::vga.xres;
    for (int y = 0; y < (int)VIDEO::vga.yres; y++) {
        uint8_t* r = VIDEO::vga.frameBuffer[y];
        if (!r) continue;
        for (int x = 0; x < xres; x++) r[x] = tab[r[x]];
    }
}

// Pair slots of the old 16 colours -> pair slots of the new ones (p80Map16 is
// already rebuilt): each half to the new colour nearest to what it showed.
static void p80RemapPal(const uint8_t old[16]) {
    uint8_t rev[256], tab[256];
    VIDEO::getPairSlotReverse(rev);
    for (int s = 0; s < 256; s++) {
        const uint8_t ia = old[rev[s] >> 4], ib = old[rev[s] & 15];
        const uint8_t na = ia == 0xFF ? 0 : p80Map16[ia], nb = ib == 0xFF ? 0 : p80Map16[ib];
        tab[s] = VIDEO::profi_pair_lookup[na][nb];
    }
    p80RemapFb(tab);
}

void NEXTVID::pairForceOff() {
    if (!p80Live) return;
    c1Drain();
    {
        // back to palette indices: each byte shows its left pixel's colour
        uint8_t rev[256], tab[256];
        VIDEO::getPairSlotReverse(rev);
        for (int s = 0; s < 256; s++) { const uint8_t i = p80Cur[rev[s] >> 4]; tab[s] = i == 0xFF ? 0 : i; }
        p80RemapFb(tab);
    }
    p80Live = false;
    VIDEO::nextPairOff();
    computeGeometry();
}

void NEXTVID::pairPoll() {
    const uint8_t* r = NextReg::reg;
    // core0's own row scratch in EVERY mode now, not only the 640 pair frame:
    // it renders queued rows in its frame-pacing idle as well (ESPectrum::loop)
    nxHelperAlloc();
    {
        // the frame shows a 256-line window: move the fb window with the
        // program's own screen change, at the frame boundary
        const bool l2 = (NextReg::port123B & 0x02) || (r[0x69] & 0x80);
        const bool tall = (r[0x6B] & 0x80) || (l2 && (r[0x70] & 0x30));
        if (tall != s_tall) {
            c1Drain();
            s_tall = tall;
            computeGeometry();
        }
    }
    // Layer 2 320x256 / 640x256 on: the column band (SRAM, ~2.5 KB, once)
    static bool s_bandTried = false;
    if (!l2Band && !s_bandTried && (r[0x70] & 0x30) && ((NextReg::port123B & 0x02) || (r[0x69] & 0x80))) {
        s_bandTried = true;
        uint8_t* b = nxSramBlock(NX_L2_BAND);
        c1Drain();
        l2Band = b;
        Debug::log("[NXV] Layer 2 column band %s", b ? "in SRAM" : "not allocated (no SRAM heap)");
    }
    // 80-column tilemap, or Layer 2 640x256x4bpp (NR 0x70 bit 5, modes 2/3; SymbOS ZNX)
    const bool l2Hi = ((NextReg::port123B & 0x02) || (r[0x69] & 0x80)) && (r[0x70] & 0x20);
    const bool want = ((r[0x6B] & 0xC0) == 0xC0 || l2Hi) && !VIDEO::timex_hires_live && !VIDEO::timex_hires_pending_on;
    static uint8_t s_on = 0, s_off = 0;
    static bool s_refused = false;
    if (!p80Live) {
        if (!want) { s_on = 0; s_refused = false; return; }
        if (s_refused || ++s_on < 2) return;
        if (!p80Alloc()) {
            s_refused = true;
            Debug::log("[NXV] 640: no SRAM for the pair tables (%u B), staying at 320", P80_BLK);
            return;
        }
        c1Drain();
        for (int k = 0; k < 16; k++) p80Cur[k] = (uint8_t)k;    // the ZX 16 until a frame is counted
        for (int i = 0; i < 256; i++) p80Hist[i] = 0;
        s_p80Full = true; s_p80Frames = 0;
        uint32_t pal[16];
        p80BuildTables(pal);                // needs the pair lookup: built by nextPairOn first
        if (!VIDEO::nextPairOn(pal)) { s_refused = true; return; }
        p80BuildTables(pal);                // ... now with this session's pair slots
        p80RemapFb(p80Solid);               // the 320 picture on screen as solid pairs
        p80Live = true;
        computeGeometry();                  // the window moves up (see computeGeometry)
        s_off = 0;
        return;
    }
    if (!want) {
        // Timex hi-res wants the driver: hand it over now, not 25 frames late
        if (VIDEO::timex_hires_pending_on || ++s_off >= 25) { pairForceOff(); s_on = 0; }
        return;
    }
    s_off = 0;
    nxHelperAlloc();                        // core0 renders rows too while it waits (c0Help)
    uint8_t old[16];
    for (int k = 0; k < 16; k++) old[k] = p80Cur[k];
    if (!p80Choose()) return;
    c1Drain();
    uint32_t pal[16];
    p80BuildTables(pal);
    p80RemapPal(old);                       // what is on screen, in the new 16 colours
    VIDEO::nextPairPalette(pal);
}

// Renderer (or core0 with the queue drained): apply the palette log up to `upto`.
// Per palette: the one 9-bit colour every entry holds (all 256 the same), or
// 0xFFFF; recomputed lazily (s_palMonoDirty, set by every rawPal[] writer —
// palApply, the ULANext reset, renderRowSegments' rollback). RenderL2Line
// skips a layer whose palette is all one transparent colour: NEXTEST enables
// Layer 2 with an all-black palette over NR 0x14 = 0 and runs its memory test
// through the banks — 192 PSRAM rows a frame (5 ms of the render) for no pixel.
static uint16_t s_palMono[8] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
static volatile uint8_t s_palMonoDirty[8];
// Bumped with every rawPal[] write: the Layer 2 320x256 row LUT (RenderL2Line)
// is rebuilt when its palette's generation moved.
static volatile uint16_t s_palGen[8];
static inline __attribute__((always_inline)) void nxPalDirty(uint8_t p) {
    s_palMonoDirty[p] = 1; s_palGen[p] = (uint16_t)(s_palGen[p] + 1);
}
IRAM_ATTR void NEXTVID::palApply(uint32_t upto) {
    uint32_t i = pal_app;
    // A line captured before core0 applied the log ahead (renderRowSegments,
    // rebuildLut, a full log) names an older position: nothing to do. `!=` here
    // ran ~4e9 iterations (hw 2026-10-09: !Copper.snx froze core0).
    if ((int32_t)(upto - i) <= 0) return;
    __dmb();
#if NEXT_TRACE
    g_nx_logapply_pal = g_nx_logapply_pal + (upto - i);
#endif
    for (; i != upto; i++) {
        const uint32_t e = palLog[i % PAL_LOG];
        const uint8_t p = (e >> 24) & 7, x = (uint8_t)(e >> 16);
        const uint16_t v = (uint16_t)e;
        rawPal[p][x] = v; nxPalDirty(p);
        lut[p][x] = remap[v & 0x1FF];
    }
    __dmb();
    pal_app = i;
}

#if NEXT_TRACE
volatile uint32_t g_nx_palw = 0;   // palette writes through nrHot (NEXT: pal/s)
#endif
// The palette NEXTREGs (0x40 / 0x41 / 0x44) without NextReg::write's flash
// switch: from flash each write cost ~160 XIP fetches and ~1 us, and TX-1696
// fills 256-entry palettes dozens of times a frame (12k writes = 12 ms, the
// frames of 26-35 ms; hw 2026-10-09). Same effect as palIndex / palValue8 / 9.
static inline uint16_t rgb332_to_333h(uint8_t v) {
    uint8_t b2 = v & 3;
    return ((v & 0xE0) << 1) | ((v & 0x1C) << 1) | (b2 << 1) | (b2 ? 1 : 0);
}
IRAM_ATTR bool NEXTVID::nrHot(uint8_t r, uint8_t v) {
    if (r == 0x40) {
        palIdx = v;
        pal9Second = false;
    } else if (r == 0x41) {
        setLutEntry((palCtrl >> 4) & 7, palIdx, rgb332_to_333h(v));
        pal9Second = false;
        if (!(palCtrl & 0x80)) palIdx++;
    } else if (r == 0x44) {
        if (!pal9Second) {
            pal9First = v;
            pal9Second = true;
        } else {
            uint16_t rgb = (rgb332_to_333h(pal9First) & ~1) | (v & 1);
            const uint8_t pal = (palCtrl >> 4) & 7;
            if (v & 0x80) { rgb |= 0x8000; palHasPrio[pal] = true; }
            setLutEntry(pal, palIdx, rgb);
            pal9Second = false;
            if (!(palCtrl & 0x80)) palIdx++;
        }
    } else {
        return false;
    }
    NextReg::reg[r] = v;
#if NEXT_TRACE
    g_nx_palw = g_nx_palw + 1;
#endif
    return true;
}

IRAM_ATTR void NEXTVID::setLutEntry(uint8_t pal, uint8_t idx, uint16_t rgb333) {
    if (s_pdRec && s_pdn < NX_PAL_DELTAS) {
        const int x = s_copXBase + (s_copT >> 2);
        // A write that lands before the first pixel its palette can colour
        // splits nothing: the ULA palette also paints the border (x 0), the
        // Layer 2 / sprite / tilemap ones start at the 320-wide area's left edge
        // (pal = NR 0x43 bits 6:4, xx0 = ULA). Such writes come before the
        // row's capture (hc 140), so the captured palette already has them.
        // TX-1696: 3 tilemap-palette writes at x 13..16 on 14 rows a frame —
        // at 576p the tilemap starts at x 20, and every one cost a full extra
        // pass of the row (4 renders a row; at 480p it starts at x 0, none).
        const int first = (pal & 3) ? xPaperOff - 32 : 0;
        if (x > first && x < (int)VIDEO::vga.xres)
            s_pd[s_pdn++] = { (uint16_t)(x < 0 ? 0 : x), pal, idx, cRaw[pal][idx], rgb333 };
    }
    cRaw[pal][idx] = rgb333;
    if (pal_w - pal_app >= PAL_LOG) { c1Drain(1); palApply(pal_w); }   // log full
    palLog[pal_w % PAL_LOG] = ((uint32_t)(pal & 7) << 24) | ((uint32_t)idx << 16) | rgb333;
    __dmb();
    pal_w = pal_w + 1;
}

void NEXTVID::rebuildLut(uint8_t pal) {
    c1Drain(2);
    palApply(pal_w);
    for (int i = 0; i < 256; i++)
        lut[pal][i] = remap[rawPal[pal][i] & 0x1FF];
}

static inline uint16_t rgb332_to_333(uint8_t v) {
    uint8_t b2 = v & 3;
    return ((v & 0xE0) << 1) | ((v & 0x1C) << 1) | (b2 << 1) | (b2 ? 1 : 0);
}

void NEXTVID::defaultPalettes() {
    c1Drain(2);
    palApply(pal_w);
    // Classic Spectrum colours in RGB333: level 5 normal, 7 bright
    for (int pal = 0; pal < 8; pal++) {
        palHasPrio[pal] = false;
        for (int i = 0; i < 256; i++)
            rawPal[pal][i] = rgb332_to_333((uint8_t)i); nxPalDirty(pal);
    }
    for (int pal = 0; pal < 8; pal += 4) { // both ULA palettes
        for (int c = 0; c < 16; c++) {
            uint8_t base = c & 7;
            uint8_t l = (c & 8) ? 7 : 5;
            uint16_t v = ((base & 2) ? (l << 6) : 0)   // R
                       | ((base & 4) ? (l << 3) : 0)   // G
                       | ((base & 1) ? l : 0);         // B
            rawPal[pal][c] = v; nxPalDirty(pal);   // ink
            rawPal[pal][16 + c] = v;   // paper (same colours)
        }
    }
    for (int pal = 0; pal < 8; pal++) rebuildLut(pal);
    memcpy(cRaw, rawPal, 8 * 256 * 2);
}

static inline uint8_t writePalSel_(uint8_t ctrl) { return (ctrl >> 4) & 7; }

int NEXTVID::nrRead(uint8_t r) {
    switch (r) {
        case 0x40: return palIdx;                                   // auto-incremented index
        case 0x61: return copIndex & 0xFF;                          // copper write address 7:0
        case 0x62: return (copCtrl & 0xC0) | ((copIndex >> 8) & 7); // mode & 000 & address 10:8
        default:   return -1;
    }
}

uint8_t NEXTVID::palRead8() {
    // nextreg 0x41 read: bits 8:1 of the current entry (RRRGGGBB)
    if (!cRaw) return 0;
    return (uint8_t)(cRaw[writePalSel_(palCtrl)][palIdx] >> 1);
}

uint8_t NEXTVID::palRead9() {
    // nextreg 0x44 read: second-byte view — bit7 = L2 priority, bit0 = B lsb
    if (!cRaw) return 0;
    uint16_t raw = cRaw[writePalSel_(palCtrl)][palIdx];
    return (uint8_t)(((raw & 0x8000) ? 0x80 : 0) | (raw & 1));
}

void NEXTVID::ulapWrite(uint8_t idx, uint8_t v) {
    // the nextreg 0xFF stream: GGGRRRBB -> RRRGGGBB, blue lsb = B1 | B0
    const uint8_t pal = (palCtrl & 0x40) ? 4 : 0;
    const uint8_t rgb = (uint8_t)(((v & 0x1C) << 3) | ((v & 0xE0) >> 3) | (v & 0x03));
    setLutEntry(pal, (uint8_t)(0xC0 | (idx & 0x3F)), rgb332_to_333(rgb));
}

uint8_t NEXTVID::ulapRead(uint8_t idx) {
    if (!cRaw) return 0;
    const uint16_t e = cRaw[(palCtrl & 0x40) ? 4 : 0][0xC0 | (idx & 0x3F)];
    // port_ff3b_dat = dat(5:3) & dat(8:6) & dat(2:1)  (GGG RRR BB)
    return (uint8_t)((((e >> 3) & 7) << 5) | (((e >> 6) & 7) << 2) | ((e >> 1) & 3));
}

void NEXTVID::palIndex(uint8_t v) {
    palIdx = v;
    pal9Second = false;
}

static inline uint8_t writePalSel(uint8_t ctrl) { return (ctrl >> 4) & 7; }

void NEXTVID::palValue8(uint8_t v) {
    setLutEntry(writePalSel(palCtrl), palIdx, rgb332_to_333(v));
    pal9Second = false;
    if (!(palCtrl & 0x80)) palIdx++;
}

void NEXTVID::palValue9(uint8_t v) {
    if (!pal9Second) {
        pal9First = v;
        pal9Second = true;
    } else {
        // second byte: bit0 = blue LSB, bit7 = Layer 2 priority
        uint16_t rgb = (rgb332_to_333(pal9First) & ~1) | (v & 1);
        uint8_t pal = writePalSel(palCtrl);
        if (v & 0x80) {
            rgb |= 0x8000;
            palHasPrio[pal] = true; // sticky until reset — cheap and safe
        }
        setLutEntry(pal, palIdx, rgb);
        pal9Second = false;
        if (!(palCtrl & 0x80)) palIdx++;
    }
}

void NEXTVID::palControl(uint8_t v) {
    palCtrl = v;
    pal9Second = false;
}

void NEXTVID::clipWrite(uint8_t win, uint8_t v) {
    win &= 3;
    clip[win][clipIdx[win] & 3] = v;
    clipIdx[win] = (clipIdx[win] + 1) & 3;
}

void NEXTVID::clipIndexReset(uint8_t mask) {
    for (int w = 0; w < 4; w++)
        if (mask & (1 << w)) clipIdx[w] = 0;
}

// ===== sprites =====

// Port 0x303B write: bits 5:0 select the pattern slot (bit 7 adds a 128-byte
// half-offset for 4bpp patterns) and bits 6:0 the attribute slot.
void NEXTVID::spriteSlotSelect(uint8_t v) {
    sprPatWrite = (uint16_t)(v & 0x3F) * 256 + ((v & 0x80) ? 128 : 0);
    sprAttrSlot = v & 0x7F;
    sprAttrByte = 0;
}

uint8_t NEXTVID::spriteFlagsRead() {
    uint8_t f = sprFlags;
    sprFlags = 0; // collision/overflow flags clear on read
    return f;
}

void NEXTVID::spriteAttrWrite(uint8_t v) {
    uint8_t* a = sprAttr[sprAttrSlot];
    a[sprAttrByte] = v;
    sprPut(sprAttrSlot, sprAttrByte, v);
    // Auto-advance: 4-byte sprites move on after byte 3 unless byte 3
    // requests the 5th attribute byte
    if (sprAttrByte == 3 && !(v & 0x40)) {
        a[4] = 0;
        sprPut(sprAttrSlot, 4, 0);
        sprAttrByte = 0;
        sprAttrSlot = (sprAttrSlot + 1) & 0x7F;
    } else if (sprAttrByte == 4) {
        sprAttrByte = 0;
        sprAttrSlot = (sprAttrSlot + 1) & 0x7F;
    } else {
        sprAttrByte++;
    }
}

// Logged like the attributes (sprLog, bit 31 set): sprPatterns is the
// renderer's, the guest never reads it back. A live write let lines already
// past show patterns uploaded after them — TX-1696's bottom sprites blinked at
// the level start; a drain per byte instead cost 2-3 ms a frame, the game
// animates its sprites by re-uploading patterns (hw 2026-10-09).
void NEXTVID::spritePatternWrite(uint8_t v) {
    sprLogPut(0x80000000u | ((uint32_t)(sprPatWrite & 0x3FFF) << 8) | v);
    sprPatWrite = (sprPatWrite + 1) & 0x3FFF;
}

// Nextreg 0x34 keeps its OWN sprite index; only with lockstep (NR 0x09 bit 4)
// is it the port 0x303B index (and then it also selects the pattern). Sharing
// it unconditionally moved the port's pattern upload pointer whenever a game
// updated a sprite through nextregs (zxnext.vhd / ZEsarUX tbblue.c agree).
void NEXTVID::spriteNrSelect(uint8_t v) {
    if (NextReg::reg[0x09] & 0x10) spriteSlotSelect(v);
    else sprNrSlot = v & 0x7F;
}

uint8_t NEXTVID::spriteNrIndex() {
    return (NextReg::reg[0x09] & 0x10) ? sprAttrSlot : sprNrSlot;
}

void NEXTVID::spriteAttrDirect(uint8_t byteIdx, uint8_t v, bool inc) {
    const bool lock = NextReg::reg[0x09] & 0x10;
    uint8_t& slot = lock ? sprAttrSlot : sprNrSlot;
    if (byteIdx < 5) { sprAttr[slot][byteIdx] = v; sprPut(slot, byteIdx, v); }
    if (inc) {
        slot = (slot + 1) & 0x7F;
        if (lock) sprAttrByte = 0;
    }
}

// ===== Copper =====

// Copper NOP runs (MOVE to register 0): the first time the copper reaches a NOP
// the run's end is found once and remembered until the copper memory changes,
// so a list padded with NOPs costs one step per run, not one per NOP.
// Atic Atac Remake runs a short WAIT/MOVE list in mode 01 inside an otherwise
// empty copper memory: the copper walked ~1800 NOPs on every raster line,
// copperRun was ~45% of core0 (hw 2026-10-16). Hardware does that for free.
static uint32_t s_copGen = 1;        // bumped by every copper memory write
static uint32_t s_runGen[2];          // per kind (NOP, MOVE 7F): s_copGen the cached run belongs to
static uint16_t s_runFrom[2];         // its first instruction
static uint16_t s_runLen[2];          // its length (1024 = the whole memory)
static uint8_t  s_runLo[2];           // MOVE 7F: the value

// Length of the run of `hi lo` instructions starting at pc (flash: called once
// per run, the scan only when the run is new or the copper memory changed).
// For a NOP (hi = 0) any lo matches.
static __attribute__((noinline)) uint32_t copSkipRun(const uint8_t* m, uint16_t pc, uint8_t hi, uint8_t lo) {
    const int k = hi ? 1 : 0;
    const uint32_t into = (uint32_t)(pc - s_runFrom[k]) & 0x3FF;   // a run may wrap 0x3FF -> 0
    if (s_runGen[k] == s_copGen && (!k || s_runLo[k] == lo) && into < s_runLen[k]) return s_runLen[k] - into;
    uint32_t run = 0;
    uint16_t q = pc;
    while (run < 1024 && m[q * 2] == hi && (!k || m[q * 2 + 1] == lo)) { run++; q = (q + 1) & 0x3FF; }
    s_runGen[k] = s_copGen; s_runFrom[k] = pc; s_runLen[k] = (uint16_t)run; s_runLo[k] = lo;
    return run;
}

void NEXTVID::copperDataWrite(uint8_t v) {
    s_copGen++;
    copperMem[copIndex] = v;
    copIndex = (copIndex + 1) & 0x7FF;
}

void NEXTVID::copperIndexLo(uint8_t v) {
    copIndex = (copIndex & 0x700) | v;
}

void NEXTVID::copperControl(uint8_t v) {
    copIndex = ((v & 0x07) << 8) | (copIndex & 0xFF);
    uint8_t prevMode = copCtrl >> 6, mode = v >> 6;
    copCtrl = v;
    // copper.vhd 70-77: a mode change restarts the list only into 01 (run
    // once) or 11 (loop each frame); 10 resumes where the copper stopped.
    if (mode != prevMode && (mode & 1)) copPC = 0;
}

void NEXTVID::copperData16Write(uint8_t v) {
    // reg 0x63: full instruction per two writes (MSB first), committed on
    // the second byte, index auto-advances by one instruction
    if (!cop16Second) {
        cop16First = v;
        cop16Second = true;
    } else {
        s_copGen++;
        copperMem[copIndex & 0x7FE] = cop16First;
        copperMem[(copIndex & 0x7FE) + 1] = v;
        copIndex = (copIndex + 2) & 0x7FF;
        cop16Second = false;
    }
}

// The copper (copper.vhd), one 28 MHz clock per step: a WAIT that is due
// takes a clock, a MOVE two (the write, then the strobe clear), a NOP one.
// Instructions are big-endian:
//   MOVE: 0RRRRRRR VVVVVVVV  — write V to nextreg R; register 0 = NOP
//   WAIT: 1HHHHHHL LLLLLLLL  — wait until cvc == L and hc_ula >= H*8 + 12
// hc_ula (zxula_timing.vhd 422-438, 7 MHz) is 0 twelve pixels before the
// paper's left edge, so WAIT H lands on paper pixel H*8; cvc steps (and NR
// 0x64 offsets it) at hc_ula 0, which is why the left border of a display
// row still belongs to the previous copper line (MrKWatkins !Copper, test 2).
// PC stays ON a pending WAIT, so the next span re-tests it.
IRAM_ATTR void NEXTVID::copperRun(uint16_t cvc, uint16_t from, uint16_t to) {
    if ((copCtrl >> 6) == 0) return;
    const int32_t end = (int32_t)to << 2;
    if (s_copT < ((int32_t)from << 2)) s_copT = (int32_t)from << 2;
    int guard = 2048;
    while (guard-- && s_copT < end) {
        const uint8_t hi = copperMem[copPC * 2];
        const uint8_t lo = copperMem[copPC * 2 + 1];
        if (hi & 0x80) {
            const uint16_t wl = ((uint16_t)(hi & 0x01) << 8) | lo;
            const int32_t tgt = (int32_t)(((hi >> 1) & 0x3F) * 8 + 12) << 2;
            if (wl != cvc || tgt >= end) return;   // not due in this span
            if (tgt > s_copT) s_copT = tgt;
            s_copT += 1;
        } else if (hi == 0x7F) {
            // MOVE NR 0x7F (the user register, a plain latch): a run of identical
            // ones is one write. The same game pads its list with MOVE 7F,0 —
            // 2.2M copper writes a second through NextReg::write (hw 2026-10-16).
            uint32_t run = copSkipRun(copperMem, copPC, 0x7F, lo);
            const uint32_t fit = ((uint32_t)(end - s_copT) + 1) / 2;   // 2 clocks each
            if (run > fit) run = fit;
            if (run == 0) run = 1;
            NextReg::reg[0x7F] = lo;
            s_copT += (int32_t)(2 * run);
            copPC = (uint16_t)((copPC + run) & 0x3FF);
            continue;
        } else if (hi & 0x7F) {
#if NEXT_TRACE
            extern bool g_nx_copper_wr;   // NextReg::write counts copper writes, does not record them
            g_nx_copper_wr = true;
            NextReg::write(hi & 0x7F, lo);
            g_nx_copper_wr = false;
#else
            NextReg::write(hi & 0x7F, lo);
#endif
            s_copT += 2;
        } else {
            // NOP (MOVE to register 0): see copSkipRun
            uint32_t run = copSkipRun(copperMem, copPC, 0, lo);
            const uint32_t room = (uint32_t)(end - s_copT);
            if (run > room) run = room;
            if (run == 0) run = 1;
            s_copT += (int32_t)run;
            copPC = (uint16_t)((copPC + run) & 0x3FF);
            continue;
        }
        copPC = (copPC + 1) & 0x3FF;
    }
}

// ===== geometry / frame control =====

void NEXTVID::softReset() {
    c1Drain();
    palIdx = 0; pal9Second = false; palCtrl = 0;
    static const uint8_t defclip[4][4] = {
        { 0, 255, 0, 191 }, { 0, 255, 0, 191 }, { 0, 255, 0, 191 }, { 0, 159, 0, 255 },
    };
    for (int w = 0; w < 4; w++) {
        for (int i = 0; i < 4; i++) clip[w][i] = defclip[w][i];
        clipIdx[w] = 0;
    }
    copIndex = 0;
    copPC = 0;
    copCtrl = 0;
    cop16Second = false;
}

size_t getFreeHeap(void);
void NEXTVID::Reset() {
    c1Drain();
    const bool firstReset = !sprPatterns;
    const size_t heap0 = firstReset ? getFreeHeap() : 0;
    if (!sprPatterns) {
        // Two blocks, Next-boot only, never freed (the Next boundary reboots).
        // It used to be ONE 29 KB malloc, which PANICKED at boot once the heap
        // had no hole that size (hw 2026-10-08, pico_malloc panics on NULL).
        // Hot (read per pixel, ~11 KB): rawPal 4K | lut 2K | lineBuf 384 |
        //   pairMap 256 | hiresRow 256 | remap 512 |
        //   layer lines 4 x 384 x 2  -> HOT_SRAM (SRAM when the heap has it).
        // Cold (~19 KB): sprite patterns 16K | copper 2K | sprAttr 640.
        //   HOT_SRAM again (2026-10-11, NXModPlayer): SRAM when the heap has it,
        //   PSRAM on a thin heap. The PREFER_PSRAM reasoning, for the record: 720x576 + NeoGS did
        //   not fit, and this is the one block of the Next video state that is
        //   indexed rather than per-pixel — the sprite row loop below reads 8
        //   or 16 CONTIGUOUS bytes of one pattern per sprite per rendered line
        //   (one XIP line fill, not one per pixel), the copper is core0 and
        //   reads two bytes per instruction, and core0 writing / core1 reading
        //   through the shared coherent XIP cache is exactly what sprLog and
        //   palLog have always done. Costs +19 072 B of SRAM heap on a boot
        //   that claims them; read the [NXV] state: line's addresses (0x11...
        //   = PSRAM, 0x20... = SRAM) and the PERF `c1=` on a sprite title.
        //   Sprite renderer copy + attribute log: ~2.1 KB + 4 KB, their own block.
        static_assert(sizeof(SprRes) == 10, "SprRender packs 128 x 10");
        const unsigned hotSz0 = 8*256*2 + 8*256 + 384 + 512 + 512 + 4 * 768;
        // + the tilemap map-row shadow headers (scanned per tilemap line on core1)
        const unsigned hotSz  = hotSz0 + sizeof(NxMapShadow) * NX_MAP_SH;
        const unsigned coldSz = 16384 + 2048 + 128*5;
        uint8_t* hot  = (uint8_t*)Buffer::palloc(hotSz,  Buffer::NEED_POINTER | Buffer::HOT_SRAM);
        // The 640-wide pair tables and the Layer 2 column band (~6 KB, read per
        // pixel, SRAM or nothing) BEFORE the cold block: that one is read per
        // sprite row and tolerates PSRAM, these do not. Allocated lazily they
        // found 4 KB of heap left on the NEXT_TRACE build (hw 2026-10-11).
        p80Alloc();
        if (!l2Band) l2Band = nxSramBlock(NX_L2_BAND);
        Debug::log("[NXV] 640 tables %s, Layer 2 band %s", p80Blk ? "in SRAM" : "none",
                   l2Band ? "in SRAM" : "none");
        // PREFER_PSRAM again (2026-10-11 evening): read per sprite row, 8-16
        // contiguous bytes = one XIP line fill; 19 KB of SRAM heap was what kept
        // F5 and 720x576 out of reach of a Next session.
        // HOT_SRAM again (2026-10-13): read per sprite row. The hot-page pool
        // takes the heap only ~3 s later (MemESP s_hotWait), after this block.
        uint8_t* cold = (uint8_t*)Buffer::palloc(coldSz, Buffer::NEED_POINTER | Buffer::HOT_SRAM);
        const unsigned sprSz = (sizeof(SprRender) + 3) & ~3u;
        // Two blocks, not one: the renderer state is read per sprite per line
        // (core1), the log only for the bytes written since the last line. As
        // one 6 KB block it missed HOT_SRAM's rule on a thin heap and the whole
        // state landed in butter PSRAM (Santa, hw 2026-10-09: spr 3.9 ms/frame).
#if NEXT_CODE_OVERLAY
        // In the .nxovl window (SRAM on a Next boot, heap on any other): even
        // 2 KB missed HOT_SRAM's rule there (Santa, hw 2026-10-09).
        static SprRender s_sprR __attribute__((section(".time_critical.nxspr"), aligned(4)));
        uint8_t* sb = (uint8_t*)&s_sprR;
#else
        uint8_t* sb = (uint8_t*)Buffer::palloc(sprSz, Buffer::NEED_POINTER | Buffer::HOT_SRAM);
#endif
        // The logs go to butter PSRAM: written by core0, read by core1 a few
        // entries a line, and both cores see one XIP cache (coherent). 6 KB of
        // SRAM heap back for the .nxovl window (2026-10-10).
        uint8_t* sl = (uint8_t*)Buffer::palloc(4 * SPR_LOG, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        Debug::log("[NXV] sprite renderer state %u B @%08X, log %u B @%08X", sprSz,
                   (unsigned)(uintptr_t)sb, 4 * SPR_LOG, (unsigned)(uintptr_t)sl);
        if (!sb || !sl) panic("NEXTVID: no memory for the sprite state");
        sprR = (SprRender*)sb;
        sprLog = (uint32_t*)sl;
        palLog = (uint32_t*)Buffer::palloc(4 * PAL_LOG, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        cRaw = (uint16_t(*)[256])Buffer::palloc(8 * 256 * 2, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        Debug::log("[NXV] palette log @%08X, core0 palette @%08X",
                   (unsigned)(uintptr_t)palLog, (unsigned)(uintptr_t)cRaw);
        if (!palLog || !cRaw) panic("NEXTVID: no memory for the palette log");
        Debug::log("[NXV] state: hot %u B @%08X, cold %u B @%08X",
                   hotSz, (unsigned)(uintptr_t)hot, coldSz, (unsigned)(uintptr_t)cold);
        if (!hot || !cold) panic("NEXTVID: no memory for the Next video state");
        memset(hot, 0, hotSz);
        memset(cold, 0, coldSz);
        sprPatterns = cold;
        copperMem   = cold + 16384;
        sprAttr     = (uint8_t(*)[5])(cold + 16384 + 2048);
        rawPal      = (uint16_t(*)[256])hot;
        lut         = (uint8_t(*)[256])(hot + 8*256*2);
        lineBuf     = hot + 8*256*2 + 8*256;
        pairMap     = lineBuf + 384;
        hiresRow    = pairMap + 256;
        remap       = hiresRow + 256;
        nxUb = (uint16_t*)(remap + 512);
        s_mapSh = (NxMapShadow*)(hot + hotSz0);
        nxCtxRefresh();
    }
    // After the video state: that block is read per pixel, this one per line, so
    // on a thin heap it is the queue that goes to PSRAM (HOT_SRAM rule), not it.
    if (!nx_lock) {
        const int sl = spin_lock_claim_unused(false);
        if (sl >= 0) nx_lock = spin_lock_init((uint)sl);
        else Debug::log("[NXC1] no free hardware spinlock - rendering on core0");
    }
    if (!nx_ring && nx_lock) {
        // The core1 line queue: 320 x 4 B of jobs + 64 x 120 B of per-line state
        // + 64 x 4 B of the state-reuse memo = 9 216 B. No RAM = render on
        // core0 as before.
        // HOT_SRAM (2026-10-11, NXModPlayer): SRAM when the heap has it, butter
        // PSRAM otherwise. Was PREFER_PSRAM for a few hours (SRAM wave 2): it is a
        // LINE-granular interface, not per-pixel — core1 reads exactly one
        // state (120 contiguous bytes, four 32-byte line fills) per rendered
        // line and never touches it again, and the whole memo beside it
        // (nx_st_job) is core0-only. The cross-core rule is the one sprLog and
        // palLog have always relied on: both cores see one coherent XIP cache,
        // so a state core0 wrote is visible to core1. Costs 9 216 B of SRAM
        // heap back; watch `c1=` / FPS and the `ring=` field of the NEXT line.
        // + the copper segment rows: 1024 x 8 B deltas, 320 x 8 B records,
        // 320 x 2 B ring tags = 11 392 B (PSRAM).
        constexpr unsigned base = 4 * NEXT_C1_RING + sizeof(NextLineState) * NEXT_C1_STATES
                                + 4 * NEXT_C1_STATES;
        constexpr unsigned segB = sizeof(NxPalDelta) * NX_SEG_POOL + sizeof(NxSegRec) * NX_SEG_Q
                                + 2 * NEXT_C1_RING;
        constexpr unsigned shB = NX_MAP_SH_ROW * NX_MAP_SH;          // map-row copies, 20 KB (PSRAM)
        static_assert(base % 4 == 0, "segment pool alignment");
        // PSRAM (2026-10-14): read a few bytes a line, the 24 KB of SRAM bought ~nothing
        // (TX-1696 at 576p) and cost the core0 row helper its place after a menu visit.
        static_assert(NX_MAP_SH < 0xFF, "nx_mapOf: 0xFF = no copy");
        uint8_t* q = (uint8_t*)Buffer::palloc(base + segB + shB + 4 + NEXT_C1_RING, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (q) {
            nx_ring = (uint32_t*)q;
            nx_st = (NextLineState*)(q + 4 * NEXT_C1_RING);
            nx_st_job = (uint32_t*)(q + 4 * NEXT_C1_RING + sizeof(NextLineState) * NEXT_C1_STATES);
            memset(nx_st_job, 0, 4 * NEXT_C1_STATES);
            s_segQ    = (NxSegRec*)(q + base);
            s_segPool = (NxPalDelta*)(q + base + sizeof(NxSegRec) * NX_SEG_Q);
            nx_segOf  = (uint16_t*)(q + base + sizeof(NxSegRec) * NX_SEG_Q + sizeof(NxPalDelta) * NX_SEG_POOL);
            memset(nx_segOf, 0, 2 * NEXT_C1_RING);
            s_mapShData = (uint8_t (*)[NX_MAP_SH_ROW])(q + ((base + segB + 3) & ~3u));
            nx_mapOf = q + ((base + segB + 3) & ~3u) + shB;
            memset(nx_mapOf, 0xFF, NEXT_C1_RING);
        }
        Debug::log("[NXC1] core1 line renderer %s @%08X", q ? "ON" : "unavailable (no RAM)", (unsigned)(uintptr_t)q);
    }
    nx_have = false;
    nx_c = nx_r = nx_w;               // drained above; a dead queue may have left a core "busy"
    nx_busy[0] = nx_busy[1] = 0;
    nx_segRun = 0; s_jobSeg[0] = s_jobSeg[1] = 0;
    s_segRel = s_segW; s_segQt = s_segQh;
    if (s_mapSh) for (int k = 0; k < NX_MAP_SH; k++) s_mapSh[k].live = 0;
    s_mapPostK = -1;
    g_next_c1_live = nx_ring != nullptr;
    if (firstReset)
        Debug::log("[NXV] video state took %u B of heap (%u -> %u)",
                   (unsigned)(heap0 - getFreeHeap()), (unsigned)heap0, (unsigned)getFreeHeap());
#if NEXT_TRACE
    extern volatile uint16_t* g_nx_pcs;
    extern volatile uint16_t* g_nx_pcpg;
    extern volatile uint32_t (*g_nx_dmav)[3];
    if (!g_nx_pcs) {
        uint16_t* pb = (uint16_t*)tryCalloc((2048 + 256) * 2 + 8 * 3 * 4);
        if (pb) {
            g_nx_pcs = pb; g_nx_pcpg = pb + 2048;
            g_nx_dmav = (volatile uint32_t (*)[3])(pb + 2048 + 256);
        }
    }
#endif
    pal_w = pal_app = 0;
    buildRemap();
    defaultPalettes();
    palIdx = 0; pal9Second = false; palCtrl = 0;
    static const uint8_t defclip[4][4] = {
        { 0, 255, 0, 191 },  // Layer 2
        { 0, 255, 0, 191 },  // sprites
        { 0, 255, 0, 191 },  // ULA
        { 0, 159, 0, 255 },  // tilemap
    };
    for (int w = 0; w < 4; w++) {
        for (int i = 0; i < 4; i++) clip[w][i] = defclip[w][i];
        clipIdx[w] = 0;
    }

    memset(sprAttr, 0, 128 * 5);
    memset(sprR, 0, sizeof(SprRender));      // queue drained above
    sprR->dirty = 1; sprR->lo = 0; sprR->hi = 127;
    spr_w = spr_app = spr_app0 = s_sprPatW = 0;
    s_sprR0Valid = 0;
    sprNrSlot = 0;
    sprPatWrite = 0;
    sprAttrSlot = 0;
    sprAttrByte = 0;
    sprFlags = 0;

    copIndex = 0;
    copPC = 0;
    copCtrl = 0;
    cop16Second = false;

    totalLines = CPU::statesInFrame ?
        ((CPU::statesInFrame >> ESPectrum::multiplicator) / VIDEO::tStatesPerLine) : 311;
    computeGeometry();
    xPaperOff = ((int)VIDEO::vga.xres - 256) / 2;
    lineIdx = 0;
    s_capPhase = 0; s_capY = -1; s_mcSeen = s_mcActive = 0;
    nxLineArm();
    skipFrame = false;
    framePacing.reset(time_us_32());
}

// Which raster line is fb row 0, and which fb row the paper starts on. The
// anchors are the 128K 50 Hz ones VIDEO::Reset sets. At 60 Hz (nextreg 0x05
// bit 2) the frame is 262 lines and the TOP border is 26 lines shorter: the
// paper starts 38 lines after the INT instead of 64 (z88dk zxn copper table —
// 50 Hz: 192 display + 56 bottom + 8 vsync + 56 top = 312; 60 Hz: 192 + 32 +
// 8 + 30 = 262). Keeping the 50 Hz anchor at 60 Hz put fb row 0 on line 39, so
// only 262 - 39 = 223 rows were ever rendered and rows 223..239 kept whatever
// was drawn there last — the menu footer, under NextZXOS (hw 2026-10-07).
// Returns true when the geometry moved.
bool NEXTVID::computeGeometry() {
    const int tpl = VIDEO::tStatesPerLine;
    const int shift = NextReg::raster().hz60 ? 26 : 0;   // Pentagon: 50 Hz only
    int fv = (VIDEO::tStatesBorder + tpl / 2) / tpl - shift;
    if (fv < 0) fv = 0;
    const int paper = (VIDEO::tStatesScreen + tpl / 2) / tpl - shift;
    // 320x256 frames (the tilemap, Layer 2 320/640 — NextBench and other such
    // desktops): show the 256-line window from its first line — a 240-row fb
    // otherwise cuts 8 lines at the top, where such programs keep their menu
    // bar — and give up 16 at the bottom instead. A 288-row fb shows it all.
    // Keyed on what the frame SHOWS (s_tall, pairPoll), not on the 640 pair
    // mode: that one switches off 25 frames after the program left 640, and
    // the picture jumped 8 rows in the middle of the next screen (hw 2026-10-11).
    if (s_tall) {
        const int d = 32 - (paper - fv);
        if (d > 0) { fv -= d; if (fv < 0) fv = 0; }
    }
    const bool moved = fv != firstVisibleLine || paper - fv != paperTopRow;
    firstVisibleLine = fv;
    paperTopRow = paper - fv;
    // The column comes from the 320-wide anchor (the hw-verified one) moved left
    // by the extra side border of a wider fb: the classic 360-wide 48K/128K
    // anchors are 8 T earlier, fitted to that renderer's 8-px cells, where the
    // paper really moved 10 T (fb x 52 vs 32) — border effects sat 4 px off at
    // 720x576 / 720x480 while 640x480 was right (hw 2026-10-16). Pentagon's
    // anchors already differ by exactly 10.
    {
        const int kind = NextReg::raster().kind;
        const int tb = kind == 0 ? TS_BORDER_320x240
                     : kind == 2 ? TS_BORDER_320x240_PENTAGON : TS_BORDER_320x240_128;
        s_bpad = (int16_t)((((int)VIDEO::vga.xres - 256) / 2 - 32) / 2);
        s_bcol = (int16_t)(tb - ((tb + tpl / 2) / tpl) * tpl - s_bpad);
        s_bstep = kind == 2 ? 1 : 4;
        // The paper begins at fb x (xres - 256) / 2, i.e. x/2 T after fb x 0:
        // the phase-0 boundary of the line clock (nxPaperStart). Kept strictly
        // inside the line so the two boundaries stay ordered.
        int ps = s_bcol + ((int)VIDEO::vga.xres - 256) / 4;
        if (ps < 4) ps = 4;
        if (ps > tpl - 4 - 8 * (NX_CAP_GROUPS - 1)) ps = tpl - 4 - 8 * (NX_CAP_GROUPS - 1);   // the last group stays inside the line
        s_pStartT = (int16_t)ps;
    }
    return moved;
}

// nextreg 0x07 can switch the CPU clock MID-FRAME; the pending line
// boundary was computed in the old T-state scale.
void NEXTVID::onClockChange() {
    nxLineArm();
}

// Skip straight to the next line boundary instead of ticking 4T at a time
// (~17.7k Draw_Opcode calls a frame at 3.5 MHz, 4x that at 14) — TS-Conf's
// haltAdvanceTo. Exactly equivalent to the 4T loop it replaced: every step is
// a whole number of 4T HALT cycles, so tstates ends on the same value, R
// advances by the same count (mod 256 is enough, R is 7-bit), and
// ScanlineWork runs at the same crossings.
IRAM_ATTR void NEXTVID::haltAdvanceTo(uint32_t stEnd) {
    // A HALTed Z80 keeps fetching; in contended memory every fetch waits
    // (halt2int's HALT column). Step it one M1 at a time then, exactly like
    // CPU::FlushOnHaltTo does for the classic 48K/128K.
    if (__builtin_expect(contend, 0) && MemESP::ramContended[Z80::getRegPC() >> 13]) {
        while (CPU::tstates < stEnd) { Tick_Opcode(true); Z80::incRegR(1); }
        return;
    }
    while (CPU::tstates < stEnd) {
        uint32_t tgt = nextLineTstate;
        if (tgt > stEnd) tgt = stEnd;
        const uint32_t n = (tgt > CPU::tstates) ? ((tgt - CPU::tstates + 3) & ~3u) : 4u;
        CPU::tstates += n;
        Z80::incRegR((uint8_t)(n >> 2));
        while (CPU::tstates >= nextLineTstate) ScanlineWork();
    }
}

// NextIdle's fast-forward of a memory-poll loop: the guest clock to stEnd with
// the lines rendered on the way, like haltAdvanceTo but without its R count
// (the caller adds the loop's own fetches). Not in contended memory, where
// every pass has its own length: false, nothing done. Flash: a few calls a frame.
__attribute__((noinline)) bool NEXTVID::idleAdvanceTo(uint32_t stEnd) {
    if (contend) return false;
    while (CPU::tstates < stEnd) {
        const uint32_t t = nextLineTstate < stEnd ? nextLineTstate : stEnd;
        if (t > CPU::tstates) CPU::tstates = t;
        while (CPU::tstates >= nextLineTstate) ScanlineWork();
    }
    return true;
}

extern int ts_notice_x0, ts_notice_x1, ts_notice_y0, ts_notice_y1;   // Video.cpp
extern int led_carve_x0, led_carve_x1, led_carve_y0, led_carve_y1;   // Video.cpp: LED panel
extern int led_strip_y0;   // Video.cpp: LED strip, stamped over the rendered row
#if NEXT_TRACE
void nxErrPoll();   // Z80_JLS.cpp: ERR_NR turning #15 -> dump the RST 8 ring
#endif
// Gigascreen for the next frame (Config::gigascreen_onoff: 0 off, 1 on, 2 auto —
// Auto blends while VIDEO::gigascreenAutoFlip() keeps the countdown up, i.e. the
// program flips the displayed page through #7FFD bit 3 / NR 0x69 bit 6).
// The generic Gigascreen (prev-FB + blend slots 17..136) stays suspended on the
// Next: those slots are NEXTVID's colour lattice.
// A frame whose paper is the classic ULA and nothing else — the only kind the
// blend can apply to (RenderUlaLine's per-line rule). While Layer 2, sprites, the
// tilemap, LoRes, ULANext / ULA+, Timex hi-res or the 640 mode are up the whole
// Gigascreen path is OFF for the frame: not even the row recording runs, which
// otherwise cost every ULA line its per-bit loop plus a 128-byte PSRAM row — the
// Profi DS80 / TS-Conf rule (gigascreenModeIncompatible) applied to the Next's
// own blend (owner, 2026-10-15: "Gigascreen eats a few FPS" on a Layer 2 title).
__attribute__((noinline)) void NEXTVID::gigsFrame() {
    const uint8_t mode = Config::gigascreen_onoff;
    if (!mode) {
        if (gigsPrev) { c1Drain(); Buffer::pfree(gigsPrev); gigsPrev = nullptr; }
        if (s_gigsPrevBrd) { c1Drain(); Buffer::pfree(s_gigsPrevBrd); s_gigsPrevBrd = nullptr; }
        s_gigs = 0;
        return;
    }
    const uint8_t* r = NextReg::reg;
    const bool zxOnly = !(r[0x15] & 0x81)                                // sprites on / LoRes
                     && !(r[0x6B] & 0x80)                                // tilemap
                     && !((NextReg::port123B & 0x02) || (r[0x69] & 0x80))   // Layer 2
                     && !(r[0x68] & 0x88)                                // ULA off / ULA+
                     && !(palCtrl & 0x01)                                // ULANext
                     && !VIDEO::timex_hires_live && !p80Live;            // Timex hi-res / 640
    if (!zxOnly) {
        s_gigs = 0;
        s_gigsSeed = true;    // the prev row is stale once a ZX frame returns: record one first
        return;
    }
    if (!gigsPrev) {
        gigsPrev = (uint8_t*)Buffer::palloc(192 * 128, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        s_gigsSeed = true;
        if (!gigsPrev) { s_gigs = 0; return; }
    }
    if (!s_gigsPrevBrd) {   // best effort: without it the border stays unblended
        s_gigsPrevBrd = (uint8_t*)Buffer::palloc((VIDEO::vga.xres / 2) * VIDEO::vga.yres,
                                                 Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (s_gigsPrevBrd) s_gigsSeed = true;
    }
    bool want = (mode == 1);
    if (mode == 2 && VIDEO::gigascreen_auto_countdown > 0) {
        VIDEO::gigascreen_auto_countdown--;
        want = true;
    }
    if (want && !s_gigsSeed) {
        // ULA palette (NR 0x43 bit 1 picks the second): the average of every
        // pair of its 16 ink colours, to the nearest renderable index. Rebuilt
        // per blended frame — 256 entries, and the palette may have moved.
        const uint16_t* pal = cRaw[(palCtrl & 0x02) ? 4 : 0];
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++) {
                const uint16_t a = pal[i] & 0x1FF, b = pal[j] & 0x1FF;
                const unsigned r = (((a >> 6) & 7) + ((b >> 6) & 7) + 1) >> 1;
                const unsigned g = (((a >> 3) & 7) + ((b >> 3) & 7) + 1) >> 1;
                const unsigned bl = ((a & 7) + (b & 7) + 1) >> 1;
                gigsBlend[(i << 4) | j] = (i == j) ? remap[a]
                                                   : remap[(r << 6) | (g << 3) | bl];
            }
        s_gigs = 1;
    } else {
        s_gigs = 2;   // record only
    }
    s_gigsSeed = false;
}

// Flash: once per frame (it was 300+ B of the full .nxovl window).
void NEXTVID::EndFrame() {
#if NEXT_TRACE
    s_frameSeq = (uint8_t)(s_frameSeq + 1);
    nxErrPoll();
    if (!skipFrame) ++g_nextvid_frames;
#endif
    // fb rows the raster never reaches this frame (a 288-row fb at 60 Hz:
    // 262 lines for 288 rows) are drawn here from the last line's state, or
    // they would keep whatever was painted there before — UI included.
    if (!skipFrame) {
        int row = (int)totalLines - firstVisibleLine;
        if (row < 0) row = 0;
        for (; row < (int)VIDEO::vga.yres; row++) emitRow(row);
    }
    s_brdHead = s_brdCnt = 0;   // the frame's T-states restart
    s_flipHead = s_flipCnt = 0;
    s_flipPrev = s_flipFrame; s_flipFrame = 0;
    // The attribute copy stays on for two frames after the last in-row change.
    s_mcActive = s_mcSeen ? 2 : (s_mcActive ? (uint8_t)(s_mcActive - 1) : 0);
    s_mcSeen = 0;
    s_bmpShTried = 0;
    // The machine timing (nextreg 0x03) and 50/60 Hz (0x05) change the whole
    // frame: apply at the frame boundary, after core1 has drawn every row of
    // the old geometry (RenderLayers reads paperTopRow live). The BIOS writes
    // the timing just before its soft reset into a personality, NextZXOS's TAP
    // Loader without any reset.
    {
        static uint8_t s_key = 0xFF;
        const uint8_t key = NextReg::rasterKey();
        if (key != s_key) {
            s_key = key;
            c1Drain();
            ESPectrum::nextRasterApply();
            computeGeometry();
        }
    }
    if (ESPectrum::nextRefreshCheckPending) {
        ESPectrum::nextRefreshCheckPending = false;
        ESPectrum::nextRefreshCheck();
    }
    // A fixed 1-in-64 skip made the BIOS update at <1 Hz even when the CPU
    // managed 50 emulated frames/s. Bound skipping by wall time instead;
    // when emulation is slow, every frame is drawn.
    skipFrame = framePacing.skip(ESPectrum::maxSpeed, time_us_32());
    // Renderer-bound frames: when core0 spent more than NX_AUTOSKIP_US of this
    // frame waiting for room in the core1 queue (or rendering rows itself in
    // that wait), the next frame is emulated but not drawn — never two in a
    // row, so the picture still updates at >= 25 Hz. The guest then runs at
    // the speed core0 can give it instead of the speed core1 can draw it
    // (Adventures in Videoland, Layer 2 320x256 + 80-column tilemap: core1
    // 24-27 ms a frame, hw 2026-10-16). A light frame never waits: core0
    // posts the whole frame and idles in the frame pacing with the queue full.
    {
        static uint32_t s_qw = 0;
        static bool s_lastAuto = false;
        const uint32_t qw = g_nx_qwait_us, waited = qw - s_qw;
        s_qw = qw;
        // Not while the ULA is gigascreening or a title flips the screen inside
        // the frame (Across the Edge, Insult): a skipped frame pairs frame N
        // with N+2, which has the SAME page phase — the blend sees one page
        // twice and the flip effect runs at 25 Hz. The guest slows instead.
        // ...nor while a writer races the beam (the attribute/bitmap capture is
        // on): its effect is per frame, and a dropped frame shows as a twitch.
        const bool coherent = s_gigs != 0 || s_flipFrame >= 2 || s_flipPrev >= 2 || s_mcActive != 0;
        if (!skipFrame && !s_lastAuto && g_next_c1_live && waited > NX_AUTOSKIP_US && !coherent) {
            skipFrame = true;
            s_lastAuto = true;
            g_nx_autoskip = g_nx_autoskip + 1;
        } else s_lastAuto = false;
    }
    lineIdx = 0;
    totalLines = (CPU::statesInFrame >> ESPectrum::multiplicator) / VIDEO::tStatesPerLine;
    s_capPhase = 0; s_capY = -1;
    nxLineArm();
    gigsFrame();
}

// Wait states of a contended access starting now. Contention exists only at
// 3.5 MHz (multiplicator 0), so the line structure is nextLineTstate's: the
// current line is lineIdx, it began at nextLineTstate - tStatesPerLine. A
// paper line's 128-T window starts contCol T into the line BEFORE it (48K
// #14335 = line 63 col 223) and so crosses the line boundary.
inline __attribute__((always_inline)) unsigned NEXTVID::contendWait() {
    // The current line began at lineIdx * tpl (NOT nextLineTstate - tpl: the
    // line clock has two boundaries per line since the paper-start capture).
    const int tpl = VIDEO::tStatesPerLine;
    int idx = (int)CPU::tstates - (int)lineIdx * tpl - contCol;
    int k = (int)lineIdx - contLine;
    if (idx < 0) { idx += tpl; k--; }
    return ((unsigned)k < 192u && (unsigned)idx < 128u) ? contWait[idx & 7] : 0u;
}

IRAM_ATTR void NEXTVID::Tick(unsigned int statestoadd, bool contended) {
    if (__builtin_expect(contended, 0) && contend) statestoadd += contendWait();
    CPU::tstates += statestoadd;
    while (CPU::tstates >= nextLineTstate) ScanlineWork();
}

IRAM_ATTR void NEXTVID::lineTick() {
    while (CPU::tstates >= nextLineTstate) ScanlineWork();
}

IRAM_ATTR void NEXTVID::Tick_Opcode(bool contended) {
    CPU::tstates += (__builtin_expect(contended, 0) && contend) ? 4u + contendWait() : 4u;
    while (CPU::tstates >= nextLineTstate) ScanlineWork();
}


#if NEXT_TRACE
// Compositor cost estimate for the 1 Hz NEXT: line: every 16th visible line
// is timed and extrapolated ×16 (~15 time_us_64 pairs per frame — noise-level
// overhead, but enough to see RenderLine's real share of cpu_frame_us).
volatile uint32_t g_nextvid_us = 0;
// Per-layer split of the same sampled lines (x16): ULA/LoRes, tilemap,
// Layer 2, sprites. nx_layer_sample is set by the sampling wrapper.
volatile uint32_t g_nextvid_layer_us[7] = {0, 0, 0, 0, 0, 0, 0};   // [6] = log apply (spr/pal) before the layers
volatile uint32_t g_nx_logapply_spr = 0, g_nx_logapply_pal = 0;     // entries applied (all rows)   // + mix, out (RenderLine copy)
volatile uint32_t g_nx_rows = 0;            // rows composed (all, not only sampled)
// one 32-bit load of the raw timer (time_us_64 is a hi/lo retry loop): these
// marks sit in the full .nxovl window, the delta is wrap-safe
#define NX_LT_BEGIN() uint32_t _lt0 = nx_layer_sample ? timer_hw->timerawl : 0
#define NX_LT_END(i) do { if (nx_layer_sample) { const uint32_t _t = timer_hw->timerawl; \
    g_nextvid_layer_us[i] += (_t - _lt0) * 16; _lt0 = _t; } } while (0)
#endif
#if !NEXT_TRACE
#define NX_LT_BEGIN() do {} while (0)
#define NX_LT_END(i) do {} while (0)
#endif

// noinline: at -O3 it was inlined into Tick, Tick_Opcode, lineTick and
// haltAdvanceTo — four ~0.5 KB SRAM copies of a once-per-scanline function.
#if NEXT_TRACE
// Profile (NEXT pc: line): Z80 PC sampled once per raster line, 32-byte buckets,
// plus the MMU page of the PC's slot.
// Heap (taken at the first NEXTVID::Reset): 4.5 KB of .bss here OOM-panicked the
// boot in Config::load (hw 2026-10-09).
volatile uint16_t* g_nx_pcs = nullptr;    // [2048]
volatile uint16_t* g_nx_pcpg = nullptr;   // [256]
#endif
__attribute__((noinline)) IRAM_ATTR void NEXTVID::ScanlineWork() {
    // Phases 0..15 = the attribute capture points inside line lineIdx's paper
    // (flash; phase 0 alone unless a racing writer is on), then the line end.
    if (s_capPhase < NX_CAP_GROUPS) { nxPaperStart(); return; }
#if NEXT_TRACE
    if (g_nx_pcs) { const uint16_t pc = Z80::getRegPC();
      g_nx_pcs[pc >> 5] = g_nx_pcs[pc >> 5] + 1;
      const uint8_t pg = MemESP::mmu[pc >> 13];
      g_nx_pcpg[pg] = g_nx_pcpg[pg] + 1; }
#endif
    uint8_t m = ESPectrum::multiplicator;
    if (lineIdx < totalLines) {
        // Copper, in time order across the display row (copper.vhd /
        // zxula_timing.vhd): the row's left border is the tail of the PREVIOUS
        // copper line (hc_ula >= uL), its paper and right border the head of
        // this one. cvc = raster line - paper top + NR 0x64. Mode 11 restarts
        // at hc_ula 0 of copper line 0. Registers are captured mid-paper (one
        // snapshot per row); palette writes are recorded with their fb x and
        // the row is then drawn in segments (renderRowSegments).
        const uint32_t paperStart = (uint32_t)(firstVisibleLine + paperTopRow);
        const uint32_t off = NextReg::reg[0x64];
        const uint16_t cur = (uint16_t)((lineIdx + totalLines - paperStart + off) % totalLines);
        const uint16_t prev = (uint16_t)((lineIdx + 2 * totalLines - 1 - paperStart + off) % totalLines);
        const uint16_t hcEnd = (uint16_t)(VIDEO::tStatesPerLine * 2);
        const int uL = (int)hcEnd - (xPaperOff - 12);       // hc_ula of fb x 0
        int row = (int)lineIdx - firstVisibleLine;
        const bool draw = !skipFrame && row >= 0 && row < (int)VIDEO::vga.yres;
        if ((copCtrl >> 6) != 0) {
            s_pdn = 0;
            s_pdRec = draw;
            s_copXBase = (int16_t)(xPaperOff - 12 - (int)hcEnd);
            copperRun(prev, (uint16_t)uL, hcEnd);
            s_copT -= (int32_t)hcEnd << 2;                  // hc_ula wraps, cvc steps
            if (s_copT < 0) s_copT = 0;
            s_copXBase = (int16_t)(xPaperOff - 12);
            if ((copCtrl >> 6) == 3 && cur == 0) copPC = 0;
            copperRun(cur, 0, 140);                         // up to paper x 128
            NextLineState st;
            if (draw) {
                captureLine(st);
                nxRowFixups(st, row, firstVisibleLine, paperTopRow);
            }
            copperRun(cur, 140, (uint16_t)uL);
            s_pdRec = false;
            if (draw) {
                if (s_pdn) {
                    if (!segPost(row, st)) {
                        nx_sync = st;
                        renderRowSegments(row);
                    }
                } else {
                    s_preCap = &st;
                    emitRow(row);
                    s_preCap = nullptr;
                }
            }
        } else if (draw) {
            emitRow(row);
        }
        lineIdx++;
        s_capPhase = 0;
        nxLineArm();   // the next line's paper start (or the frame-tail sentinel)
    } else {
        // frame over-run (turbo tail) — wait for EndFrame to restart
        nextLineTstate = 0xFFFFFFFF;
    }
}

// One fb row: posted to core1 with a register snapshot, or drawn here.
// noinline: ScanlineWork and EndFrame's tail fill both call it.
#if NEXT_TRACE
volatile uint32_t g_nx_emit_us = 0;          // core0: emitRow (capture + post) per second
volatile uint32_t g_nx_rowprobe[2][8];
#endif
__attribute__((noinline)) IRAM_ATTR void NEXTVID::emitRow(int row) {
#if NEXT_TRACE
    const uint32_t _em0 = timer_hw->timerawl;
    emitRowBody(row);
    g_nx_emit_us = g_nx_emit_us + (timer_hw->timerawl - _em0);
}
__attribute__((always_inline)) inline void NEXTVID::emitRowBody(int row) {
#endif
    if (g_next_c1_live) {
        // Post the line: capture, and open a new state slot only when
        // something the renderer reads has moved since the last line.
        NextLineState st;
        if (s_preCap) st = *s_preCap;
        else {
            captureLine(st);
            nxRowFixups(st, row, firstVisibleLine, paperTopRow);
        }
        if (!nx_have || !nxSameState(st, nx_st[nx_cur])) {
            const uint32_t k = (nx_cur + 1) % NEXT_C1_STATES;
            nxWaitUntil(nx_st_job[k]);
            nx_st[k] = st;
            nx_cur = k;
            nx_have = true;
        }
        if (nx_w - nx_r >= NEXT_C1_RING) nxWaitUntil(nx_w - NEXT_C1_RING + 1);
    }
    if (g_next_c1_live) {
        nx_ring[nx_w % NEXT_C1_RING] = (uint32_t)row | (nx_cur << 16);
        nx_segOf[nx_w % NEXT_C1_RING] = s_segPost;
        if (s_mapSh && (nx_st[nx_cur].r6B & 0x80)) nxMapSnapPost(nx_st[nx_cur], row, paperTopRow, nx_w);
        else nx_mapOf[nx_w % NEXT_C1_RING] = 0xFF;
        __dmb();                    // job + state visible before the index
        nx_w = nx_w + 1;
        nx_st_job[nx_cur] = nx_w;
#if NEXT_TRACE
        const uint32_t occ = nx_w - nx_r;      // ring occupancy (see g_nx_ring_max)
        if (occ > g_nx_ring_max) g_nx_ring_max = occ;
        g_nx_ring_now = occ;
#endif
    } else {
        if (s_preCap) nx_sync = *s_preCap;
        else {
            captureLine(nx_sync);
            nxRowFixups(nx_sync, row, firstVisibleLine, paperTopRow);
        }
        LS = &nx_sync;
#if NEXT_TRACE
        if ((row & 15) == 0) {
            const uint32_t t0 = timer_hw->timerawl;
            nx_layer_sample = true;
            RenderLine(row);
            nx_layer_sample = false;
            g_nextvid_us += (timer_hw->timerawl - t0) * 16;
        } else
#endif
        RenderLine(row);
    }
}


// For sdcard.c (core1, between SD blocks): the renderer is behind — core0 is
// draining the row queue, or the queue is within 64 rows of full and core0 is
// (or is about to be) waiting for room. A speculative SD read-ahead then gives way (see g_sd_as_need).
extern "C" int sd_async_render_behind(void) {
    return g_next_c1_live && (g_nx_c0wait || (uint32_t)(nx_w - nx_r) > NEXT_C1_RING - 64u);
}

// core1: render up to 8 posted lines per call (render_core interleaves the
// GS pump and the rest of its loop between batches).
// (The background SD read is pumped by render_core right before each call of
// this — a second pump here was redundant and cost .nxovl bytes.)
IRAM_ATTR void NEXTVID::c1Pump() {
#if NEXT_TRACE
    if (nx_c == nx_w) return;
    const uint64_t tb = time_us_64();
#endif
    for (int n = 0; n < 8; n++) {
        uint32_t j;
        if (!rowClaim(1, j)) break;         // nothing queued, or core0 renders a row on other logs
        renderJob(j);
        rowRelease(1);
#if NEXT_TRACE
        g_nx_c1_rows = g_nx_c1_rows + 1;
#endif
    }
#if NEXT_TRACE
    g_nx_c1_us = g_nx_c1_us + (uint32_t)(time_us_64() - tb);
#endif
}
extern "C" IRAM_ATTR void next_render_core1_pump(void) { NEXTVID::c1Pump(); }

// ===== scanline compositor =====

// One framebuffer row. The layers are composed into lineBuf in linear pixel
// order, then copied into the framebuffer in ONE pass, already rotated into
// scanout order (the HDMI/VGA ISR reads fb bytes at x^2 — legacy renderers bake
// that into their AluBytes words; hw: NextZXOS text scrambled in 4-pixel groups
// without it). The framebuffer therefore only ever holds a finished row. It used
// to be composed in place and rotated afterwards, and whenever the scanout read
// the row between those two passes the display showed it unrotated for a frame:
// 4-pixel specks on glyph edges of a STATIC picture, one fb row at a time,
// drifting slowly with the frame/display phase (capture 2026-09-25).
// The copy also skips the OSD box: the F8 stats / F9-F10 volume box, 144x16 at
// VIDEO::osd_bar_x/osd_bar_y (168/188 x 220/268 in the border; on the paper in
// the big HDMI modes, osdGeomRecalc), which ESPectrum::loop repaints only after
// EndFrame — tsRenderLine's and Update_Border_DS80's rectangle. The box is
// word-aligned (osd_bar_x and 144 are multiples of 4), so it is a range of whole words.
// A row the copper re-coloured mid-line (MrKWatkins !Copper: Swedish flags
// drawn by rewriting PAPER/BORDER 7 every 8 pixels). The palette now holds the
// line-END values; each recorded write knows its fb x and the value it
// replaced. Roll the palette back to the line start, then compose the row once
// per palette state and keep each pass's span. core0, after a drain — core1
// must not read the palette while it is rolled back. Flash: rare rows.
__attribute__((noinline)) void NEXTVID::renderRowSegments(int row) {
    c1Drain(3);
    palApply(pal_w);              // line-end values, as the deltas below expect
    LS = &nx_sync;
    segCompose(row, 0, 0xFFFFFFFFu, s_pdn, false);
}

// Post a copper-segmented row to the queue (see NX_SEG_POOL). false = no room
// (pool / record ring full, or no queue): the caller takes the drain path.
// The row's palette log position is the line END: the deltas' `neu` values are
// the line-end palette, and segCompose rolls back from there.
#if NEXT_TRACE
// Copper segment rows, per second: [0] posted [1] drain fallback [2] deltas
// [3] distinct x (= extra passes) [4] palette mask (bit = pal) [5] x min
// [6] x max [7] deltas in the left border [8] deltas in the right border
// [9] rows whose deltas are all outside the paper.
volatile uint32_t g_nx_seg[10] = { 0, 0, 0, 0, 0, 0xFFFF, 0, 0, 0, 0 };
static __attribute__((noinline)) void nxSegNote(bool posted, int xp) {
    g_nx_seg[posted ? 0 : 1] = g_nx_seg[posted ? 0 : 1] + 1;
    g_nx_seg[2] = g_nx_seg[2] + s_pdn;
    int lastX = -1, outside = 0;
    for (unsigned i = 0; i < s_pdn; i++) {
        const int x = s_pd[i].x;
        if (x != lastX) { g_nx_seg[3] = g_nx_seg[3] + 1; lastX = x; }
        g_nx_seg[4] = g_nx_seg[4] | (1u << (s_pd[i].pal & 7));
        if ((uint32_t)x < g_nx_seg[5]) g_nx_seg[5] = x;
        if ((uint32_t)x > g_nx_seg[6]) g_nx_seg[6] = x;
        if (x < xp) { g_nx_seg[7] = g_nx_seg[7] + 1; outside++; }
        else if (x >= xp + 256) { g_nx_seg[8] = g_nx_seg[8] + 1; outside++; }
    }
    if (outside == (int)s_pdn) g_nx_seg[9] = g_nx_seg[9] + 1;
}
#endif

__attribute__((noinline)) bool NEXTVID::segPost(int row, NextLineState& st) {
#if NEXT_TRACE
    const bool ok = segPostBody(row, st);
    nxSegNote(ok, xPaperOff);
    return ok;
}
__attribute__((noinline)) bool NEXTVID::segPostBody(int row, NextLineState& st) {
#endif
    if (!g_next_c1_live || !nx_segOf) return false;
    const unsigned n = s_pdn;
    while (s_segQt != s_segQh && (int32_t)(nx_r - s_segQ[s_segQt % NX_SEG_Q].job) > 0) {
        s_segRel = s_segQ[s_segQt % NX_SEG_Q].end;      // rendered: its deltas are free
        s_segQt++;
    }
    if (s_segQh - s_segQt >= NX_SEG_Q || s_segW + n - s_segRel > NX_SEG_POOL) return false;
    const uint32_t start = s_segW;
    for (unsigned i = 0; i < n; i++) s_segPool[(start + i) & (NX_SEG_POOL - 1)] = s_pd[i];
    st.palSeq = pal_w;
    s_segPost = (uint16_t)(((start & (NX_SEG_POOL - 1)) << 6) | n);   // n <= 32
    s_preCap = &st;
    emitRow(row);
    s_preCap = nullptr;
    s_segPost = 0;
    s_segQ[s_segQh % NX_SEG_Q] = { nx_w - 1, start + n };
    s_segQh++;
    s_segW = start + n;
    return true;
}

// Compose a row whose palette the copper rewrote mid-line: roll the palette back
// to the line start, compose once per palette state, keep each pass's span. The
// caller guarantees no other renderer reads the palette meanwhile (core0 after a
// drain, or a queue claim with the other core idle and blocked).
__attribute__((noinline)) void NEXTVID::segCompose(int row, unsigned start, unsigned mask, int n, bool pool) {
    const NxPalDelta* const d = pool ? s_segPool : s_pd;
#define D(i) d[(start + (unsigned)(i)) & mask]
    uint8_t* const lineBuf = nxCtx()->line;
    uint8_t acc[400];
    const int xres = VIDEO::vga.xres;
    for (int i = n - 1; i >= 0; i--) {
        rawPal[D(i).pal][D(i).idx] = D(i).old; nxPalDirty(D(i).pal);
        lut[D(i).pal][D(i).idx] = remap[D(i).old & 0x1FF];
    }
    int x0 = 0, i = 0;
    while (x0 < xres) {
        // apply every write that takes effect at or before x0
        while (i < n && D(i).x <= x0) {
            rawPal[D(i).pal][D(i).idx] = D(i).neu; nxPalDirty(D(i).pal);
            lut[D(i).pal][D(i).idx] = remap[D(i).neu & 0x1FF];
            i++;
        }
        const int x1 = (i < n && D(i).x < xres) ? D(i).x : xres;
        RenderLayers(lineBuf, row);
        for (int x = x0; x < x1; x++) acc[x] = lineBuf[x];
        x0 = x1;
    }
    for (; i < n; i++) {   // writes past the visible line: still the final palette
        rawPal[D(i).pal][D(i).idx] = D(i).neu; nxPalDirty(D(i).pal);
        lut[D(i).pal][D(i).idx] = remap[D(i).neu & 0x1FF];
    }
#undef D
    for (int x = 0; x < xres; x++) lineBuf[x] = acc[x];
    s_rowDiff = false;            // the right pixels were not kept per segment: solid pairs
    s_lineReady = true;
    RenderLine(row);
    s_lineReady = false;
}

// The framebuffer words a row copy must leave alone: the F8 stats box rows,
// or the OSD::notify banner (VIDEO::setNoticeCarve, 4-aligned) — drawn by
// their own painters, a row copy would erase them on the very next frame.
// [l0, l1) = the LED strip / panel words (left of the stats box by construction:
// LED::drawPanel never reaches it, the strip is at x 4 and short).
static inline __attribute__((always_inline)) void nxCarve(int row, int words, int& k0, int& k1,
                                                           int& l0, int& l1) {
    k0 = words; k1 = words; l0 = 0; l1 = 0;
    if (row >= led_carve_y0 && row < led_carve_y1) {
        l0 = led_carve_x0 >> 2; l1 = (led_carve_x1 + 3) >> 2;
        if (l1 > words) l1 = words;
    }
    const int cy0 = VIDEO::osd_bar_y;   // osdGeomRecalc: the border, or the paper in big modes
    if ((LS->osd & 0x07) != 0 && row >= cy0 && row < cy0 + 16) {
        k0 = VIDEO::osd_bar_x >> 2;
        k1 = k0 + 144 / 4;
    } else if (row >= ts_notice_y0 && row < ts_notice_y1) {
        k0 = ts_notice_x0 >> 2;
        k1 = (ts_notice_x1 + 3) >> 2;
    }
}

// Packed-pair frame (Timex hi-res): the ULA left HIRES_MARK where its
// 512-pixel line goes (hiresRow, already pair slots); everything else is a
// palette index and becomes a solid pair. Word at a time: almost every 4-byte
// group is either all HIRES_MARK (take the hi-res word as is) or one colour
// (border, ULA-free paper); only groups that mix layers go byte by byte.
// xPaperOff is a multiple of 4 in both fb widths (32 / 52), so paper words
// line up with hiresRow. Flash: Timex hi-res is rare on the Next, and the
// .nxovl window is full (2026-10-12).
static uint16_t nxHiresUnder(int x);          // the mix at fb x with the ULA transparent (9-bit RGB)
static uint8_t  nxZxNearest(uint16_t v);
__attribute__((noinline)) static void nxHiresPairRow(uint8_t* lineBuf, const uint8_t* hiresRow,
                                                     const uint8_t* pairMap, int x0, int words) {
    uint32_t* lw = (uint32_t*)lineBuf;
    const uint32_t* hw = (const uint32_t*)hiresRow;
    const uint8_t* const pr = &VIDEO::profi_pair_lookup[0][0];
    for (int k = 0; k < words; k++) {
        const uint32_t w = lw[k];
        const int x = k << 2;
        if (w == 0x10101010u && (unsigned)(x - x0) < 256u) { lw[k] = hw[(x - x0) >> 2]; continue; }
        const uint8_t b0 = (uint8_t)w;
        if (w == b0 * 0x01010101u && b0 != HIRES_MARK && b0 != HIRES_MARK2) {
            lw[k] = pairMap[b0] * 0x01010101u; continue;
        }
        uint8_t* b = (uint8_t*)&lw[k];
        for (int i = 0; i < 4; i++) {
            const uint8_t v = b[i];
            const int hx = x + i - x0;
            if (v == HIRES_MARK && (unsigned)hx < 256u) b[i] = hiresRow[hx];
            else if (v == HIRES_MARK2 && (unsigned)hx < 256u) {
                const uint8_t h = hiresRow[hx], c = h & 0x0F, u = nxZxNearest(nxHiresUnder(x + i));
                b[i] = (h & 0x80) ? pr[(c << 4) | u] : pr[(u << 4) | c];
            } else b[i] = pairMap[v];
        }
    }
}

IRAM_ATTR void NEXTVID::RenderLine(int row) {
    uint32_t* dst = (uint32_t*)VIDEO::vga.frameBuffer[row];
    uint8_t* const lineBuf = nxCtx()->line;
    if (!s_lineReady) RenderLayers(lineBuf, row);
#if NEXT_TRACE
    g_nx_rows = g_nx_rows + 1;
    const uint32_t _out0 = nx_layer_sample ? timer_hw->timerawl : 0;
#endif
    if (LS->hires == 1) {
        nxHiresPairRow(lineBuf, hiresRow, pairMap, xPaperOff, ((int)VIDEO::vga.xres + 3) >> 2);
    } else if (LS->hires == 2) {
        // 640-wide pair frame: lineBuf = left pixels, p80Line2 = right pixels
        // (only on rows whose 80-column tilemap phases differ); count what is
        // shown for the frame's 16 colours (pairPoll), every 4th row.
        const int xres = (int)VIDEO::vga.xres;
        uint32_t* const hist = p80Hist;
        if (!(row & 3)) {                       // every 4th row: pairPoll only ranks colours
            for (int x = 0; x < xres; x++) hist[lineBuf[x]]++;
            if (s_rowDiff) for (int x = s_dx0; x < s_dx1; x++) hist[p80Line2[x]]++;
        }
        const uint8_t* __restrict m16 = p80Map16;
        const uint8_t* __restrict so = p80Solid;
        uint8_t* __restrict l = lineBuf;
        if (s_rowDiff) {
            const uint8_t* __restrict pr = &VIDEO::profi_pair_lookup[0][0];
            const uint8_t* __restrict r = p80Line2;
            const int x0 = s_dx0, x1 = s_dx1;
            for (int x = 0; x < x0; x++) l[x] = so[l[x]];
            for (int x = x0; x < x1; x++) l[x] = pr[(m16[l[x]] << 4) | m16[r[x]]];
            for (int x = x1; x < xres; x++) l[x] = so[l[x]];
        } else {
            for (int x = 0; x < xres; x++) l[x] = so[l[x]];
        }
    }
    const uint32_t* src = (const uint32_t*)lineBuf;
    const int words = ((int)VIDEO::vga.xres + 3) >> 2;
    int k0, k1, l0, l1;                            // [l0, l1) and [k0, k1) = words left alone
    nxCarve(row, words, k0, k1, l0, l1);
    if (l1 > k0) l1 = k0;
    for (int k = 0; k < l0; k++) { const uint32_t v = src[k]; dst[k] = (v >> 16) | (v << 16); }
    for (int k = l1; k < k0; k++) { const uint32_t v = src[k]; dst[k] = (v >> 16) | (v << 16); }
    for (int k = k1; k < words; k++) { const uint32_t v = src[k]; dst[k] = (v >> 16) | (v << 16); }
    // The LED strip: glyphs over whatever this row shows (border, tilemap, Layer
    // 2), fresh every frame; 8 rows a frame, flash. A carve froze the border.
    if ((unsigned)(row - led_strip_y0) < 8u) LED::stampStripRow(row - led_strip_y0, (uint8_t*)dst);
#if NEXT_TRACE
    if (nx_layer_sample) g_nextvid_layer_us[5] += (timer_hw->timerawl - _out0) * 16;
#endif
}

// ---- Layer compositor (zxnext.vhd 6940-7350, zxula.vhd 470-565) ----
// Every layer renders its line into its own buffer of 9-bit RGB (palette
// applied), in framebuffer x; the mixer then picks per pixel by NR 0x15 bits
// 4:2 exactly as the board does, with transparency decided on the COLOUR after
// the palette (NR 0x14) for the ULA / LoRes, Layer 2 and text-mode tiles, on
// the index for sprites (NR 0x4B) and standard tiles (NR 0x4C), and the
// fallback colour (NR 0x4A) where nothing is opaque. Flags above the RGB:
static constexpr uint16_t NX_TR    = 0x8000;   // transparent
static constexpr uint16_t NX_SLOT  = 0x4000;   // low byte already a fb slot (Timex hi-res mark, Gigascreen blend)
static constexpr uint16_t NX_BRD   = 0x2000;   // ULA border pixel (layer orders 3-5)
static constexpr uint16_t NX_BELOW = 0x1000;   // tilemap pixel under the ULA (attr bit 0 / 512-tile mode)
static constexpr uint16_t NX_PRIO  = 0x0800;   // Layer 2 palette priority (bit 15 of the entry)

static inline __attribute__((always_inline)) void nxFillTr(uint16_t* b, int n) {
    uint32_t* w = (uint32_t*)b;
    for (int i = 0, e = (n + 1) >> 1; i < e; i++) w[i] = 0x80008000u;
}
// palette entry -> 9-bit RGB, or transparent when it equals NR 0x14
static inline __attribute__((always_inline)) uint16_t nxRgb(uint16_t raw, uint8_t tr) {
    const uint16_t v = raw & 0x1FF;
    return ((v >> 1) == tr) ? NX_TR : v;
}
static void nxLoResStd(uint16_t* p, const uint8_t* base, uint8_t sxl, uint8_t ofs,
                       const uint16_t* raw, uint8_t tr);

// Mid-line border changes: coloured runs, then the closing colour. Paper rows
// get the runs under the paper too; the ULA layer overwrites those 256 pixels.
// Flash: only rows that carry a change come here.
__attribute__((noinline)) static void nxPaintBorderRuns(uint16_t* u, int xres, const uint16_t* bc,
                                                        uint16_t borderV) {
    int x = 0;
    for (int i = 0; i < LS->brd.n; i++) {
        int xe = 2 * LS->brd.t[i];
        if (xe > xres) xe = xres;
        if (xe > x) {
            const uint16_t c = bc[(LS->brd.c[i >> 1] >> ((i & 1) * 4)) & 7];
            for (; x < xe; x++) u[x] = c;
        }
    }
    for (; x < xres; x++) u[x] = borderV;
}

// Gigascreen on the BORDER pixels of a row (s_gigsPrevBrd): each pixel blended
// with the previous frame's nibble through the same table as the paper (the
// border colours are the first eight ULA colours), this frame's recorded. The
// paper columns of a paper row belong to the ULA renderer; a transparent
// border pixel keeps NX_TR. Walks the row's #FE runs like nxPaintBorderRuns.
__attribute__((noinline)) static void nxBorderGigs(uint16_t* u, int xres, int row, bool paperRow,
                                                   int xPap, const uint8_t* blendTab) {
    uint8_t* pb = s_gigsPrevBrd + row * (xres / 2);
    const bool blend = LS->gigs == 1;
    int x = 0;
    auto span = [&](int xe, uint8_t c) {
        if (xe > xres) xe = xres;
        for (; x < xe; x++) {
            if (paperRow && x >= xPap && x < xPap + 256) continue;
            const uint8_t sh = (uint8_t)((x & 1) * 4);
            const uint8_t prev = (pb[x >> 1] >> sh) & 15;
            pb[x >> 1] = (uint8_t)((pb[x >> 1] & (0xF0 >> sh)) | (c << sh));
            if (blend && !(u[x] & NX_TR)) u[x] = (uint16_t)(NX_SLOT | NX_BRD | blendTab[(prev << 4) | c]);
        }
    };
    for (int i = 0; i < LS->brd.n; i++) span(2 * LS->brd.t[i], (LS->brd.c[i >> 1] >> ((i & 1) * 4)) & 7);
    span(xres, LS->border & 7);
}

// The ULA layer (zxula.vhd): border, then the paper — classic / ULANext /
// LoRes / Timex hi-res / Gigascreen. NR 0x68 bit 7 (ULA off) is applied by the
// mixer, not here: blend mode 00 still sees the ULA colour.
// Rotate the 256-pixel paper left by n (three reversals, no buffer: core1's
// stack is 2 KB). Only on a scrolled line.
static IRAM_ATTR void nxRev(uint16_t* a, int i, int j) {
    for (j--; i < j; i++, j--) { const uint16_t t = a[i]; a[i] = a[j]; a[j] = t; }
}
static IRAM_ATTR void nxRotL(uint16_t* a, int n) {
    nxRev(a, 0, n); nxRev(a, n, 256); nxRev(a, 0, 256);
}

// Nearest of the 16 ZX colours (the only ones a packed-pair hi-res frame can
// show) to a 9-bit RRRGGGBBB palette colour. Twice a hi-res line.
__attribute__((noinline)) static uint8_t nxZxNearest(uint16_t v) {
    static const uint8_t zxlvl[2] = { 205, 255 };
    const int tr = ((v >> 6) & 7) * 255 / 7, tg = ((v >> 3) & 7) * 255 / 7, tb = (v & 7) * 255 / 7;
    uint32_t bestd = 0xFFFFFFFFu; uint8_t best = 0;
    for (int c = 0; c < 16; c++) {
        const int l = zxlvl[(c >> 3) & 1];
        const int dr = tr - ((c & 2) ? l : 0), dg = tg - ((c & 4) ? l : 0), db = tb - ((c & 1) ? l : 0);
        const uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 6 + db * db);
        if (d < bestd) { bestd = d; best = (uint8_t)c; }
    }
    return best;
}

// Timex hi-res (port FF mode 1xx, zxula.vhd border_clr_tmx): the ULA's
// attribute is fixed at 01 & ~ink & ink — BRIGHT, paper = the inverse of the
// ink in port FF 5:3 — and the border takes that paper colour too. Ink / paper
// go through whatever palette the ULA uses (classic, ULANext, ULA+) like any
// attribute. Flash, with nxHiresUlaRow below: hi-res is rare on the Next and
// the .nxovl window is full (2026-10-12).
// Ink, paper and border carry NX_TR like any ULA pixel (colour == NR 0x14):
// a transparent hi-res paper shows the layers under the ULA at half-pixel
// width (LmxHiRes). zxula.vhd 483-560 for the three palette paths:
//   ULANext  ink = attr & format; paper = 128 + attr >> shift (format 2^n-1,
//            else the fallback); BORDER = 128 + attr(5:3), i.e. the paper
//            WITHOUT the bright bit (the test's "core 2.00.25+ shows [130]")
//   ULA+     "11" & attr(7:6) & 1: hi-res puts the INK in the paper group too
//   classic  ink 8|ink, paper = border = 16 + 8|paper
__attribute__((noinline)) static void nxHiresInkPaper(const NextLineState* ls, const uint16_t* raw,
                                                      uint8_t mask, uint16_t fallback, bool ulaNext,
                                                      bool ulap, uint8_t tr, uint16_t& hrInk,
                                                      uint16_t& hrPap, uint16_t& hrBrd) {
    const uint8_t ink = (ls->r69 >> 3) & 7, pap = (uint8_t)(~ink & 7);
    const uint8_t a = (uint8_t)(0x40 | (pap << 3) | ink);
    if (ulaNext) {
        uint8_t shift = 0;
        for (uint8_t m = mask; m; m >>= 1) shift++;
        const bool bgnd = (mask & (uint8_t)(mask + 1)) != 0 || mask == 0xFF || mask == 0;
        hrInk = nxRgb(raw[a & mask], tr);
        hrPap = bgnd ? (uint16_t)(fallback & 0x1FF) : nxRgb(raw[128 + (a >> shift)], tr);
        hrBrd = mask == 0xFF ? (uint16_t)(fallback & 0x1FF) : nxRgb(raw[128 + pap], tr);
    } else if (ulap) {
        hrInk = nxRgb(raw[0xD8 | ink], tr);
        hrPap = hrBrd = nxRgb(raw[0xD8 | pap], tr);
    } else {
        hrInk = nxRgb(raw[8 | ink], tr);
        hrPap = hrBrd = nxRgb(raw[16 + (8 | pap)], tr);
    }
}
// Timex hi-res 512x192 (zxula.vhd vram_a, shift_reg_32): each 16-pixel column
// is the "pixel" byte (left 8) then the "attribute" byte (right 8), fetched per
// mode bits 1:0 — mode 6: 0x4000 / 0x6000 pixels (the usual hi-res), 7: 0x6000
// twice, 4: 0x4000 pixels / 0x5800 attributes, 5: 0x6000 pixels / 0x7800
// attributes. One lores pixel = two hi-res halves, each ink or paper, each
// opaque or transparent (NX_TR on hrInk / hrPap):
//   both opaque       -> NX_SLOT | HIRES_MARK, hiresRow = the pair slot
//   both transparent  -> NX_TR (the mix shows the layers under the ULA)
//   one opaque        -> NX_SLOT | HIRES_MARK2, hiresRow = which half + its
//                        ZX colour; the other half is resolved after the mix
//                        (nxHiresUnder) — a sprite over the ULA still covers both.
__attribute__((noinline)) static void nxHiresUlaRow(const NextLineState* ls, uint16_t* p, int sy,
                                                    uint16_t hrInk, uint16_t hrPap, uint8_t* hiresRow) {
    const uint8_t m = ls->r69 & 7;
    const bool iOp = !(hrInk & NX_TR), pOp = !(hrPap & NX_TR);
    const uint8_t ci = iOp ? nxZxNearest(hrInk) : 0, cp = pOp ? nxZxNearest(hrPap) : 0;
    uint16_t pv[4]; uint8_t hv[4];               // per 2-bit pattern: nxU value, hiresRow byte
    for (int q = 0; q < 4; q++) {
        const bool lOp = (q & 2) ? iOp : pOp, rOp = (q & 1) ? iOp : pOp;
        const uint8_t lc = (q & 2) ? ci : cp, rc = (q & 1) ? ci : cp;
        if (lOp && rOp)       { pv[q] = NX_SLOT | HIRES_MARK;  hv[q] = VIDEO::profi_pair_lookup[lc][rc]; }
        else if (!lOp && !rOp){ pv[q] = NX_TR;                 hv[q] = 0; }
        else                  { pv[q] = NX_SLOT | HIRES_MARK2; hv[q] = (uint8_t)(lOp ? 0x80 | lc : rc); }
    }
    const uint8_t* lb = ls->grmem + ((m & 1) << 13) + VIDEO::offBmp[sy];
    const uint8_t* rb = (m & 2) ? ls->grmem + 0x2000 + VIDEO::offBmp[sy]
                                : ls->grmem + ((m & 1) << 13) + VIDEO::offAtt[sy];
    for (int col = 0; col < 32; col++) {
        const uint8_t e = lb[col], o = rb[col];
        uint16_t* pp = p + col * 8; uint8_t* hp = hiresRow + col * 8;
        for (int k = 0; k < 4; k++) { const uint8_t q = (e >> (6 - 2 * k)) & 3; pp[k] = pv[q]; hp[k] = hv[q]; }
        for (int k = 0; k < 4; k++) { const uint8_t q = (o >> (6 - 2 * k)) & 3; pp[4 + k] = pv[q]; hp[4 + k] = hv[q]; }
    }
}
// A hi-res line while the frame is NOT a pair frame (the first frame after
// the switch, a driver that refused, SOFTTV/TFT): the classic 256-pixel
// OR-merge — ink where either half is ink. Readable text, no pair slots.
__attribute__((noinline)) static void nxHiresUlaRowMerge(const NextLineState* ls, uint16_t* p, int sy,
                                                         uint16_t hrInk, uint16_t hrPap) {
    const uint8_t m = ls->r69 & 7;
    const uint8_t* lb = ls->grmem + ((m & 1) << 13) + VIDEO::offBmp[sy];
    const uint8_t* rb = (m & 2) ? ls->grmem + 0x2000 + VIDEO::offBmp[sy]
                                : ls->grmem + ((m & 1) << 13) + VIDEO::offAtt[sy];
    for (int col = 0; col < 32; col++) {
        const uint8_t e = lb[col], o = rb[col];
        uint16_t* pp = p + col * 8;
        for (int k = 0; k < 4; k++) pp[k]     = ((e >> (6 - 2 * k)) & 3) ? hrInk : hrPap;
        for (int k = 0; k < 4; k++) pp[4 + k] = ((o >> (6 - 2 * k)) & 3) ? hrInk : hrPap;
    }
}

// Returns false when the whole row (border included) is transparent; nxU is
// then just filled transparent and the mixer may leave the ULA out.
IRAM_ATTR bool NEXTVID::RenderUlaLine(int row) {
    const int xres = VIDEO::vga.xres;
    uint16_t* const u = nxU;
    const uint16_t* raw = rawPal[(LS->palCtrl & 0x02) ? 4 : 0];
    const uint8_t tr = LS->r14;
    const bool ulaNext = LS->palCtrl & 0x01;
    const uint8_t mask = LS->r42;
    const uint16_t fallback = rgb332_to_333(LS->r4A);
    // border colours 0..7: ULANext paper base 128 (format 0xFF selects the
    // fallback colour, ula_select_bgnd), classic ULA the paper entries 16..23
    // ULA+ (NR 0x68 bit 3 = port FF3B mode 01, zxula.vhd 531-540): index
    // 11 & attr 7:6 & paper & colour — the border is attr 00, paper: 192+8+c.
    const bool ulap = !ulaNext && (LS->r68 & 0x08);
    uint16_t bc[8];
    for (int c = 0; c < 8; c++)
        bc[c] = (uint16_t)(ulaNext ? (mask == 0xFF ? fallback : nxRgb(raw[128 + c], tr))
                                   : nxRgb(raw[(ulap ? 200 : 16) + c], tr));
    for (int c = 0; c < 8; c++) if (!(bc[c] & NX_TR)) bc[c] |= NX_BRD;
    // Timex hi-res (port FF mode 1xx, zxula.vhd border_clr_tmx): the ULA's
    // attribute is fixed at 01 & ~ink & ink — BRIGHT, paper = the inverse of
    // the ink in port FF 5:3 — and the border takes that paper colour too.
    // Ink / paper go through whatever palette the ULA uses (classic, ULANext,
    // ULA+) like any attribute.
    // Per LINE (the copper sets NR 0x69 per region): hrMode = this line is a
    // hi-res one; hr = ... and the frame runs the packed-pair driver (the 512
    // pixels are exact); otherwise the line takes the 256-pixel OR-merge.
    const bool hrMode = (LS->r69 & 0x04) && !(LS->r69 & 0x40);
    const bool hr = hrMode && LS->hires == 1;
    uint16_t hrInk = 0, hrPap = 0, hrBrd = 0;
    if (hrMode) nxHiresInkPaper(LS, raw, mask, fallback, ulaNext, ulap, tr, hrInk, hrPap, hrBrd);
    const uint16_t borderV = hrMode ? ((hrBrd & NX_TR) ? NX_TR : (uint16_t)(hrBrd | NX_BRD))
                                   : bc[LS->border & 7];
    const int y = row - paperTopRow;

    // A row that is transparent end to end (NXModPlayer: black screen, black
    // border, NR 0x14 = 0 under its text tilemap and Layer 2 — 3.3 ms of core1
    // a frame for nothing, hw 2026-10-11): classic ULA only, the check is the
    // attribute pass without the 256 stores.
    bool brdTr = (borderV & NX_TR) != 0;
    if (LS->brd.n && !hrMode) for (int c = 0; c < 8; c++) brdTr = brdTr && (bc[c] & NX_TR);
    if (brdTr && !hrMode && !ulaNext && !ulap && !(LS->r15 & 0x80) && !(LS->gigs && gigsPrev)) {
        bool op = false;
        if (y >= 0 && y < 192 && !VIDEO::paper_off) {
            const int sy = LS->r27 ? (y + LS->r27) % 192 : y;
            const uint8_t tm = (LS->r69 & 0x40) ? 0 : (LS->r69 & 0x03);
            const uint16_t pOff = VIDEO::offBmp[sy];
            const uint8_t* bmp = (LS->bmpOvr && sy == y) ? s_bmpSh + y * 32 : LS->grmem + pOff + ((tm & 1) << 13);
            const uint8_t* att = (LS->attOvr && sy == y) ? LS->attr
                               : LS->grmem + ((tm & 2) ? 0x2000 + pOff
                                                       : VIDEO::offAtt[sy] + ((tm & 1) << 13));
            for (int col = 0; col < 32 && !op; col++) {
                const uint8_t a = att[col];
                const uint8_t b = bmp[col] ^ (uint8_t)(-((a & LS->flashing) >> 7));
                const uint8_t bright = (a & 0x40) >> 3;
                if (b && !(nxRgb(raw[(a & 7) | bright], tr) & NX_TR)) op = true;
                else if (b != 0xFF && !(nxRgb(raw[16 + (((a >> 3) & 7) | bright)], tr) & NX_TR)) op = true;
            }
        }
        if (!op) { nxFillTr(u, xres); return false; }
    }

    if (LS->brd.n && !hrMode) nxPaintBorderRuns(u, xres, bc, borderV);
    else for (int x = 0; x < xres; x++) u[x] = borderV;
    if (LS->gigs && s_gigsPrevBrd && !hrMode && !ulaNext && !ulap && row >= 0 && row < (int)VIDEO::vga.yres)
        nxBorderGigs(u, xres, row, y >= 0 && y < 192 && !VIDEO::paper_off, xPaperOff, gigsBlend);

    if (y < 0 || y >= 192 || VIDEO::paper_off) return true;
    uint16_t* p = u + xPaperOff;
    // ULA scroll (NR 0x26/0x27, zxula.vhd 192-206): the memory row is
    // (y + scroll Y) mod 192, the columns start scroll X pixels in and wrap
    // at 256 (rotated below, after the row is drawn). NR 0x68 bit 2 (fine
    // scroll, one more 14 MHz half-pixel) has no lores equivalent: ignored.
    // LoRes keeps its own scroll registers (NR 0x32/0x33).
    const int sy = LS->r27 ? (y + LS->r27) % 192 : y;

    if (LS->r15 & 0x80) {          // LoRes 128x96 replaces the paper (not ULA-clipped)
        // lores.vhd: rows wrap at 192, columns at 256 (scroll NR 0x32/0x33).
        // Standard: one byte a pixel, rows of 128 in two halves (0x0000 /
        // 0x2000), index = (high nibble + NR 0x6A offset) & low nibble.
        // Radastan (NR 0x6A bit 5): 4bpp, 64 bytes a row, display file =
        // port FF bit 0 XOR NR 0x6A bit 4 (+0x2000), index = offset (or 11 &
        // offset 1:0 with ULA+, NR 0x68 bit 3) & the pixel nibble.
        const uint8_t ly = (uint8_t)((y + LS->r33) % 192);
        const uint8_t sxl = LS->r32, ofs = LS->r6A & 0x0F;
        if (LS->r6A & 0x20) {
            const uint8_t hiN = (uint8_t)(((LS->r68 & 0x08) ? (0x0C | (ofs & 3)) : ofs) << 4);
            const bool df = ((LS->r69 ^ (LS->r6A >> 4)) & 1) != 0;
            const uint8_t* base = LS->grmem + (df ? 0x2000 : 0) + (ly >> 1) * 64;
            for (int x = 0; x < 256; x++) {
                const uint8_t xx = (uint8_t)(x + sxl);
                const uint8_t b = base[xx >> 2];
                p[x] = nxRgb(raw[hiN | ((xx & 2) ? (b & 0x0F) : (b >> 4))], tr);
            }
        } else {
            const uint8_t ry = ly >> 1;
            const uint8_t* base = LS->grmem +
                (ry < 48 ? ry * 128 : 0x2000 + (uint16_t)(ry - 48) * 128);
            nxLoResStd(p, base, sxl, ofs, raw, tr);
        }
        return true;
    }
    // Timex screen mode = port FF / NR 0x69 bits 2:0, forced to 000 while the
    // shadow screen (bank 7, NR 0x69 bit 6 = port 7FFD bit 3) is displayed —
    // zxula.vhd: "limit timex modes to bank 5 as bank 7 only has 8k bram".
    if (hr) {
        nxHiresUlaRow(LS, p, sy, hrInk, hrPap, hiresRow);
    } else if (hrMode) {
        nxHiresUlaRowMerge(LS, p, sy, hrInk, hrPap);
    } else {
        // zxula.vhd vram_a: pixel = mode(0) & pixel address, attribute =
        // '1' & PIXEL address when mode(1) (hi-colour 8x1 attributes at
        // 0x6000), else mode(0) & attribute address.
        const uint8_t tm = (LS->r69 & 0x40) ? 0 : (LS->r69 & 0x03);
        const uint16_t pOff = VIDEO::offBmp[sy];
        const uint8_t* bmp = (LS->bmpOvr && sy == y) ? s_bmpSh + y * 32 : LS->grmem + pOff + ((tm & 1) << 13);
        const uint8_t* att = (LS->attOvr && sy == y) ? LS->attr
                           : LS->grmem + ((tm & 2) ? 0x2000 + pOff
                                                   : VIDEO::offAtt[sy] + ((tm & 1) << 13));
        if (ulaNext) {
            // ink = attr & format; paper = 128 + the bits above the format;
            // a format that is not 2^n-1 (or 0xFF) gives the fallback paper
            uint8_t shift = 0;
            for (uint8_t m = mask; m; m >>= 1) shift++;
            const bool bgnd = (mask & (uint8_t)(mask + 1)) != 0 || mask == 0xFF || mask == 0;
            for (int col = 0; col < 32; col++) {
                const uint8_t a = att[col], b = bmp[col];
                const uint16_t ink = nxRgb(raw[a & mask], tr);
                const uint16_t pap = bgnd ? fallback : nxRgb(raw[128 + (a >> shift)], tr);
                for (int bit = 0; bit < 8; bit++) p[bit] = (b & (0x80 >> bit)) ? ink : pap;
                p += 8;
            }
        } else if (ulap) {
            // no FLASH, no BRIGHT: attr 7:6 picks one of four 16-entry groups
            for (int col = 0; col < 32; col++) {
                const uint8_t a = att[col], b = bmp[col];
                const uint8_t grp = (uint8_t)(0xC0 | ((a >> 6) << 4));
                const uint16_t ink = nxRgb(raw[grp | (a & 7)], tr);
                const uint16_t pap = nxRgb(raw[grp | 8 | ((a >> 3) & 7)], tr);
                for (int bit = 0; bit < 8; bit++) p[bit] = (b & (0x80 >> bit)) ? ink : pap;
                p += 8;
            }
        } else if (LS->gigs && gigsPrev) {
            // Gigascreen: blended only when the classic ULA is all there is on
            // the line (the row is still recorded so the next frame has a prev)
            const bool blend = LS->gigs == 1 && !(LS->r15 & 0x01) && !(LS->r6B & 0x80) &&
                               !((LS->p123B & 0x02) || (LS->r69 & 0x80)) && !(LS->r68 & 0x80);
            uint32_t* gp = (uint32_t*)(gigsPrev + sy * 128);
            for (int col = 0; col < 32; col++) {
                const uint8_t a = att[col];
                const uint8_t b = bmp[col] ^ (uint8_t)(-((a & LS->flashing) >> 7));
                const uint8_t bright = (a & 0x40) >> 3;
                const uint8_t ci = (a & 7) | bright, cp = ((a >> 3) & 7) | bright;
                const uint16_t ink = nxRgb(raw[ci], tr), pap = nxRgb(raw[16 + cp], tr);
                const uint32_t prev = gp[col];
                uint32_t cur = 0;
                for (int bit = 0; bit < 8; bit++) {
                    const bool on = b & (0x80 >> bit);
                    const uint8_t c = on ? ci : cp;
                    cur |= (uint32_t)c << (bit * 4);
                    p[bit] = blend ? (uint16_t)(NX_SLOT | gigsBlend[((prev >> (bit * 4)) & 15) << 4 | c])
                                   : (on ? ink : pap);
                }
                gp[col] = cur;
                p += 8;
            }
        } else {
            for (int col = 0; col < 32; col++) {
                const uint8_t a = att[col];
                const uint8_t b = bmp[col] ^ (uint8_t)(-((a & LS->flashing) >> 7));
                const uint8_t bright = (a & 0x40) >> 3;
                const uint16_t ink = nxRgb(raw[(a & 7) | bright], tr);
                const uint16_t pap = nxRgb(raw[16 + (((a >> 3) & 7) | bright)], tr);
                p[0] = (b & 0x80) ? ink : pap;
                p[1] = (b & 0x40) ? ink : pap;
                p[2] = (b & 0x20) ? ink : pap;
                p[3] = (b & 0x10) ? ink : pap;
                p[4] = (b & 0x08) ? ink : pap;
                p[5] = (b & 0x04) ? ink : pap;
                p[6] = (b & 0x02) ? ink : pap;
                p[7] = (b & 0x01) ? ink : pap;
                p += 8;
            }
        }
    }
    if (LS->r26 && !hrMode) nxRotL(u + xPaperOff, LS->r26);
    // ULA clip window (NR 0x1A, zxula.vhd 562): paper only, never the border
    const uint8_t* cw = LS->clip[2];
    if (y < cw[2] || y > cw[3]) { nxFillTr(u + xPaperOff, 256); return true; }
    for (int x = 0; x < cw[0]; x++) u[xPaperOff + x] = NX_TR;
    for (int x = cw[1] + 1; x < 256; x++) u[xPaperOff + x] = NX_TR;
    return true;
}

// LoRes standard mode, one paper row: output pixel x shows byte
// base[(uint8_t)(x + sxl) >> 1]. Each byte covers two output pixels — its
// colour is computed once (TX-1696: 3 ms of core1 a frame at -Os, hw 2026-10-09).
static IRAM_ATTR __attribute__((noinline, optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
void nxLoResStd(uint16_t* p, const uint8_t* base, uint8_t sxl, uint8_t ofs,
                const uint16_t* raw, uint8_t tr) {
    const uint8_t o4 = (uint8_t)(ofs << 4);
    int x = 0;
    if (sxl & 1) {                       // odd scroll: the first byte shows one pixel
        const uint8_t b = base[sxl >> 1];
        p[0] = nxRgb(raw[(uint8_t)(((b + o4) & 0xF0) | (b & 0x0F))], tr);
        x = 1;
    }
    for (; x + 1 < 256; x += 2) {
        const uint8_t b = base[(uint8_t)(x + sxl) >> 1];
        const uint16_t v = nxRgb(raw[(uint8_t)(((b + o4) & 0xF0) | (b & 0x0F))], tr);
        p[x] = v; p[x + 1] = v;
    }
    if (x < 256) {                       // odd scroll: the last pixel
        const uint8_t b = base[(uint8_t)(x + sxl) >> 1];
        p[x] = nxRgb(raw[(uint8_t)(((b + o4) & 0xF0) | (b & 0x0F))], tr);
    }
}

// Mixer state for one line (constant across the row)
struct NxMix { uint8_t* fb; const uint8_t* remap; uint16_t fb9; bool ulaEn, stencil, tm, l2, spr; uint8_t blend; };

static inline __attribute__((always_inline)) uint8_t nxOut(const NxMix& m, uint16_t v) {
    return (v & NX_SLOT) ? (uint8_t)v : m.remap[v & 0x1FF];
}
// ula_final (zxnext.vhd 7112-7136): ULA and tilemap merged, or the stencil
static inline __attribute__((always_inline)) uint16_t nxUlaFinal(const NxMix& m, uint16_t u, uint16_t t) {
    const bool uTr = (u & NX_TR) || !m.ulaEn, tTr = t & NX_TR;
    if (m.stencil) return (uTr || tTr) ? NX_TR : (uint16_t)(u & t & 0x1FF);
    if (!tTr && (!(t & NX_BELOW) || uTr)) return (uint16_t)(t & 0x1FF);
    return uTr ? NX_TR : u;
}

// ula_final in place, for a line with BOTH the ULA and the tilemap on: nxU[x]
// becomes the merged ULA/tilemap pixel. NX_BRD survives only where orders 3-5
// look at it (a ULA border pixel with the tilemap transparent there), so the
// mixer below reads ONE buffer for the pair. With only one of the two on, the
// mixer reads that layer's buffer directly and this pass does not run.
static IRAM_ATTR __attribute__((noinline, optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
void nxUlaMerge(bool stencil, int xres) {
    uint16_t* __restrict u = nxU;
    const uint16_t* __restrict t = nxT;
    for (int x = 0; x < xres; x++) {
        const uint16_t uv = u[x], tv = t[x];
        const bool uTr = uv & NX_TR, tTr = tv & NX_TR;
        uint16_t r;
        if (stencil) r = (uTr || tTr) ? NX_TR : (uint16_t)(uv & tv & 0x1FF);
        else if (!tTr && (!(tv & NX_BELOW) || uTr)) r = (uint16_t)(tv & 0x1FF);
        else r = uTr ? NX_TR : (tTr ? uv : (uint16_t)(uv & ~NX_BRD));
        u[x] = r;
    }
}

// Layer orders 0..5 (NR 0x15 bits 4:2): S L U / L S U / S U L / L U S / U S L / U L S.
// One loop per (order, Layer 2 on/off); the order is decided once per line,
// not per pixel (the per-pixel switch at -Os was 15 of Warhawk's 24 ms of core1
// per frame, hw 2026-10-08). pu = ula_final for the line (nxUlaMerge's result,
// the ULA alone, or the tilemap alone). __restrict + locals: with the old
// NxMix& argument every fb byte store forced a reload of the per-line flags
// (Santa: mix = 8.9 of 24 ms of core1, hw 2026-10-09).
// MERGE: ULA and tilemap both on (no stencil) — ula_final computed inline from
// nxU / nxT instead of a separate nxUlaMerge pass over the line (NXModPlayer:
// mix 9.5 ms of core1 a frame, hw 2026-10-10). pu is ignored then.
template <int MODE, bool L2, bool MERGE = false>
static IRAM_ATTR __attribute__((noinline, optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
void nxMixT(uint8_t* __restrict fb, const uint16_t* __restrict pu, const uint8_t* __restrict remap,
            uint16_t fb9, int xres, int x0 = 0) {
    const uint16_t* __restrict pl = nxL;
    const uint16_t* __restrict ps = nxS;
    const uint16_t* __restrict pt = nxT;
    if (MERGE) pu = nxU;
    for (int x = x0; x < xres; x++) {
        const uint16_t l = L2 ? pl[x] : NX_TR, s = ps[x];
        uint16_t uf = pu[x];
        if (MERGE) {
            const uint16_t tv = pt[x];
            const bool uTr0 = uf & NX_TR, tTr0 = tv & NX_TR;
            if (!tTr0 && (!(tv & NX_BELOW) || uTr0)) uf = (uint16_t)(tv & 0x1FF);
            else if (!uTr0 && !tTr0) uf = (uint16_t)(uf & ~NX_BRD);
        }
        const bool lTr = l & NX_TR, sTr = s & NX_TR, uTr = uf & NX_TR;
        const bool lPrio = (l & (NX_TR | NX_PRIO)) == NX_PRIO;
        uint16_t v = fb9;
        if (MODE == 0)      { if (lPrio) v = l; else if (!sTr) v = s; else if (!lTr) v = l; else if (!uTr) v = uf; }
        else if (MODE == 1) { if (!lTr) v = l; else if (!sTr) v = s; else if (!uTr) v = uf; }
        else if (MODE == 2) { if (lPrio) v = l; else if (!sTr) v = s; else if (!uTr) v = uf; else if (!lTr) v = l; }
        else {
            // orders 3-5: a ULA BORDER pixel gives way to a sprite where the tilemap is transparent
            const bool uOk = !uTr && !((uf & NX_BRD) && !sTr);
            if (MODE == 3)      { if (!lTr) v = l; else if (uOk) v = uf; else if (!sTr) v = s; }
            else if (MODE == 4) { if (lPrio) v = l; else if (uOk) v = uf; else if (!sTr) v = s; else if (!lTr) v = l; }
            else                { if (lPrio) v = l; else if (uOk) v = uf; else if (!lTr) v = l; else if (!sTr) v = s; }
        }
        fb[x] = (v & NX_SLOT) ? (uint8_t)v : remap[v & 0x1FF];
    }
}

// Neither Layer 2 nor a sprite has an opaque pixel on this row: every order 0-5
// reduces to "ula_final, else the fallback colour" (the border give-way of
// orders 3-5 needs an opaque sprite).
template <bool MERGE>
static IRAM_ATTR __attribute__((noinline, optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
void nxMixU(uint8_t* __restrict fb, const uint16_t* __restrict pu, const uint8_t* __restrict remap,
            uint16_t fb9, int xres, int x0 = 0) {
    const uint16_t* __restrict pt = nxT;
    if (MERGE) pu = nxU;
    for (int x = x0; x < xres; x++) {
        uint16_t uf = pu[x];
        if (MERGE) {
            const uint16_t tv = pt[x];
            if (!(tv & NX_TR) && (!(tv & NX_BELOW) || (uf & NX_TR))) uf = (uint16_t)(tv & 0x1FF);
        }
        const uint16_t v = (uf & NX_TR) ? fb9 : uf;
        fb[x] = (v & NX_SLOT) ? (uint8_t)v : remap[v & 0x1FF];
    }
}

#if NEXT_TRACE
volatile uint32_t g_nx_mix_span = 0, g_nx_mix_spanRows = 0;   // sprite-span rows and their pixels
#endif

// Layer 2 on, no opaque sprite pixel (a whole row, or outside its sprite span):
// the six orders collapse to two. Without a sprite, orders 3-5's border give-way
// never applies, so: UOVER = false (0 S L U, 1 L S U, 3 L U S) Layer 2 over
// ula_final; UOVER = true (2 S U L, 4 U S L, 5 U L S) ula_final over Layer 2,
// except a Layer 2 priority pixel. Same results as nxMixT with every sprite
// pixel transparent, without reading the sprite line (Atic Atac Remake intro:
// L2 + tilemap + sprites on every row, mix 8.2 ms of render a frame,
// hw 2026-10-19).
template <bool UOVER, bool MERGE>
static IRAM_ATTR __attribute__((noinline, optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
void nxMixL(uint8_t* __restrict fb, const uint16_t* __restrict pu, const uint8_t* __restrict remap,
            uint16_t fb9, int xres, int x0) {
    const uint16_t* __restrict pl = nxL;
    const uint16_t* __restrict pt = nxT;
    if (MERGE) pu = nxU;
    for (int x = x0; x < xres; x++) {
        const uint16_t l = pl[x];
        uint16_t uf = pu[x];
        if (MERGE) {
            const uint16_t tv = pt[x];
            const bool uTr0 = uf & NX_TR, tTr0 = tv & NX_TR;
            if (!tTr0 && (!(tv & NX_BELOW) || uTr0)) uf = (uint16_t)(tv & 0x1FF);
            else if (!uTr0 && !tTr0) uf = (uint16_t)(uf & ~NX_BRD);
        }
        uint16_t v;
        if (UOVER) v = ((l & (NX_TR | NX_PRIO)) == NX_PRIO) ? l : !(uf & NX_TR) ? uf : !(l & NX_TR) ? l : fb9;
        else       v = !(l & NX_TR) ? l : !(uf & NX_TR) ? uf : fb9;
        fb[x] = (v & NX_SLOT) ? (uint8_t)v : remap[v & 0x1FF];
    }
}

// One table per mixer family (flash, read once a row): every caller goes
// through a pointer, so each loop exists once with its x0 argument instead of a
// constprop clone per call site — the dispatch below was three 6-way switches.
typedef void (*NxMixFn)(uint8_t*, const uint16_t*, const uint8_t*, uint16_t, int, int);
// Only the Layer 2 variants of nxMixT: nxL is filled NX_TR on every row
// (RenderLayers), so with no opaque Layer 2 pixel they give exactly what the
// L2 = false loops gave — one more load a pixel on sprite rows without Layer 2
// (inside the sprite span only), for 12 loops (~1.9 KB) out of the full window.
static const NxMixFn kNxMixT[2][6] = {   // [MERGE][order]
    { nxMixT<0, true, false>, nxMixT<1, true, false>, nxMixT<2, true, false>,
      nxMixT<3, true, false>, nxMixT<4, true, false>, nxMixT<5, true, false> },
    { nxMixT<0, true, true>, nxMixT<1, true, true>, nxMixT<2, true, true>,
      nxMixT<3, true, true>, nxMixT<4, true, true>, nxMixT<5, true, true> },
};
// No opaque sprite pixel: [L2][MERGE][ula_final over Layer 2]
static const NxMixFn kNxMixNoSpr[2][2][2] = {
    { { nxMixU<false>, nxMixU<false> }, { nxMixU<true>, nxMixU<true> } },
    { { nxMixL<false, false>, nxMixL<true, false> }, { nxMixL<false, true>, nxMixL<true, true> } },
};

static IRAM_ATTR void nxMixOrder(const NxMix& m, int xres, int mode, bool span = false) {
    const bool l2 = m.l2;
    if (!l2 && !m.spr) {
        const uint16_t* pu;
        if (m.ulaEn && m.tm && !m.stencil) { nxMixU<true>(m.fb, nxU, m.remap, m.fb9, xres); return; }
        if (m.ulaEn && m.tm) { nxUlaMerge(m.stencil, xres); pu = nxU; }
        else pu = m.ulaEn ? nxU : nxT;
        nxMixU<false>(m.fb, pu, m.remap, m.fb9, xres);
        return;
    }
    // ula_final: inline in the loop (MERGE), a merge pass under the stencil, or one layer
    const bool merge = m.ulaEn && m.tm && !m.stencil;
    const uint16_t* pu;
    if (merge) pu = nxU;
    else if (m.ulaEn && m.tm) { nxUlaMerge(m.stencil, xres); pu = nxU; }
    else pu = m.ulaEn ? nxU : nxT;           // ULA off: tilemap (filled transparent when off)
    const bool uover = mode == 2 || mode == 4 || mode == 5;
    const NxMixFn rest = kNxMixNoSpr[l2][merge][uover];
    if (!m.spr) { rest(m.fb, pu, m.remap, m.fb9, xres, 0); return; }
    const NxMixFn full = kNxMixT[merge][mode];
    if (span) {
        // resolve the sprite layer only over its opaque span (TX-1696 at 576p:
        // ~100 sprite rows a frame without Layer 2; Atic Atac with it)
        int s0 = nxSprX0, s1 = nxSprX1;
        if (s0 < 0) s0 = 0;
        if (s1 > xres) s1 = xres;
        if (s0 < s1) {
#if NEXT_TRACE
            g_nx_mix_span = g_nx_mix_span + (uint32_t)(s1 - s0);
            g_nx_mix_spanRows = g_nx_mix_spanRows + 1;
#endif
            if (s0 > 0) rest(m.fb, pu, m.remap, m.fb9, s0, 0);
            full(m.fb, pu, m.remap, m.fb9, s1, s0);
            if (s1 < xres) rest(m.fb, pu, m.remap, m.fb9, xres, s1);
            return;
        }
    }
    full(m.fb, pu, m.remap, m.fb9, xres, 0);
}

// Orders 6/7: Layer 2 + the blend source (NR 0x68 bits 6:5), saturated (6) or
// minus 5 (7). One SRAM loop per blend source, the NxMix fields in locals: from
// flash with the per-pixel switch it was 17 of NextSID's 26 ms of core1 a frame
// (6.4 M XIP accesses, hw 2026-10-10).
template <int BLEND>
static IRAM_ATTR __attribute__((noinline, optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
void nxMixBlendT(uint8_t* __restrict fb, const uint8_t* __restrict remap, uint16_t fb9,
                 bool ulaEn, bool stencil, int xres, bool minus5) {
    const uint16_t* __restrict pu = nxU;
    const uint16_t* __restrict pt = nxT;
    const uint16_t* __restrict pl = nxL;
    const uint16_t* __restrict ps = nxS;
    for (int x = 0; x < xres; x++) {
        const uint16_t u = pu[x], t = pt[x], l = pl[x], s = ps[x];
        const bool uMixTr = u & NX_TR, uTr = uMixTr || !ulaEn, tTr = t & NX_TR, below = t & NX_BELOW;
        uint16_t mix, top = NX_TR, bot = NX_TR;
        if (BLEND == 0) {
            mix = uMixTr ? NX_TR : u;
            if (!tTr) { if (below) bot = t; else top = t; }
        } else if (BLEND == 2) {
            // ula_final (nxUlaFinal)
            if (stencil) mix = (uTr || tTr) ? NX_TR : (uint16_t)(u & t & 0x1FF);
            else if (!tTr && (!below || uTr)) mix = (uint16_t)(t & 0x1FF);
            else mix = uTr ? NX_TR : u;
        } else if (BLEND == 3) {
            mix = tTr ? NX_TR : t;
            if (!uTr) { if (below) top = u; else bot = u; }
        } else {
            mix = NX_TR;
            if (below) { top = uTr ? NX_TR : u; bot = tTr ? NX_TR : t; }
            else       { top = tTr ? NX_TR : t; bot = uTr ? NX_TR : u; }
        }
        const bool lTr = l & NX_TR, sTr = s & NX_TR;
        // The blended colour is needed only where Layer 2 is opaque, and with
        // the blend source transparent it is Layer 2's own colour (saturate:
        // l + 0; minus 5 applies only with both opaque). Computed lazily: the
        // per-pixel channel split + clamp was most of the loop (NextSID: mix
        // 13 of core1's 23 ms a frame, hw 2026-10-10).
        uint16_t v;
        const bool lFirst = !lTr && (l & NX_PRIO);
        if (lFirst || (!lTr && (top & NX_TR) && sTr && (bot & NX_TR))) {
            if (mix & NX_TR) {
                v = (uint16_t)(l & 0x1FF);
            } else {
                int r = ((l >> 6) & 7) + ((mix >> 6) & 7), g = ((l >> 3) & 7) + ((mix >> 3) & 7), b = (l & 7) + (mix & 7);
                if (!minus5) { if (r > 7) r = 7; if (g > 7) g = 7; if (b > 7) b = 7; }
                else {
                    r = r <= 4 ? 0 : r >= 12 ? 7 : r - 5;
                    g = g <= 4 ? 0 : g >= 12 ? 7 : g - 5;
                    b = b <= 4 ? 0 : b >= 12 ? 7 : b - 5;
                }
                v = (uint16_t)((r << 6) | (g << 3) | b);
            }
        }
        else if (!(top & NX_TR)) v = top;
        else if (!sTr) v = s;
        else if (!(bot & NX_TR)) v = bot;
        else v = fb9;
        fb[x] = (v & NX_SLOT) ? (uint8_t)v : remap[v & 0x1FF];
    }
}

static IRAM_ATTR void nxMixBlend(const NxMix& m, int xres, bool minus5) {
    switch (m.blend) {
        case 0:  nxMixBlendT<0>(m.fb, m.remap, m.fb9, m.ulaEn, m.stencil, xres, minus5); break;
        case 2:  nxMixBlendT<2>(m.fb, m.remap, m.fb9, m.ulaEn, m.stencil, xres, minus5); break;
        case 3:  nxMixBlendT<3>(m.fb, m.remap, m.fb9, m.ulaEn, m.stencil, xres, minus5); break;
        default: nxMixBlendT<1>(m.fb, m.remap, m.fb9, m.ulaEn, m.stencil, xres, minus5); break;
    }
}

#if NEXT_TRACE
// NEXT mix: which rows take the full mixer (Layer 2 / sprite opaque on the
// row) and how wide the 640 frame's second pass is. Flash: one call a row.
volatile uint32_t g_nx_mix[5] = {};   // rows: l2, spr, either, p2; p2 pixels
// NEXT spr: resolves a frame and the attribute span each covered, sprites
// tested per row (the visible list) and sprites drawn per row.
volatile uint32_t g_nx_spr[4] = {};
__attribute__((noinline)) static void nxSprNote(int k, uint32_t n) { g_nx_spr[k] = g_nx_spr[k] + n; }
__attribute__((noinline)) static void nxMixNote(bool l2, bool spr, bool p2, int p2px) {
    if (l2) g_nx_mix[0] = g_nx_mix[0] + 1;
    if (spr) g_nx_mix[1] = g_nx_mix[1] + 1;
    if (l2 || spr) g_nx_mix[2] = g_nx_mix[2] + 1;
    if (p2) { g_nx_mix[3] = g_nx_mix[3] + 1; g_nx_mix[4] = g_nx_mix[4] + (uint32_t)p2px; }
}
#endif

#if NEXT_TRACE
// Warhawk bottom-stripe probe (trace only, flash — the .nxovl window is full):
// for paper rows 0 and 191, which layers are opaque, the Layer 2 source row and
// a hash of each layer's paper pixels.
__attribute__((noinline)) static void nxRowProbe(int y, int xres, int x0) {
    extern volatile uint32_t g_nx_rowprobe[2][8];
    volatile uint32_t* pr = g_nx_rowprobe[y ? 1 : 0];
    uint32_t nl = 0, ns = 0, nu = 0, hl = 2166136261u, hs = 2166136261u, hu = 2166136261u;
    for (int x = x0; x < x0 + 256 && x < xres; x++) {
        const uint16_t l = nxL[x], sp = nxS[x], u = nxU[x];
        if (!(l & NX_TR)) nl++;
        if (!(sp & NX_TR)) ns++;
        if (!(u & NX_TR)) nu++;
        hl = (hl ^ l) * 16777619u; hs = (hs ^ sp) * 16777619u; hu = (hu ^ u) * 16777619u;
    }
    uint16_t sy = (uint16_t)(y + LS->r17); if (sy >= 192) sy -= 192;
    pr[0] = LS->r17; pr[1] = sy; pr[2] = nl; pr[3] = ns; pr[4] = nu;
    pr[5] = hl; pr[6] = hs; pr[7] = hu;
}
#endif

// Timex hi-res, the transparent half of a HIRES_MARK2 pixel: what the mix
// gives at fb x with the ULA transparent there — nxMixT's order rules over
// the layer lines RenderLayers left (core1 only: core0 never renders hi-res
// rows). The per-line mixer flags are kept in s_hrMix by RenderLayers.
// Orders 6/7 (Layer 2 blends) are approximated by order 1. Flash: rare.
struct NxHrMix { uint16_t fb9; uint8_t order; bool layers, l2, spr, tm, stencil; };
static NxHrMix s_hrMix;
__attribute__((noinline)) static uint16_t nxHiresUnder(int x) {
    const NxHrMix& m = s_hrMix;
    if (!m.layers) return m.fb9;
    const uint16_t t = m.tm ? nxT[x] : NX_TR;
    const uint16_t uf = (m.stencil || (t & NX_TR)) ? NX_TR : (uint16_t)(t & 0x1FF);   // ula_final, ULA transparent
    const uint16_t l = m.l2 ? nxL[x] : NX_TR, s = m.spr ? nxS[x] : NX_TR;
    const bool lTr = l & NX_TR, sTr = s & NX_TR, uTr = uf & NX_TR;
    const bool lPrio = (l & (NX_TR | NX_PRIO)) == NX_PRIO;
    uint16_t v = m.fb9;
    switch (m.order) {
        case 0:  if (lPrio) v = l; else if (!sTr) v = s; else if (!lTr) v = l; else if (!uTr) v = uf; break;
        case 2:  if (lPrio) v = l; else if (!sTr) v = s; else if (!uTr) v = uf; else if (!lTr) v = l; break;
        case 3:  if (!lTr) v = l; else if (!uTr) v = uf; else if (!sTr) v = s; break;
        case 4:  if (lPrio) v = l; else if (!uTr) v = uf; else if (!sTr) v = s; else if (!lTr) v = l; break;
        case 5:  if (lPrio) v = l; else if (!uTr) v = uf; else if (!lTr) v = l; else if (!sTr) v = s; break;
        default: if (!lTr) v = l; else if (!sTr) v = s; else if (!uTr) v = uf; break;   // 1, 6, 7
    }
    return (uint16_t)(v & 0x1FF);
}

IRAM_ATTR void NEXTVID::RenderLayers(uint8_t* fb, int row) {
    const int xres = VIDEO::vga.xres;
    const int y = row - paperTopRow;
    const uint8_t r15 = LS->r15;
    s_rowDiff = false;
    if (LS->hires == 1) { s_hrMix.fb9 = rgb332_to_333(LS->r4A); s_hrMix.layers = false; }
    NX_LT_BEGIN();
    sprApply(LS->sprSeq);               // keep the attribute log moving on every line
    palApply(LS->palSeq);
    NX_LT_END(6);
    // NR 0x68 bit 7: ULA (and LoRes) output off — the mixer then never looks at
    // the ULA layer (ula_final takes the tilemap or transparent), so do not
    // compose it (TX-1696: 3 ms of core1 a frame, hw 2026-10-09). Timex hi-res
    // still needs RenderUlaLine for its pair row.
    bool ulaRow = false;
    if ((LS->r68 & 0x80) && LS->hires != 1) nxFillTr(nxU, xres);
    else ulaRow = RenderUlaLine(row);
    NX_LT_END(0);
    const bool tmEn = LS->r6B & 0x80;
    const bool l2En = (LS->p123B & 0x02) || (LS->r69 & 0x80);
    if (!tmEn && !l2En && !(r15 & 0x01) && !(LS->r68 & 0x80)) {
        // ULA alone (NextZXOS, every 48K/128K title): the order is moot, only
        // transparency -> fallback is left of the mixer
        // the -O2 / __restrict loop: inline here (RenderLayers is -Os) every fb
        // byte store reloaded remap and nxU — 7.5 ms of core1 a frame for 288
        // rows of ULA alone (TX-1696's title screen at 576p, hw 2026-10-13)
        nxMixU<false>(fb, nxU, remap, rgb332_to_333(LS->r4A), xres);
        NX_LT_END(4);
        return;
    }
    nxFillTr(nxT, xres);
    // 640-wide pair frame, 80-column row: the odd phase into nxT2 as well
    const bool tmPair = tmEn && LS->hires == 2 && (LS->r6B & 0x40);
    if (tmPair) nxFillTr(nxT2, xres);
    if (tmEn) RenderTilemapLine(row);
    bool p2 = false;
    int dx0 = 0, dx1 = 0;                  // [dx0, dx1): where the two phases differ (even, word-aligned)
    if (tmPair) {
        const uint32_t* a = (const uint32_t*)nxT; const uint32_t* b = (const uint32_t*)nxT2;
        const int n = (xres + 1) >> 1;
        int k0 = 0, k1 = n;
        while (k0 < n && a[k0] == b[k0]) k0++;
        if (k0 < n) {
            while (k1 > k0 && a[k1 - 1] == b[k1 - 1]) k1--;
            p2 = true; dx0 = k0 * 2; dx1 = k1 * 2;
            if (dx1 > xres) dx1 = xres;
        }
    }
    NX_LT_END(1);
    nxFillTr(nxL, xres);
    nxRowL2 = false;
    // 640-wide pair frame, Layer 2 640x256x4bpp: the right pixels into nxL2nd
    const bool l2Pair = l2En && LS->hires == 2 && (LS->r70 & 0x20);
    if (l2Pair) nxFillTr(nxL2nd, xres);
    if (l2En) RenderL2Line(row);
    if (l2Pair && nxRowL2) {
        const uint32_t* a = (const uint32_t*)nxL; const uint32_t* b = (const uint32_t*)nxL2nd;
        const int n = (xres + 1) >> 1;
        int k0 = 0, k1 = n;
        while (k0 < n && a[k0] == b[k0]) k0++;
        if (k0 < n) {
            while (k1 > k0 && a[k1 - 1] == b[k1 - 1]) k1--;
            int e0 = k0 * 2, e1 = k1 * 2;
            if (e1 > xres) e1 = xres;
            if (!p2) { dx0 = e0; dx1 = e1; p2 = true; }
            else { if (e0 < dx0) dx0 = e0; if (e1 > dx1) dx1 = e1; }
        }
    }
    NX_LT_END(2);
    nxFillTr(nxS, xres);
    nxRowSpr = false;
    nxSprX0 = 0; nxSprX1 = (int16_t)xres;
    if (r15 & 0x01) RenderSpritesLine(y);
    NX_LT_END(3);

#if NEXT_TRACE
    // Warhawk bottom-stripe probe: for paper rows 0 and 191, which layers are
    // opaque, the Layer 2 source row and a hash of each layer's paper pixels.
    if (y == 0 || y == 191) nxRowProbe(y, xres, xPaperOff);
#endif
    NxMix m;
    m.fb = fb;
    m.remap = remap;
    m.fb9 = rgb332_to_333(LS->r4A);
    m.ulaEn = !(LS->r68 & 0x80);
    m.stencil = (LS->r68 & 0x01) && m.ulaEn && tmEn;
    // an all-transparent ULA row mixes exactly like the ULA switched off —
    // except under the stencil, where it blanks the tilemap
    if (!ulaRow && !m.stencil) m.ulaEn = false;
    m.tm = tmEn;
    m.l2 = l2En && nxRowL2;             // empty rows drop out of the mix
    m.spr = (r15 & 0x01) && nxRowSpr;
    m.blend = (LS->r68 >> 5) & 3;
    const int order = (r15 >> 2) & 7;
    if (LS->hires == 1) {
        s_hrMix.layers = true; s_hrMix.order = (uint8_t)order;
        s_hrMix.l2 = m.l2; s_hrMix.spr = m.spr; s_hrMix.tm = m.tm; s_hrMix.stencil = m.stencil;
    }
    // nxUlaMerge rewrites nxU in place: keep the ULA span for the second pass
    if (p2 && m.ulaEn && m.tm) {
        const uint32_t* a = (const uint32_t*)nxU; uint32_t* b = (uint32_t*)nxUs;
        for (int k = dx0 >> 1; k < (dx1 + 1) >> 1; k++) b[k] = a[k];
    }
#if NEXT_TRACE
    nxMixNote(m.l2, m.spr, p2, p2 ? dx1 - dx0 : 0);
#endif
    if (order < 6) nxMixOrder(m, xres, order, !p2);
    else nxMixBlend(m, xres, order == 7);
    if (p2) {
        // the right pixels: same layers, the tilemap's odd phase, only over
        // [dx0, dx1) — the mixers take their buffers from these pointers
        if (m.ulaEn && m.tm) {
            const uint32_t* a = (const uint32_t*)nxUs; uint32_t* b = (uint32_t*)nxU;
            for (int k = dx0 >> 1; k < (dx1 + 1) >> 1; k++) b[k] = a[k];
        }
        uint16_t* const su = nxU; uint16_t* const st = nxT; uint16_t* const sl = nxL; uint16_t* const ss = nxS;
        nxU = su + dx0; nxT = (tmPair ? nxT2 : st) + dx0; nxL = (l2Pair ? nxL2nd : sl) + dx0; nxS = ss + dx0;
        m.fb = p80Line2 + dx0;
        if (order < 6) nxMixOrder(m, dx1 - dx0, order);
        else nxMixBlend(m, dx1 - dx0, order == 7);
        nxU = su; nxT = st; nxL = sl; nxS = ss;
        s_rowDiff = true;
        s_dx0 = dx0; s_dx1 = dx1;
    }
    NX_LT_END(4);
}

// Layer 2 320/640 the slow way (flash): 640x256x4bpp, or no band (no SRAM) —
// one pixel at a time. o = the line buffer at tilemap/L2 x 0, brow = this
// row of the band or nullptr.
__attribute__((noinline)) static bool nxL2SlowRow(uint16_t* o, const uint8_t* brow, const uint16_t* raw,
                                                  int t0, int t1, int scrollX, uint8_t sy, uint16_t bank,
                                                  uint8_t palOfs, uint8_t transp, bool hires,
                                                  uint16_t* o2 = nullptr) {
    bool any = false;
    uint16_t curPage = 0xFFFF;
    const uint8_t* pgp = nullptr;
    for (int tx = t0; tx <= t1; tx++) {
        int sx = tx + scrollX;
        if (sx >= 320) sx -= 320;
        uint8_t b;
        if (brow) b = brow[sx];
        else {
            const uint32_t off = (uint32_t)sx * 256 + sy;
            const uint16_t page = bank + (off >> 13);
            if (page != curPage) { curPage = page; pgp = MemESP::nextPgPtr(page); }
            if (!pgp) continue;               // >= NEXT_PAGES, or no Next RAM
            b = pgp[off & 0x1FFF];
        }
        uint16_t e, v;
        if (o2) {
            // 640-wide pair frame: the left pixel here, the right one in o2
            e = raw[(uint8_t)(palOfs | (b & 0x0F))];
            v = e & 0x1FF;
            if ((v >> 1) != transp) { o2[tx] = (uint16_t)(v | ((e & 0x8000) ? NX_PRIO : 0)); any = true; }
            e = raw[(uint8_t)(palOfs | (b >> 4))];
            v = e & 0x1FF;
        } else if (!hires) {
            e = raw[(uint8_t)(b + palOfs)];
            v = e & 0x1FF;
        } else {
            e = raw[(uint8_t)(palOfs | (b >> 4))];
            v = e & 0x1FF;
            if ((v >> 1) == transp) {
                e = raw[(uint8_t)(palOfs | (b & 0x0F))];
                v = e & 0x1FF;
            }
        }
        if ((v >> 1) != transp) { o[tx] = (uint16_t)(v | ((e & 0x8000) ? NX_PRIO : 0)); any = true; }
    }
    return any;
}

// Layer 2: 256x192x8bpp (row-major) or 320x256x8bpp (column-major, covers
// the border window). the palette priority bit
// rides along as NX_PRIO for the mixer.
// Flash: runs once per palette change, not per row (inline it was 244 B of
// the full .nxovl window).
// One span of a 320x256 Layer 2 row through the row LUT (RenderL2Line): four
// source pixels per 32-bit band read (unaligned: SRAM), the
// pointers in registers (inline, RenderL2Line spilled the band base and
// reloaded it per pixel). Returns the OR of (colour ^ NX_TR): non-zero = an
// opaque pixel was written.
// Returns the AND of the pixel pairs written (both halves): bit 15 / bit 31
// clear = an opaque pixel was written (NX_TR is 0x8000 and an opaque entry has
// bit 15 clear) — two ANDs per four pixels where the XOR/OR form was eight
// (Atic Atac Remake: 82k Layer 2 pixels a frame at 7 instructions each).
static inline __attribute__((always_inline)) uint32_t nxL2LutSpan(const uint8_t* __restrict sp,
        uint16_t* __restrict op, int n, const uint16_t* __restrict lt) {
    uint32_t acc = 0xFFFFFFFFu;
    for (; n >= 4; n -= 4, sp += 4, op += 4) {
        uint32_t b;
        __builtin_memcpy(&b, sp, 4);       // the band is SRAM: an unaligned LDR is fine on the M33
        const uint32_t w0 = (uint32_t)lt[b & 0xFF] | ((uint32_t)lt[(b >> 8) & 0xFF] << 16);
        const uint32_t w1 = (uint32_t)lt[(b >> 16) & 0xFF] | ((uint32_t)lt[b >> 24] << 16);
        __builtin_memcpy(op, &w0, 4);      // unaligned STR: the row buffer is SRAM too
        __builtin_memcpy(op + 2, &w1, 4);
        acc &= w0 & w1;
    }
    while (n-- > 0) { const uint16_t v = lt[*sp++]; *op++ = v; acc &= (uint32_t)v | 0xFFFF0000u; }
    return acc;
}
static inline __attribute__((always_inline)) bool nxL2SpanOpaque(uint32_t acc) {
    return (acc & 0x80008000u) != 0x80008000u;
}

// Flash: 256x192 with an odd X scroll or clip (rare — the word-aligned run in
// RenderL2Line is what titles use). True = an opaque pixel was written.
__attribute__((noinline)) static bool nxL2Mode0Slow(const uint8_t* src, uint16_t* q, uint8_t sx, int count,
                                                   const uint16_t* raw, uint8_t palOfs, uint8_t transp) {
    bool any = false;
    while (count-- > 0) {
        const uint16_t e = raw[(uint8_t)(src[sx++] + palOfs)];   // uint8 wrap = 256px scroll wrap
        const uint16_t v = e & 0x1FF;
        if ((v >> 1) != transp) { *q = (uint16_t)(v | ((e & 0x8000) ? NX_PRIO : 0)); any = true; }
        q++;
    }
    return any;
}

// Flash: once per palette / offset / NR 0x14 change, not per row.
__attribute__((noinline)) static void nxL2LutBuild(uint16_t* lt, const uint16_t* raw, uint8_t palOfs, uint8_t transp) {
    for (int i = 0; i < 256; i++) {
        const uint16_t e = raw[(uint8_t)(i + palOfs)];
        const uint16_t v = e & 0x1FF;
        lt[i] = ((v >> 1) != transp) ? (uint16_t)(v | ((e & 0x8000) ? NX_PRIO : 0)) : NX_TR;
    }
}

__attribute__((noinline)) static void nxPalMono(uint8_t pal, const uint16_t* raw) {
    s_palMonoDirty[pal] = 0;
    const uint16_t c = raw[0] & 0x1FF;
    for (int i = 1; i < 256; i++) if ((raw[i] & 0x1FF) != c) { s_palMono[pal] = 0xFFFF; return; }
    s_palMono[pal] = c;
}
__attribute__((optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
#if NEXT_TRACE
volatile uint32_t g_nx_l2band[3] = {};   // band fills, borrowed from the other core, waits that gave up
#endif
// Layer 2 320x256: load the 8-row column band for (bank, blk). core0 helps by
// taking the OLDEST queued row, so the two cores render consecutive rows and
// each needed the same band: both filled it, 320 PSRAM line fills each, on the
// one XIP port both cores share (Atic Atac Remake intro: ~28k misses a frame,
// Layer 2 11 ms of render, hw 2026-10-19). A band the other core filled for the
// same block in the same frame is copied instead (2.5 KB SRAM -> SRAM); one it
// is filling right now is waited for. Same frame = queue positions less than 8
// apart; gen is a seqlock (odd while written, our own copy included — the
// other core may be borrowing from us). Flash: once per 8 rows.
__attribute__((noinline)) static void nxL2BandLoad(NxBand& bd, uint8_t* band, uint8_t* shared, uint16_t bank,
                                                   uint8_t blk, uint32_t key, int brow) {
    const unsigned core = *(const uint32_t*)SIO_BASE & 1;
    const uint32_t job = nx_jobAt[core];
    NxBand& ot = s_band[&bd == &s_band[0] ? 1 : 0];
    const volatile NxBand& vo = ot;
    uint8_t* const obuf = ot.buf ? ot.buf : shared;
    const bool share = g_next_c1_live && s_band[1].buf && obuf && obuf != band;
    bd.pendKey = key; bd.pendRow = brow; bd.pendJob = job;
    if (share) {
        const uint64_t t0 = time_us_64();
        for (;;) {
            const uint32_t g1 = vo.gen;
            __dmb();
            if (g1 & 1) {
                // the other core is writing its band: worth waiting for only if it is ours
                if (vo.pendKey == key && vo.pendRow == brow && (uint32_t)(job - vo.pendJob) + 7u < 15u &&
                    time_us_64() - t0 < 2000) { tight_loop_contents(); continue; }
#if NEXT_TRACE
                if (vo.pendKey == key && vo.pendRow == brow) g_nx_l2band[2] = g_nx_l2band[2] + 1;
#endif
                break;
            }
            if (vo.key != key || vo.row != brow || (uint32_t)(job - vo.fillJob) + 7u >= 15u) break;
            __dmb();
            bd.gen = bd.gen + 1;
            __dmb();
            memcpy(band, obuf, 320 * 8);
            __dmb();
            const bool ok = vo.gen == g1;
            if (ok) { bd.key = key; bd.row = brow; bd.fillJob = vo.fillJob; }
            __dmb();
            bd.gen = bd.gen + 1;
            if (ok) {
#if NEXT_TRACE
                g_nx_l2band[1] = g_nx_l2band[1] + 1;
#endif
                return;
            }
            break;   // it changed under the copy: fill it ourselves
        }
    }
    __dmb();
    bd.gen = bd.gen + 1;
    __dmb();
    for (int c0 = 0; c0 < 320; c0 += 32) {         // one 8K page = 32 columns
        const uint32_t off0 = (uint32_t)c0 * 256 + blk;
        const uint8_t* pg = MemESP::nextPgPtr(bank + (off0 >> 13));
        for (int c = c0; c < c0 + 32; c++) {
            uint32_t lo = 0, hi = 0;
            if (pg) {
                const uint32_t* q = (const uint32_t*)(pg + (((uint32_t)c * 256 + blk) & 0x1FFF));
                lo = q[0]; hi = q[1];
            }
            uint8_t* d = band + c;
            d[0]    = (uint8_t)lo; d[320]  = (uint8_t)(lo >> 8);
            d[640]  = (uint8_t)(lo >> 16); d[960] = (uint8_t)(lo >> 24);
            d[1280] = (uint8_t)hi; d[1600] = (uint8_t)(hi >> 8);
            d[1920] = (uint8_t)(hi >> 16); d[2240] = (uint8_t)(hi >> 24);
        }
    }
    __dmb();
    bd.key = key; bd.row = brow; bd.fillJob = job;
    __dmb();
    bd.gen = bd.gen + 1;
#if NEXT_TRACE
    g_nx_l2band[0] = g_nx_l2band[0] + 1;
#endif
}

// no-peel: with the band fill moved out (nxL2BandLoad) GCC peeled the scroll-wrap
// loop below into a second copy of the LUT span, +300 B of the full .nxovl window
IRAM_ATTR __attribute__((optimize("no-peel-loops", "no-unswitch-loops"))) void NEXTVID::RenderL2Line(int row) {
    uint8_t pal = (LS->palCtrl & 0x04) ? 5 : 1;
    const uint16_t* raw = rawPal[pal];
    const uint8_t transp = LS->r14;     // compared with the COLOUR (zxnext.vhd 7123)
    uint8_t mode = (LS->r70 >> 4) & 3;
    uint8_t palOfs = (LS->r70 & 0x0F) << 4;
    uint16_t bank = (uint16_t)(LS->r12 & 0x7F) * 2;
    uint16_t* const out = nxL;
    // Every entry of the palette transparent: nothing this layer can show,
    // whatever its banks hold — NEXTEST enables Layer 2 with an all-black
    // palette over NR 0x14 = 0 and runs its memory test through the banks,
    // which cost 192 PSRAM rows (5 ms of the frame's render) for no pixel.
    // The scan stops at the first opaque entry (entry 0 or 1 on a real picture).
    if (s_palMonoDirty[pal]) nxPalMono(pal, raw);
    if ((s_palMono[pal] >> 1) == transp) return;   // 0xFFFF >> 1 never equals an 8-bit colour

    if (mode == 0) {
        // 256x192, aligned with the paper area
        int y = row - paperTopRow;
        if (y < 0 || y >= 192) return;
        if (y < LS->clip[0][2] || y > LS->clip[0][3]) return;
        uint16_t sy = y + LS->r17;
        if (sy >= 192) sy -= 192;
        uint16_t page = bank + (sy >> 5);
        if (page >= MemESP::NEXT_PAGES - 1u) return;
        const uint8_t* src = MemESP::nextPgPtr(page) + (sy & 31) * 256;
        uint8_t sx = LS->r16 + LS->clip[0][0];
        uint16_t* q = out + xPaperOff + LS->clip[0][0];
        int count = (int)LS->clip[0][1] - (int)LS->clip[0][0] + 1;
        if (!(sx & 3) && !(count & 3)) {
            // Word-aligned run (no odd X scroll / clip — Santa, most titles):
            // 4 source pixels per load (the row is in butter PSRAM, one XIP
            // access instead of four), branchless store — the buffer was
            // filled transparent, writing NX_TR again is the same value.
            const uint32_t* w = (const uint32_t*)src;
            bool any = false;
            // index 0 transparent (NXModPlayer: NR 0x14 = 0, black background):
            // four zero pixels are skipped — the buffer is already NX_TR
            const bool z0 = ((raw[palOfs] & 0x1FF) >> 1) == transp;
            for (int k = 0; k < count; k += 4, sx = (uint8_t)(sx + 4)) {
                uint32_t b = w[sx >> 2];
                if (z0 && b == 0) { q += 4; continue; }
                for (int i = 0; i < 4; i++, b >>= 8) {
                    const uint16_t e = raw[(uint8_t)(b + palOfs)];
                    const uint16_t v = e & 0x1FF;
                    const bool op = (v >> 1) != transp;
                    any |= op;
                    *q++ = op ? (uint16_t)(v | ((e >> 4) & NX_PRIO)) : NX_TR;
                }
            }
            nxRowL2 = any;
            return;
        }
        if (nxL2Mode0Slow(src, q, sx, count, raw, palOfs, transp)) nxRowL2 = true;
    } else {
        // 320x256 (mode 1) and 640x256x4bpp (modes 2/3), both column-major
        // (byte offset = x*256 + y, layer2.vhd 160) and covering the border.
        // 640: the byte is two pixels, high nibble left, index = (offset + 0) & nibble
        // (layer2.vhd 202-203). Our fb pixel is a 320-wide one, so the pair is
        // shown as its left pixel, or the right one where the left is
        // transparent (deviation, like the 80-column tilemap).
        const bool hires = mode & 2;
        int ty = row - (paperTopRow - 32);
        if (ty < 0 || ty >= 256) return;
        int cy1 = LS->clip[0][2], cy2 = LS->clip[0][3];
        if (ty < cy1 || ty > cy2) return;
        uint16_t scrollX = ((LS->r71 & 1) << 8) | LS->r16;
        uint8_t sy = (uint8_t)(ty + LS->r17);
        int xOrig = xPaperOff - 32;
        int xres = VIDEO::vga.xres;
        int cx1 = LS->clip[0][0] * 2, cx2 = LS->clip[0][1] * 2 + 1;
        // Column-major: each pixel of a row is in another 256-byte column, and
        // the 2-way XIP cache maps every 32nd column to one set, so reading the
        // row straight from butter PSRAM missed on every pixel (NextBench: Layer
        // 2 22 ms of core1 a frame, 80k misses). Eight rows of every column are
        // ONE 8-byte line fill: fill the band once per 8 source rows, read the
        // rest from SRAM. Rows render in order and the next frame starts above
        // the last band, so (row - fill row) < 8 with the same key is a hit.
        // Guest writes to the rows of a live band show a few lines late — the
        // same race the beam has.
        // Stored transposed — band[r * 320 + column] — so a row is read in order.
        NxBand& bd = *nxCtx()->band;
        uint8_t* const band = bd.buf ? bd.buf : l2Band;
        uint32_t& s_bandKey = bd.key;
        int& s_bandRow = bd.row;
        int& s_lastRow = bd.lastRow;
        const uint32_t key = ((uint32_t)bank << 8) | (sy & 0xF8);
        if (row < s_lastRow) s_bandKey = 0xFFFFFFFFu;     // a new frame: nothing carries over
        s_lastRow = row;
        if (band && (key != s_bandKey || (unsigned)(row - s_bandRow) >= 8u))
            nxL2BandLoad(bd, band, l2Band, bank, (uint8_t)(sy & 0xF8), key, row - (sy & 7));
        // The visible columns: the clip window and the framebuffer, decided once.
        int t0 = cx1 > -xOrig ? cx1 : -xOrig;
        int t1 = cx2 < xres - 1 - xOrig ? cx2 : xres - 1 - xOrig;
        if (t0 < 0) t0 = 0;
        if (t1 > 319) t1 = 319;
        bool any = false;
        if (band && !hires) {
            const uint8_t* __restrict src = band + (sy & 7) * 320;
            uint16_t* __restrict o = out + xOrig;
            // The colour each index gives on this row, NX_TR where transparent:
            // one load and one store per pixel, no compare (the row buffer was
            // filled NX_TR, so storing NX_TR again is the same value). Built
            // when the palette, its offset or NR 0x14 move — Adventures in
            // Videoland, 320x256 at 28 MHz: Layer 2 11-12 ms of core1 a frame.
            uint16_t* __restrict lt = (uint16_t*)(band + 320 * 8);
            const uint32_t lk = (uint32_t)pal | ((uint32_t)palOfs << 8) | ((uint32_t)transp << 16);
            const uint16_t gen = s_palGen[pal];
            if (bd.lutKey != lk || bd.lutGen != gen) {
                nxL2LutBuild(lt, raw, palOfs, transp);
                bd.lutKey = lk; bd.lutGen = gen;
            }
            uint32_t acc = 0xFFFFFFFFu;
            int sx = t0 + scrollX; if (sx >= 320) sx -= 320;
            for (int tx = t0; tx <= t1; ) {
                int n = t1 - tx + 1;
                if (n > 320 - sx) n = 320 - sx;            // up to the scroll wrap
                acc &= nxL2LutSpan(src + sx, o + tx, n, lt);
                tx += n; sx = 0;
            }
            any = nxL2SpanOpaque(acc);
        } else {
            any = nxL2SlowRow(out + xOrig, band ? band + (sy & 7) * 320 : nullptr, raw, t0, t1, scrollX,
                              sy, bank, palOfs, transp, hires,
                              (hires && LS->hires == 2) ? nxL2nd + xOrig : nullptr);
        }
        if (any) nxRowL2 = true;
    }
}


// Tilemap text mode (NR 0x6B bit 3) — one tile's span of a line. 1bpp: pixel
// = bit (7 - x) of the row byte, palette index = attr 7:1 + pixel; never the
// NR 0x4C index test: a text pixel is transparent when its colour equals NR 0x14
// (tilemap.vhd 385-386, zxnext.vhd 7109). 80 columns are drawn at half width,
// so the pixel pair is ORed and 1-pixel strokes of the font survive (deviation).
// Flash on purpose: text mode is rare and this keeps ~1 KB out of every board's
// SRAM (RenderTilemapLine is RAM-resident).
// pairPhase: one phase of a 640-wide pair frame (odd X scroll only — the even
// case is RenderTilemapLine's one-walk loop): every other pixel, never ORed.
__attribute__((noinline)) static void tmTextSpan(uint16_t* o, int n, uint8_t b, int u0, int step,
                                                 uint8_t palHi, const uint16_t* rawTM, uint8_t r14,
                                                 uint16_t below, bool pairPhase) {
    for (int k = 0; k < n; k++) {
        const int u = u0 + k * step;
        uint8_t bit = (b >> (7 - u)) & 1;
        if (step == 2 && !pairPhase && u < 7) bit |= (b >> (6 - u)) & 1;
        const uint16_t v = rawTM[(uint8_t)(palHi | bit)] & 0x1FF;
        if ((v >> 1) != r14) o[k] = (uint16_t)(v | below);
    }
}

// Tilemap: 40x32 (or 80x32 rendered horizontally halved) tiles of 8x8x4bpp.
// Map and tile definitions live in bank 5 (offsets from nextreg 0x6E/0x6F).
// The tilemap window is 320x256 and covers the border area around the paper.
// -O2 (the file is -Os): tile setup and the span copy are per-pixel hot code
// (TX-1696: 5 ms of core1 a frame, hw 2026-10-09).
__attribute__((optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
IRAM_ATTR void NEXTVID::RenderTilemapLine(int row) {
    uint8_t ctrl = LS->r6B;
    int ty = row - (paperTopRow - 32);
    if (ty < 0 || ty >= 256) return;
    if (ty < LS->clip[3][2] || ty > LS->clip[3][3]) return;

    bool cols80 = ctrl & 0x40;
    bool noAttr = ctrl & 0x20;
    // tilemap.vhd 191-195: bit 3 text mode (1bpp, 8 bytes a tile, attr 7:1 =
    // palette, no mirror/rotate), bit 1 512 tiles (attr bit 0 = tile bit 8, and
    // the tilemap goes UNDER the ULA), bit 0 tilemap always on top of the ULA.
    const bool text = ctrl & 0x08, m512 = ctrl & 0x02, onTop = ctrl & 0x01;
    const uint8_t palNo = (ctrl & 0x10) ? 7 : 3;
    const uint16_t* rawTM = rawPal[palNo];
    uint8_t transp = LS->r4C & 0x0F;
    // NR 0x6E/0x6F: bit 7 = bank 7 instead of bank 5, bits 5:0 = 256-byte
    // offset; the address wraps inside the 16K bank (tilemap.vhd 399-401).
    const uint8_t* mapBank  = MemESP::ram[(LS->r6E & 0x80) ? 7 : 5].direct();
    const uint8_t* defsBank = MemESP::ram[(LS->r6F & 0x80) ? 7 : 5].direct();
    const uint16_t defsOff = (uint16_t)(LS->r6F & 0x3F) << 8;
    const uint8_t* map  = mapBank + (uint16_t)(LS->r6E & 0x3F) * 256;
    uint16_t scrollX = ((LS->r2F & 3) << 8) | LS->r30;
    uint8_t sy = (uint8_t)(ty + LS->r31); // wraps at 256
    int cols = cols80 ? 80 : 40;
    int entrySz = noAttr ? 1 : 2;
    const uint8_t* mrow = map + (sy >> 3) * cols * entrySz;
    if (s_mapShLive)                      // a DMA rewrote map rows still owed to queued lines
        mrow = nxMapRowFor((LS->r6E & 0x80) ? 7 : 5,
                           (uint32_t)(LS->r6E & 0x3F) * 256 + (uint32_t)(sy >> 3) * cols * entrySz, mrow);
    uint8_t inRow = sy & 7;
    const int xOrig = xPaperOff - 32;
    const int xres = VIDEO::vga.xres;
    // Visible tx range: the clip window (in half-columns) and the framebuffer.
    int t0 = LS->clip[3][0] * 2, t1 = LS->clip[3][1] * 2 + 1;
    if (t0 < -xOrig) t0 = -xOrig;
    if (t1 > 319) t1 = 319;
    if (t1 > xres - 1 - xOrig) t1 = xres - 1 - xOrig;

    // Tile by tile: one map entry, one attribute decode and the tile's 8
    // source pixels per tile instead of per output pixel. 80-column mode
    // renders 640 source pixels halved: every other source pixel.
    const int step = cols80 ? 2 : 1;
    const int wrap = cols80 ? 640 : 320;
    // 640-wide pair frame: the odd source pixels go to nxT2 (a second pass).
    const bool pair = cols80 && LS->hires == 2;
    if (pair && text && !(scrollX & 1)) {
        // 640-wide pair frame, text mode, even X scroll: both phases in ONE walk
        // of the tiles — a tile's even source pixels go to nxT, the odd ones to
        // nxT2, and the two colours are rebuilt only when the attribute changes
        // (NEXTEST: one attribute for the whole map, tilemap 13.7 ms of the
        // frame's render with the two-pass loop below, hw 2026-10-11).
        uint16_t* __restrict const o0 = nxT + xOrig;
        uint16_t* __restrict const o1 = nxT2 + xOrig;
        const uint8_t r14 = LS->r14, a6C = LS->r6C;
        int tx = t0;
        int sx = (tx * 2 + scrollX) % 640;
        int lastAttr = -1;
        uint16_t c0 = NX_TR, c1 = NX_TR;
        while (tx <= t1) {
            const int u0 = sx & 7;                     // even
            int n = (8 - u0) >> 1;
            if (n > t1 - tx + 1) n = t1 - tx + 1;
            const uint8_t* e = mrow + (sx >> 3) * entrySz;
            const uint8_t attr = noAttr ? a6C : e[1];
            if (attr != lastAttr) {
                lastAttr = attr;
                const uint16_t below = (!onTop && ((attr & 0x01) || m512)) ? NX_BELOW : 0;
                const uint8_t ph2 = (uint8_t)(attr & 0xFE);
                const uint16_t v0 = rawTM[ph2] & 0x1FF, v1 = rawTM[(uint8_t)(ph2 | 1)] & 0x1FF;
                c0 = ((v0 >> 1) != r14) ? (uint16_t)(v0 | below) : NX_TR;
                c1 = ((v1 >> 1) != r14) ? (uint16_t)(v1 | below) : NX_TR;
            }
            const uint16_t tnum = (uint16_t)(e[0] | ((m512 && (attr & 0x01)) ? 0x100 : 0));
            const uint32_t b = (uint32_t)defsBank[(uint16_t)(defsOff + tnum * 8 + inRow) & 0x3FFF] << u0;
            uint16_t* __restrict p0 = o0 + tx;
            uint16_t* __restrict p1 = o1 + tx;
            if (n == 4) {
                p0[0] = (b & 0x80) ? c1 : c0; p1[0] = (b & 0x40) ? c1 : c0;
                p0[1] = (b & 0x20) ? c1 : c0; p1[1] = (b & 0x10) ? c1 : c0;
                p0[2] = (b & 0x08) ? c1 : c0; p1[2] = (b & 0x04) ? c1 : c0;
                p0[3] = (b & 0x02) ? c1 : c0; p1[3] = (b & 0x01) ? c1 : c0;
            } else {
                for (int k = 0; k < n; k++) {
                    p0[k] = ((b << (2 * k)) & 0x80) ? c1 : c0;
                    p1[k] = ((b << (2 * k)) & 0x40) ? c1 : c0;
                }
            }
            tx += n;
            sx += 2 * n;
            if (sx >= 640) sx -= 640;
        }
        return;
    }
    // 4bpp whole tile: the 16 colours of the tile's palette row, transparency
    // (NX_TR) and the under-ULA mark folded in, so a pixel is one load and one
    // store — no compare, no branch. Rebuilt when the attribute's palette or
    // the mark changes (one or two palettes a row: Atic Atac Remake, 40 tiles a
    // row at ~12 instructions a pixel = tilemap 5.2 ms of render a frame).
    uint16_t tl[16];
    int tlKey = -1;
    for (int ph = 0; ph <= (pair ? 1 : 0); ph++) {
    uint16_t* const out = (ph ? nxT2 : nxT) + xOrig;
    int tx = t0;
    int sx = (tx * step + ph + scrollX) % wrap; // advanced per tile below, no per-tile %
    while (tx <= t1) {
        const int u0 = sx & 7;
        int n = (8 - u0 + step - 1) / step;        // output pixels left in this tile
        if (n > t1 - tx + 1) n = t1 - tx + 1;
        const uint8_t* e = mrow + (sx >> 3) * entrySz;
        const uint8_t tile = e[0];
        const uint8_t attr = noAttr ? LS->r6C : e[1];
        // The ULA is drawn OVER this tile when attr bit 0 is set (512-tile
        // mode: always), unless the tilemap is forced on top (tilemap.vhd 388);
        // the mixer decides with the ULA's own transparency.
        const uint16_t below = (!onTop && ((attr & 0x01) || m512)) ? NX_BELOW : 0;
        const uint16_t tnum = (uint16_t)(tile | ((m512 && (attr & 0x01)) ? 0x100 : 0));
        if (text) {
            const uint8_t b = defsBank[(uint16_t)(defsOff + tnum * 8 + inRow) & 0x3FFF];
            if (n == 8 && step == 1) {
                // Whole 40-column text tile: two colours, 8 pixels by bit. The
                // line buffer was filled transparent and tiles never overlap,
                // so writing NX_TR for a transparent colour is the same as
                // skipping it (NXModPlayer: tilemap 6.3 ms of core1, hw 2026-10-10).
                const uint8_t ph = (uint8_t)(attr & 0xFE), r14 = LS->r14;
                const uint16_t v0 = rawTM[ph] & 0x1FF, v1 = rawTM[(uint8_t)(ph | 1)] & 0x1FF;
                const uint16_t c0 = ((v0 >> 1) != r14) ? (uint16_t)(v0 | below) : NX_TR;
                const uint16_t c1 = ((v1 >> 1) != r14) ? (uint16_t)(v1 | below) : NX_TR;
                uint16_t* __restrict o = out + tx;
                for (int k = 0; k < 8; k++) o[k] = ((b << k) & 0x80) ? c1 : c0;
                tx += 8; sx += 8; if (sx >= wrap) sx -= wrap;
                continue;
            }
            tmTextSpan(out + tx, n, b, u0, step, (uint8_t)(attr & 0xFE), rawTM, LS->r14, below, pair);
        } else {
            const bool xm = attr & 0x08, ym = attr & 0x04, rot = attr & 0x02;
            const uint16_t defBase = (uint16_t)(defsOff + tnum * 32);
            const uint8_t palHi = attr & 0xF0;
            // The tile's 8 pixels of this row, mirror/rotate applied, indexed by
            // the source column u (0..7) the span below steps through.
            uint8_t px[8];
            if (!rot) {
                const uint8_t vv = ym ? (uint8_t)(7 - inRow) : inRow;
                // 4 bytes = 8 pixels, high nibble first; 4-aligned in the bank
                const uint32_t w = *(const uint32_t*)(defsBank + ((defBase + vv * 4) & 0x3FFC));
                if (n == 8 && step == 1) {
                    // Whole tile (every tile of a row but the first under a fine
                    // X scroll): output pixel k = nibble k of wn, no px[] and no
                    // per-pixel index arithmetic (Santa: tilemap 5.6 ms of core1
                    // a frame, hw 2026-10-10). X mirror = byte-reversed word: its
                    // low nibbles are the source's odd pixels from the right.
                    uint32_t wn = xm ? __builtin_bswap32(w)
                                     : (((w >> 4) & 0x0F0F0F0Fu) | ((w << 4) & 0xF0F0F0F0u));
                    const int key = (int)palHi | (int)below;
                    if (key != tlKey) {
                        tlKey = key;
                        const uint16_t* __restrict pr = rawTM + palHi;
                        for (int i = 0; i < 16; i++) tl[i] = (uint16_t)((pr[i] & 0x1FF) | below);
                        tl[transp] = NX_TR;   // the buffer was filled NX_TR: storing it is the skip
                    }
                    uint16_t* __restrict o = out + tx;
                    if (!((uintptr_t)o & 2)) {
                        // two pixels per word store (the row buffer is SRAM)
                        uint32_t* __restrict ow = (uint32_t*)o;
                        for (int k = 0; k < 4; k++, wn >>= 8)
                            ow[k] = (uint32_t)tl[wn & 0x0F] | ((uint32_t)tl[(wn >> 4) & 0x0F] << 16);
                    } else {
                        for (int k = 0; k < 8; k++, wn >>= 4) o[k] = tl[wn & 0x0F];
                    }
                    tx += 8; sx += 8; if (sx >= wrap) sx -= wrap;
                    continue;
                }
                for (int i = 0; i < 4; i++) {
                    const uint8_t b = (uint8_t)(w >> (8 * i));
                    px[2 * i] = b >> 4; px[2 * i + 1] = b & 0x0F;
                }
                if (xm)
                    for (int i = 0; i < 4; i++) { const uint8_t t = px[i]; px[i] = px[7 - i]; px[7 - i] = t; }
            } else {
                for (int u = 0; u < 8; u++) {
                    uint8_t uu = (uint8_t)u, vv = inRow;
                    if (xm) uu = 7 - uu;                                    // X mirror
                    if (ym) vv = 7 - vv;                                    // Y mirror
                    const uint8_t t = uu; uu = vv; vv = 7 - t;              // rotate
                    const uint8_t b = defsBank[(uint16_t)(defBase + vv * 4 + (uu >> 1)) & 0x3FFF];
                    px[u] = (uu & 1) ? (b & 0x0F) : (b >> 4);
                }
            }
            uint16_t* o = out + tx;
            for (int k = 0; k < n; k++) {
                const uint8_t pix = px[u0 + k * step];
                if (pix != transp) o[k] = (uint16_t)((rawTM[pix | palHi] & 0x1FF) | below);
            }
        }
        tx += n;
        sx += n * step;
        if (sx >= wrap) sx -= wrap;
    }
    }
}

// Resolve all 128 attribute sets against their anchors, in sprite order —
// the one order the anchor chain is defined in (a relative sprite belongs to
// the last anchor BEFORE it), independent of the draw priority. Semantics are
// ZEsarUX tbblue.c tbsprite_do_overlay line for line:
//  - attr 4 bits 7:6 == 01 marks a RELATIVE sprite; anything else (incl. a
//    4-byte sprite, E=0) is an anchor. Rendering those as independent 8-bit
//    anchors put every part of a composite sprite at its small signed offset
//    near the display's top-left corner (hw: TX-1696's broken ship/bosses).
//  - relative: X/Y are signed 8-bit offsets from the anchor, visible only if
//    the anchor is, 4bpp/8bpp from the anchor, N6 in attr 4 bit 5, attr 2 bit
//    0 adds the anchor's palette offset, attr 4 bit 0 its pattern number.
//  - an anchor with attr 4 bit 5 (T) makes its relatives UNIFIED: the anchor's
//    rotate / mirror / scale transform the offsets and the parts as one sprite.
static inline bool sprIsRel(const uint8_t* a) {
    return (a[3] & 0x40) && (a[4] & 0xC0) == 0x40;
}

// core0: log one attribute byte for the renderer. A full log (the renderer far
// behind, or no lines rendered — skipped frames) is applied here after the
// queue drains: nothing renders then.
IRAM_ATTR void NEXTVID::sprLogPut(uint32_t e) {
    if (spr_w - spr_app >= SPR_LOG) {
        c1Drain(5);
        sprApplyTo(*sprR, spr_app, spr_w);   // core1's copy, nobody rendering
        s_sprR0Valid = 0;                     // core0's: taken from core1's again at its next row
    } else if (s_sprR0Valid && spr_w - spr_app0 >= SPR_LOG) {
        s_sprR0Valid = 0;                     // core0 fell behind (it helped little): re-sync, no drain
    }
    sprLog[spr_w % SPR_LOG] = e;
    __dmb();
    if (e & 0x80000000u) s_sprPatW = spr_w + 1;
    spr_w = spr_w + 1;
}

IRAM_ATTR void NEXTVID::sprPut(uint8_t s, uint8_t b, uint8_t v) {
    sprLogPut(((uint32_t)(s & 0x7F) << 11) | ((uint32_t)b << 8) | v);
}

// Renderer: apply the log up to a line's capture position, widen the dirty range.
IRAM_ATTR NEXTVID::SprRender* NEXTVID::sprCur() { return (nxOnCore0() && s_sprR0Valid) ? s_sprR0 : sprR; }
IRAM_ATTR void NEXTVID::sprApply(uint32_t upto) {
    if (nxOnCore0() && s_sprR0Valid) sprApplyTo(*s_sprR0, spr_app0, upto);
    else sprApplyTo(*sprR, spr_app, upto);
}
// core0's copy = core1's attributes at core1's log position (then core0 replays
// the log to each row's own position, exactly as core1 does). core1 writes its
// attr[] only inside sprApplyTo (`applying` up, position published after);
// a copy taken with the flag down and the position unmoved is whole. NOT the
// live attributes at spr_w: that gave core0's rows a newer state than core1's
// and the two cores' rows interleaved — striped sprites (hw 2026-10-19).
void NEXTVID::sprR0Sync() {
    if (!s_sprR0) return;
    SprRender& d = *s_sprR0;
    for (int tries = 0; tries < 64; tries++) {
        const uint32_t a0 = spr_app;
        __dmb();
        if (sprR->applying) { tight_loop_contents(); continue; }
        memcpy(d.attr, sprR->attr, sizeof d.attr);
        __dmb();
        if (sprR->applying || spr_app != a0) continue;
        // resolveSprites rebuilds the visible list only where a sprite's VIS
        // flag CHANGED against res[]: clear it so the full resolve below sees
        // every visible sprite as new (stale flags left d.n at 0 — no sprites
        // on core0's rows, hw 2026-10-19).
        memset(d.res, 0, sizeof d.res);
        d.n = 0; d.dirty = 1; d.lo = 0; d.hi = 127;
        spr_app0 = a0;
        __dmb();
        s_sprR0Valid = 1;
        return;
    }
}
IRAM_ATTR void NEXTVID::sprApplyTo(SprRender& R, volatile uint32_t& app, uint32_t upto) {
    uint32_t i = app;
    if ((int32_t)(upto - i) <= 0) return;   // older position: see palApply
    R.applying = 1;
    __dmb();
    int lo = R.dirty ? R.lo : 127, hi = R.dirty ? R.hi : 0;
#if NEXT_TRACE
    g_nx_logapply_spr = g_nx_logapply_spr + (upto - i);
#endif
    for (; i != upto; i++) {
        const uint32_t e = sprLog[i % SPR_LOG];
        if (e & 0x80000000u) { sprPatterns[(e >> 8) & 0x3FFF] = (uint8_t)e; continue; }
        const int sn = (e >> 11) & 0x7F;
        R.attr[sn][(e >> 8) & 7] = (uint8_t)e;
        if (sn < lo) lo = sn;
        if (sn > hi) hi = sn;
    }
    if (lo <= hi) { R.lo = (uint8_t)lo; R.hi = (uint8_t)hi; R.dirty = 1; }   // patterns only: nothing to resolve
    __dmb();
    app = i;
    __dmb();
    R.applying = 0;
}

// Renderer: re-resolve the dirty range — from the anchor at or before lo to the
// last relative after hi (a relative is resolved against the anchor before it).
IRAM_ATTR void NEXTVID::resolveSprites() {
    SprRender& d = *sprCur();
    int s0 = d.lo, s1 = d.hi + 1;
    while (s0 > 0 && sprIsRel(d.attr[s0])) s0--;
    while (s1 < 128 && sprIsRel(d.attr[s1])) s1++;
#if NEXT_TRACE
    nxSprNote(0, 1); nxSprNote(1, (uint32_t)(s1 - s0));
#endif
    bool visChanged = false;
    int ax = 0, ay = 0, apat = 0, azx = 0, azy = 0;
    uint8_t apal = 0;
    bool avis = false, a4bpp = false, arot = false, axm = false, aym = false, unified = false;
    uint8_t n = 0;
    for (int s = s0; s < s1; s++) {
        const uint8_t* a = d.attr[s];
        const uint8_t a4 = (a[3] & 0x40) ? a[4] : 0;
        const bool rel = (a4 & 0xC0) == 0x40;
        bool vis = a[3] & 0x80;
        bool xm = a[2] & 0x08, ym = a[2] & 0x04, rot = a[2] & 0x02;
        int x = a[0] | ((a[2] & 0x01) << 8);
        int y = a[1];
        uint8_t pal = a[2] & 0xF0;
        int idx = a[3] & 0x3F;
        int zx = (a4 >> 3) & 3, zy = (a4 >> 1) & 3;
        bool is4;
        if (!rel) {
            y |= (a4 & 0x01) << 8;
            is4 = a4 & 0x80;
            if (is4) idx = idx * 2 + ((a4 >> 6) & 1);
            avis = vis; a4bpp = is4; unified = a4 & 0x20;
            ax = x; ay = y; apal = pal; apat = idx;
            axm = xm; aym = ym; arot = rot; azx = zx; azy = zy;
        } else {
            vis = vis && avis;
            is4 = a4bpp;
            if (is4) idx = idx * 2 + ((a4 >> 5) & 1);
            x = (int8_t)a[0];
            y = (int8_t)a[1];
            if (a[2] & 0x01) pal = (uint8_t)((pal + apal) & 0xF0);
            if (a4 & 0x01) idx = (idx + apat) & (is4 ? 127 : 63);
            if (unified) {
                zx = azx; zy = azy;
                if (arot) {
                    rot = !rot;
                    const int ox = x; x = -y; y = ox;
                    const bool om = xm;
                    xm = rot ? ym : !ym;
                    ym = rot ? om : !om;
                }
                if (axm) { xm = !xm; x = -x; }
                if (aym) { ym = !ym; y = -y; }
                x <<= zx; y <<= zy;
            }
            x = (x + ax) & 0x1FF;
            y = (y + ay) & 0x1FF;
        }
        SprRes& r = d.res[s];
        r.x = (int16_t)x;
        r.y = (int16_t)(y > 255 ? y - 512 : y);
        r.pat = (uint16_t)(is4 ? idx * 128 : idx * 256);
        const uint8_t of = r.flags;
        r.flags = (xm ? SR_XM : 0) | (ym ? SR_YM : 0) | (rot ? SR_ROT : 0) | (is4 ? SR_4BPP : 0) | (vis ? SR_VIS : 0);
        if ((of ^ r.flags) & SR_VIS) visChanged = true;
        r.pal = pal;
        r.xs = (uint8_t)zx; r.ys = (uint8_t)zy;
    }
    if (visChanged) {
        for (int s = 0; s < 128; s++)
            if (d.res[s].flags & SR_VIS) d.vis[n++] = (uint8_t)s;
        d.n = n;
    }
    d.dirty = 0;
}


// Render all visible sprites crossing paper line y (0-191). Sprite
// coordinate space puts (32,32) at the paper top-left corner; rendering is
// clipped to the paper area for now (sprites-over-border comes later).
__attribute__((optimize("O2", "no-unroll-loops", "no-tree-loop-distribute-patterns")))
IRAM_ATTR void NEXTVID::RenderSpritesLine(int y) {
    if (sprCur()->dirty) resolveSprites();  // RenderLayers applied this line's log
    const SprRender& sn = *sprCur();
    const uint16_t* sprRaw = rawPal[(LS->palCtrl & 0x08) ? 6 : 2];   // NR 0x43 bit 3
    const uint8_t transp = LS->r4B;
    const int sline = y + 32;
    if (sline < 0 || sline > 255) return;
    // NR 0x15 bit 6: 1 = sprite 0 on top, 0 = sprite 127 on top (ZEsarUX;
    // was inverted). Painter's order: the top one is drawn last.
    const bool zeroOnTop = LS->r15 & 0x40;
    const bool overBorder = LS->r15 & 0x02;
    // Everything that does not depend on the pixel is decided here, once per
    // line / per sprite: the X window (paper or border, clip window, the
    // framebuffer edges) becomes a px range, and the Y clip a line test.
    int lo = overBorder ? -32 : 0, hi = overBorder ? 287 : 255;
    if (!overBorder) {
        if (y < LS->clip[1][2] || y > LS->clip[1][3]) return;
        if (lo < LS->clip[1][0]) lo = LS->clip[1][0];
        if (hi > LS->clip[1][1]) hi = LS->clip[1][1];
    }
    if (lo < -xPaperOff) lo = -xPaperOff;
    if (hi > (int)VIDEO::vga.xres - 1 - xPaperOff) hi = (int)VIDEO::vga.xres - 1 - xPaperOff;
    if (lo > hi) return;
    // Line coverage for the collision flag, bit = paper x + 32 (0..351): an
    // unscaled sprite row is one 16-bit mask ORed in as a word — the byte
    // read-modify-write per pixel was most of the blit (NEXTEST: ~17 sprites a
    // row on 103 rows, 5.4 ms of the frame's render, hw 2026-10-12).
    uint32_t coverage[11];
    for (int k = 0; k < 11; k++) coverage[k] = 0;
    uint16_t* const fbp = nxS + xPaperOff;    // indexed by paper x
    int rendered = 0;
    const int nv = sn.n;
    uint32_t collided = 0;

    for (int i = 0; i < nv; i++) {
        const SprRes& r = sn.res[sn.vis[zeroOnTop ? nv - 1 - i : i]];
        const int ys = r.ys, xs = r.xs;
        const int rowIn = sline - r.y;
        if (rowIn < 0 || rowIn >= (16 << ys)) continue;
        if (++rendered > 100) { sprFlags |= 0x02; break; } // per-line overflow

        // Paper x of pixel 0. x > 319 is left of the display; a sprite whose
        // pixels run past 319 is off the right edge either way, so the 512 wrap
        // needs no per-pixel test.
        const int x0 = (r.x > 319 ? r.x - 512 : r.x) - 32;
        const int width = 16 << xs;
        int p0 = lo - x0, p1 = hi - x0;            // px range, inclusive
        if (p0 < 0) p0 = 0;
        if (p1 > width - 1) p1 = width - 1;
        if (p0 > p1) continue;

        // The 16 pixels of this sprite row as 9-bit colours, mirror/rotate and
        // the palette applied; 0xFFFF = transparent. Unrotated rows read the
        // pattern row in order (NEXTEST: 109 sprites, ~13 on a line, sprites
        // 5.4 ms of the frame's render with the per-pixel mirror/rotate loop).
        uint16_t row[16];
        uint8_t v = rowIn >> ys;
        if (r.flags & SR_YM) v = 15 - v;
        const uint8_t* pat = sprPatterns + r.pat;
        const bool xm = r.flags & SR_XM, rot = r.flags & SR_ROT;
        const bool b4 = r.flags & SR_4BPP;
        const uint8_t tr = b4 ? (uint8_t)(transp & 0x0F) : transp;
        if (!rot) {
            if (b4) {
                const uint8_t* pr = pat + v * 8;
                for (int k = 0; k < 8; k++) {
                    const uint8_t b = pr[k], h = b >> 4, l = b & 0x0F;
                    row[2 * k]     = (h == tr) ? 0xFFFF : (uint16_t)(sprRaw[h | r.pal] & 0x1FF);
                    row[2 * k + 1] = (l == tr) ? 0xFFFF : (uint16_t)(sprRaw[l | r.pal] & 0x1FF);
                }
            } else {
                const uint8_t* pr = pat + v * 16;
                for (int u = 0; u < 16; u++) {
                    const uint8_t px = pr[u];
                    row[u] = (px == tr) ? 0xFFFF : (uint16_t)(sprRaw[(uint8_t)(px + r.pal)] & 0x1FF);
                }
            }
            if (xm)
                for (int u = 0; u < 8; u++) { const uint16_t t = row[u]; row[u] = row[15 - u]; row[15 - u] = t; }
        } else {
            for (int u = 0; u < 16; u++) {
                const int su = xm ? 15 - u : u;
                const int uu = v, vv = 15 - su;
                uint8_t px;
                if (b4) { const uint8_t b = pat[vv * 8 + (uu >> 1)]; px = (uu & 1) ? (b & 0x0F) : (b >> 4); }
                else px = pat[vv * 16 + uu];
                const uint8_t ci = b4 ? (uint8_t)(px | r.pal) : (uint8_t)(px + r.pal);
                row[u] = (px == tr) ? 0xFFFF : (uint16_t)(sprRaw[ci] & 0x1FF);
            }
        }

        if (xs == 0) {
            // Unscaled (nearly every sprite): the opaque pixels of the row as a
            // mask, clipped to [p0, p1]; one word for the coverage, one store
            // per set bit.
            uint32_t m = 0;
            for (int u = 0; u < 16; u++) if (row[u] != 0xFFFF) m |= 1u << u;
            m = (m >> p0) & ((2u << (p1 - p0)) - 1u);
            if (!m) continue;
            const unsigned b0 = (unsigned)(x0 + 32 + p0);     // >= 0: lo >= -32
            const unsigned sh = b0 & 31;
            const uint32_t w0 = m << sh;
            collided |= coverage[b0 >> 5] & w0;
            coverage[b0 >> 5] |= w0;
            if (sh > 16) {
                const uint32_t w1 = m >> (32 - sh);
                collided |= coverage[(b0 >> 5) + 1] & w1;
                coverage[(b0 >> 5) + 1] |= w1;
            }
            uint16_t* const o = fbp + x0 + p0;
            const uint16_t* const rp = row + p0;
            while (m) {
                const int b = __builtin_ctz(m);
                o[b] = rp[b];
                m &= m - 1;
            }
            nxRowSpr = true;
            continue;
        }
        for (int px = p0; px <= p1; px++) {
            const uint16_t c = row[px >> xs];
            if (c == 0xFFFF) continue;
            const int xp = x0 + px;
            const unsigned cbit = (unsigned)(xp + 32);
            const uint32_t mask = 1u << (cbit & 31);
            collided |= coverage[cbit >> 5] & mask;
            coverage[cbit >> 5] |= mask;
            fbp[xp] = c;
            nxRowSpr = true;
        }
    }
    if (collided) sprFlags |= 0x01;
    if (nxRowSpr) {
        // the opaque span, from the coverage bits (bit = paper x + 32): the
        // mixer resolves the sprite layer only there (nxMixOrder)
        int k0 = 0, k1 = 10;
        while (k0 < 11 && !coverage[k0]) k0++;
        while (k1 > k0 && !coverage[k1]) k1--;
        if (k0 < 11) {
            const int b0 = k0 * 32 + __builtin_ctz(coverage[k0]);
            const int b1 = k1 * 32 + 31 - __builtin_clz(coverage[k1]);
            nxSprX0 = (int16_t)(b0 - 32 + xPaperOff);
            nxSprX1 = (int16_t)(b1 - 32 + xPaperOff + 1);
        }
    }
#if NEXT_TRACE
    nxSprNote(2, (uint32_t)nv); nxSprNote(3, (uint32_t)rendered);
#endif
}



// ===== .pss snapshot (PSNV / PSNA) =====
// Flash code: the menu / a hot key / the boot resume only.
#include "app/SnapPack.h"
static constexpr uint8_t NXV_SNAP_VER = 1;

void NEXTVID::snapPrepare() {
    if (!sprPatterns) return;
    c1Drain();
    palApply(pal_w);                      // rawPal == cRaw
    sprApplyTo(*sprR, spr_app, spr_w);    // pattern bytes live only in the log until applied
}

uint8_t* NEXTVID::snapArea(uint8_t id, uint32_t& size) {
    size = 0;
    if (!sprPatterns) return nullptr;
    switch (id) {
        case SNAP_PAL:     size = 8 * 256 * 2; return (uint8_t*)cRaw;
        case SNAP_SPRATTR: size = 128 * 5;     return (uint8_t*)sprAttr;
        case SNAP_SPRPAT:  size = 16384;       return sprPatterns;
        case SNAP_COPPER:  size = 2048;        return copperMem;
        default:           return nullptr;
    }
}

uint32_t NEXTVID::snapSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(NXV_SNAP_VER);
    o.u8(palIdx); o.u8(pal9Second ? 1 : 0); o.u8(pal9First); o.u8(palCtrl);
    o.raw(clip, 16); o.raw(clipIdx, 4);
    o.u16(sprPatWrite); o.u8(sprAttrSlot); o.u8(sprAttrByte); o.u8(sprFlags); o.u8(sprNrSlot);
    o.u16(copIndex); o.u16(copPC); o.u8(copCtrl); o.u8(cop16Second ? 1 : 0); o.u8(cop16First);
    return o.n;
}

void NEXTVID::snapLoadBegin() {
    if (!sprPatterns) return;
    c1Drain();
    palApply(pal_w);
    sprApplyTo(*sprR, spr_app, spr_w);
    // Both logs are now fully applied (app == w), so nothing can replay over the
    // areas the load writes in place. core0's sprite copy is re-taken from core1's.
    s_sprR0Valid = 0;
}

void NEXTVID::snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn s(in, n);
    if (s.u8() < 1) return;
    const uint8_t pi = s.u8(), p9s = s.u8(), p9f = s.u8(), pc = s.u8();
    uint8_t cl[16], ci[4];
    s.raw(cl, 16); s.raw(ci, 4);
    const uint16_t spw = s.u16();
    const uint8_t sas = s.u8(), sab = s.u8(), sf = s.u8(), sns = s.u8();
    const uint16_t cix = s.u16(), cpc = s.u16();
    const uint8_t cct = s.u8(), c16s = s.u8(), c16f = s.u8();
    if (!s.ok) return;
    palIdx = pi; pal9Second = p9s != 0; pal9First = p9f; palCtrl = pc;
    memcpy(clip, cl, 16);
    for (int w = 0; w < 4; w++) clipIdx[w] = ci[w] & 3;
    sprPatWrite = spw & 0x3FFF; sprAttrSlot = sas & 0x7F; sprAttrByte = sab < 5 ? sab : 0;
    sprFlags = sf; sprNrSlot = sns & 0x7F;
    copIndex = cix & 0x7FF; copPC = cpc & 0x3FF; copCtrl = cct;
    cop16Second = c16s != 0; cop16First = c16f;
}

void NEXTVID::snapLoadEnd() {
    if (!sprPatterns) return;
    // Palettes: the renderer's copy, the CLUT LUTs and the derived flags.
    memcpy(rawPal, cRaw, 8 * 256 * 2);
    for (int p = 0; p < 8; p++) {
        bool prio = false;
        for (int i = 0; i < 256; i++) {
            prio |= (rawPal[p][i] & 0x8000) != 0;
            lut[p][i] = remap[rawPal[p][i] & 0x1FF];
        }
        palHasPrio[p] = prio;
        nxPalDirty((uint8_t)p);
    }
    // Sprites: the renderer resolves all 128 against their anchors again.
    memcpy(sprR->attr, sprAttr, 128 * 5);
    memset(sprR->res, 0, sizeof sprR->res);
    sprR->n = 0; sprR->dirty = 1; sprR->lo = 0; sprR->hi = 127; sprR->applying = 0;
    // The raster may have changed (NextReg::snapLoad applied it).
    totalLines = CPU::statesInFrame ?
        ((CPU::statesInFrame >> ESPectrum::multiplicator) / VIDEO::tStatesPerLine) : 311;
    computeGeometry();
    xPaperOff = ((int)VIDEO::vga.xres - 256) / 2;
}

void NEXTVID::snapLineSync() {
    const uint32_t tl = (uint32_t)VIDEO::tStatesPerLine << ESPectrum::multiplicator;
    if (!tl) return;
    lineIdx = CPU::tstates / tl;
    if (lineIdx > totalLines) lineIdx = totalLines;
    // Inside the line: past its paper start = the line-end boundary is next.
    s_capPhase = (CPU::tstates - lineIdx * tl) >= ((uint32_t)s_pStartT << ESPectrum::multiplicator) ? NX_CAP_GROUPS : 0;
    s_capY = -1;
    nxLineArm();
}
