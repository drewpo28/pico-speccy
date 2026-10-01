// FT812 display-list rasterizer — the graphics engine half of Ft812.
//
// A display list is a stream of 32-bit words (opcode in bits 31..24, VERTEX2F =
// 0x40, VERTEX2II = 0x80). The real chip scans it once per DISPLAY LINE; this
// port walks it once per BAND of output rows (ft812RenderBand), rasterizing
// only what touches the band into an ARGB8888 band buffer in SRAM, so the whole
// frame never exists in memory at once. The output is SCALED: FT screen pixels
// map onto the emulator framebuffer through sxQ16/syQ16 (1024x768 → 320x240 is
// exactly 5/16), and every primitive is sampled at output-pixel centres.
//
// Deliberate deviations (all logged once by the chip model, not here):
//   - no stencil, no tag buffer, no anti-aliasing on bitmap edges; POINTS,
//     LINES and RECTS get a 1-px coverage ramp;
//   - TEXT8X8 / TEXTVGA / BARGRAPH formats draw nothing (they need the chip's
//     own 8x8 ROM glyphs);
//   - CALL/JUMP take the destination as a DL WORD index.
// Everything else — bitmap formats incl. the PALETTED* family through
// PALETTE_SOURCE, BILINEAR filtering, BORDER/REPEAT wrap, the 8.8/15.8 transform
// matrix, SAVE/RESTORE_CONTEXT (4 deep), CALL/RETURN (4 deep), MACRO, SCISSOR,
// ALPHA_FUNC, BLEND_FUNC with all six factors, COLOR_MASK, CLEAR with clear
// colour + alpha, VERTEX_FORMAT and VERTEX_TRANSLATE — follows the FT81x
// Programmer's Guide.
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifndef FT812_TRACE
#define FT812_TRACE 0                 // the host test and any TU built outside CMake
#endif

namespace Ft812 {

// ── the FT812_TRACE render meter ─────────────────────────────────────────────
// Advanced by the rasterizer (core1) in a trace build only — every increment is
// behind FTR(), so a plain build pays nothing in the per-pixel loops. Read and
// differenced by VIDEO::ftTraceTick (Video.cpp) every 60 frames.
struct RenderStats {
    uint32_t bands;               // ft812RenderBand calls (one per band of fb rows)
    uint32_t words;               // display-list words walked (all bands)
    uint32_t bitmaps;             // bitmap cells drawn (any format)
    uint32_t pxNearest, pxBilinear;   // output-pixel AREA of bitmap blits, by filter
    uint32_t pxPrim;              // coverage-ramped pixels (points/lines/rects/edges)
    uint32_t clears, points, lines, rects, edges;
    uint32_t bmpFmt[18];          // bitmap cells by format (F_* index)
    uint32_t pxFmt[18];           // blit AREA by format
    uint32_t usFmt[18];           // time inside blitBitmap by format (cfg.clockUs)
    uint32_t palCopies;           // PALETTE_SOURCE tables copied into palScratch
};
extern RenderStats g_renderStats;
#if FT812_TRACE
#define FTR(stmt) do { stmt; } while (0)
#else
#define FTR(stmt) ((void)0)
#endif

constexpr uint32_t RAM_DL_WORDS = 2048;

// What the renderer reads. `view(addr, &avail)` resolves a 22-bit chip address
// to bytes (RAM_G, the ROM font glyphs) — Ft812::memView on the device.
struct MemView {
    const uint8_t* (*view)(uint32_t addr, uint32_t* avail);
    const uint32_t* dl;           // 2048 words
    uint32_t macro[2];            // REG_MACRO_0/1
    uint32_t (*gen)();            // RAM_G write generation (Ft812::ramgGen), or nullptr: the palette cache key
};

// Output geometry: FT screen (hsize x vsize) → output rectangle (outW x outH),
// scale = out px per FT px (Q16), inv = FT px per out px (Q16).
struct RenderCfg {
    const MemView* mem;
    int hsize, vsize;
    int outW, outH;
    uint32_t sxQ16, syQ16, invXQ16, invYQ16;
    // An SRAM copy of the PALETTE_SOURCE table (nullptr = read it in place). The
    // texel stream evicts the palette out of the XIP cache, so a PALETTED* pixel
    // cost TWO PSRAM line fills; copied once per (address, RAM_G generation) it
    // costs one (hw 2026-09-28: ZUMA is PALETTED4444 almost throughout).
    uint8_t* palScratch;
    uint32_t palScratchSize;
    uint64_t (*clockUs)();        // trace build only: per-format blit time (nullptr = none)
    // "Smooth": every bitmap cell that is MINIFIED (>= 1.5 texels per output
    // pixel on an axis — at our 5/16 scale that is nearly everything) is sampled
    // with four taps spread over the pixel's footprint instead of one texel.
    // Four texel fetches per pixel instead of one. Off = nearest (Fast).
    bool smooth;
};

// Per-handle bitmap parameters. They are graphics-ENGINE state, not graphics-
// CONTEXT state: SAVE/RESTORE_CONTEXT does not touch them and they persist
// across display lists — the ROM font handles 16..31 are pre-set at chip reset
// and stay until a list overrides them.
struct BitmapHandle {
    uint32_t source;              // 22-bit
    uint16_t stride;              // BITMAP_LAYOUT linestride (+ _H bits), bytes
    uint16_t lh;                  // layout height, lines
    uint16_t sw, sh;              // BITMAP_SIZE, screen pixels
    uint8_t  fmt;
    uint8_t  filter, wrapx, wrapy;
};

// Engine state that outlives one band walk (the handles). One per chip.
struct RenderState {
    BitmapHandle handle[32];
    uint32_t     warned;          // bit per unsupported feature already reported
    uint32_t     palCacheAddr, palCacheGen, palCacheLen;   // what cfg.palScratch holds
    bool         palCacheValid;
};

// Reset the engine state: every handle to the ROM font defaults (handles 16..31)
// or empty. `romFontHandle(h, out)` fills the default for a ROM font handle.
void ft812RenderStateReset(RenderState& st, void (*romFontHandle)(int handle, BitmapHandle* out));

// Render output rows [row0, row1) of the frame into `band` (ARGB8888,
// cfg.outW pixels per row, row-major; row 0 of the band = output row `row0`).
// The band starts black/transparent; CLEAR in the list is what paints it.
void ft812RenderBand(const RenderCfg& cfg, RenderState& st, int row0, int row1, uint32_t* band);

// ── output quantizer ─────────────────────────────────────────────────────────
// The framebuffer is 8bpp palette indices; the FT812 output is truecolour.
// The colours are quantized onto a fixed RGB cube laid out over the hardware
// slots the caller programmed (`slot[]` in cube order r*gl*bl + g*bl + b), with a
// 4x4 ordered dither so gradients do not band. Levels per channel are chosen by
// the pool size: 6x6x5 = 180 slots, or 6x8x5 = 240.
struct PalLut {
    uint8_t rl, gl, bl;           // levels per channel
    uint8_t lut[3][256 + 64];     // channel value + dither → level
    uint8_t slot[240];            // cube index → hardware palette slot
};
void ft812PalLutInit(PalLut& p, int poolSlots, const uint8_t* poolSlot);
// The RGB888 colour of cube entry i (for programming the hardware palette).
uint32_t ft812PalLutColor(const PalLut& p, int i);
// Quantize one band row (w pixels, output row y for the dither phase) into
// palette indices, natural x order.
// x0 = the output column of src[0] (dither phase), for callers that quantize a span.
void ft812QuantizeRow(const PalLut& p, const uint32_t* src, int w, int y, uint8_t* dst, int x0 = 0);

// ── adaptive palette ─────────────────────────────────────────────────────────
// The fixed 6x6x5 cube dithers every gradient into a checkerboard (hw 2026-09-28,
// ZUMA's sky). Instead: a 4-4-4 histogram of the frame, a median cut into the
// pool's slots, and a 4096-entry bin -> entry map for the quantizer. Rebuilt only
// when the frame's colour content moves (see ft812AdaptChanged).
//
// The framebuffer is single-buffered indices, so a rebuild is visible: the rows
// on screen were written through the OLD map and show under the NEW colours
// until they are rendered again. Two rules keep that from being garbage:
//  - entry 0 is ALWAYS black (it is the letterbox / blank-frame index, and the
//    cube's entry 0 is black too, so that index means the same in every palette);
//  - a rebuilt palette is matched onto the previous one (each new colour takes
//    the index of the nearest old colour), so an old-map row is approximately
//    right under the new palette instead of arbitrary.
// ~17 KB, all SRAM (the map is read per pixel).
enum { ADAPT_RGB444 = 0, ADAPT_YCC633 = 1 };   // bin layouts, see Ft812Render.cpp (the second is "the YCC one", whatever its bit split)
// A bin layout: bin = a << sa | b << sb | c; axis sizes; value units per bin; the
// value of bin 0's centre on each axis; the weight of axis a when a box is split.
struct AdaptSpace { uint8_t sa, sb, na, nb, nc, ua, ub, uc, oa, ob, oc, wa; };
// The MJPEG layout: Y 6 bits (step 4), Cb and Cr 3 bits each (step 32, centred on
// 128). The eye resolves brightness far better than colour, so that is where the
// 12 bits go. Picked on real frames of a film (host, 2026-10-01): mean luma error
// after a 3x3 blur 2.4, against 3.3 for Y5/Cb3/Cr4 and 5.2 for a 4-4-4 split; the
// chroma error is ~1.5 for all of them once the dither is averaged.
#ifndef FT812_ADAPT_YCC_LAYOUT
#define FT812_ADAPT_YCC_LAYOUT { 6, 3, 64, 8, 8, 4, 32, 32, 2, 0, 0, 2 }
#endif
const AdaptSpace& ft812AdaptSpace(int space);
struct AdaptPal {
    enum { MAX = 185 };           // entry 0 (black) + 184 median-cut boxes
    uint8_t  space, histSpace;    // the layout lut/coarse were built in / the one hist is being filled in
    uint16_t hist[4096];          // this frame's histogram (saturating), bins per histSpace
    uint16_t coarse[512];         // 3-3-3 fold of the histogram the palette was built from
    uint32_t total;               // pixels in hist
    uint8_t  lut[4096];           // bin -> palette entry, 0xFF = not resolved yet (ft812AdaptResolve); (core1 builds it between frames, so one buffer)
    uint32_t col[MAX];            // palette colours RGB888 (core0 programs them from ftFrameTick)
    uint32_t ncol[MAX];           // build scratch: the new colours before they are matched onto col[]
    uint8_t  used[MAX];           // build scratch: entry already taken by the matching
    int      n;                   // entries in use (incl. entry 0)
    bool     built;               // a palette exists
    struct Box { uint8_t r0, r1, g0, g1, b0, b1; uint32_t count; } box[MAX - 1];   // median-cut scratch (here, not .bss)
};
void ft812AdaptClear(AdaptPal& ap, int space = ADAPT_RGB444);                 // start of a frame
void ft812AdaptAccumulate(AdaptPal& ap, const uint32_t* src, int w);          // one band row
bool ft812AdaptChanged(const AdaptPal& ap, int permille);                     // hist vs the built one
// Median cut of ap.hist into `slots` entries (black + slots-1 colours) -> lut/col.
void ft812AdaptBuild(AdaptPal& ap, int slots);
// Quantize one row through lut and the hardware slot of each entry (slotOf[i]).
// No dither: the entries are the frame's own colours, and a 4-4-4 map cannot
// tell which neighbour a dithered pixel should mix with (see the .cpp).
// (lut entries of bins the measured frame did not use are resolved on first use: 0xFF = not yet)
void ft812QuantizeRowAdapt(AdaptPal& ap, const uint8_t* slotOf, const uint32_t* src, int w, int y, uint8_t* dst);
uint8_t ft812AdaptResolve(AdaptPal& ap, uint32_t bin);

} // namespace Ft812
