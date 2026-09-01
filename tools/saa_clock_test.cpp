/*
 * saa_clock_test.cpp — validates SAASound's Q16 master-clock scale
 * (SAASound::set_clock): at the default 8 MHz the generator is bit-identical
 * to the pre-clock-aware code; at the CMS/Game Blaster 7.159090 MHz every
 * frequency scales by exactly clock/8MHz.
 *
 * SAASound.h drags in ESPectrum.h (quoted include, so src/ always wins), so
 * this builds against COPIES next to stub headers:
 *
 *   D=$(mktemp -d)
 *   cp src/SAASound.h src/SAASound.cpp tools/saa_clock_test.cpp "$D"
 *   printf '#pragma once\n' > "$D"/hardconfig.h
 *   printf '#pragma once\n' > "$D"/Config.h
 *   printf '#pragma once\n#define ESP_AUDIO_SAMPLES_PENTAGON 640\n' > "$D"/ESPectrum.h
 *   g++ -O2 -DIRAM_ATTR= -I"$D" -o /tmp/saa_clock_test "$D"/saa_clock_test.cpp "$D"/SAASound.cpp
 *   /tmp/saa_clock_test
 *
 * Re-run after any change to the SAASound tone/noise tick or set_clock.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "SAASound.h"

#define RATE 31250

static SAASound saa;

static void wr(int reg, int val) { saa.selectRegister(reg); saa.setRegisterData(val); }

static double tone_freq() {
    // ch0: freq offset 32, octave 4; enable, amplitude max, tone-only mix
    wr(0x1C, 0x01);           // sound enable
    wr(0x08, 32);             // ch0 frequency offset
    wr(0x10, 0x04);           // ch0 octave = 4
    wr(0x00, 0xFF);           // ch0 amplitude L/R max
    wr(0x14, 0x01);           // tone enable ch0
    wr(0x15, 0x00);           // noise off
    for (int p = 0; p < 3; p++) saa.gen_sound(640, 0);   // settle
    static uint8_t l[RATE];
    int total = 0;
    while (total < RATE) {
        int n = RATE - total > 640 ? 640 : RATE - total;
        saa.gen_sound(n, 0);
        memcpy(l + total, saa.SamplebufSAA_L, n);
        total += n;
    }
    int lo = 255, hi = 0;
    for (int i = 0; i < RATE; i++) { if (l[i] < lo) lo = l[i]; if (l[i] > hi) hi = l[i]; }
    int mid = (lo + hi) / 2, cr = 0, first = -1, last = -1;
    for (int i = 1; i < RATE; i++)
        if (l[i - 1] <= mid && l[i] > mid) { cr++; if (first < 0) first = i; last = i; }
    return cr >= 2 ? (double)(cr - 1) * RATE / (last - first) : 0.0;
}

int main() {
    saa.init();
    double f8 = tone_freq();
    saa.init();
    saa.set_clock(7159090);
    double f7 = tone_freq();
    // expected at 8 MHz: 15625 * 2^4 / (511-32) = 521.9 Hz
    double want8 = 15625.0 * 16 / 479, ratio = f7 / f8, wantr = 7159090.0 / 8000000.0;
    printf("8 MHz: %.1f Hz (want %.1f)\n7.159 MHz: %.1f Hz, ratio %.4f (want %.4f)\n",
           f8, want8, f7, ratio, wantr);
    int ok = fabs(f8 - want8) < 3.0 && fabs(ratio - wantr) < 0.004;
    printf(ok ? "PASS\n" : "FAIL\n");
    return !ok;
}
