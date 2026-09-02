/*
 * opllfm_test.cpp — host-side validation for src/OpllFm.cpp (YM2413/OPLL).
 *
 *   g++ -O2 -Isrc -o /tmp/opllfm_test tools/opllfm_test.cpp src/OpllFm.cpp && /tmp/opllfm_test
 *
 * Re-run after ANY change there — an FM core fails quietly and by degrees.
 * Checks: a ROM-instrument note at the demanded frequency, key-off decay to
 * exact silence, the user patch, rhythm mode (bass drum), and half-rate
 * pitch equality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "OpllFm.h"

#define RATE 31250

static OpllFm chip;
static int16_t buf[RATE];

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  OK   " __VA_ARGS__); printf("\n"); } \
    else      { printf("  FAIL " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static void wr(int r, int v) { chip.writeAddr((uint8_t)r); chip.writeData((uint8_t)v); }

static void gen(int n) {
    if (n > RATE) n = RATE;
    memset(buf, 0, n * sizeof(int16_t));
    chip.gen(buf, n, 0);
}

static double measure_freq(const int16_t* b, int n) {
    int start = n / 4, crossings = 0, first = -1, last = -1;
    for (int i = start + 1; i < n; i++) {
        if (b[i - 1] <= 0 && b[i] > 0) {
            crossings++;
            if (first < 0) first = i;
            last = i;
        }
    }
    if (crossings < 2) return 0.0;
    return (double)(crossings - 1) * RATE / (last - first);
}

static int peak_of(const int16_t* b, int from, int n) {
    int pk = 0;
    for (int i = from; i < n; i++) if (abs(b[i]) > pk) pk = abs(b[i]);
    return pk;
}

int main() {
    chip.setRates(OPLL_YM2413_CLOCK, RATE);
    chip.reset();

    printf("[1] silence after reset\n");
    gen(1000);
    int nz = 0; for (int i = 0; i < 1000; i++) if (buf[i]) nz++;
    CHECK(nz == 0, "%d nonzero samples", nz);

    printf("[2] ROM instrument note (violin, fnum=290 block=4 -> 440.0 Hz)\n");
    wr(0x30, 0x10);                    // ch0: instrument 1, volume 0
    wr(0x10, 290 & 0xFF);              // fnum low
    wr(0x20, 0x10 | (4 << 1) | 1);     // key on, block 4, fnum MSB
    gen(RATE);
    // a real FM timbre crosses zero more than twice per period — measure the
    // fundamental by autocorrelation instead
    double best = 0; int bestlag = 0;
    for (int lag = 40; lag <= 200; lag++) {
        double acc = 0;
        for (int i = RATE / 2; i < RATE - 200; i++)
            acc += (double)buf[i] * buf[i + lag];
        if (acc > best) { best = acc; bestlag = lag; }
    }
    double f = bestlag ? (double)RATE / bestlag : 0;
    CHECK(fabs(f - 440.0) < 8.0, "autocorrelation %.1f Hz (lag %d)", f, bestlag);
    int pk = peak_of(buf, RATE / 4, RATE);
    CHECK(pk > 1500, "peak %d (> 1500)", pk);

    printf("[3] key-off decays to exact silence\n");
    wr(0x20, (4 << 1) | 1);            // key off
    for (int p = 0; p < 40; p++) gen(RATE / 10);
    gen(RATE / 10);
    nz = 0; for (int i = 0; i < RATE / 10; i++) if (buf[i]) nz++;
    CHECK(nz == 0, "%d nonzero samples in the last 100 ms", nz);

    printf("[3b] audible() clears after decay (parked-modulator DC bug)\n");
    CHECK(!chip.audible(), "audible() = %d after 4 s of silence (want 0)", (int)chip.audible());

    printf("[4] user patch (instrument 0)\n");
    chip.reset();
    wr(0x00, 0x21); wr(0x01, 0x21);    // mod+car: MULT=1, sustained
    wr(0x02, 0x3f);                    // mod TL max (carrier-only tone)
    wr(0x03, 0x00);                    // no feedback, sine waves
    wr(0x04, 0xf0); wr(0x05, 0xf0);    // AR=15 DR=0
    wr(0x06, 0x0f); wr(0x07, 0x0f);    // SL=0 RR=15
    wr(0x30, 0x00);                    // ch0: instrument 0 (user), volume 0
    wr(0x10, 290 & 0xFF);
    wr(0x20, 0x10 | (4 << 1) | 1);
    gen(RATE / 2);
    f = measure_freq(buf, RATE / 2);
    CHECK(fabs(f - 440.0) < 3.0, "user-patch tone %.1f Hz", f);
    wr(0x20, (4 << 1) | 1);

    printf("[5] rhythm mode: bass drum\n");
    chip.reset();
    wr(0x16, 0x20); wr(0x26, 0x05);    // standard rhythm fnums (SMS BIOS values)
    wr(0x17, 0x50); wr(0x27, 0x05);
    wr(0x18, 0xC0); wr(0x28, 0x01);
    wr(0x36, 0x00);                    // BD volume max
    wr(0x0e, 0x20 | 0x10);             // rhythm mode + BD key on
    gen(RATE / 8);
    pk = peak_of(buf, 0, RATE / 8);
    CHECK(pk > 500, "bass drum peak %d", pk);

    printf("[6] half-rate mode: pitch exact\n");
    chip.setRates(OPLL_YM2413_CLOCK, RATE, true);
    chip.reset();
    wr(0x30, 0x10);
    wr(0x10, 290 & 0xFF);
    wr(0x20, 0x10 | (4 << 1) | 1);
    gen(RATE);
    double fh = measure_freq(buf, RATE);
    CHECK(fabs(fh - 440.0) < 3.5, "half-rate tone %.1f Hz", fh);
    chip.setRates(OPLL_YM2413_CLOCK, RATE, false);

    printf(fails ? "\n%d FAILURES\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
