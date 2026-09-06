// Host test for the TS-Conf .spg loader's depackers (src/TsSpgDepack.h):
// every block of every .spg given on the command line is unpacked twice — by
// our bounded port and by UnrealSpeccy's original depack.cpp (fetched from
// tslabs/zx-evo into the build dir by the recipe below) — and the outputs must
// be byte-identical. Also checks the header layout (SPGv1_0.txt) and that no
// block overruns its page.
//
//   curl -sSf https://raw.githubusercontent.com/tslabs/zx-evo/master/pentevo/unreal/Unreal/depack.cpp \
//     | sed -e 's/#include "std.h"//' -e 's/#include "sysdefs.h"//' | tr -d '\r' > /tmp/depack_body.inc
//   g++ -O2 -Wall -Isrc -o /tmp/spg_test tools/spg_test.cpp && /tmp/spg_test debug/TSCONF/*.spg
// (the -Wsequence-point warnings are in Unreal's code, not ours)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include "TsSpgDepack.h"

// Unreal's original, textually included with its typedefs supplied.
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define _CRT_SECURE_NO_WARNINGS
namespace unreal {
#include "/tmp/depack_body.inc"
}

static std::vector<uint8_t> readFile(const char* fn) {
    std::vector<uint8_t> v; FILE* f = fopen(fn, "rb"); if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize(n); if (fread(v.data(), 1, n, f) != (size_t)n) v.clear(); fclose(f); return v;
}

int main(int argc, char** argv) {
    int fails = 0;
    for (int a = 1; a < argc; a++) {
        std::vector<uint8_t> d = readFile(argv[a]);
        if (d.size() < 0x400 || memcmp(&d[0x20], "SpectrumProg", 12)) { printf("%s: not SPG\n", argv[a]); fails++; continue; }
        unsigned nblk = d[0x3A] | (d[0x3B] << 8);
        printf("%s: v%u.%u start=%04X sp=%04X page3=%u clk=%u nblk=%u\n", argv[a], d[0x2C] >> 4, d[0x2C] & 15,
               d[0x30] | (d[0x31] << 8), d[0x32] | (d[0x33] << 8), d[0x34], d[0x35], nblk);
        size_t pos = 0x400; unsigned cnt[4] = {0,0,0,0}; unsigned maxpage = 0;
        std::vector<uint8_t> ours(16384 + 64), ref(65536);
        for (unsigned i = 0; i < 256 && i < nblk; i++) {
            const uint8_t* bd = &d[0x100 + i * 3];
            unsigned off = (bd[0] & 0x1F) << 9, size = ((bd[1] & 0x1F) + 1) << 9, comp = bd[1] >> 6, page = bd[2];
            bool last = bd[0] & 0x80;
            if (pos + size > d.size()) { printf("  block %u: truncated file\n", i); fails++; break; }
            const uint8_t* src = &d[pos]; pos += size;
            cnt[comp]++; if (page > maxpage) maxpage = page;
            if (comp == 1 || comp == 2) {
                memset(ours.data(), 0xAA, ours.size()); memset(ref.data(), 0xAA, ref.size());
                size_t n1 = comp == 1 ? tsspg::demlz(ours.data(), ours.data() + 16384 - off, src, size)
                                      : tsspg::dehrust(ours.data(), ours.data() + 16384 - off, src, size);
                size_t n2;
                if (comp == 1) { unreal::demlz(ref.data(), (u8*)src, size); n2 = 0;
                    // demlz returns nothing: measure by the 0xAA fill boundary on ours' size
                    n2 = n1; }
                else n2 = unreal::dehrust(ref.data(), (u8*)src, size);
                bool same = (n1 == n2) && memcmp(ours.data(), ref.data(), n1) == 0;
                if (comp == 1) same = memcmp(ours.data(), ref.data(), n1) == 0 && ref[n1] == 0xAA;
                if (!same || n1 == 0 || off + n1 > 16384) {
                    printf("  block %u: page %u off %04X comp %u in %u -> ours %zu ref %zu %s\n", i, page, off, comp, size,
                           n1, n2, same ? "" : "MISMATCH"); fails++;
                }
            }
            if (last) break;
        }
        printf("  blocks: raw=%u mlz=%u hrust=%u other=%u, max page %u, data end %zu of %zu\n",
               cnt[0], cnt[1], cnt[2], cnt[3], maxpage, pos, d.size());
    }
    printf(fails ? "FAIL (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
