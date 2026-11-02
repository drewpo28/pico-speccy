#include "ElmSid.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include("pico.h")
#include "app/TryAlloc.h"
#else
#define tryMalloc malloc
#endif

namespace ElmSid {

namespace {

constexpr uint32_t CLOCK_PAL  = 985248;
constexpr uint32_t CLOCK_NTSC = 1022730;
constexpr int      BIAS       = 128;
enum { GATE = 1, SYNC = 2, RING = 4, TEST = 8, TRI = 0x10, SAW = 0x20, PULSE = 0x40, NOISE = 0x80 };
enum { ATTACK, DECAY_SUSTAIN, RELEASE };

// reSID envelope.cc: SID cycles per envelope step for each 4-bit rate.
const uint16_t kRatePeriod[16] = { 9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126,
                                   3907, 11720, 19532, 31251 };

// reSID filter.cc f0_points: cutoff register (11 bits) -> Hz. The 6581 curve falls back
// at 1024, as measured on the chip.
struct Pt { uint16_t fc, hz; };
const Pt k6581[] = { {0, 220}, {128, 230}, {256, 250}, {384, 300}, {512, 420}, {640, 780},
    {768, 1600}, {832, 2300}, {896, 3200}, {960, 4300}, {992, 5000}, {1008, 5400},
    {1016, 5700}, {1023, 6000}, {1024, 4600}, {1032, 4800}, {1056, 5300}, {1088, 6000},
    {1120, 6600}, {1152, 7200}, {1280, 9500}, {1408, 12000}, {1536, 14500}, {1664, 16000},
    {1792, 17100}, {1920, 17700}, {2047, 18000} };
const Pt k8580[] = { {0, 0}, {128, 800}, {256, 1600}, {384, 2500}, {512, 3300}, {640, 4100},
    {768, 4800}, {896, 5600}, {1024, 6500}, {1152, 7500}, {1280, 8400}, {1408, 9200},
    {1536, 9800}, {1664, 10500}, {1792, 11000}, {1920, 11700}, {2047, 12500} };

struct Voice {
    uint32_t acc, lfsr;              // 24-bit phase, 23-bit noise
    uint16_t freq, pw;
    uint8_t ctrl, ad, sr;
    uint8_t env, state, expCnt, expPeriod;
    uint16_t rateCnt, ratePeriod;    // 15-bit counter: a period below it wraps (ADSR bug)
    bool holdZero;
};

struct State {
    uint8_t reg[0x1D], par, reg20, bus;
    Voice v[3];
    bool muted, wasLive;
    int rate, n, bias, kick;
    uint32_t cycStep, cyc;           // SID cycles per output sample, 16.16
    int waveZero, voiceDc, mixerDc;  // 6581: wave and mixer DC, the volume-digi path
    float a1, a2, a3, k, ic1, ic2;   // ZDF state-variable filter
    float dcX, dcY, dcR;             // the output capacitor
    int16_t out[MAX_SAMPLES];
};

State* s;

void filterCoef() {
    const Pt* t = (s->par & PAR_8580) ? k8580 : k6581;
    const int cnt = (s->par & PAR_8580) ? sizeof(k8580) / sizeof(Pt) : sizeof(k6581) / sizeof(Pt);
    const int fc = s->reg[0x16] << 3 | (s->reg[0x15] & 7);
    int i = 0;
    while (i + 2 < cnt && t[i + 1].fc < fc) i++;
    const int span = t[i + 1].fc - t[i].fc;
    float hz = span ? t[i].hz + (float)(t[i + 1].hz - t[i].hz) * (fc - t[i].fc) / span : t[i + 1].hz;
    const float top = 0.45f * s->rate;
    if (hz > top) hz = top;
    if (hz < 20.f) hz = 20.f;
    const float g = tanf(3.14159265f * hz / s->rate);
    s->k = 1.f / (0.707f + (s->reg[0x17] >> 4) / 15.f);
    s->a1 = 1.f / (1.f + g * (g + s->k));
    s->a2 = g * s->a1;
    s->a3 = g * s->a2;
}

void envClock(Voice& v, uint32_t n) {
    while (n) {
        const uint32_t need = v.rateCnt < v.ratePeriod ? v.ratePeriod - v.rateCnt
                                                       : 0x8000u - v.rateCnt + v.ratePeriod;
        if (n < need) { v.rateCnt = (uint16_t)((v.rateCnt + n) & 0x7FFF); return; }
        n -= need;
        v.rateCnt = 0;
        if (v.state != ATTACK && ++v.expCnt != v.expPeriod) continue;
        v.expCnt = 0;
        if (v.holdZero) continue;
        switch (v.state) {
        case ATTACK:
            if (++v.env == 0xFF) { v.state = DECAY_SUSTAIN; v.ratePeriod = kRatePeriod[v.ad & 15]; }
            break;
        case DECAY_SUSTAIN:
            if (v.env != (v.sr >> 4) * 0x11) v.env--;
            break;
        default:
            v.env--;
        }
        switch (v.env) {
        case 0xFF: v.expPeriod = 1;  break;
        case 0x5D: v.expPeriod = 2;  break;
        case 0x36: v.expPeriod = 4;  break;
        case 0x1A: v.expPeriod = 8;  break;
        case 0x0E: v.expPeriod = 16; break;
        case 0x06: v.expPeriod = 30; break;
        case 0x00: v.expPeriod = 1; v.holdZero = true; break;
        }
    }
}

inline void lfsrStep(Voice& v) {
    v.lfsr = ((v.lfsr << 1) | (((v.lfsr >> 22) ^ (v.lfsr >> 17)) & 1)) & 0x7FFFFF;
}

// n SID cycles at once. Sync lands on the cycle the source's MSB rose in.
void oscClock(uint32_t n) {
    uint32_t over[3];
    bool rose[3];
    for (int i = 0; i < 3; i++) {
        Voice& v = s->v[i];
        rose[i] = false;
        if (v.ctrl & TEST) continue;
        const uint32_t old = v.acc, nw = old + v.freq * n;
        for (uint32_t c = ((nw + 0x80000) >> 20) - ((old + 0x80000) >> 20); c; c--) lfsrStep(v);
        if (old < 0x800000 && nw >= 0x800000) { rose[i] = true; over[i] = nw - 0x800000; }
        v.acc = nw & 0xFFFFFF;
    }
    for (int i = 0; i < 3; i++) {
        Voice& v = s->v[i];
        const int src = i ? i - 1 : 2;
        if ((v.ctrl & SYNC) && !(v.ctrl & TEST) && rose[src])
            v.acc = (v.freq * (over[src] / s->v[src].freq)) & 0xFFFFFF;
    }
}

uint32_t wave(int i) {
    const Voice& v = s->v[i];
    const uint8_t w = v.ctrl >> 4;
    if (!w) return 0;
    uint32_t out = 0xFFF;
    if (v.ctrl & TRI) {
        uint32_t msb = v.acc & 0x800000;
        if (v.ctrl & RING) msb ^= s->v[i ? i - 1 : 2].acc & 0x800000;
        out &= ((msb ? ~v.acc : v.acc) >> 11) & 0xFFF;
    }
    if (v.ctrl & SAW) out &= v.acc >> 12;
    if (v.ctrl & PULSE) out &= ((v.ctrl & TEST) || (v.acc >> 12) >= v.pw) ? 0xFFF : 0;
    if (v.ctrl & NOISE) {
        const uint32_t r = v.lfsr;
        out &= ((r >> 9) & 0x800) | ((r >> 8) & 0x400) | ((r >> 5) & 0x200) | ((r >> 3) & 0x100)
             | ((r >> 2) & 0x080) | ((r << 1) & 0x040) | ((r << 3) & 0x020) | ((r << 4) & 0x010);
    }
    return out;
}

bool live() {
    if (s->kick) return true;
    for (int i = 0; i < 3; i++) if ((s->v[i].ctrl & GATE) || s->v[i].env) return true;
    return false;
}

int sample() {
    const uint8_t filt = s->reg[0x17], mode = s->reg[0x18];
    int vf = 0, vnf = 0;
    for (int i = 0; i < 3; i++) {
        const int o = (((int)wave(i) - s->waveZero) * s->v[i].env >> 7) + s->voiceDc;
        if (filt & (1 << i)) vf += o;
        else if (i != 2 || !(mode & 0x80)) vnf += o;   // 3OFF: voice 3 out of the direct path
    }
    const float v3 = vf - s->ic2;
    const float bp = s->a1 * s->ic1 + s->a2 * v3;
    const float lp = s->ic2 + s->a2 * s->ic1 + s->a3 * v3;
    s->ic1 = 2.f * bp - s->ic1;
    s->ic2 = 2.f * lp - s->ic2;
    float f = 0.f;
    if (mode & 0x10) f += lp;
    if (mode & 0x20) f += bp;
    if (mode & 0x40) f += vf - s->k * bp - lp;
    const float x = (vnf + f + s->mixerDc) * (mode & 15);
    if (!s->wasLive) { s->dcX = x; s->dcY = 0.f; s->wasLive = true; }
    s->dcY = x - s->dcX + s->dcR * s->dcY;
    s->dcX = x;
    int y = (int)(s->dcY * (1.f / 2048.f));   // 3 full voices at volume 15 = +-90
    if (y > 255) y = 255; else if (y < -255) y = -255;
    return y;
}

} // namespace

bool init(int outputRate) {
    if (!s) s = (State*)tryMalloc(sizeof(State));
    if (!s) return false;
    memset(s, 0, sizeof(State));
    s->rate = outputRate;
    reset();
    return true;
}

void reset() {
    if (!s) return;
    const int rate = s->rate;
    const uint8_t par = s->par;
    const bool muted = s->muted;
    memset(s, 0, sizeof(State));
    s->rate = rate;
    s->muted = muted;
    s->dcR = 1.f - 6.2831853f * 5.f / rate;   // 5 Hz
    for (int i = 0; i < 3; i++) {
        Voice& v = s->v[i];
        v.lfsr = 0x7FFFF8;
        v.state = RELEASE;
        v.ratePeriod = kRatePeriod[0];
        v.expPeriod = 1;
        v.holdZero = true;
    }
    setParams(par);
}

void setParams(uint8_t par) {
    if (!s) return;
    s->par = par & 3;
    const bool m8580 = s->par & PAR_8580;
    s->waveZero = m8580 ? 0x800 : 0x380;
    s->voiceDc  = m8580 ? 0 : 0x800 * 0xFF >> 7;
    s->mixerDc  = m8580 ? 0 : -0xFFF * 0xFF / 18 >> 7;
    s->cycStep = (uint32_t)(((uint64_t)((s->par & PAR_NTSC) ? CLOCK_NTSC : CLOCK_PAL) << 16) / s->rate);
    filterCoef();
}

void mute(bool m) { if (s) s->muted = m; }

void write(uint8_t reg, uint8_t val) {
    if (!s) return;
    if (reg == 0x1F) { setParams(val); return; }
    if (reg == 0x20) { s->reg20 = val; return; }
    if (reg > 0x1C) return;
    s->bus = val;
    s->reg[reg] = val;
    s->kick = s->rate / 2;
    if (reg >= 0x15) {
        if (reg <= 0x17) filterCoef();
        return;
    }
    Voice& v = s->v[reg / 7];
    switch (reg % 7) {
    case 0: v.freq = (v.freq & 0xFF00) | val; break;
    case 1: v.freq = (uint16_t)((v.freq & 0x00FF) | val << 8); break;
    case 2: v.pw = (v.pw & 0xF00) | val; break;
    case 3: v.pw = (uint16_t)((v.pw & 0x0FF) | (val & 15) << 8); break;
    case 4: {
        const uint8_t was = v.ctrl;
        v.ctrl = val;
        if (val & TEST) v.acc = 0;
        else if (was & TEST) v.lfsr = 0x7FFFFF;
        if (!(was & GATE) && (val & GATE)) {
            v.state = ATTACK;
            v.ratePeriod = kRatePeriod[v.ad >> 4];
            v.holdZero = false;
        } else if ((was & GATE) && !(val & GATE)) {
            v.state = RELEASE;
            v.ratePeriod = kRatePeriod[v.sr & 15];
        }
        break;
    }
    case 5:
        v.ad = val;
        if (v.state == ATTACK) v.ratePeriod = kRatePeriod[val >> 4];
        else if (v.state == DECAY_SUSTAIN) v.ratePeriod = kRatePeriod[val & 15];
        break;
    case 6:
        v.sr = val;
        if (v.state == RELEASE) v.ratePeriod = kRatePeriod[val & 15];
        break;
    }
}

uint8_t read(uint8_t reg) {
    if (!s) return 0xFF;
    switch (reg) {
    case 0x19: case 0x1A: return 0xFF;       // POTX/POTY: no paddles
    case 0x1B: return (uint8_t)(wave(2) >> 4);
    case 0x1C: return s->v[2].env;
    case 0x1F: return s->par;
    case 0x20: return s->reg20;
    }
    return reg < 0x1F ? s->bus : 0xFF;       // write-only: the last byte on the bus
}

void renderTo(int to) {
    if (!s) return;
    if (to > MAX_SAMPLES) to = MAX_SAMPLES;
    for (int i = s->n; i < to; i++) {
        s->cyc += s->cycStep;
        const uint32_t n = s->cyc >> 16;
        s->cyc &= 0xFFFF;
        oscClock(n);
        for (int c = 0; c < 3; c++) envClock(s->v[c], n);
        if (live()) {
            if (s->bias < BIAS) s->bias++;
            s->out[i] = (int16_t)(s->bias + sample());
        } else {
            if (s->bias > 0) s->bias--;
            s->wasLive = false;
            s->out[i] = (int16_t)s->bias;
        }
        if (s->kick) s->kick--;
    }
    if (to > s->n) s->n = to;
}

void frameEnd(int samples) {
    if (!s) return;
    renderTo(samples);
    s->n = 0;
}

const int16_t* mix() {
    if (!s || s->muted || (!s->bias && !live())) return nullptr;
    return s->out;
}

}
