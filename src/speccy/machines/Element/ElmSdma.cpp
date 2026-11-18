#include "ElmSdma.h"
#include <stdlib.h>
#include <string.h>
#if __has_include("pico.h")
#include "app/TryAlloc.h"
#include "ElmTail.h"
#define stateAlloc Elm::tailAlloc   // session-lifetime, per sample: the pages57 tail first
#else
#define stateAlloc malloc           // the host tools (tools/elm_*_test.cpp)
#endif

namespace ElmSdma {

namespace {

constexpr uint64_t CLOCK_NORMAL = 7167969;     // 7.16796875 MHz
constexpr uint64_t CLOCK_EXT    = 114687500;   // 114.6875 MHz
// 24-bit addresses into the protected URAM; the page callback wraps at the pool. It was
// 512K until 2026-11-14: PF2026 streams through a 64K buffer at #0F0000 (pages #78-#7F)
// and the mask played pages #38-#3F instead - noise in place of the music.
constexpr uint32_t ADDR_MASK    = 0xFFFFFF;
constexpr int      BIAS         = 128;

struct Ch {
    uint32_t pos, end, frac, step;
    uint32_t page;
    const uint8_t* mem;
    uint8_t on;
};

struct State {
    uint8_t reg[REG_BYTES];
    Ch ch[CHANNELS];
    int n, bias, rate;
    bool audible;
    int16_t l[MAX_SAMPLES], r[MAX_SAMPLES];
};

State* s;
PageFn s_page;

inline uint32_t reg24(const uint8_t* r, int o) {
    return r[o] | (uint32_t)r[o + 1] << 8 | (uint32_t)r[o + 2] << 16;
}

void stepRecalc(int c) {
    const uint8_t* r = s->reg + c * 16;
    const bool ext = r[0] & P_EXT;
    const uint32_t period = ext ? reg24(r, 0x0C) : (uint32_t)(r[0x0C] | r[0x0D] << 8);
    s->ch[c].step = period ? (uint32_t)(((ext ? CLOCK_EXT : CLOCK_NORMAL) << 16) / ((uint64_t)period * s->rate)) : 0;
}

inline uint8_t byteAt(Ch& c, uint32_t a) {
    a &= ADDR_MASK;
    const uint32_t pg = a >> 13;
    if (pg != c.page) { c.page = pg; c.mem = s_page(pg); }
    return c.mem ? c.mem[a & 0x1FFF] : 0;
}

void trigger(int c, uint8_t par) {
    Ch& h = s->ch[c];
    const uint8_t* r = s->reg + c * 16;
    if (!(par & P_PLAY)) { h.on = 0; return; }
    stepRecalc(c);
    if (par & P_CONT) { h.on = 1; return; }
    h.pos = reg24(r, 3) & ADDR_MASK;
    h.end = h.pos + reg24(r, 9);
    h.frac = 0;
    h.on = 1;
}

// Wraps to the loop; false = the sample has ended.
bool wrap(int c, uint8_t par) {
    Ch& h = s->ch[c];
    if (!(par & P_LOOP)) { h.on = 0; return false; }
    const uint8_t* r = s->reg + c * 16;
    const uint32_t loop = reg24(r, 6) & ADDR_MASK;
    if (par & P_LATE) h.end = loop + reg24(r, 9);
    if (loop >= h.end) { h.on = 0; return false; }
    h.pos = loop;
    return true;
}

void renderCh(int c, int from, int to, int32_t* l, int32_t* r) {
    Ch& h = s->ch[c];
    const uint8_t* rg = s->reg + c * 16;
    const uint8_t par = rg[0];
    const bool stereo = par & P_STEREO, wide = c == 0 && (par & P_16BIT);
    const uint32_t bytes = wide ? 2 : stereo ? 2 : 1;
    const int8_t flip = (par & P_SIGNED) ? 0 : (int8_t)0x80;
    const int vl = rg[1], vr = rg[2];
    for (int i = from; i < to; i++) {
        if (h.pos >= h.end && !wrap(c, par)) return;
        if (wide) {
            int v = (int16_t)(byteAt(h, h.pos) | byteAt(h, h.pos + 1) << 8);
            if (!(par & P_SIGNED)) v -= 0x8000;
            l[i - from] += v * 4; r[i - from] += v * 4;   // always at full volume
        } else if (stereo) {
            l[i - from] += ((int8_t)(byteAt(h, h.pos) ^ flip)) * vl * 4;
            r[i - from] += ((int8_t)(byteAt(h, h.pos + 1) ^ flip)) * vr * 4;
        } else {
            const int v = (int8_t)(byteAt(h, h.pos) ^ flip);
            l[i - from] += v * vl * 4;
            r[i - from] += v * vr * 4;
        }
        h.frac += h.step;
        h.pos += (h.frac >> 16) * bytes;
        h.frac &= 0xFFFF;
    }
}

} // namespace

bool init(PageFn fn, int outputRate) {
    s_page = fn;
    if (!s) s = (State*)stateAlloc(sizeof(State));
    if (!s) return false;
    memset(s, 0, sizeof(State));
    s->rate = outputRate;
    reset();
    return true;
}

void reset() {
    if (!s) return;
    const int rate = s->rate;
    memset(s, 0, sizeof(State));
    s->rate = rate;
    for (int c = 0; c < CHANNELS; c++) s->ch[c].page = ~0u;
}

void setRate(int outputRate) {
    if (!s || s->rate == outputRate) return;
    s->rate = outputRate;
    for (int c = 0; c < CHANNELS; c++) stepRecalc(c);
}

void write(uint8_t off, uint8_t v) {
    if (!s || off >= REG_BYTES) return;
    const int c = off >> 4;
    const uint8_t o = off & 15;
    s->reg[off] = v;
    Ch& h = s->ch[c];
    if (o == 0) trigger(c, v);
    else if (o >= 0x0C) stepRecalc(c);
    else if (o >= 9 && o <= 0x0B && h.on && !(s->reg[c * 16] & P_LATE))
        h.end = (reg24(s->reg + c * 16, 3) & ADDR_MASK) + reg24(s->reg + c * 16, 9);
}

void renderTo(int sample) {
    if (!s) return;
    if (sample > MAX_SAMPLES) sample = MAX_SAMPLES;
    if (sample <= s->n) return;
    const int from = s->n, cnt = sample - from;
    int32_t l[64], r[64];
    for (int done = 0; done < cnt; ) {
        const int part = cnt - done > 64 ? 64 : cnt - done;
        memset(l, 0, part * sizeof(int32_t));
        memset(r, 0, part * sizeof(int32_t));
        bool any = false;
        for (int c = 0; c < CHANNELS; c++)
            if (s->ch[c].on) { renderCh(c, 0, part, l, r); any = true; }
        if (any) s->audible = true;
        for (int i = 0; i < part; i++) {
            if (any && s->bias < BIAS) s->bias++;
            else if (!any && s->bias > 0) s->bias--;
            int a = l[i] >> 10, b = r[i] >> 10;   // volume 64 (0 dB) = +-32, 255 = +-128
            if (a > 255) a = 255; else if (a < -255) a = -255;
            if (b > 255) b = 255; else if (b < -255) b = -255;
            s->l[from + done + i] = (int16_t)(s->bias + a);
            s->r[from + done + i] = (int16_t)(s->bias + b);
        }
        done += part;
    }
    s->n = sample;
}

void frameEnd(int samples) {
    if (!s) return;
    renderTo(samples);
    s->n = 0;
}

uint8_t posHigh() { return s ? (uint8_t)(s->ch[0].pos >> 8) : 0; }

bool active() {
    if (!s) return false;
    if (s->bias) return true;
    for (int c = 0; c < CHANNELS; c++) if (s->ch[c].on) return true;
    return false;
}

const int16_t* mixL() { return active() ? s->l : nullptr; }
const int16_t* mixR() { return active() ? s->r : nullptr; }

}
