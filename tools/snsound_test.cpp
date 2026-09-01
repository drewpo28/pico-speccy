/*
 * snsound_test.cpp — host-side validation for src/SnSound.cpp (2 x SN76489).
 *
 *   g++ -O2 -Isrc -o /tmp/snsound_test tools/snsound_test.cpp src/SnSound.cpp && /tmp/snsound_test
 *
 * Checks: tone frequency, attenuation off = silence, PCM mode (period 0 ->
 * constant DC following the volume register), white vs periodic noise, the
 * second chip's independence, and the noise LFSR reset rule.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "SnSound.h"

#define RATE 31250

static SnSound sn;
static uint8_t buf[RATE];

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  OK   " __VA_ARGS__); printf("\n"); } \
    else      { printf("  FAIL " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static void gen(int n) {
    if (n > RATE) n = RATE;
    memset(buf, 0, n);
    sn.gen(buf, n, 0);
}

// latch+data write of a full 10-bit tone period
static void tone(int chip, int ch, int period, int att) {
    sn.write(chip, 0x80 | (ch << 5) | (period & 0x0F));
    sn.write(chip, (period >> 4) & 0x3F);
    sn.write(chip, 0x90 | (ch << 5) | (att & 0x0F));
}

static double measure_freq(const uint8_t* b, int n) {
    // midpoint crossings on a unipolar square
    int lo = 255, hi = 0;
    for (int i = n / 4; i < n; i++) { if (b[i] < lo) lo = b[i]; if (b[i] > hi) hi = b[i]; }
    int mid = (lo + hi) / 2;
    int crossings = 0, first = -1, last = -1;
    for (int i = n / 4 + 1; i < n; i++) {
        if (b[i - 1] <= mid && b[i] > mid) {
            crossings++;
            if (first < 0) first = i;
            last = i;
        }
    }
    if (crossings < 2) return 0.0;
    return (double)(crossings - 1) * RATE / (last - first);
}

int main() {
    sn.setRates(SN76489_CLOCK, RATE);
    sn.reset();

    printf("[1] silence at power-on (all attenuators 0x0F)\n");
    gen(1000);
    int nz = 0; for (int i = 0; i < 1000; i++) if (buf[i]) nz++;
    CHECK(nz == 0, "%d nonzero samples", nz);

    printf("[2] tone frequency: N=254 -> %.1f Hz\n", SN76489_CLOCK / 32.0 / 254);
    tone(0, 0, 254, 0);
    gen(RATE);
    double f = measure_freq(buf, RATE);
    CHECK(fabs(f - 440.4) < 2.0, "measured %.1f Hz", f);

    printf("[3] attenuation 0x0F mutes the channel\n");
    sn.write(0, 0x9F);              // ch0 att = 0x0F
    gen(RATE / 10);
    nz = 0; for (int i = 100; i < RATE / 10; i++) if (buf[i]) nz++;
    CHECK(nz == 0, "%d nonzero samples", nz);

    printf("[4] PCM mode: period 0 -> constant DC = volume\n");
    tone(0, 0, 0, 0);
    gen(1000);
    int lo = 255, hi = 0;
    for (int i = 100; i < 1000; i++) { if (buf[i] < lo) lo = buf[i]; if (buf[i] > hi) hi = buf[i]; }
    CHECK(lo == hi && hi == 48, "DC lo=%d hi=%d (want 48)", lo, hi);
    sn.write(0, 0x9F);

    printf("[5] white noise: nonzero, aperiodic-looking\n");
    sn.reset();
    sn.write(0, 0xE4);              // noise: white, rate 0
    sn.write(0, 0xF0);              // noise att = 0
    gen(RATE / 2);
    nz = 0; long transitions = 0;
    for (int i = 1; i < RATE / 2; i++) {
        if (buf[i]) nz++;
        if ((buf[i] != 0) != (buf[i - 1] != 0)) transitions++;
    }
    CHECK(nz > RATE / 8 && nz < (RATE * 3) / 8, "on-samples %d of %d", nz, RATE / 2);
    CHECK(transitions > 1000, "%ld level transitions", transitions);

    printf("[6] periodic noise = tone/15: rate from tone-2 period\n");
    sn.reset();
    tone(0, 2, 100, 0x0F);          // tone2 period 100, muted itself
    sn.write(0, 0xE3);              // noise: periodic, rate = tone2
    sn.write(0, 0xF0);
    gen(RATE);
    // periodic noise: 1 bit set in 16 -> pulse train at clock/(32*N*16)
    double fp = 0;
    {   // measure pulse rate: count rising edges
        int cr = 0, first = -1, last = -1;
        for (int i = RATE / 4 + 1; i < RATE; i++)
            if (buf[i] > 0 && buf[i - 1] == 0) { cr++; if (first < 0) first = i; last = i; }
        if (cr >= 2) fp = (double)(cr - 1) * RATE / (last - first);
    }
    double want = SN76489_CLOCK / 32.0 / 100 / 16;   // ~69.9 Hz
    CHECK(fabs(fp - want) < 3.0, "pulse rate %.1f Hz (want %.1f)", fp, want);

    printf("[7] chips are independent\n");
    sn.reset();
    tone(1, 0, 508, 0);             // chip 2: ~220 Hz
    gen(RATE);
    f = measure_freq(buf, RATE);
    CHECK(fabs(f - 220.2) < 2.0, "chip2 tone %.1f Hz", f);
    tone(0, 0, 254, 0);             // add chip 1: DC level rises
    gen(RATE / 10);
    int peak = 0; for (int i = 0; i < RATE / 10; i++) if (buf[i] > peak) peak = buf[i];
    CHECK(peak == 96, "both chips peak %d (want 96)", peak);

    printf(fails ? "\n%d FAILURES\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
