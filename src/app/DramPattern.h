// pico-speccy — the DRAM power-on pattern of one 16 KB page, a chunk at a time.
// ESPectrum::powerOnRamFill fills RAM with it at cold boot; a .pss omits pages that
// still hold it and the loader / the converter regenerate them. Depends on nothing,
// so the host test of the converter (tools/pss_export_test.cpp) uses it too.
//
// 0xFF/0x00 alternating every 8 bytes, phase inverted every 64 (the 8x2-character
// checkerboard of a real wake-up), plus ~1/32 bytes with sparse bit flips from a
// fixed-seed xorshift — deterministic per page, deliberately no RNG.
#pragma once
#include <stdint.h>

struct DramPattern {
    uint32_t rnd, a;
    explicit DramPattern(uint32_t page) : rnd(0x9E3779B9u * (page + 1)), a(0) {}
    void next(uint8_t* out, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i, ++a) {
            uint8_t v = (((a >> 3) ^ (a >> 6)) & 1) ? 0x00 : 0xFF;
            rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
            if ((rnd & 0x1F) == 0) v ^= (uint8_t)((rnd >> 8) & (rnd >> 16));
            out[i] = v;
        }
    }
};
