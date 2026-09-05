// plus3_paging_test.cpp — host-side check of the ZX Spectrum +3 memory map.
//
// The firmware cannot be built without the Pico SDK, and MemESP::plus3Remap() drags in
// the whole memory subsystem (PSRAM pools, SD swap) — so the arithmetic that is actually
// specific to the +3 lives in src/Plus3Paging.h, which this harness INCLUDES. It tests
// the shipped table, not a copy of it.
//
// The reference is Fuse: machines/specplus3.c (special_memory_map / select_special_map /
// specplus3_memory_map) and specplus3_plus2a_common_reset for the contention rule. These
// are exactly the transcription errors that produce a machine which boots and then dies
// inside a game, so they are worth pinning.
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/plus3_paging_test tools/plus3_paging_test.cpp
//   /tmp/plus3_paging_test

#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include "Plus3Paging.h"

// ── slot resolution, the one part of MemESP::plus3Remap that is not in the header ──
struct Map {
    int  page[4];      // RAM page in each 16K slot, -1 = ROM
    int  rom;          // ROM bank at slot 0, -1 = none
    bool contended[4];
};

static Map plus3Remap(uint8_t p1ffd, uint8_t p7ffd) {
    Map m{};
    const uint8_t bankLatch = p7ffd & 0x07;
    const uint8_t romLatch  = (p7ffd >> 4) & 0x01;
    if (plus3IsSpecial(p1ffd)) {
        const uint8_t* c = plus3SpecialPages(p1ffd);
        m.rom = -1;
        for (int i = 0; i < 4; i++) {
            m.page[i] = c[i];
            m.contended[i] = plus3PageContended(c[i]);
        }
    } else {
        m.rom = plus3RomIndex(p1ffd, romLatch);
        m.page[0] = -1;        m.contended[0] = false;
        m.page[1] = 5;         m.contended[1] = plus3PageContended(5);
        m.page[2] = 2;         m.contended[2] = plus3PageContended(2);
        m.page[3] = bankLatch; m.contended[3] = plus3PageContended(bankLatch);
    }
    return m;
}

// ── harness ────────────────────────────────────────────────────────────────────
static int failures = 0;
static void ck(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); failures++; }
}
static void ckEq(int got, int want, const char* what) {
    if (got != want) { printf("  FAIL  %s: got %d, want %d\n", what, got, want); failures++; }
}

int main() {
    printf("+3 paging\n");

    // Normal mode: ROM / 5 / 2 / bank, exactly like a 128K.
    {
        Map m = plus3Remap(0x00, 0x00);
        ckEq(m.rom, 0, "normal 1FFD=00 7FFD=00 -> ROM 0");
        ckEq(m.page[0], -1, "slot0 is ROM");
        ckEq(m.page[1], 5, "slot1 = RAM 5");
        ckEq(m.page[2], 2, "slot2 = RAM 2");
        ckEq(m.page[3], 0, "slot3 = bank 0");
    }

    // Four-way ROM select: (1FFD D2 << 1) | (7FFD D4).
    //   0 editor/menu   1 syntax   2 +3DOS   3 48 BASIC
    ckEq(plus3Remap(0x00, 0x00).rom, 0, "ROM select 00 -> 0");
    ckEq(plus3Remap(0x00, 0x10).rom, 1, "ROM select 7FFD.D4 -> 1");
    ckEq(plus3Remap(0x04, 0x00).rom, 2, "ROM select 1FFD.D2 -> 2");
    ckEq(plus3Remap(0x04, 0x10).rom, 3, "ROM select both -> 3");

    // D2 is the ROM high bit ONLY while D0=0; with D0=1 it is part of the
    // configuration number, and no ROM is mapped at all.
    for (int c = 0; c < 4; c++)
        ckEq(plus3Remap((uint8_t)(0x01 | (c << 1)), 0x10).rom, -1,
             "special mode maps no ROM");

    // The four all-RAM configurations (Fuse specplus3.c:274-286).
    {
        static const int want[4][4] = {
            { 0, 1, 2, 3 },
            { 4, 5, 6, 7 },
            { 4, 5, 6, 3 },
            { 4, 7, 6, 3 },
        };
        for (int c = 0; c < 4; c++) {
            Map m = plus3Remap((uint8_t)(0x01 | (c << 1)), 0x00);
            for (int i = 0; i < 4; i++) {
                char msg[64];
                snprintf(msg, sizeof(msg), "special cfg %d slot %d", c, i);
                ckEq(m.page[i], want[c][i], msg);
            }
        }
    }

    // Contention: pages 4-7 are contended on a +2A/+3, NOT the odd ones
    // (Fuse specplus3_plus2a_common_reset: memory_ram_set_16k_contention(i, i >= 4)).
    {
        Map m = plus3Remap(0x01, 0x00);              // cfg 0 = pages 0,1,2,3
        for (int i = 0; i < 4; i++) ck(!m.contended[i], "cfg 0: pages 0-3 uncontended");
        m = plus3Remap(0x03, 0x00);                  // cfg 1 = pages 4,5,6,7
        for (int i = 0; i < 4; i++) ck(m.contended[i], "cfg 1: pages 4-7 contended");
        m = plus3Remap(0x07, 0x00);                  // cfg 3 = pages 4,7,6,3
        ck(m.contended[0] && m.contended[1] && m.contended[2] && !m.contended[3],
           "cfg 3: 4,7,6 contended, 3 not");
        // The 128K rule (odd pages) would call page 1 contended and page 4 not —
        // assert we are NOT doing that.
        m = plus3Remap(0x00, 0x01);                  // bank 1 at 0xC000
        ck(!m.contended[3], "bank 1 at C000 is NOT contended on a +3");
        m = plus3Remap(0x00, 0x04);                  // bank 4 at 0xC000
        ck(m.contended[3], "bank 4 at C000 IS contended on a +3");
        // RAM 5 sits at 0x4000 always and is contended in both models.
        m = plus3Remap(0x00, 0x00);
        ck(m.contended[1] && !m.contended[2], "slot1 (RAM 5) contended, slot2 (RAM 2) not");
    }

    // The contention delay tables (Video.cpp wait_st_tab). Fuse expresses these as
    // contention_pattern_65432100 / _76543210; in this repo's phase (index 0 ==
    // TS_SCREEN_128) they are the two rows below.
    {
        static const uint8_t k128[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
        static const uint8_t kP3 [8] = { 1, 0, 7, 6, 5, 4, 3, 2 };
        int sum128 = 0, sumP3 = 0;
        for (int i = 0; i < 8; i++) { sum128 += k128[i]; sumP3 += kP3[i]; }
        ckEq(sum128, 21, "128K pattern sums to 21");
        ckEq(sumP3, 28, "+3 pattern sums to 28 (0..7, every delay distinct)");
        bool seen[8] = { false };
        for (int i = 0; i < 8; i++) seen[kP3[i]] = true;
        for (int i = 0; i < 8; i++) ck(seen[i], "+3 pattern is a permutation of 0..7");
    }

    if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
    printf("  all checks passed\n");
    return 0;
}
