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
    uint32_t mipCells, mipBuilds, mipBuildUs, mipResets, mipRefused;   // pre-scaled cache: cells blitted from it, tiles built (+us), pool resets, cells it declined
    uint32_t pxMip;               // blit AREA served from the cache
    uint32_t drawUs;              // time inside drawBitmap (all cells, blits included)
    uint32_t mipGetUs;            // time inside the cache lookup / validation / build
    uint32_t mipBlitUs;           // time inside blits served from the cache
    uint32_t mipParked;           // entries parked for rebuilding on three consecutive frames
    uint32_t genPx;               // blit AREA that took the GENERIC loop (neither fast path), all cells
    uint32_t mipGenPx;            // ...the cached-cell part of it
    uint32_t mipGenReason[8];     // genPx by cause: bilinear, box, wrap, non-default blend, non-white colour, rotated, mirrored (A <= 0), paletted with no palette slot
    uint32_t loopUs;              // time inside the band's fetch/exec loop (all words; drawUs and vtxUs are inside it)
    uint32_t vtxUs;               // time inside vertex() (the front, the setup and the blit of every vertex)
    uint32_t ctxCopies;           // SAVE_CONTEXT + RESTORE_CONTEXT words
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
    const uint16_t* pageGen;      // per-4 KB-page RAM_G write counters (Ft812::ramgPageGen), or nullptr: the bitmap cache's validity
    // RAM_G itself, for the per-cell resolve: `view()` is a flash function — one
    // call per cell was ~0.5 us of instruction misses under core0's PSRAM traffic
    // (hw 2026-10-05). nullptr = resolve everything through view().
    const uint8_t* ramg; uint32_t ramgBytes;
};

// ── pre-scaled bitmap cache ──────────────────────────────────────────────────
// Every bitmap cell the games draw is MINIFIED here (1024x768 -> 320x240 is 3.2
// texels per output pixel; the 1.6x matrix both ZUMA and R-Type use makes it 2.0),
// so a nearest blit reads one PSRAM line per 2-3 output pixels at random, and a
// filtered one four times that. The cache keeps, per (cell, scale, filter), the
// cell ALREADY SCALED to the output grid — an area (box) average or the centre
// texel — as ARGB4444 in PSRAM, read 1:1 and sequentially by the blit: one line
// fill per four pixels, no palette lookup, no per-pixel filter, and the picture
// is a proper downscale instead of every other texel. Rotated cells (ZUMA's
// balls) keep their matrix, scaled onto the cache image.
//
// Layout: one block, carved by ft812MipInit — the MipCache header, a 256-way
// hash of entry chains, `cap` entries, then the pool. An entry's image is cut into
// TILES of MIP_TILE_ROWS cache rows, each with the sum of the RAM_G page
// generations its source rows (and palette) spanned when it was built: a strip
// copied into one corner of R-Type's 614 KB tile layer rebuilds the one or two
// tiles it touched, not the layer. The pool is a bump allocator; when it runs
// out the whole cache is dropped and refilled (counted — two drops in one frame
// turn the cache off for the rest of that frame, so a working set larger than
// the pool costs the uncached path and not a rebuild storm).
constexpr int      MIP_TILE_SHIFT = 3;
constexpr int      MIP_TILE_ROWS  = 1 << MIP_TILE_SHIFT;
constexpr uint32_t MIP_RAMG_SIZE  = 0x100000;        // only RAM_G cells are cached
constexpr uint32_t MIP_MAX_SCALE8 = 8 << 8;          // past 8 texels per pixel the box is too wide to build cheaply
// RAM_G write-generation pages: 1 KB. 4 KB was too coarse for R-Type, whose sprite
// slots (512 B) are refilled every frame and share pages with cells that do not
// change (hw 2026-10-05: ~1300 tile rebuilds a second).
constexpr uint32_t MIP_PAGE_SHIFT = 10;
constexpr uint32_t MIP_PAGES      = MIP_RAMG_SIZE >> MIP_PAGE_SHIFT;
constexpr uint16_t MIP_SKIP_FRAMES = 32;             // a cell rebuilt on 3 consecutive frames is left uncached this long
struct MipTile { uint8_t* data; uint32_t gen; };
struct MipEntry {
    uint32_t src;                 // cell address (22-bit): BITMAP_SOURCE + cell * stride * lh
    uint32_t k1;                  // stride:12 | lw:12 | fmt:5 | wrapx:1 | wrapy:1 | smooth:1
    uint32_t pal;                 // PALETTE_SOURCE for the paletted formats, else 0
    uint16_t su8, sv8;            // texels per cache pixel, Q8
    uint16_t cw, ch;              // the cache image
    uint16_t lh, ntiles;
    uint16_t next;                // hash chain (entry index), 0xFFFF = end
    uint16_t lastBuild;           // frame of the last REbuild of an existing tile
    uint16_t skipFrom;            // frame the entry was parked (see MIP_SKIP_FRAMES)
    uint8_t  streak;              // consecutive frames with a rebuild
    uint8_t  skip;                // parked: served from the source until skipFrom + MIP_SKIP_FRAMES
    MipTile* tiles;               // ntiles, in the pool
};
struct MipCache {
    uint8_t*  pool; uint32_t poolBytes, poolUsed;
    MipEntry* ent; uint16_t cap, nent;
    uint8_t   resetsThisFrame;    // see above
    uint8_t   off;                // cache declined for the rest of this frame
    uint32_t  frame;              // frames rendered (the parking clock)
    uint16_t  hash[256];
};
// Carve a cache out of `block` (`bytes`); nullptr when it cannot hold at least a
// small pool. `cap` entries (R-Type's working set is ~100 cells).
MipCache* ft812MipInit(void* block, size_t bytes, int cap = 192);
void      ft812MipReset(MipCache& mc);   // drop every entry (a chip reset, a filter change)
uint32_t  ft812MipUsed(const MipCache& mc);

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
    // "Smooth": every bitmap cell that is MINIFIED (>= 1.25 texels per output
    // pixel on an axis — at our 5/16 scale that is nearly everything) is AREA
    // filtered (a box over the pixel's footprint) instead of taking one texel.
    // With a MipCache the box is computed once per cell into the cache; without
    // one it is approximated per pixel with four taps (four fetches per pixel).
    // Off = nearest (Fast) — also served from the cache when one is there.
    bool smooth;
    MipCache* mip;                // the pre-scaled bitmap cache, or nullptr
    // The band walk's working state (ft812RenderScratchBytes(), 8-aligned, SRAM):
    // it used to be a stack local and overflowed core1's 2 KB stack. Required —
    // ft812RenderBand draws nothing without it.
    void* scratch;
};
size_t ft812RenderScratchBytes();

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
    // What cfg.palScratch holds: up to 4 expanded PALETTE_SOURCE tables of 1 KB
    // each (R-Type changes palettes per sprite — ~150 expansions a frame with one
    // slot), replaced least-recently-used.
    struct PalSlot { uint32_t addr, gen; uint16_t age; uint8_t fmt, valid; } pal[4];
    uint16_t     palAge;
};
void ft812PalCacheInvalidate(RenderState& st);

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
    uint8_t lutm[2][256 + 64];    // the R and G levels times their weight in the cube index (the quantizer's hot loop)
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
// The MJPEG layout: Y 6 bits (step 4), Cb and Cr 3 bits each (step 16, centres
// 64..176 — 128 is bin 4's centre). The eye resolves brightness far better than
// colour, so that is where the 12 bits go (host, 2026-10-01: luma error after a
// 3x3 blur 2.4, against 3.3 for Y5/Cb3/Cr4 and 5.2 for a 4-4-4 split).
// The chroma STEP was 32 first, and the sink dithers chroma by one step: +-32 of
// Cb is +-57 of blue in RGB, so a dark bluish film (Cb ~140) came out as a field
// of blue dots (hw 2026-10-01, adaptps3: "много синих точек"). Step 16 halves the
// per-pixel error; film chroma beyond +-56 of neutral is rare and clips to the
// last bin. Host, four frame pairs of the Equilibrium trailer rendered through a
// palette built from a frame 8 earlier: pixels >24 too blue 1.6-3.5% -> 0%, mean
// chroma error 13.2 -> 6.2 on the most saturated scene.
#ifndef FT812_ADAPT_YCC_LAYOUT
#define FT812_ADAPT_YCC_LAYOUT { 6, 3, 64, 8, 8, 4, 16, 16, 2, 64, 64, 2 }
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
void ft812AdaptAccumulate(AdaptPal& ap, const uint32_t* src, int w, int step = 1);   // one band row (every step-th pixel)
bool ft812AdaptChanged(const AdaptPal& ap, int permille);                     // hist vs the built one
// Median cut of ap.hist into `slots` entries (black + slots-1 colours) -> lut/col.
void ft812AdaptBuild(AdaptPal& ap, int slots);
// Quantize one row through lut and the hardware slot of each entry (slotOf[i]).
// No dither: the entries are the frame's own colours, and a 4-4-4 map cannot
// tell which neighbour a dithered pixel should mix with (see the .cpp).
// (lut entries of bins the measured frame did not use are resolved on first use: 0xFF = not yet)
// `lut`: an SRAM copy of ap.lut (the map is read per pixel and AdaptPal lives in
// PSRAM on the device); nullptr = read ap.lut itself. A bin resolved on the way
// is written into both.
void ft812QuantizeRowAdapt(AdaptPal& ap, const uint8_t* slotOf, const uint32_t* src, int w, int y, uint8_t* dst, uint8_t* lut = nullptr);
uint8_t ft812AdaptResolve(AdaptPal& ap, uint32_t bin);

} // namespace Ft812
