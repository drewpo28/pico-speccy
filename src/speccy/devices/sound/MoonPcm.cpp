/*
 * MoonPcm.cpp — the YMF278B (OPL4) wavetable half for ZXM-MoonSound.
 * See MoonPcm.h for the model, the deviations from ymfm and its license.
 *
 * No firmware dependencies: tools/moonpcm_test.cpp builds it on the host.
 */

#include "MoonPcm.h"
#include <string.h>

#if !defined(MOONSOUND) || MOONSOUND   // cmake/board.cmake

MoonPcm* moonpcm = nullptr;

namespace {

// ymfm attenuation_to_volume: 5.8 log attenuation -> 13-bit linear volume.
const uint16_t kPow[256] = {
    0x1FE8, 0x1FD4, 0x1FBC, 0x1FA8, 0x1F90, 0x1F7C, 0x1F68, 0x1F50,
    0x1F3C, 0x1F24, 0x1F10, 0x1EFC, 0x1EE4, 0x1ED0, 0x1EB8, 0x1EA4,
    0x1E90, 0x1E7C, 0x1E64, 0x1E50, 0x1E3C, 0x1E28, 0x1E10, 0x1DFC,
    0x1DE8, 0x1DD4, 0x1DC0, 0x1DA8, 0x1D94, 0x1D80, 0x1D6C, 0x1D58,
    0x1D44, 0x1D30, 0x1D1C, 0x1D08, 0x1CF4, 0x1CE0, 0x1CCC, 0x1CB8,
    0x1CA4, 0x1C90, 0x1C7C, 0x1C68, 0x1C54, 0x1C40, 0x1C2C, 0x1C18,
    0x1C08, 0x1BF4, 0x1BE0, 0x1BCC, 0x1BB8, 0x1BA4, 0x1B94, 0x1B80,
    0x1B6C, 0x1B58, 0x1B48, 0x1B34, 0x1B20, 0x1B10, 0x1AFC, 0x1AE8,
    0x1AD4, 0x1AC4, 0x1AB0, 0x1AA0, 0x1A8C, 0x1A78, 0x1A68, 0x1A54,
    0x1A44, 0x1A30, 0x1A20, 0x1A0C, 0x19FC, 0x19E8, 0x19D8, 0x19C4,
    0x19B4, 0x19A0, 0x1990, 0x197C, 0x196C, 0x195C, 0x1948, 0x1938,
    0x1924, 0x1914, 0x1904, 0x18F0, 0x18E0, 0x18D0, 0x18C0, 0x18AC,
    0x189C, 0x188C, 0x1878, 0x1868, 0x1858, 0x1848, 0x1838, 0x1824,
    0x1814, 0x1804, 0x17F4, 0x17E4, 0x17D4, 0x17C0, 0x17B0, 0x17A0,
    0x1790, 0x1780, 0x1770, 0x1760, 0x1750, 0x1740, 0x1730, 0x1720,
    0x1710, 0x1700, 0x16F0, 0x16E0, 0x16D0, 0x16C0, 0x16B0, 0x16A0,
    0x1690, 0x1680, 0x1670, 0x1664, 0x1654, 0x1644, 0x1634, 0x1624,
    0x1614, 0x1604, 0x15F8, 0x15E8, 0x15D8, 0x15C8, 0x15BC, 0x15AC,
    0x159C, 0x158C, 0x1580, 0x1570, 0x1560, 0x1550, 0x1544, 0x1534,
    0x1524, 0x1518, 0x1508, 0x14F8, 0x14EC, 0x14DC, 0x14D0, 0x14C0,
    0x14B0, 0x14A4, 0x1494, 0x1488, 0x1478, 0x146C, 0x145C, 0x1450,
    0x1440, 0x1430, 0x1424, 0x1418, 0x1408, 0x13FC, 0x13EC, 0x13E0,
    0x13D0, 0x13C4, 0x13B4, 0x13A8, 0x139C, 0x138C, 0x1380, 0x1370,
    0x1364, 0x1358, 0x1348, 0x133C, 0x1330, 0x1320, 0x1314, 0x1308,
    0x12F8, 0x12EC, 0x12E0, 0x12D4, 0x12C4, 0x12B8, 0x12AC, 0x12A0,
    0x1290, 0x1284, 0x1278, 0x126C, 0x1260, 0x1250, 0x1244, 0x1238,
    0x122C, 0x1220, 0x1214, 0x1208, 0x11F8, 0x11EC, 0x11E0, 0x11D4,
    0x11C8, 0x11BC, 0x11B0, 0x11A4, 0x1198, 0x118C, 0x1180, 0x1174,
    0x1168, 0x115C, 0x1150, 0x1144, 0x1138, 0x112C, 0x1120, 0x1114,
    0x1108, 0x10FC, 0x10F0, 0x10E4, 0x10D8, 0x10CC, 0x10C0, 0x10B4,
    0x10A8, 0x10A0, 0x1094, 0x1088, 0x107C, 0x1070, 0x1064, 0x1058,
    0x1050, 0x1044, 0x1038, 0x102C, 0x1020, 0x1018, 0x100C, 0x1000,
};

// ymfm attenuation_increment: 4-bit EG step per (6-bit rate, 3-bit index).
const uint32_t kInc[64] = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010,
    0x10101010, 0x10101010, 0x11101110, 0x11101110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x11111111, 0x21112111, 0x21212121, 0x22212221,
    0x22222222, 0x42224222, 0x42424242, 0x44424442,
    0x44444444, 0x84448444, 0x84848484, 0x88848884,
    0x88888888, 0x88888888, 0x88888888, 0x88888888,
};

const uint8_t kLfoSteps[8] = { 1, 12, 19, 25, 31, 35, 37, 42 };
const uint8_t kAmDepth[8]  = { 0, 0x14, 0x20, 0x28, 0x30, 0x40, 0x50, 0x80 };
const uint8_t kPmDepth[8]  = { 0, 2, 3, 4, 6, 12, 24, 48 };
// OPL4 mix level -> scale (.11), ymfm ymf278b::generate.
const int16_t kMix[8] = { 0x7fa, 0x5a4, 0x3fd, 0x2d2, 0x1fe, 0x169, 0xff, 0 };

// The power-on FM mix level (0xF8 = 0x1B: 3 = -9 dB on both sides) is the level
// the FM half already has as a plain OPL3, so factors are relative to it.
constexpr int32_t kMixRef = 0x2d2;
// The PCM half is the chip's own ratio against FM, halved: our mixer taps the
// OPL buffer at +-16k full scale where the chip's DAC is +-32k.
constexpr int kPcmShift = 1;

const uint16_t EG_QUIET = 0x200;

inline uint32_t effRate(uint32_t raw, int32_t corr) {
    if (raw == 0)  return 0;
    if (raw == 15) return 63;
    int32_t r = (int32_t)raw * 4 + corr;
    return r < 0 ? 0 : r > 63 ? 63 : (uint32_t)r;
}

inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

} // namespace

MoonPcm::MoonPcm(const uint8_t* rom, uint8_t* ram) : m_rom(rom), m_ram(ram) {
    memcpy(m_pow, kPow, sizeof(m_pow));
    memcpy(m_inc, kInc, sizeof(m_inc));
    setRate(31250);
    reset();
}

void MoonPcm::setRate(uint32_t outRate, bool halfRate) {
    if (!outRate) outRate = 31250;
    m_ratio  = (uint32_t)(((uint64_t)CHIP_RATE << 16) / outRate);
    m_ratio2 = m_ratio * 2;
    m_half = 0xFF;                  // force the recompute below
    setHalfRate(halfRate);
}

void MoonPcm::setHalfRate(bool halfRate) {
    const uint8_t h = halfRate ? 1 : 0;
    if (m_half == h) return;
    m_half = h;
    m_stepRatio = m_half ? m_ratio2 : m_ratio;
    m_halfTick = 0;
    m_cL = m_cR = 0;
    for (Ch& c : m_ch) c.stepOut = (uint32_t)(((uint64_t)c.step * m_stepRatio) >> 16);
}

void MoonPcm::reset() {
    memset(m_regs, 0, sizeof(m_regs));
    m_regs[0xF8] = 0x1B;
    memset(m_ch, 0, sizeof(m_ch));
    for (Ch& c : m_ch) { c.eg = EG_RELEASE; c.env = 0x3FF; c.tl = c.tlTarget = 0x7F << 10; }
    m_active = 0;
    m_envCounter = 0;
    m_acc = 0;
    m_halfTick = 0;
    m_cL = m_cR = 0;
    m_addr = 0;
    m_new2 = false;
    m_flashCyc = 0;
    m_flashId = false;
    mixRecalc();
    for (int i = 0; i < CHANNELS; i++) recache(i);
}

void MoonPcm::mixRecalc() {
    const uint8_t f8 = m_regs[0xF8], f9 = m_regs[0xF9];
    m_fmFL  = (int32_t)kMix[f8 & 7]        * 4096 / kMixRef;
    m_fmFR  = (int32_t)kMix[(f8 >> 3) & 7] * 4096 / kMixRef;
    m_pcmFL = ((int32_t)kMix[f9 & 7]        * 4096 / kMixRef) >> kPcmShift;
    m_pcmFR = ((int32_t)kMix[(f9 >> 3) & 7] * 4096 / kMixRef) >> kPcmShift;
}

// ymfm pcm_registers::cache_channel_data.
void MoonPcm::recache(int ch) {
    Ch& c = m_ch[ch];
    const uint8_t r20 = m_regs[0x20 + ch], r38 = m_regs[0x38 + ch];
    const uint8_t r68 = m_regs[0x68 + ch];
    const int32_t  oct  = (int8_t)(uint8_t)(r38 & 0xF0) >> 4;
    const uint32_t fnum = ((r20 >> 1) & 0x7F) | ((uint32_t)(r38 & 7) << 7);
    const int32_t  sh   = oct + 7;
    c.step = sh >= 0 ? ((0x400u | fnum) << sh) >> 2 : ((0x400u | fnum) >> 1) >> 2;
    c.stepOut = (uint32_t)(((uint64_t)c.step * m_stepRatio) >> 16);

    c.tlTarget = (uint32_t)((m_regs[0x50 + ch] >> 1) & 0x7F) << 10;
    if (m_regs[0x50 + ch] & 1) c.tl = c.tlTarget;           // level direct

    const int32_t pan = (int8_t)(uint8_t)(r68 << 4) >> 4;
    if (pan >= 0)       { c.panL = pan == 7 ? 0x3FF : 0x20 * pan; c.panR = 0; }
    else if (pan >= -7) { c.panL = 0; c.panR = pan == -7 ? 0x3FF : -0x20 * pan; }
    else                { c.panL = c.panR = 0x3FF; }
    c.out1 = (r68 >> 4) & 1;

    c.lfoStep = kLfoSteps[(m_regs[0x80 + ch] >> 3) & 7];
    c.pmDepth = kPmDepth[m_regs[0x80 + ch] & 7];
    c.amDepth = kAmDepth[m_regs[0xE0 + ch] & 7];

    uint32_t sl = m_regs[0xB0 + ch] >> 4;
    sl |= (sl + 1) & 0x10;
    c.sustain = (uint16_t)(sl << 5);

    int32_t corr = m_regs[0xC8 + ch] >> 4;
    corr = corr == 15 ? 0 : (oct + corr) * 2 + (int32_t)((fnum >> 9) & 1);
    c.rate[EG_ATTACK]  = effRate(m_regs[0x98 + ch] >> 4, corr);
    c.rate[EG_DECAY]   = effRate(m_regs[0x98 + ch] & 15, corr);
    c.rate[EG_SUSTAIN] = effRate(m_regs[0xB0 + ch] & 15, corr);
    c.rate[EG_RELEASE] = effRate(m_regs[0xC8 + ch] & 15, corr);
    c.rate[EG_REVERB]  = 5;
    if (r68 & 0x40) {                                         // damp
        c.rate[EG_DECAY] = 48; c.rate[EG_SUSTAIN] = 63; c.rate[EG_RELEASE] = 63;
        c.sustain = 0x80;
    }
}

// ymfm pcm_channel::load_wavetable.
void MoonPcm::loadWave(int ch) {
    Ch& c = m_ch[ch];
    const uint32_t wav = m_regs[0x08 + ch] | ((uint32_t)(m_regs[0x20 + ch] & 1) << 8);
    uint32_t h = 12 * wav;
    if (wav >= 384) {
        const uint32_t bank = (m_regs[0x02] >> 2) & 7;
        if (bank) h = (512u << 10) * bank + (wav - 384) * 12;
    }
    const uint8_t b0 = memRead(h);
    c.format  = b0 >> 6;
    c.base    = ((uint32_t)(b0 & 0x3F) << 16) | ((uint32_t)memRead(h + 1) << 8) | memRead(h + 2);
    c.looppos = (((uint32_t)memRead(h + 3) << 8) | memRead(h + 4)) << 16;
    c.endpos  = (uint32_t)(-(int32_t)(((uint32_t)memRead(h + 5) << 8) | memRead(h + 6))) << 16;
    m_regs[0x80 + ch] = memRead(h + 7);
    m_regs[0x98 + ch] = memRead(h + 8);
    m_regs[0xB0 + ch] = memRead(h + 9);
    m_regs[0xC8 + ch] = memRead(h + 10);
    m_regs[0xE0 + ch] = memRead(h + 11);
    c.env = 0x3FF;
    // Direct pointer when the whole sample sits in one flat block, so fetch()
    // is plain loads instead of memRead's region tests per byte (nullptr =
    // the general path: a paged ROM, or a sample crossing a block edge).
    c.ptr = nullptr;
    if (!m_romPages) {
        const uint32_t n = c.endpos >> 16;
        const uint32_t bytes = (c.format == 0 ? n : c.format == 2 ? n * 2 : (n * 3 + 1) / 2) + 2;
        const uint32_t a = c.base & 0x3FFFFF;
        if (a < m_romSize) {
            if (m_rom && a + bytes <= m_romSize) c.ptr = m_rom + a;
        } else if (m_ram) {
            const uint32_t ra = a & (RAM_SIZE - 1);
            if (ra + bytes <= RAM_SIZE) c.ptr = m_ram + ra;
        }
    }
    recache(ch);
}

void MoonPcm::keyOnOff(int ch, bool on) {
    Ch& c = m_ch[ch];
    if (on == (c.keyOn != 0)) return;
    c.keyOn = on;
    if (on) {
        if (c.eg == EG_ATTACK) return;
        c.eg = EG_ATTACK;
        if (m_regs[0x68 + ch] & 0x20) c.lfo = 0;              // LFO reset
        if (c.rate[EG_ATTACK] == 63) c.env = 0;
        c.pos = 0;
        m_active |= 1u << ch;
    } else if (c.eg < EG_RELEASE) {
        c.eg = EG_RELEASE;
    }
}

// ymfm pcm_channel::clock_envelope.
inline void MoonPcm::clockEnv(Ch& c, int ch, uint32_t counter) {
    if (c.eg == EG_ATTACK && c.env == 0)          c.eg = EG_DECAY;
    if (c.eg == EG_DECAY && c.env >= c.sustain)   c.eg = EG_SUSTAIN;
    const uint32_t rate = c.rate[c.eg];
    const uint32_t rs = rate >> 2;
    counter <<= rs;
    if (counter & 0x7FF) return;
    const uint32_t inc = attInc(rate, (counter >> (rs <= 11 ? 11 : rs)) & 7);
    if (c.eg == EG_ATTACK) {
        // ymfm: env += (~env * inc) >> 4 in int — ~env = -(env+1), so the
        // attenuation only ever falls, and never below 0.
        int32_t e = c.env;
        e += (~e * (int32_t)inc) >> 4;
        c.env = (uint16_t)(e < 0 ? 0 : e);
    } else {
        uint32_t e = c.env + inc;
        if (e >= 0x400) e = 0x3FF;
        c.env = (uint16_t)e;
        if (e >= 0xC0 && c.eg < EG_REVERB && (m_regs[0x38 + ch] & 0x08)) c.eg = EG_REVERB;
    }
}

inline int16_t MoonPcm::fetch(const Ch& c) const {
    const uint32_t p = c.pos >> 16;
    if (const uint8_t* s = c.ptr) {
        if (c.format == 0) return (int16_t)(s[p] << 8);
        if (c.format == 2) { s += p * 2; return (int16_t)((s[0] << 8) | s[1]); }
        s += (p >> 1) * 3;
        if (!(p & 1)) return (int16_t)((s[0] << 8) | ((s[1] << 4) & 0xF0));
        return (int16_t)((s[2] << 8) | (s[1] & 0xF0));
    }
    uint32_t a = c.base;
    if (c.format == 0) return (int16_t)(memRead(a + p) << 8);
    if (c.format == 2) { a += p * 2; return (int16_t)((memRead(a) << 8) | memRead(a + 1)); }
    a += (p >> 1) * 3;
    if (!(p & 1)) return (int16_t)((memRead(a) << 8) | ((memRead(a + 1) << 4) & 0xF0));
    return (int16_t)((memRead(a + 2) << 8) | (memRead(a + 1) & 0xF0));
}

uint32_t MoonPcm::memAutoInc() {
    const uint32_t a = ((uint32_t)(m_regs[3] & 0x3F) << 16) | ((uint32_t)m_regs[4] << 8) | m_regs[5];
    const uint32_t n = a + 1;
    m_regs[5] = (uint8_t)n; m_regs[4] = (uint8_t)(n >> 8); m_regs[3] = (uint8_t)((n >> 16) & 0x3F);
    return a;
}

void MoonPcm::write(uint8_t reg, uint8_t v) {
    if (!m_new2) return;
    if (reg == 6 && (m_regs[2] & 1)) {                        // memory data
        const uint32_t a = memAutoInc() & 0x3FFFFF;
        if (a >= m_romSize) { if (m_ram) m_ram[a & (RAM_SIZE - 1)] = v; }
        else               flashCmd(a, v);                   // the ROM takes flash commands only
        return;
    }
    m_regs[reg] = v;
    if (reg == 0xF8 || reg == 0xF9) { mixRecalc(); return; }
    if (reg < 0x08 || reg >= 0xF8) return;
    const int ch = (reg - 0x08) % CHANNELS;
    if (reg < 0x20) loadWave(ch);
    else            recache(ch);
    if (reg >= 0x68 && reg < 0x80) keyOnOff(ch, (v & 0x80) != 0);
}

// The card's ROM is an AM29F016 (MoonService v0.3 identifies and reflashes it
// through the memory-access registers). Only the command decoder is modelled, for
// autoselect: AA@555 55@2AA 90@555 enters it, F0 leaves it, and reads then give the
// manufacturer (AMD 01h) at A1:A0 = 0, the device (ADh) at 1 and "not protected"
// (00h) at 2. Program (A0) and erase (80, then 10/30) are accepted and dropped: the
// embedded ROM is read-only, so a write reads back unchanged and the toggle-bit
// poll sees "ready" at once. Unlock addresses are A10..A0 of the byte address.
void MoonPcm::flashCmd(uint32_t a, uint8_t v) {
    const uint32_t lo = a & 0x7FF;
    if (v == 0xF0) { m_flashCyc = 0; m_flashId = false; return; }
    switch (m_flashCyc) {
    case 0: m_flashCyc = (lo == 0x555 && v == 0xAA) ? 1 : 0; break;
    case 1: m_flashCyc = (lo == 0x2AA && v == 0x55) ? 2 : 0; break;
    case 2:
        if (lo == 0x555 && v == 0x90)      { m_flashId = true; m_flashCyc = 0; }
        else if (lo == 0x555 && v == 0x80) m_flashCyc = 3;   // erase setup
        else if (lo == 0x555 && v == 0xA0) m_flashCyc = 6;   // program: next write = data
        else                               m_flashCyc = 0;
        break;
    case 3: m_flashCyc = (lo == 0x555 && v == 0xAA) ? 4 : 0; break;
    case 4: m_flashCyc = (lo == 0x2AA && v == 0x55) ? 5 : 0; break;
    default: m_flashCyc = 0; break;                          // 10/30 erase, program data: dropped
    }
}

uint8_t MoonPcm::flashIdByte(uint32_t a) const {
    switch (a & 3) {
    case 0:  return 0x01;    // AMD
    case 1:  return 0xAD;    // AM29F016
    default: return 0x00;    // sector not protected
    }
}

uint8_t MoonPcm::read(uint8_t reg) {
    if (reg == 6 && (m_regs[2] & 1)) {
        const uint32_t a = memAutoInc() & 0x3FFFFF;
        if (m_flashId && a < m_romSize) return flashIdByte(a);
        return memRead(a);
    }
    uint8_t v = m_regs[reg];
    if (reg == 2) v = (v & 0x1F) | 0x20;                      // device ID 001 = YMF278
    return v;
}

// One step of `ratio` (Q16 chip samples) of chip time: the envelope / LFO /
// level ticks it spans and the PCM pair of every active channel summed into
// sl/sr (before the mix level). One body, one call site: see MoonPcm.h.
inline void MoonPcm::chipStep(uint32_t ratio, int32_t& sl, int32_t& sr) {
    {
        m_acc += ratio;
        const uint32_t ticks = m_acc >> 16;
        m_acc &= 0xFFFF;
        const uint32_t c0 = m_envCounter;
        m_envCounter += ticks;

        sl = 0; sr = 0;
        uint32_t act = m_active;
        while (act) {
            const int ch = __builtin_ctz(act);
            act &= act - 1;
            Ch& c = m_ch[ch];
            for (uint32_t t = 1; t <= ticks; t++) {
                c.lfo += c.lfoStep;
                clockEnv(c, ch, (c0 + t) >> 1);
                if (c.tl != c.tlTarget) {                     // ~156 ms full sweep
                    if (c.tl < c.tlTarget) { c.tl += 19; if (c.tl > c.tlTarget) c.tl = c.tlTarget; }
                    else { c.tl = c.tl > c.tlTarget + 38 ? c.tl - 38 : c.tlTarget; }
                }
            }
            uint32_t inc = c.stepOut;                     // step x ratio, cached
            if (c.pmDepth) {
                const uint32_t ls = c.lfo + (1u << 16);
                int32_t lv = (ls >> 10) & 0x7F;
                if (ls & (1u << 17)) lv ^= 0x7F;
                const uint32_t step = c.step + (((lv - 0x40) * (int32_t)c.pmDepth) >> 7);
                inc = (uint32_t)(((uint64_t)step * ratio) >> 16);
            }
            if (c.env <= EG_QUIET && !c.out1) {
                uint32_t e = c.env;
                if (c.amDepth) {
                    uint32_t lv = (c.lfo >> 10) & 0x7F;
                    if (c.lfo & (1u << 17)) lv ^= 0x7F;
                    e += (lv * c.amDepth) >> 7;
                }
                e += c.tl >> 8;
                uint32_t le = e + c.panL, re = e + c.panR;
                if (le > 0x3FF) le = 0x3FF;
                if (re > 0x3FF) re = 0x3FF;
                const int32_t s = fetch(c);
                sl += (attToVol(le << 2) * s) >> 15;
                sr += (attToVol(re << 2) * s) >> 15;
            }
            uint64_t np = (uint64_t)c.pos + inc;
            if (np >= c.endpos) {
                if (c.endpos > c.looppos) {
                    uint64_t over = np - c.endpos, len = c.endpos - c.looppos;
                    if (over >= len) over %= len;
                    np = c.looppos + over;
                } else {
                    np = c.looppos;
                }
            }
            c.pos = (uint32_t)np;
            if (c.eg >= EG_RELEASE && c.env >= EG_QUIET) m_active &= ~(1u << ch);
        }
    }
}

// One output sample of the PCM sum (before the mix level). Half rate: a chip
// step of two output samples every other sample, the midpoint on the compute
// tick, the new pair held on the next (OplFm).
inline void MoonPcm::samplePair(int32_t& sl, int32_t& sr) {
    const bool compute = !m_half || (m_halfTick ^= 1);
    if (compute) {
        chipStep(m_half ? m_ratio2 : m_ratio, sl, sr);
        if (m_half) {
            const int32_t nl = sl, nr = sr;
            sl = (m_cL + nl) >> 1; sr = (m_cR + nr) >> 1;
            m_cL = nl; m_cR = nr;
        }
    } else {
        sl = m_cL; sr = m_cR;
    }
}

void MoonPcm::gen(int16_t* L, int16_t* R, int n, int off) {
    if (!m_active && m_fmFL == 4096 && m_fmFR == 4096) return;
    L += off; R += off;
    for (int i = 0; i < n; i++) {
        int32_t sl, sr;
        samplePair(sl, sr);
        L[i] = clamp16((L[i] * m_fmFL + sl * m_pcmFL) >> 12);
        R[i] = clamp16((R[i] * m_fmFR + sr * m_pcmFR) >> 12);
    }
}

bool MoonPcm::genPcm(int16_t* L, int16_t* R, int n, int off) {
    L += off; R += off;
    if (!m_active) { memset(L, 0, n * sizeof(int16_t)); memset(R, 0, n * sizeof(int16_t)); return false; }
    for (int i = 0; i < n; i++) {
        int32_t sl, sr;
        samplePair(sl, sr);
        L[i] = clamp16((sl * m_pcmFL) >> 12);
        R[i] = clamp16((sr * m_pcmFR) >> 12);
    }
    return true;
}

#endif // MOONSOUND
