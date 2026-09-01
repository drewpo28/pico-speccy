/*
 * oplfm_test.cpp — host-side validation for src/OplFm.cpp (YMF262/OPL3 core).
 *
 * Build & run (OplFm.cpp has no project dependencies):
 *   g++ -O2 -Isrc -o /tmp/oplfm_test tools/oplfm_test.cpp src/OplFm.cpp && /tmp/oplfm_test
 *
 * Re-run after ANY change to OplFm.cpp — an FM core fails quietly and by
 * degrees. Checks:
 *   1. the AlexZor VGM plugin's detect sequence answers status 0xC0 (=192)
 *   2. a programmed 2-op tone comes out at the demanded frequency
 *   3. key-off decays to exact digital silence
 *   4. timer 1 / timer 2 overflow rates
 *   5. OPL3 mode: a bank-2 channel plays, pan bits route L/R
 *   6. rhythm mode (bass drum) produces output
 *   7. waveform 2 (abs sine) never goes negative
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "OplFm.h"

#define RATE 31250

static OplFm chip;
static int16_t bufL[RATE], bufR[RATE];

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  OK   " __VA_ARGS__); printf("\n"); } \
    else      { printf("  FAIL " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static void wr(int a, int v) { chip.write(a, (uint8_t)v); }
static void wreg(int r, int v) { wr(0, r); wr(1, v); }         // set #1
static void wreg2(int r, int v) { wr(2, r); wr(3, v); }        // set #2

static void gen(int n) {
    if (n > RATE) n = RATE;
    memset(bufL, 0, n * sizeof(int16_t));
    memset(bufR, 0, n * sizeof(int16_t));
    chip.gen(bufL, bufR, n, 0);
}

// program a plain 2-op tone on a channel of set #1: op1 muted, op2 carrier
static void tone(int ch, int fnum, int block, int wave2) {
    static const int op1_off[9] = { 0,1,2, 8,9,10, 16,17,18 };
    int o1 = op1_off[ch], o2 = o1 + 3;
    wreg(0x20 + o1, 0x21);            // op1: mul=1, sustain
    wreg(0x20 + o2, 0x21);            // op2: mul=1, sustain
    wreg(0x40 + o1, 0x3f);            // op1 TL = max attenuation
    wreg(0x40 + o2, 0x00);            // op2 TL = 0 dB
    wreg(0x60 + o1, 0xf0);            // AR=15 DR=0
    wreg(0x60 + o2, 0xf0);
    wreg(0x80 + o1, 0x0f);            // SL=0 RR=15
    wreg(0x80 + o2, 0x0f);
    wreg(0xe0 + o2, wave2);
    wreg(0xc0 + ch, 0x31);            // L+R on, FB=0, CON=1 (additive: op2 alone sounds)
    wreg(0xa0 + ch, fnum & 0xff);
    wreg(0xb0 + ch, 0x20 | (block << 2) | (fnum >> 8));
}

static double measure_freq(const int16_t* buf, int n) {
    // count positive-going zero crossings over the last 3/4 (skip attack)
    int start = n / 4, crossings = 0, first = -1, last = -1;
    for (int i = start + 1; i < n; i++) {
        if (buf[i - 1] <= 0 && buf[i] > 0) {
            crossings++;
            if (first < 0) first = i;
            last = i;
        }
    }
    if (crossings < 2) return 0.0;
    return (double)(crossings - 1) * RATE / (last - first);
}

int main() {
    chip.setRates(OPL3_YMF262_CLOCK, RATE);
    chip.reset();

    printf("[1] VGM plugin detect sequence\n");
    CHECK(chip.status() == 0x00, "status after reset = 0x%02X (want 0x00)", chip.status());
    wreg(0x04, 0x60);                  // mask both timers
    wreg(0x02, 0xff);                  // timer 1 preset = 0xFF (1 tick = 80.4 us)
    wreg(0x04, 0x21);                  // unmask+start T1, T2 masked
    gen(30);                           // the plugin waits ~950 us; 30 samples = 960 us
    CHECK(chip.status() == 0xC0, "status after T1 start + wait = 0x%02X (want 0xC0)", chip.status());
    wreg(0x04, 0x80);                  // IRQ reset
    CHECK(chip.status() == 0x00, "status after IRQRST = 0x%02X (want 0x00)", chip.status());
    wreg(0x04, 0x00);                  // stop timers

    printf("[2] tone frequency (fnum=580 block=4 -> 440.0 Hz)\n");
    chip.reset();
    tone(0, 580, 4, 0);
    gen(RATE);
    double f = measure_freq(bufL, RATE);
    CHECK(fabs(f - 440.0) < 2.0, "measured %.1f Hz", f);
    int16_t peak = 0;
    for (int i = RATE / 4; i < RATE; i++) if (abs(bufL[i]) > peak) peak = abs(bufL[i]);
    CHECK(peak > 1000, "peak amplitude %d (> 1000)", peak);

    printf("[3] key-off decays to silence\n");
    wreg(0xb0 + 0, (4 << 2) | (580 >> 8));   // key off
    for (int pass = 0; pass < 40; pass++) gen(RATE / 10);
    gen(RATE / 10);
    int nonzero = 0;
    for (int i = 0; i < RATE / 10; i++) if (bufL[i] != 0) nonzero++;
    CHECK(nonzero == 0, "%d nonzero samples in the last 100 ms", nonzero);

    printf("[4] timer rates\n");
    chip.reset();
    // T1: v=0 -> period 256*4 chip samples = 1024/49716 s -> 48.55 Hz
    wreg(0x02, 0x00);
    wreg(0x04, 0x21);
    int overs = 0;
    for (int i = 0; i < RATE; i++) {           // one second, sample by sample
        gen(1);
        if (chip.status() & 0x40) { overs++; wreg(0x04, 0x80); }
    }
    CHECK(abs(overs - 49) <= 1, "timer1 %d overflows/s (want ~48.6)", overs);
    wreg(0x04, 0x00);
    // T2: v=0 -> period 256*16 chip samples -> 12.14 Hz
    wreg(0x03, 0x00);
    wreg(0x04, 0x12);                          // unmask+start T2 (T1 masked)
    overs = 0;
    for (int i = 0; i < RATE; i++) {
        gen(1);
        if (chip.status() & 0x20) { overs++; wreg(0x04, 0x80); }
    }
    CHECK(abs(overs - 12) <= 1, "timer2 %d overflows/s (want ~12.1)", overs);
    wreg(0x04, 0x00);

    printf("[5] OPL3 mode: bank-2 channel + panning\n");
    chip.reset();
    wreg2(0x05, 0x01);                 // OPL3 mode on
    // channel 9 (bank 2, offsets are the same as bank 1)
    wreg2(0x20 + 0, 0x21);
    wreg2(0x20 + 3, 0x21);
    wreg2(0x40 + 0, 0x3f);
    wreg2(0x40 + 3, 0x00);
    wreg2(0x60 + 0, 0xf0);
    wreg2(0x60 + 3, 0xf0);
    wreg2(0x80 + 0, 0x0f);
    wreg2(0x80 + 3, 0x0f);
    wreg2(0xc0 + 0, 0x11);             // LEFT only, CON=1
    wreg2(0xa0 + 0, 580 & 0xff);
    wreg2(0xb0 + 0, 0x20 | (4 << 2) | (580 >> 8));
    gen(RATE / 4);
    int16_t peakL = 0, peakR = 0;
    for (int i = RATE / 16; i < RATE / 4; i++) {
        if (abs(bufL[i]) > peakL) peakL = abs(bufL[i]);
        if (abs(bufR[i]) > peakR) peakR = abs(bufR[i]);
    }
    CHECK(peakL > 1000 && peakR == 0, "L-only pan: peakL=%d peakR=%d", peakL, peakR);
    wreg2(0xc0 + 0, 0x21);             // RIGHT only
    gen(RATE / 4);
    peakL = peakR = 0;
    for (int i = RATE / 16; i < RATE / 4; i++) {
        if (abs(bufL[i]) > peakL) peakL = abs(bufL[i]);
        if (abs(bufR[i]) > peakR) peakR = abs(bufR[i]);
    }
    CHECK(peakR > 1000 && peakL == 0, "R-only pan: peakL=%d peakR=%d", peakL, peakR);
    double f9 = measure_freq(bufR, RATE / 4);
    CHECK(fabs(f9 - 440.0) < 3.0, "bank-2 tone %.1f Hz", f9);

    printf("[6] rhythm mode: bass drum\n");
    chip.reset();
    // BD voice: ch6 = ops 12 (0x30) and 15 (0x33)
    wreg(0x20 + 16, 0x01);             // op "12": reg offset 0x10
    wreg(0x20 + 19, 0x01);             // op "15": reg offset 0x13
    wreg(0x40 + 16, 0x00);
    wreg(0x40 + 19, 0x00);
    wreg(0x60 + 16, 0xf4);
    wreg(0x60 + 19, 0xf4);
    wreg(0x80 + 16, 0x2f);             // some decay
    wreg(0x80 + 19, 0x2f);
    wreg(0xc0 + 6, 0x30);
    wreg(0xa0 + 6, 0x40);
    wreg(0xb0 + 6, (3 << 2) | 0x01);   // frequency, NO key-on bit (rhythm keys it)
    wreg(0xbd, 0x30);                  // rhythm mode + BD key on
    gen(RATE / 8);
    peak = 0;
    for (int i = 0; i < RATE / 8; i++) if (abs(bufL[i]) > peak) peak = abs(bufL[i]);
    CHECK(peak > 500, "bass drum peak %d", peak);

    printf("[7] waveform 2 (abs sine) never negative\n");
    chip.reset();
    wreg2(0x05, 0x01);                 // 4-waveform select needs OPL3 (or OPL2 test bit)
    tone(0, 580, 4, 2);
    gen(RATE / 4);
    int neg = 0;
    for (int i = RATE / 16; i < RATE / 4; i++) if (bufL[i] < 0) neg++;
    CHECK(neg == 0, "%d negative samples", neg);

    printf(fails ? "\n%d FAILURES\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
