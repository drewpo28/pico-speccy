// pico-speccy — Enterprise Nick + Dave core. See EpCore.h for the model and its
// sources. Flash code; no firmware dependencies (the host simulator links it too).

#include "EpCore.h"
#include <string.h>

namespace EpCore {

// ---------------------------------------------------------------------------
// Pixel decoding (ep128emu NickTables, computed instead of tabled: no RAM on the
// machines that do not run it). 4 colours: pixel k = bits (3-k, 7-k); 16 colours:
// pixel 0 = bits 1,5,3,7 and pixel 1 = bits 0,4,2,6 (value bits 3..0).

static inline uint8_t four(uint8_t b, int k) {
    return (uint8_t)((((b >> (3 - k)) & 1) << 1) | ((b >> (7 - k)) & 1));
}
static inline uint8_t sixteen(uint8_t b, int k) {
    const uint8_t x = (uint8_t)(b >> (k ^ 1));          // pixel 0 reads the odd bits
    return (uint8_t)(((x & 1) << 3) | (((x >> 4) & 1) << 2) | (((x >> 2) & 1) << 1) | ((x >> 6) & 1));
}

uint32_t colourRgb(uint8_t c) {
    const uint32_t r = ((c << 2) & 4) | ((c >> 2) & 2) | ((c >> 6) & 1);
    const uint32_t g = ((c << 1) & 4) | ((c >> 3) & 2) | ((c >> 7) & 1);
    const uint32_t b = ((c >> 1) & 2) | ((c >> 5) & 1);
    return ((r * 255 + 3) / 7) << 16 | ((g * 255 + 3) / 7) << 8 | ((b * 255 + 1) / 3);
}

// ---------------------------------------------------------------------------
// Nick

void Nick::reset() {
    nLines = 1;
    irqFlag = false; vres = false; reload = true;
    colorMode = 0; videoMode = 7;
    altInd0 = altInd1 = lsbAlt = msbAlt = false;
    leftMargin = 8; rightMargin = 54;
    dataBus = 0xFF;
    memset(pal, 0, sizeof(pal));
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
        for (int i = 0; i < 8; i++) pal[8 + i] = (uint8_t)(fb | i);
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

// 8 lores pixels of a 2-colour byte (one pixel a bit, MSB first).
static inline void px2(uint8_t* o, uint8_t b, uint8_t c0, uint8_t c1) {
    for (int k = 0; k < 8; k++) o[k] = (b & (0x80 >> k)) ? c1 : c0;
}

// One display slot (8..53): ep128emu Nick::render_Generic, 8 lores pixels out.
void Nick::slot(int s, uint8_t* o) {
    (void)s;
    if (!displayEnabled) {
        const uint8_t c = videoMode == 0 ? 0 : border;
        memset(o, c, 8);
        return;
    }
    switch (videoMode) {
    case 1: {                                        // PIXEL: two bytes a slot
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
            for (int i = 0; i < 2; i++)
                for (int k = 0; k < 4; k++)
                    o[i * 4 + k] = pal[((b[i] >> (7 - 2 * k)) & 1) | m[i]];
            break;
        case 1:
            for (int i = 0; i < 2; i++)
                for (int k = 0; k < 4; k++) o[i * 4 + k] = pal[four(b[i], k) | m[i]];
            break;
        case 2:
            for (int i = 0; i < 2; i++)
                for (int k = 0; k < 2; k++) o[i * 4 + k * 2] = o[i * 4 + k * 2 + 1] = pal[sixteen(b[i], k) | m[i]];
            break;
        default:
            memset(o, b[0], 4); memset(o + 4, b[1], 4);
            break;
        }
        break;
    }
    case 2: {                                        // ATTRIBUTE
        const uint8_t a = v(ld1);
        uint8_t b = v(ld2);
        ld2++;
        dataBus = b;
        if (lsbAlt) b &= 0xFE;
        if (msbAlt) b &= 0x7F;
        const uint8_t c0 = pal[a >> 4], c1 = pal[a & 15];
        switch (colorMode) {
        case 0: px2(o, b, c0, c1); break;
        case 1: for (int k = 0; k < 4; k++) o[2 * k] = o[2 * k + 1] = ((b >> (7 - k)) & 1) ? c1 : c0; break;
        case 2: memset(o, ((b >> 7) & 1) ? c1 : c0, 4); memset(o + 4, ((b >> 6) & 1) ? c1 : c0, 4); break;
        default: memset(o, b, 8); break;
        }
        break;
    }
    case 3: case 4: case 5: case 6: case 7: {       // CH256 / CH128 / CH64 / invalid / LPIXEL
        uint8_t c = v(ld1), b;
        if (videoMode == 7) b = c;
        else {
            uint16_t a = 0xFFFF;
            if (videoMode == 3) a = (uint16_t)((ld2 << 8) | c);
            else if (videoMode == 4) a = (uint16_t)((ld2 << 7) | (c & 0x7F));
            else if (videoMode == 5) a = (uint16_t)((ld2 << 6) | (c & 0x3F));
            b = v(a);
        }
        dataBus = b;
        uint8_t mk = 0;
        if (altInd0) mk = (uint8_t)((c & 0x40) >> 4);
        if (altInd1 && (c & 0x80)) mk |= 0x02;
        if (lsbAlt && (b & 0x01)) { b &= 0xFE; mk |= 0x04; }
        if (msbAlt && (b & 0x80)) { b &= 0x7F; mk |= 0x02; }
        switch (colorMode) {
        case 0: px2(o, b, pal[0 | mk], pal[1 | mk]); break;
        case 1: for (int k = 0; k < 4; k++) o[2 * k] = o[2 * k + 1] = pal[four(b, k) | mk]; break;
        case 2: memset(o, pal[sixteen(b, 0) | mk], 4); memset(o + 4, pal[sixteen(b, 1) | mk], 4); break;
        default: memset(o, b, 8); break;
        }
        break;
    }
    default:                                         // VSYNC
        dataBus = v(ld1);
        memset(o, 0, 8);
        break;
    }
    ld1++;
}

int Nick::line(uint8_t* out) {
    uint8_t scratch[8];
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
            slot(s, out ? out + (s - FIRST) * 8 : scratch);
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
static inline uint8_t polyStep(Dave::Poly& p) {
    p.out = (p.sr & p.m0) ? 1 : 0;
    const uint8_t b1 = (p.sr & p.m1) ? 1 : 0;
    p.sr = ((p.sr << 1) | (uint32_t)(p.out ^ b1)) & p.mask;
    return p.out;
}
// #A6 bits 2-3: 17 / 15 / 11 / 9-bit variable-length counter (ep128emu polycnt_params).
static void polyVl(Dave::Poly& p, int sel) {
    static const uint32_t m[4][2] = {{0x10000, 0x2000}, {0x4000, 0x2000}, {0x400, 0x100}, {0x100, 0x10}};
    if (p.m0 == m[sel][0]) return;
    const uint32_t sr = p.sr;
    p.m0 = m[sel][0]; p.m1 = m[sel][1]; p.mask = (p.m0 << 1) - 1;
    p.sr = sr & p.mask;
    if (!p.sr) p.sr = p.mask;                  // an all-zero LFSR would stay silent
}

void Dave::reset(bool cold) {
    clk1000 = clk50 = clk1 = 0;
    clk62Phase = 0; clk62 = 0;
    for (int i = 0; i < 3; i++) { chPhase[i] = 0; chFrq[i] = 0; chRun[i] = 1; }
    for (int i = 0; i < 4; i++) st[i] = prv[i] = st1[i] = 0;
    st2_3 = 0; n3ClkPrv = 0;
    polyInit(p4, 0x8, 0x4);
    polyInit(p5, 0x10, 0x4);
    polyInit(p7, 0x40, 0x20);
    polyInit(pvl, 0x10000, 0x2000);
    stSnd = stHz = 0; st1_ = 1; st2 = 1;
    actSnd = actHz = act1 = act2 = 0;
    for (int i = 0; i < 0x20; i++) write((uint8_t)i, 0);
    write(0x14, 0xAA);
    accL = accR = 0; accN = 0;
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
}

uint8_t Dave::read(uint8_t r) const {
    switch (r & 0x1F) {
    case 0x10: case 0x11: case 0x12: case 0x13: return page[r & 3];
    case 0x14:
        return (uint8_t)((stSnd | (actSnd << 1)) | ((stHz | (actHz << 1)) << 2) |
                         ((st1_ | (act1 << 1)) << 4) | ((st2 | (act2 << 1)) << 6));
    case 0x15: return kbRow < 10 ? keys[kbRow] : 0xFF;
    case 0x16: return 0xCF;               // no tape signal, no joystick
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
    polyStep(p4);
    polyStep(p5);
    const uint8_t clkSrc = n3Clk == 0 ? clk62 : st[n3Clk - 1];
    const bool edge = clkSrc < n3ClkPrv;
    n3ClkPrv = clkSrc;
    Poly& noise = noise7 ? p7 : pvl;           // the noise channel's counter
    Poly& other = noise7 ? pvl : p7;           // free-running
    polyStep(other);
    if (edge) {
        st1[3] = polyStep(noise);
        if (!n3Lp) st2_3 = st1[3];
    }

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
            case 1: st1[c] = p4.out; break;
            case 2: st1[c] = p5.out; break;
            default: st1[c] = noise7 ? pvl.out : p7.out; break;
            }
        }
        const int h = hpBy[c];
        if (chHp[c] && st[h] < prv[h]) st1[c] = 0;
        st[c] = st1[c];
        if (chRm[c]) st[c] ^= (uint8_t)(st[rmBy[c]] ^ 1);
    }

    // DAC: in DAC mode a side plays channel 0's volume register x4 (sampled sound)
    unsigned l = 0, r = 0;
    if (dacL) l = volL[0] << 2;
    else for (int c = 0; c < 4; c++) if (st[c]) l += volL[c];
    if (dacR) r = volR[0] << 2;
    else for (int c = 0; c < 4; c++) if (st[c]) r += volR[c];
    outLv = (uint8_t)l; outRv = (uint8_t)r;
}

void Dave::tick(int n) {
    while (n-- > 0) {
        step();
        accL += outLv; accR += outRv;
        if (++accN == 8) {
            if (outL && outN < outCap) {
                outL[outN] = (uint8_t)(accL >> 3);
                outR[outN] = (uint8_t)(accR >> 3);
                outN++;
            }
            accL = accR = 0; accN = 0;
        }
    }
}

} // namespace EpCore
