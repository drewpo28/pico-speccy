// TS-Conf CPU cache (zmem.v: 256 x 16-bit words, index a[8:1], tag {page,
// a[13:9]}) — the DRAM model's hit test, in the shape the per-access hot path
// pays for. Firmware-free so tools/tsdram_cache_test.cpp can drive it against a
// plain tag model; TsConf.cpp defines the state, the test defines its own.
//
// Representation (2026-09-13, the host-cost session): the truth is still one
// tag per cache index (g_ts_cache_tag), but the per-access test no longer
// derives the expected tag from the window's page — it reads a per-WINDOW ROW,
// tsdc_row[w][idx] = a[13:9] of the word the row's page has cached at idx, or
// 0xFF. So a hit is one byte load and one compare on the address bits alone:
//
//     g_ts_hitrow[addr >> 14][(addr >> 1) & 0xFF] == (addr >> 9) & 0x1F
//
// 8 instructions, two scratch registers, against 11 and a push {r4} for the
// tagbase | tag form (fishbone at 14 MHz lost 3 FPS to it). g_ts_hitrow[w]
// points at the window's row while the window is RAM with its cache enabled
// and at tsdc_none (all 0xFF, never matches) otherwise — ROM in window 0 and a
// CacheConfig toggle are pointer swaps, not rebuilds. g_ts_invrow[w] is the
// same for the WRITE side, where a RAM window invalidates whether or not its
// cache is enabled (cache_inv follows cpu_strobe, not cache_en).
//
// Rows are PER PHYSICAL PAGE, from a pool of TSDC_ROWS (2026-09-13 evening, after
// demo 200 showed 640 window-page changes a FRAME — a per-window row had to be
// rebuilt on every one, ~2.5 ms/frame): a window points at its page's row, so
// re-mapping a window to a page already in the pool is a pointer swap and only a
// page NEW to the pool rebuilds one row (256 entries, ~1500 cycles). Two windows
// on one page share one row, so there is no alias case at all. tsdc_page_row
// maps page -> pool row (0xFF = none), which is also how a fill clears the entry
// of the page the tag used to hold.
#pragma once
#include <stdint.h>
#include <string.h>

#define TSDC_ROWS 8                          // pool of per-page rows (256 B each)
extern uint8_t* g_ts_hitrow[4];      // read test: the window page's row, or tsdc_none
extern uint8_t* g_ts_invrow[4];      // write invalidate: the row while RAM, else tsdc_none
extern uint16_t g_ts_cache_tag[256]; // 0 = invalid, else 0x8000 | page << 5 | a[13:9]
extern uint16_t g_ts_tagbase[4];     // cold path: 0 = ROM (no DRAM), else 0x8000 | page << 5 [| 0x4000 cache off]
extern uint8_t  tsdc_row[TSDC_ROWS][256];
extern uint8_t  tsdc_none[256];      // all 0xFF
extern uint16_t tsdc_row_page[TSDC_ROWS];  // page each pool row holds, 0xFFFF = free
extern uint32_t tsdc_row_use[TSDC_ROWS];   // last time the row was bound to a window (LRU)
extern uint8_t  tsdc_page_row[256];  // page -> pool row, 0xFF = not in the pool

#define TSDC_INLINE static inline __attribute__((always_inline))

// True when the access costs no DRAM cycle: a hit in an enabled cache window.
// ROM answers false here and 0 in tsdcFill — one cold call per ROM access
// instead of a third load on every access (in the whole-line video modes where
// this path is hot, ROM code is TS-BIOS Setup and nothing else).
TSDC_INLINE bool tsMemNoDram(uint16_t addr) {
    return g_ts_hitrow[addr >> 14][(addr >> 1) & 0xFF] == (uint8_t)((addr >> 9) & 0x1F);
}

// A CPU write invalidates the cached word (cache_inv) — no wait; the DMA steal
// is the caller's business. Two halves:
//  - tsMemWriteHit: the TEST alone, for the leaf fast paths. A write that hits
//    is the exception (the 256-word cache is swept by every 512 bytes of code,
//    so a word rarely survives from its read to its write), and the leaf
//    tail-calls its cold path on it — the invalidate itself needs the tag
//    array's address plus a zero and a 0xFF in registers, which cost poke8 a
//    push/pop pair on EVERY write when it was inlined (2026-09-13).
//  - tsMemWriteInv: the full rule; what the cold paths run. The window's row IS
//    the page's row, so clearing it clears every window mapping that page.
TSDC_INLINE bool tsMemWriteHit(uint16_t addr) {
    return g_ts_invrow[addr >> 14][(addr >> 1) & 0xFF] == (uint8_t)((addr >> 9) & 0x1F);
}
TSDC_INLINE void tsMemWriteInv(uint16_t addr) {
    if (__builtin_expect(tsMemWriteHit(addr), 0)) {
        const uint32_t idx = (addr >> 1) & 0xFF;
        g_ts_cache_tag[idx] = 0;
        g_ts_invrow[addr >> 14][idx] = 0xFF;
    }
}

// --- cold side -------------------------------------------------------------

// The miss: a CPU read of RAM that goes to DRAM fills the tag (cpu_strobe). The
// page the tag held before loses its row entry, the new page (if its row is in
// the pool) gains one. Returns false for ROM (no DRAM request at all).
static inline bool tsdcFill(uint16_t addr) {
    const uint16_t tb = g_ts_tagbase[addr >> 14];
    if (!tb) return false;
    const uint32_t idx = (addr >> 1) & 0xFF;
    const uint8_t  a   = (uint8_t)((addr >> 9) & 0x1F);
    const uint32_t page = (tb >> 5) & 0xFF;
    const uint16_t old = g_ts_cache_tag[idx];
    if (old) {
        const uint8_t ro = tsdc_page_row[(old >> 5) & 0xFF];
        if (ro != 0xFF) tsdc_row[ro][idx] = 0xFF;
    }
    g_ts_cache_tag[idx] = (uint16_t)((tb & ~0x4000u) | a);
    const uint8_t r = tsdc_page_row[page];
    if (r != 0xFF) tsdc_row[r][idx] = a;
    return true;
}

// Bind pool row r to `page`: rebuild it from the tags (the previous holder, if
// any, leaves the pool).
static inline void tsdcBindRow(uint32_t r, uint16_t page) {
    if (tsdc_row_page[r] != 0xFFFF) tsdc_page_row[tsdc_row_page[r] & 0xFF] = 0xFF;
    const uint16_t want = (uint16_t)(0x8000u | ((uint32_t)page << 5));
    for (uint32_t idx = 0; idx < 256; idx++) {
        const uint16_t t = g_ts_cache_tag[idx];
        tsdc_row[r][idx] = ((t & 0xFFE0u) == want) ? (uint8_t)(t & 0x1F) : 0xFF;
    }
    tsdc_row_page[r] = page;
    tsdc_page_row[page & 0xFF] = (uint8_t)r;
}

// The bank map / W0_RAM / CacheConfig may have changed. bank_phys[w] = the
// physical page in window w (for a ROM window 0 that is the ROM page number and
// is ignored). Returns the number of rows rebuilt (PERF).
static inline uint32_t tsdcRecalc(const uint8_t bank_phys[4], bool w0_ram, uint8_t cacheconf) {
    static uint32_t clock = 0;
    uint32_t rebuilt = 0;
    uint8_t rows[4];                                   // pool row per RAM window, 0xFF = ROM
    for (uint32_t w = 0; w < 4; w++) {
        const bool ram = (w != 0) || w0_ram;
        rows[w] = 0xFF;
        if (!ram) { g_ts_tagbase[w] = 0; g_ts_hitrow[w] = g_ts_invrow[w] = tsdc_none; continue; }
        const uint16_t page = bank_phys[w];
        const bool en = (cacheconf >> w) & 1;
        g_ts_tagbase[w] = (uint16_t)(0x8000u | ((uint32_t)page << 5) | (en ? 0 : 0x4000u));
        uint8_t r = tsdc_page_row[page & 0xFF];
        if (r == 0xFF) {
            // Not in the pool: take a free row, else the least recently bound one
            // that no OTHER window is using right now (4 windows, 8 rows: always one).
            uint32_t best = 0xFF, bestUse = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < TSDC_ROWS; i++) {
                if (tsdc_row_page[i] == 0xFFFF) { best = i; break; }
                bool inUse = false;
                for (uint32_t v = 0; v < 4; v++)
                    if (v != w && (v < w ? rows[v] == i : (bank_phys[v] == tsdc_row_page[i] && ((v != 0) || w0_ram)))) inUse = true;
                if (!inUse && tsdc_row_use[i] < bestUse) { bestUse = tsdc_row_use[i]; best = i; }
            }
            tsdcBindRow(best, page); rebuilt++;
            r = (uint8_t)best;
        }
        rows[w] = r;
        tsdc_row_use[r] = ++clock;
        g_ts_hitrow[w] = en ? tsdc_row[r] : tsdc_none;
        g_ts_invrow[w] = tsdc_row[r];
    }
    return rebuilt;
}

// Power-up / reset: the cache comes up invalid, the pool empty.
static inline void tsdcReset() {
    memset(g_ts_cache_tag, 0, sizeof g_ts_cache_tag);
    memset(tsdc_row, 0xFF, sizeof tsdc_row);
    memset(tsdc_none, 0xFF, sizeof tsdc_none);
    memset(tsdc_page_row, 0xFF, sizeof tsdc_page_row);
    for (uint32_t r = 0; r < TSDC_ROWS; r++) { tsdc_row_page[r] = 0xFFFF; tsdc_row_use[r] = 0; }
    for (uint32_t w = 0; w < 4; w++) { g_ts_hitrow[w] = g_ts_invrow[w] = tsdc_none; g_ts_tagbase[w] = 0; }
}
