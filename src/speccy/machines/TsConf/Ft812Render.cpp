// FT812 display-list rasterizer. See Ft812Render.h for the model and the
// deviations. Host-testable: no firmware dependency (tools/ft812_test.cpp).
#include "Ft812Render.h"
#include <string.h>
#include <math.h>

// Hot code placement: the whole per-pixel path — the walk, the blits, the
// primitives, the blend — ran from FLASH through the same XIP cache the texel
// stream thrashes (hw 2026-09-28: 0.7 us per output pixel with ~0.5 PSRAM line
// fills, i.e. mostly INSTRUCTION fetches). FT812_RENDER_IN_RAM (CMake) puts it
// in SRAM; the host test has no pico headers and keeps it plain.
#if FT812_RENDER_IN_RAM && !defined(FT812_HOST_TEST)
#include "pico.h"
#define FT_HOT __not_in_flash("ft812")
#else
#define FT_HOT
#endif
// The output quantizer alone (every pixel of every frame goes through it) rides in
// the VDAC2 code overlay window (.ftovl): it is ~100 bytes.
#if VDAC2_CODE_OVERLAY && !defined(FT812_HOST_TEST)
#include "app/CodeOverlay.h"
#define FT_HOT_Q FT_OVL_CODE
#elif FT812_RENDER_IN_RAM && !defined(FT812_HOST_TEST)
#include "pico.h"
#define FT_HOT_Q __not_in_flash("ft812q")
#else
#define FT_HOT_Q
#endif
// The per-pixel helpers MUST inline: at -Os GCC left texel<>/bilerp out of line,
// called through veneers from the blit loop — one call into flash per pixel,
// which the texel stream's XIP misses turned into an instruction fetch each
// (hw 2026-09-28: 0.7 us per output pixel for L4 with one data fill per 8 px).
#define FT_INLINE static inline __attribute__((always_inline))
// Trace timing inside the walk: the raw 32-bit microsecond timer, read INLINE.
// The first cut went through cfg.clockUs() — a function pointer to time_us_64()
// in FLASH — four times per blitted cell and twice per vertex, i.e. ~100k flash
// calls a window under the texel stream: the trace itself charged ~1 us to every
// vertex that merely clipped out and "draw minus blit" read 107 ms a window
// (hw 2026-10-06, mip6). The host test has no timer and keeps cfg.clockUs.
#if defined(FT812_HOST_TEST) || !__has_include("hardware/timer.h")
#define FT_RAW_TICK 0
#else
#include "hardware/timer.h"
#define FT_RAW_TICK 1
#endif
// The band WALK — the list interpreter, the vertex clip, the band clear — runs
// once per DL word / vertex per band (ZUMA: 17.7k words and 1900 vertices a
// frame) and was FLASH code: on a VDAC2 session core0 keeps the XIP queue full
// of guest PSRAM traffic, so every instruction-cache miss on this long straight
// path cost ~0.4 us — measured ~8 us per drawBitmap call that merely clipped out,
// ~30 ms a frame in total (hw 2026-10-05, mip3). It rides in the VDAC2 overlay
// window now; the per-cell SETUP of a cell that does draw (drawBitmapCold) and
// the blits stay where they were.
#if VDAC2_CODE_OVERLAY && !defined(FT812_HOST_TEST)
#define FT_WALK_HOT FT_OVL_CODE
#else
#define FT_WALK_HOT FT_HOT
#endif

namespace Ft812 {

RenderStats g_renderStats = {};

#if FT_RAW_TICK
FT_INLINE uint32_t ftTick(const RenderCfg&) { return timer_hw->timerawl; }
#else
FT_INLINE uint32_t ftTick(const RenderCfg& c) { return c.clockUs ? (uint32_t)c.clockUs() : 0; }
#endif

// ── fixed-point helpers ──────────────────────────────────────────────────────
static inline int32_t mulQ16(int32_t a, uint32_t bQ16) { return (int32_t)(((int64_t)a * (int64_t)bQ16) >> 16); }
static inline int32_t ceilQ16(int32_t v) { return (v + 0xFFFF) >> 16; }
// First / one-past-last output pixel whose CENTRE lies in the FT-px range [x0, x1) (Q16).
static inline int outMin(int32_t x0Q16, uint32_t sQ16) { return ceilQ16(mulQ16(x0Q16, sQ16) - 0x8000); }
static inline int outEnd(int32_t x1Q16, uint32_t sQ16) { return ceilQ16(mulQ16(x1Q16, sQ16) - 0x8000); }
FT_INLINE uint32_t mul255(uint32_t a, uint32_t b) { return (a * b + 128) * 257 >> 16; }
FT_INLINE uint32_t clamp255(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint32_t)v); }

// ── graphics context (Table 5 of the FT81x PG) ───────────────────────────────
struct Ctx {
    uint32_t color;          // AARRGGBB
    uint32_t clearColor;     // AARRGGBB
    uint8_t  handle, cell;
    uint8_t  alphaFunc, alphaRef;
    uint8_t  blendSrc, blendDst;
    uint8_t  colorMask;      // b3 R, b2 G, b1 B, b0 A
    uint8_t  vfmt;
    uint16_t lineWidth;      // 1/16 px
    uint16_t pointSize;      // 1/16 px
    int32_t  scX, scY, scW, scH;
    int32_t  vtx, vty;       // VERTEX_TRANSLATE, 1/16 px
    int32_t  A, B, C, D, E, F;   // A B D E 8.8, C F 15.8
    uint32_t palSrc;
};

static void ctxDefault(Ctx& c) {
    c.color = 0xFFFFFFFF;
    c.clearColor = 0x00000000;
    c.handle = 0; c.cell = 0;
    c.alphaFunc = 7; c.alphaRef = 0;
    c.blendSrc = 2; c.blendDst = 4;
    c.colorMask = 0x0F;
    c.vfmt = 4;
    c.lineWidth = 16; c.pointSize = 16;
    c.scX = 0; c.scY = 0; c.scW = 2048; c.scH = 2048;
    c.vtx = 0; c.vty = 0;
    c.A = 256; c.B = 0; c.C = 0; c.D = 0; c.E = 256; c.F = 0;
    c.palSrc = 0;
}

enum { PR_NONE = 0, PR_BITMAPS = 1, PR_POINTS = 2, PR_LINES = 3, PR_LINE_STRIP = 4,
       PR_EDGE_R = 5, PR_EDGE_L = 6, PR_EDGE_A = 7, PR_EDGE_B = 8, PR_RECTS = 9 };

enum WarnBit { W_STENCIL = 1, W_TEXTFMT = 2, W_PRIM = 4, W_DEPTH = 8 };

struct Walk {
    const RenderCfg* cfg;
    RenderState*     st;
    BitmapHandle     hnd[32];    // working copy for this band
    uint32_t*        band;
    int row0, row1;              // output rows of this band
    Ctx ctx;
    Ctx stack[4]; int sp;
    uint16_t callStack[4]; int csp;
    int prim;
    int32_t vx[2], vy[2]; int nv;        // vertex accumulator (Q16 FT px)
    int clipX0, clipX1, clipY0, clipY1;  // out px, absolute rows
    uint32_t genNow;                     // RAM_G write generation at band start (the palette cache key; one gen() call per band, not per cell)
    // drawBitmap's working structures live HERE, not on the stack: the renderer
    // runs on core1, whose 2 KB stack (SCRATCH_X) also takes the HDMI line ISR's
    // frames, and this Walk (~1 KB) plus a BlitArgs and the cache descriptor on
    // the stack overflowed it into the ISR's data — core1 dead, isr% to 0
    // (hw 2026-10-05, mip2 at 576p). RenderCfg::scratch supplies the Walk.
    alignas(8) uint8_t argsBuf[192];     // BlitArgs
    alignas(8) uint8_t msrcBuf[64];      // MipSrc
};

static FT_WALK_HOT void clipRecalc(Walk& w) {
    const RenderCfg& c = *w.cfg;
    int x0 = outMin(w.ctx.scX << 16, c.sxQ16), x1 = outEnd((w.ctx.scX + w.ctx.scW) << 16, c.sxQ16);
    int y0 = outMin(w.ctx.scY << 16, c.syQ16), y1 = outEnd((w.ctx.scY + w.ctx.scH) << 16, c.syQ16);
    if (x0 < 0) x0 = 0;
    if (x1 > c.outW) x1 = c.outW;
    if (y0 < w.row0) y0 = w.row0;
    if (y1 > w.row1) y1 = w.row1;
    w.clipX0 = x0; w.clipX1 = x1; w.clipY0 = y0; w.clipY1 = y1;
}

// ── blending ─────────────────────────────────────────────────────────────────
FT_INLINE uint32_t blendFactor(uint8_t f, uint32_t sa, uint32_t da) {
    switch (f) {
        case 0: return 0;
        case 1: return 255;
        case 2: return sa;
        case 3: return da;
        case 4: return 255 - sa;
        default: return 255 - da;
    }
}

// src = premodulated ARGB (colour * COLOR_RGB, alpha * COLOR_A * coverage).
// SRAM (FT_WALK_HOT): it is called once per TRANSLUCENT pixel from every blit
// loop, and as flash code that was ~0.5 us of instruction misses per pixel under
// core0's PSRAM traffic — ZUMA's boot screen (every pixel through a non-default
// blend) and R-Type's tinted sprites both read 0.7-1 us/px because of it
// (hw 2026-10-05, mip5). noinline: one copy, 400 B.
static FT_WALK_HOT __attribute__((noinline)) void blendPixel(uint32_t* dp, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa, const Ctx& c) {
    // ALPHA_FUNC on the source alpha
    switch (c.alphaFunc) {
        case 0: return;
        case 1: if (!(sa <  c.alphaRef)) return; break;
        case 2: if (!(sa <= c.alphaRef)) return; break;
        case 3: if (!(sa >  c.alphaRef)) return; break;
        case 4: if (!(sa >= c.alphaRef)) return; break;
        case 5: if (!(sa == c.alphaRef)) return; break;
        case 6: if (!(sa != c.alphaRef)) return; break;
        default: break;
    }
    const uint32_t d = *dp;
    const uint32_t da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    uint32_t r, g, b, a;
    if (c.blendSrc == 2 && c.blendDst == 4) {
        // SRC_ALPHA / ONE_MINUS_SRC_ALPHA — the default, and the common case
        if (sa == 0) return;
        if (sa == 255) { r = sr; g = sg; b = sb; a = 255; }
        else {
            const uint32_t ia = 255 - sa;
            r = (sr * sa + dr * ia + 127) / 255;
            g = (sg * sa + dg * ia + 127) / 255;
            b = (sb * sa + db * ia + 127) / 255;
            a = (sa * sa + da * ia + 127) / 255;
        }
    } else {
        const uint32_t sf = blendFactor(c.blendSrc, sa, da), df = blendFactor(c.blendDst, sa, da);
        r = clamp255((int32_t)((sr * sf + dr * df + 127) / 255));
        g = clamp255((int32_t)((sg * sf + dg * df + 127) / 255));
        b = clamp255((int32_t)((sb * sf + db * df + 127) / 255));
        a = clamp255((int32_t)((sa * sf + da * df + 127) / 255));
    }
    const uint8_t m = c.colorMask;
    if (m == 0x0F) { *dp = (a << 24) | (r << 16) | (g << 8) | b; return; }
    uint32_t o = d;
    if (m & 8) o = (o & 0xFF00FFFF) | (r << 16);
    if (m & 4) o = (o & 0xFFFF00FF) | (g << 8);
    if (m & 2) o = (o & 0xFFFFFF00) | b;
    if (m & 1) o = (o & 0x00FFFFFF) | (a << 24);
    *dp = o;
}

// ── texel fetch ──────────────────────────────────────────────────────────────
enum Fmt { F_ARGB1555 = 0, F_L1 = 1, F_L4 = 2, F_L8 = 3, F_RGB332 = 4, F_ARGB2 = 5, F_ARGB4 = 6,
           F_RGB565 = 7, F_PALETTED = 8, F_TEXT8X8 = 9, F_TEXTVGA = 10, F_BARGRAPH = 11,
           F_PALETTED565 = 14, F_PALETTED4444 = 15, F_PALETTED8 = 16, F_L2 = 17 };

static inline int fmtBits(int fmt) {
    switch (fmt) {
        case F_ARGB1555: case F_ARGB4: case F_RGB565: case F_TEXTVGA: return 16;
        case F_L1: return 1;
        case F_L2: return 2;
        case F_L4: return 4;
        default: return 8;
    }
}
static inline bool fmtIsL(int fmt) { return fmt == F_L1 || fmt == F_L2 || fmt == F_L4 || fmt == F_L8; }

// The texel as ARGB8888. L formats return the luminance in the alpha byte with
// rgb = 0xFFFFFF (the colour comes from COLOR_RGB, the alpha from the texel —
// that is how the ROM fonts are drawn). `pal` = PALETTE_SOURCE bytes (may be null).
// The host test renders every scene of its differential check twice, with the
// fast cell loop of blitBitmap on and off; on the device it is always on.
#ifdef FT812_HOST_TEST
bool g_ft812FastBlit = true;
#define FT_FAST_BLIT_ON g_ft812FastBlit
#else
#define FT_FAST_BLIT_ON true
#endif

template <int FMT>
FT_INLINE uint32_t texel(const uint8_t* row, uint32_t tu, const uint8_t* pal, uint32_t palAvail) {
    switch (FMT) {
        case F_ARGB1555: { const uint32_t p = *(const uint16_t*)(row + tu * 2);
            const uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
            return ((p & 0x8000) ? 0xFF000000u : 0u) | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2); }
        case F_L1: return ((row[tu >> 3] >> (7 - (tu & 7))) & 1) ? 0xFFFFFFFFu : 0x00FFFFFFu;
        case F_L2: { const uint32_t v = (row[tu >> 2] >> (6 - 2 * (tu & 3))) & 3; return (v * 85) << 24 | 0xFFFFFF; }
        case F_L4: { const uint32_t v = (tu & 1) ? (row[tu >> 1] & 15) : (row[tu >> 1] >> 4); return (v * 17) << 24 | 0xFFFFFF; }
        case F_L8: return ((uint32_t)row[tu] << 24) | 0xFFFFFF;
        case F_RGB332: { const uint32_t p = row[tu]; const uint32_t r = p >> 5, g = (p >> 2) & 7, b = p & 3;
            return 0xFF000000u | ((r * 255 / 7) << 16) | ((g * 255 / 7) << 8) | (b * 85); }
        case F_ARGB2: { const uint32_t p = row[tu]; return ((p >> 6) * 85) << 24 | (((p >> 4) & 3) * 85) << 16 | (((p >> 2) & 3) * 85) << 8 | ((p & 3) * 85); }
        case F_ARGB4: { const uint32_t p = *(const uint16_t*)(row + tu * 2);
            return ((p >> 12) * 17) << 24 | (((p >> 8) & 15) * 17) << 16 | (((p >> 4) & 15) * 17) << 8 | ((p & 15) * 17); }
        case F_RGB565: { const uint32_t p = *(const uint16_t*)(row + tu * 2);
            const uint32_t r = p >> 11, g = (p >> 5) & 63, b = p & 31;
            return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2); }
        case F_PALETTED: case F_PALETTED8: {   // 32-bit ARGB8888 entries at PALETTE_SOURCE
            const uint32_t i = row[tu]; if (!pal || i * 4 + 4 > palAvail) return 0;
            const uint8_t* e = pal + i * 4; return (uint32_t)e[3] << 24 | (uint32_t)e[2] << 16 | (uint32_t)e[1] << 8 | e[0]; }
        case F_PALETTED565: { const uint32_t i = row[tu]; if (!pal || i * 2 + 2 > palAvail) return 0;
            const uint32_t p = pal[i * 2] | (pal[i * 2 + 1] << 8);
            const uint32_t r = p >> 11, g = (p >> 5) & 63, b = p & 31;
            return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2); }
        case F_PALETTED4444: { const uint32_t i = row[tu]; if (!pal || i * 2 + 2 > palAvail) return 0;
            const uint32_t p = pal[i * 2] | (pal[i * 2 + 1] << 8);
            return ((p >> 12) * 17) << 24 | (((p >> 8) & 15) * 17) << 16 | (((p >> 4) & 15) * 17) << 8 | ((p & 15) * 17); }
        default: return 0;
    }
}

FT_INLINE uint32_t bilerp(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, uint32_t wx, uint32_t wy) {
    // wx, wy 0..256
    const uint32_t w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const uint32_t v = (((c00 >> sh) & 255) * w00 + ((c10 >> sh) & 255) * w10 + ((c01 >> sh) & 255) * w01 + ((c11 >> sh) & 255) * w11) >> 16;
        out |= v << sh;
    }
    return out;
}

struct BlitArgs {
    Walk* w;
    const uint8_t* data; uint32_t avail;     // bitmap bytes from the cell's first line
    const uint8_t* pal;  uint32_t palAvail;
    const uint32_t* pal32;                   // the palette expanded to 256 x ARGB8888 in cfg.palScratch (else nullptr)
    uint32_t stride, lw, lh;                 // bytes per line, pixels per line, lines
    bool wrapx, wrapy, bilinear;
    bool blendDefault;                       // ALPHA_FUNC ALWAYS + SRC_ALPHA/ONE_MINUS_SRC_ALPHA + full COLOR_MASK
    bool box;                                // RenderCfg::smooth on a minified cell: four equal taps over the pixel's footprint
    int ox0, ox1, oy0, oy1;                  // output pixel range (absolute rows)
    int32_t vx, vy;                          // vertex, Q16 FT px
    int32_t Aq, Bq, Cq, Dq, Eq, Fq;          // the bitmap matrix, Q16 (the context's 8.8 / 15.8 shifted; scaled onto the cache image for a mip cell)
    const MipTile* tiles;                    // a pre-scaled cell: rows come from these tiles (MIP_TILE_ROWS each), `data`/`avail` unused
};

static_assert(sizeof(BlitArgs) <= sizeof(((Walk*)nullptr)->argsBuf), "Walk::argsBuf must hold a BlitArgs");

// The source row `tv` of a cell: tile-indirected for a cached cell, bounds-checked
// against the bytes the view gave us otherwise (nullptr = nothing there).
FT_INLINE const uint8_t* cellRow(const BlitArgs& a, uint32_t tv) {
    if (a.tiles) return a.tiles[tv >> MIP_TILE_SHIFT].data + (tv & (MIP_TILE_ROWS - 1)) * a.stride;
    const uint32_t off = tv * a.stride;
    return off + a.stride > a.avail ? nullptr : a.data + off;
}

// A texel of ANY format as ARGB8888 — the generic blit loop and the cache build
// (one switch per texel is fine there; the fast loops inline texel<FMT>).
FT_INLINE uint32_t texelAny(int fmt, const uint8_t* row, uint32_t tu, const uint8_t* pal, uint32_t palAvail, const uint32_t* pal32) {
    switch (fmt) {
        case F_ARGB1555: return texel<F_ARGB1555>(row, tu, pal, palAvail);
        case F_L1:       return texel<F_L1>(row, tu, pal, palAvail);
        case F_L2:       return texel<F_L2>(row, tu, pal, palAvail);
        case F_L4:       return texel<F_L4>(row, tu, pal, palAvail);
        case F_L8:       return texel<F_L8>(row, tu, pal, palAvail);
        case F_RGB332:   return texel<F_RGB332>(row, tu, pal, palAvail);
        case F_ARGB2:    return texel<F_ARGB2>(row, tu, pal, palAvail);
        case F_ARGB4:    return texel<F_ARGB4>(row, tu, pal, palAvail);
        case F_RGB565:   return texel<F_RGB565>(row, tu, pal, palAvail);
        case F_PALETTED: case F_PALETTED8: case F_PALETTED565: case F_PALETTED4444:
            if (pal32) return pal32[row[tu]];
            return fmt == F_PALETTED565 ? texel<F_PALETTED565>(row, tu, pal, palAvail)
                 : fmt == F_PALETTED4444 ? texel<F_PALETTED4444>(row, tu, pal, palAvail) : texel<F_PALETTED>(row, tu, pal, palAvail);
        default: return 0;
    }
}

// One tap of the generic loop: wrap / bounds, the row, the texel. SRAM and OUT
// OF LINE on purpose — inlined, the format switch would sit nine times in the
// loop (three sampling modes x up to four taps); a call inside the window is
// ~10 cycles where one flash instruction miss is ~200.
static FT_WALK_HOT __attribute__((noinline)) bool blitTap(const BlitArgs& a, int fmt, int32_t su, int32_t sv, uint32_t* out) {
    if (a.wrapx) su = (int32_t)((uint32_t)su % a.lw); else if ((uint32_t)su >= a.lw) return false;
    if (a.wrapy) sv = (int32_t)((uint32_t)sv % a.lh); else if ((uint32_t)sv >= a.lh) return false;
    const uint8_t* const row = cellRow(a, (uint32_t)sv);
    if (!row) return false;
    *out = texelAny(fmt, row, (uint32_t)su, a.pal, a.palAvail, a.pal32);
    return true;
}

// The GENERIC blit: any matrix, bilinear / box / nearest, wrap, any blend, any
// COLOR. ONE instance for every format, in SRAM — it used to be part of each of
// the 13 blitBitmap<FMT> instantiations, 11 of them in FLASH, and ZUMA's boot
// screen (an L4 alpha mask + two RGB565 planes composited through COLOR_MASK /
// BLEND_FUNC — nothing a fast loop takes) ran all 225k px of a frame through it
// at 0.72 us each: 180 ms a frame (hw 2026-10-05, mip4 and mip5 alike).
static FT_WALK_HOT __attribute__((noinline)) void blitGeneric(const BlitArgs& a, int fmt) {
    Walk& w = *a.w;
    const RenderCfg& cfg = *w.cfg;
    const Ctx& c = w.ctx;
    const uint32_t cr = (c.color >> 16) & 255, cg = (c.color >> 8) & 255, cb = c.color & 255, ca = c.color >> 24;
    const bool lfmt = fmtIsL(fmt);
    const bool white = (c.color & 0xFFFFFF) == 0xFFFFFF;
    const bool nearest = !a.box && !a.bilinear;
    const int32_t Aq = a.Aq, Bq = a.Bq, Cq = a.Cq, Dq = a.Dq, Eq = a.Eq, Fq = a.Fq;
    const int32_t du = mulQ16(Aq, cfg.invXQ16), dv = mulQ16(Dq, cfg.invXQ16);
    const int32_t fxRel0 = (int32_t)((((int64_t)a.ox0 << 16) + 0x8000) * cfg.invXQ16 >> 16) - a.vx;
    // box: the output pixel's footprint in the bitmap is the parallelogram spanned
    // by (du, dv) per x and (duy, dvy) per y; the four taps sit at the centres of
    // its quarters. At exactly 2:1 that is the 2x2 block the pixel covers.
    const int32_t qux = du / 4, qvx = dv / 4;
    const int32_t quy = mulQ16(Bq, cfg.invYQ16) / 4, qvy = mulQ16(Eq, cfg.invYQ16) / 4;
    // The blend, classified ONCE per cell. With ALPHA_FUNC ALWAYS every pair of
    // factors and every COLOR_MASK is done inline below — blendPixel's arithmetic
    // exactly, minus the call, the alpha-function switch and the default-pair
    // test per pixel. ZUMA's boot screen is nothing but such cells (an L4 mask
    // written to the alpha channel under ONE/ZERO, two RGB565 planes composited
    // through DST_ALPHA/ZERO and ONE_MINUS_DST_ALPHA/ONE), 225k px a frame, and it
    // read 0.81 us per pixel with the call (hw 2026-10-06, mip6). Gated on
    // FT_FAST_BLIT_ON so the host test's differential covers it.
    const bool inlineBlend = FT_FAST_BLIT_ON && !a.blendDefault && c.alphaFunc == 7;
    const uint8_t bs = c.blendSrc, bd = c.blendDst, mask = c.colorMask;
    // Everything the nearest tap reads, as SCALAR locals whose address is never
    // taken: `a.lw`, `a.stride`, `a.avail` are uint32_t like the band pixels, so
    // through the reference every `*dp` store could alias them and GCC reloaded
    // the lot per pixel (hw 2026-10-06, mip7: 0.62 us/px on the boot screen with
    // the blend already inline).
    const uint32_t lw = a.lw, lh = a.lh, stride = a.stride, avail = a.avail;
    const bool wrapx = a.wrapx, wrapy = a.wrapy;
    const uint8_t* const data = a.data; const MipTile* const tiles = a.tiles;
    const uint8_t* const pal = a.pal; const uint32_t palAvail = a.palAvail; const uint32_t* const pal32 = a.pal32;
#if FT812_TRACE
    {   // why this cell is here (all cells; the cached ones also in mipGenPx)
        const uint32_t npx = (uint32_t)(a.ox1 - a.ox0) * (uint32_t)(a.oy1 - a.oy0);
        g_renderStats.genPx += npx;
        if (a.tiles) g_renderStats.mipGenPx += npx;
        if (a.bilinear) g_renderStats.mipGenReason[0] += npx;
        if (a.box) g_renderStats.mipGenReason[1] += npx;
        if (a.wrapx || a.wrapy) g_renderStats.mipGenReason[2] += npx;
        if (!a.blendDefault) g_renderStats.mipGenReason[3] += npx;
        if (c.color != 0xFFFFFFFFu) g_renderStats.mipGenReason[4] += npx;
        if (Bq != 0 || Dq != 0) g_renderStats.mipGenReason[5] += npx;
        if (du <= 0 && Bq == 0 && Dq == 0) g_renderStats.mipGenReason[6] += npx;
        if ((fmt == F_PALETTED || fmt == F_PALETTED8 || fmt == F_PALETTED565 || fmt == F_PALETTED4444) && !a.pal32) g_renderStats.mipGenReason[7] += npx;
    }
#endif
    for (int oy = a.oy0; oy < a.oy1; oy++) {
        const int32_t fyRel = (int32_t)((((int64_t)oy << 16) + 0x8000) * cfg.invYQ16 >> 16) - a.vy;
        int32_t u = (int32_t)(((int64_t)Aq * fxRel0 + (int64_t)Bq * fyRel) >> 16) + Cq;
        int32_t v = (int32_t)(((int64_t)Dq * fxRel0 + (int64_t)Eq * fyRel) >> 16) + Fq;
        uint32_t* dp = w.band + (size_t)(oy - w.row0) * cfg.outW + a.ox0;
        for (int ox = a.ox0; ox < a.ox1; ox++, dp++, u += du, v += dv) {
            const int32_t tu = u >> 16, tv = v >> 16;
            uint32_t px;
            if (nearest) {
                // the one tap of the common case, inline (one copy of the format
                // switch; the four-tap modes go through blitTap)
                int32_t su = tu, sv = tv;
                if (wrapx) su = (int32_t)((uint32_t)su % lw); else if ((uint32_t)su >= lw) continue;
                if (wrapy) sv = (int32_t)((uint32_t)sv % lh); else if ((uint32_t)sv >= lh) continue;
                const uint8_t* row;
                if (tiles) row = tiles[(uint32_t)sv >> MIP_TILE_SHIFT].data + ((uint32_t)sv & (MIP_TILE_ROWS - 1)) * stride;
                else { const uint32_t off = (uint32_t)sv * stride; if (off + stride > avail) continue; row = data + off; }
                px = texelAny(fmt, row, (uint32_t)su, pal, palAvail, pal32);
            } else if (a.box) {
                // Four scalars, not `uint32_t cc[4] = {0}`: GCC turned that
                // zero-fill into a libc memset — a FLASH call per pixel.
                uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, t;
                if (blitTap(a, fmt, (u - qux - quy) >> 16, (v - qvx - qvy) >> 16, &t)) c0 = t;
                if (blitTap(a, fmt, (u + qux - quy) >> 16, (v + qvx - qvy) >> 16, &t)) c1 = t;
                if (blitTap(a, fmt, (u - qux + quy) >> 16, (v - qvx + qvy) >> 16, &t)) c2 = t;
                if (blitTap(a, fmt, (u + qux + quy) >> 16, (v + qvx + qvy) >> 16, &t)) c3 = t;
                px = bilerp(c0, c1, c2, c3, 128u, 128u);
            } else {
                const uint32_t wx = (u >> 8) & 255, wy = (v >> 8) & 255;
                uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, t;
                if (blitTap(a, fmt, tu,     tv,     &t)) c0 = t;
                if (blitTap(a, fmt, tu + 1, tv,     &t)) c1 = t;
                if (blitTap(a, fmt, tu,     tv + 1, &t)) c2 = t;
                if (blitTap(a, fmt, tu + 1, tv + 1, &t)) c3 = t;
                px = bilerp(c0, c1, c2, c3, wx, wy);
            }
            uint32_t sr, sg, sb;
            const uint32_t sa = mul255(px >> 24, ca);
            if (lfmt) { sr = cr; sg = cg; sb = cb; }
            else if (white) { sr = (px >> 16) & 255; sg = (px >> 8) & 255; sb = px & 255; }
            else { sr = mul255((px >> 16) & 255, cr); sg = mul255((px >> 8) & 255, cg); sb = mul255(px & 255, cb); }
            // The common case short-circuits the blend: with the default ALPHA_FUNC /
            // BLEND_FUNC / COLOR_MASK an opaque texel is a plain store and a
            // transparent one is nothing — most of a scene is one or the other.
            if (a.blendDefault) {
                if (sa == 255) { *dp = 0xFF000000u | (sr << 16) | (sg << 8) | sb; continue; }
                if (sa == 0) continue;
            } else if (inlineBlend) {
                const uint32_t d = *dp;
                const uint32_t da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
                const uint32_t sf = blendFactor(bs, sa, da), df = blendFactor(bd, sa, da);
                const uint32_t r = clamp255((int32_t)((sr * sf + dr * df + 127) / 255));
                const uint32_t g = clamp255((int32_t)((sg * sf + dg * df + 127) / 255));
                const uint32_t b = clamp255((int32_t)((sb * sf + db * df + 127) / 255));
                const uint32_t na = clamp255((int32_t)((sa * sf + da * df + 127) / 255));
                if (mask == 0x0F) { *dp = (na << 24) | (r << 16) | (g << 8) | b; continue; }
                uint32_t o = d;
                if (mask & 8) o = (o & 0xFF00FFFF) | (r << 16);
                if (mask & 4) o = (o & 0xFFFF00FF) | (g << 8);
                if (mask & 2) o = (o & 0xFFFFFF00) | b;
                if (mask & 1) o = (o & 0x00FFFFFF) | (na << 24);
                *dp = o;
                continue;
            }
            blendPixel(dp, sr, sg, sb, sa, c);
        }
    }
}

// The store at the end of a fast-loop pixel. `mod` = COLOR_RGB is not white or
// COLOR_A is not 255 (loop-invariant; a predicted branch per pixel — the loops
// are PSRAM-bound at ~55 cycles a pixel anyway). The arithmetic is the generic
// loop's exactly, so the host test's fast-on/fast-off differential stays
// bit-exact. R-Type tints its sprites (COLOR_RGB) and both games fade with
// COLOR_A, which used to send every such pixel to the generic loop
// (hw 2026-10-05, mip5: 270k px a window on R-Type, `col` in the mip: line).
FT_INLINE void fastStore(uint32_t* dp, uint32_t px, bool mod, uint32_t ca, uint32_t cr, uint32_t cg, uint32_t cb, const Ctx& c) {
    uint32_t sa = px >> 24;
    if (!mod) {
        if (sa == 255) *dp = px;
        else if (sa) blendPixel(dp, (px >> 16) & 255, (px >> 8) & 255, px & 255, sa, c);
        return;
    }
    sa = mul255(sa, ca);
    const uint32_t r = mul255((px >> 16) & 255, cr), g = mul255((px >> 8) & 255, cg), b = mul255(px & 255, cb);
    if (sa == 255) *dp = 0xFF000000u | (r << 16) | (g << 8) | b;
    else if (sa) blendPixel(dp, r, g, b, sa, c);
}

template <int FMT>
FT_INLINE void blitBitmapImpl(const BlitArgs& a) {
    Walk& w = *a.w;
    const RenderCfg& cfg = *w.cfg;
    const Ctx& c = w.ctx;
    const uint32_t cr = (c.color >> 16) & 255, cg = (c.color >> 8) & 255, cb = c.color & 255, ca = c.color >> 24;
    const bool mod = c.color != 0xFFFFFFFFu;
    // matrix in Q16 (drawBitmap's: the context's, or scaled onto a cached cell)
    const int32_t Aq = a.Aq, Bq = a.Bq, Cq = a.Cq, Dq = a.Dq, Eq = a.Eq, Fq = a.Fq;
    const int32_t du = mulQ16(Aq, cfg.invXQ16), dv = mulQ16(Dq, cfg.invXQ16);
    const int32_t fxRel0 = (int32_t)((((int64_t)a.ox0 << 16) + 0x8000) * cfg.invXQ16 >> 16) - a.vx;
    const uint32_t lw = a.lw, lh = a.lh;
#if FT812_TRACE
    const uint32_t npx = (uint32_t)(a.ox1 - a.ox0) * (uint32_t)(a.oy1 - a.oy0);
    if (a.bilinear) g_renderStats.pxBilinear += npx; else g_renderStats.pxNearest += npx;
    g_renderStats.pxFmt[FMT] += npx;
    if (a.tiles) g_renderStats.pxMip += npx;
    // (a cached cell's blit time goes to usFmt[F_ARGB4] AND mipBlitUs)
    struct MipBlitTimer { const RenderCfg* c; uint32_t t0; bool on;
        inline __attribute__((always_inline)) ~MipBlitTimer() { if (on) g_renderStats.mipBlitUs += ftTick(*c) - t0; } } mbt_ = { &cfg, 0, a.tiles != nullptr };
    if (mbt_.on) mbt_.t0 = ftTick(cfg);
    struct FmtTimer { const RenderCfg* c; uint32_t t0;
        inline __attribute__((always_inline)) ~FmtTimer() { g_renderStats.usFmt[FMT] += ftTick(*c) - t0; } } ft_ = { &cfg, ftTick(cfg) };
#endif
    // The common cell — nearest, axis-aligned, default blend, a direct-texel
    // format — has nothing per pixel but the fetch: the row pointer and the valid
    // x range are settled once per row, so the loop is a Q16 step, one texel, and
    // a store (or a blend for a translucent texel). The generic loop spends ~70
    // instructions a pixel on tests this cell can never fail (hw 2026-10-01,
    // R-Type: 162k px a frame at 0.5 us each, 80 ms of blit per frame).
    const bool fmtFast = a.pal32 || FMT == F_ARGB4 || FMT == F_RGB565 || FMT == F_ARGB1555 || FMT == F_RGB332 || FMT == F_ARGB2;
    // A ROTATED cached cell (ZUMA's balls: 16 rotation matrices over a 32x32 ARGB4
    // atlas) with the default blend and no wrap: here a pixel is the matrix step,
    // a bounds test, one 16-bit load and a store or blend.
    if (FT_FAST_BLIT_ON && a.tiles && !a.box && !a.bilinear && !a.wrapx && !a.wrapy && a.blendDefault && (Bq != 0 || Dq != 0)) {
        for (int oy = a.oy0; oy < a.oy1; oy++) {
            const int32_t fyRel = (int32_t)((((int64_t)oy << 16) + 0x8000) * cfg.invYQ16 >> 16) - a.vy;
            int32_t u = (int32_t)(((int64_t)Aq * fxRel0 + (int64_t)Bq * fyRel) >> 16) + Cq;
            int32_t v = (int32_t)(((int64_t)Dq * fxRel0 + (int64_t)Eq * fyRel) >> 16) + Fq;
            uint32_t* dp = w.band + (size_t)(oy - w.row0) * cfg.outW + a.ox0;
            for (int n = a.ox1 - a.ox0; n > 0; n--, dp++, u += du, v += dv) {
                const uint32_t tu = (uint32_t)(u >> 16), tv = (uint32_t)(v >> 16);
                if (tu >= lw || tv >= lh) continue;
                const uint8_t* const row = a.tiles[tv >> MIP_TILE_SHIFT].data + (tv & (MIP_TILE_ROWS - 1)) * a.stride;
                fastStore(dp, texel<F_ARGB4>(row, tu, nullptr, 0), mod, ca, cr, cg, cb, c);
            }
        }
        return;
    }
    // (du < 0 = a MIRRORED cell, A < 0: R-Type flips every sprite that faces left
    // this way, ~10% of its blit area, and it fell into the generic loop at 0.8
    // us/px until 2026-10-06 — the one cause the trace could not name.)
    if (FT_FAST_BLIT_ON && fmtFast && !a.bilinear && !a.box && Bq == 0 && Dq == 0 && du != 0 && a.blendDefault) {
        const int32_t lwq = (int32_t)(lw << 16);
        const bool pow2 = (lw & (lw - 1)) == 0;
        for (int oy = a.oy0; oy < a.oy1; oy++) {
            const int32_t fyRel = (int32_t)((((int64_t)oy << 16) + 0x8000) * cfg.invYQ16 >> 16) - a.vy;
            int32_t tv = ((int32_t)(((int64_t)Eq * fyRel) >> 16) + Fq) >> 16;
            if (a.wrapy) tv = (int32_t)((uint32_t)tv % lh); else if ((uint32_t)tv >= lh) continue;
            const uint8_t* const row = cellRow(a, (uint32_t)tv);
            if (!row) continue;
            int32_t u = (int32_t)(((int64_t)Aq * fxRel0) >> 16) + Cq;
            int xs = a.ox0, xe = a.ox1;
            if (!a.wrapx) {                                    // the pixels whose texel is inside the line
                if (du > 0) {
                    if (u < 0) { const int n = (int)((-u + du - 1) / du); xs += n; u += n * du; }
                    if (xs >= xe || u >= lwq) continue;
                    const int maxn = (int)((lwq - 1 - u) / du) + 1;
                    if (xe - xs > maxn) xe = xs + maxn;
                } else {                                       // mirrored: u DEcreases along the row
                    const int32_t nd = -du;
                    if (u >= lwq) { const int n = (int)((u - lwq) / nd) + 1; xs += n; u -= n * nd; }
                    if (xs >= xe || u < 0) continue;
                    const int maxn = (int)(u / nd) + 1;
                    if (xe - xs > maxn) xe = xs + maxn;
                }
            }
            uint32_t* dp = w.band + (size_t)(oy - w.row0) * cfg.outW + xs;
            const uint32_t* const pal32 = a.pal32;
#define FT_FAST_PIXEL(TU) fastStore(dp, pal32 ? pal32[row[TU]] : texel<FMT>(row, (TU), nullptr, 0), mod, ca, cr, cg, cb, c)
            if (!a.wrapx)   for (int n = xe - xs; n > 0; n--, dp++, u += du) FT_FAST_PIXEL((uint32_t)(u >> 16));
            else if (pow2)  for (int n = xe - xs; n > 0; n--, dp++, u += du) FT_FAST_PIXEL((uint32_t)(u >> 16) & (lw - 1));
            else            for (int n = xe - xs; n > 0; n--, dp++, u += du) FT_FAST_PIXEL((uint32_t)(u >> 16) % lw);
#undef FT_FAST_PIXEL
        }
        return;
    }
    blitGeneric(a, FMT);
}

// One instantiation per format — the FAST loops only (the generic loop is the
// shared blitGeneric). The two the games run on — the cache's ARGB4444 (every
// cached cell) and PALETTED4444 (ZUMA's background, R-Type's parked sprites) —
// go into the SRAM window with the walk; the rest stay in flash.
template <int FMT> FT_HOT void blitBitmap(const BlitArgs& a) { blitBitmapImpl<FMT>(a); }
template <> FT_WALK_HOT void blitBitmap<F_ARGB4>(const BlitArgs& a) { blitBitmapImpl<F_ARGB4>(a); }
template <> FT_WALK_HOT void blitBitmap<F_PALETTED4444>(const BlitArgs& a) { blitBitmapImpl<F_PALETTED4444>(a); }

// ── the pre-scaled bitmap cache (see Ft812Render.h) ──────────────────────────
MipCache* ft812MipInit(void* block, size_t bytes, int cap) {
    if (!block || cap < 8) return nullptr;
    const size_t head = (sizeof(MipCache) + 7) & ~(size_t)7, entB = ((size_t)cap * sizeof(MipEntry) + 7) & ~(size_t)7;
    if (bytes < head + entB + 16384) return nullptr;          // a pool under 16 KB cannot hold one tile of a 1024-wide cell
    MipCache* mc = (MipCache*)block;
    memset(mc, 0, sizeof(*mc));
    mc->ent = (MipEntry*)((uint8_t*)block + head);
    mc->cap = (uint16_t)cap;
    mc->pool = (uint8_t*)block + head + entB;
    mc->poolBytes = (uint32_t)(bytes - head - entB);
    ft812MipReset(*mc);
    return mc;
}
void ft812MipReset(MipCache& mc) {
    mc.nent = 0; mc.poolUsed = 0;
    memset(mc.hash, 0xFF, sizeof(mc.hash));
}
uint32_t ft812MipUsed(const MipCache& mc) { return mc.poolUsed; }

namespace {
// (texelAny — the any-format texel the tile builder reads with — is defined
// above blitGeneric, which shares it. Inlined: as a flash function it was one
// call per SOURCE texel of every rebuilt tile.)
// 8 -> 4 bits as round(v / 17): the inverse of the 4444 expansion (n * 17) the
// texel fetch does, so a 4-bit source survives the round trip exactly.
FT_INLINE uint32_t q4(uint32_t v) { if (v > 255) v = 255; return (v * 15 + 127) / 255; }
FT_INLINE uint16_t pack4444(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return (uint16_t)((q4(a) << 12) | (q4(r) << 8) | (q4(g) << 4) | q4(b));
}
static inline int32_t overlapQ8(int32_t t0, int32_t t1, int32_t s0, int32_t s1) {   // length of [t0,t1) ∩ [s0,s1), Q8 units
    const int32_t lo = t0 > s0 ? t0 : s0, hi = t1 < s1 ? t1 : s1;
    return hi > lo ? hi - lo : 0;
}
static uint8_t* mipAlloc(MipCache& mc, uint32_t n) {
    n = (n + 7) & ~7u;
    if (mc.poolUsed + n > mc.poolBytes) return nullptr;
    uint8_t* p = mc.pool + mc.poolUsed; mc.poolUsed += n; return p;
}
// The source pages a tile reads, summed: its source rows (the whole cell when
// the box wraps vertically — the first and last tiles read the far edge) and
// the palette.
struct MipSrc { uint32_t src, stride, lw, lh, pal, palBytes; int fmt; bool wrapx, wrapy, smooth; };
static_assert(sizeof(MipSrc) <= sizeof(((Walk*)nullptr)->msrcBuf), "Walk::msrcBuf must hold a MipSrc");
static uint32_t tileGen(const uint16_t* pg, const MipEntry& e, const MipSrc& s, int k) {
    if (!pg) return 0;
    uint32_t y0 = 0, y1 = s.lh;
    if (!s.wrapy) {
        y0 = ((uint32_t)k * MIP_TILE_ROWS * e.sv8) >> 8;
        y1 = (((uint32_t)(k + 1) * MIP_TILE_ROWS * e.sv8 + 255) >> 8) + 1;
        if (y1 > s.lh) y1 = s.lh;
    }
    uint32_t g = 0;
    const uint32_t b0 = s.src + y0 * s.stride, b1 = s.src + y1 * s.stride;
    if (b1 > b0) for (uint32_t p = b0 >> MIP_PAGE_SHIFT, pe = (b1 - 1) >> MIP_PAGE_SHIFT; p <= pe && p < MIP_PAGES; p++) g += pg[p];
    if (s.palBytes) for (uint32_t p = s.pal >> MIP_PAGE_SHIFT, pe = (s.pal + s.palBytes - 1) >> MIP_PAGE_SHIFT; p <= pe && p < MIP_PAGES; p++) g += pg[p];
    return g;
}
// Build tile k of entry e: cache rows [k*8, k*8+8) of the cell scaled by (su8, sv8).
static void mipBuildTile(MipEntry& e, const MipSrc& s, int k, const uint8_t* data, const uint8_t* pal, uint32_t palAvail, const uint32_t* pal32) {
    uint16_t* out = (uint16_t*)e.tiles[k].data;
    for (int r = 0; r < MIP_TILE_ROWS; r++) {
        const int ry = k * MIP_TILE_ROWS + r;
        uint16_t* o = out + (size_t)r * e.cw;
        if (ry >= e.ch) { memset(o, 0, (size_t)e.cw * 2); continue; }
        const int32_t sv0 = ry * (int32_t)e.sv8, sv1 = sv0 + e.sv8;
        for (int c = 0; c < e.cw; c++) {
            const int32_t su0 = c * (int32_t)e.su8, su1 = su0 + e.su8;
            if (!s.smooth) {                                   // the centre texel (Fast)
                int32_t tx = (su0 + (int32_t)e.su8 / 2) >> 8, ty = (sv0 + (int32_t)e.sv8 / 2) >> 8;
                if (s.wrapx) tx = (int32_t)((uint32_t)tx % s.lw);
                if (s.wrapy) ty = (int32_t)((uint32_t)ty % s.lh);
                uint32_t px = 0;
                if ((uint32_t)tx < s.lw && (uint32_t)ty < s.lh) px = texelAny(s.fmt, data + (uint32_t)ty * s.stride, (uint32_t)tx, pal, palAvail, pal32);
                o[c] = pack4444(px >> 24, (px >> 16) & 255, (px >> 8) & 255, px & 255);
                continue;
            }
            // the box: every texel the pixel's footprint overlaps, weighted by the
            // overlap area (Q8 x Q8 -> Q8); outside the cell (BORDER) is transparent
            // and still counts in the area, so an edge pixel fades out
            uint32_t A = 0, R = 0, G = 0, B = 0, W = 0;
            for (int32_t y = sv0 >> 8; y < ((sv1 + 255) >> 8); y++) {
                const int32_t wy = overlapQ8(y << 8, (y + 1) << 8, sv0, sv1);
                if (!wy) continue;
                int32_t ty = y;
                if (s.wrapy) ty = (int32_t)((uint32_t)ty % s.lh);
                const bool rowOk = (uint32_t)ty < s.lh;
                const uint8_t* row = rowOk ? data + (uint32_t)ty * s.stride : nullptr;
                for (int32_t x = su0 >> 8; x < ((su1 + 255) >> 8); x++) {
                    const int32_t wx = overlapQ8(x << 8, (x + 1) << 8, su0, su1);
                    if (!wx) continue;
                    const uint32_t wgt = (uint32_t)(wx * wy) >> 8;          // <= 256
                    W += wgt;
                    int32_t tx = x;
                    if (s.wrapx) tx = (int32_t)((uint32_t)tx % s.lw);
                    if (!row || (uint32_t)tx >= s.lw) continue;
                    const uint32_t px = texelAny(s.fmt, row, (uint32_t)tx, pal, palAvail, pal32);
                    const uint32_t wa = wgt * (px >> 24);                   // <= 65280
                    A += wa; R += (wa * ((px >> 16) & 255)) >> 8; G += (wa * ((px >> 8) & 255)) >> 8; B += (wa * (px & 255)) >> 8;
                }
            }
            if (!W || !A) { o[c] = 0; continue; }
            const uint32_t a8 = A / W;                            // alpha = area-weighted mean
            const uint32_t A8 = A >> 8 ? A >> 8 : 1;             // colour = alpha-weighted mean (R was scaled by 1/256 above)
            o[c] = pack4444(a8, R / A8, G / A8, B / A8);
        }
    }
}
// Look up / build the cached image of a cell. nullptr = not cached (declined, or
// the pool could not hold it): the caller blits the source.
static MipEntry* mipGet(const RenderCfg& cfg, const MipSrc& s, uint16_t su8, uint16_t sv8, int tile0, int tile1,
                        const uint8_t* data, uint32_t avail, const uint8_t* pal, uint32_t palAvail, const uint32_t* pal32) {
    MipCache& mc = *cfg.mip;
    if (mc.off) return nullptr;
    const uint32_t cw = (s.lw * 256u + su8 - 1) / su8, ch = (s.lh * 256u + sv8 - 1) / sv8;
    if (cw < 1 || ch < 1 || cw > 1024 || ch > 1024) return nullptr;
    if ((s.wrapx && (s.lw * 256u) % su8) || (s.wrapy && (s.lh * 256u) % sv8)) return nullptr;   // REPEAT needs an exact period
    const uint32_t ntiles = (ch + MIP_TILE_ROWS - 1) / MIP_TILE_ROWS, tileBytes = cw * 2u * MIP_TILE_ROWS;
    if (ntiles * tileBytes + ntiles * sizeof(MipTile) > mc.poolBytes / 2) return nullptr;
    if ((uint64_t)s.lh * s.stride > avail) return nullptr;   // the cell must be whole in the view
    const uint32_t k1 = (s.stride & 0xFFF) | ((s.lw & 0xFFF) << 12) | ((uint32_t)(s.fmt & 31) << 24) | (s.wrapx ? 1u << 29 : 0) | (s.wrapy ? 1u << 30 : 0) | (s.smooth ? 1u << 31 : 0);
    const uint32_t palKey = s.palBytes ? s.pal : 0;
    const uint32_t h = ((s.src * 2654435761u) ^ (k1 * 2246822519u) ^ (palKey * 3266489917u) ^ ((uint32_t)su8 << 16 | sv8)) >> 24;
    MipEntry* e = nullptr;
    for (uint16_t i = mc.hash[h]; i != 0xFFFF; i = mc.ent[i].next) {
        MipEntry& c = mc.ent[i];
        if (c.src == s.src && c.k1 == k1 && c.pal == palKey && c.su8 == su8 && c.sv8 == sv8 && c.lh == s.lh) { e = &c; break; }
    }
    // A cell whose source is rewritten every frame (R-Type streams sprite frames
    // into fixed RAM_G slots) would be rebuilt every frame — more work than the
    // direct blit it replaces. Three consecutive frames with a rebuild park the
    // entry for MIP_SKIP_FRAMES; it is tried again after that.
    if (e && e->skip) {
        if ((uint16_t)(mc.frame - e->skipFrom) < MIP_SKIP_FRAMES) return nullptr;
        e->skip = 0; e->streak = 0;
    }
    const bool fresh = !e;
    for (int attempt = 0; attempt < 2 && !e; attempt++) {
        if (mc.nent < mc.cap) {
            MipTile* tiles = (MipTile*)mipAlloc(mc, ntiles * sizeof(MipTile));
            if (tiles) {
                e = &mc.ent[mc.nent];
                e->src = s.src; e->k1 = k1; e->pal = palKey; e->su8 = su8; e->sv8 = sv8;
                e->cw = (uint16_t)cw; e->ch = (uint16_t)ch; e->lh = (uint16_t)s.lh; e->ntiles = (uint16_t)ntiles;
                e->lastBuild = (uint16_t)(mc.frame - 2); e->skipFrom = 0; e->streak = 0; e->skip = 0;
                e->tiles = tiles;
                for (uint32_t k = 0; k < ntiles; k++) { tiles[k].data = nullptr; tiles[k].gen = 0; }
                e->next = mc.hash[h]; mc.hash[h] = mc.nent++;
                break;
            }
        }
        // full: drop everything and start over — once per frame; a second time
        // means the working set does not fit, and the cache steps aside
        if (mc.resetsThisFrame++) { mc.off = 1; return nullptr; }
        FTR(g_renderStats.mipResets++);
        ft812MipReset(mc);
    }
    if (!e) return nullptr;
    // the tiles this blit reads: valid when their source pages are unchanged
    if (tile0 < 0) tile0 = 0;
    if (tile1 >= (int)ntiles) tile1 = (int)ntiles - 1;
    bool rebuilt = false;
    for (int k = tile0; k <= tile1; k++) {
        MipTile& t = e->tiles[k];
        const uint32_t g = tileGen(cfg.mem->pageGen, *e, s, k);
        if (t.data && t.gen == g) continue;
        if (t.data) rebuilt = true;                              // a tile that existed: its source moved
        if (!t.data) {
            t.data = mipAlloc(mc, tileBytes);
            if (!t.data) {                                      // the pool ran out under a live entry: same rule as above
                if (mc.resetsThisFrame++) mc.off = 1;
                FTR(g_renderStats.mipResets++);
                ft812MipReset(mc);
                return nullptr;
            }
        }
#if FT812_TRACE
        const uint32_t t0 = ftTick(cfg);
#endif
        mipBuildTile(*e, s, k, data, pal, palAvail, pal32);
        t.gen = g;
        FTR(g_renderStats.mipBuilds++);
#if FT812_TRACE
        g_renderStats.mipBuildUs += ftTick(cfg) - t0;
#endif
    }
    if (rebuilt && !fresh && e->lastBuild != (uint16_t)mc.frame) {
        e->streak = (e->lastBuild == (uint16_t)(mc.frame - 1)) ? (uint8_t)(e->streak + 1) : 1;
        e->lastBuild = (uint16_t)mc.frame;
        if (e->streak >= 3) { e->skip = 1; e->skipFrom = (uint16_t)mc.frame; e->streak = 0; FTR(g_renderStats.mipParked++); }
    }
    return e;
}
} // namespace

#if FT812_TRACE
// Scoped: the trace's "time inside drawBitmap" without a wrapper frame on core1's stack.
struct DrawTimer {
    const RenderCfg* c; uint32_t t0;
    explicit DrawTimer(const RenderCfg* cfg) : c(cfg), t0(ftTick(*cfg)) {}
    ~DrawTimer() { g_renderStats.drawUs += ftTick(*c) - t0; }
};
#endif
static void drawBitmapCold(Walk& w, int32_t vx, int32_t vy, int handle, int cell);
// The per-vertex front: the output rectangle of the cell clipped to the band —
// two thirds of the vertices a band meets end here (hw 2026-10-05, ZUMA: 1900
// calls a frame, ~650 draw), so this much lives in the walk's SRAM window and the
// setup of a cell that does draw is a separate flash function.
static FT_WALK_HOT void drawBitmap(Walk& w, int32_t vx, int32_t vy, int handle, int cell) {
#if FT812_TRACE
    DrawTimer dt_(w.cfg);
#endif
    FTR(g_renderStats.bitmaps++);
    const RenderCfg& cfg = *w.cfg;
    const BitmapHandle& h = w.hnd[handle & 31];
    const uint32_t sw = h.sw ? h.sw : 512, sh = h.sh ? h.sh : 512;
    BlitArgs& a = *(BlitArgs*)w.argsBuf;    // in the scratch, not on core1's stack (see Walk)
    a.oy0 = outMin(vy, cfg.syQ16); a.oy1 = outEnd(vy + (int32_t)(sh << 16), cfg.syQ16);
    if (a.oy0 < w.clipY0) a.oy0 = w.clipY0;
    if (a.oy1 > w.clipY1) a.oy1 = w.clipY1;
    if (a.oy0 >= a.oy1) return;
    a.ox0 = outMin(vx, cfg.sxQ16); a.ox1 = outEnd(vx + (int32_t)(sw << 16), cfg.sxQ16);
    if (a.ox0 < w.clipX0) a.ox0 = w.clipX0;
    if (a.ox1 > w.clipX1) a.ox1 = w.clipX1;
    if (a.ox0 >= a.ox1) return;
    drawBitmapCold(w, vx, vy, handle, cell);
}
// The PALETTE_SOURCE table of a paletted cell, expanded to 256 ARGB8888 words in
// one of the SRAM slots of cfg.palScratch (1 KB each, least recently used
// replaced). nullptr = no scratch. Flash: it runs only when a palette the slots
// do not hold comes up.
// The HIT half runs inline in drawBitmapCold (SRAM): one paletted cell = one
// slot scan, no call. `gen` = Walk::genNow (RAM_G generation sampled once per band).
FT_INLINE int palSlots(const RenderCfg& cfg) {
    if (!cfg.palScratch || !cfg.mem->gen || cfg.palScratchSize < 1024 || ((uintptr_t)cfg.palScratch & 3)) return 0;
    return cfg.palScratchSize / 1024 < 4 ? (int)(cfg.palScratchSize / 1024) : 4;
}
FT_INLINE const uint32_t* palLookup(const RenderCfg& cfg, RenderState& st, int fmt, uint32_t palAddr, uint32_t gen, int nslots) {
    st.palAge++;
    for (int i = 0; i < nslots; i++) {
        RenderState::PalSlot& s = st.pal[i];
        if (s.valid && s.addr == palAddr && s.gen == gen && s.fmt == (uint8_t)fmt) { s.age = st.palAge; return (const uint32_t*)(cfg.palScratch + i * 1024); }
    }
    return nullptr;
}
static __attribute__((noinline)) const uint32_t* palExpand(const RenderCfg& cfg, RenderState& st, int fmt, uint32_t palAddr, const uint8_t* src8, uint32_t pa, uint32_t gen, int nslots) {
    int victim = 0;
    for (int i = 0; i < nslots; i++) {
        RenderState::PalSlot& s = st.pal[i];
        if (!s.valid) { victim = i; break; }
        if ((uint16_t)(st.palAge - s.age) > (uint16_t)(st.palAge - st.pal[victim].age)) victim = i;
    }
    uint32_t* p32 = (uint32_t*)(cfg.palScratch + victim * 1024);
    if (fmt == F_PALETTED || fmt == F_PALETTED8) {
        const uint32_t n = pa / 4 < 256 ? pa / 4 : 256;
        memcpy(p32, src8, n * 4);
        for (uint32_t k = n; k < 256; k++) p32[k] = 0;
    } else {
        const uint32_t n = pa / 2 < 256 ? pa / 2 : 256;
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t p = (uint32_t)src8[k * 2] | ((uint32_t)src8[k * 2 + 1] << 8);
            if (fmt == F_PALETTED565) {
                const uint32_t r = p >> 11, g = (p >> 5) & 63, b = p & 31;
                p32[k] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            } else p32[k] = ((p >> 12) * 17) << 24 | (((p >> 8) & 15) * 17) << 16 | (((p >> 4) & 15) * 17) << 8 | ((p & 15) * 17);
        }
        for (uint32_t k = n; k < 256; k++) p32[k] = 0;
    }
    RenderState::PalSlot& s = st.pal[victim];
    s.valid = 1; s.addr = palAddr; s.gen = gen; s.fmt = (uint8_t)fmt; s.age = st.palAge;
    FTR(g_renderStats.palCopies++);
    return p32;
}

// SRAM too (FT_WALK_HOT): ~30 us per in-band cell as flash code — instruction
// misses, the same as the walk (hw 2026-10-05, mip4: 16-20 ms a frame on both games).
static FT_WALK_HOT __attribute__((noinline)) void drawBitmapCold(Walk& w, int32_t vx, int32_t vy, int handle, int cell) {
    const RenderCfg& cfg = *w.cfg;
    const BitmapHandle& h = w.hnd[handle & 31];
    const int fmt = h.fmt;
    FTR(if (fmt < 18) g_renderStats.bmpFmt[fmt]++);     // (cells that reach the band, since 2026-10-05)
    if (fmt == F_TEXT8X8 || fmt == F_TEXTVGA || fmt == F_BARGRAPH || fmt > F_L2 || (fmt > F_BARGRAPH && fmt < F_PALETTED565)) {
        w.st->warned |= W_TEXTFMT; return;
    }
    const uint32_t lh = h.lh ? h.lh : 512;
    const uint32_t stride = h.stride;
    if (!stride) return;
    const uint32_t lw = stride * 8u / (uint32_t)fmtBits(fmt);
    if (!lw) return;
    BlitArgs& a = *(BlitArgs*)w.argsBuf;    // ox0..oy1 set by the front
    a.w = &w;
    const uint32_t src = (h.source + (uint32_t)cell * stride * lh) & 0x3FFFFF;
    uint32_t avail = 0;
    // RAM_G resolves inline (MemView::ramg); view() — a flash function — only for
    // the ROM fonts / the video frame buffer.
    const uint8_t* const ramg = cfg.mem->ramg;
    if (ramg && src < cfg.mem->ramgBytes) { a.data = ramg + src; avail = cfg.mem->ramgBytes - src; }
    else a.data = cfg.mem->view(src, &avail);
    if (!a.data || !avail) return;
    a.avail = avail;
    a.pal = nullptr; a.palAvail = 0; a.pal32 = nullptr;
    if (fmt == F_PALETTED || fmt == F_PALETTED565 || fmt == F_PALETTED4444 || fmt == F_PALETTED8) {
        uint32_t pa = 0;
        const uint32_t palAddr = w.ctx.palSrc & 0x3FFFFF;
        if (ramg && palAddr < cfg.mem->ramgBytes) { a.pal = ramg + palAddr; pa = cfg.mem->ramgBytes - palAddr; }
        else a.pal = cfg.mem->view(palAddr, &pa);
        a.palAvail = pa;
        if (!a.pal) return;
        // The table is kept in SRAM EXPANDED to 256 ARGB8888 words: a texel is then
        // one PSRAM byte and one word load, where the raw table cost two byte loads
        // and four multiplies per pixel (and two PSRAM line fills without the copy —
        // hw 2026-09-28: ZUMA is PALETTED4444 almost throughout; R-Type too).
        const int nslots = palSlots(cfg);
        if (nslots) {
            a.pal32 = palLookup(cfg, *w.st, fmt, palAddr, w.genNow, nslots);
            if (!a.pal32) a.pal32 = palExpand(cfg, *w.st, fmt, palAddr, a.pal, pa, w.genNow, nslots);
        }
    }
    a.stride = stride; a.lw = lw; a.lh = lh;
    a.wrapx = h.wrapx != 0; a.wrapy = h.wrapy != 0; a.bilinear = h.filter != 0;
    a.blendDefault = w.ctx.alphaFunc == 7 && w.ctx.blendSrc == 2 && w.ctx.blendDst == 4 && w.ctx.colorMask == 0x0F;
    a.Aq = w.ctx.A << 8; a.Bq = w.ctx.B << 8; a.Cq = w.ctx.C << 8; a.Dq = w.ctx.D << 8; a.Eq = w.ctx.E << 8; a.Fq = w.ctx.F << 8;
    a.tiles = nullptr;
    // BILINEAR is demoted to nearest when the cell is MINIFIED by 1.5x or more on
    // both axes: at that step the four taps span a fraction of the texels each
    // output pixel covers, so the filter aliases exactly like nearest sampling
    // and only costs three more PSRAM fetches per pixel (hw 2026-09-28: 1.1 us per
    // output pixel, 260 ms per frame on ZUMA's boot screen). At our 5/16 scale
    // every bitmap drawn 1:1 on the FT screen is 3.2x minified.
    a.box = false;
    {
        const int32_t du = mulQ16((int32_t)w.ctx.A << 8, cfg.invXQ16), dv = mulQ16((int32_t)w.ctx.E << 8, cfg.invYQ16);
        const int32_t adu = du < 0 ? -du : du, adv = dv < 0 ? -dv : dv;
        if (a.bilinear && adu >= 0x18000 && adv >= 0x18000) a.bilinear = false;
        // Smooth: ANY cell minified by 1.5x or more on an axis is sampled with four
        // taps spread over the output pixel's footprint. It was "exactly 2:1 only"
        // at first, which on a real screen is a handful of cells (and none at all
        // on a 360x288 framebuffer, where the net scale is 1.78) — the owner saw
        // no difference (2026-10-01). Four taps of a 3.2x footprint is not a full
        // box, but it is what turns shimmer and broken thin lines into a blur.
        const int32_t duy = mulQ16((int32_t)w.ctx.B << 8, cfg.invYQ16), dvx = mulQ16((int32_t)w.ctx.D << 8, cfg.invXQ16);
        const int32_t aduy = duy < 0 ? -duy : duy, advx = dvx < 0 ? -dvx : dvx;
        if (cfg.smooth && (adu + aduy >= 0x18000 || adv + advx >= 0x18000)) a.box = true;
        // The pre-scaled cache (Ft812Render.h): a MINIFIED cell in RAM_G is blitted
        // from its cached image, scaled onto the output grid once. Worth it when the
        // source would cost more bytes per output pixel than the cache's two
        // (16-bit formats at any minification, 8-bit ones from 1.75 texels/px),
        // when the cell is rotated (the source reads have no row locality), or
        // whenever Smooth is on (the per-pixel 4-tap box is the alternative).
        if (cfg.mip && src < MIP_RAMG_SIZE && (uint64_t)src + (uint64_t)lh * stride <= MIP_RAMG_SIZE) {
            // the footprint of one output pixel in texels, per texel axis (a rotation
            // spreads it over both output axes — the Euclidean norm keeps it exact)
            const float fsu = sqrtf((float)adu * (float)adu + (float)aduy * (float)aduy) * (1.0f / 65536.f);
            const float fsv = sqrtf((float)adv * (float)adv + (float)advx * (float)advx) * (1.0f / 65536.f);
            const bool rotated = w.ctx.B != 0 || w.ctx.D != 0;
            // (1.5, not the box's 1.25: ZUMA's 400x300 background is drawn at 1.25
            // texels per pixel and would cost 154 KB of pool for a cell whose
            // source reads are already sequential and cheap)
            const bool minified = fsu >= 1.5f || fsv >= 1.5f;
            const int bpp = fmtBits(fmt);
            // MEASURED, not estimated (hw 2026-10-05, mip5, R-Type): an axis-aligned
            // PALETTED4444 cell at 2.0 texels/px blits DIRECT at 0.11 us/px and from
            // the cache at 0.18-0.25 — the direct read is one PSRAM line per 4 output
            // pixels either way (every other texel of an 8-byte line vs 2 B/px of
            // cached ARGB4444), and the cache adds the tile indirection. So an
            // unrotated nearest cell pays for the cache only when its source read is
            // wider than ~3.5 B per output pixel: 16-bit formats from 1.75x, 8-bit
            // from 3.5x (the 14 of the first cut put every 2:1 paletted cell in and
            // cost R-Type ~15 ms a frame).
            const bool worth = cfg.smooth || rotated || fsu * (float)bpp >= 28.f;
            if (minified && worth && fsu <= 8.f && fsv <= 8.f) {
                // The scale is part of the key, quantized to 1/8 texel: ZUMA's balls are
                // one cell under 16 rotation matrices whose 8.8 sin/cos round the
                // footprint to 1.99..2.01, and at 1/256 that was 16 cached copies of
                // every ball frame (hw 2026-10-05: the pool overflowed every ~10 s and
                // rebuilt ~2300 tiles). At 1/8 the image is at most 3% off the true
                // footprint — under half a pixel on a 16-px sprite.
                uint32_t su8 = ((uint32_t)(fsu * 256.f + 0.5f) + 16) & ~31u, sv8 = ((uint32_t)(fsv * 256.f + 0.5f) + 16) & ~31u;
                if (su8 < 256) su8 = 256;
                if (sv8 < 256) sv8 = 256;
                MipSrc& s = *(MipSrc*)w.msrcBuf;
                s.src = src; s.stride = stride; s.lw = lw; s.lh = lh; s.fmt = fmt; s.wrapx = a.wrapx; s.wrapy = a.wrapy; s.smooth = cfg.smooth;
                s.pal = w.ctx.palSrc & 0x3FFFFF;
                s.palBytes = a.pal ? ((fmt == F_PALETTED || fmt == F_PALETTED8) ? 1024u : 512u) : 0;
                // the cache rows this blit reads (axis-aligned: the band's first and
                // last output rows; rotated: all of them — such cells are small)
                int tile0 = 0, tile1 = 0x7FFF;
                if (!rotated) {
                    const int32_t fy0 = (int32_t)((((int64_t)a.oy0 << 16) + 0x8000) * cfg.invYQ16 >> 16) - vy;
                    const int32_t fy1 = (int32_t)((((int64_t)(a.oy1 - 1) << 16) + 0x8000) * cfg.invYQ16 >> 16) - vy;
                    int32_t t0 = (int32_t)((((int64_t)a.Eq * fy0) >> 16) + a.Fq) >> 16, t1 = (int32_t)((((int64_t)a.Eq * fy1) >> 16) + a.Fq) >> 16;
                    if (t0 > t1) { const int32_t t = t0; t0 = t1; t1 = t; }
                    // in cache rows: tv / sv
                    // 32-bit: t is a texel row (< 2^19), the M33 divides in one instruction;
                    // a 64-bit operand here was a libgcc __aeabi_ldivmod call into FLASH.
                    tile0 = (int)(((uint32_t)(t0 < 0 ? 0 : t0) * 256u) / sv8) >> MIP_TILE_SHIFT;
                    tile1 = (int)(((uint32_t)(t1 < 0 ? 0 : t1) * 256u) / sv8 + 1u) >> MIP_TILE_SHIFT;
                    if (a.wrapy) { tile0 = 0; tile1 = 0x7FFF; }
                }
#if FT812_TRACE
                const uint32_t mt0 = ftTick(cfg);
#endif
                MipEntry* e = mipGet(cfg, s, (uint16_t)su8, (uint16_t)sv8, tile0, tile1, a.data, a.avail, a.pal, a.palAvail, a.pal32);
#if FT812_TRACE
                g_renderStats.mipGetUs += ftTick(cfg) - mt0;
#endif
                if (e) {
                    // the matrix onto the cache image: u' = u / su, v' = v / sv
                    // x' = (x << 8) / s as a multiply by the Q32 reciprocal (two 32-bit
                    // divides instead of six 64-bit libgcc calls per cell). |x| < 2^26
                    // (Cq/Fq carry the vertex), s >= 384 (minified 1.5x): the product
                    // fits int64 and the error is under 2^-6 of a Q16 unit.
                    const int64_t ru = (int64_t)(0xFFFFFFFFu / su8) + 1, rv = (int64_t)(0xFFFFFFFFu / sv8) + 1;   // ~2^32/s, 32-bit udiv (a 64-bit divide is a libgcc call)
                    a.Aq = (int32_t)(((int64_t)a.Aq * ru) >> 24); a.Bq = (int32_t)(((int64_t)a.Bq * ru) >> 24); a.Cq = (int32_t)(((int64_t)a.Cq * ru) >> 24);
                    a.Dq = (int32_t)(((int64_t)a.Dq * rv) >> 24); a.Eq = (int32_t)(((int64_t)a.Eq * rv) >> 24); a.Fq = (int32_t)(((int64_t)a.Fq * rv) >> 24);
                    a.tiles = e->tiles; a.stride = e->cw * 2u; a.lw = e->cw; a.lh = e->ch;
                    a.data = nullptr; a.avail = 0; a.pal = nullptr; a.pal32 = nullptr; a.palAvail = 0;
                    a.bilinear = false; a.box = false;          // the cache is already filtered
                    FTR(g_renderStats.mipCells++);
                    a.vx = vx; a.vy = vy;
                    blitBitmap<F_ARGB4>(a);                     // ARGB4444 is the F_ARGB4 layout
                    return;
                }
                FTR(g_renderStats.mipRefused++);
            }
        }
    }
    a.vx = vx; a.vy = vy;
    switch (fmt) {
        case F_ARGB1555:     blitBitmap<F_ARGB1555>(a); break;
        case F_L1:           blitBitmap<F_L1>(a); break;
        case F_L4:           blitBitmap<F_L4>(a); break;
        case F_L8:           blitBitmap<F_L8>(a); break;
        case F_RGB332:       blitBitmap<F_RGB332>(a); break;
        case F_ARGB2:        blitBitmap<F_ARGB2>(a); break;
        case F_ARGB4:        blitBitmap<F_ARGB4>(a); break;
        case F_RGB565:       blitBitmap<F_RGB565>(a); break;
        case F_PALETTED:     blitBitmap<F_PALETTED>(a); break;
        case F_PALETTED565:  blitBitmap<F_PALETTED565>(a); break;
        case F_PALETTED4444: blitBitmap<F_PALETTED4444>(a); break;
        case F_PALETTED8:    blitBitmap<F_PALETTED8>(a); break;
        case F_L2:           blitBitmap<F_L2>(a); break;
        default: break;
    }
}

// ── geometric primitives (coverage-ramped, float) ────────────────────────────
static FT_HOT void coverPixel(Walk& w, int ox, int oy, float cov) {
    FTR(g_renderStats.pxPrim++);
    if (cov <= 0.f) return;
    if (cov > 1.f) cov = 1.f;
    const Ctx& c = w.ctx;
    const uint32_t sa = (uint32_t)((c.color >> 24) * cov + 0.5f);
    blendPixel(w.band + (size_t)(oy - w.row0) * w.cfg->outW + ox, (c.color >> 16) & 255, (c.color >> 8) & 255, c.color & 255, sa, c);
}

// Output pixel centre in FT px.
static inline float centreX(const RenderCfg& c, int ox) { return ((float)ox + 0.5f) * (float)c.invXQ16 * (1.0f / 65536.f); }
static inline float centreY(const RenderCfg& c, int oy) { return ((float)oy + 0.5f) * (float)c.invYQ16 * (1.0f / 65536.f); }

static void rangeFor(Walk& w, int32_t x0, int32_t x1, int32_t y0, int32_t y1,
                     int& ox0, int& ox1, int& oy0, int& oy1) {
    const RenderCfg& c = *w.cfg;
    ox0 = outMin(x0, c.sxQ16); ox1 = outEnd(x1, c.sxQ16);
    oy0 = outMin(y0, c.syQ16); oy1 = outEnd(y1, c.syQ16);
    if (ox0 < w.clipX0) ox0 = w.clipX0;
    if (ox1 > w.clipX1) ox1 = w.clipX1;
    if (oy0 < w.clipY0) oy0 = w.clipY0;
    if (oy1 > w.clipY1) oy1 = w.clipY1;
}

static FT_HOT void drawPoint(Walk& w, int32_t vx, int32_t vy) {
    FTR(g_renderStats.points++);
    const int32_t r = (int32_t)w.ctx.pointSize << 12;   // 1/16 px → Q16
    int ox0, ox1, oy0, oy1;
    rangeFor(w, vx - r - 0x10000, vx + r + 0x10000, vy - r - 0x10000, vy + r + 0x10000, ox0, ox1, oy0, oy1);
    if (ox0 >= ox1 || oy0 >= oy1) return;
    const float cx = vx * (1.0f / 65536.f), cy = vy * (1.0f / 65536.f), rf = r * (1.0f / 65536.f);
    for (int oy = oy0; oy < oy1; oy++) {
        const float dy = centreY(*w.cfg, oy) - cy;
        for (int ox = ox0; ox < ox1; ox++) {
            const float dx = centreX(*w.cfg, ox) - cx;
            coverPixel(w, ox, oy, rf + 0.5f - sqrtf(dx * dx + dy * dy));
        }
    }
}

static FT_HOT void drawLine(Walk& w, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    FTR(g_renderStats.lines++);
    const int32_t r = (int32_t)w.ctx.lineWidth << 12;
    int ox0, ox1, oy0, oy1;
    const int32_t bx0 = (x0 < x1 ? x0 : x1) - r - 0x10000, bx1 = (x0 < x1 ? x1 : x0) + r + 0x10000;
    const int32_t by0 = (y0 < y1 ? y0 : y1) - r - 0x10000, by1 = (y0 < y1 ? y1 : y0) + r + 0x10000;
    rangeFor(w, bx0, bx1, by0, by1, ox0, ox1, oy0, oy1);
    if (ox0 >= ox1 || oy0 >= oy1) return;
    const float ax = x0 / 65536.f, ay = y0 / 65536.f, bx = x1 / 65536.f, by = y1 / 65536.f, rf = r / 65536.f;
    const float ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
    for (int oy = oy0; oy < oy1; oy++) {
        const float py = centreY(*w.cfg, oy);
        for (int ox = ox0; ox < ox1; ox++) {
            const float px = centreX(*w.cfg, ox);
            float t = len2 > 0.f ? ((px - ax) * ex + (py - ay) * ey) / len2 : 0.f;
            if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
            const float dx = px - (ax + t * ex), dy = py - (ay + t * ey);
            coverPixel(w, ox, oy, rf + 0.5f - sqrtf(dx * dx + dy * dy));
        }
    }
}

static FT_HOT void drawRect(Walk& w, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    FTR(g_renderStats.rects++);
    const int32_t r = (int32_t)w.ctx.lineWidth << 12;
    if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int32_t t = y0; y0 = y1; y1 = t; }
    int ox0, ox1, oy0, oy1;
    rangeFor(w, x0 - r - 0x10000, x1 + r + 0x10000, y0 - r - 0x10000, y1 + r + 0x10000, ox0, ox1, oy0, oy1);
    if (ox0 >= ox1 || oy0 >= oy1) return;
    const float fx0 = x0 / 65536.f, fy0 = y0 / 65536.f, fx1 = x1 / 65536.f, fy1 = y1 / 65536.f, rf = r / 65536.f;
    for (int oy = oy0; oy < oy1; oy++) {
        const float py = centreY(*w.cfg, oy);
        float dy = 0.f; if (py < fy0) dy = fy0 - py; else if (py > fy1) dy = py - fy1;
        for (int ox = ox0; ox < ox1; ox++) {
            const float px = centreX(*w.cfg, ox);
            float dx = 0.f; if (px < fx0) dx = fx0 - px; else if (px > fx1) dx = px - fx1;
            coverPixel(w, ox, oy, rf + 0.5f - sqrtf(dx * dx + dy * dy));
        }
    }
}

// EDGE_STRIP_*: the region between the segment and one screen edge.
static FT_HOT void drawEdge(Walk& w, int prim, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    FTR(g_renderStats.edges++);
    const RenderCfg& c = *w.cfg;
    const float ax = x0 / 65536.f, ay = y0 / 65536.f, bx = x1 / 65536.f, by = y1 / 65536.f;
    if (prim == PR_EDGE_A || prim == PR_EDGE_B) {
        int ox0, ox1, oy0, oy1;
        const int32_t bx0 = x0 < x1 ? x0 : x1, bx1 = x0 < x1 ? x1 : x0;
        const int32_t by0 = prim == PR_EDGE_A ? 0 : (y0 < y1 ? y0 : y1);
        const int32_t by1 = prim == PR_EDGE_A ? (y0 > y1 ? y0 : y1) : (int32_t)c.vsize << 16;
        rangeFor(w, bx0, bx1, by0, by1, ox0, ox1, oy0, oy1);
        if (ox0 >= ox1 || oy0 >= oy1 || bx == ax) return;
        for (int ox = ox0; ox < ox1; ox++) {
            const float px = centreX(c, ox);
            const float ly = ay + (by - ay) * (px - ax) / (bx - ax);
            for (int oy = oy0; oy < oy1; oy++) {
                const float py = centreY(c, oy);
                if (prim == PR_EDGE_A ? (py <= ly) : (py >= ly)) coverPixel(w, ox, oy, 1.f);
            }
        }
    } else {
        int ox0, ox1, oy0, oy1;
        const int32_t by0 = y0 < y1 ? y0 : y1, by1 = y0 < y1 ? y1 : y0;
        const int32_t bx0 = prim == PR_EDGE_L ? 0 : (x0 < x1 ? x0 : x1);
        const int32_t bx1 = prim == PR_EDGE_L ? (x0 > x1 ? x0 : x1) : (int32_t)c.hsize << 16;
        rangeFor(w, bx0, bx1, by0, by1, ox0, ox1, oy0, oy1);
        if (ox0 >= ox1 || oy0 >= oy1 || by == ay) return;
        for (int oy = oy0; oy < oy1; oy++) {
            const float py = centreY(c, oy);
            const float lx = ax + (bx - ax) * (py - ay) / (by - ay);
            for (int ox = ox0; ox < ox1; ox++) {
                const float px = centreX(c, ox);
                if (prim == PR_EDGE_L ? (px <= lx) : (px >= lx)) coverPixel(w, ox, oy, 1.f);
            }
        }
    }
}

static FT_WALK_HOT void vertex(Walk& w, int32_t vx, int32_t vy, int handle, int cell) {
    switch (w.prim) {
        case PR_BITMAPS: drawBitmap(w, vx, vy, handle, cell); break;
        case PR_POINTS:  drawPoint(w, vx, vy); break;
        case PR_LINES:
        case PR_RECTS:
            if (w.nv == 0) { w.vx[0] = vx; w.vy[0] = vy; w.nv = 1; }
            else {
                if (w.prim == PR_LINES) drawLine(w, w.vx[0], w.vy[0], vx, vy);
                else drawRect(w, w.vx[0], w.vy[0], vx, vy);
                w.nv = 0;
            }
            break;
        case PR_LINE_STRIP:
        case PR_EDGE_R: case PR_EDGE_L: case PR_EDGE_A: case PR_EDGE_B:
            if (w.nv) {
                if (w.prim == PR_LINE_STRIP) drawLine(w, w.vx[0], w.vy[0], vx, vy);
                else drawEdge(w, w.prim, w.vx[0], w.vy[0], vx, vy);
            }
            w.vx[0] = vx; w.vy[0] = vy; w.nv = 1;
            break;
        default: w.st->warned |= W_PRIM; break;
    }
}

static FT_WALK_HOT void clearBand(Walk& w) {
    FTR(g_renderStats.clears++);
    const RenderCfg& c = *w.cfg;
    const uint32_t col = w.ctx.clearColor;
    const uint8_t m = w.ctx.colorMask;
    for (int oy = w.clipY0; oy < w.clipY1; oy++) {
        uint32_t* dp = w.band + (size_t)(oy - w.row0) * c.outW;
        if (m == 0x0F) { for (int ox = w.clipX0; ox < w.clipX1; ox++) dp[ox] = col; continue; }
        uint32_t keep = 0;
        if (!(m & 8)) keep |= 0x00FF0000;
        if (!(m & 4)) keep |= 0x0000FF00;
        if (!(m & 2)) keep |= 0x000000FF;
        if (!(m & 1)) keep |= 0xFF000000;
        for (int ox = w.clipX0; ox < w.clipX1; ox++) dp[ox] = (dp[ox] & keep) | (col & ~keep);
    }
}

// SAVE_CONTEXT / RESTORE_CONTEXT: a word loop in SRAM. A struct assignment here
// became a libc memcpy through a FLASH veneer (hw 2026-10-06, mip7: three
// memcpy veneers in ft812RenderBand) — ZUMA brackets its sprites with the pair.
// The attribute keeps GCC from turning the loop straight back into memcpy.
static FT_WALK_HOT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns"))) void ctxCopy(Ctx& d, const Ctx& s) {
    static_assert(sizeof(Ctx) % 4 == 0, "Ctx is word-sized");
    uint32_t* dp = (uint32_t*)&d; const uint32_t* sp = (const uint32_t*)&s;
    for (size_t i = 0; i < sizeof(Ctx) / 4; i++) dp[i] = sp[i];
}

// One DL word. Returns false at DISPLAY.
static FT_WALK_HOT bool execWord(Walk& w, uint32_t word, uint32_t& pc, int depth) {
    const uint32_t op = word >> 24;
    if (op & 0xC0) {          // VERTEX2II (b7) / VERTEX2F (b6)
#if FT812_TRACE
        const uint32_t tv0 = ftTick(*w.cfg);
#endif
        if (op & 0x80) {
            const int32_t x = (int32_t)((word >> 21) & 0x1FF), y = (int32_t)((word >> 12) & 0x1FF);
            vertex(w, (x << 16) + (w.ctx.vtx << 12), (y << 16) + (w.ctx.vty << 12), (word >> 7) & 31, word & 127);
        } else {
            const int32_t x = ((int32_t)(word << 2)) >> 17, y = ((int32_t)(word << 17)) >> 17;
            const int sh = 16 - w.ctx.vfmt;
            vertex(w, (x << sh) + (w.ctx.vtx << 12), (y << sh) + (w.ctx.vty << 12), w.ctx.handle, w.ctx.cell);
        }
        FTR(g_renderStats.vtxUs += ftTick(*w.cfg) - tv0);
        return true;
    }
    Ctx& c = w.ctx;
    switch (op) {
        case 0x00: return false;                                            // DISPLAY
        case 0x01: w.hnd[c.handle].source = word & 0x3FFFFF; break;         // BITMAP_SOURCE
        case 0x02: c.clearColor = (c.clearColor & 0xFF000000) | (word & 0xFFFFFF); break;
        case 0x03: break;                                                   // TAG
        case 0x04: c.color = (c.color & 0xFF000000) | (word & 0xFFFFFF); break;
        case 0x05: c.handle = word & 31; break;
        case 0x06: c.cell = word & 127; break;
        case 0x07: {                                                        // BITMAP_LAYOUT
            BitmapHandle& h = w.hnd[c.handle];
            h.fmt = (word >> 19) & 31;
            h.stride = (uint16_t)((h.stride & 0xC00) | ((word >> 9) & 0x3FF));
            h.lh = (uint16_t)((h.lh & 0x600) | (word & 0x1FF));
            break;
        }
        case 0x08: {                                                        // BITMAP_SIZE
            BitmapHandle& h = w.hnd[c.handle];
            h.filter = (word >> 20) & 1; h.wrapx = (word >> 19) & 1; h.wrapy = (word >> 18) & 1;
            h.sw = (uint16_t)((h.sw & 0x600) | ((word >> 9) & 0x1FF));
            h.sh = (uint16_t)((h.sh & 0x600) | (word & 0x1FF));
            break;
        }
        case 0x09: c.alphaFunc = (word >> 8) & 7; c.alphaRef = word & 255; break;
        case 0x0A: w.st->warned |= W_STENCIL; break;                        // STENCIL_FUNC
        case 0x0B: c.blendSrc = (word >> 3) & 7; c.blendDst = word & 7; break;
        case 0x0C: break;                                                   // STENCIL_OP
        case 0x0D: c.pointSize = word & 0x1FFF; break;
        case 0x0E: c.lineWidth = word & 0xFFF; break;
        case 0x0F: c.clearColor = (c.clearColor & 0x00FFFFFF) | ((word & 255) << 24); break;
        case 0x10: c.color = (c.color & 0x00FFFFFF) | ((word & 255) << 24); break;
        case 0x11: case 0x12: case 0x13: case 0x14: break;                  // CLEAR_STENCIL/TAG, STENCIL_MASK, TAG_MASK
        case 0x15: c.A = ((int32_t)(word << 15)) >> 15; break;
        case 0x16: c.B = ((int32_t)(word << 15)) >> 15; break;
        case 0x17: c.C = ((int32_t)(word << 8)) >> 8; break;
        case 0x18: c.D = ((int32_t)(word << 15)) >> 15; break;
        case 0x19: c.E = ((int32_t)(word << 15)) >> 15; break;
        case 0x1A: c.F = ((int32_t)(word << 8)) >> 8; break;
        case 0x1B: c.scX = (word >> 11) & 0x7FF; c.scY = word & 0x7FF; clipRecalc(w); break;
        case 0x1C: c.scW = (word >> 12) & 0xFFF; c.scH = word & 0xFFF; clipRecalc(w); break;
        case 0x1D:                                                          // CALL
            if (w.csp < 4) { w.callStack[w.csp++] = (uint16_t)pc; pc = word & 0x7FF; }
            else w.st->warned |= W_DEPTH;
            break;
        case 0x1E: pc = word & 0x7FF; break;                                // JUMP
        case 0x1F: w.prim = word & 15; w.nv = 0; break;                     // BEGIN
        case 0x20: c.colorMask = word & 15; break;
        case 0x21: w.prim = PR_NONE; w.nv = 0; break;                       // END
        case 0x22: FTR(g_renderStats.ctxCopies++); if (w.sp < 4) ctxCopy(w.stack[w.sp++], c); else w.st->warned |= W_DEPTH; break;
        case 0x23: FTR(g_renderStats.ctxCopies++); if (w.sp > 0) { ctxCopy(c, w.stack[--w.sp]); clipRecalc(w); } break;
        case 0x24: if (w.csp > 0) pc = w.callStack[--w.csp]; else return false;   // RETURN
            break;
        case 0x25:                                                          // MACRO
            if (depth == 0) return execWord(w, w.cfg->mem->macro[word & 1], pc, 1);
            break;
        case 0x26: if (word & 4) clearBand(w); break;                       // CLEAR (colour only)
        case 0x27: c.vfmt = word & 7; if (c.vfmt > 4) c.vfmt = 4; break;
        case 0x28: {                                                        // BITMAP_LAYOUT_H
            BitmapHandle& h = w.hnd[c.handle];
            h.stride = (uint16_t)((h.stride & 0x3FF) | (((word >> 2) & 3) << 10));
            h.lh = (uint16_t)((h.lh & 0x1FF) | ((word & 3) << 9));
            break;
        }
        case 0x29: {                                                        // BITMAP_SIZE_H
            BitmapHandle& h = w.hnd[c.handle];
            h.sw = (uint16_t)((h.sw & 0x1FF) | (((word >> 2) & 3) << 9));
            h.sh = (uint16_t)((h.sh & 0x1FF) | ((word & 3) << 9));
            break;
        }
        case 0x2A: c.palSrc = word & 0x3FFFFF; break;
        case 0x2B: c.vtx = ((int32_t)(word << 15)) >> 15; break;
        case 0x2C: c.vty = ((int32_t)(word << 15)) >> 15; break;
        case 0x2D: break;                                                   // NOP
        default: break;
    }
    return true;
}

size_t ft812RenderScratchBytes() { return sizeof(Walk); }

void ft812PalCacheInvalidate(RenderState& st) { memset(st.pal, 0, sizeof(st.pal)); }

void ft812RenderStateReset(RenderState& st, void (*romFontHandle)(int handle, BitmapHandle* out)) {
    memset(&st, 0, sizeof(st));
    for (int h = 16; h < 32; h++) if (romFontHandle) romFontHandle(h, &st.handle[h]);
}

// The band setup's fill and copy as word loops in SRAM. As libc memset / memcpy
// they were FLASH calls whose tight loops core0's PSRAM traffic evicts mid-loop:
// 139 us per BAND on R-Type = 4.2 ms a frame, 39 us on ZUMA (hw 2026-10-06, mip8
// — `walk - loop`). The attribute keeps GCC from turning them back into the calls.
static FT_WALK_HOT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns"))) void wordFill(uint32_t* d, uint32_t v, size_t n) {
    for (; n >= 4; n -= 4, d += 4) { d[0] = v; d[1] = v; d[2] = v; d[3] = v; }
    while (n--) *d++ = v;
}
static FT_WALK_HOT __attribute__((noinline, optimize("no-tree-loop-distribute-patterns"))) void wordCopy(uint32_t* d, const uint32_t* s, size_t n) {
    for (; n >= 4; n -= 4, d += 4, s += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
    while (n--) *d++ = *s++;
}

FT_WALK_HOT void ft812RenderBand(const RenderCfg& cfg, RenderState& st, int row0, int row1, uint32_t* band) {
    if (row1 <= row0) return;
    if (cfg.mip && row0 == 0) { cfg.mip->resetsThisFrame = 0; cfg.mip->off = 0; cfg.mip->frame++; }   // a new frame: the cache may try again
    wordFill(band, 0, (size_t)cfg.outW * (size_t)(row1 - row0));
    if (!cfg.scratch) return;                  // no Walk scratch: nothing can be rendered (see Walk)
    Walk& w = *(Walk*)cfg.scratch;
    w.cfg = &cfg; w.st = &st; w.band = band; w.row0 = row0; w.row1 = row1;
    w.genNow = cfg.mem->gen ? cfg.mem->gen() : 0;
    static_assert(sizeof(w.hnd) % 4 == 0 && sizeof(st.handle) == sizeof(w.hnd), "handle tables are word-sized");
    wordCopy((uint32_t*)w.hnd, (const uint32_t*)st.handle, sizeof(w.hnd) / 4);
    ctxDefault(w.ctx);
    w.sp = 0; w.csp = 0; w.prim = PR_NONE; w.nv = 0;
    clipRecalc(w);
    uint32_t pc = 0;
    // A JUMP loop must not hang core1: 8192 words is four full lists.
    int steps = 0;
#if FT812_TRACE
    const uint32_t tl0 = ftTick(cfg);
#endif
    for (; steps < 8192; steps++) {
        if (pc >= RAM_DL_WORDS) break;
        const uint32_t word = cfg.mem->dl[pc++];
        if (!execWord(w, word, pc, 0)) break;
    }
    FTR(g_renderStats.loopUs += ftTick(cfg) - tl0);
    FTR(g_renderStats.bands++; g_renderStats.words += (uint32_t)steps);
    // The handles persist into the next list: every band walks the same list from
    // the same start state, so only the LAST band's end state is kept.
    if (row1 >= cfg.outH) wordCopy((uint32_t*)st.handle, (const uint32_t*)w.hnd, sizeof(st.handle) / 4);
}

// ── quantizer ────────────────────────────────────────────────────────────────
static const uint8_t kBayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

void ft812PalLutInit(PalLut& p, int poolSlots, const uint8_t* poolSlot) {
    if (poolSlots >= 240)      { p.rl = 6; p.gl = 8; p.bl = 5; }
    else if (poolSlots >= 180) { p.rl = 6; p.gl = 6; p.bl = 5; }
    else if (poolSlots >= 125) { p.rl = 5; p.gl = 5; p.bl = 5; }
    else                       { p.rl = 4; p.gl = 4; p.bl = 4; }
    const int L[3] = { p.rl, p.gl, p.bl };
    for (int c = 0; c < 3; c++)
        for (int x = 0; x < 256 + 64; x++) {
            int q = x * (L[c] - 1) / 255;
            if (q > L[c] - 1) q = L[c] - 1;
            p.lut[c][x] = (uint8_t)q;
            if (c == 0) p.lutm[0][x] = (uint8_t)(q * p.gl * p.bl);
            if (c == 1) p.lutm[1][x] = (uint8_t)(q * p.bl);
        }
    const int n = p.rl * p.gl * p.bl;
    for (int i = 0; i < 240; i++) p.slot[i] = (i < n && i < poolSlots) ? poolSlot[i] : 0;
}

uint32_t ft812PalLutColor(const PalLut& p, int i) {
    const int b = i % p.bl, g = (i / p.bl) % p.gl, r = i / (p.bl * p.gl);
    const uint32_t R = (uint32_t)(r * 255 / (p.rl - 1)), G = (uint32_t)(g * 255 / (p.gl - 1)), B = (uint32_t)(b * 255 / (p.bl - 1));
    return (R << 16) | (G << 8) | B;
}

// ~100k pixels a frame: everything the loop reads is a local, the dither is an
// add from a per-row table, and the cube index is three loads and two adds (the
// R and G levels come pre-multiplied by their weight, PalLut::lutm). It used to
// reload p.bl and recompute three dither products per pixel — R-Type spent about
// as long here and in ftPutRow as in its sprites (hw 2026-10-01).
__attribute__((optimize("O2")))
FT_HOT_Q void ft812QuantizeRow(const PalLut& p, const uint32_t* src, int w, int y, uint8_t* dst, int x0) {
    const uint8_t* br = kBayer4[y & 3];
    // Dither amplitude = one quantization step, spread over the 16 Bayer levels.
    const int sr = 255 / (p.rl - 1), sg = 255 / (p.gl - 1), sb = 255 / (p.bl - 1);
    int dr[4], dg[4], db[4];
    for (int i = 0; i < 4; i++) { const int t = br[i]; dr[i] = (t * sr) >> 4; dg[i] = (t * sg) >> 4; db[i] = (t * sb) >> 4; }
    const uint8_t* const lr = p.lutm[0], * const lg = p.lutm[1], * const lb = p.lut[2], * const slot = p.slot;
    uint32_t ph = (uint32_t)x0;
    for (int x = 0; x < w; x++, ph++) {
        const uint32_t px = src[x];
        dst[x] = slot[lr[((px >> 16) & 255) + dr[ph & 3]] + lg[((px >> 8) & 255) + dg[ph & 3]] + lb[(px & 255) + db[ph & 3]]];
    }
}

// ── adaptive palette ─────────────────────────────────────────────────────────
// Two bin layouts over the same 4096-entry histogram / map (AdaptPal::space):
//   ADAPT_RGB444  a = R, b = G, c = B, 4 bits each — the display-list path;
//   ADAPT_YCC633  a = Y >> 2 (6 bits), b = Cb, c = Cr as (v - 56) >> 4 clamped 0..7 (3 bits each) — the MJPEG path.
// The second exists because a film frame is mostly smooth luma: with 16 levels
// per channel every gradient stepped in visible contours (owner, 2026-10-01),
// and no palette can fix that, since the entries are means of BIN CENTRES. 64
// luma levels do; the 8 chroma levels are dithered by the sink.
namespace {
// (AdaptSpace, Ft812Render.h: index shifts, axis sizes, value units per bin, the
// value of bin 0's centre, luma weight.) The chroma bins of the YCC layout are
// CENTRED on multiples of their width (level = (v + half) >> shift, capped), so
// that neutral — Cb = Cr = 128 — is a bin centre. With plain v >> n bins 128 is a
// bin EDGE: grey and black had no entry of their own and came out as a dither of
// a green-tinted and a purple-tinted one (host simulation of a real frame,
// 2026-10-01: the film's black bars went purple).
typedef AdaptSpace Space;
static const Space kSpace[2] = { { 8, 4, 16, 16, 16, 16, 16, 16, 8, 8, 8, 1 }, FT812_ADAPT_YCC_LAYOUT };
static inline uint32_t nativeToRgb(const Space& sp, uint32_t n, bool ycc) {
    (void)sp;
    if (!ycc) return n;
    const int y = (int)((n >> 16) & 255), cb = (int)((n >> 8) & 255) - 128, cr = (int)(n & 255) - 128;
    int r = y + ((1436 * cr) >> 10), g = y - ((352 * cb + 731 * cr) >> 10), b = y + ((1815 * cb) >> 10);
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
} // namespace

const AdaptSpace& ft812AdaptSpace(int space) { return kSpace[space == ADAPT_YCC633 ? 1 : 0]; }

void ft812AdaptClear(AdaptPal& ap, int space) { memset(ap.hist, 0, sizeof(ap.hist)); ap.total = 0; ap.histSpace = (uint8_t)space; }

FT_HOT_Q void ft812AdaptAccumulate(AdaptPal& ap, const uint32_t* src, int w, int step) {
    uint16_t* h = ap.hist;
    if (step < 1) step = 1;
    uint32_t n = 0;
    for (int x = 0; x < w; x += step, n++) {
        const uint32_t px = src[x];
        const uint32_t bin = ((px >> 12) & 0xF00) | ((px >> 8) & 0x0F0) | ((px >> 4) & 0x00F);
        if (h[bin] != 0xFFFF) h[bin]++;
    }
    ap.total += n;
}

// One cell of the 512-cell fold used to tell "the frame's colours moved": the
// top three bits of each axis. (Computed on the fly — core1's stack is 2 KB, no
// room for a 512-entry temporary.)
static inline uint32_t coarseAt(const uint16_t* h, int ci, int space) {
    const Space& sp = kSpace[space == ADAPT_YCC633 ? 1 : 0];
    // the fine bins sharing the top three bits of each axis (an axis of 8 or fewer levels keeps them all)
    const int fa = sp.na > 8 ? sp.na / 8 : 1, fb = sp.nb > 8 ? sp.nb / 8 : 1, fc = sp.nc > 8 ? sp.nc / 8 : 1;
    const int a = ((ci >> 6) & 7) * fa, b = ((ci >> 3) & 7) * fb, c = (ci & 7) * fc;
    if (a >= sp.na || b >= sp.nb || c >= sp.nc) return 0;
    uint32_t n = 0;
    for (int da = 0; da < fa; da++) for (int db = 0; db < fb; db++) for (int dc = 0; dc < fc; dc++)
        n += h[((a + da) << sp.sa) | ((b + db) << sp.sb) | (c + dc)];
    return n > 0xFFFF ? 0xFFFF : n;
}

bool ft812AdaptChanged(const AdaptPal& ap, int permille) {
    if (!ap.built || ap.space != ap.histSpace) return true;
    uint32_t diff = 0, tot = 0;
    for (int i = 0; i < 512; i++) {
        const uint32_t now = coarseAt(ap.hist, i, ap.histSpace);
        const int d = (int)now - (int)ap.coarse[i];
        diff += (uint32_t)(d < 0 ? -d : d);
        tot += now;
    }
    return tot ? (diff * 1000u / tot) > (uint32_t)permille : false;
}

namespace {
typedef AdaptPal::Box CutBox;     // r/g/b = the a/b/c axis of the space in use
static uint32_t boxCount(const uint16_t* h, const Space& sp, const CutBox& x) {
    uint32_t c = 0;
    for (int r = x.r0; r <= x.r1; r++) for (int g = x.g0; g <= x.g1; g++) for (int b = x.b0; b <= x.b1; b++) c += h[(r << sp.sa) | (g << sp.sb) | b];
    return c;
}
static void boxShrink(const uint16_t* h, const Space& sp, CutBox& x) {   // to the bounding box of its populated bins
    int r0 = 64, r1 = -1, g0 = 64, g1 = -1, b0 = 64, b1 = -1;
    for (int r = x.r0; r <= x.r1; r++) for (int g = x.g0; g <= x.g1; g++) for (int b = x.b0; b <= x.b1; b++)
        if (h[(r << sp.sa) | (g << sp.sb) | b]) { if (r < r0) r0 = r; if (r > r1) r1 = r; if (g < g0) g0 = g; if (g > g1) g1 = g; if (b < b0) b0 = b; if (b > b1) b1 = b; }
    if (r1 >= 0) { x.r0 = (uint8_t)r0; x.r1 = (uint8_t)r1; x.g0 = (uint8_t)g0; x.g1 = (uint8_t)g1; x.b0 = (uint8_t)b0; x.b1 = (uint8_t)b1; }
}
} // namespace

void ft812AdaptBuild(AdaptPal& ap, int slots) {
    if (slots > AdaptPal::MAX) slots = AdaptPal::MAX;
    const int maxBox = slots - 1;           // entry 0 is black, the rest are boxes
    const bool ycc = ap.histSpace == ADAPT_YCC633;
    const Space& sp = kSpace[ycc ? 1 : 0];
    const uint16_t* h = ap.hist;
    CutBox* box = ap.box;
    int n = 0;
    box[0] = { 0, (uint8_t)(sp.na - 1), 0, (uint8_t)(sp.nb - 1), 0, (uint8_t)(sp.nc - 1), 0 };
    boxShrink(h, sp, box[0]); box[0].count = boxCount(h, sp, box[0]);
    n = box[0].count ? 1 : 0;
    // median cut: split the box with the most pixels (weighted by its extent so a
    // populous single-bin box does not hog the choice) along its longest axis —
    // extents in value units, luma counted double in the YCC layout
    while (n > 0 && n < maxBox) {
        int pick = -1; uint64_t best = 0;
        for (int i = 0; i < n; i++) {
            const int ext = (box[i].r1 - box[i].r0) * sp.ua * sp.wa + (box[i].g1 - box[i].g0) * sp.ub + (box[i].b1 - box[i].b0) * sp.uc;
            if (!ext) continue;
            const uint64_t w = (uint64_t)box[i].count * (uint64_t)(ext + 16);
            if (w > best) { best = w; pick = i; }
        }
        if (pick < 0) break;
        CutBox& x = box[pick];
        const int er = (x.r1 - x.r0) * sp.ua * sp.wa, eg = (x.g1 - x.g0) * sp.ub, eb = (x.b1 - x.b0) * sp.uc;
        const int axis = (er >= eg && er >= eb) ? 0 : (eg >= eb ? 1 : 2);
        // walk the axis until half the count is behind us
        const uint32_t half = x.count / 2;
        uint32_t acc = 0; int cut = -1;
        const int lo = axis == 0 ? x.r0 : axis == 1 ? x.g0 : x.b0, hi = axis == 0 ? x.r1 : axis == 1 ? x.g1 : x.b1;
        for (int p = lo; p < hi; p++) {
            uint32_t plane = 0;
            if (axis == 0)      for (int g = x.g0; g <= x.g1; g++) for (int b = x.b0; b <= x.b1; b++) plane += h[(p << sp.sa) | (g << sp.sb) | b];
            else if (axis == 1) for (int r = x.r0; r <= x.r1; r++) for (int b = x.b0; b <= x.b1; b++) plane += h[(r << sp.sa) | (p << sp.sb) | b];
            else                for (int r = x.r0; r <= x.r1; r++) for (int g = x.g0; g <= x.g1; g++) plane += h[(r << sp.sa) | (g << sp.sb) | p];
            acc += plane;
            if (acc >= half) { cut = p; break; }
        }
        if (cut < 0) cut = lo;
        CutBox y = x;
        if (axis == 0)      { x.r1 = (uint8_t)cut; y.r0 = (uint8_t)(cut + 1); }
        else if (axis == 1) { x.g1 = (uint8_t)cut; y.g0 = (uint8_t)(cut + 1); }
        else                { x.b1 = (uint8_t)cut; y.b0 = (uint8_t)(cut + 1); }
        boxShrink(h, sp, x); x.count = boxCount(h, sp, x);
        boxShrink(h, sp, y); y.count = boxCount(h, sp, y);
        if (!y.count) { continue; }             // the split landed on an empty tail; x keeps its shrunk extent
        if (!x.count) { x = y; continue; }
        box[n++] = y;
    }
    // colours = count-weighted mean of the bin centres (in the layout's own axes), as RGB
    for (int i = 0; i < n; i++) {
        uint64_t sr = 0, sg = 0, sb = 0; uint32_t c = 0;
        const CutBox& x = box[i];
        for (int r = x.r0; r <= x.r1; r++) for (int g = x.g0; g <= x.g1; g++) for (int b = x.b0; b <= x.b1; b++) {
            const uint32_t k = h[(r << sp.sa) | (g << sp.sb) | b];
            sr += (uint64_t)k * (uint32_t)(r * sp.ua + sp.oa); sg += (uint64_t)k * (uint32_t)(g * sp.ub + sp.ob); sb += (uint64_t)k * (uint32_t)(b * sp.uc + sp.oc); c += k;
        }
        if (!c) c = 1;
        const uint32_t R = (uint32_t)(sr / c), G = (uint32_t)(sg / c), B = (uint32_t)(sb / c);
        ap.ncol[i] = nativeToRgb(sp, ((R > 255 ? 255 : R) << 16) | ((G > 255 ? 255 : G) << 8) | (B > 255 ? 255 : B), ycc);
    }
    // Entries: 0 = black, 1..n = the boxes, each on the index whose PREVIOUS colour
    // it continues. The rows on screen were written through the old map — this is
    // what keeps them looking right until they are rendered again.
    //   pass 1: mutual nearest neighbours keep their index (a surviving colour is
    //           never robbed of it by a new colour that merely happens to be close);
    //   pass 2: the rest take the nearest index still free; one the old palette
    //           did not have costs more than any real match.
    const int oldN = ap.built ? ap.n : 0;
    const auto dist = [](uint32_t c, uint32_t o) {
        const int dr = (int)((c >> 16) & 255) - (int)((o >> 16) & 255), dg = (int)((c >> 8) & 255) - (int)((o >> 8) & 255), db = (int)(c & 255) - (int)(o & 255);
        return (uint32_t)(dr * dr + dg * dg + db * db);
    };
    memset(ap.used, 0, sizeof(ap.used));
    for (int j = 0; j < n; j++) box[j].count = 0;      // the count is not needed any more: 0 = no index yet
    const int oldTop = oldN < n + 1 ? oldN : n + 1;     // old indices that exist in the new palette too
    for (int j = 0; j < n; j++) {
        uint32_t bestD = 0xFFFFFFFFu; int bi = 0;
        for (int i = 1; i < oldTop; i++) { const uint32_t d = dist(ap.ncol[j], ap.col[i]); if (d < bestD) { bestD = d; bi = i; } }
        if (!bi || ap.used[bi]) continue;
        int bj = j;
        for (int k = 0; k < n; k++) if (dist(ap.ncol[k], ap.col[bi]) < bestD) { bj = -1; break; }
        if (bj == j) { ap.used[bi] = 1; box[j].count = (uint32_t)bi; }
    }
    for (int j = 0; j < n; j++) {
        if (box[j].count) continue;
        uint32_t bestD = 0xFFFFFFFFu; int bi = 1;
        for (int i = 1; i <= n; i++) {
            if (ap.used[i]) continue;
            const uint32_t d = i < oldN ? dist(ap.ncol[j], ap.col[i]) : 0x40000000u + (uint32_t)i;
            if (d < bestD) { bestD = d; bi = i; }
        }
        ap.used[bi] = 1; box[j].count = (uint32_t)bi;
    }
    ap.col[0] = 0;
    for (int j = 0; j < n; j++) ap.col[box[j].count] = ap.ncol[j];
    n = n ? n + 1 : 0;
    // The map. A bin inside a box IS that box's entry — that is all of the frame
    // just measured, and it costs one pass over the boxes. Every other bin is left
    // UNRESOLVED (0xFF) and takes its nearest entry the first time a pixel lands in
    // it (ft812AdaptResolve). Searching all 4096 bins against 185 entries here cost
    // 25-55 ms on core1 — a visible hitch at every rebuild, i.e. every 8th frame of
    // a moving scene (hw 2026-10-01, `render: max` against `avg`).
    memset(ap.lut, 0xFF, sizeof(ap.lut));
    for (int jb = 0; jb < n - 1; jb++) {
        const CutBox& x = box[jb];
        const uint8_t e = (uint8_t)x.count;                      // the entry the matching gave this box
        for (int r = x.r0; r <= x.r1; r++) for (int g = x.g0; g <= x.g1; g++) for (int b = x.b0; b <= x.b1; b++) ap.lut[(r << sp.sa) | (g << sp.sb) | b] = e;
    }
    if (!ycc) ap.lut[0] = 0;                 // RGB: the darkest bin is the black entry (YCC: bin 0 is not black)
    ap.n = n;
    for (int ci = 0; ci < 512; ci++) ap.coarse[ci] = (uint16_t)coarseAt(ap.hist, ci, ap.histSpace);
    ap.space = ap.histSpace;
    ap.built = n > 0;
}

// An unresolved bin of the map.
//   YCC (MJPEG): the NEAREST BOX in the layout's own axes, luma weighted. Those
//     bins are mostly the sink's dither pushing a pixel one chroma bin over (the
//     histogram samples half the columns, so a bin hit only on the other half is
//     in no box), and the right answer is the box of the populated bin it was
//     pushed out of. Taking the nearest ENTRY to the bin's centre in RGB instead
//     turned a dark near-grey pushed one Cb bin up (+57 of blue in RGB) into a
//     saturated blue entry: "много синих точек" (hw 2026-10-01, adaptps3).
//   RGB (display list): no dither there, so the nearest entry to the centre.
uint8_t ft812AdaptResolve(AdaptPal& ap, uint32_t bin) {
    const bool ycc = ap.space == ADAPT_YCC633;
    const Space& sp = kSpace[ycc ? 1 : 0];
    bin &= 4095;
    const int ka = (int)(bin >> sp.sa), kb = (int)((bin >> sp.sb) & (uint32_t)(sp.nb - 1)), kc = (int)(bin & (uint32_t)(sp.nc - 1));
    if (ycc && ap.n > 1) {
        uint32_t bestD = 0xFFFFFFFFu; int bi = 0;
        for (int j = 0; j < ap.n - 1; j++) {
            const CutBox& x = ap.box[j];
            const int da = (ka < x.r0 ? x.r0 - ka : ka > x.r1 ? ka - x.r1 : 0) * sp.ua * sp.wa;
            const int db = (kb < x.g0 ? x.g0 - kb : kb > x.g1 ? kb - x.g1 : 0) * sp.ub;
            const int dc = (kc < x.b0 ? x.b0 - kc : kc > x.b1 ? kc - x.b1 : 0) * sp.uc;
            const uint32_t d = (uint32_t)(da * da + db * db + dc * dc);
            if (d < bestD) { bestD = d; bi = (int)x.count; }       // count = the entry the build gave this box
        }
        ap.lut[bin] = (uint8_t)bi;
        return (uint8_t)bi;
    }
    const uint32_t cen = nativeToRgb(sp, ((uint32_t)(ka * sp.ua + sp.oa) << 16) | ((uint32_t)(kb * sp.ub + sp.ob) << 8) | (uint32_t)(kc * sp.uc + sp.oc), ycc);
    const int r = (int)((cen >> 16) & 255), g = (int)((cen >> 8) & 255), b = (int)(cen & 255);
    uint32_t bestD = 0xFFFFFFFFu; int bi = 0;
    for (int i = 0; i < ap.n; i++) {
        const uint32_t c = ap.col[i];
        const int dr = r - (int)((c >> 16) & 255), dg = g - (int)((c >> 8) & 255), db = b - (int)(c & 255);
        const uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < bestD) { bestD = d; bi = i; }
    }
    ap.lut[bin] = (uint8_t)bi;
    return (uint8_t)bi;
}

// No dither here, on purpose. The cube can dither because its neighbouring
// levels are evenly spaced, so two of them average to the colour in between.
// This map only knows the nearest entry per 4-4-4 bin: pushing a pixel into the
// next bin lands on whatever entry happens to be nearest THERE, which need not
// lie on the line through the pixel's own colour — flat areas came out speckled
// with a foreign colour (hw 2026-10-01). The palette is cut from the frame
// itself, so the plain nearest entry is already close.
FT_HOT_Q void ft812QuantizeRowAdapt(AdaptPal& ap, const uint8_t* slotOf, const uint32_t* src, int w, int y, uint8_t* dst, uint8_t* lutCopy) {
    (void)y;
    uint8_t* lut = lutCopy ? lutCopy : ap.lut;
    for (int x = 0; x < w; x++) {
        const uint32_t px = src[x];
        const uint32_t bin = ((px >> 12) & 0xF00) | ((px >> 8) & 0x0F0) | ((px >> 4) & 0x00F);
        uint32_t e = lut[bin];
        if (e == 0xFF) { e = ft812AdaptResolve(ap, bin); lut[bin] = (uint8_t)e; }
        dst[x] = slotOf[e];
    }
}

} // namespace Ft812
