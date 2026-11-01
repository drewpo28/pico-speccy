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
bool rgbOrder;

static uint8_t* s_buf;              // 128K, PSRAM
static uint8_t* s_col;              // the colour table, PSRAM
static struct Hot { uint8_t map[256], zx[WIDTH], px[WIDTH]; }* s_hot;   // read per pixel: heap
static uint8_t s_r[REG_COUNT];
static bool s_live, s_palDirty, s_palAll;
static uint64_t s_busyUntil;
// The current mode's geometry. HGFX 1 has two buffers, HGFX 2 one 512-wide.
static int s_w = WIDTH, s_h = HEIGHT, s_stride = WIDTH;
static uint32_t s_b1 = BUF1;

int vramPages() { return (s_w / 8) * s_h / 256; }

void setMode(uint8_t m) {
    mode = m;
    const bool wide = m == 4;
    s_w = s_stride = wide ? 512 : WIDTH;
    s_h = HEIGHT;
    s_b1 = wide ? 0 : BUF1;
}

static inline int16_t s16(uint8_t r) { return (int16_t)(s_r[r] | (s_r[r + 1] << 8)); }
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
    const uint8_t row = linear ? (uint8_t)(off / bpl) : zxRow(off);
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
            d[x] = planar(d[x], s[sx], m);
        }
        return;
    }
    const uint8_t ink = s_r[REG_INDEX], paper = s_r[REG_PAPER];
    for (int i = 0; i < 8; i++, x++, v <<= 1) {
        if (x < 0 || x >= s_w) continue;
        const bool on = v & 0x80;
        if (!on && pm != 2) continue;
        if (!inWindow(x, y)) continue;
        d[x] = planar(d[x], on ? ink : paper, m);
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
static void paletteProgram() {
    static const uint16_t kRange[][2] = { { 16, 152 }, { 168, 184 }, { 200, 216 } };
    uint8_t slots[168]; uint32_t cols[168]; int cap = 0, used = 0;
    for (auto& r : kRange) for (int s = r[0]; s < r[1]; s++) slots[cap++] = (uint8_t)s;
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

// The ZX layer of one row as palette slots (index 0 shows it).
static void zxLine(int y, uint8_t* out) {
    const uint8_t* scr = VIDEO::grmem;
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

static void renderFrame() {
    const uint8_t* buf = s_buf + ((s_r[REG_VP] & 1) ? s_b1 : 0);
    const bool wide = s_w > WIDTH;
    const bool under = !(g20 & 4);
    const int x0 = ((int)VIDEO::vga.xres - WIDTH) / 2, y0 = VIDEO::screenTopRows();
    const uint8_t* map = s_hot->map;
    for (int y = 0; y < HEIGHT; y++) {
        const int fy = y0 + y;
        uint8_t* row = VIDEO::vga.frameBuffer[fy];
        const uint8_t* p = buf + (uint32_t)y * s_stride;
        if (wide) {                      // 512 px on 256: the left pixel, or the right over 0
            for (int x = 0; x < WIDTH; x++)
                s_hot->px[x] = p[x * 2] ? p[x * 2] : p[x * 2 + 1];
            p = s_hot->px;
        }
        if (under) zxLine(y, s_hot->zx);
        int cx[8][2];
        const int nc = VIDEO::rowCarves(fy, cx);
        if (nc) {
            for (int x = 0; x < WIDTH; x++) {
                const int fx = x0 + x;
                bool cut = false;
                for (int k = 0; k < nc; k++) if (fx >= cx[k][0] && fx < cx[k][1]) cut = true;
                if (!cut) row[fx ^ 2] = (p[x] || !under) ? map[p[x]] : s_hot->zx[x];
            }
            continue;
        }
        uint32_t* d = (uint32_t*)(row + x0);
        if (under) {
            const uint8_t* z = s_hot->zx;
            for (int x = 0; x < WIDTH; x += 4) {
                const uint8_t a = p[x] ? map[p[x]] : z[x], b = p[x + 1] ? map[p[x + 1]] : z[x + 1];
                const uint8_t c = p[x + 2] ? map[p[x + 2]] : z[x + 2], e = p[x + 3] ? map[p[x + 3]] : z[x + 3];
                *d++ = c | (uint32_t)e << 8 | (uint32_t)a << 16 | (uint32_t)b << 24;
            }
        } else {
            for (int x = 0; x < WIDTH; x += 4)
                *d++ = map[p[x + 2]] | (uint32_t)map[p[x + 3]] << 8 |
                       (uint32_t)map[p[x]] << 16 | (uint32_t)map[p[x + 1]] << 24;
        }
    }
}

void frameEnd() {
    const bool want = (mode == 3 || mode == 4) && s_buf && s_hot;
    if (want != s_live) {
        s_live = want;
        VIDEO::paper_hold = want;
        s_palDirty = true;
        if (!want) VIDEO::applyPalette();
        Debug::log("[ELM] HGFX %s (mode %u)", want ? "on" : "off", mode);
    }
    if (!s_live) return;
    if (s_palDirty) { s_palDirty = false; paletteProgram(); }
    renderFrame();
}

void repaint() { if (s_live) { if (s_palDirty) { s_palDirty = false; paletteProgram(); } renderFrame(); } }
void paletteLost() { s_palDirty = true; }

}
