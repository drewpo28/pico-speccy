// pico-speccy — Enterprise Nick + Dave core. See EpCore.h for the model and its
// sources. Flash code; no firmware dependencies (the host simulator links it too).

#include "EpCore.h"
#include <string.h>

namespace EpCore {

// ---------------------------------------------------------------------------
// Pixel decoding (ep128emu NickTables, computed instead of tabled: no RAM on the
// machines that do not run it). 4 colours: pixel k = bits (3-k, 7-k); 16 colours:
// pixel 0 = bits 1,5,3,7 and pixel 1 = bits 0,4,2,6 (value bits 3..0).

static __attribute__((always_inline)) inline uint8_t four(uint8_t b, int k) {
    return (uint8_t)((((b >> (3 - k)) & 1) << 1) | ((b >> (7 - k)) & 1));
}
static __attribute__((always_inline)) inline uint8_t sixteen(uint8_t b, int k) {
    const uint8_t x = (uint8_t)(b >> (k ^ 1));          // pixel 0 reads the odd bits
    return (uint8_t)(((x & 1) << 3) | (((x >> 4) & 1) << 2) | (((x >> 2) & 1) << 1) | ((x >> 6) & 1));
}

// Word stores into the pixel buffers (every slot output is 4-aligned: `out` and
// the hi buffer are aligned and the steps are 8 / 16). memcpy of 4 bytes is one str.
static __attribute__((always_inline)) inline void st32(uint8_t* p, uint32_t w) { __builtin_memcpy(p, &w, 4); }
static __attribute__((always_inline)) inline uint32_t ld32(const uint8_t* p) { uint32_t w; __builtin_memcpy(&w, p, 4); return w; }
static __attribute__((always_inline)) inline uint32_t x4(uint8_t c) { return (uint32_t)c * 0x01010101u; }
// The framebuffer's word order: pixels 0,1,2,3 are stored as 2,3,0,1 — a 16-bit
// rotate of the linear word. Its own inverse.
static __attribute__((always_inline)) inline uint32_t x2w(uint32_t w) { return (w >> 16) | (w << 16); }
// Nibble n (bit 3 = the first pixel) -> a byte mask per pixel: byte k = 0xFF when
// bit (3-k) is set. n * 0x08040201 puts n at bit 0, 9, 18, 27; shifted by 3 the
// four bits of n land one per byte.
static __attribute__((always_inline)) inline uint32_t nibMask(uint32_t n) {
    return ((((n & 15u) * 0x08040201u) >> 3) & 0x01010101u) * 0xFFu;
}
// 4 pixels of two colours from a nibble, MSB first: linear (hi buffer) and fb order.
static __attribute__((always_inline)) inline uint32_t px4w(uint32_t n, uint8_t c0, uint8_t c1) {
    return x4(c0) ^ (x4((uint8_t)(c0 ^ c1)) & nibMask(n));
}
static __attribute__((always_inline)) inline uint32_t px4wx(uint32_t n, uint8_t c0, uint8_t c1) {
    return x4(c0) ^ (x4((uint8_t)(c0 ^ c1)) & x2w(nibMask(n)));
}
// 4 pixels a, b, c, d (linear) as an fb-order word; two doubled pixels a, a, b, b.
static __attribute__((always_inline)) inline uint32_t w4x(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)c | (uint32_t)d << 8 | (uint32_t)a << 16 | (uint32_t)b << 24;
}
static __attribute__((always_inline)) inline uint32_t w2x(uint8_t a, uint8_t b) {
    return (uint32_t)b * 0x0101u | (uint32_t)a * 0x01010000u;
}
static __attribute__((always_inline)) inline void fill8(uint8_t* o, uint8_t c)  { const uint32_t w = x4(c); st32(o, w); st32(o + 4, w); }
static __attribute__((always_inline)) inline void fill16(uint8_t* o, uint8_t c) { const uint32_t w = x4(c); st32(o, w); st32(o + 4, w); st32(o + 8, w); st32(o + 12, w); }
// 8 lores pixels (fb order) -> 16 hi-res ones (linear, each doubled).
static __attribute__((always_inline)) inline uint32_t dup2(uint32_t w) {
    return (w & 0xFFu) * 0x0101u | ((w >> 8) & 0xFFu) * 0x01010000u;
}
static __attribute__((always_inline)) inline void dup8to16(uint8_t* hi, const uint8_t* o) {
    const uint32_t w0 = x2w(ld32(o)), w1 = x2w(ld32(o + 4));
    st32(hi, dup2(w0)); st32(hi + 4, dup2(w0 >> 16)); st32(hi + 8, dup2(w1)); st32(hi + 12, dup2(w1 >> 16));
}
static const uint8_t kIdent[256] = {
#define I16(b) b, b+1, b+2, b+3, b+4, b+5, b+6, b+7, b+8, b+9, b+10, b+11, b+12, b+13, b+14, b+15
    I16(0), I16(16), I16(32), I16(48), I16(64), I16(80), I16(96), I16(112),
    I16(128), I16(144), I16(160), I16(176), I16(192), I16(208), I16(224), I16(240)
#undef I16
};

uint32_t colourRgb(uint8_t c) {
    const uint32_t r = ((c << 2) & 4) | ((c >> 2) & 2) | ((c >> 6) & 1);
    const uint32_t g = ((c << 1) & 4) | ((c >> 3) & 2) | ((c >> 7) & 1);
    const uint32_t b = ((c >> 1) & 2) | ((c >> 5) & 1);
    return ((r * 255 + 3) / 7) << 16 | ((g * 255 + 3) / 7) << 8 | ((b * 255 + 1) / 3);
}

// ---------------------------------------------------------------------------
// Nick

Nick::Nick() : outMap(kIdent) {}

void Nick::setOutMap(const uint8_t* m) {
    outMap = m ? m : kIdent;
    for (int i = 0; i < 16; i++) palO[i] = outMap[pal[i]];
}

void Nick::reset() {
    nLines = 1;
    irqFlag = false; vres = false; reload = true;
    colorMode = 0; videoMode = 7;
    altInd0 = altInd1 = lsbAlt = msbAlt = false;
    leftMargin = 8; rightMargin = 54;
    dataBus = 0xFF;
    memset(pal, 0, sizeof(pal));
    for (int i = 0; i < 16; i++) palO[i] = outMap[0];
    ld1 = ld2 = 0;
    lptBase = lptCur = 0;
    linesRemaining = 0;
    displayEnabled = false;
    vsync = false;
    lptFlags = 0;
    port3 = 0xF0;
    writePort(0, 0); writePort(1, 0); writePort(2, 0); writePort(3, 0xF0);
}

void Nick::writePort(uint8_t port, uint8_t value) {
    dataBus = value;
    switch (port & 3) {
    case 0: {
        port0 = value;
        const uint8_t fb = (uint8_t)((value & 0x1F) << 3);
        for (int i = 0; i < 8; i++) { pal[8 + i] = (uint8_t)(fb | i); palO[8 + i] = outMap[fb | i]; }
        break;
    }
    case 1: border = value; break;
    case 2: lptBase = (uint16_t)((lptBase & 0xF000) | ((uint16_t)value << 4)); break;
    case 3: {
        lptBase = (uint16_t)((lptBase & 0x0FF0) | ((uint16_t)(value & 0x0F) << 12));
        const uint8_t changed = (uint8_t)((value ^ port3) & 0xC0);
        port3 = value;
        if (changed) {
            if (!(value & 0x40)) lptFlags = 0;
            else {
                if (changed & 0x40) lptFlags |= 0x80;
                if (!(value & 0x80)) lptFlags |= 0x40;
            }
        }
        break;
    }
    }
}

void Nick::setIrq(bool s) {
    if (s == irqFlag) return;
    irqFlag = s;
    if (irqCb) irqCb(ud, s);
}

// 8 pixels of a 2-colour byte (one pixel a bit, MSB first): linear (hi) / fb order (o).
static __attribute__((always_inline)) inline void px2(uint8_t* o, uint8_t b, uint8_t c0, uint8_t c1) {
    st32(o, px4w(b >> 4, c0, c1)); st32(o + 4, px4w(b, c0, c1));
}
static __attribute__((always_inline)) inline void px2x(uint8_t* o, uint8_t b, uint8_t c0, uint8_t c1) {
    st32(o, px4wx(b >> 4, c0, c1)); st32(o + 4, px4wx(b, c0, c1));
}

// One display slot (8..53): ep128emu Nick::render_Generic, 8 lores pixels out in
// fb order, colours through palO / outMap. VM = the video mode (fixed for a run of
// slots, so the dispatch is per run); returns true for a 2-colour PIXEL slot whose
// 16 px differ from its 8.
template <int VM>
__attribute__((always_inline)) inline bool Nick::slotT(uint8_t* o, uint8_t* hi) {
    bool wroteHi = false;
    if (VM == 1) {                                   // PIXEL: two bytes a slot
        uint8_t b[2], m[2];
        for (int i = 0; i < 2; i++) {
            uint8_t x = v(ld1);
            if (i == 0) ld1++;
            uint8_t mk = 0;
            if (altInd0) mk = (uint8_t)((x & 0x40) >> 4);
            if (altInd1 && (x & 0x80)) mk |= 0x02;
            if (lsbAlt && (x & 0x01)) { x &= 0xFE; mk |= 0x04; }
            if (msbAlt && (x & 0x80)) { x &= 0x7F; mk |= 0x02; }
            b[i] = x; m[i] = mk;
        }
        dataBus = b[1];
        switch (colorMode) {
        case 0:                                      // 16 hi-res px: the left of each pair
            for (int i = 0; i < 2; i++) {
                const uint8_t c0 = palO[m[i]], c1 = palO[1 | m[i]];
                // bits 7,5,3,1 = the left pixel of each pair, as a nibble
                const uint32_t odd = ((b[i] >> 4) & 8) | ((b[i] >> 3) & 4) | ((b[i] >> 2) & 2) | ((b[i] >> 1) & 1);
                st32(o + i * 4, px4wx(odd, c0, c1));
                if (hi) px2(hi + i * 8, b[i], c0, c1);
            }
            // The line needs 16 px only if some pixel differs from its neighbour
            // (bits 7/6, 5/4, 3/2, 1/0 of a byte) in two different colours: else
            // the 8-px picture is the same. EXOS's blank lines are PIXEL too.
            for (int i = 0; i < 2 && !wroteHi; i++)
                wroteHi = ((b[i] ^ (b[i] << 1)) & 0xAA) && pal[m[i]] != pal[1 | m[i]];
            break;
        case 1:
            for (int i = 0; i < 2; i++)
                st32(o + i * 4, w4x(palO[four(b[i], 0) | m[i]], palO[four(b[i], 1) | m[i]],
                                    palO[four(b[i], 2) | m[i]], palO[four(b[i], 3) | m[i]]));
            break;
        case 2:
            for (int i = 0; i < 2; i++)
                st32(o + i * 4, w2x(palO[sixteen(b[i], 0) | m[i]], palO[sixteen(b[i], 1) | m[i]]));
            break;
        default:
            st32(o, x4(outMap[b[0]])); st32(o + 4, x4(outMap[b[1]]));
            break;
        }
    } else if (VM == 2) {                            // ATTRIBUTE
        const uint8_t a = v(ld1);
        uint8_t b = v(ld2);
        ld2++;
        dataBus = b;
        if (lsbAlt) b &= 0xFE;
        if (msbAlt) b &= 0x7F;
        const uint8_t c0 = palO[a >> 4], c1 = palO[a & 15];
        switch (colorMode) {
        case 0: px2x(o, b, c0, c1); break;
        case 1:
            st32(o, w2x((b & 0x80) ? c1 : c0, (b & 0x40) ? c1 : c0));
            st32(o + 4, w2x((b & 0x20) ? c1 : c0, (b & 0x10) ? c1 : c0));
            break;
        case 2: st32(o, x4(((b >> 7) & 1) ? c1 : c0)); st32(o + 4, x4(((b >> 6) & 1) ? c1 : c0)); break;
        default: fill8(o, outMap[b]); break;
        }
    } else if (VM >= 3) {                            // CH256 / CH128 / CH64 / invalid / LPIXEL
        uint8_t c = v(ld1), b;
        if (VM == 7) b = c;
        else {
            uint16_t a = 0xFFFF;
            if (VM == 3) a = (uint16_t)((ld2 << 8) | c);
            else if (VM == 4) a = (uint16_t)((ld2 << 7) | (c & 0x7F));
            else if (VM == 5) a = (uint16_t)((ld2 << 6) | (c & 0x3F));
            b = v(a);
        }
        dataBus = b;
        uint8_t mk = 0;
        if (altInd0) mk = (uint8_t)((c & 0x40) >> 4);
        if (altInd1 && (c & 0x80)) mk |= 0x02;
        if (lsbAlt && (b & 0x01)) { b &= 0xFE; mk |= 0x04; }
        if (msbAlt && (b & 0x80)) { b &= 0x7F; mk |= 0x02; }
        switch (colorMode) {
        case 0: px2x(o, b, palO[0 | mk], palO[1 | mk]); break;
        case 1:
            st32(o, w2x(palO[four(b, 0) | mk], palO[four(b, 1) | mk]));
            st32(o + 4, w2x(palO[four(b, 2) | mk], palO[four(b, 3) | mk]));
            break;
        case 2: st32(o, x4(palO[sixteen(b, 0) | mk])); st32(o + 4, x4(palO[sixteen(b, 1) | mk])); break;
        default: fill8(o, outMap[b]); break;
        }
    } else {                                         // VSYNC
        dataBus = v(ld1);
        fill8(o, outMap[0]);
    }
    ld1++;
    return wroteHi;
}

// n display slots in mode VM: o advances by `ostep` (0 = a scratch slot, the
// line is run without pixels), hi (if set) by 16. Returns L_HIRES or 0.
template <int VM>
__attribute__((always_inline)) inline int Nick::spanT(int n, uint8_t* o, int ostep, uint8_t* hi) {
    int flags = 0;
    for (; n > 0; n--, o += ostep) {
        if (slotT<VM>(o, hi)) flags = L_HIRES;
        else if (hi) dup8to16(hi, o);
        if (hi) hi += 16;
    }
    return flags;
}

// A run of display slots: the border when display is off, else the mode's span.
int Nick::span(int n, uint8_t* o, int ostep, uint8_t* hi) {
    if (n <= 0) return 0;
    if (!displayEnabled) {
        const uint8_t c = outMap[videoMode == 0 ? 0 : border];
        for (int k = 0; k < n; k++, o += ostep) {
            fill8(o, c);
            if (hi) { fill16(hi, c); hi += 16; }
        }
        return 0;
    }
    switch (videoMode) {
    case 0:  return spanT<0>(n, o, ostep, hi);
    case 1:  return spanT<1>(n, o, ostep, hi);
    case 2:  return spanT<2>(n, o, ostep, hi);
    case 3:  return spanT<3>(n, o, ostep, hi);
    case 4:  return spanT<4>(n, o, ostep, hi);
    case 5:  return spanT<5>(n, o, ostep, hi);
    case 6:  return spanT<6>(n, o, ostep, hi);
    default: return spanT<7>(n, o, ostep, hi);
    }
}

int Nick::line(uint8_t* out, uint8_t* aux, int visLo, int visHi) {
    alignas(4) uint8_t scratch[8];
    int flags = 0;
    for (int s = 0; s < SLOTS; s++) {
        if (s == rightMargin) {
            displayEnabled = false;
            vsync = false;
        } else if (s == leftMargin) {
            displayEnabled = true;
            vsync = (videoMode == 0);
        }
        if (s >= FIRST && s < LAST) {
            // The display slots in runs up to the next margin event (the margin
            // test above runs at the start of each run, as it did per slot), split
            // again where the visible range of `out` begins and ends.
            int e = LAST;
            if (rightMargin > s && rightMargin < e) e = rightMargin;
            if (leftMargin > s && leftMargin < e) e = leftMargin;
            if (visLo > s && visLo < e) e = visLo;
            if (visHi > s && visHi < e) e = visHi;
            const bool vis = s >= visLo && s < visHi;
            uint8_t* dst = vis ? out : aux;
            uint8_t* o = dst ? dst + (s - FIRST) * 8 : scratch;
            uint8_t* hi = hiOut ? hiOut + (s - FIRST) * 16 : nullptr;
            flags |= span(e - s, o, dst ? 8 : 0, hi);
            s = e - 1;
            continue;
        }
        const uint16_t p = lptCur;
        switch (s) {
        case 0: {
            nLines = 256 - (int)v(p + 0);
            const uint8_t mb = v(p + 1);
            dataBus = mb;
            setIrq((mb & 0x80) != 0);
            colorMode = (mb >> 5) & 3;
            vres = (mb & 0x10) != 0;
            videoMode = (mb >> 1) & 7;
            reload = (mb & 0x01) != 0;
            if (vsync && videoMode != 0) vsync = false;
            if (videoMode == 0) flags |= L_VSYNC;
            break;
        }
        case 1: {
            const uint8_t lm = v(p + 2), rm = v(p + 3);
            lsbAlt = (lm & 0x40) != 0; msbAlt = (lm & 0x80) != 0; leftMargin = lm & 0x3F;
            altInd1 = (rm & 0x40) != 0; altInd0 = (rm & 0x80) != 0; rightMargin = rm & 0x3F;
            dataBus = rm;
            break;
        }
        case 2:
            dataBus = v(p + 5);
            if (linesRemaining <= 0 || !vres) ld1 = (uint16_t)(v(p + 4) | (dataBus << 8));
            break;
        case 3: {
            const uint16_t base2 = (uint16_t)(v(p + 6) | (v(p + 7) << 8));
            dataBus = v(p + 7);
            if (videoMode >= 3 && videoMode <= 5) ld2++;
            if (linesRemaining <= 0) { ld2 = base2; linesRemaining = nLines; }
            break;
        }
        case 4: case 5: case 6: case 7: {
            const int i = (s - 4) * 2;
            pal[i] = v(p + 8 + i); pal[i + 1] = v(p + 9 + i);
            palO[i] = outMap[pal[i]]; palO[i + 1] = outMap[pal[i + 1]];
            dataBus = pal[i + 1];
            break;
        }
        case 56: {
            linesRemaining--;
            if (linesRemaining == 0 || (lptFlags & 0x80)) {
                if (port3 & 0x40) {
                    if (!((reload ? 1 : 0) | (lptFlags & 0x40))) lptCur = (uint16_t)((lptCur + 0x10) & 0xFFF0);
                    else lptCur = lptBase;
                }
            }
            lptFlags = (uint8_t)(((int)port3 & ((~(int)port3) >> 1)) & 0x40);
            break;
        }
        default: break;
        }
        if (displayEnabled) {
            ld1 = (uint16_t)(ld1 + (videoMode == 1 ? 2 : 1));
            if (videoMode == 2) ld2++;
        }
    }
    return flags;
}

// ---------------------------------------------------------------------------
// Dave

static inline void polyInit(Dave::Poly& p, uint32_t m0, uint32_t m1) {
    p.m0 = m0; p.m1 = m1; p.mask = (m0 << 1) - 1; p.sr = p.mask; p.out = 0;
}
static __attribute__((always_inline)) inline uint8_t polyStep(Dave::Poly& p) {
    p.out = (p.sr & p.m0) ? 1 : 0;
    const uint8_t b1 = (p.sr & p.m1) ? 1 : 0;
    p.sr = ((p.sr << 1) | (uint32_t)(p.out ^ b1)) & p.mask;
    return p.out;
}
// #A6 bits 2-3: 17 / 15 / 11 / 9-bit variable-length counter (ep128emu polycnt_params).
static const uint32_t kVl[4][2] = {{0x10000, 0x2000}, {0x4000, 0x2000}, {0x400, 0x100}, {0x100, 0x10}};
static void polyVl(Dave::Poly& p, int sel) {
    if (p.m0 == kVl[sel][0]) return;
    const uint32_t sr = p.sr;
    p.m0 = kVl[sel][0]; p.m1 = kVl[sel][1]; p.mask = (p.m0 << 1) - 1;
    p.sr = sr & p.mask;
    if (!p.sr) p.sr = p.mask;                  // an all-zero LFSR would stay silent
}

// The fixed counters' output sequences, step by step from the reset state (the
// LFSRs above with all ones loaded): 4-bit period 15, 5-bit 31, 7-bit 127.
static uint8_t s_tab4[15], s_tab5[31], s_tab7[127];
static bool s_tabsOk;
static void buildTabs() {
    Dave::Poly p;
    polyInit(p, 0x8, 0x4);   for (int i = 0; i < 15; i++)  s_tab4[i] = polyStep(p);
    polyInit(p, 0x10, 0x4);  for (int i = 0; i < 31; i++)  s_tab5[i] = polyStep(p);
    polyInit(p, 0x40, 0x20); for (int i = 0; i < 127; i++) s_tab7[i] = polyStep(p);
    s_tabsOk = true;
}
static __attribute__((always_inline)) inline uint8_t tabStep(Dave::Tab& t, const uint8_t* tab, uint8_t period) {
    t.out = tab[t.ph];
    if (++t.ph == period) t.ph = 0;
    return t.out;
}

// Apply the free-running steps owed to the variable-length counter (its period
// is 2^n - 1, so only the remainder matters).
void Dave::pvlSync() {
    uint32_t n = pvlPend;
    pvlPend = 0;
    const uint32_t period = pvl.mask;          // 2^n - 1
    if (n > period) { n %= period; if (!n) n = period; }   // same state, and `out` the last step's
    while (n--) polyStep(pvl);
}

void Dave::reset(bool cold) {
    clk1000 = clk50 = clk1 = 0;
    clk62Phase = 0; clk62 = 0;
    for (int i = 0; i < 3; i++) { chPhase[i] = 0; chFrq[i] = 0; chRun[i] = 1; }
    for (int i = 0; i < 4; i++) st[i] = prv[i] = st1[i] = 0;
    st2_3 = 0; n3ClkPrv = 0;
    if (!s_tabsOk) buildTabs();
    t4 = Tab{}; t5 = Tab{}; t7 = Tab{};
    polyInit(pvl, 0x10000, 0x2000);
    pvlPend = 0;
    stPrev = 0xFFFFFFFFu;
    stSnd = stHz = 0; st1_ = 1; st2 = 1;
    actSnd = actHz = act1 = act2 = 0;
    maskDirty = true;
    for (int i = 0; i < 0x20; i++) write((uint8_t)i, 0);
    write(0x14, 0xAA);
    accL = accR = 0; accT = 0;
    cycles = 0;
    if (cold) memset(keys, 0xFF, sizeof(keys));
}

void Dave::write(uint8_t r, uint8_t value) {
    r &= 0x1F;
    reg[r] = value;
    switch (r) {
    case 0x00: case 0x02: case 0x04:
        chFrq[r >> 1] = (chFrq[r >> 1] & 0x0F00) | value;
        break;
    case 0x01: case 0x03: case 0x05: {
        const int c = r >> 1;
        chFrq[c] = (chFrq[c] & 0x00FF) | ((value & 0x0F) << 8);
        chIn[c] = (value >> 4) & 3;
        chHp[c] = (value & 0x40) ? 1 : 0;      // ch0 by ch1, ch1 by ch2, ch2 by noise
        chRm[c] = (value & 0x80) ? 1 : 0;      // ch0 by ch2, ch1 by noise, ch2 by ch0
        break;
    }
    case 0x06:
        n3Clk = value & 3;                     // 31.25 kHz, ch0, ch1, ch2
        pvlSync();
        polyVl(pvl, (value >> 2) & 3);
        noise7 = (value & 0x10) ? 1 : 0;
        n3Lp = (value & 0x20) ? 1 : 0;
        n3Hp = (value & 0x40) ? 1 : 0;
        n3Rm = (value & 0x80) ? 1 : 0;
        break;
    case 0x07:
        for (int c = 0; c < 3; c++) {
            if (value & (1 << c)) { chRun[c] = 0; st1[c] = 0; }
            else if (!chRun[c]) { chPhase[c] = chFrq[c]; chRun[c] = 1; }
        }
        dacL = (value & 0x08) ? 1 : 0;
        dacR = (value & 0x10) ? 1 : 0;
        sndSrc = (value >> 5) & 3;
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: volL[r & 3] = value & 0x3F; break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: volR[r & 3] = value & 0x3F; break;
    case 0x10: case 0x11: case 0x12: case 0x13:
        page[r & 3] = value;
        if (pageCb) pageCb(ud, r & 3, value);
        break;
    case 0x14: {
        const uint8_t t = (uint8_t)(value ^ 0x55);
        enSnd = (t & 0x01) ? 0 : 1; if (t & 0x03) actSnd = 0;
        enHz  = (t & 0x04) ? 0 : 1; if (t & 0x0C) actHz = 0;
        en1   = (t & 0x10) ? 0 : 1; if (t & 0x30) act1 = 0;
        en2   = (t & 0x40) ? 0 : 1; if (t & 0xC0) act2 = 0;
        break;
    }
    case 0x15: kbRow = value & 0x0F; break;
    case 0x1F: sysconf = value; break;
    default: break;
    }
    if (r >= 0x07 && r <= 0x0F) sumOut();      // DAC mode / volumes: the level now
    if (r < 0x10) maskDirty = true;
}

uint32_t Dave::statusCycles() const {
    // step(): clk1000-- then "< 0" -> the 1 kHz chain (stSnd for source 0, clk50 /
    // stHz, and the 50 Hz source) moves in cycle clk1000 + 1 from now.
    uint32_t n = (uint32_t)(clk1000 + 1);
    if (sndSrc >= 2) {                         // tone channel as the sound interrupt's source
        const int c = sndSrc - 2;
        if (chPhase[c] < 0) n = 1;
        else if (chRun[c] && (uint32_t)chPhase[c] + 1 < n) n = (uint32_t)chPhase[c] + 1;
    }
    return n ? n : 1;
}

uint8_t Dave::read(uint8_t r) const {
    switch (r & 0x1F) {
    case 0x10: case 0x11: case 0x12: case 0x13: return page[r & 3];
    case 0x14:
        return (uint8_t)((stSnd | (actSnd << 1)) | ((stHz | (actHz << 1)) << 2) |
                         ((st1_ | (act1 << 1)) << 4) | ((st2 | (act2 << 1)) << 6));
    case 0x15: return kbRow < 10 ? keys[kbRow] : 0xFF;
    case 0x16: {
        // Tape input idle (bits 6-7 high), then the joysticks the way ep128emu wires
        // them (dave.cpp read 0x16): EXT1 on keys[14], EXT2 on keys[15], both
        // active low with bits 0 RIGHT 1 LEFT 2 DOWN 3 UP 4 FIRE. Row 0 / 5 read the
        // fire buttons (bits 0-2 of the port), rows 1-4 / 6-9 the directions on bit 0.
        uint8_t n = 0xCF;
        if (kbRow < 5) {
            if (kbRow == 0) n &= (uint8_t)(0xF8 | (keys[14] >> 4));
            else            n &= (uint8_t)(0xFE | (keys[14] >> (4 - kbRow)));
        } else if (kbRow < 10) {
            if (kbRow == 5) n &= (uint8_t)(0xF8 | (keys[15] >> 4));
            else            n &= (uint8_t)(0xFE | (keys[15] >> (9 - kbRow)));
        }
        return n;
    }
    }
    return 0xFF;
}

void Dave::setInt1(bool state) {
    const uint8_t old = st1_;
    st1_ = state ? 1 : 0;
    if (!en1 || st1_ || !old || act1) return;          // negative edge, enabled, not active
    act1 = 1;
}

void Dave::step() {
    // polynomial counters (the noise channel's one steps on its clock's falling edge)
    tabStep(t4, s_tab4, 15);
    tabStep(t5, s_tab5, 31);
    const uint8_t clkSrc = n3Clk == 0 ? clk62 : st[n3Clk - 1];
    const bool edge = clkSrc < n3ClkPrv;
    n3ClkPrv = clkSrc;
    if (noise7) {                              // noise = 7-bit, VL free-running
        pvlPend++;
        if (edge) st1[3] = tabStep(t7, s_tab7, 127);
    } else {                                   // noise = VL, 7-bit free-running
        tabStep(t7, s_tab7, 127);
        if (edge) { if (pvlPend) pvlSync(); st1[3] = polyStep(pvl); }
    }
    if (edge && !n3Lp) st2_3 = st1[3];

    // phases, timers, interrupts
    clk62Phase--;
    clk1000--;
    for (int c = 0; c < 3; c++) chPhase[c] -= chRun[c];
    const int* src = sndSrc == 0 ? &clk1000 : sndSrc == 1 ? &clk50 : &chPhase[sndSrc - 2];
    if (clk1000 < 0) {
        clk50--;
        if (*src < 0) { stSnd ^= 1; if (enSnd) trigSnd(); }
        clk1000 = 249;
        if (clk50 < 0) {
            clk50 = 19;
            if (--clk1 < 0) { clk1 = 49; stHz ^= 1; if (enHz) trigHz(); }
        }
    } else if (*src < 0) {
        stSnd ^= 1;
        if (enSnd) trigSnd();
    }
    if (clk62Phase < 0) { clk62Phase = 3; clk62 ^= 1; }

    // channel 3 (noise)
    prv[3] = st[3];
    if (n3Lp && st[2] < prv[2]) st2_3 = st1[3];    // lowpass: hold until ch2 falls
    if (n3Hp && st[0] < prv[0]) st2_3 = 0;         // highpass: 0 when ch0 falls
    st[3] = st2_3;
    if (n3Rm) st[3] ^= (uint8_t)(st[1] ^ 1);       // ring mod: XNOR ch1
    // channels 2, 1, 0: highpass by noise / ch2 / ch1, ring mod by ch0 / noise / ch2
    static const uint8_t hpBy[3] = {1, 2, 3}, rmBy[3] = {2, 3, 0};
    for (int c = 2; c >= 0; c--) {
        prv[c] = st[c];
        if (chPhase[c] < 0) {
            chPhase[c] = chFrq[c];
            switch (chIn[c]) {
            case 0: st1[c] ^= 1; break;
            case 1: st1[c] = t4.out; break;
            case 2: st1[c] = t5.out; break;
            default:
                if (noise7) { if (pvlPend) pvlSync(); st1[c] = pvl.out; }
                else st1[c] = t7.out;
                break;
            }
        }
        const int h = hpBy[c];
        if (chHp[c] && st[h] < prv[h]) st1[c] = 0;
        st[c] = st1[c];
        if (chRm[c]) st[c] ^= (uint8_t)(st[rmBy[c]] ^ 1);
    }

    // Summed only when a channel output changed (a volume or DAC write resums at once).
    const uint32_t pk = (uint32_t)st[0] | (uint32_t)st[1] << 8 | (uint32_t)st[2] << 16 | (uint32_t)st[3] << 24;
    if (pk != stPrev) { stPrev = pk; sumOut(); }
}

// DAC: in DAC mode a side plays channel 0's volume register x4 (sampled sound).
void Dave::sumOut() {
    unsigned l = 0, r = 0;
    if (dacL) l = volL[0] << 2;
    else for (int c = 0; c < 4; c++) if (st[c]) l += volL[c];
    if (dacR) r = volR[0] << 2;
    else for (int c = 0; c < 4; c++) if (st[c]) r += volR[c];
    outLv = (uint8_t)l; outRv = (uint8_t)r;
}

// The T of `k` Dave cycles at one output level into the 128-T samples.
__attribute__((always_inline)) inline void Dave::emit(unsigned t) {
    if (t < 128u - accT) {                     // inside the current sample: no split
        accL += outLv * t; accR += outRv * t; accT += t;
        return;
    }
    while (t) {
        const unsigned take = t < 128u - accT ? t : 128u - accT;
        accL += outLv * take; accR += outRv * take;
        accT += take; t -= take;
        if (accT == 128) {
            if (outL && outN < outCap) {
                outL[outN] = (uint8_t)(accL >> 7);
                outR[outN] = (uint8_t)(accR >> 7);
                outN++;
            }
            accL = accR = 0; accT = 0;
        }
    }
}

static __attribute__((always_inline)) inline void tabSkip(Dave::Tab& t, const uint8_t* tab, unsigned period, uint32_t k) {
    const unsigned ph = (unsigned)((t.ph + k) % period);
    t.ph = (uint8_t)ph;
    t.out = tab[ph ? ph - 1 : period - 1];
}

// k cycles in which nothing but the counters moves (the caller checked that no
// counter runs out within them and that no output is still in transition):
// what step() would have done k times, in closed form.
// k cycles in which nothing but the counters moves (the caller checked that no
// counter runs out within them, that no audible or read output is in transition,
// and that the noise steps on no clock edge): what step() would have done k
// times, in closed form. Channels in `idle` (silent, read by nothing, no inputs
// of their own) may run out inside: their phase and output are advanced by
// arithmetic, so a later volume write finds them exactly where step() would.
void Dave::skip(uint32_t k, unsigned idle, bool nz) {
    const unsigned ph4 = t4.ph, ph5 = t5.ph, ph7 = t7.ph;   // before this run
    // Noise on the 62.5 kHz clock, heard and read by nothing (nz): the falling
    // edges inside the k steps step its counter, and only its state must follow.
    // Step i samples the clock as it was after i-1 cycles; the clock toggles after
    // cycles clk62Phase+1, +5, +9, ... and every second toggle is a falling edge.
    uint32_t edges = 0;
    bool lastEdge = false;                     // step k itself saw an edge (prv[3] then lags st[3])
    const uint8_t st3Before = st[3];
    if (nz) {
        if (clk62 == 0 && n3ClkPrv) { edges = 1; lastEdge = (k == 1); }   // step 1 sees the edge pending now
        const uint32_t j0 = (uint32_t)clk62Phase + (clk62 ? 1u : 5u);  // first falling toggle, as "after j0 cycles"
        if (j0 <= k - 1) { edges += (k - 1 - j0) / 8 + 1; if (((k - 1 - j0) & 7) == 0) lastEdge = true; }
    }
    for (int c = 0; c < 3; c++) {
        if (!(idle & (1u << c))) { if (chRun[c]) chPhase[c] -= (int)k; continue; }
        const uint8_t stOld = st[c];
        const uint32_t P = (uint32_t)chPhase[c], F = (uint32_t)chFrq[c] + 1;
        if (!chRun[c] || k <= P) {
            // No reload inside: every step copies st1 into st (an idle channel has no
            // rm of its own), so st follows st1 from step 1 and prv is step k-1's st.
            if (chRun[c]) chPhase[c] -= (int)k;
            prv[c] = k == 1 ? stOld : st1[c];
            if (st[c] != st1[c]) { st[c] = st1[c]; stPrev = 0xFFFFFFFFu; }
            continue;
        }
        const uint32_t nUf = (k - P - 1) / F + 1;          // times it ran out
        const uint32_t u = P + 1 + (nUf - 1) * F;          // the step of the last one
        chPhase[c] = (int)(F - 1 - ((k - P - 1) % F));
        uint8_t last, before;                               // output after u, and before it
        if (chIn[c] == 0) {
            last = (uint8_t)(st1[c] ^ (nUf & 1));
            before = (uint8_t)(last ^ 1);
        } else {
            const uint8_t* tab; unsigned per, ph0;
            if (chIn[c] == 1) { tab = s_tab4; per = 15; ph0 = ph4; }
            else if (chIn[c] == 2) { tab = s_tab5; per = 31; ph0 = ph5; }
            else { tab = s_tab7; per = 127; ph0 = ph7; }      // not noise7: free-running
            last = tab[(ph0 + u - 1) % per];
            before = nUf >= 2 ? tab[(ph0 + u - F - 1) % per] : st1[c];
        }
        st1[c] = st[c] = last;
        prv[c] = k == 1 ? stOld : (u == k) ? before : last;
        stPrev = 0xFFFFFFFFu;                  // st[] moved behind step()'s key: resum next
    }
    tabSkip(t4, s_tab4, 15, k);
    tabSkip(t5, s_tab5, 31, k);
    if (noise7) pvlPend += k;                  // the VL counter free-runs, the noise is the 7-bit one
    else tabSkip(t7, s_tab7, 127, k);
    if (nz) {
        // The noise counter steps once per edge (the 7-bit one under noise7, else the
        // VL one) and, with no lp/hp/rm, st2_3 follows it; every step then copies
        // st2_3 into st[3]. prv[3] = st[3] as the step before the last one left it.
        // (a plain loop twice over, not a lambda: GCC outlines one into a flash clone)
        uint8_t st3AfterKm1 = st3Before;       // k == 1: the state before this run
        for (int part = 0; part < 2; part++) {
            const uint32_t e = part == 0 ? (lastEdge ? edges - 1 : edges) : (lastEdge ? 1u : 0u);
            if (part == 1 && k >= 2) st3AfterKm1 = st2_3;
            if (!e) continue;
            if (noise7) { tabSkip(t7, s_tab7, 127, e); st1[3] = t7.out; }
            else { pvlPend += e; pvlSync(); st1[3] = pvl.out; }
            st2_3 = st1[3];
        }
        if (st[3] != st2_3) { st[3] = st2_3; stPrev = 0xFFFFFFFFu; }
        prv[3] = st3AfterKm1;
    } else prv[3] = st[3];                     // constant over quiet steps (tick() checked st[3] == its next value)
    clk1000 -= (int)k;
    // n3ClkPrv = the clock at the start of the last skipped step (step() samples it
    // before toggling); the caller stops before a falling edge, so no noise step.
    if (n3Clk == 0) n3ClkPrv = clk62After(k - 1);
    int ph = clk62Phase - (int)k;
    if (ph < 0) { const int n = (-ph - 1) / 4 + 1; ph += 4 * n; if (n & 1) clk62 ^= 1; }
    clk62Phase = ph;
}

// A channel read by nothing but the output mixer: no own highpass / ring mod /
// noise input, not the noise clock, not another channel's hp / rm source, not the
// sound interrupt's source.
__attribute__((always_inline)) inline bool Dave::chFree(int c) const {
    if (chHp[c] || chRm[c] || (chIn[c] == 3 && noise7)) return false;
    if (n3Clk == c + 1) return false;
    bool read;
    if (c == 0) read = chRm[2] || n3Hp;
    else if (c == 1) read = chHp[0] || n3Rm;
    else read = chHp[1] || chRm[0] || n3Lp;
    if (read) return false;
    return sndSrc != c + 2;
}

// The noise channel on the 62.5 kHz clock, inaudible, feeding no channel, with no
// filter of its own: Dave::skip steps it across the clock edges in closed form.
bool Dave::noiseIdle() const {
    if (n3Clk != 0 || n3Lp || n3Hp || n3Rm) return false;
    if (chHp[2] || chRm[1]) return false;      // channel 2 hp / channel 1 rm read it
    return (dacL || volL[3] == 0) && (dacR || volR[3] == 0);
}

// Channels whose output nobody hears or reads (Dave::skip may run them through).
unsigned Dave::idleMask() const {
    unsigned m = 0;
    for (int c = 0; c < 3; c++) {
        if (!(dacL || volL[c] == 0) || !(dacR || volR[c] == 0)) continue;   // audible
        if (chFree(c)) m |= 1u << c;
    }
    return m;
}

// Audible SQUARE channels read by nothing else: their toggles move only the output
// level, so a skip can emit them as segments (Turbo Esprit's title leaves channel 0
// at frequency 0, volume 47 — a 125 kHz square, a toggle every cycle).
unsigned Dave::sqMask(unsigned idle) const {
    unsigned m = 0;
    for (int c = 0; c < 3; c++) {
        if (idle & (1u << c)) continue;
        if (chIn[c] != 0 || !chRun[c]) continue;
        if (dacL && dacR) continue;                 // DAC mode: the level ignores st[]
        if (chFree(c)) m |= 1u << c;
    }
    return m;
}

// k skipped cycles with the square channels in `sq` toggling inside them: the
// output level segment by segment, exactly as k steps would have emitted it.
// Only the levels: Dave::skip sets the state afterwards.
void Dave::emitSq(uint32_t k, unsigned sq, unsigned ct) {
    int ph[3]; uint8_t sv[3];
    for (int c = 0; c < 3; c++) { ph[c] = chPhase[c]; sv[c] = st[c]; }
    uint32_t done = 0;
    while (done < k) {
        uint32_t seg = k - done;                    // cycles before the next toggle step
        for (int c = 0; c < 3; c++) if ((sq & (1u << c)) && (uint32_t)ph[c] < seg) seg = (uint32_t)ph[c];
        if (seg) {
            emit(ct * seg);
            for (int c = 0; c < 3; c++) if (sq & (1u << c)) ph[c] -= (int)seg;
            done += seg;
            if (done == k) break;
        }
        // the toggle step: channels at phase 0 reload and flip, the others count
        for (int c = 0; c < 3; c++) {
            if (!(sq & (1u << c))) continue;
            if (ph[c] == 0) { ph[c] = chFrq[c]; sv[c] ^= 1; st[c] = sv[c]; }
            else ph[c]--;
        }
        sumOut();
        emit(ct);
        done++;
    }
}

// The 62.5 kHz clock after m more cycles (it toggles when its phase runs out).
uint8_t Dave::clk62After(uint32_t m) const {
    if (m <= (uint32_t)clk62Phase) return clk62;
    const uint32_t n = (m - (uint32_t)clk62Phase - 1) / 4 + 1;
    return (uint8_t)(clk62 ^ (n & 1));
}

void Dave::tick(int n) {
    // A sample is 128 T of the 4 MHz clock (31250 Hz) whatever a Dave cycle lasts
    // (16 T, or 24 T with #BF bit 1): each cycle's level is weighted by the T it
    // covers. Between events (a counter running out) step() changes nothing but
    // the counters once every output has settled, so those cycles are skipped in
    // one go (5000 full steps a frame were ~11% of core0, hw 2026-10-30).
    const unsigned ct = (unsigned)cycleT();
    if (maskDirty) {
        idleC = (uint8_t)idleMask();
        sqC = (uint8_t)sqMask(idleC);
        nzC = noiseIdle() ? 1 : 0;
        maskDirty = false;
    }
    const unsigned idle = idleC, sq = sqC, skipm = idle | sq;
    const bool nz = nzC != 0;
    while (n > 0) {
        const uint8_t clkSrc = n3Clk == 0 ? clk62 : st[n3Clk - 1];
        // Every step recomputes st[c] = st1[c] (^ ~st[rm source] under rm) and st[3]
        // = st2_3 (^ ~st[1]): right after a register write (a channel stopped, an rm
        // / lp bit flipped) the two can differ for ONE step, and a skip would hold
        // the old output. A non-idle channel must also be settled (st == prv): a
        // transition of it feeds hp / lp / rm consumers at the next step.
        static const uint8_t rmBy[3] = {2, 3, 0};
        bool quiet = true;
        for (int c = 0; c < 3; c++) {
            if (idle & (1u << c)) continue;                 // skip() syncs st from st1 itself
            if (st[c] != (uint8_t)(st1[c] ^ (chRm[c] ? (st[rmBy[c]] ^ 1) : 0))) quiet = false;
            if (!(sq & (1u << c)) && st[c] != prv[c]) quiet = false;
        }
        if (!nz && st[3] != (uint8_t)(st2_3 ^ (n3Rm ? (st[1] ^ 1) : 0))) quiet = false;
        if (quiet) {
            int k = clk1000;
            for (int c = 0; c < 3; c++) if (chRun[c] && !(skipm & (1u << c)) && chPhase[c] < k) k = chPhase[c];
            bool ok = false;
            if (nz) {
                // Inaudible, unread noise on the 62.5 kHz clock: its edges are
                // folded into skip(), nothing bounds k but the counters above.
                ok = true;
            } else if (n3Clk == 0 && !chHp[2] && !chRm[1]) {
                // Noise on the 62.5 kHz clock, feeding nothing but the output: only
                // the steps that see a falling clock edge step it; up to the next
                // such step nothing else changes.
                int f;
                if (clk62 == 0 && n3ClkPrv) f = 1;
                else if (clk62) f = clk62Phase + 2;
                else f = clk62Phase + 6;
                if (f - 1 < k) k = f - 1;
                ok = true;
            } else if (st[3] == prv[3] && clkSrc == n3ClkPrv) {
                if (n3Clk == 0 && clk62Phase < k) k = clk62Phase;
                ok = true;
            }
            if (k > n) k = n;
            if (ok && k > 0) {
                if (sq) emitSq((uint32_t)k, sq, ct);
                else emit(ct * (unsigned)k);
                skip((uint32_t)k, skipm, nz);
                if (sq) sumOut();                   // the level of the state the skip left
                cycles += (uint32_t)k;
                n -= k;
                continue;
            }
        }
        step();
        cycles++; steps++;
        emit(ct);
        n--;
    }
}

} // namespace EpCore
