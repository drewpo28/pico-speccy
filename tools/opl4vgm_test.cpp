// Host test of the Pico-Zx-Player VGM decoder's YMF278B (OPL4) path: builds small
// VGM files by hand and plays them through the REAL decoder, OplFm and MoonPcm
// (SD, allocator and the unrelated sound chips are stubbed in tools/opl4vgm_stub/).
// A: ROM tone via D0 port 2; B: a RAM sample from a 0x87 data block (pitch checked,
// and silent without the block); C: FM via D0 ports 0/1 at the OPL4 clock (pitch
// checked against clock/684); D: 42 meter channels, wave-channel meter; E: the file's
// own sample ROM (0x84 blocks, as arcade rips carry) at a 28.6 MHz clock.
// With <rom> <file> <out.wav> [seconds] it renders a real VGM to a WAV instead.
//   g++ -O1 -DMOONSOUND=1 -fsanitize=address -Itools/opl4vgm_stub -Isrc -Iexternal \
//       -o /tmp/opl4vgm_test tools/opl4vgm_test.cpp src/player/PlayerVgm.cpp \
//       src/speccy/devices/sound/OplFm.cpp src/speccy/devices/sound/FmTables.cpp \
//       src/speccy/devices/sound/MoonPcm.cpp -x c external/miniz/miniz.c && \
//   (cd /tmp && /tmp/opl4vgm_test /path/to/pico-speccy/src/speccy/roms/moonsound/yrw801.rom)
#include "player/PicoPlayer.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <vector>
#include <string>
extern "C" { uint8_t gb_rom_yrw801[2u << 20]; }
// MoonRom stub: the player takes the YRW801 from MoonRom::get() (MoonRom.cpp needs FatFs).
#include "speccy/devices/sound/MoonRom.h"
namespace MoonRom {
bool builtin() { return true; }
const uint8_t* get() { return gb_rom_yrw801; }
bool filePresent() { return false; }
uint8_t* loadFile(const char**) { return nullptr; }
bool loadForCard(const char**) { return true; }
uint32_t psramBytes() { return 0; }
}
namespace pp {
Decoder* createVgmDecoder();
Decoder::Decoder() : loops_(1), fadeS_(0) {}
bool Decoder::loopPassed() { return false; }
uint32_t Decoder::loopedLenMs(uint32_t a, uint32_t) const { return a; }
void textCopy(char* d, size_t cap, const uint8_t*, size_t, TextEnc) { if (cap) d[0] = 0; }
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Vgm {
    std::vector<uint8_t> d;
    Vgm() { d.assign(0x100, 0); memcpy(&d[0], "Vgm ", 4); put32(0x08, 0x171); put32(0x34, 0x100 - 0x34); put32(0x60, 33868800); }
    void put32(size_t o, uint32_t v) { for (int i = 0; i < 4; i++) d[o + i] = v >> (8 * i); }
    void d0(int port, int reg, int v) { d.push_back(0xD0); d.push_back(port); d.push_back(reg); d.push_back(v); }
    void wait(uint32_t n) { while (n) { uint32_t k = n > 65535 ? 65535 : n; d.push_back(0x61); d.push_back(k & 255); d.push_back(k >> 8); n -= k; } }
    void block(uint8_t t, const std::vector<uint8_t>& b) { d.push_back(0x67); d.push_back(0x66); d.push_back(t); uint32_t l = b.size(); for (int i = 0; i < 4; i++) d.push_back(l >> (8 * i)); d.insert(d.end(), b.begin(), b.end()); }
    void save(const char* p, uint32_t samples) { d.push_back(0x66); put32(0x04, d.size() - 4); put32(0x18, samples); FILE* f = fopen(p, "wb"); fwrite(d.data(), 1, d.size(), f); fclose(f); }
};

struct Out { std::vector<int16_t> lr; double rms() const { double s = 0; for (auto v : lr) s += (double)v * v; return lr.empty() ? 0 : sqrt(s / lr.size()); } };
static Out play(const char* p, pp::Decoder** keep = nullptr) {
    Out o; pp::Decoder* dec = pp::createVgmDecoder();
    if (!dec->open(p)) { printf("open %s failed: %s\n", p, dec->err ? dec->err : "?"); delete dec; return o; }
    printf("  %s: %s | ch=%d extra='%s'\n", p, dec->meta.format, dec->channels(), dec->meta.extra);
    int16_t buf[2 * 512]; int n;
    while ((n = dec->render(buf, 512)) > 0) o.lr.insert(o.lr.end(), buf, buf + 2 * n);
    if (keep) *keep = dec; else delete dec;
    return o;
}
static double freq(const Out& o) {   // zero crossings of L, rising
    int c = 0; for (size_t i = 2; i < o.lr.size(); i += 2) if (o.lr[i - 2] < 0 && o.lr[i] >= 0) c++;
    return c * 31250.0 / (o.lr.size() / 2);
}

int main(int argc, char** argv) {
    FILE* f = fopen(argc > 1 ? argv[1] : "src/speccy/roms/moonsound/yrw801.rom", "rb");
    if (!f || fread(gb_rom_yrw801, 1, sizeof gb_rom_yrw801, f) != sizeof gb_rom_yrw801) { printf("no ROM\n"); return 1; }
    fclose(f);
    if (argc > 3) {                          // render mode: <rom> <file.vgm/vgz> <out.wav> [seconds]
        pp::Decoder* dec = pp::createVgmDecoder();
        if (!dec->open(argv[2])) { printf("open failed: %s\n", dec->err); return 1; }
        printf("%s | ch=%d extra='%s'\n", dec->meta.format, dec->channels(), dec->meta.extra);
        const int maxF = (argc > 4 ? atoi(argv[4]) : 30) * 31250;
        std::vector<int16_t> lr; int16_t buf[1024]; int n; double sq = 0; int clip = 0;
        uint8_t lv[pp::MAX_CH]; int lvMax[pp::MAX_CH] = {};
        while ((int)lr.size() / 2 < maxF && (n = dec->render(buf, 512)) > 0) {
            lr.insert(lr.end(), buf, buf + 2 * n);
            dec->levels(lv); for (int i = 0; i < dec->channels(); i++) if (lv[i] > lvMax[i]) lvMax[i] = lv[i];
        }
        for (auto v : lr) { sq += (double)v * v; if (v == 32767 || v == -32768) clip++; }
        printf("frames %zu rms %.0f clipped %d; meters max:", lr.size() / 2, lr.empty() ? 0 : sqrt(sq / lr.size()), clip);
        for (int i = 0; i < dec->channels(); i++) printf(" %d", lvMax[i]);
        printf("\n");
        FILE* w = fopen(argv[3], "wb"); uint32_t db = lr.size() * 2, rate = 31250, br = rate * 4;
        fwrite("RIFF", 1, 4, w); uint32_t t = 36 + db; fwrite(&t, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
        t = 16; fwrite(&t, 4, 1, w); uint16_t s2[2] = { 1, 2 }; fwrite(s2, 2, 2, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w);
        s2[0] = 4; s2[1] = 16; fwrite(s2, 2, 2, w); fwrite("data", 1, 4, w); fwrite(&db, 4, 1, w); fwrite(lr.data(), 2, lr.size(), w); fclose(w);
        delete dec; return 0;
    }
    // The self-test writes its synthetic .vgm files into a private temp dir, not the cwd.
    char tmpl[] = "/tmp/opl4vgm_test.XXXXXX";
    if (!mkdtemp(tmpl) || chdir(tmpl) != 0) { printf("no temp dir\n"); return 1; }

    // A: ROM tone through the wave port
    { Vgm v; v.d0(2, 0x20, 0x00); v.d0(2, 0x08, 0x00); v.d0(2, 0x38, 0x00); v.d0(2, 0x50, 0x00); v.d0(2, 0x68, 0x80); v.wait(22050);
      v.save("a.vgm", 22050); Out o = play("a.vgm"); printf("A rom tone: rms=%.0f frames=%zu\n", o.rms(), o.lr.size() / 2);
      CHECK(o.rms() > 200, "ROM tone silent"); CHECK(o.lr.size() / 2 >= 15600 && o.lr.size() / 2 <= 15700, "length"); }

    // B: RAM sample via data block 0x87, header bank 4 (= 0x200000)
    auto ramVgm = [](const char* p, bool withBlock) {
        Vgm v;
        std::vector<uint8_t> b(8, 0); uint32_t tot = 1u << 20; for (int i = 0; i < 4; i++) b[i] = tot >> (8 * i);   // start 0
        uint8_t hdr[12] = { 0x20, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xF0, 0x00, 0x0F, 0x00 };
        b.insert(b.end(), hdr, hdr + 12); b.resize(8 + 0x10, 0);
        for (int i = 0; i < 256; i++) b.push_back((i & 32) ? 0x60 : 0xA0);
        if (withBlock) v.block(0x87, b);
        v.block(0x84, std::vector<uint8_t>(16, 0x55));            // a ROM block: must be ignored
        v.d0(2, 0x02, 0x10); v.d0(2, 0x20, 0x01); v.d0(2, 0x08, 0x80); v.d0(2, 0x38, 0x10); v.d0(2, 0x50, 0x00); v.d0(2, 0x68, 0x80); v.wait(22050);
        v.save(p, 22050);
    };
    ramVgm("b1.vgm", true); ramVgm("b0.vgm", false);
    { Out o1 = play("b1.vgm"), o0 = play("b0.vgm");
      // square, period 64 samples at 44.1 kHz -> 689 Hz
      printf("B ram sample: rms=%.0f f=%.0f Hz | no block: rms=%.0f\n", o1.rms(), freq(o1), o0.rms());
      CHECK(o1.rms() > 500, "RAM sample silent"); CHECK(fabs(freq(o1) - 689) < 20, "RAM sample pitch");
      CHECK(o0.rms() < o1.rms() / 4, "no block should be silent-ish"); }

    // C: FM via D0 ports 0/1 (NEW=1 for OPL3 mode, C0 L+R)
    { Vgm v; v.d0(1, 0x05, 0x03);
      const int r[][2] = { {0x20,0x01},{0x40,0x3F},{0x60,0xF0},{0x80,0x07},{0x23,0x01},{0x43,0x00},{0x63,0xF0},{0x83,0x07},{0xC0,0x30},{0xA0,0x98},{0xB0,0x31} };
      for (auto& x : r) v.d0(0, x[0], x[1]);
      v.wait(44100); v.save("c.vgm", 44100);
      pp::Decoder* dec = nullptr; Out o = play("c.vgm", &dec);
      uint8_t lv[pp::MAX_CH] = {}; dec->levels(lv); delete dec;
      printf("C fm: rms=%.0f f=%.1f Hz (OPL4 expect %.1f, OPL3 clock would give %.1f) meter ch0=%d\n",
             o.rms(), freq(o), 408 * (33868800.0 / 684) / 65536, 408 * (14318180.0 / 288) / 65536, lv[0]);
      CHECK(o.rms() > 200, "FM silent"); CHECK(fabs(freq(o) - 408 * (33868800.0 / 684) / 65536) < 1.0, "FM pitch"); CHECK(lv[0] > 0, "FM meter"); }

    // E: the file's own ROM (0x84 blocks, 4 MB, 28.6 MHz arcade clock): wave 0's header at
    // ROM 0, its sample at 0x3F0000 (a page only the second block touches). The YRW801
    // must not be used, and the pitch follows clock / 768 (37.3 kHz chip rate).
    { Vgm v; v.put32(0x60, 28636350);
      auto romBlock = [&](uint32_t start, const std::vector<uint8_t>& bytes) {
          std::vector<uint8_t> b(8, 0); for (int i = 0; i < 4; i++) { b[i] = (4u << 20) >> (8 * i); b[4 + i] = start >> (8 * i); }
          b.insert(b.end(), bytes.begin(), bytes.end()); v.block(0x84, b); };
      romBlock(0, { 0x3F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xF0, 0x00, 0x0F, 0x00 });
      std::vector<uint8_t> smp; for (int i = 0; i < 256; i++) smp.push_back((i & 32) ? 0x60 : 0xA0);
      romBlock(0x3F0000, smp);
      v.d0(2, 0x20, 0x00); v.d0(2, 0x08, 0x00); v.d0(2, 0x38, 0x10); v.d0(2, 0x50, 0x00); v.d0(2, 0x68, 0x80); v.wait(22050);
      v.save("e.vgm", 22050); Out o = play("e.vgm");
      const double want = 28636350.0 / 768 / 64;            // 64-sample square period
      printf("E file ROM: rms=%.0f f=%.0f Hz (want %.0f)\n", o.rms(), freq(o), want);
      CHECK(o.rms() > 500, "file ROM sample silent"); CHECK(fabs(freq(o) - want) < 15, "file ROM pitch / clock"); }

    // D: meter on a wave channel
    { pp::Decoder* dec = nullptr; play("a.vgm", &dec); uint8_t lv[pp::MAX_CH] = {}; dec->levels(lv);
      printf("D meter: chans=%d FM0=%d PCM0=%d PCM1=%d groups '%s' '%s'\n", dec->channels(), lv[0], lv[18], lv[19], dec->groupName(0), dec->groupName(18));
      CHECK(dec->channels() == 42, "42 channels"); CHECK(lv[18] > 0 && lv[19] == 0, "PCM meter"); delete dec; }

    printf(fails ? "%d FAILED\n" : "all OK\n", fails);
    return fails != 0;
}
