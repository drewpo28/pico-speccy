// Host-side test harness for src/OpnFm.cpp (the TurboSound FM / YM2203 OPN core).
// An FM core fails quietly and by degrees, so run this after ANY change to it.
//
//   g++ -O2 -Isrc -o /tmp/opnfm_test tools/opnfm_test.cpp src/OpnFm.cpp && /tmp/opnfm_test
//
// It links against a local Debug::log stub (OpnFm.cpp's only project dependency),
// so nothing else of the firmware has to build. Expected output:
//   A 440.1 Hz   B 0   C/F fundamental 440.1 Hz   D 48 (48.1)   E 12 (12.0)
//   G nonzero and bounded   H 440.1 Hz
// I is a throughput figure, not a pass/fail.

#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <ctime>
#include "OpnFm.h"

// stub for Debug::log
struct Debug { static void log(const char* fmt, ...); };
void Debug::log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n");
}

static const int RATE = 31250;

static void wr(OpnFm& c, uint8_t r, uint8_t v) { c.writeAddr(r); c.writeData(v); }

// dominant frequency by zero-crossing count on a mono int16 buffer
static double domFreq(const int16_t* b, int n, int rate) {
    int cross = 0;
    for (int i = 1; i < n; i++) if ((b[i-1] < 0) != (b[i] < 0)) cross++;
    return (double)cross * rate / (2.0 * n);
}

int main() {
    OpnFm chip;
    if (!OpnFm::tablesReady()) { printf("tables FAILED\n"); return 1; }
    chip.setRates(TSFM_YM2203_CLOCK, RATE);
    chip.reset();
    printf("clock=%d rate=%d  (FM rate = clock/72 = %.1f Hz)\n",
           TSFM_YM2203_CLOCK, RATE, TSFM_YM2203_CLOCK / 72.0);

    // Manual §4.6 test program, channel 1 (registers ...0), algorithm 7 replaced
    // by the manual's own B0=0x32 patch further down; first a clean sine:
    // algorithm 7 = all four operators straight to the output.
    for (int op = 0; op < 4; op++) {
        wr(chip, 0x30 + op*4, 0x01);   // DT=0 MUL=1
        wr(chip, 0x40 + op*4, 0x00);   // TL=0 (max level)
        wr(chip, 0x50 + op*4, 0x1f);   // AR=31
        wr(chip, 0x60 + op*4, 0x00);   // DR=0
        wr(chip, 0x70 + op*4, 0x00);   // SR=0
        wr(chip, 0x80 + op*4, 0x0f);   // SL=0 RR=15
        wr(chip, 0x90 + op*4, 0x00);   // no SSG-EG
    }
    wr(chip, 0xb0, 0x07);              // FB=0 ALGO=7

    // A4 (block+fnum high) then A0 (fnum low), as the manual insists.
    // fnum for 440 Hz: fnum = 440 * 2^20 / (clock/72) / 2^(blk-1), blk=4
    const double fmrate = TSFM_YM2203_CLOCK / 72.0;
    int blk = 4;
    int fnum = (int)lround(440.0 * (1 << 20) / fmrate / (1 << (blk - 1)));
    printf("fnum=%d blk=%d\n", fnum, blk);
    wr(chip, 0xa4, (blk << 3) | (fnum >> 8));
    wr(chip, 0xa0, fnum & 0xff);
    wr(chip, 0x28, 0xf0);              // key on all 4 operators, channel 1

    std::vector<int16_t> buf(RATE);    // one second
    std::fill(buf.begin(), buf.end(), 0); chip.gen(buf.data(), RATE, 0);

    int peak = 0; long long energy = 0;
    for (int i = 0; i < RATE; i++) { int a = abs(buf[i]); if (a > peak) peak = a; energy += (long long)buf[i]*buf[i]; }
    printf("A: alg7 sustained  peak=%d rms=%.1f freq=%.1f Hz (want 440)\n",
           peak, sqrt((double)energy / RATE), domFreq(buf.data() + 1000, RATE - 1000, RATE));

    // B: key off -> must decay to silence with RR=15
    wr(chip, 0x28, 0x00);
    std::fill(buf.begin(), buf.end(), 0); chip.gen(buf.data(), RATE, 0);
    peak = 0; for (int i = RATE/2; i < RATE; i++) { int a = abs(buf[i]); if (a > peak) peak = a; }
    printf("B: after key off, peak over the 2nd half = %d (want 0)\n", peak);

    // C: an actual 2-op FM patch (algorithm 0: op1 modulates ... chain), TL of the
    // modulators nonzero, to exercise op_calc's phase modulation path.
    chip.reset();
    for (int op = 0; op < 4; op++) {
        wr(chip, 0x30 + op*4, 0x01);
        wr(chip, 0x40 + op*4, op == 3 ? 0x00 : 0x20);
        wr(chip, 0x50 + op*4, 0x1f);
        wr(chip, 0x60 + op*4, 0x05);
        wr(chip, 0x70 + op*4, 0x02);
        wr(chip, 0x80 + op*4, 0x11);
        wr(chip, 0x90 + op*4, 0x00);
    }
    wr(chip, 0xb0, 0x32);              // the manual's FB/ALGO
    wr(chip, 0xa4, (blk << 3) | (fnum >> 8));
    wr(chip, 0xa0, fnum & 0xff);
    wr(chip, 0x28, 0xf0);
    std::fill(buf.begin(), buf.end(), 0); chip.gen(buf.data(), RATE, 0);
    peak = 0; energy = 0;
    for (int i = 0; i < RATE; i++) { int a = abs(buf[i]); if (a > peak) peak = a; energy += (long long)buf[i]*buf[i]; }
    printf("C: alg2 FM patch   peak=%d rms=%.1f freq=%.1f Hz (want ~440)\n",
           peak, sqrt((double)energy / RATE), domFreq(buf.data(), RATE/4, RATE));

    // D: timers. Timer A period = 72*(1024-TA)/clock; load and count overflows.
    chip.reset();
    int TA = 0;                        // longest: 1024 ticks
    wr(chip, 0x24, TA >> 2);
    wr(chip, 0x25, TA & 3);
    wr(chip, 0x27, 0x05);              // enable A + load A
    int overflows = 0;
    for (int s = 0; s < RATE; s++) {
        int16_t one; one = 0;
        chip.gen(&one, 1, 0);
        if (chip.status() & 1) { overflows++; wr(chip, 0x27, 0x15); wr(chip, 0x27, 0x05); }
    }
    double want = 1.0 / (72.0 * (1024 - TA) / TSFM_YM2203_CLOCK);
    printf("D: timer A overflows/s = %d (want %.1f)\n", overflows, want);

    // E: timer B period = 16*72*(256-TB)/clock
    chip.reset();
    int TB = 0;
    wr(chip, 0x26, TB);
    wr(chip, 0x27, 0x0a);              // enable B + load B
    overflows = 0;
    for (int s = 0; s < RATE; s++) {
        int16_t one = 0;
        chip.gen(&one, 1, 0);
        if (chip.status() & 2) { overflows++; wr(chip, 0x27, 0x2a); wr(chip, 0x27, 0x0a); }
    }
    want = 1.0 / (16.0 * 72.0 * (256 - TB) / TSFM_YM2203_CLOCK);
    printf("E: timer B overflows/s = %d (want %.1f)\n", overflows, want);

    // F: fundamental of the C patch by autocorrelation (the zero-cross estimate
    // counts harmonics, and an FM patch is full of them).
    {
        OpnFm c2; c2.setRates(TSFM_YM2203_CLOCK, RATE); c2.reset();
        for (int op = 0; op < 4; op++) {
            wr(c2, 0x30 + op*4, 0x01); wr(c2, 0x40 + op*4, op == 3 ? 0x00 : 0x20);
            wr(c2, 0x50 + op*4, 0x1f); wr(c2, 0x60 + op*4, 0x05);
            wr(c2, 0x70 + op*4, 0x02); wr(c2, 0x80 + op*4, 0x11); wr(c2, 0x90 + op*4, 0x00);
        }
        wr(c2, 0xb0, 0x32);
        wr(c2, 0xa4, (blk << 3) | (fnum >> 8)); wr(c2, 0xa0, fnum & 0xff);
        wr(c2, 0x28, 0xf0);
        std::fill(buf.begin(), buf.end(), 0); c2.gen(buf.data(), RATE, 0);
        int best = 0; double bestv = -1e18;
        for (int lag = 20; lag < 400; lag++) {
            double acc = 0; for (int i = 2000; i < 12000; i++) acc += (double)buf[i]*buf[i+lag];
            if (acc > bestv) { bestv = acc; best = lag; }
        }
        printf("F: autocorrelation fundamental = %.1f Hz (want ~440)\n", (double)RATE/best);
    }

    // G: SSG-EG on every operator must stay bounded and keep making sound.
    {
        OpnFm c3; c3.setRates(TSFM_YM2203_CLOCK, RATE); c3.reset();
        for (int op = 0; op < 4; op++) {
            wr(c3, 0x30 + op*4, 0x01); wr(c3, 0x40 + op*4, 0x00);
            wr(c3, 0x50 + op*4, 0x1f); wr(c3, 0x60 + op*4, 0x1a);
            wr(c3, 0x70 + op*4, 0x18); wr(c3, 0x80 + op*4, 0x0f);
            wr(c3, 0x90 + op*4, 0x08);          // SSG-EG, repeating decay
        }
        wr(c3, 0xb0, 0x07);
        wr(c3, 0xa4, (blk << 3) | (fnum >> 8)); wr(c3, 0xa0, fnum & 0xff);
        wr(c3, 0x28, 0xf0);
        std::fill(buf.begin(), buf.end(), 0); c3.gen(buf.data(), RATE, 0);
        int pk = 0; long long en = 0;
        for (int i = 0; i < RATE; i++) { int a = abs(buf[i]); if (a > pk) pk = a; en += (long long)buf[i]*buf[i]; }
        printf("G: SSG-EG        peak=%d rms=%.1f (want nonzero, bounded)\n", pk, sqrt((double)en/RATE));
    }

    // H: channel 3 special mode — operator frequencies set independently.
    {
        OpnFm c4; c4.setRates(TSFM_YM2203_CLOCK, RATE); c4.reset();
        for (int op = 0; op < 4; op++) {
            wr(c4, 0x32 + op*4, 0x01); wr(c4, 0x42 + op*4, 0x00);
            wr(c4, 0x52 + op*4, 0x1f); wr(c4, 0x62 + op*4, 0x00);
            wr(c4, 0x72 + op*4, 0x00); wr(c4, 0x82 + op*4, 0x0f); wr(c4, 0x92 + op*4, 0x00);
        }
        wr(c4, 0xb2, 0x07);
        wr(c4, 0x27, 0x40);                      // 3-slot mode
        wr(c4, 0xa6, (blk << 3) | (fnum >> 8));  // ch3 operator 4 = the channel freq
        wr(c4, 0xa2, fnum & 0xff);
        for (int i = 0; i < 3; i++) {            // the three per-operator slots
            wr(c4, 0xac + i, (blk << 3) | (fnum >> 8));
            wr(c4, 0xa8 + i, fnum & 0xff);
        }
        wr(c4, 0x28, 0xf2);
        std::fill(buf.begin(), buf.end(), 0); c4.gen(buf.data(), RATE, 0);
        int pk = 0; for (int i = 0; i < RATE; i++) { int a = abs(buf[i]); if (a > pk) pk = a; }
        printf("H: ch3 3-slot    peak=%d freq=%.1f Hz (want ~440)\n",
               pk, domFreq(buf.data() + 1000, RATE - 1000, RATE));
    }
    // I: throughput, for a feel of the RP2350 budget (two chips, all 3 channels
    // keyed, worst case: every operator audible so no ENV_QUIET early-out fires).
    {
        OpnFm a, b;
        for (OpnFm* c : {&a, &b}) {
            c->setRates(TSFM_YM2203_CLOCK, RATE); c->reset();
            for (int ch = 0; ch < 3; ch++) {
                for (int op = 0; op < 4; op++) {
                    wr(*c, 0x30 + op*4 + ch, 0x01); wr(*c, 0x40 + op*4 + ch, 0x00);
                    wr(*c, 0x50 + op*4 + ch, 0x1f); wr(*c, 0x60 + op*4 + ch, 0x00);
                    wr(*c, 0x70 + op*4 + ch, 0x00); wr(*c, 0x80 + op*4 + ch, 0x0f);
                }
                wr(*c, 0xb0 + ch, 0x3f);
                wr(*c, 0xa4 + ch, (blk << 3) | (fnum >> 8)); wr(*c, 0xa0 + ch, fnum & 0xff);
                wr(*c, 0x28, 0xf0 | ch);
            }
        }
        const int SEC = 20;
        clock_t t0 = clock();
        for (int k = 0; k < SEC; k++) {
            std::fill(buf.begin(), buf.end(), 0);
            a.gen(buf.data(), RATE, 0); b.gen(buf.data(), RATE, 0);
        }
        double us = 1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / (SEC * (double)RATE);
        printf("I: %.3f us per output sample on this host (both chips, all keyed)\n", us);

        OpnFm i1, i2;   // idle: reset state, nothing ever keyed
        i1.setRates(TSFM_YM2203_CLOCK, RATE); i1.reset();
        i2.setRates(TSFM_YM2203_CLOCK, RATE); i2.reset();
        t0 = clock();
        for (int k = 0; k < SEC; k++) {
            std::fill(buf.begin(), buf.end(), 0);
            i1.gen(buf.data(), RATE, 0); i2.gen(buf.data(), RATE, 0);
        }
        us = 1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / (SEC * (double)RATE);
        printf("I: %.3f us per output sample, both chips idle\n", us);
    }
    return 0;
}
