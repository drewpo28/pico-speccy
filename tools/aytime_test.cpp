// Host check of src/player/AyTime.h (the data-walk track length of PT3/PT2/STC/
// STP/SQT) against the REAL Z80 replayer the firmware plays them with
// (src/player/PlayerAyZ80.cpp: same binaries, same INIT/PLAY, same loop flag).
//
//   gcc -O2 -c -Iexternal/redcode -DZ80_STATIC '-DZ80_EXTERNAL_HEADER="Z80_compat.h"' \
//       -o /tmp/z80.o external/redcode/Z80_redcode.c && \
//   g++ -O2 -Wall -Isrc -Iexternal/redcode -o /tmp/aytime_test tools/aytime_test.cpp /tmp/z80.o && \
//     /tmp/aytime_test file.pt3 dir/*.stc ...
//
// Prints one line per mismatch and a summary; exit code 1 on any mismatch.
#include "player/AyTime.h"
#include "player/ptsplay_bin.h"
#include "player/ayplayers_bin.h"

#define Z80_STATIC
#define Z80_EXTERNAL_HEADER "Z80_compat.h"
extern "C" {
#include "redcode/Z80_redcode.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using namespace pp;

static uint8_t ram[0x10000];
static bool done;
static Z80 cpu;
static constexpr uint16_t STUB = 0xEFF0, STACK = 0xEFE0;

static zuint8 rd(void*, zuint16 a) { return ram[a]; }
static void   wr(void*, zuint16 a, zuint8 v) { ram[a] = v; }
static zuint8 op(void*, zuint16 a) { if (a == STUB + 3) { done = true; z80_break(&cpu); } return ram[a]; }
static zuint8 in(void*, zuint16) { return 0xFF; }
static void   out(void*, zuint16, zuint8) {}
static zuint8 inta(void*, zuint16) { return 0xFF; }

static bool call(uint16_t a, uint16_t hl, uint16_t de = 0) {
    ram[STUB] = 0xCD; ram[STUB + 1] = (uint8_t)a; ram[STUB + 2] = a >> 8; ram[STUB + 3] = 0x18; ram[STUB + 4] = 0xFE;
    Z80_PC(cpu) = STUB; Z80_SP(cpu) = STACK;
    cpu.hl.uint16_value = hl; cpu.de.uint16_value = de; cpu.iff1 = cpu.iff2 = 0;
    done = false;
    size_t spent = 0;
    while (!done && spent < 70000) spent += z80_run(&cpu, 4096);
    return done;
}

struct Def { const uint8_t* bin; uint16_t size, org, init, play, setup, modAt; };
static const Def kDefs[] = {
    { kPtsPlayBin, sizeof(kPtsPlayBin), 0xF000, 0xF003, 0xF005, 0xF00A, 0x0100 },
    { kPtsPlayBin, sizeof(kPtsPlayBin), 0xF000, 0xF003, 0xF005, 0xF00A, 0x0100 },
    { kStcPlayBin, sizeof(kStcPlayBin), 0xF000, 0xF004, 0xF0D9, 0xF000, 0x0100 },
    { kStpPlayBin, sizeof(kStpPlayBin), 0xF000, 0xF004, 0xF138, 0xF000, 0x0100 },
    { kSqtPlayBin, sizeof(kSqtPlayBin), 0x4000, 0x4001, 0x4064, 0x4000, 0x45C7 },
};

// frames the firmware plays before the loop flag; -1 = none within 15 min
static long z80Frames(aytime::Fmt f, const std::vector<uint8_t>& m) {
    const Def& d = kDefs[f];
    if (m.size() > (size_t)(STUB - d.modAt)) return -2;
    memset(ram, 0, sizeof(ram));
    memcpy(ram + d.modAt, m.data(), m.size());
    memcpy(ram + d.org, d.bin, d.size);
    memset(&cpu, 0, sizeof(cpu));
    cpu.fetch_opcode = op; cpu.fetch = rd; cpu.read = rd; cpu.write = wr;
    cpu.in = in; cpu.out = out; cpu.nop = rd; cpu.inta = inta;
    z80_power(&cpu, Z_TRUE);
    uint16_t mod2 = 0;
    if (f == aytime::PT3 && m.size() > 16) {
        const uint8_t* t = m.data() + m.size() - 16;
        const uint16_t s1 = t[4] | (t[5] << 8), s2 = t[10] | (t[11] << 8);
        if (!memcmp(t, "PT3!", 4) && !memcmp(t + 6, "PT3!", 4) && !memcmp(t + 12, "02TS", 4) &&
            (uint32_t)s1 + s2 + 16 <= m.size() && !memcmp(m.data() + s1, "Vortex", 6) + !memcmp(m.data() + s1, "ProTracker", 10))
            mod2 = d.modAt + s1;
    }
    ram[d.setup] = f == aytime::PT2 ? 2 : f == aytime::PT3 ? (mod2 ? 16 : 32) : 1;
    if (!call(d.init, d.modAt, mod2)) return -3;
    ram[d.setup] &= 0x3F;
    for (long n = 0; n < 15 * 60 * 50; n++) {
        if (!call(d.play, 0)) return -3;
        if (ram[d.setup] & 0x80) return n;
    }
    return -1;
}

int main(int argc, char** argv) {
    int ok = 0, bad = 0, skip = 0;
    for (int a = 1; a < argc; a++) {
        std::string p = argv[a], e = p.substr(p.find_last_of('.') + 1);
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        aytime::Fmt f;
        if (e == "pt3") f = aytime::PT3; else if (e == "pt2") f = aytime::PT2;
        else if (e == "stc") f = aytime::STC; else if (e == "stp") f = aytime::STP;
        else if (e == "sqt") f = aytime::SQT; else continue;
        FILE* fp = fopen(p.c_str(), "rb");
        if (!fp) continue;
        std::vector<uint8_t> m;
        int c;
        while ((c = fgetc(fp)) != EOF) m.push_back((uint8_t)c);
        fclose(fp);
        if (m.size() < 16 || m.size() > 65536) { skip++; continue; }
        if (f == aytime::PT3 && memcmp(m.data(), "Vortex Tracker II", 17) && memcmp(m.data(), "ProTracker 3.", 13)) { skip++; continue; }
        std::vector<uint8_t> M(65536 + 16, 0);
        memcpy(M.data(), m.data(), m.size());
        const long w = aytime::frames(f, M.data());
        const long z = z80Frames(f, m);
        if (z < 0 && z != -1) { skip++; continue; }
        if (w == z || (z == -1 && w < 0)) ok++;
        else { bad++; printf("MISMATCH %s: walk=%ld z80=%ld (%+ld)\n", p.c_str(), w, z, w - z); }
    }
    printf("ok=%d mismatch=%d skipped=%d\n", ok, bad, skip);
    return bad ? 1 : 0;
}
