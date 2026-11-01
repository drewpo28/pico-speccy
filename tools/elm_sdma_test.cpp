// Host test of the eLeMeNt ZX SoundDMA.
// g++ -O2 -Wall -Wextra -Isrc -o /tmp/es tools/elm_sdma_test.cpp src/speccy/machines/Element/ElmSdma.cpp && /tmp/es
#include "speccy/machines/Element/ElmSdma.h"
#include <cstdio>
#include <cstring>
using namespace ElmSdma;
static uint8_t mem[0x80000];
static const uint8_t* pg(uint32_t p) { return mem + (p & 63) * 0x2000; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void w3(int o, uint32_t v) { write(o, v); write(o + 1, v >> 8); write(o + 2, v >> 16); }
static void setup(int ch, int rateHalf, uint32_t st, uint32_t lp, uint32_t len, uint8_t par, int vl = 64, int vr = 64) {
    const int b = ch * 16;
    write(b + 1, vl); write(b + 2, vr); w3(b + 3, st); w3(b + 6, lp); w3(b + 9, len);
    const uint32_t p = rateHalf; write(b + 0x0C, p); write(b + 0x0D, p >> 8);
    write(b, par);
}
int main() {
    CHECK(init(pg, 31250));
    // Period for one source byte per output sample at 31250 Hz: 7167969 / 31250 = 229.
    memset(mem, 0x80, sizeof mem);
    for (int i = 0; i < 100; i++) mem[i] = (uint8_t)(0x80 + 100);
    setup(0, 229, 0, 0, 100, P_PLAY);
    frameEnd(200);
    const int16_t* l = mixL();
    CHECK(l != nullptr);
    CHECK(l[90] >= 90 + 25);                   // bias ramping + the sample
    CHECK(!ElmSdma::active() || posHigh() == 0);
    // No loop: stops at the end.
    frameEnd(640); frameEnd(640);
    CHECK(mixL() == nullptr || mixL()[600] <= 128);
    // Length counts from Start, the loop runs from Loop to Start + Length.
    reset();
    for (int i = 0; i < 300; i++) mem[0x1000 + i] = (uint8_t)(i < 200 ? 0xC0 : 0x40);
    setup(1, 229, 0x1000, 0x1000 + 200, 300, P_PLAY | P_LOOP);
    frameEnd(640);
    l = mixL();
    // 300 samples in, then the 100 bytes of the loop for ever: low only.
    int hi = 0; for (int i = 400; i < 640; i++) if (l[i] > 128) hi++;
    CHECK(hi == 0);
    // Signed sample, volumes separate.
    reset();
    mem[0x2000] = 100;
    setup(2, 229, 0x2000, 0, 1, P_PLAY | P_SIGNED, 64, 0);
    frameEnd(1);
    CHECK(mixL()[0] > mixR()[0]);
    // Stereo takes two bytes a step, high byte of the position follows.
    reset();
    setup(0, 229, 0, 0, 0x10000, P_PLAY | P_LOOP | P_STEREO);
    frameEnd(320);
    CHECK(posHigh() == 2);
    // Continue keeps the position, stop halts it.
    write(0, P_LOOP);
    const uint8_t at = posHigh();
    frameEnd(320);
    CHECK(posHigh() == at);
    write(0, P_PLAY | P_CONT | P_LOOP | P_STEREO);
    frameEnd(320);
    CHECK(posHigh() == at + 3);
    // Rate: period 161 is 44.5 kHz, 1.42 source steps an output sample.
    reset();
    setup(0, 161, 0, 0, 0x70000, P_PLAY | P_LOOP);
    frameEnd(640);
    CHECK(posHigh() == 3);
    printf("%d failed\n", fails);
    return fails != 0;
}
