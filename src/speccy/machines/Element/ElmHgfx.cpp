// HGFX renderer and planar engine: see ElmHgfx.h. Flash, whole frame at EndFrame on
// core0 (docs/element-zx-plan.md); only HGFX 1 is drawn so far.
#include "speccy/machines/Element/ElmHgfx.h"
#include <string.h>

#include "app/Buffer.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "speccy/core/MemESP.h"
#include "app/ESPectrum.h"
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"

namespace Hgfx {

uint8_t mode, g20;
bool rgbOrder, radastan;
uint8_t radBank, radPad, keyColour;
uint16_t radOffset;
const uint8_t* keyTop;
const uint8_t* keyBottom;

static inline bool keyIs(uint8_t c) {
    return c == keyColour || (!(keyColour & 7) && !(c & 7));   // black has no bright
}

static uint8_t* s_buf;              // 128K, PSRAM
static uint8_t* s_col;              // the colour table, PSRAM
static struct Hot {                 // read per pixel: heap
    uint8_t map[256], pidx[256], zxp[16], line[720], line2[720], slots[720];
    uint32_t pal[16];
}* s_hot;
static uint8_t s_cube[168];
static bool s_pair, s_zxPair, s_palValid, s_pairWant2;
static uint8_t s_pairHold;
static uint8_t s_r[REG_COUNT];
static inline int16_t s16(uint8_t r) { return (int16_t)(s_r[r] | (s_r[r + 1] << 8)); }
static bool s_live, s_palDirty, s_palAll;
static uint64_t s_busyUntil;
// The current mode's geometry: pixels, bits per pixel, the second buffer.
static int s_w = WIDTH, s_h = HEIGHT, s_stride = WIDTH, s_bpp = 8;
static uint32_t s_b1 = BUF1;

int vramPages() { return (s_w / 8) * s_h / 256; }

void setMode(uint8_t m) {
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

static inline uint8_t* dstBuf() { return s_buf + ((s_r[REG_VP] & 2) ? s_b1 : 0); }
static inline uint8_t* srcBuf() { return s_buf + ((s_r[REG_VP] & 4) ? s_b1 : 0); }
static inline uint64_t now() { return CPU::global_tstates + CPU::tstates; }

bool ensure() {
    if (!s_hot) s_hot = (Hot*)tryMalloc(sizeof(Hot));
    if (!s_buf) {
        s_buf = (uint8_t*)Buffer::palloc(BUF_SIZE + COLOURS, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (s_buf) { memset(s_buf, 0, BUF_SIZE + COLOURS); s_col = s_buf + BUF_SIZE; }
        else Debug::log("[ELM] no memory for the HGFX buffers");
    }
    return s_buf && s_hot;
}

void reset() {
    const bool was = s_live;
    mode = 0; g20 = 0; rgbOrder = false;
    memset(s_r, 0, sizeof(s_r));
    s_r[REG_MASK] = 0xFF;
    s_busyUntil = 0;
    if (was) { s_live = false; VIDEO::paper_hold = false; VIDEO::applyPalette(); }
}

void loadState(const uint8_t* regs, const uint8_t* colours) {
    if (!s_buf) return;
    memcpy(s_r, regs, REG_COUNT);
    memcpy(s_col, colours, COLOURS);
    s_palDirty = true;
}

static inline bool inWindow(int x, int y) {
    if (!(s_r[REG_VP] & 8)) return true;
    const int wx = s16(REG_WX), wy = s16(REG_WY);
    return x >= wx && y >= wy && x < wx + s16(REG_WW) && y < wy + s16(REG_WH);
}

void writeVram(uint16_t off, uint8_t v) {
    if (!s_buf) return;
    // Bytes per line follow the mode's width (Nether Earth steps 64 in HGFX 2), and
    // only HGFX 1 has the ZX layout.
    const int bpl = s_w / 8;
    const bool linear = (g20 & 1) || s_w != WIDTH;
    const int row = linear ? off / bpl : zxRow(off);
    const int col = (off % bpl) * 8;
    const int y = row + s16(REG_OY);
    if (y < 0 || y >= s_h) return;
    int x = col + s16(REG_OX);
    uint8_t* d = dstBuf() + (uint32_t)y * s_stride;
    const uint8_t m = s_r[REG_MASK], pm = s_r[REG_MODE];
    if (pm == 1) {                       // COPY: the written value does not matter
        const int sy = row + s16(REG_SY);
        if (sy < 0 || sy >= s_h) return;
        int sx = col + s16(REG_SX);
        const uint8_t* s = srcBuf() + (uint32_t)sy * s_stride;
        for (int i = 0; i < 8; i++, x++, sx++) {
            if (x < 0 || x >= s_w || sx < 0 || sx >= s_w || !inWindow(x, y)) continue;
            pxPlanar(d, x, pxGet(s, sx), m);
        }
        return;
    }
    const uint8_t ink = s_r[REG_INDEX], paper = s_r[REG_PAPER];
    for (int i = 0; i < 8; i++, x++, v <<= 1) {
        if (x < 0 || x >= s_w) continue;
        const bool on = v & 0x80;
        if (!on && pm != 2) continue;
        if (!inWindow(x, y)) continue;
        pxPlanar(d, x, on ? ink : paper, m);
    }
}

// The whole-buffer commands: instant here, busy for their documented time.
static void command(uint8_t c) {
    if (!s_buf || (c != 1 && c != 2)) return;
    const uint8_t m = s_r[REG_MASK];
    uint8_t* d = dstBuf();
    const uint32_t n = (uint32_t)s_stride * s_h;
    if (c == 1) {
        const uint8_t* s = srcBuf();
        if (s != d) {
            if (m == 0xFF) memcpy(d, s, n);
            else for (uint32_t i = 0; i < n; i++) d[i] = planar(d[i], s[i], m);
        }
        s_busyUntil = now() + ((uint64_t)6144 << ESPectrum::multiplicator);
    } else {
        if (m == 0xFF) memset(d, 0, n);
        else for (uint32_t i = 0; i < n; i++) d[i] &= (uint8_t)~m;
        s_busyUntil = now() + ((uint64_t)3500 << ESPectrum::multiplicator);
    }
}

void writeReg(uint8_t r, uint8_t v) {
    if (r >= REG_COUNT) return;
    s_r[r] = v;
    if (r == REG_CMD) command(v);
}

uint8_t reg(uint8_t r) { return r < REG_COUNT ? s_r[r] : 0; }

void writeColour(uint16_t off, uint8_t v) {
    if (s_col && off < COLOURS && s_col[off] != v) { s_col[off] = v; s_palDirty = true; }
}

uint8_t status() { return now() < s_busyUntil ? 1 : 0; }

uint8_t* chunkyRead(uint32_t off) {
    return s_buf ? srcBuf() + ((off + (uint32_t)(uint16_t)s16(REG_OY) * s_stride) & (BUF_SIZE - 1)) : nullptr;
}

void chunkyWrite(uint32_t off, uint8_t v) {
    if (!s_buf) return;
    if ((s_r[REG_VP] & 0x10) && !v) return;
    uint8_t* d = dstBuf() + ((off + (uint32_t)(uint16_t)s16(REG_OY) * s_stride) & (BUF_SIZE - 1));
    *d = planar(*d, v, s_r[REG_MASK]);
}

// Hardware slots: 0..15 stay the ZX colours (the layer under index 0), the UI block
// and the HDMI Data Island words are out, the rest takes the table's colours.
static uint8_t kSlots(int i) { return (uint8_t)(i < 136 ? 16 + i : i < 152 ? 168 + i - 136 : 200 + i - 152); }

static void paletteProgram() {
    uint8_t slots[168]; uint32_t cols[168]; int cap = 0, used = 0;
    while (cap < 168) { slots[cap] = kSlots(cap); cap++; }
    for (int i = 0; i < 256; i++) {
        const uint8_t* c = s_col + i * 3;
        const uint32_t rgb = rgbOrder ? (c[0] << 16 | c[1] << 8 | c[2])
                                      : (c[2] << 16 | c[1] << 8 | c[0]);
        int hit = -1;
        for (int k = 0; k < used; k++) if (cols[k] == rgb) { hit = k; break; }
        if (hit < 0 && used < cap) {
            hit = used; cols[used++] = rgb;
            VIDEO::elmPaletteSet(slots[hit], rgb);
        }
        if (hit < 0) {                   // more colours than slots: the nearest
            uint32_t best = ~0u;
            for (int k = 0; k < used; k++) {
                const int dr = (int)(cols[k] >> 16) - (int)(rgb >> 16);
                const int dg = (int)((cols[k] >> 8) & 255) - (int)((rgb >> 8) & 255);
                const int db = (int)(cols[k] & 255) - (int)(rgb & 255);
                const uint32_t d = dr * dr + dg * dg + db * db;
                if (d < best) { best = d; hit = k; }
            }
        }
        s_hot->map[i] = slots[hit];
    }
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
        const uint8_t ink = (a & 7) | br, pap = ((a >> 3) & 7) | br;
        for (int i = 0; i < 8; i++, bits <<= 1) *out++ = (bits & 0x80) ? ink : pap;
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

static inline uint32_t tableRgb(const uint8_t* c) {
    return rgbOrder ? ((uint32_t)c[0] << 16 | c[1] << 8 | c[2]) : ((uint32_t)c[2] << 16 | c[1] << 8 | c[0]);
}
static inline uint32_t dist(uint32_t a, uint32_t b) {
    const int dr = (int)(a >> 16) - (int)(b >> 16), dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255),
              db = (int)(a & 255) - (int)(b & 255);
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

// The pair frame has 16 colours, four of them the ZX 12..15. `hist` picks the other
// twelve, `rgbOf` maps every source index to the nearest.
static void pairPalette(const uint32_t* hist, const uint32_t* rgbOf, int n) {
    uint32_t pal[16], h[256];
    memcpy(h, hist, n * sizeof(uint32_t));
    for (int i = 0; i < 12; i++) {
        int best = 0;
        for (int k = 1; k < n; k++) if (h[k] > h[best]) best = k;
        pal[i] = rgbOf[best];
        h[best] = 0;
    }
    for (int i = 12; i < 16; i++) pal[i] = VIDEO::zxRgb((uint8_t)i);
    s_pair = VIDEO::samPairSync(true, pal);
    memcpy(s_hot->pal, pal, sizeof(pal));
    for (int k = 0; k < n; k++) {
        uint32_t bd = ~0u; int b = 0;
        for (int i = 0; i < 16; i++) { const uint32_t d = dist(pal[i], rgbOf[k]); if (d < bd) { bd = d; b = i; } }
        s_hot->pidx[k] = (uint8_t)b;
    }
}

uint8_t pairBorder(uint8_t zx) {
    if (!s_hot) return zx;
    if (!s_zxPair) return s_hot->zxp[zx & 15];
    return zx & 15;
}

static inline void putCarved(uint8_t* row, int fx, uint8_t v, int nc, int (*cx)[2]) {
    for (int k = 0; k < nc; k++) if (fx >= cx[k][0] && fx < cx[k][1]) return;
    row[fx ^ 2] = v;
}

// One fb row from 2 output px per byte, colours in l[].
static void outPair(int fy, int x0, const uint8_t* l, int bytes) {
    uint8_t* row = VIDEO::vga.frameBuffer[fy];
    int cx[8][2];
    const int nc = VIDEO::rowCarves(fy, cx);
    for (int x = 0; x < bytes; x++)
        putCarved(row, x0 + x, VIDEO::profi_pair_lookup[l[x * 2]][l[x * 2 + 1]], nc, cx);
}
// One fb row of palette slots.
static void outDirect(int fy, int x0, const uint8_t* slots, int n) {
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
static inline uint8_t cubeSlot(uint32_t rgb, int x, int y) {
    static const int8_t bay[4] = { -18, 6, 18, -6 };
    const int t = bay[(x & 1) | ((y & 1) << 1)];
    const auto lvl = [t](int v, int n) { v += t; v = v < 0 ? 0 : v > 255 ? 255 : v; return (v * (n - 1) + 127) / 255; };
    return s_cube[lvl(rgb >> 16, 6) * 28 + lvl((rgb >> 8) & 255, 7) * 4 + lvl(rgb & 255, 4)];
}
static void hamLine(const uint8_t* p, int n, int y, uint8_t* slots) {
    uint32_t c = 0;
    for (int x = 0; x < n; x++) {
        const uint8_t b = p[x], v = (uint8_t)((b & 0x3F) << 2);
        switch (b >> 6) {
        case 0: c = tableRgb(s_col + (b & 0x3F) * 3); break;
        case 1: c = (c & 0xFFFF03) | (v);                  break;
        case 2: c = (c & 0x03FFFF) | ((uint32_t)v << 16);  break;
        case 3: c = (c & 0xFF03FF) | ((uint32_t)v << 8);   break;
        }
        slots[x] = cubeSlot(c, x, y);
    }
}

static void cubeProgram() {
    int k = 0;
    for (int r = 0; r < 6; r++) for (int g = 0; g < 7; g++) for (int b = 0; b < 4; b++, k++) {
        const uint8_t s = kSlots(k);
        s_cube[k] = s;
        VIDEO::elmPaletteSet(s, (uint32_t)(r * 255 / 5) << 16 | (uint32_t)(g * 255 / 6) << 8 | (b * 255 / 3));
    }
}

static bool wantPairFor2() {             // HGFX 2: text or a picture (owner's rule)
    uint32_t hist[256]; memset(hist, 0, sizeof(hist));
    const uint8_t* buf = s_buf + ((s_r[REG_VP] & 1) ? s_b1 : 0);
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
        if (s_pair) for (int y = 0; y < HEIGHT; y++) {
            hrLine(zx0, y, idx, H->line);
            if (keyBottom) {
                hrLine(keyBottom, y, idx, H->line2);
                for (int x = 0; x < 512; x++) if (keyIs(H->line[x])) H->line[x] = H->line2[x];
            }
            if (idx) for (int x = 0; x < 512; x++) H->line[x] = H->pidx[H->line[x]];
            outPair(y0 + y, x0, H->line, WIDTH);
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
    const uint8_t* buf = s_buf + ((s_r[REG_VP] & 1) ? s_b1 : 0);
    const uint8_t feature = s_r[REG_FEATURE];
    const bool under = !(g20 & 4) && s_w <= 512 && s_h == HEIGHT;
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
                for (int x = 0; x < xres; x++) H->slots[x] = H->map[pxGet(p, x + skip)];
                outDirect(r, 0, H->slots, xres);
            }
        }
        return;
    }
    for (int y = 0; y < HEIGHT; y++) {
        const uint8_t* p = buf + (uint32_t)(s_h > HEIGHT ? y * 2 : y) * s_stride;
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
            for (int x = 0; x < WIDTH; x++) H->slots[x] = q[x] ? H->map[q[x]] : H->line[x];
        } else {
            for (int x = 0; x < WIDTH; x++) H->slots[x] = H->map[q[x]];
        }
        outDirect(y0 + y, x0, H->slots, WIDTH);
    }
}

// The palette a frame needs, or a change of output.
static void frameSetup() {
    uint32_t rgb[256], hist[256];
    const bool hr = mode == 1 || mode == 2;
    bool wantPair = hr || mode == 5 || mode == 6 || mode == 7;
    if (mode == 4) {
        if (s_pairHold) s_pairHold--;
        else { wantPair = wantPairFor2(); s_pairWant2 = wantPair; s_pairHold = 25; }
        wantPair = s_pairWant2;
    }
    if (hr && mode == 1) {
        for (int i = 0; i < 256; i++) { rgb[i] = 0; hist[i] = 0; }
        for (int i = 0; i < 12; i++) { rgb[i] = VIDEO::zxRgb((uint8_t)i); hist[i] = (uint32_t)(100 - i); }
        pairPalette(hist, rgb, 12);
        s_zxPair = true;
        return;
    }
    s_zxPair = false;
    if (wantPair) {
        int n = 256;
        memset(hist, 0, sizeof(hist));
        if (mode == 2) {
            n = 32;
            const uint8_t* ct = VIDEO::grmem + 0x3C00;
            for (int i = 0; i < 32; i++) rgb[i] = tableRgb(ct + i * 3);
            const uint8_t* at = VIDEO::grmem + 0x3000;
            for (int i = 0; i < 3072; i += 3) { hist[at[i] & 15]++; hist[16 | (at[i] >> 4)]++; }
        } else {
            n = mode == 4 ? 256 : mode == 5 ? 16 : 4;
            for (int i = 0; i < n; i++) { rgb[i] = tableRgb(s_col + i * 3); hist[i] = (uint32_t)(n - i); }
            if (mode == 4) {
                const uint8_t* buf = s_buf + ((s_r[REG_VP] & 1) ? s_b1 : 0);
                memset(hist, 0, sizeof(hist));
                for (int y = 0; y < s_h; y += 8) for (int x = 0; x < s_w; x += 3) hist[buf[(uint32_t)y * s_stride + x]]++;
            }
        }
        pairPalette(hist, rgb, n);
        if (s_pair) { for (int i = 0; i < 16; i++) {
            uint32_t bd = ~0u; const uint32_t z = VIDEO::zxRgb((uint8_t)i);
            for (int k = 0; k < 16; k++) { const uint32_t d = dist(s_hot->pal[k], z); if (d < bd) { bd = d; s_hot->zxp[i] = (uint8_t)k; } }
        } return; }
    }
    if (s_pair) { s_pair = false; VIDEO::samPairSync(false, nullptr); s_palDirty = true; }
    if (radastan && !mode) {
        for (int i = 0; i < 16; i++) {
            const uint8_t g = VIDEO::ulaplus_palette[radBank * 16 + i];
            const uint32_t r3 = (g >> 2) & 7, g3 = g >> 5, b2 = g & 3;
            VIDEO::elmPaletteSet((uint8_t)(16 + i), (r3 * 255 / 7) << 16 | (g3 * 255 / 7) << 8 | (b2 * 255 / 3));
        }
        return;
    }
    if (s_palDirty || !s_palValid) {
        s_palDirty = false; s_palValid = true;
        if (s_r[REG_FEATURE] == 2) cubeProgram(); else if (mode) paletteProgram();
    }
}

void frameEnd() {
    const bool want = (mode >= 1 || keyBottom || radastan) && ensure();
    if (want != s_live) {
        s_live = want;
        VIDEO::paper_hold = want;
        s_palDirty = true; s_palValid = false;
        if (!want) {
            if (s_pair) { s_pair = false; VIDEO::samPairSync(false, nullptr); }
            VIDEO::applyPalette();
            VIDEO::brdChange = VIDEO::brdnextframe = true;
        }
        Debug::log("[ELM] extra video %s (mode %u key %d rad %d)", want ? "on" : "off", mode, keyBottom != nullptr, (int)radastan);
    }
    if (!s_live) return;
    const bool was = s_pair;
    frameSetup();
    if (was != s_pair) { VIDEO::updateBorderBrd(); VIDEO::brdChange = VIDEO::brdnextframe = true; }
    renderFrame();
}

void repaint() { if (s_live) { frameSetup(); renderFrame(); } }
void paletteLost() { s_palDirty = true; }

}
