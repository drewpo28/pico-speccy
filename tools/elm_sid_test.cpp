// Host test of the eLeMeNt ZX SID.
// g++ -O2 -Wall -Wextra -Isrc -o /tmp/esid tools/elm_sid_test.cpp src/speccy/machines/Element/ElmSid.cpp && /tmp/esid
#include "speccy/machines/Element/ElmSid.h"
#include <cmath>
#include <cstdio>
using namespace ElmSid;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)
enum { FRAME = 625, RATE = 31250 };
static double buf[FRAME * 20];
// Renders frames, keeps the mixed output minus its +128 bias.
static int run(int frames) {
    int n = 0;
    for (int f = 0; f < frames; f++) {
        frameEnd(FRAME);
        const int16_t* m = mix();
        for (int i = 0; i < FRAME; i++) buf[n++] = m ? m[i] - 128 : 0;
    }
    return n;
}
static int crossings(int from, int to) {
    double mean = 0;
    for (int i = from; i < to; i++) mean += buf[i];
    mean /= to - from;
    int c = 0;
    for (int i = from + 1; i < to; i++) if ((buf[i - 1] < mean) != (buf[i] < mean)) c++;
    return c;
}
static double rms(int from, int to) {
    double mean = 0, e = 0;
    for (int i = from; i < to; i++) mean += buf[i];
    mean /= to - from;
    for (int i = from; i < to; i++) e += (buf[i] - mean) * (buf[i] - mean);
    return sqrt(e / (to - from));
}
static void tone(int voice, double hz, uint8_t ctrl) {
    const int b = voice * 7;
    const uint16_t f = (uint16_t)(hz * 16777216.0 / 985248.0 + 0.5);
    write(b, f); write(b + 1, f >> 8);
    write(b + 2, 0x00); write(b + 3, 0x08);   // 50 % pulse
    write(b + 5, 0x00); write(b + 6, 0xF0);   // instant attack, full sustain
    write(b + 4, ctrl | 1);
}
int main() {
    CHECK(init(RATE));
    CHECK(mix() == nullptr);                   // a quiet chip is not mixed
    // 440 Hz triangle, PAL: 16 frames = 0.32 s = 140.8 cycles = 282 crossings.
    write(0x18, 0x0F);
    tone(0, 440, 0x10);
    int n = run(20);
    const int c = crossings(FRAME * 4, n);
    printf("tri 440 Hz: %d crossings in 0.32 s (282), rms %.1f\n", c, rms(FRAME * 4, n));
    CHECK(c >= 278 && c <= 286);
    CHECK(rms(FRAME * 4, n) > 15);
    // NTSC runs the same register value 3.8 % higher.
    setParams(PAR_NTSC);
    n = run(20);
    const int cn = crossings(FRAME * 4, n);
    printf("NTSC: %d crossings (292)\n", cn);
    CHECK(cn >= 288 && cn <= 297);
    setParams(0);
    // ENV3: attack 0 reaches 255 inside a frame, sustain F holds it, release 0 empties it.
    tone(2, 1000, 0x20);
    run(1);
    CHECK(read(0x1C) == 0xFF);
    write(2 * 7 + 4, 0x20);                    // gate off
    run(2);
    CHECK(read(0x1C) == 0);
    // Decay to sustain 8 (0x88) with decay 0 (6 ms full scale).
    write(2 * 7 + 5, 0x00); write(2 * 7 + 6, 0x80); write(2 * 7 + 4, 0x21);
    run(2);
    printf("sustain 8: ENV3 = %02X (88)\n", read(0x1C));
    CHECK(read(0x1C) == 0x88);
    // OSC3 with noise: a random number source.
    tone(2, 3000, 0x80);
    int diff = 0, last = -1;
    for (int i = 0; i < 32; i++) { renderTo(i * 19 + 1); const int r = read(0x1B); if (r != last) diff++; last = r; }
    frameEnd(FRAME);
    printf("OSC3 noise: %d changes of 32\n", diff);
    CHECK(diff > 20);
    // Write-only registers read back the bus, h1F the parameters.
    write(0x05, 0x5A);
    CHECK(read(0x00) == 0x5A);
    setParams(PAR_8580 | PAR_NTSC);
    CHECK(read(0x1F) == 3);
    // Filter: a 3 kHz saw through the low pass at the bottom of the 8580 curve is gone.
    reset(); setParams(PAR_8580);
    write(0x18, 0x0F);
    tone(0, 3000, 0x20);
    n = run(10);
    const double dry = rms(FRAME * 2, n);
    write(0x15, 0); write(0x16, 0x08); write(0x17, 0x01); write(0x18, 0x1F);
    n = run(10);
    const double wet = rms(FRAME * 2, n);
    printf("8580 LP: dry %.1f wet %.1f\n", dry, wet);
    CHECK(wet < dry * 0.2);
    // Volume-register digi: audible on the 6581 through its DC, silent on the 8580.
    for (int model = 0; model < 2; model++) {
        reset(); setParams(model ? PAR_8580 : 0);
        for (int f = 0; f < 10; f++) {
            for (int i = 0; i < FRAME; i += 16) { renderTo(i); write(0x18, (i / 16) & 1 ? 0x0F : 0x00); }
            frameEnd(FRAME);
        }
        const int16_t* m = mix();
        int lo = 999, hi = -999;
        for (int i = 0; i < FRAME; i++) { if (m && m[i] < lo) lo = m[i]; if (m && m[i] > hi) hi = m[i]; }
        printf("%s volume digi: p-p %d\n", model ? "8580" : "6581", m ? hi - lo : 0);
        CHECK(model ? (!m || hi - lo < 4) : (m && hi - lo > 60));
    }
    // Muted (e_zxi_002 bit 3 clear): nothing to mix.
    tone(0, 440, 0x10);
    mute(true); run(1); CHECK(mix() == nullptr);
    mute(false); run(1); CHECK(mix() != nullptr);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
