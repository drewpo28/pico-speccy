/*

pico-speccy — 2 x SN76489 PSG. See SnSound.h for the conventions implemented
(Sega/VGM-default noise: 16-bit LFSR, feedback bit0^bit3, reset to 0x8000).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include "SnSound.h"

#include <string.h>

SnSound* snChip = nullptr;

// 2 dB per attenuation step, 0x0F = off. Base 48 per channel keeps the worst
// case (8 channels flat out) at 384 — the mixer path saturates at 255 — while
// a typical 3-4 voice tune sits in AY territory.
static const uint8_t sn_vol[16] = {
    48, 38, 30, 24, 19, 15, 12, 10, 8, 6, 5, 4, 3, 2, 2, 0
};

SnSound::SnSound() {
    memset((void*)this, 0, sizeof(*this));
    m_step_q16 = 1u << 16;
    reset();
}

void SnSound::setRates(int clock, int rate) {
    // internal tick = clock/16
    if (rate > 0)
        m_step_q16 = (uint32_t)(((uint64_t)clock << 16) / 16u / (uint32_t)rate);
}

void SnSound::reset() {
    for (int i = 0; i < 2; i++) {
        Chip& c = m_chip[i];
        for (int ch = 0; ch < 4; ch++) {
            c.period[ch] = 0;
            c.att[ch]    = 0x0F;    // power-on: everything off
            c.cnt[ch]    = 0;
            c.level[ch]  = 1;
        }
        c.latch    = 0;
        c.lfsr     = 0x8000;
        c.lfsr_out = 0;
    }
    m_acc_q16  = 0;
    m_last_out = 0;
}

int32_t SnSound::noisePeriod(const Chip& c) const {
    // rate bits: 0/1/2 -> clock/512, /1024, /2048 (= 16/32/64 ticks per
    // flip-flop toggle, the LFSR shifts on every second toggle); 3 -> tone 2.
    switch (c.period[3] & 3) {
    case 0:  return 16;
    case 1:  return 32;
    case 2:  return 64;
    default: {
        int32_t p = c.period[2] & 0x3FF;
        return p < 1 ? 1 : p;
    }
    }
}

void SnSound::writeChip(Chip& c, uint8_t v) {
    if (v & 0x80) {
        c.latch = (v >> 4) & 7;
        int ch = c.latch >> 1;
        if (c.latch & 1) {
            c.att[ch] = v & 0x0F;
        } else if (ch < 3) {
            c.period[ch] = (c.period[ch] & 0x3F0) | (v & 0x0F);
        } else {
            c.period[3] = v & 0x07;
            c.lfsr = 0x8000;        // any noise-register write resets the LFSR
            c.lfsr_out = 0;
        }
    } else {
        int ch = c.latch >> 1;
        if (c.latch & 1) {
            c.att[ch] = v & 0x0F;
        } else if (ch < 3) {
            c.period[ch] = (c.period[ch] & 0x00F) | ((uint16_t)(v & 0x3F) << 4);
        } else {
            c.period[3] = v & 0x07;
            c.lfsr = 0x8000;
            c.lfsr_out = 0;
        }
    }
}

void SnSound::write(int chip, uint8_t v) {
    writeChip(m_chip[chip & 1], v);
}

// one clock/16 step
inline void SnSound::tick(Chip& c) {
    for (int ch = 0; ch < 3; ch++) {
        uint16_t p = c.period[ch] & 0x3FF;
        if (p <= 1) {
            // period 0/1: constant +1 — the chip's PCM mode, and a 55-111 kHz
            // square is DC at our rate anyway
            c.level[ch] = 1;
            c.cnt[ch] = 0;
            continue;
        }
        if (--c.cnt[ch] <= 0) {
            c.cnt[ch] += p;
            c.level[ch] ^= 1;
        }
    }
    // noise: a countdown toggles a flip-flop; the LFSR shifts on 1->0
    if (--c.cnt[3] <= 0) {
        c.cnt[3] += noisePeriod(c);
        c.level[3] ^= 1;
        if (!c.level[3]) {
            uint16_t in = (c.period[3] & 4)
                ? ((c.lfsr ^ (c.lfsr >> 3)) & 1)    // white: bit0 ^ bit3
                : (c.lfsr & 1);                     // periodic: bit0
            c.lfsr = (c.lfsr >> 1) | (in << 15);
            c.lfsr_out = c.lfsr & 1;
        }
    }
}

inline int32_t SnSound::chipOut(const Chip& c) const {
    int32_t out = 0;
    if (c.level[0]) out += sn_vol[c.att[0]];
    if (c.level[1]) out += sn_vol[c.att[1]];
    if (c.level[2]) out += sn_vol[c.att[2]];
    if (c.lfsr_out) out += sn_vol[c.att[3]];
    return out;
}

void SnSound::gen(uint8_t* buf, int count, int bufpos) {
    buf += bufpos;
    for (int i = 0; i < count; i++) {
        m_acc_q16 += m_step_q16;
        int n = (int)(m_acc_q16 >> 16);
        m_acc_q16 &= 0xFFFF;

        if (n > 0) {
            // box-average the ~7 chip ticks inside this output sample
            int32_t sum = 0;
            for (int t = 0; t < n; t++) {
                tick(m_chip[0]);
                tick(m_chip[1]);
                sum += chipOut(m_chip[0]) + chipOut(m_chip[1]);
            }
            sum /= n;
            m_last_out = sum > 255 ? 255 : (uint8_t)sum;
        }
        int v = buf[i] + m_last_out;
        buf[i] = v > 255 ? 255 : (uint8_t)v;
    }
}
