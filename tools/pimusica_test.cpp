// pico-speccy — host test of the PI-CARD MUSICA protocol machine (PiMusica.cpp).
//
// The shipped src/speccy/devices/sound/PiMusica.cpp is built against the stub
// headers in tools/pimusica_stub/ (SDK spinlocks, Buffer, the player's Decoder,
// the card ring) and driven by a transcription of the Tracker player plugin's
// bus side: ping, a 10000-byte file in three windows (two 0x12 + a 0x13 tail),
// 0x14, the telemetry cadence, pause, forward seek, the core0 top-up, the end of
// a track, a .mid by magic, a short window, 3 NAKs, a failed open, an unknown
// host command and a reset mid-session. Every frame's checksum and the offset /
// length copies are checked as the plugin checks them.
//
//   g++ -O1 -Wall -std=gnu++17 -fsanitize=address,undefined -Itools/pimusica_stub -Isrc
//       -o /tmp/pimus_test tools/pimusica_test.cpp src/speccy/devices/sound/PiMusica.cpp
//   /tmp/pimus_test
//
// Re-run after ANY change to PiMusica.cpp. Mutations each caught: 0x14 not
// armed after the open, a frame read past its end not wrapping (both found by
// this test on the first run), a wrong checksum, a copy mismatch.
#include "speccy/devices/sound/PiMusica.h"
#include "player/PicoPlayer.h"
#include "drivers/sound/pwm_audio.h"
#include "app/Buffer.h"
#include "app/Config.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

bool Config::pimusica = true;
int g_lock_depth = 0;
uint32_t g_now_us = 0;
volatile int16_t* volatile pcm_card_ring = nullptr;
volatile uint32_t pcm_card_mask, pcm_card_r, pcm_card_w;
void pcm_card_attach(int16_t* ring, uint32_t frames) { pcm_card_mask = frames - 1; pcm_card_r = pcm_card_w = 0; pcm_card_ring = ring; }
void pcm_card_detach() { pcm_card_ring = nullptr; }
void* Buffer::palloc(size_t n, int) { return malloc(n); }
void Buffer::pfree(void* p) { free(p); }

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ── the fake decoder: records the image, renders a ramp ──────────────────────
struct FakeDec : pp::Decoder {
    static std::vector<uint8_t> lastImage; static int opens; static std::string lastExt;
    uint32_t frames = 0, len = 0; bool fail = false;
    bool openMemory(const uint8_t* d, uint32_t n) override {
        opens++; lastImage.assign(d, d + n);
        if (fail) { err = "fake fail"; return false; }
        strcpy(meta.format, "FAKE, 4 ch"); len = 125000; return true;   // 2:05
    }
    int render(int16_t* lr, int n) override {
        if (posMs() >= len) return 0;
        for (int i = 0; i < n; i++) { lr[2*i] = (int16_t)(frames + i); lr[2*i+1] = (int16_t)-(frames + i); }
        frames += n; return n;
    }
    int channels() const override { return 4; }
    void levels(uint8_t* o) override { o[0] = 10; o[1] = 0; o[2] = 5; o[3] = 0; }
    uint32_t posMs() const override { return (uint32_t)((uint64_t)frames * 1000 / pp::RATE); }
    uint32_t lenMs() const override { return len; }
};
std::vector<uint8_t> FakeDec::lastImage; int FakeDec::opens = 0; std::string FakeDec::lastExt;
static bool g_failOpen = false;
bool pp::available() { return true; }
pp::Decoder* pp::createDecoder(const std::string& e) { FakeDec::lastExt = e; FakeDec* d = new FakeDec; d->fail = g_failOpen; return d; }

// ── the host side, as the plugin does it ─────────────────────────────────────
static uint8_t in63() { uint8_t v; PiMusica::portRead(0x63, v); return v; }
static uint8_t in67() { uint8_t v; PiMusica::portRead(0x67, v); return v; }
static void sendCmd(uint8_t c0, uint8_t a1 = 0, uint8_t a2 = 0, uint8_t a3 = 0, uint8_t a4 = 0, uint8_t a5 = 0, uint8_t a6 = 0, uint8_t a7 = 0) {
    uint8_t b[8] = { c0, a1, a2, a3, a4, a5, a6, a7 };
    for (int i = 0; i < 8; i++) PiMusica::portWrite(0x63, b[i]);
}
static bool readFrame(uint8_t* f, int n) {    // + the plugin's checksum test
    for (int i = 0; i < n; i++) f[i] = in67();
    uint8_t s = 0; for (int i = 0; i < n - 1; i++) s = (uint8_t)(s + f[i]);
    return n == 1 ? f[0] == 0 : s == f[n - 1];
}
static uint8_t poll(int tries = 50) { for (int i = 0; i < tries; i++) { uint8_t s = in63(); if (s && s != 0xFF) return s; } return 0; }

static std::vector<uint8_t> g_file;
static uint8_t lastState = 0, lastPosMin = 0, lastPosSec = 0;
// One service step, like handle(s). Returns the code served.
static uint8_t serve(bool nakReads = false) {
    uint8_t s = poll(); if (!s) return 0;
    uint8_t f[20];
    if (s == 0x12 || s == 0x13) {
        CHECK(readFrame(f, 13));
        uint32_t off = f[0] | f[1] << 8 | f[2] << 16 | (uint32_t)f[3] << 24, len = f[4] | f[5] << 8;
        CHECK(!memcmp(f, f + 6, 6)); CHECK(len >= 1 && len <= 4096);
        if (s == 0x12) { CHECK(len == 4096); CHECK(off % 512 == 0); }
        if (nakReads) { sendCmd(0x04); return s; }
        CHECK(off + len <= g_file.size());
        for (uint32_t i = 0; i < len; i++) PiMusica::portWrite(0x67, g_file[off + i]);
        sendCmd(0x03, (uint8_t)len, (uint8_t)(len >> 8));
        return s;
    }
    int n = s == 0x11 ? 17 : s == 0x14 ? 1 : s == 0x15 ? 9 : s == 0x16 ? 4 : s == 0x19 ? 7 : s == 0x1A ? 5 : -1;
    if (n < 0) { printf("unknown code %02X\n", s); fails++; sendCmd(0x04); return s; }
    CHECK(readFrame(f, n));
    if (s == 0x11) CHECK(!memcmp(f, "PICO-SPECCY V1.1", 16));
    if (s == 0x15) { CHECK(f[0] == 0x02 && f[1] == 0x05); CHECK(f[2] == 4); }   // 2:05 BCD, 4 "samples"
    if (s == 0x16) { lastState = f[2]; lastPosMin = f[0]; lastPosSec = f[1]; }
    if (s == 0x1A) { CHECK(f[0] == 2); }                                        // two channels sounding
    sendCmd(0x03);
    return s;
}

// Frames of emulation: the card's frameTick + core1 pumping in between.
static void frames(int n, bool core1 = true) {
    for (int i = 0; i < n; i++) {
        g_now_us += 20000;
        PiMusica::frameTick();
        if (core1) for (int k = 0; k < 40; k++) PiMusica::core1Pump();
    }
}
// The audio tick draining the ring: ~640 frames a frame.
static void drain(int frames_) { uint32_t r = pcm_card_r, w = pcm_card_w; uint32_t n = w - r; if ((uint32_t)frames_ < n) n = frames_; pcm_card_r = r + n; }

static void session(uint32_t size, bool core1 = true) {
    g_file.resize(size); for (uint32_t i = 0; i < size; i++) g_file[i] = (uint8_t)(i * 7 + (i >> 8));
    memcpy(&g_file[0], "MOD!", 4);
    sendCmd(0x02, (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24), 2, 0x23, 0);
    int reads = 0; uint8_t s;
    while ((s = serve()) == 0x12 || s == 0x13) { reads++; }
    CHECK(reads == (int)((size + 4095) / 4096));
    CHECK(s == 0);                                   // nothing more until the frame tick opens it
    frames(1, core1);
    CHECK(FakeDec::lastImage == g_file);
    s = serve(); CHECK(s == 0x14);
    frames(1, core1); s = serve(); CHECK(s == 0x15);  // info first (mask bit 0)
    frames(1, core1); s = serve(); CHECK(s == 0x16);  // the state change 0 -> playing
    CHECK(lastState == 0x09);                         // playing + seek available
}

int main() { setvbuf(stdout, nullptr, _IONBF, 0);
    CHECK(PiMusica::init() == nullptr);
    CHECK(g_pimus_on == 1);
    CHECK(in63() == 0);                               // idle: no command
    // ping
    sendCmd(0x01);
    CHECK(serve() == 0x11);
    CHECK(in63() == 0);
    // a re-read of a frame is allowed: NAK re-arms the same command
    sendCmd(0x01); uint8_t f[20]; CHECK(poll() == 0x11); in67(); in67(); sendCmd(0x04);
    CHECK(poll() == 0x11); CHECK(readFrame(f, 17)); sendCmd(0x03);

    // ── session 1: 10000 bytes = 2 x 0x12 + a 1808-byte 0x13 ──
    session(10000);
    CHECK(FakeDec::opens == 1); CHECK(FakeDec::lastExt == "mod");
    CHECK(g_pimus_c1_live);
    CHECK(pcm_card_w > 0);                            // the head start + core1 filled it
    uint32_t w0 = pcm_card_w;
    frames(3); CHECK(pcm_card_w == w0);               // ring full: nothing more
    CHECK(pcm_card_w - pcm_card_r <= 4096);
    // the ring content is the decoder's output in order
    CHECK(pcm_card_ring[0] == 0 && pcm_card_ring[2] == 1 && pcm_card_ring[3] == -1);
    // play: drain a frame's worth per frame, core1 refills
    for (int i = 0; i < 60; i++) { drain(640); frames(1); }
    CHECK(pcm_card_w - pcm_card_r >= 4096 - 640);
    // telemetry cadence: position / polyphony alternate every 25 frames
    int pos = 0, poly = 0;
    for (int i = 0; i < 200; i++) { drain(640); frames(1); uint8_t s = serve(); if (s == 0x16) pos++; else if (s == 0x1A) poly++; }
    CHECK(pos >= 3 && poly >= 3);
    // pause: the ring stops filling, the state byte says so
    sendCmd(0x06, 1); frames(1);
    uint8_t s = serve(); CHECK(s == 0x16); CHECK(lastState == 0x0D);
    uint32_t w1 = pcm_card_w; drain(2000); frames(3); CHECK(pcm_card_w == w1);
    sendCmd(0x06, 1); frames(1); s = serve(); CHECK(s == 0x16); CHECK(lastState == 0x09);
    CHECK(pcm_card_w > w1);
    // forward seek: +10 s in slices
    uint32_t p0 = lastPosSec + lastPosMin * 60;
    sendCmd(0x06, 2);
    for (int i = 0; i < 20; i++) { drain(640); frames(1); }
    frames(25); serve(); frames(1); s = serve();        // the next position frame
    while (s != 0x16) { frames(1); s = serve(); }
    uint32_t p1 = lastPosSec + lastPosMin * 60;
    CHECK(p1 >= p0 + 9);
    // the core0 top-up when core1 does not run
    drain(4096); frames(1, false); CHECK(pcm_card_w - pcm_card_r >= 512);
    // end of track: state 2 is reported, core1 is off
    for (int i = 0; i < 8000; i++) { drain(640); frames(1); uint8_t s2 = serve(); if (s2 == 0x16 && lastState == 0x02) break; }
    CHECK(lastState == 0x02);
    CHECK(!g_pimus_c1_live);

    // ── session 2: a 300-byte file, 0x13 only, opened as MIDI by its magic ──
    g_file.clear();
    { uint32_t size = 300; g_file.resize(size); memcpy(&g_file[0], "MThd\0\0\0\6", 8);
      sendCmd(0x02, (uint8_t)size, (uint8_t)(size >> 8), 0, 0, 2, 0x33, 0);
      CHECK(serve() == 0x13); CHECK(serve() == 0); frames(1); CHECK(FakeDec::lastExt == "mid");
      CHECK(!g_pimus_c1_live); CHECK(PiMusica::ownsMidiSynth());
      CHECK(serve() == 0x14); frames(1); CHECK(serve() == 0x15); frames(1); CHECK(serve() == 0x19); frames(1); CHECK(serve() == 0x16);
      CHECK(pcm_card_w - pcm_card_r >= 1500);        // core0 keeps ~2.5 frames ahead
      drain(1000); frames(1, false); CHECK(pcm_card_w - pcm_card_r >= 1500); }
    // a short window: the plugin acknowledges fewer bytes than asked -> same request again
    sendCmd(0x01); CHECK(serve() == 0x11);
    CHECK(!PiMusica::ownsMidiSynth());
    g_file.assign(5000, 0x5A);
    sendCmd(0x02, (uint8_t)5000, (uint8_t)(5000 >> 8), 0, 0, 2, 0x02, 0);
    CHECK(poll() == 0x12); readFrame(f, 13);
    for (int i = 0; i < 4000; i++) PiMusica::portWrite(0x67, 0x5A);
    sendCmd(0x03, (uint8_t)4000, (uint8_t)(4000 >> 8));
    CHECK(poll() == 0x12);                            // re-armed, offset 0 again
    readFrame(f, 13); CHECK(f[0] == 0 && f[1] == 0);
    CHECK(serve() == 0x12); CHECK(serve() == 0x13); frames(1); CHECK(serve() == 0x14);
    // 3 NAKs on a read end the session as "finished"
    sendCmd(0x01); serve();
    g_file.assign(4096, 1);
    sendCmd(0x02, 0, 0x10, 0, 0, 2, 0x02, 0);
    for (int i = 0; i < 4; i++) serve(true);
    CHECK(in63() == 0); frames(1); CHECK(serve() == 0x16); CHECK(lastState == 0x02);
    // a decoder failure also ends as "finished"
    sendCmd(0x01); serve();
    g_failOpen = true; g_file.assign(100, 2);
    sendCmd(0x02, 100, 0, 0, 0, 2, 0x02, 0);
    CHECK(serve() == 0x13); frames(1); CHECK(serve() == 0x16); CHECK(lastState == 0x02);
    g_failOpen = false;
    // an unknown host command is logged and ignored; a reset mid-session is clean
    sendCmd(0x7E); CHECK(in63() == 0);
    sendCmd(0x02, 100, 0, 0, 0, 2, 0x02, 0); CHECK(poll() == 0x13);
    PiMusica::reset(); CHECK(in63() == 0); CHECK(!g_pimus_c1_live);
    CHECK(g_lock_depth == 0);
    printf("%s (%d fails)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
