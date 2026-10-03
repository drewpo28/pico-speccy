// Host test for src/speccy/core/PssExport.cpp (.pss -> .sna / .z80 / .szx).
//
//   g++ -O2 -Wall -Wextra -Itools/pss_host -Isrc -Isrc/speccy/core
//       -o /tmp/pss_export_test tools/pss_export_test.cpp src/speccy/core/PssExport.cpp
//   /tmp/pss_export_test /tmp/pss_test && python3 tools/pss_export.py --check /tmp/pss_test
//
// tools/pss_host/ stands in for FatFs and three firmware headers. The test writes
// synthetic .pss files for every machine phase 1 covers, converts each to every
// format the table allows, checks the facts that can be checked without a second
// implementation (sizes, headers, the 48K SNA's pushed PC, the extended-SNA page
// bits), and leaves the files for tools/pss_export.py --check, which is that second
// implementation and compares byte for byte.
#include "Pss.h"
#include "PssIo.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace std;

// The three Pss.cpp helpers PssExport.cpp uses, without Config: every CFG line is kept.
namespace Pss {
bool openPss(const string& path, FIL*& f, uint8_t hdr[8]) {
    f = fopen2(path.c_str(), FA_READ);
    if (!f) return false;
    UINT br;
    if (f_read(f, hdr, 8, &br) != FR_OK || br != 8 || memcmp(hdr, "PSS1", 4) != 0 || hdr[4] != VER_MAJOR) {
        fclose2(f); f = nullptr; return false;
    }
    return true;
}
void readCfgLines(R& r, uint32_t size, vector<string>& out) {
    string all(size, '\0');
    r.raw(&all[0], size);
    size_t a = 0;
    while (a < all.size()) {
        size_t e = all.find('\n', a);
        if (e == string::npos) e = all.size();
        out.push_back(all.substr(a, e - a));
        a = e + 1;
    }
}
bool cfgValue(const vector<string>& lines, const char* key, string& v) {
    const size_t k = strlen(key);
    for (const string& l : lines)
        if (l.size() > k && l.compare(0, k, key) == 0 && l[k] == '=') { v = l.substr(k + 1); return true; }
    return false;
}
} // namespace Pss

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case {
    const char* name; const char* arch; const char* rom;
    int npages; uint32_t bank; uint8_t video, romL, lock, notMore128, trdos, p1ffd, eff7;
    uint16_t sp, pc; bool ay48, scld, ulaplus, covox, halted;
};

static uint8_t pageByte(int p, int i) { return (uint8_t)(p * 7 + i * 13 + (i >> 8)); }

static void writePss(const string& path, const Case& c) {
    FIL* f = fopen2(path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    Pss::W w(f);
    w.raw("PSS1", 4); w.u8(1); w.u8(0); w.u8(0); w.u8(0);
    char nb[64] = "test"; w.begin("NAME"); w.raw(nb, 64); w.end();
    string cfg = string("cpu_mhz=378\narch=") + c.arch + "\nromSet=" + c.rom + "\nAY48=" +
                 (c.ay48 ? "true" : "false") + "\ndrive0.file=/x/game.trd\n";
    w.begin("CFG "); w.raw(cfg.data(), (UINT)cfg.size()); w.end();
    w.begin("JOY "); w.raw("*\tKempston\tNONE\n", 16); w.end();
    w.begin("Z80R");
    const uint16_t regs[12] = { 0x1234, 0x2345, 0x3456, 0x4567, 0x5678, 0x6789, 0x789A, 0x89AB,
                                0x9ABC, 0xABCD, c.sp, c.pc };
    for (uint16_t r : regs) w.u16(r);
    w.u8(0x3F); w.u8(0xA5); w.u8(1); w.u8(1); w.u8(2);
    w.u32(12345); w.u8(0); w.u8(c.halted ? 2 : 0); w.u16(0xBEEF);
    w.end();
    w.begin("SPCR"); w.u8(5); w.u8(0); w.u8(0); w.u8(0x15); w.u32(0); w.end();
    w.begin("PSPT");
    w.u8(1); w.u32(c.bank); w.u8(c.video); w.u8(c.romL); w.u8(c.lock); w.u8(0); w.u8(0);
    w.u8(0); w.u8(c.notMore128); w.u8(c.trdos); w.u8(c.p1ffd); w.u8(c.eff7); w.u8(0);
    w.u8(0x06); w.u8(0x03); w.u8(0);
    w.end();
    w.begin("AY\0\0"); w.u8(0); w.u8(7); for (int i = 0; i < 16; i++) w.u8((uint8_t)(i * 9)); w.end();
    if (c.scld)    { w.begin("SCLD"); w.u8(0x03); w.u8(0x06); w.end(); }
    if (c.ulaplus) { w.begin("PLTT"); w.u8(1); w.u8(0x40); for (int i = 0; i < 64; i++) w.u8((uint8_t)i); w.u8(1); w.end(); }
    if (c.covox)   { w.begin("COVX"); w.u8(0x80); w.u8(0); w.u8(0); w.u8(0); w.end(); }
    static const uint8_t p48[3] = { 5, 2, 0 };
    vector<uint8_t> pg(16384);
    for (int i = 0; i < c.npages; i++) {
        const int p = c.npages == 3 ? p48[i] : i;
        for (int k = 0; k < 16384; k++) pg[k] = pageByte(p, k);
        w.begin("RAMP"); w.u16(0); w.u8((uint8_t)p); w.raw(pg.data(), 16384); w.end();
    }
    fclose2(f);
}

static vector<uint8_t> readAll(const string& p) {
    vector<uint8_t> v;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); v.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    if (!v.empty() && fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    fclose(f);
    return v;
}

static ArchIdx archOf(const char* a) { return archFromStr(a, A_NONE); }
static RomsetIdx romOf(const char* r) { return romsetFromStr(r, R_NONE); }

int main(int argc, char** argv) {
    const string dir = argc > 1 ? argv[1] : "/tmp/pss_test";
    mkdir(dir.c_str(), 0755);
    const Case cases[] = {
        // name    arch        rom       pg  bank v r l nm tr 1ffd eff7  sp      pc    ay48 scld ula  cov  halt
        { "k48",   "48K",      "48K",     3,  0, 0,0,1, 0, 0, 0, 0,  0x5B00, 0x8000, false,false,false,false,false },
        { "k48b",  "48K",      "48Kby",   3,  0, 0,0,1, 0, 0, 0, 0,  0x4001, 0x1234, true, false,true, true, true  },
        { "k48c",  "48K",      "48K",     3,  0, 0,0,1, 0, 0, 0, 0,  0xC000, 0xABCD, false,false,false,false,false },
        { "tc68",  "48K",      "TC2068",  3,  0, 0,0,1, 0, 0, 0, 0,  0x6000, 0x7000, false,true, false,false,false },
        { "k128",  "128K",     "128K",    8,  3, 1,1,0, 0, 1, 0, 0,  0xFF00, 0x6000, false,false,false,false,false },
        { "plus2", "128K",     "+2",      8,  6, 0,1,1, 0, 0, 0, 0,  0x7000, 0x7100, false,false,false,false,false },
        { "plus3", "128K",     "P3",      8,  7, 0,0,0, 0, 0, 0x05,0, 0x7000, 0x7100, false,false,false,false,false },
        { "plus3e","128K",     "P3e",     8,  1, 0,1,0, 0, 0, 0x04,0, 0x7000, 0x7100, false,false,false,false,false },
        { "pent",  "Pentagon", "128Kp",   8,  6, 0,1,0, 0, 0, 0, 0,  0x7000, 0x7100, false,false,false,false,false },
        { "p512",  "P512",     "128Kp",  32, 21, 1,0,0, 0, 0, 0, 0,  0x7000, 0x7100, false,false,false,false,false },
        { "scorp", "Scorpion", "Scorp",  16, 13, 0,1,0, 0, 1, 0x12,0, 0x7000, 0x7100, false,false,false,false,false },
        { "scorpg","Scorpion", "ScorpGr", 16, 9, 1,0,1, 0, 0, 0x10,0, 0x7000, 0x7100, false,false,false,false,false },
        { "p512b", "P512",     "128Kp",  32, 10, 0,1,0, 0, 0, 0, 0,  0x7000, 0x7100, false,false,false,false,false },
        { "p1024", "P1024",    "128Kp",  64, 37, 0,1,0, 0, 0, 0, 0x10, 0x7000, 0x7100, false,false,false,true, false },
    };
    for (const Case& c : cases) {
        const string base = dir + "/" + c.name;
        for (const char* e : { ".pss", ".szx", ".z80", ".sna" }) remove((base + e).c_str());
        writePss(base + ".pss", c);

        ArchIdx a; RomsetIdx r;
        a = archOf(c.arch); r = romOf(c.rom);
        const uint8_t fm = Pss::exportFormats(a, r);
        CHECK(fm & Pss::EX_SZX, "%s: no .szx", c.name);
        const bool plus3 = !strncmp(c.rom, "P3", 2) || !strcmp(c.arch, "Scorpion");
        const bool bigPent = c.npages > 8 && strcmp(c.arch, "Scorpion") != 0;
        CHECK(!!(fm & Pss::EX_Z80) == !bigPent, "%s: .z80 availability", c.name);
        CHECK(!!(fm & Pss::EX_SNA) == !plus3, "%s: .sna availability", c.name);

        for (Pss::ExportFmt fmt : { Pss::EX_SZX, Pss::EX_Z80, Pss::EX_SNA }) {
            if (!(fm & fmt)) continue;
            const char* ext = fmt == Pss::EX_SZX ? ".szx" : fmt == Pss::EX_Z80 ? ".z80" : ".sna";
            string dropped, err;
            const bool ok = Pss::exportTo(base + ".pss", base + ext, fmt, dropped, err);
            CHECK(ok, "%s%s: %s", c.name, ext, err.c_str());
            if (!ok) continue;
            CHECK(dropped.find("settings") != string::npos && dropped.find("mounted media") != string::npos
                  && dropped.find("joystick") != string::npos, "%s%s: dropped = '%s'", c.name, ext, dropped.c_str());
            const vector<uint8_t> o = readAll(base + ext);
            if (fmt == Pss::EX_SNA) {
                // A current bank of 2 or 5 is written twice (the format's second size).
                const uint32_t cur = c.bank & 7;
                const size_t dup = (cur == 2 || cur == 5) ? 1u : 0u;
                const size_t want = c.npages == 3 ? 49179u
                                  : 27u + 3u * 16384u + 4u + (size_t)(c.npages - 3 + dup) * 16384u;
                CHECK(o.size() == want, "%s.sna: %zu bytes, want %zu", c.name, o.size(), want);
                if (c.npages == 3 && o.size() == want) {
                    const uint16_t sp = (uint16_t)(c.sp - 2);
                    CHECK((o[23] | (o[24] << 8)) == sp, "%s.sna: SP %04X", c.name, o[23] | (o[24] << 8));
                    for (int k = 0; k < 2; k++) {
                        const uint16_t ad = (uint16_t)(sp + k);
                        if (ad < 0x4000) continue;
                        const uint8_t want_b = k ? (uint8_t)(c.pc >> 8) : (uint8_t)c.pc;
                        CHECK(o[27 + ad - 0x4000] == want_b, "%s.sna: pushed PC byte %d", c.name, k);
                    }
                    // A byte the push must not have touched.
                    const uint16_t other = sp >= 0x4002 ? (uint16_t)(sp - 1) : 0x5000;
                    CHECK(o[27 + other - 0x4000] == pageByte(other < 0x8000 ? 5 : other < 0xC000 ? 2 : 0, (other - 0x4000) & 0x3FFF),
                          "%s.sna: neighbour of the pushed PC changed", c.name);
                }
                if (c.npages > 3 && o.size() == want) {
                    const uint8_t port = o[27 + 3 * 16384 + 2];
                    uint32_t b = port & 7;
                    if (c.npages > 8) {
                        if (port & 0x40) b += 8;
                        if (port & 0x80) b += 16;
                        if (c.npages == 64 && (port & 0x20)) b += 32;
                    }
                    CHECK(b == c.bank, "%s.sna: 7FFD %02X decodes to bank %u, want %u", c.name, port, b, c.bank);
                    CHECK(o[27 + 2 * 16384] == pageByte(c.bank & 7, 0), "%s.sna: 3rd block is not bank&7", c.name);
                }
            } else if (fmt == Pss::EX_Z80) {
                const size_t want = 87u + (size_t)c.npages * (3u + 16384u);
                CHECK(o.size() == want, "%s.z80: %zu bytes, want %zu", c.name, o.size(), want);
                if (o.size() == want) {
                    CHECK(o[6] == 0 && o[7] == 0 && o[30] == 55, "%s.z80: not a v3 header", c.name);
                    CHECK((o[32] | (o[33] << 8)) == c.pc, "%s.z80: PC", c.name);
                    if (plus3) CHECK(o[34] == (strcmp(c.arch, "Scorpion") ? 7 : 10) && o[86] == c.p1ffd,
                                     "%s.z80: +3/Scorpion mode, 1FFD", c.name);
                }
            } else {
                CHECK(o.size() > 8 && !memcmp(o.data(), "ZXST", 4), "%s.szx: magic", c.name);
            }
        }
        // A format the table refuses must fail cleanly.
        if (!(fm & Pss::EX_SNA)) {
            string d, e;
            CHECK(!Pss::exportTo(base + ".pss", base + ".bad", Pss::EX_SNA, d, e), "%s: refused .sna converted", c.name);
        }
    }
    // Damage: a truncated file.
    {
        const vector<uint8_t> full = readAll(dir + "/k128.pss");
        FILE* f = fopen((dir + "/trunc.tmp").c_str(), "wb");
        fwrite(full.data(), 1, 1000, f); fclose(f);
        string d, e;
        CHECK(!Pss::exportTo(dir + "/trunc.tmp", dir + "/trunc.szx", Pss::EX_SZX, d, e), "truncated file converted");
        remove((dir + "/trunc.tmp").c_str());
    }
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
