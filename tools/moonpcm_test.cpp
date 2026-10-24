/*
 * moonpcm_test.cpp — host-side validation for src/speccy/devices/sound/MoonPcm.cpp
 * (the YMF278B/OPL4 wavetable half of ZXM-MoonSound).
 *
 * Build & run (MoonPcm.cpp has no project dependencies):
 *   g++ -O2 -Wall -Wextra -Isrc -fsanitize=address,undefined -o /tmp/moonpcm_test \
 *       tools/moonpcm_test.cpp src/speccy/devices/sound/MoonPcm.cpp && /tmp/moonpcm_test
 *
 * Re-run after ANY change to MoonPcm.cpp. Checks:
 *   1. device ID (reg 2 reads 001xxxxx) and NEW2 gating of data writes
 *   2. memory access through regs 3-6: auto-increment, RAM write + read-back,
 *      ROM write-protected, 22-bit wrap into the RAM mirror
 *   3. a looped ROM tone plays at the pitch OCT/FNUM demand (OCT=1, FNUM=0 =
 *      the recorded rate), at 31250 AND 44100 Hz output
 *   4. key-off decays to digital silence and the channel leaves the active set
 *   5. wave numbers >= 384 take their header from the bank in reg 2 (RAM)
 *   6. FM in the buffer passes unchanged at the power-on mix (0xF8 = 0x1B),
 *      FM mix 7 mutes it
 *   7. panning: pan +7 attenuates the left side fully
 *   8. 12-bit and 16-bit sample formats decode
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

#include "speccy/devices/sound/MoonPcm.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::vector<uint8_t> g_mem(MoonPcm::MEM_BYTES);

// Header at 12*wav in ROM: 8-bit square of `period` samples, looped whole.
static void putHeader(uint32_t h, uint8_t fmt, uint32_t addr, uint16_t loop, uint16_t end) {
    g_mem[h + 0] = (uint8_t)((fmt << 6) | ((addr >> 16) & 0x3F));
    g_mem[h + 1] = (uint8_t)(addr >> 8);
    g_mem[h + 2] = (uint8_t)addr;
    g_mem[h + 3] = (uint8_t)(loop >> 8);
    g_mem[h + 4] = (uint8_t)loop;
    uint16_t ne = (uint16_t)(-(int)end);
    g_mem[h + 5] = (uint8_t)(ne >> 8);
    g_mem[h + 6] = (uint8_t)ne;
    g_mem[h + 7]  = 0x00;          // no LFO
    g_mem[h + 8]  = 0xF0;          // AR 15 (instant), DR 0
    g_mem[h + 9]  = 0x00;          // SL 0, SR 0
    g_mem[h + 10] = 0x0E;          // RC 0, RR 14 (fast release)
    g_mem[h + 11] = 0x00;
}

static void w(MoonPcm& m, uint8_t r, uint8_t v) { m.write(r, v); }

static void keyTone(MoonPcm& m, int ch, int wav, int oct, int fnum, uint8_t pan = 0) {
    w(m, 0x20 + ch, (uint8_t)(((fnum & 0x7F) << 1) | ((wav >> 8) & 1)));
    w(m, 0x08 + ch, (uint8_t)wav);
    w(m, 0x38 + ch, (uint8_t)(((oct & 0xF) << 4) | ((fnum >> 7) & 7)));
    w(m, 0x50 + ch, 0x01);                       // TL 0, level direct
    w(m, 0x68 + ch, (uint8_t)(0x80 | (pan & 0xF)));
}

// Frequency from rising zero crossings of a buffer.
static double freqOf(const int16_t* b, int n, int rate) {
    int first = -1, last = -1, cnt = 0;
    for (int i = 1; i < n; i++)
        if (b[i - 1] <= 0 && b[i] > 0) { if (first < 0) first = i; last = i; cnt++; }
    if (cnt < 2) return 0;
    return (double)(cnt - 1) * rate / (last - first);
}

int main() {
    // ROM wave 0: 8-bit square, 100-sample period at 0x1000 (recorded at 44.1 kHz
    // that is 441 Hz). Wave 1: the same as 16-bit. Wave 2: 12-bit.
    putHeader(0 * 12, 0, 0x1000, 0, 100);
    for (int i = 0; i < 100; i++) g_mem[0x1000 + i] = i < 50 ? 0x60 : 0xA0;
    putHeader(1 * 12, 2, 0x2000, 0, 100);
    for (int i = 0; i < 100; i++) { int16_t s = i < 50 ? 0x6000 : -0x6000; g_mem[0x2000 + 2*i] = (uint8_t)(s >> 8); g_mem[0x2000 + 2*i + 1] = (uint8_t)s; }
    putHeader(2 * 12, 1, 0x3000, 0, 100);
    for (int i = 0; i < 100; i += 2) {
        int16_t a = (i < 50 ? 0x600 : -0x600), b = (i + 1 < 50 ? 0x600 : -0x600);   // 12-bit values
        uint8_t* p = &g_mem[0x3000 + (i / 2) * 3];
        p[0] = (uint8_t)(a >> 4); p[1] = (uint8_t)(((a & 0xF) << 4) | (b & 0xF)); p[2] = (uint8_t)(b >> 4);
    }

    MoonPcm m(g_mem.data(), g_mem.data() + MoonPcm::ROM_SIZE);
    m.setRate(31250);

    // 1. ID + NEW2 gate.
    CHECK((m.read(2) & 0xE0) == 0x20, "device id %02X", m.read(2));
    w(m, 0x50, 0x55);
    CHECK(m.read(0x50) == 0, "write accepted without NEW2");
    m.setNew2(true);

    // 2. Memory access.
    w(m, 2, 0x11);                               // memory mode, header bank 4
    w(m, 3, 0x20); w(m, 4, 0x00); w(m, 5, 0x00);
    for (int i = 0; i < 16; i++) w(m, 6, (uint8_t)(0xA0 + i));
    CHECK(m.read(5) == 16 && m.read(4) == 0 && m.read(3) == 0x20, "autoinc %02X%02X%02X", m.read(3), m.read(4), m.read(5));
    w(m, 3, 0x20); w(m, 4, 0x00); w(m, 5, 0x00);
    bool ok = true;
    for (int i = 0; i < 16; i++) ok &= m.read(6) == (uint8_t)(0xA0 + i);
    CHECK(ok, "RAM read-back");
    CHECK(g_mem[MoonPcm::ROM_SIZE + 3] == 0xA3, "RAM byte placed");
    w(m, 3, 0x00); w(m, 4, 0x10); w(m, 5, 0x00);
    w(m, 6, 0x00);
    CHECK(g_mem[0x1000] == 0x60, "ROM written");
    CHECK(m.memRead(0x300003) == 0xA3, "RAM mirror");
    // 2b. AM29F016 autoselect, the way MoonService v0.3 reads it.
    {
        auto fw = [&](uint32_t a, uint8_t v) {
            w(m, 3, (uint8_t)(a >> 16)); w(m, 4, (uint8_t)(a >> 8)); w(m, 5, (uint8_t)a); w(m, 6, v);
        };
        auto fr = [&](uint32_t a) {
            w(m, 3, (uint8_t)(a >> 16)); w(m, 4, (uint8_t)(a >> 8)); w(m, 5, (uint8_t)a); return m.read(6);
        };
        const uint8_t s0 = g_mem[0], s1 = g_mem[1];
        g_mem[0] = 0x5A; g_mem[1] = 0xC3;
        CHECK(fr(0) == 0x5A, "ROM byte before autoselect");
        fw(0x555, 0xAA); fw(0x2AA, 0x55); fw(0x555, 0x90);
        CHECK(fr(0) == 0x01 && fr(1) == 0xAD && fr(2) == 0x00, "autoselect id %02X %02X", fr(0), fr(1));
        CHECK(fr(0x10000) == 0x01, "autoselect in another sector");
        fw(0, 0xF0);
        CHECK(fr(0) == 0x5A && fr(1) == 0xC3, "reset back to read");
        fw(0x555, 0xAA); fw(0x2AA, 0x55); fw(0x555, 0xA0); fw(0x100, 0x00);   // program: dropped
        CHECK(fr(0x100) == g_mem[0x100], "program changed the ROM");
        fw(0x555, 0xAA); fw(0x2AA, 0x55); fw(0x555, 0x80);
        fw(0x555, 0xAA); fw(0x2AA, 0x55); fw(0, 0x30);                         // sector erase: dropped
        CHECK(fr(0) == 0x5A && fr(0) == fr(0), "erase / toggle bit");
        fw(0x556, 0xAA); fw(0x2AA, 0x55); fw(0x555, 0x90);                     // wrong unlock
        CHECK(fr(0) == 0x5A, "autoselect without a valid unlock");
        g_mem[0] = s0; g_mem[1] = s1;
    }
    w(m, 2, 0x10);                               // memory mode off, bank 4

    // 3. Pitch at OCT=1 FNUM=0 (recorded rate): 441 Hz; OCT=2: 882 Hz.
    const int N = 31250;
    std::vector<int16_t> L(N), R(N);
    keyTone(m, 0, 0, 1, 0);
    m.gen(L.data(), R.data(), N, 0);
    double f = freqOf(L.data(), N, 31250);
    CHECK(fabs(f - 441.0) < 1.0, "pitch oct1 %.2f", f);
    int peak = 0;
    for (int i = 0; i < N; i++) peak = abs(L[i]) > peak ? abs(L[i]) : peak;
    CHECK(peak > 2000, "too quiet %d", peak);
    w(m, 0x38, (uint8_t)(2 << 4));
    std::fill(L.begin(), L.end(), 0); std::fill(R.begin(), R.end(), 0);
    m.gen(L.data(), R.data(), N, 0);
    f = freqOf(L.data(), N, 31250);
    CHECK(fabs(f - 882.0) < 2.0, "pitch oct2 %.2f", f);

    // 4. Key-off -> silence, channel inactive.
    w(m, 0x68, 0x00);
    std::fill(L.begin(), L.end(), 0); std::fill(R.begin(), R.end(), 0);
    m.gen(L.data(), R.data(), N, 0);
    CHECK(L[N - 1] == 0 && R[N - 1] == 0, "not silent after key-off");
    CHECK(!m.anyActive(), "channel still active");

    // Same pitch at a 44.1 kHz output rate.
    {
        MoonPcm m2(g_mem.data(), g_mem.data() + MoonPcm::ROM_SIZE); m2.setRate(44100); m2.setNew2(true);
        std::vector<int16_t> L2(44100), R2(44100);
        keyTone(m2, 3, 0, 1, 0x100);            // fnum 256 -> x1.25
        m2.gen(L2.data(), R2.data(), 44100, 0);
        double f2 = freqOf(L2.data(), 44100, 44100);
        CHECK(fabs(f2 - 551.25) < 1.5, "pitch @44.1k %.2f", f2);
        // Half-rate synthesis (every other output sample computed, x2 lerp):
        // same pitch, same level within the interpolation's loss at 551 Hz.
        {
            MoonPcm m3(g_mem.data(), g_mem.data() + MoonPcm::ROM_SIZE); m3.setRate(44100, true); m3.setNew2(true);
            std::vector<int16_t> L3(44100), R3(44100);
            keyTone(m3, 3, 0, 1, 0x100);
            m3.gen(L3.data(), R3.data(), 44100, 0);
            double f3 = freqOf(L3.data(), 44100, 44100);
            CHECK(fabs(f3 - 551.25) < 1.5, "pitch @44.1k half-rate %.2f", f3);
            int p2 = 0, p3 = 0;
            for (int i = 4410; i < 44100; i++) { if (abs(L2[i]) > p2) p2 = abs(L2[i]); if (abs(L3[i]) > p3) p3 = abs(L3[i]); }
            CHECK(p3 > p2 * 9 / 10 && p3 <= p2 + 1, "half-rate level %d vs %d", p3, p2);
        }
        // The split path (MoonSplit: the wave half generated apart with genPcm,
        // the FM buffer scaled by fmMix* and the two added at the frame end)
        // must equal gen()'s single expression within its own rounding, with a
        // non-default FM/PCM mix level and FM content in the buffer.
        {
            MoonPcm a(g_mem.data(), g_mem.data() + MoonPcm::ROM_SIZE); a.setRate(31250); a.setNew2(true);
            MoonPcm b(g_mem.data(), g_mem.data() + MoonPcm::ROM_SIZE); b.setRate(31250); b.setNew2(true);
            w(a, 0xF8, 0x12); w(b, 0xF8, 0x12);          // FM -6 dB-ish, PCM -3 dB-ish
            keyTone(a, 3, 0, 1, 0x100); keyTone(b, 3, 0, 1, 0x100);
            std::vector<int16_t> fmL(N), fmR(N), La(N), Ra(N), wL(N), wR(N);
            for (int i = 0; i < N; i++) { fmL[i] = (int16_t)(12000 * sin(i * 0.05)); fmR[i] = (int16_t)(-9000 * sin(i * 0.031)); }
            La = fmL; Ra = fmR;
            a.gen(La.data(), Ra.data(), N, 0);
            CHECK(b.genPcm(wL.data(), wR.data(), N, 0), "genPcm: nothing sounded");
            int worst = 0;
            for (int i = 0; i < N; i++) {
                int32_t l = ((fmL[i] * b.fmMixL()) >> 12) + wL[i];
                int32_t r = ((fmR[i] * b.fmMixR()) >> 12) + wR[i];
                l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
                r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
                int d = abs(l - La[i]); if (abs(r - Ra[i]) > d) d = abs(r - Ra[i]);
                if (d > worst) worst = d;
            }
            CHECK(worst <= 2, "split path differs from gen by %d", worst);
            w(b, 0x68 + 3, 0x00);
            b.genPcm(wL.data(), wR.data(), N, 0);
            CHECK(!b.anyActive() && wL[N - 1] == 0, "genPcm: silence after key-off");
        }
        keyTone(m, 3, 0, 1, 0x100);
        std::fill(L.begin(), L.end(), 0); std::fill(R.begin(), R.end(), 0);
        m.gen(L.data(), R.data(), N, 0);
        f = freqOf(L.data(), N, 31250);
        CHECK(fabs(f - 551.25) < 1.5, "pitch @31.25k fnum256 %.2f", f);
        w(m, 0x68 + 3, 0x00);
        std::fill(L.begin(), L.end(), 0); m.gen(L.data(), R.data(), N, 0);
    }

    // 5. Wave 384 header from RAM bank 4 (0x200000): points at the ROM square.
    putHeader(0x200000, 0, 0x1000, 0, 100);
    keyTone(m, 5, 384, 1, 0);
    std::fill(L.begin(), L.end(), 0); std::fill(R.begin(), R.end(), 0);
    m.gen(L.data(), R.data(), N, 0);
    f = freqOf(L.data(), N, 31250);
    CHECK(fabs(f - 441.0) < 1.0, "wave 384 pitch %.2f", f);
    w(m, 0x68 + 5, 0x00);
    std::fill(L.begin(), L.end(), 0); m.gen(L.data(), R.data(), N, 0);
    CHECK(!m.anyActive(), "wave384 still active");

    // 6. FM passthrough at the default mix; FM mix 7 = mute.
    for (int i = 0; i < 64; i++) { L[i] = (int16_t)(i * 100 - 3000); R[i] = (int16_t)(3000 - i * 50); }
    std::vector<int16_t> L0(L.begin(), L.begin() + 64), R0(R.begin(), R.begin() + 64);
    m.gen(L.data(), R.data(), 64, 0);
    CHECK(memcmp(L.data(), L0.data(), 128) == 0 && memcmp(R.data(), R0.data(), 128) == 0, "FM changed at default mix");
    w(m, 0xF8, 0x3F);
    m.gen(L.data(), R.data(), 64, 0);
    CHECK(L[10] == 0 && R[10] == 0, "FM not muted %d %d", L[10], R[10]);
    w(m, 0xF8, 0x1B);

    // 7. Pan +7: left fully attenuated.
    keyTone(m, 7, 0, 1, 0, 7);
    std::fill(L.begin(), L.end(), 0); std::fill(R.begin(), R.end(), 0);
    m.gen(L.data(), R.data(), 4000, 0);
    int pl = 0, pr = 0;
    for (int i = 0; i < 4000; i++) { pl = abs(L[i]) > pl ? abs(L[i]) : pl; pr = abs(R[i]) > pr ? abs(R[i]) : pr; }
    CHECK(pl == 0 && pr > 2000, "pan +7 L=%d R=%d", pl, pr);
    w(m, 0x68 + 7, 0x00);
    std::fill(L.begin(), L.end(), 0); m.gen(L.data(), R.data(), N, 0);

    // 8. 16-bit and 12-bit formats.
    for (int wav = 1; wav <= 2; wav++) {
        keyTone(m, 9, wav, 1, 0);
        std::fill(L.begin(), L.end(), 0); std::fill(R.begin(), R.end(), 0);
        m.gen(L.data(), R.data(), N, 0);
        f = freqOf(L.data(), N, 31250);
        int pk = 0; for (int i = 0; i < N; i++) pk = abs(L[i]) > pk ? abs(L[i]) : pk;
        CHECK(fabs(f - 441.0) < 1.0 && pk > 2000, "format wave %d f=%.2f peak=%d", wav, f, pk);
        w(m, 0x68 + 9, 0x00);
        std::fill(L.begin(), L.end(), 0); m.gen(L.data(), R.data(), N, 0);
    }

    // Reset clears everything, NEW2 included.
    m.reset();
    CHECK(!m.new2() && !m.anyActive(), "reset");

    printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
