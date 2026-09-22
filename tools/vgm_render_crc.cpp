/*
 * vgm_render_crc.cpp — render a .vgm/.vgz through the OPL3 (OplFm) and OPLL
 * (OpllFm) cores on the host and print a CRC32 of the output. A regression
 * gate for changes that must be BIT-EXACT (table → arithmetic rewrites, EG
 * shortcuts, -O level changes): take the CRC before, compare after.
 *
 * Build:
 *   g++ -O2 -Wall -Isrc -o /tmp/vgm_render_crc tools/vgm_render_crc.cpp \
 *       src/OplFm.cpp src/OpllFm.cpp src/FmTables.cpp -lz
 * Run:
 *   /tmp/vgm_render_crc file.vgm [file2.vgz ...]
 *
 * Per file it prints two lines: full-rate and half-rate synthesis (the
 * <450 MHz path), each with the CRC and how many samples were non-zero. VGM
 * commands understood: 0x5E/0x5F (YMF262 set 1/2), 0x51 (YM2413), waits
 * 0x61/0x62/0x63/0x7n; everything else is skipped by its documented length.
 * Timing: VGM waits are 44100 Hz sample counts, converted to 31250 Hz output
 * samples by fixed-point accumulation — deterministic, which is all a CRC
 * needs (the plugin's own pacing is not reproduced here).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <vector>
#include <string>

#include "OplFm.h"
#include "OpllFm.h"

#define RATE 31250

namespace Debug { void log(const char*, ...) {} }
extern "C" void* tryMalloc(size_t n) { return malloc(n); }
extern "C" void* tryCalloc(size_t n) { return calloc(n, 1); }

static uint32_t crc_tab[256];
static void crc_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}
static uint32_t crc_upd(uint32_t crc, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n--) crc = crc_tab[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return crc;
}

static bool read_file(const char* path, std::vector<uint8_t>& out) {
    gzFile f = gzopen(path, "rb");          // transparently reads plain files too
    if (!f) return false;
    uint8_t buf[65536];
    int n;
    while ((n = gzread(f, buf, sizeof(buf))) > 0) out.insert(out.end(), buf, buf + n);
    gzclose(f);
    return out.size() >= 0x40 && memcmp(out.data(), "Vgm ", 4) == 0;
}

struct Render {
    OplFm  opl;
    OpllFm opll;
    int16_t l[RATE], r[RATE], m[RATE];
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t nonzero = 0, total = 0;
    bool oplUsed = false, opllUsed = false;

    void gen(int n) {
        while (n > 0) {
            int c = n > RATE ? RATE : n;
            memset(l, 0, c * 2); memset(r, 0, c * 2); memset(m, 0, c * 2);
            if (oplUsed)  opl.gen(l, r, c, 0);
            if (opllUsed) opll.gen(m, c, 0);
            for (int i = 0; i < c; i++) {
                int16_t v[3] = { l[i], r[i], m[i] };
                crc = crc_upd(crc, v, sizeof(v));
                if (v[0] || v[1] || v[2]) nonzero++;
            }
            total += c;
            n -= c;
        }
    }
};

static int cmd_len(uint8_t c) {
    if (c >= 0x30 && c <= 0x3F) return 2;
    if (c >= 0x40 && c <= 0x4E) return 3;
    if (c == 0x4F || c == 0x50) return 2;
    if (c >= 0x51 && c <= 0x5F) return 3;
    if (c == 0x61) return 3;
    if (c == 0x62 || c == 0x63 || c == 0x66) return 1;
    if (c >= 0x70 && c <= 0x8F) return 1;
    if (c == 0x90 || c == 0x91 || c == 0x95) return 5;
    if (c == 0x92) return 6;
    if (c == 0x93) return 11;
    if (c == 0x94) return 2;
    if (c >= 0xA0 && c <= 0xBF) return 3;
    if (c >= 0xC0 && c <= 0xDF) return 4;
    if (c >= 0xE0) return 5;
    return 1;
}

static void run(const std::vector<uint8_t>& d, bool half, const char* name) {
    uint32_t ver  = *(const uint32_t*)&d[0x08];
    uint32_t doff = ver >= 0x150 ? *(const uint32_t*)&d[0x34] : 0;
    size_t pos = doff ? 0x34 + doff : 0x40;

    Render* R = new Render();
    R->opl.setRates(OPL3_YMF262_CLOCK, RATE, half);   R->opl.reset();
    R->opll.setRates(OPLL_YM2413_CLOCK, RATE, half);  R->opll.reset();

    // 44100 -> 31250 in 32.32 fixed point
    const uint64_t step = ((uint64_t)RATE << 32) / 44100;
    uint64_t acc = 0;
    auto wait = [&](uint32_t n44) {
        acc += step * n44;
        int out = (int)(acc >> 32);
        acc &= 0xFFFFFFFFu;
        if (out) R->gen(out);
    };

    while (pos < d.size()) {
        uint8_t c = d[pos];
        if (c == 0x66) break;
        if (c == 0x67) {                       // data block: 67 66 tt ss ss ss ss
            if (pos + 7 > d.size()) break;
            uint32_t sz = *(const uint32_t*)&d[pos + 3] & 0x7FFFFFFF;
            pos += 7 + sz;
            continue;
        }
        int len = cmd_len(c);
        if (pos + len > d.size()) break;
        switch (c) {
        case 0x5E: R->oplUsed = true; R->opl.write(0, d[pos + 1]); R->opl.write(1, d[pos + 2]); break;
        case 0x5F: R->oplUsed = true; R->opl.write(2, d[pos + 1]); R->opl.write(3, d[pos + 2]); break;
        case 0x51: R->opllUsed = true; R->opll.writeAddr(d[pos + 1]); R->opll.writeData(d[pos + 2]); break;
        case 0x61: wait(d[pos + 1] | (d[pos + 2] << 8)); break;
        case 0x62: wait(735); break;
        case 0x63: wait(882); break;
        default:
            if (c >= 0x70 && c <= 0x7F) wait((c & 15) + 1);
            break;
        }
        pos += len;
    }
    wait(44100);                                // let the tail decay
    printf("%s  %-4s  crc=%08X  samples=%llu  nonzero=%llu  chips=%s%s\n",
           half ? "half" : "full", "", R->crc ^ 0xFFFFFFFFu,
           (unsigned long long)R->total, (unsigned long long)R->nonzero,
           R->oplUsed ? "OPL3 " : "", R->opllUsed ? "OPLL" : "");
    (void)name;
    delete R;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.vgm [...]\n", argv[0]); return 2; }
    crc_init();
    for (int i = 1; i < argc; i++) {
        std::vector<uint8_t> d;
        if (!read_file(argv[i], d)) { printf("%s: not a VGM\n", argv[i]); continue; }
        printf("== %s\n", argv[i]);
        run(d, false, argv[i]);
        run(d, true, argv[i]);
    }
    return 0;
}
