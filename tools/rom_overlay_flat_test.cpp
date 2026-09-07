// Host check: rom_overlay_flatten() must produce, for every offset, exactly the
// byte rom_overlay_byte() resolves — MemESP aims a bank pointer at the flat page
// and skips the binary search, so the two must never disagree.
//   g++ -O2 -Wall -Isrc -o /tmp/rof tools/rom_overlay_flat_test.cpp && /tmp/rof
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "RomOverlay.h"

static uint32_t rng = 0x12345678u;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main() {
    const uint32_t ROM = 16384;
    int fails = 0;
    for (int trial = 0; trial < 200; trial++) {
        std::vector<uint8_t> base(ROM);
        for (auto& b : base) b = (uint8_t)rnd();
        // random sorted, non-overlapping runs
        uint32_t nruns = rnd() % 64;
        std::vector<uint32_t> starts, lens;
        uint32_t pos = 0;
        for (uint32_t r = 0; r < nruns && pos < ROM - 2; r++) {
            uint32_t gap = rnd() % 300, len = 1 + rnd() % 200;
            if (pos + gap + len > ROM) break;
            starts.push_back(pos + gap); lens.push_back(len); pos += gap + len;
        }
        nruns = starts.size();
        std::vector<uint8_t> ov(ROM_OVERLAY_HDR + nruns * ROM_OVERLAY_RUN);
        memcpy(&ov[0], "RPO1", 4);
        ov[4] = ROM & 0xFF; ov[5] = ROM >> 8; ov[6] = 0; ov[7] = 0;
        ov[8] = nruns & 0xFF; ov[9] = nruns >> 8; ov[10] = 0; ov[11] = 0;
        uint32_t dof = 0;
        for (uint32_t r = 0; r < nruns; r++) {
            uint8_t* rr = &ov[ROM_OVERLAY_HDR + r * ROM_OVERLAY_RUN];
            rr[0] = starts[r] & 0xFF; rr[1] = starts[r] >> 8;
            rr[2] = lens[r] & 0xFF;   rr[3] = lens[r] >> 8;
            rr[4] = dof & 0xFF;       rr[5] = dof >> 8;
            dof += lens[r];
        }
        for (uint32_t i = 0; i < dof; i++) ov.push_back((uint8_t)rnd());
        std::vector<uint8_t> flat(ROM);
        rom_overlay_flatten(ov.data(), base.data(), flat.data());
        for (uint32_t off = 0; off < ROM; off++) {
            uint8_t want = rom_overlay_byte(ov.data(), base.data(), (uint16_t)off);
            if (flat[off] != want) { if (fails < 5) printf("trial %d off %u: flat %02X want %02X\n", trial, off, flat[off], want); fails++; }
        }
    }
    printf(fails ? "FAIL (%d mismatches)\n" : "OK: rom_overlay_flatten == rom_overlay_byte over 200 random overlays\n", fails);
    return fails ? 1 : 0;
}
