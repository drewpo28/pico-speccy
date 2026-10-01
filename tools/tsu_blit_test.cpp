// Host test for src/speccy/machines/TsConf/TsuBlit.h: the SWAR element blit against the per-nibble
// reference over random elements (both directions, every position incl. the
// 512-pixel wrap, random palettes, random buffer contents, elements with
// forced fully-opaque / fully-transparent / single-nibble patterns).
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/tsu_blit_test tools/tsu_blit_test.cpp && /tmp/tsu_blit_test
#include "speccy/machines/TsConf/TsuBlit.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t rng = 0xC0FFEE11;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// The blit the firmware used until 2026-09-13 (Video.cpp tsuComposeLine blit8).
static uint32_t refBlit8(uint8_t* ts, uint32_t pos, int dir, uint8_t pal, const uint8_t* src) {
    uint32_t s4; memcpy(&s4, src, 4);
    if (!s4) return (uint32_t)((pos + 8 * dir) & 0x1FF);
    for (int i = 0; i < 4; i++) {
        const uint8_t c = src[i];
        if (c & 0xF0) ts[pos] = (uint8_t)(pal | (c >> 4));
        pos = (pos + dir) & 0x1FF;
        if (c & 0x0F) ts[pos] = (uint8_t)(pal | (c & 0x0F));
        pos = (pos + dir) & 0x1FF;
    }
    return pos;
}

int main() {
    static uint8_t a[512 + 16], b[512 + 16];   // +16: guard bytes, must stay untouched
    int fails = 0; long opaque = 0, partial = 0, empty = 0, wraps = 0;
    for (long it = 0; it < 3000000; it++) {
        uint8_t src[4];
        const uint32_t kind = rnd() % 8;
        for (int i = 0; i < 4; i++) src[i] = (uint8_t)rnd();
        if (kind == 0) for (int i = 0; i < 4; i++) src[i] |= 0x11;            // fully opaque
        if (kind == 1) memset(src, 0, 4);                                      // transparent
        if (kind == 2) { memset(src, 0, 4); src[rnd() & 3] = (uint8_t)(((rnd() & 1) ? 0xF0 : 0x0F) & rnd()) | (uint8_t)((rnd() & 1) ? 0x10 : 0x01); }
        if (kind == 3) for (int i = 0; i < 4; i++) src[i] &= (uint8_t)(rnd() & 1 ? 0xF0 : 0x0F);   // half transparent
        for (int i = 0; i < 512 + 16; i++) a[i] = b[i] = (uint8_t)rnd();
        const uint32_t pos = rnd() & 0x1FF;
        const int dir = (rnd() & 1) ? 1 : -1;
        const uint8_t pal = (uint8_t)(rnd() & 0xF0);
        const uint32_t r1 = refBlit8(a, pos, dir, pal, src);
        const uint32_t r2 = tsuBlit8(b, pos, dir, pal, src);
        uint32_t s4; memcpy(&s4, src, 4);
        uint32_t nz = s4 | (s4 >> 1); nz |= nz >> 2; nz &= 0x11111111u;
        if (!s4) empty++; else if (nz == 0x11111111u) opaque++; else partial++;
        const uint32_t start = dir > 0 ? pos : ((pos - 7u) & 0x1FF);
        if (start > 504 && s4) wraps++;
        if (r1 != r2 || memcmp(a, b, sizeof a) != 0) {
            fails++;
            if (fails < 10) {
                printf("FAIL pos=%u dir=%d pal=%02X src=%02X%02X%02X%02X ret %u/%u\n", pos, dir, pal, src[0], src[1], src[2], src[3], r1, r2);
                for (int i = 0; i < 512 + 16; i++) if (a[i] != b[i]) printf("  [%d] ref %02X got %02X\n", i, a[i], b[i]);
            }
        }
    }
    printf("opaque=%ld partial=%ld empty=%ld wraps=%ld fails=%d\n", opaque, partial, empty, wraps, fails);
    return fails ? 1 : 0;
}
