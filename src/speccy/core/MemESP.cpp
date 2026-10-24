/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or 
visit https://zxespectrum.speccy.org/contacto

*/

#include "speccy/machines/Next/NextReg.h"
#include "MemESP.h"
#include "app/Debug.h"
#include "speccy/z80/z80.h"
#include "speccy/z80/CPU.h"
#include "app/Buffer.h"
#include "app/TryAlloc.h"
#include "app/CodeOverlay.h"   // NX_OVL_CODE (applyMMU)
#include "speccy/video/Video.h" // VIDEO::tsFastMemRecalc (next_stale_mask)
#include "speccy/machines/Plus3/Plus3Paging.h"
#include "speccy/machines/Timex.h"   // g_timex_mmu + Timex::rd/read8: the SCLD window the debugger must see
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/psram/psram_spi.h"
#include "fatfs/ff.h"

std::list<mem_desc_t> mem_desc_t::pages;
uint8_t* mem_desc_t::plugged_in[4] = { 0, 0, 0, 0 };
bool  mem_desc_t::dirty_sink = false;
bool* mem_desc_t::bank_dirty[8] = {
    &mem_desc_t::dirty_sink, &mem_desc_t::dirty_sink,
    &mem_desc_t::dirty_sink, &mem_desc_t::dirty_sink,
    &mem_desc_t::dirty_sink, &mem_desc_t::dirty_sink,
    &mem_desc_t::dirty_sink, &mem_desc_t::dirty_sink,
};
mem_desc_t mem_desc_t::acc_bank[4];
#if MEM_ACCESS_TRACE
uint32_t  mem_desc_t::access_sink = 0;
uint32_t* mem_desc_t::bank_access[8] = {
    &mem_desc_t::access_sink, &mem_desc_t::access_sink,
    &mem_desc_t::access_sink, &mem_desc_t::access_sink,
    &mem_desc_t::access_sink, &mem_desc_t::access_sink,
    &mem_desc_t::access_sink, &mem_desc_t::access_sink,
};
volatile uint32_t mem_acc_clean_cnt = 0, mem_acc_clean_sum = 0, mem_acc_clean_max = 0;
volatile uint32_t mem_acc_lo128 = 0, mem_acc_lo512 = 0;
volatile uint32_t mem_acc_dirty_cnt = 0, mem_acc_dirty_sum = 0;
#endif
uint32_t MEM_PG_CNT = 64;

// Per-frame SPI PSRAM swap counters — reset each EndFrame, read in Debug::log.
volatile uint32_t mem_spi_evict_count = 0;  // from_vram calls (SPI DMA loads)
volatile uint32_t mem_spi_evict_page  = 0;  // last evicted page index
volatile uint32_t mem_spi_read_skip   = 0;  // first-touch loads with the read skipped
volatile uint32_t mem_spi_wb_skip     = 0;  // clean-victim evictions with the write-back skipped
volatile uint32_t mem_spi_swap_us     = 0;  // total µs spent in _sync page swaps
volatile uint32_t mem_spi_accb       = 0;   // accessor-mode per-byte SPI accesses
volatile uint32_t mem_spi_promo      = 0;   // accessor→pool promotions (16KB loads)
volatile uint32_t mem_spi_promo_idle = 0;   // ...of which executed in the idle window
volatile uint32_t mem_spi_swap_idle_us = 0; // µs of swap work done in the idle window

// Accessor-mode bank window.  hw measurements (3 games) put the trampoline
// bank-visit access counts at 0-128 in 85-100% of cases, with the real working
// set in the thousands — the distribution is bimodal, so the low threshold in
// mem_desc_t::acc_tick() (128, cumulative per page) both captures the wins and
// promotes genuinely hot pages quickly (a promoted page pays ≤128 per-byte
// accesses ≈ 0.3ms on top of the 1.45ms load).

// Deferred-promotion state (see MemESP::idleService).  Butter-only: the
// butter accessor costs ~0.2 µs/byte (uncached XIP), so a hot page can keep
// running per-byte for a frame or two while its 16KB swap waits for the idle
// window.  SPI PSRAM keeps the old always-inline promotion — its per-byte
// accessor is ~10× more expensive, so delaying a hot page there costs more
// than the swap itself.
static uint8_t g_promoPending    = 0;   // bit per CPU bank slot
static uint8_t g_promoInlineLeft = 1;   // inline promotions left this frame

static inline void maybePromote(uint8_t bank) {
    if (vram_butter(mem_desc_t::acc_bank[bank].spiBase())) {
        if (g_promoInlineLeft == 0) {
            g_promoPending |= (uint8_t)(1u << bank);
            return;
        }
        g_promoInlineLeft--;
    }
    MemESP::promoteBank(bank);
}

uint8_t MemESP::accessorRead(uint8_t bank, uint16_t off) {
    mem_spi_accb++;
    uint8_t v = mem_desc_t::acc_bank[bank].read(off);
    if (mem_desc_t::acc_bank[bank].acc_tick()) maybePromote(bank);
    return v;
}

void MemESP::accessorWrite(uint8_t bank, uint16_t off, uint8_t v) {
    mem_spi_accb++;
    mem_desc_t::acc_bank[bank].write(off, v);   // _write → backing store + valid bit
    if (mem_desc_t::acc_bank[bank].acc_tick()) maybePromote(bank);
}

void MemESP::promoteBank(uint8_t bank) {
    mem_spi_promo++;
    g_promoPending &= (uint8_t)~(1u << bank);
    plugPtr16(bank, mem_desc_t::acc_bank[bank].materialize(bank));
}

void MemESP::promoFrameReset(uint8_t inlineBudget) {
    g_promoInlineLeft = inlineBudget;
}

void MemESP::idleService(uint64_t deadline_us) {
    static uint32_t promoEstUs = 1500;   // rolling estimate of one promotion
    while (g_promoPending) {
        uint8_t bank = (uint8_t)__builtin_ctz(g_promoPending);
        if (time_us_64() + promoEstUs > deadline_us) return;
        g_promoPending &= (uint8_t)~(1u << bank);
        // Re-validate: the slot may have been re-synced (or promoted via the
        // inline budget) since the request was queued.
        if (ramCurrent[bank * 2] != nullptr) continue;
        if (!mem_desc_t::acc_bank[bank].acc_hot()) continue;
        uint64_t t0 = time_us_64();
        promoteBank(bank);
        uint32_t dt = (uint32_t)(time_us_64() - t0);
        mem_spi_promo_idle++;
        mem_spi_swap_idle_us += dt;
        promoEstUs = (promoEstUs + dt) / 2;
        if (promoEstUs < 600)  promoEstUs = 600;
        if (promoEstUs > 4000) promoEstUs = 4000;
    }
}

// Backing-store validity bitmap: bit set = this vram/swap page has been
// materialized (written to PSRAM or the swap file) at least once.  A page that
// was never materialized holds power-on garbage, so from_vram() skips the 16KB
// load entirely — the stale content of the reused SRAM frame is just as good.
// This halves the fault cost of the Profi boot RAM-clear and the 1024K memtest
// first pass, where every fault touches a page for the first time.
#define VRAM_PG_MAX 512  // 8MB of backing store; out-of-range = always "valid"
static uint32_t vram_pg_valid[VRAM_PG_MAX / 32];
static inline bool vram_pg_is_valid(uint32_t ba) {
    uint32_t pg = ba / MEM_PG_SZ;
    return pg >= VRAM_PG_MAX || ((vram_pg_valid[pg >> 5] >> (pg & 31)) & 1u);
}
static inline void vram_pg_set_valid(uint32_t ba) {
    uint32_t pg = ba / MEM_PG_SZ;
    if (pg < VRAM_PG_MAX) vram_pg_valid[pg >> 5] |= 1u << (pg & 31);
}

// Spare 16KB SRAM frame for asynchronous page swaps: a fault loads the incoming
// page into the spare and pushes the victim's write-back to the background
// (psram_write_page_async), so the Z80 resumes after just the read half of the
// swap.  The victim's old frame becomes the next spare — it must stay untouched
// until the background write is joined, which the driver guarantees (every
// PSRAM entry point joins first).  Allocated lazily from the heap (NOT stolen
// from the pool — the Profi pool is sized exactly to its CP/M working set and
// losing a slot reignites the bank-trampoline thrash); skipped when the heap
// is tight (ZiFi TLS needs ~50KB headroom for HTTPS).
static uint8_t* g_swap_spare = nullptr;
static bool     g_spare_tried = false;
extern "C" size_t getLargestAllocatable(void);  // also used by mem_bounce_acquire below

// Cold paths for memory breakpoints — see declaration in MemESP.h.
__attribute__((noinline)) void MemESP::checkMemReadBP(uint16_t addr) {
    if (Config::hasBreakPoint(addr, Config::BP_MEM_READ))
        CPU::portBasedBP = true;
}

__attribute__((noinline)) void MemESP::checkMemWriteBP(uint16_t addr) {
    if (Config::hasBreakPoint(addr, Config::BP_MEM_WRITE))
        CPU::portBasedBP = true;
}

static const char PAGEFILE[] = "/tmp/pico-speccy.swap";
// The SD swap file's FIL, made on the FIRST swap access. A board where every page
// is SRAM- or butter-backed never touches the swap tier, and used to pay a 608 B
// static FIL (plus a file created on every boot) for it anyway. reset() only marks
// the file stale; the unlink + create runs when a swap page first needs it.
static FIL* s_swapFil = nullptr;
static bool s_swapFresh = true;
static FIL* swapF() {
    if (!s_swapFil) s_swapFil = new FIL();     // zero-initialised; heap, panics on OOM like the old static never could
    if (s_swapFresh) {
        s_swapFresh = false;
        f_close(s_swapFil);
        f_unlink(PAGEFILE); // ensure it is new file
        f_open(s_swapFil, PAGEFILE, FA_WRITE | FA_CREATE_ALWAYS);
        f_close(s_swapFil);
        f_open(s_swapFil, PAGEFILE, FA_READ | FA_WRITE);
    }
    return s_swapFil;
}

// Called by FileUtils::remountSD() to reopen swap file after SD remount
extern "C" void mem_swap_reopen(void) {
    if (!s_swapFil || s_swapFresh) return;      // swap never used this session: nothing to reopen
    FSIZE_t sz = f_size(s_swapFil);
    f_close(s_swapFil);
    if (sz > 0) {
        f_open(s_swapFil, PAGEFILE, FA_READ | FA_WRITE);
    } else {
        f_open(s_swapFil, PAGEFILE, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
    }
}

// Page-descriptor pool.  One mem_desc_int_t exists per ZX RAM page and is never
// destroyed (setup() builds them once, mem_desc_t copies share the pointer), so a
// bump allocator over pooled blocks is the whole lifetime story.  It matters because
// MEM_PG_CNT reaches 2048 with Murmuzavr 32 MB: 2050 individual mallocs pay a 4-byte
// chunk header each (8 KB wasted) and leave 2050 tiny entries in the free list for the
// framebuffer and the WD1793 track buffer to allocate around.
void* mem_desc_t::mem_desc_int_t::operator new(size_t sz) {
#if !MEM_ACCESS_TRACE
    // Locked down on purpose: 4 bytes added here is 8 KB of SRAM at 2050 pages.
    static_assert(sizeof(void*) != 4 || sizeof(mem_desc_int_t) == 12, "page descriptor grew — see MemESP.h");
#endif
    static uint8_t* blk  = nullptr;
    static size_t   left = 0;
    const size_t need = (sz + 3u) & ~(size_t)3u;
    if (left < need) {
        // Block sized to what THIS MEM_PG_CNT actually needs, capped at 4 KB. A flat
        // 4 KB block was a net heap LOSS at the default 64 pages — the case every
        // non-Murmuzavr machine boots in, Profi/Karabas included: 66 descriptors want
        // 792 B, so 3.3 KB sat unused where 66 individual mallocs cost 1584 B.  That
        // heap is what assign_ram() hands out as SRAM-resident ZX pages and what
        // VIDEO::Init + the WD1793 track buffer draw from afterwards (hw log
        // 2026-07-29: freeHeap at MEMORY SETUP was 2328 B lower than pre-commit).
        // MEM_PG_CNT is constant-initialized to 64 in this TU, so the static `temp[8]`
        // descriptors built during static init read a sane value; ESPectrum::setup()
        // has set the final count long before `new mem_desc_t[MEM_PG_CNT + 2]`.
        const size_t want = ((size_t)MEM_PG_CNT + 2u) * need;
        size_t take = want < 4096u ? want : 4096u;
        // pico_malloc panics rather than returning NULL, so a short block is not a
        // case we can hit here; oversized requests (never happens for this struct)
        // still get their own chunk.
        if (take < need) take = need;
        blk  = (uint8_t*)malloc(take);
        left = blk ? take : 0;
        if (!blk) return malloc(sz);
    }
    void* r = blk;
    blk  += need;
    left -= need;
    return r;
}

void mem_desc_t::reset(void) {
    memset(vram_pg_valid, 0, sizeof(vram_pg_valid));
    for (int i = 0; i < 8; ++i) bank_dirty[i] = &dirty_sink;
#if MEM_ACCESS_TRACE
    for (int i = 0; i < 8; ++i) bank_access[i] = &access_sink;
#endif
    pages.clear();
    s_swapFresh = true;     // recreate the swap file on its first use (see swapF)
}

uint8_t* mem_desc_t::to_vram(void) {
    uint8_t* res = _int->p;
    uint32_t ba = _int->vram_off();
    if (vram_butter(ba)) {
        // Uncached alias: pure QMI write burst, no XIP cache allocation/eviction
        // (a cached-alias write would read-allocate every line first).
        memcpy(butter_nc(ba), res, MEM_PG_SZ);
        _int->mem_type = PSRAM_SPI;   // "external PSRAM" backing; butter vs SPI is
                                      // re-dispatched per access via vram_butter()
        vram_pg_set_valid(ba);
        _int->p = 0;
        return res;
    }
    if (psram_size() >= ba + MEM_PG_SZ) {
        psram_write_page(ba, res); // single SPI CS for full 16KB (32-bit PIO, exact x)
        _int->mem_type = PSRAM_SPI;
    } else {
        #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        #endif
        UINT bw;
        FSIZE_t lba = ba;
        f_lseek(swapF(), lba);
        f_write(swapF(), res, MEM_PG_SZ, &bw);
        #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        #endif
        _int->mem_type = SWAP;
    }
    vram_pg_set_valid(ba);
    _int->p = 0;
    return res;
}
void mem_desc_t::from_vram(uint8_t* p) {
    this->_int->p = p;
    uint32_t ba = _int->vram_off();
    _int->mem_type = POINTER;
    _int->dirty = false;   // frame == backing store (or both garbage on skip)
    _int->acc_hits = 0;    // page is pool-resident now; accessor counting restarts
#if MEM_ACCESS_TRACE
    _int->acc = 0;         // start counting accesses served by this load
#endif
    if (!vram_pg_is_valid(ba)) {
        // First touch: the backing store was never written, its content is
        // power-on garbage — the reused frame's stale bytes are just as good.
        mem_spi_read_skip++;
        return;
    }
    if (vram_butter(ba)) {
        mem_spi_evict_count++;
        mem_spi_evict_page = ba / MEM_PG_SZ;
        memcpy(p, butter_nc(ba), MEM_PG_SZ);   // uncached: no XIP cache wipe
        return;
    }
    if (psram_size() >= ba + MEM_PG_SZ) {
        mem_spi_evict_count++;
        mem_spi_evict_page = ba / MEM_PG_SZ;
        psram_read_page(ba, p); // single SPI CS for full 16KB (32-bit PIO, exact x/y)
    } else {
        UINT br;
        FSIZE_t lba = ba;
        f_lseek(swapF(), lba);
        f_read(swapF(), p, 0x4000, &br);
    }
}
uint8_t mem_desc_t::_read(uint16_t addr) {
    uint32_t ba = _int->vram_off();
    if (vram_butter(ba)) {
        return butter_nc(ba)[addr];
    }
    if (psram_size() >= ba + MEM_PG_SZ) {
        return read8psram(ba + addr);
    }
    UINT br;
    FSIZE_t lba = ba;
    f_lseek(swapF(), lba + addr);
    uint8_t r;
    f_read(swapF(), &r, 1, &br);
    return r;
}
void mem_desc_t::_write(uint16_t addr, uint8_t v) {
    uint32_t ba = _int->vram_off();
    // The byte lands in the backing store — a later from_vram must not skip
    // the load anymore (the rest of the page stays garbage, like real RAM).
    vram_pg_set_valid(ba);
    if (vram_butter(ba)) {
        butter_nc(ba)[addr] = v;
        return;
    }
    if (psram_size() >= ba + MEM_PG_SZ) {
        write8psram(ba + addr, v);
        return;
    }
    #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    #endif
    UINT br;
    FSIZE_t lba = ba;
    f_lseek(swapF(), lba + addr);
    f_write(swapF(), &v, 1, &br);
    #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    #endif
}
void mem_desc_t::_sync(uint8_t bank) {
    uint32_t t0 = time_us_32();
    // One-time: allocate the swap spare from the heap (~100KB free at runtime
    // on m1p2; keep ≥48KB headroom for ZiFi TLS / OSD).  No pool stealing.
    if (!g_spare_tried && psram_size()) {
        g_spare_tried = true;
        if (getLargestAllocatable() >= MEM_PG_SZ + 49152)
            g_swap_spare = (uint8_t*)malloc(MEM_PG_SZ);
    }
    for (auto it = pages.begin(); it != pages.end(); ++it) {
        mem_desc_t& page = *it;
        if (page._int->mem_type == POINTER && !page._int->pinned) {
            for (uint8_t i = 0; i < 4; ++i) {
                if (i != bank) {
                    if (page._int->p == plugged_in[i]) goto skip;
                }
            }
            {
                uint32_t vba = page._int->vram_off();
                bool vspi = psram_size() >= vba + MEM_PG_SZ;
                // butter backing counts as external PSRAM for the victim's new
                // mem_type; the async-spare arm stays SPI-only (g_swap_spare is
                // never allocated on butter boards and psram_write_page_async
                // is a PIO-SPI call).
                bool vext = vspi || vram_butter(vba);
#if MEM_ACCESS_TRACE
                // How many Z80 accesses did this page's 16KB load actually
                // serve?  Clean victims with a low count are the accessor-mode
                // win candidates (the load was mostly wasted).
                {
                    uint32_t a = page._int->acc;
                    if (page._int->dirty) {
                        mem_acc_dirty_cnt++; mem_acc_dirty_sum += a;
                    } else {
                        mem_acc_clean_cnt++; mem_acc_clean_sum += a;
                        if (a > mem_acc_clean_max) mem_acc_clean_max = a;
                        if (a < 128) mem_acc_lo128++;
                        if (a < 512) mem_acc_lo512++;
                    }
                }
#endif
                if (!page._int->dirty) {
                    // Clean victim: the backing store already matches the frame
                    // (or both hold garbage — never materialized).  Detach with
                    // NO write-back and reuse the frame for the incoming page:
                    // the fault costs only the read half.  Read-heavy storms
                    // (memtest verify passes, the CP/M bank trampoline over
                    // read-only code banks) skip the 16KB write entirely.
                    uint8_t* vf = page._int->p;
                    page._int->p = 0;
                    page._int->mem_type = vext ? PSRAM_SPI : SWAP;
                    mem_spi_wb_skip++;
                    from_vram(vf);
                } else if (g_swap_spare && vspi) {
                    // Async swap: load the incoming page into the spare frame,
                    // then push the victim's write-back to the background —
                    // the Z80 resumes while PIO+DMA clock out the 16KB.  The
                    // victim's frame becomes the next spare; it stays untouched
                    // until the driver joins the write on the next PSRAM access.
                    uint8_t* vf = page._int->p;
                    page._int->p = 0;
                    page._int->mem_type = PSRAM_SPI;
                    from_vram(g_swap_spare);          // may skip read on first touch
                    psram_write_page_async(vba, vf);
                    vram_pg_set_valid(vba);
                    g_swap_spare = vf;
                } else {
                    from_vram( page.to_vram() );
                }
            }
            pages.erase(it);
            pages.push_back(*this);
            break;
        }
        skip:;
    }
    mem_spi_swap_us += time_us_32() - t0;
}
// Transient bounce buffer for chunked page<->file transfers (snapshot load/save
// into evicted pages). malloc'd per call — pico_malloc PANICS on OOM instead of
// returning NULL, so gate on getLargestAllocatable() (see Buffer::palloc) and
// fall back to the per-byte path when the heap is too tight (e.g. after
// VIDEO::Init with ~5KB free).
extern "C" size_t getLargestAllocatable(void);
static uint8_t* mem_bounce_acquire(size_t* sz) {
    const size_t WANT = 1024;
    if (getLargestAllocatable() < WANT + 2048) return nullptr;
    uint8_t* p = (uint8_t*)malloc(WANT);
    if (p) *sz = WANT;
    return p;
}

void mem_desc_t::from_file(FIL* f_in, size_t sz) {
    UINT br;
    if (_int->mem_type == POINTER) {
        _int->dirty = true;   // frame modified behind writebyte's back
        f_read(f_in, direct(), sz, &br);
        return;
    }
    uint32_t ba = _int->vram_off();
    bool btr = vram_butter(ba);
    bool spi = !btr && psram_size() >= ba + MEM_PG_SZ;
    if (!spi && !btr) {
        #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        #endif
        f_lseek(swapF(), ba);
    }
    size_t bsz = 0;
    uint8_t* buf = mem_bounce_acquire(&bsz);
    if (buf) {
        for (size_t off = 0; off < sz; off += bsz) {
            size_t n = (sz - off > bsz) ? bsz : sz - off;
            f_read(f_in, buf, n, &br);
            // Bounce + CPU memcpy for butter (no f_read straight into the XIP
            // window: the SD driver may DMA into the destination, and bulk QMI
            // DMA starves the PIO video — see gigascreen_prevfb notes).
            if (btr) {
                memcpy(butter_nc(ba + off), buf, n);
            } else
            if (spi) {
                psram_write_range(ba + off, buf, n);
            } else {
                UINT bw;
                f_write(swapF(), buf, n, &bw);
            }
        }
        free(buf);
    } else {
        uint8_t v;
        for (size_t addr = 0; addr < sz; ++addr) {
            f_read(f_in, &v, 1, &br);
            if (btr) butter_nc(ba)[addr] = v;
            else
            if (spi) write8psram(ba + addr, v);
            else     f_write(swapF(), &v, 1, &br);
        }
    }
    vram_pg_set_valid(ba);
    #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    if (!spi && !btr) gpio_put(PICO_DEFAULT_LED_PIN, false);
    #endif
}
void mem_desc_t::to_file(FIL* f_out, size_t sz) {
    #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    #endif
    UINT br;
    if (_int->mem_type == POINTER) {
        f_write(f_out, direct(), sz, &br);
        #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        #endif
        return;
    }
    uint32_t ba = _int->vram_off();
    // Same page-fits-in-PSRAM test as to_vram/_read/_write — a bare psram_size()
    // check would read a high page (evicted to SD swap) from wrapped PSRAM addresses.
    bool btr = vram_butter(ba);
    bool spi = !btr && psram_size() >= ba + MEM_PG_SZ;
    if (!spi && !btr) f_lseek(swapF(), ba);
    size_t bsz = 0;
    uint8_t* buf = mem_bounce_acquire(&bsz);
    if (buf) {
        for (size_t off = 0; off < sz; off += bsz) {
            size_t n = (sz - off > bsz) ? bsz : sz - off;
            if (btr)      memcpy(buf, butter_nc(ba + off), n);
            else
            if (spi) psram_read_range(ba + off, buf, n);
            else     f_read(swapF(), buf, n, &br);
            f_write(f_out, buf, n, &br);
        }
        free(buf);
    } else {
        uint8_t v;
        for (size_t addr = 0; addr < sz; ++addr) {
            if (btr)      v = butter_nc(ba)[addr];
            else
            if (spi) v = read8psram(ba + addr);
            else     f_read(swapF(), &v, 1, &br);
            f_write(f_out, &v, 1, &br);
        }
    }
    #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    #endif
}
void mem_desc_t::read_chunk(uint32_t off, uint8_t* buf, uint32_t n) {
    if (_int->mem_type == POINTER) { memcpy(buf, _int->p + off, n); return; }
    const uint32_t ba = _int->vram_off();
    if (vram_butter(ba)) { memcpy(buf, butter_nc(ba + off), n); return; }
    if (psram_size() >= ba + MEM_PG_SZ) { psram_read_range(ba + off, buf, n); return; }
    UINT br;
    f_lseek(swapF(), (FSIZE_t)ba + off);
    if (f_read(swapF(), buf, n, &br) != FR_OK || br != n) memset(buf, 0, n);
}

void mem_desc_t::write_chunk(uint32_t off, const uint8_t* buf, uint32_t n) {
    if (_int->mem_type == POINTER) { _int->dirty = true; memcpy(_int->p + off, buf, n); return; }
    const uint32_t ba = _int->vram_off();
    vram_pg_set_valid(ba);
    if (vram_butter(ba)) { memcpy(butter_nc(ba + off), buf, n); return; }
    if (psram_size() >= ba + MEM_PG_SZ) { psram_write_range(ba + off, buf, n); return; }
    UINT bw;
    f_lseek(swapF(), (FSIZE_t)ba + off);
    f_write(swapF(), buf, n, &bw);
}

void mem_desc_t::from_mem(mem_desc_t& ram, size_t sz) {
    bool dstPtr = _int->mem_type == POINTER;
    bool srcPtr = ram._int->mem_type == POINTER;
    if (dstPtr && srcPtr) {
        _int->dirty = true;
        memcpy(direct(), ram.direct(), sz);
        return;
    }
    uint32_t sba = ram._int->vram_off();
    uint32_t dba = _int->vram_off();
    bool sbtr = vram_butter(sba);
    bool dbtr = vram_butter(dba);
    bool sspi = !sbtr && psram_size() >= sba + MEM_PG_SZ;
    bool dspi = !dbtr && psram_size() >= dba + MEM_PG_SZ;
    UINT brw;
    if (dstPtr) {          // vram/swap → SRAM: one block transfer, no bounce
        _int->dirty = true;
        if (sbtr) memcpy(direct(), butter_nc(sba), sz);
        else
        if (sspi) psram_read_range(sba, direct(), sz);
        else { f_lseek(swapF(), sba); f_read(swapF(), direct(), sz, &brw); }
        return;
    }
    if (srcPtr) {          // SRAM → vram/swap: one block transfer, no bounce
        if (dbtr) memcpy(butter_nc(dba), ram.direct(), sz);
        else
        if (dspi) psram_write_range(dba, ram.direct(), sz);
        else { f_lseek(swapF(), dba); f_write(swapF(), ram.direct(), sz, &brw); }
        vram_pg_set_valid(dba);
        return;
    }
    // vram/swap → vram/swap — bounce chunks; per-byte fallback on tight heap.
    size_t bsz = 0;
    uint8_t* buf = mem_bounce_acquire(&bsz);
    if (buf) {
        for (size_t off = 0; off < sz; off += bsz) {
            size_t n = (sz - off > bsz) ? bsz : sz - off;
            if (sbtr) memcpy(buf, butter_nc(sba + off), n);
            else
            if (sspi) psram_read_range(sba + off, buf, n);
            else { f_lseek(swapF(), sba + off); f_read(swapF(), buf, n, &brw); }
            if (dbtr) memcpy(butter_nc(dba + off), buf, n);
            else
            if (dspi) psram_write_range(dba + off, buf, n);
            else { f_lseek(swapF(), dba + off); f_write(swapF(), buf, n, &brw); }
        }
        free(buf);
        vram_pg_set_valid(dba);
    } else {
        for (size_t addr = 0; addr < sz; ++addr) {
            _write(addr, ram._read(addr));
        }
    }
}
void mem_desc_t::cleanup() {
    // A POINTER page's data is wherever `p` points — an SRAM buffer, or a butter
    // slot handed out by assign_ram in BUTTER order (butter_idx), which is NOT the
    // ZX page number. `vram_off()` (= page_idx * 16 KB) is the backing store of the
    // NON-pointer tiers only. The original test was inverted and zeroed
    // `butter_nc(page * 16K)` for pointer pages: on a TS-Conf boot with pages 0..5
    // in SRAM that hit ZX page i+6 instead of i, left the SRAM pages standing, and
    // for the last six pages ran 96 KB PAST the page strip into the Buffer arena —
    // straight over the NeoGS low RAM (fw code + work RAM, the first palloc there),
    // so every .spg load after commit 400e7c4 turned the card into a NOP slide
    // (hw 2026-09-17: GS-Z80 PC in zeroed RAM, SP at its reset top, command 0x30
    // never acknowledged, Lode Runner's uploader parked in its D0 wait). The GMX
    // F11 wipe and the snapshot loaders' void-page zeroing had the same defect.
    // Through the CACHED alias on purpose: pointer pages are read through it by
    // both cores, so an uncached memset would leave stale (and possibly dirty)
    // lines behind.
    if (_int->mem_type == POINTER) {
        if (!_int->p) return;
        memset(direct(), 0, MEM_PG_SZ);
    } else {
        // Zero the vram backing store (same effect as the old per-byte _write
        // loop — 16384 SPI transactions — but chunked).
        uint32_t ba = _int->vram_off();
        if (vram_butter(ba)) {
            memset(butter_nc(ba), 0, MEM_PG_SZ);
            vram_pg_set_valid(ba);
            return;
        }
        bool spi = psram_size() >= ba + MEM_PG_SZ;
        size_t bsz = 0;
        uint8_t* buf = mem_bounce_acquire(&bsz);
        if (buf) {
            memset(buf, 0, bsz);
            if (!spi) f_lseek(swapF(), ba);
            for (size_t off = 0; off < MEM_PG_SZ; off += bsz) {
                if (spi) psram_write_range(ba + off, buf, bsz);
                else { UINT bw; f_write(swapF(), buf, bsz, &bw); }
            }
            free(buf);
            vram_pg_set_valid(ba);
        } else {
            for (size_t addr = 0; addr < MEM_PG_SZ; ++addr) {
                _write(addr, 0);
            }
        }
    }
}

mem_desc_t MemESP::rom[64];
// Z80 RAM pages placed in the .ram_128k section, which the linker script pins
// to SRAM banks 0-1 (dedicated 128 KB region). The framebuffer lives in heap
// (banks 2-7) so HDMI DMA reading the framebuffer travels through different
// AHB ports than CPU access to Z80 RAM — no bus contention.
// NOLOAD (no init from flash); pages cleared at runtime in MemESP::reset().
//
// All 128 KB of Spectrum 128 RAM (pages 0-7) lives here. The 16col rasterizer
// in Video.cpp reads pages 4-7 via direct() in the HDMI/VGA ISR and requires
// guaranteed POINTER backing.
#define Z80_RAM_PAGE_ATTR __attribute__((section(".ram_128k"), aligned(4)))
Z80_RAM_PAGE_ATTR static uint8_t pages57[MEM_PG_SZ * 2];
Z80_RAM_PAGE_ATTR static uint8_t pages0123[MEM_PG_SZ * 4];
Z80_RAM_PAGE_ATTR static uint8_t pages46[MEM_PG_SZ * 2];
#undef Z80_RAM_PAGE_ATTR
static mem_desc_t temp[8] = {
    { pages0123 + MEM_PG_SZ * 0, 0 },
    { pages0123 + MEM_PG_SZ * 1, 1 },
    { pages0123 + MEM_PG_SZ * 2, 2 },
    { pages0123 + MEM_PG_SZ * 3, 3 },
    { pages46   + MEM_PG_SZ * 0, 4 },
    { pages57   + MEM_PG_SZ * 0, 5 },
    { pages46   + MEM_PG_SZ * 1, 6 },
    { pages57   + MEM_PG_SZ * 1, 7 },
};
mem_desc_t* MemESP::ram = temp;
bool MemESP::newSRAM = false;
int ram_pages = 2, butter_pages = 0, psram_pages = 0, swap_pages = 0;

uint8_t* MemESP::ramCurrent[8];
bool MemESP::ramContended[8];

uint8_t MemESP::notMore128 = 0;
uint32_t MemESP::page0ram = 0;
uint32_t MemESP::bankLatch = 0;
uint8_t MemESP::videoLatch = 0;
uint8_t MemESP::romLatch = 0;
uint8_t MemESP::pagingLock = 0;
uint8_t MemESP::romInUse = 0;
uint8_t MemESP::p3special = 0;

// ZX Spectrum +2A/+3 memory map. Normal mode is the 128K one (ROM / 5 / 2 / bank) with
// a FOUR-way ROM select; #1FFD D0 swaps it for one of four all-RAM configurations.
// Table and semantics from Fuse (machines/specplus3.c special_memory_map /
// select_special_map). Contended pages on a +2A/+3 are 4,5,6,7 — not the odd ones.
void MemESP::plus3Remap(uint8_t p1ffd) {
    if (plus3IsSpecial(p1ffd)) {
        const uint8_t* m = plus3SpecialPages(p1ffd);
        p3special = 1;
        page0ram  = 1;          // for outside observers (debugger, snapshots)
        for (uint8_t i = 0; i < 4; i++)
            plug16(i, ram[m[i]].sync(i), plus3PageContended(m[i]));
    } else {
        const uint8_t bank = bankLatch & 0x07;
        p3special = 0;
        page0ram  = 0;
        romInUse = plus3RomIndex(p1ffd, romLatch);
        plug16(0, rom[romInUse].direct(), false);
        plug16(1, ram[5].sync(1),   plus3PageContended(5));
        plug16(2, ram[2].sync(2),   plus3PageContended(2));
        plug16(3, ram[bank].sync(3), plus3PageContended(bank));
    }
}

const uint8_t* MemESP::overlayBase[8] = {0};
const uint8_t* MemESP::overlayPtr[8]  = {0};
const uint8_t* MemESP::overlayFlat[8] = {0};

// Materialised (base, overlay) pages in butter PSRAM — see overlayFlat. 8 slots
// of 16 KB; a slot is reused LRU when no registry entry points at it. Nothing
// is ever freed: the pool is not a heap, and the working set is a few pages.
namespace {
struct FlatSlot { const uint8_t* base; const uint8_t* ov; uint8_t* flat; uint32_t stamp; };
static FlatSlot s_flat[8];
static uint32_t s_flat_stamp = 0;
static bool     s_flat_off = false;     // no butter page could be had — stop trying

static const uint8_t* flatLookup(const uint8_t* base, const uint8_t* ov) {
    for (auto& f : s_flat)
        if (f.flat && f.base == base && f.ov == ov) { f.stamp = ++s_flat_stamp; return f.flat; }
    return nullptr;
}
} // namespace
uint8_t        MemESP::overlayCount   = 0;

void MemESP::registerOverlay(const uint8_t* base, const uint8_t* ov) {
    for (uint8_t i = 0; i < overlayCount; i++) {
        if (overlayBase[i] == base) {
            if (ov) {
                overlayPtr[i]  = ov;                      // update existing
                overlayFlat[i] = flatLookup(base, ov);    // cached page or pending
            } else {                                      // unregister: swap-remove
                overlayBase[i] = overlayBase[overlayCount - 1];
                overlayPtr[i]  = overlayPtr[overlayCount - 1];
                overlayFlat[i] = overlayFlat[overlayCount - 1];
                overlayCount--;
            }
            return;
        }
    }
    if (ov && overlayCount < 8) {
        overlayBase[overlayCount] = base;
        overlayPtr[overlayCount]  = ov;
        overlayFlat[overlayCount] = flatLookup(base, ov);
        overlayCount++;
    }
}

// Per frame from ESPectrum::loop: give one pending registry entry its flat page.
// Butter PSRAM only — a 16 KB heap block here would be the framebuffer's margin;
// boards without the butter pool keep the binary-search path for good.
size_t MemESP::overlayFlatBytes() {
    size_t n = 0;
    for (const auto& f : s_flat) if (f.flat) n += MEM_PG_SZ;
    return n;
}

void MemESP::materializeOverlays() {
    if (s_flat_off || overlayCount == 0) return;
    int pending = -1;
    for (uint8_t i = 0; i < overlayCount; i++) if (!overlayFlat[i]) { pending = i; break; }
    if (pending < 0) return;
    if (!Buffer::butterPoolReady()) return;
    const uint8_t* base = overlayBase[pending];
    const uint8_t* ov   = overlayPtr[pending];
    // A slot: free, else the LRU one that no registry entry is using right now.
    FlatSlot* slot = nullptr;
    for (auto& f : s_flat) if (!f.flat) { slot = &f; break; }
    if (!slot) {
        for (auto& f : s_flat) {
            bool inUse = false;
            for (uint8_t i = 0; i < overlayCount; i++) if (overlayFlat[i] == f.flat) inUse = true;
            if (!inUse && (!slot || f.stamp < slot->stamp)) slot = &f;
        }
        if (!slot) return;                                // all eight referenced — leave it
    }
    if (!slot->flat) {
        void* p = Buffer::palloc(MEM_PG_SZ, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!p) { s_flat_off = true; return; }
        if ((uintptr_t)p < 0x11000000u || (uintptr_t)p >= 0x20000000u) {   // not butter: give it back, never again
            Buffer::pfree(p);
            s_flat_off = true;
            Debug::log("[ROM] overlay pages: no butter PSRAM, keeping the run-list path");
            return;
        }
        slot->flat = (uint8_t*)p;
    }
    rom_overlay_flatten(ov, base, slot->flat);
    slot->base = base; slot->ov = ov; slot->stamp = ++s_flat_stamp;
    overlayFlat[pending] = slot->flat;
    Debug::log("[ROM] overlay materialised: base=%p ov=%p -> %p", base, ov, slot->flat);
}

uint8_t* MemESP::page0_lo = nullptr;
uint8_t* MemESP::page0_hi = nullptr;
bool MemESP::divmmc_lo_rom = false;
bool MemESP::divmmc_mapped = false;
bool* MemESP::divmmc_hi_dirty = nullptr;
bool* MemESP::divmmc_lo_dirty = nullptr;
bool MemESP::mb02_write_gate = true; // default: allow writes (DivMMC needs this)
bool* MemESP::mb02_page_dirty = nullptr; // set by MB02::applyMapping (SRAM page window)


// ── The debugger's view of guest memory ──────────────────────────────────────
// readbyte() is the CPU's hot path and deliberately knows nothing about the
// TC2068 SCLD window (Timex.h explains why the hook lives in Z80Ops instead), so
// a debugger built on it shows the HOME bank while the Z80 is executing out of
// the EX-ROM or a DOCK cartridge — plausible-looking bytes that are simply the
// wrong program (hw 2026-09-12: PC=019C disassembled as the BASIC keyword table
// and was really LD-SAMPLE in the EX-ROM). These two go through the window.
// They also skip the memory-breakpoint checks: reading memory to DISPLAY it must
// not fire the breakpoints the display exists to show.
uint8_t MemESP::dbgPeek(uint16_t addr) {
    if (__builtin_expect(g_timex_mmu != 0, 0) && Timex::rd[addr >> 13])
        return Timex::read8(addr);
    uint8_t slot = addr >> 13;
    if (slot <= 1 && divmmc_mapped)
        return (slot == 0) ? page0_lo[addr] : page0_hi[addr & 0x1FFF];
    return romPeek(slot, ramCurrent[slot], addr);
}

void MemESP::dbgPoke(uint16_t addr, uint8_t data) {
    if (__builtin_expect(g_timex_mmu != 0, 0) && Timex::rd[addr >> 13]) {
        Timex::write8(addr, data);   // nullptr wr[] = read-only slot: drops the byte
        return;
    }
    int saved = Config::numMemWriteBP;
    Config::numMemWriteBP = 0;
    writebyte(addr, data);           // ROM filter, dirty flags, accessor banks
    Config::numMemWriteBP = saved;
}

// ===== ZX Spectrum Next MMU =====

#include "app/Buffer.h"
#include "fs/FileUtils.h"

uint8_t* MemESP::nextPg[16];
uint8_t* MemESP::nextStrip = nullptr;
uint8_t  MemESP::mmu[8];
uint8_t  MemESP::next_stale_mask = 0;

// The byte a read from a no-SRAM slot returns: the last SRAM read, which for
// a data read is the instruction byte fetched just before it — the opcode of
// LD r,(HL) / POP, the displacement of (IX+d), the CB/DD CB second byte — so
// the byte at PC-1. The second half of a 16-bit read gets the same value: a
// read from such a slot does not update the latch (zxnext_top_issue5.vhd
// sram_port_a_dat). Not modelled: code running from bank 5/7 (BRAM, which does
// not reach the latch either) and LD A,(nn)-style forms whose last SRAM byte
// is an operand already behind PC-1.
// NextDAW.DRV (Eldritch Force) depends on it: at .install it is called with
// DE=0, takes the byte at 0x0000 (0xF3) as the song's page and parses the
// "song" out of MMU page 0xF3 — on the board the counts come back small
// (LD B,(IX+6) reads 06), with 0xFF its 0x55-byte channel table ran on for 255
// records and overwrote its own code at 0x9D2E (hw 2026-10-16: the music IM1
// hook then recursed through RST 38 for ever).
uint8_t MemESP::nextStaleByte() {
    const uint16_t a = (uint16_t)(Z80::getRegPC() - 1);
    return ramCurrent[a >> 13][a & 0x1FFF];
}
bool     MemESP::nextRamReady = false;
uint8_t* MemESP::wrOverlay[8] = {};
uint8_t  MemESP::wr_overlay_mask = 0;
uint8_t* MemESP::l2RdPtr[8] = {};
uint8_t  MemESP::l2_rd_mask = 0;
uint8_t  MemESP::l2_wr_mmu_mask = 0;
uint8_t* MemESP::nextRomBase = nullptr;
uint8_t* MemESP::nextDivRomBase = nullptr;
uint8_t  MemESP::next_rom_mask = 0;
uint8_t* MemESP::altRomBase = nullptr;
uint8_t* MemESP::mfRomBase = nullptr;
uint8_t* MemESP::mfRamBase = nullptr;
bool     MemESP::next_rom3 = false;
bool     MemESP::altrom_rd = false;
bool     MemESP::altrom_wr = false;
uint8_t  MemESP::altrom_sel = 0;
const uint8_t* MemESP::nextBootRom = nullptr;
bool     MemESP::nextBootRomActive = false;
bool     MemESP::nextConfigMode = false;
uint8_t  MemESP::nextConfigPage = 0;

// Unmapped MMU pages read 0xFF (open bus). Lives in flash (.rodata), so the
// XIP-address write guard in writebyte() blocks writes to it automatically.
struct OpenBusPage {
    uint8_t b[0x2000];
    constexpr OpenBusPage() : b() {
        for (unsigned i = 0; i < sizeof(b); i++) b[i] = 0xFF;
    }
};
static constexpr OpenBusPage open_bus_page;

// Carve the Next RAM page table. Banks 0-7 (pages 0-15) point into the
// static SRAM Z80 pages so VIDEO::grmem and the ULA renderer keep working;
// banks 8-111 (pages 16-223) plus the 72K ROM region (64K NextZXOS set + 8K
// MMC ROM) take 1736K of butter PSRAM, accounted through butter_pages so the
// DivMMC allocator / GS / Buffer arena compose on top. The cap is
// Buffer::pageBudgetButter() — the same authority that bounds Murmuzavr
// pages — so the carve can never eat DivMMC's banks, GS sample RAM or the
// minimum arena.
bool MemESP::buildNextRam() {
    size_t romExtra = 15 * 0x2000;   // 64K ROM set + 8K MMC + 32K Alt-ROM + 16K Multiface
    size_t needed = (size_t)(NEXT_PAGES - 16) * 0x2000 + romExtra;
    size_t used = (size_t)butter_pages * MEM_PG_SZ;
    if (Buffer::pageBudgetButter() < used + needed) return false;
    for (int b = 0; b < 8; b++) {
        nextPg[b * 2]     = ram[b].direct();
        nextPg[b * 2 + 1] = ram[b].direct() + 0x2000;
    }
    nextStrip = (uint8_t*)PSRAM_DATA + used;
    // ROM region sits after the RAM pages; pointers stay null until the ROM
    // files actually load. Alt-ROM banks live right after the MMC ROM.
    nextRomBase = nullptr;
    nextDivRomBase = nullptr;
    altRomBase = nextRomRegion() + 9 * 0x2000;
    altrom_rd = altrom_wr = false;
    altrom_sel = 0;
    mfRomBase = nullptr;                          // set once enNextMf.rom is in
    mfRamBase = altRomBase + 0x8000 + 0x2000;     // Multiface RAM after its ROM
    memset(mfRamBase, 0, 0x2000);
    butter_pages += (NEXT_PAGES - 16 + 15 + 1) / 2; // account in 16K units
    nextRamReady = true;
    return true;
}

// ===== Next hot-page pool =====
// NEXT_HOT_FRAMES (CMake, default 1 since 2026-10-19; max 8) frames of 8K SRAM from the heap,
// taken ~3 s into the Next session and grown while the heap allows (s_hotWait).
// Policy, once per frame from VIDEO::EndFrame:
//   * a candidate is a RAM page the CPU has mapped (mmu[0..7]) whose home is in
//     butter PSRAM, and it must have been mapped at the PREVIOUS tick too — a game
//     that pages a slot many times a frame must not turn every frame into copies;
//   * slots go PC's first, then SP's, then the rest;
//   * at most NEXT_HOT_PROMOS promotions a frame (8K copy each, ~0.2-0.4 ms);
//   * a full pool evicts the frame least recently seen mapped, never one mapped
//     now or at the last two ticks; eviction always writes the 8K back (no dirty
//     tracking — every guest write goes straight through the pointer);
//   * a frame given up is QUARANTINED for two ticks: core1 (and core0's c0Help)
//     resolve nextPgPtr at render time, so a line in flight may still be reading
//     the frame under its old page for a moment.
// Every promotion/eviction swaps the page's base pointer in ramCurrent, wrOverlay
// and l2RdPtr (all hold 8K page bases: the MMU, the +3 all-RAM overlay, the
// Layer 2 windows, the TBBLUE config window), so nothing has to re-run the MMU.
#ifndef NEXT_HOT_FRAMES
#define NEXT_HOT_FRAMES 8
#endif
#define NEXT_HOT_PROMOS 2
uint8_t* MemESP::nextHotIdx = nullptr;
uint8_t* MemESP::nextHotFrame[MemESP::NEXT_HOT_MAX] = {};
static uint8_t  s_hotN = 0;                       // frames in the pool
static uint8_t  s_hotPage[MemESP::NEXT_HOT_MAX];  // page held, 0xFF = free
static uint32_t s_hotLast[MemESP::NEXT_HOT_MAX];  // tick the page was last seen mapped
static uint32_t s_hotFree[MemESP::NEXT_HOT_MAX];  // tick from which a free frame is reusable
static uint8_t  s_hotPrev[8];                     // mmu[] at the previous tick
static uint32_t s_hotTick = 0;
// Frames before the first allocation, then between attempts to add frames.
// Not at the first Next frame: the renderer takes its SRAM blocks during the
// first seconds (640 tables, the Layer 2 band, the sprite state, c0Help) and a
// pool that took the heap first pushed them to PSRAM — the reason the pool
// was off from 2026-10-13. Late, it gets what the renderer left: 46 KB free at
// 576p on Adventures in Videoland (hw 2026-10-16).
static uint16_t s_hotWait = 150;
static uint32_t s_hotPromos = 0, s_hotEvicts = 0, s_hotLogTick = 0;

static void nextHotSwap(uint8_t* from, uint8_t* to) {
    for (int s = 0; s < 8; s++) {
        if (MemESP::ramCurrent[s] == from) MemESP::ramCurrent[s] = to;
        if (MemESP::wrOverlay[s] == from)  MemESP::wrOverlay[s] = to;
        if (MemESP::l2RdPtr[s] == from)    MemESP::l2RdPtr[s] = to;
    }
}

static void nextHotEvict(int f) {
    const uint8_t pg = s_hotPage[f];
    uint8_t* home = MemESP::nextHomePtr(pg);
    uint8_t* fr = MemESP::nextHotFrame[f];
    memcpy(home, fr, 0x2000);           // through the cached butter alias: coherent
    MemESP::nextHotIdx[pg] = 0xFF;
    nextHotSwap(fr, home);
    s_hotPage[f] = 0xFF;
    s_hotFree[f] = s_hotTick + 2;       // quarantine (see above)
    s_hotEvicts++;
}

// Frames are added while the heap has an 8K block and 16K more free besides
// (the Next allocates more later: menus, the file browser, c0Help scratch).
static int nextHotWant() {
    return NEXT_HOT_FRAMES > MemESP::NEXT_HOT_MAX ? MemESP::NEXT_HOT_MAX : NEXT_HOT_FRAMES;
}
// Heap kept free beside a taken frame. 16 KB never let the pool take a frame
// in Atic Atac Remake's game (~20 KB free there); with 4 KB it takes page 06
// (the game's code, 80% of PC samples) and core0's Z80 time drops 22-24 ->
// 15.7 ms a frame (ns per M1 380-430 -> 285-300, hw 2026-10-19). One frame is
// the default (cmake NEXT_HOT_FRAMES); the menu still opens on the 11 KB left.
#ifndef NEXT_HOT_RESERVE
#define NEXT_HOT_RESERVE 4096
#endif
static void nextHotGrow() {
    const int want = nextHotWant();
    const int n0 = s_hotN;
    int n = s_hotN;
    // Each frame its own block (a fragmented heap still yields 8K pieces): the
    // reserve is on the TOTAL free heap, only the frame itself has to be one
    // block. Asking for 8K + 16K in one piece got 1 frame out of 46 KB free
    // (hw 2026-10-16).
    extern size_t getFreeHeap(void);
    while (n < want && getLargestAllocatable() >= 0x2000 && getFreeHeap() >= 0x2000 + NEXT_HOT_RESERVE) {
        uint8_t* p = (uint8_t*)tryMalloc(0x2000);
        if (!p) break;
        MemESP::nextHotFrame[n] = p;
        s_hotPage[n] = 0xFF;
        s_hotFree[n] = 0;
        s_hotLast[n] = 0;
        n++;
    }
    s_hotN = (uint8_t)n;
    if (n != n0) Debug::log("Next hot-page pool: %d x 8K SRAM frames", n);
}

static bool nextHotAlloc() {
    if (NEXT_HOT_FRAMES <= 0) return false;
    uint8_t* idx = (uint8_t*)tryMalloc(MemESP::NEXT_PAGES);
    if (!idx) return false;
    memset(idx, 0xFF, MemESP::NEXT_PAGES);
    s_hotN = 0;
    nextHotGrow();
    if (!s_hotN) { free(idx); return false; }
    memset(s_hotPrev, 0xFF, sizeof(s_hotPrev));
    MemESP::nextHotIdx = idx;
    return true;
}

void MemESP::nextHotTick() {
    if (!nextRamReady) return;
    if (NEXT_HOT_FRAMES <= 0) return;
    if (!nextHotIdx || s_hotN < nextHotWant()) {
        if (s_hotWait) s_hotWait--;
        else {
            s_hotWait = 250;
            if (!nextHotIdx) nextHotAlloc();
            else nextHotGrow();
        }
        if (!nextHotIdx) return;
    }
    if (nextBootRomActive || nextConfigMode) return;   // the TBBLUE boot owns slots 0/1
    ++s_hotTick;
    // Mark what is mapped now.
    for (int f = 0; f < s_hotN; f++) {
        const uint8_t pg = s_hotPage[f];
        if (pg == 0xFF) continue;
        for (int s = 0; s < 8; s++) if (mmu[s] == pg) { s_hotLast[f] = s_hotTick; break; }
    }
    uint8_t order[8];
    int no = 0;
    order[no++] = (uint8_t)(Z80::getRegPC() >> 13);
    const uint8_t sps = (uint8_t)(Z80::getRegSP() >> 13);
    if (sps != order[0]) order[no++] = sps;
    for (int s = 2; s < 10; s++) {
        const uint8_t sl = (uint8_t)(s & 7);
        bool dup = false;
        for (int k = 0; k < no; k++) if (order[k] == sl) dup = true;
        if (!dup) order[no++] = sl;
    }
    int promos = 0;
    for (int k = 0; k < no && promos < NEXT_HOT_PROMOS; k++) {
        const uint8_t s = order[k];
        const uint8_t pg = mmu[s];
        if (pg >= NEXT_PAGES) continue;                     // ROM (0xFF) / open bus
        if (nextHotIdx[pg] != 0xFF) continue;               // already hot
        if (s_hotPrev[s] != pg) continue;                   // not stable yet
        uint8_t* home = nextHomePtr(pg);
        if (home < (uint8_t*)0x11000000 || home >= (uint8_t*)0x12000000) continue;   // not in butter: SRAM already
        int f = -1;
        for (int i = 0; i < s_hotN; i++)
            if (s_hotPage[i] == 0xFF && s_hotFree[i] <= s_hotTick) { f = i; break; }
        if (f < 0) {
            bool quarantined = false;
            for (int i = 0; i < s_hotN; i++) if (s_hotPage[i] == 0xFF) quarantined = true;
            if (quarantined) break;           // a frame frees up in a tick or two
            // Victim: the frame seen mapped longest ago, not in the last two ticks.
            int v = -1;
            for (int i = 0; i < s_hotN; i++) {
                if (s_hotPage[i] == 0xFF) continue;
                if (s_hotTick - s_hotLast[i] < 3) continue;
                if (v < 0 || s_hotLast[i] < s_hotLast[v]) v = i;
            }
            if (v >= 0) nextHotEvict(v);    // quarantined: usable from a later tick
            break;
        }
        uint8_t* fr = nextHotFrame[f];
        memcpy(fr, home, 0x2000);
        s_hotPage[f] = pg;
        s_hotLast[f] = s_hotTick;
        nextHotIdx[pg] = (uint8_t)f;
        nextHotSwap(home, fr);
        promos++;
        s_hotPromos++;
    }
    memcpy(s_hotPrev, mmu, sizeof(s_hotPrev));
    // One line every ~10 s while the pool is moving, so churn shows in the log.
    if (s_hotTick - s_hotLogTick >= 500 && (s_hotPromos || s_hotEvicts)) {
        int used = 0;
        for (int i = 0; i < s_hotN; i++) if (s_hotPage[i] != 0xFF) used++;
        Debug::log("Next hot pool: %d/%d frames, +%u promos, -%u evicts",
                   used, (int)s_hotN, (unsigned)s_hotPromos, (unsigned)s_hotEvicts);
        s_hotPromos = s_hotEvicts = 0;
        s_hotLogTick = s_hotTick;
    }
}

void MemESP::nextHotFlush() {
    if (!nextHotIdx) return;
    for (int f = 0; f < s_hotN; f++)
        if (s_hotPage[f] != 0xFF) nextHotEvict(f);
    memset(s_hotPrev, 0xFF, sizeof(s_hotPrev));
}

// The UI (menu, F5 browser) owns the heap while it is open: give the frames
// back (the file browser PANICKED on the 11 KB a taken frame left in Atic Atac
// Remake's game, hw 2026-10-19). nextHotTick re-takes them ~2 s after the
// pool is empty, i.e. once the UI has gone and the heap is back.
void MemESP::nextHotRelease() {
    if (!nextHotIdx || !s_hotN) return;
    nextHotFlush();
    for (int f = 0; f < s_hotN; f++) { free(nextHotFrame[f]); nextHotFrame[f] = nullptr; }
    s_hotN = 0;
    s_hotWait = 100;
    Debug::log("Next hot-page pool: frames released to the heap");
}

// Read a file into butter PSRAM through a small SRAM bounce: the SD driver
// may DMA into the destination, which bypasses the XIP cache — a direct
// f_read into the cached alias leaves stale cache lines and the Z80 then
// executes garbage (hw: black screen on the first NextZXOS ROM boot).
// CPU memcpy through the cached alias is coherent by construction.
static bool readFileToButter(const char* path, uint8_t* dst, size_t len) {
    // Heap for the duration of the read: a static 2 KB here was paid by every
    // machine for a buffer only the Next boot ever uses.
    constexpr size_t kBounce = 2048;
    ScopedHeap hb(kBounce);
    uint8_t* bounce = (uint8_t*)hb.p;
    if (!bounce) return false;
    FIL* f = fopen2(path, FA_READ);
    if (!f) return false;
    size_t got = 0;
    bool ok = true;
    while (got < len) {
        UINT chunk = (len - got) > kBounce ? (UINT)kBounce : (UINT)(len - got);
        UINT br = 0;
        if (f_read(f, bounce, chunk, &br) != FR_OK || br != chunk) { ok = false; break; }
        memcpy(dst + got, bounce, chunk);
        got += chunk;
    }
    fclose2(f);
    return ok;
}

// Load the real Next ROMs from SD (optional — the 128K ROM pair remains the
// fallback). Called from setup() after buildNextRam() with the SD mounted.
// Two locations: /roms/next/ (pico-speccy convention) and /machines/next/
// (the official specnext.com distro layout, so the distro can be copied to
// the card as-is).
bool MemESP::loadNextRoms() {
    if (!nextRamReady) return false;
    uint8_t* romRegion = nextRomRegion(); // after RAM pages
    static const char* zxPaths[]  = { "/roms/next/enNextZX.rom",
                                      "/machines/next/enNextZX.rom" };
    static const char* mmcPaths[] = { "/roms/next/enNxtmmc.rom",
                                      "/machines/next/enNxtmmc.rom" };
    for (int i = 0; i < 2 && !nextRomBase; i++)
        if (readFileToButter(zxPaths[i], romRegion, 0x10000))
            nextRomBase = romRegion;
    for (int i = 0; i < 2 && !nextDivRomBase; i++)
        if (readFileToButter(mmcPaths[i], romRegion + 0x10000, 0x2000))
            nextDivRomBase = romRegion + 0x10000;
    static const char* mfPaths[] = { "/roms/next/enNextMf.rom",
                                      "/machines/next/enNextMf.rom" };
    for (int i = 0; i < 2 && !mfRomBase; i++)
        if (readFileToButter(mfPaths[i], altRomBase + 0x8000, 0x2000))
            mfRomBase = altRomBase + 0x8000;
    Debug::log("Next ROMs: enNextZX %s, enNxtmmc %s, enNextMf %s",
               nextRomBase ? "OK" : "missing", nextDivRomBase ? "OK" : "missing",
               mfRomBase ? "OK" : "missing (no snapshot launch from the Browser)");
    return nextRomBase != nullptr;
}

// Spectrum Next: re-derive every slot's contention flag after the bank rule
// changed (machine timing, CPU speed, NR 0x08 bit 6). Slots 0/1 under the
// config window or the divMMC window, and ROM slots, are never contended.
void MemESP::nextContendRefresh() {
    for (uint8_t s = 0; s < 8; s++) {
        bool c = !(next_rom_mask & (1u << s)) && NextReg::pageContended(mmu[s]);
        if (s <= 1 && (divmmc_mapped || nextBootRomActive || nextConfigMode)) c = false;
        ramContended[s] = c;
    }
}

// In the .nxovl window (2026-10-11): every caller is Next-only, and NR 0x5x
// paging runs per guest interrupt on titles such as NXModPlayer.
NX_OVL_CODE void MemESP::applyMMU(uint8_t slot, uint8_t page) {
    slot &= 7;
    mmu[slot] = page;
    // While the TBBLUE bootrom / config-mode window owns slots 0/1 only the
    // latch updates; applyNextConfigWindow() re-applies mmu[] on window exit.
    if (slot <= 1 && (nextBootRomActive || nextConfigMode)) return;
    uint8_t* p;
    // zxnext.vhd 2964/3037: mmu_A21_A13(8) = 1 for every page >= 0xE0, and in
    // slots 0/1 that is the ROM branch — not only for the documented 0xFF.
    if (page >= 0xE0 && slot <= 1) {
        if (altrom_rd && altRomBase)
            p = altRomBase + (uint32_t)altrom_sel * 0x4000 + slot * 0x2000;
        else if (nextRomBase)
            p = nextRomBase + (uint32_t)(romInUse & 3) * 0x4000 + slot * 0x2000;
        else
            p = rom[romInUse & 1].direct() + slot * 0x2000;
        next_rom_mask |= (1 << slot);
    } else {
        next_rom_mask &= ~(1 << slot);
        uint8_t* np = nextPgPtr(page);
        if (np) p = np;
        else p = (uint8_t*)open_bus_page.b;
    }
    ramCurrent[slot] = p;
    // Layer 2 read mapping (port 0x123B bit 2) over the MMU page / ROM; below
    // the divMMC window in priority (zxnext.vhd ~3085). With map_wr clear the
    // slot's writes still belong to the MMU page, which ramCurrent no longer
    // shows — so they go through the write overlay (ROM / open bus: dropped).
    if ((l2_rd_mask >> slot) & 1) {
        ramCurrent[slot] = l2RdPtr[slot] ? l2RdPtr[slot] : (uint8_t*)open_bus_page.b;
        if ((l2_wr_mmu_mask >> slot) & 1) {
            const uint8_t bit = (uint8_t)(1u << slot);
            if (!(next_rom_mask & bit) && p != (uint8_t*)open_bus_page.b) {
                wrOverlay[slot] = p;
                wr_overlay_mask |= bit;
            } else {
                wr_overlay_mask &= (uint8_t)~bit;
            }
        }
    }
    // divMMC paged in: the window, not the MMU page, is what 0x0000-0x3FFF
    // shows (see nextDivPlug) — mmu[]/next_rom_mask above still track the MMU.
    if (slot <= 1 && divmmc_mapped) ramCurrent[slot] = slot ? page0_hi : page0_lo;
    // Contention by the PHYSICAL page the slot shows (zxnext.vhd mem_contend):
    // ROM and the divMMC window are never contended.
    ramContended[slot] = !(next_rom_mask & (1u << slot)) && !(slot <= 1 && divmmc_mapped) &&
                         NextReg::pageContended(page);
    // No SRAM behind slots 2-7 (MMU page >= 0xE0, Layer 2 read page past RAM):
    // reads return the data latch (nextStaleByte), which only the generic
    // accessors model — a change of the set re-derives the fast-path gates.
    if (slot >= 2) {
        const uint8_t bit = (uint8_t)(1u << slot);
        const uint8_t m = (ramCurrent[slot] == (uint8_t*)open_bus_page.b)
                          ? (uint8_t)(next_stale_mask | bit) : (uint8_t)(next_stale_mask & ~bit);
        if (m != next_stale_mask) {
            next_stale_mask = m;
            VIDEO::tsFastMemRecalc();
        }
    }
    // Next pages are direct pointers (SRAM or butter XIP) — no pool
    // write-back tracking, so the write hooks must not touch a stale
    // pool page's flags through this slot.
    mem_desc_t::bank_dirty[slot] = &mem_desc_t::dirty_sink;
#if MEM_ACCESS_TRACE
    mem_desc_t::bank_access[slot] = &mem_desc_t::access_sink;
#endif
}

void MemESP::resetNextMapping() {
    static const uint8_t def[8] = { 0xFF, 0xFF, 10, 11, 4, 5, 0, 1 };
    for (int i = 0; i < 8; i++) applyMMU(i, def[i]);
}

// ===== TBBLUE boot chain: config-mode window over 0x0000-0x3FFF =====

#include "speccy/devices/storage/DivMMC.h"

// The 16K backing store the NR 0x04 RAMPAGE page maps to, per the TBBlue
// firmware's hardware.h layout (pinned by zx_go configModePageBacking):
//   0x00-0x03  ROM0-3 (the 64K NextZXOS set)
//   0x04       divMMC ROM (8K, window mirrors it twice)
//   0x05       Multiface: enNextMf.rom (8K) + MF RAM (8K)
//   0x06-0x07  Alt-ROM 0/1
//   0x08-0x0F  divMMC RAM (128K as 8 x 16K windows over the 8K banks)
//   0x10-0x7F  RAM banks 0-111
// Returns null for unbacked pages: reads then fall through to whatever the
// MMU has mapped and writes drop — same fall-through zx_go implements.
uint8_t* MemESP::configBackingHalf(uint8_t page, uint8_t half) {
    if (!nextRamReady) return nullptr;
    uint8_t* romRegion = nextRomRegion();
    if (page <= 0x03)
        return romRegion + (uint32_t)page * 0x4000 + (uint32_t)half * 0x2000;
    if (page == 0x04)
        return romRegion + 0x10000;              // 8K, both halves mirror it
    if (page == 0x05)
        return altRomBase ? altRomBase + 0x8000 + (uint32_t)half * 0x2000 : nullptr;
    if (page == 0x06 || page == 0x07)
        return altRomBase ? altRomBase + (uint32_t)(page - 6) * 0x4000
                                        + (uint32_t)half * 0x2000
                          : nullptr;
    if (page >= 0x08 && page <= 0x0F) {
        int bank = (page - 0x08) * 2 + half;     // divMMC 8K banks
        return (bank < DIVMMC_NUM_BANKS) ? DivMMC::bank_ptr[bank] : nullptr;
    }
    if (page >= 0x10 && page <= 0x7F) {
        int pg = (page - 0x10) * 2 + half;
        return nextPgPtr(pg);
    }
    return nullptr;
}

void MemESP::applyNextConfigWindow() {
    if (!nextRamReady) return;
    if (!nextBootRomActive && !nextConfigMode) {
        // Window off: slots 0/1 back to the MMU, write overlay back to the
        // Layer 2 window (updateLayer2Window recomputes it from port 0x123B).
        applyMMU(0, mmu[0]);
        applyMMU(1, mmu[1]);
        NextReg::updateLayer2Window();
        return;
    }
    // Reads: bootrom (8K mirrored) wins; otherwise the RAMPAGE backing.
    if (nextBootRomActive && nextBootRom) {
        ramCurrent[0] = (uint8_t*)nextBootRom;   // romPeek masks to 8K
        ramCurrent[1] = (uint8_t*)nextBootRom;
        next_rom_mask |= 3;
#if NEXT_TRACE
        Debug::log("NEXT cfgwin: bootrom plugged, s0=%08X",
                   (unsigned)(uintptr_t)ramCurrent[0]);
#endif
    } else {
        for (int h = 0; h < 2; h++) {
            uint8_t* p = configBackingHalf(nextConfigPage, h);
            if (p) {
                ramCurrent[h] = p;
                next_rom_mask |= (uint8_t)(1u << h);
            }
            // unbacked: leave the MMU mapping (fall-through, as on zx_go)
        }
    }
    ramContended[0] = ramContended[1] = false;
    // Writes: while config mode is on they land in the RAMPAGE backing even
    // when the bootrom read overlay is still up — boot.bin streams the ROM
    // images through exactly this state (zxnext.vhd:1856 gates reads only;
    // zx_go's cold-boot NOP-slide was this rule missing).
    uint8_t* w0 = nextConfigMode ? configBackingHalf(nextConfigPage, 0) : nullptr;
    uint8_t* w1 = nextConfigMode ? configBackingHalf(nextConfigPage, 1) : nullptr;
    if (w0 && w1) {
        // Firmware writing a ROM image is what makes that image "loaded" —
        // flip the pointers the ROM-select path reads (the direct SD load in
        // loadNextRoms sets the same pointers to the same region).
        uint8_t* romRegion = nextRomRegion();
        if (nextConfigPage <= 0x03 && !nextRomBase)    nextRomBase = romRegion;
        if (nextConfigPage == 0x04 && !nextDivRomBase) nextDivRomBase = romRegion + 0x10000;
        if (nextConfigPage == 0x05 && !mfRomBase)      mfRomBase = altRomBase + 0x8000;
        wrOverlay[0] = w0;
        wrOverlay[1] = w1;
        wr_overlay_mask = (uint8_t)((wr_overlay_mask & ~3) | 3);
    } else {
        wr_overlay_mask &= (uint8_t)~3;          // unbacked: writes drop
    }
}
