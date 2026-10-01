// Host test for src/speccy/machines/TsConf/TsDramCache.h — the TS-Conf CPU cache in its per-window ROW
// representation, checked against a plain tag model (the shape the firmware
// used until 2026-09-13: hit iff the window is RAM with its cache enabled and
// g_ts_cache_tag[a[8:1]] == {page, a[13:9]}). Random reads, writes, page
// changes (from a small pool so windows alias each other often), W0_RAM and
// CacheConfig toggles, resets. Every read's verdict and every observable tag
// must agree.
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/tsdram_cache_test tools/tsdram_cache_test.cpp && /tmp/tsdram_cache_test
#include "speccy/machines/TsConf/TsDramCache.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t* g_ts_hitrow[4];
uint8_t* g_ts_invrow[4];
uint16_t g_ts_cache_tag[256];
uint16_t g_ts_tagbase[4];
uint8_t  tsdc_row[TSDC_ROWS][256];
uint8_t  tsdc_none[256];
uint16_t tsdc_row_page[TSDC_ROWS];
uint32_t tsdc_row_use[TSDC_ROWS];
uint8_t  tsdc_page_row[256];
static int cold_calls = 0;

// --- reference: tags only ---------------------------------------------------
struct Ref {
    uint16_t tag[256];
    uint8_t  page[4];
    bool     w0ram;
    uint8_t  cc;
    bool ram(uint32_t w) const { return w != 0 || w0ram; }
    uint16_t want(uint16_t a) const { return (uint16_t)(0x8000u | ((uint32_t)page[a >> 14] << 5) | ((a >> 9) & 0x1F)); }
    bool noDram(uint16_t a) const {
        const uint32_t w = a >> 14;
        if (!ram(w)) return true;
        if (!((cc >> w) & 1)) return false;
        return tag[(a >> 1) & 0xFF] == want(a);
    }
    bool fill(uint16_t a) {   // returns "DRAM request made"
        if (!ram(a >> 14)) return false;
        tag[(a >> 1) & 0xFF] = want(a);
        return true;
    }
    void inv(uint16_t a) {
        if (!ram(a >> 14)) return;
        if (tag[(a >> 1) & 0xFF] == want(a)) tag[(a >> 1) & 0xFF] = 0;
    }
};

static uint32_t rng = 0x12345678;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void apply(const Ref& r) { tsdcRecalc(r.page, r.w0ram, r.cc); }

// The model's read: what the firmware does in peek8 (test) + cpuMemMiss (fill).
static bool modelRead(uint16_t a) {
    if (tsMemNoDram(a)) return true;
    return !tsdcFill(a) ? true : false;   // ROM -> no DRAM; RAM miss -> DRAM
}

int main() {
    static const uint8_t pool[] = { 0, 2, 3, 5, 7, 0x80, 0xFF, 1, 9, 10, 11, 12 };   // 12 pages > TSDC_ROWS: aliases AND pool evictions
    Ref r{};
    tsdcReset();
    r.w0ram = false; r.cc = 0x0F;
    r.page[0] = 2; r.page[1] = 5; r.page[2] = 2; r.page[3] = 0;
    apply(r);

    long reads = 0, hits = 0, writes = 0, remaps = 0, aliasSteps = 0;
    for (long it = 0; it < 4000000; it++) {
        const uint32_t op = rnd() % 1000;
        if (op < 4) {                            // remap one window
            const uint32_t w = rnd() & 3;
            r.page[w] = pool[rnd() % (sizeof pool)];
            apply(r); remaps++;
        } else if (op < 6) {                     // W0_RAM toggle (Bomberman's bracket)
            r.w0ram = !r.w0ram; apply(r);
        } else if (op < 7) {                     // CacheConfig
            r.cc = (uint8_t)(rnd() & 0x0F); apply(r);
        } else if (op == 7 && (rnd() & 0xFF) == 0) {   // full reset
            tsdcReset(); memset(r.tag, 0, sizeof r.tag); apply(r);
        } else if (op < 700) {                   // read
            // bias toward a few hot 512-byte blocks so hits actually happen
            uint16_t a = (rnd() & 1) ? (uint16_t)rnd() : (uint16_t)((rnd() & 0xC1FF) | ((rnd() & 3) << 9));
            const bool ref = r.noDram(a);
            const bool got = tsMemNoDram(a);
            // ROM: the model answers "miss" and lets the cold path say no-DRAM
            if (!r.ram(a >> 14)) CHECK(!got, "ROM read a=%04X reported a row hit", a);
            else CHECK(got == ref, "read a=%04X model=%d ref=%d (w=%u page=%u cc=%X)", a, got, ref, a >> 14, r.page[a >> 14], r.cc);
            const bool mnd = modelRead(a);
            if (!ref) r.fill(a);
            CHECK(mnd == ref, "modelRead a=%04X got=%d ref=%d", a, mnd, ref);
            reads++; if (ref) hits++;
        } else {                                 // write
            uint16_t a = (rnd() & 1) ? (uint16_t)rnd() : (uint16_t)((rnd() & 0xC1FF) | ((rnd() & 3) << 9));
            const int cc0 = cold_calls;
            { int same = 0; for (int i = 0; i < 4; i++) for (int k = i + 1; k < 4; k++) if (r.page[i] == r.page[k] && r.ram(i) && r.ram(k)) same = 1; if (same) aliasSteps++; }
            // the firmware's shape: the leaf half first, the full rule when it hits
            if (rnd() & 1) { if (tsMemWriteHit(a)) { cold_calls++; tsMemWriteInv(a); } }   // the leaf shape
            else tsMemWriteInv(a);
            r.inv(a); writes++;
            (void)cc0;
        }
        if ((it & 0xFFF) == 0) {                 // the tags themselves must agree, and the rows with the tags
            for (int i = 0; i < 256; i++) CHECK(g_ts_cache_tag[i] == r.tag[i], "tag[%d] model=%04X ref=%04X", i, g_ts_cache_tag[i], r.tag[i]);
            for (uint32_t rr = 0; rr < TSDC_ROWS; rr++) {
                if (tsdc_row_page[rr] == 0xFFFF) continue;
                CHECK(tsdc_page_row[tsdc_row_page[rr] & 0xFF] == rr, "page_row[%u] != row %u", tsdc_row_page[rr], rr);
                for (int i = 0; i < 256; i++) {
                    const uint16_t t = g_ts_cache_tag[i];
                    const uint8_t want = ((t & 0xFFE0u) == (0x8000u | ((uint32_t)tsdc_row_page[rr] << 5))) ? (uint8_t)(t & 0x1F) : 0xFF;
                    CHECK(tsdc_row[rr][i] == want, "row[%u][%d]=%02X want %02X (page %u)", rr, i, tsdc_row[rr][i], want, tsdc_row_page[rr]);
                }
            }
            for (int pg = 0; pg < 256; pg++) if (tsdc_page_row[pg] != 0xFF) CHECK(tsdc_row_page[tsdc_page_row[pg]] == pg, "page_row[%d] -> row %u holds %u", pg, tsdc_page_row[pg], tsdc_row_page[tsdc_page_row[pg]]);
            for (uint32_t w = 0; w < 4; w++) {   // every RAM window points at its page's row
                if (!r.ram(w)) { CHECK(g_ts_hitrow[w] == tsdc_none && g_ts_invrow[w] == tsdc_none, "ROM window %u has a row", w); continue; }
                const uint8_t rw = tsdc_page_row[r.page[w]];
                CHECK(rw != 0xFF, "window %u page %u not in the pool", w, r.page[w]);
                if (rw != 0xFF) CHECK(g_ts_invrow[w] == tsdc_row[rw], "window %u invrow != its page's row", w);
            }
            for (int i = 0; i < 256; i++) CHECK(tsdc_none[i] == 0xFF, "tsdc_none[%d] clobbered", i);
        }
    }
    printf("reads=%ld hits=%ld writes=%ld remaps=%ld aliasWrites=%ld leafHits=%d fails=%d\n", reads, hits, writes, remaps, aliasSteps, cold_calls, fails);
    CHECK(hits > reads / 20, "too few hits to mean anything");
    CHECK(aliasSteps > 1000, "alias path not exercised");
    CHECK(cold_calls > 100, "leaf test never hit");
    return fails ? 1 : 0;
}
