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

#ifndef MemESP_h
#define MemESP_h

#include <inttypes.h>
#include <list>
#include "fatfs/ff.h"
#include "roms.h"
#include "app/Debug.h"
#include "app/Config.h"
#include "speccy/z80/CPU.h"
#include "RomOverlay.h"
#include "drivers/board/ChipPackage.h"

#define MEM_PG_SZ 0x4000
// with gigascreen
#define MEM_REMAIN (14*16*1024)

extern uint32_t MEM_PG_CNT;
extern uint8_t* PSRAM_DATA;
extern uint8_t psram_pin;
extern volatile uint32_t mem_spi_evict_count; // SPI PSRAM loads per frame
extern volatile uint32_t mem_spi_evict_page;  // last evicted page index
extern volatile uint32_t mem_spi_read_skip;   // first-touch loads with the read skipped
extern volatile uint32_t mem_spi_wb_skip;     // clean-victim evictions, write-back skipped
extern volatile uint32_t mem_spi_swap_us;     // total µs spent in _sync page swaps
extern volatile uint32_t mem_spi_accb;        // accessor-mode per-byte SPI accesses
extern volatile uint32_t mem_spi_promo;       // accessor→pool promotions
extern volatile uint32_t mem_spi_promo_idle;  // ...of which executed in the idle window
extern volatile uint32_t mem_spi_swap_idle_us;// µs of swap work done in the idle window
#if MEM_ACCESS_TRACE
// Access counts of evicted pages, split clean/dirty — see [ACC] log in Video.cpp.
extern volatile uint32_t mem_acc_clean_cnt, mem_acc_clean_sum, mem_acc_clean_max;
extern volatile uint32_t mem_acc_lo128, mem_acc_lo512;   // clean victims with <128 / <512 accesses
extern volatile uint32_t mem_acc_dirty_cnt, mem_acc_dirty_sum;
#endif
uint32_t butter_psram_size();
// Chip presence vs. usable size: butter_psram_size()/psram_size() answer 0 while the
// runtime PSRAM kill-switch is on (Config::psram_enabled = Debug > PSRAM), the *_probed
// pair always reports what the boot probe found. Only the menu (which has to know a
// board HAS PSRAM in order to offer turning it back on) wants the latter.
uint32_t butter_psram_probed();
void     board_psram_disable();     // apply Config::psram_enabled == false, boot-only
extern uint8_t rx[4];
extern uint8_t flash_qe;       // Puya QE-bit fix status (0=n/a; see flash_qe_text())
extern uint8_t flash_qe_diag[6];
const char* flash_qe_text();
extern uint8_t* g_alfWindow;   // Alf::Cart's 16K SD-faulted window (nullptr = unmounted)
extern "C" uint32_t psram_size();   // SPI PSRAM size (0 on butter/QSPI boards)
extern "C" uint32_t psram_probed_size();

enum mem_type_t {
    POINTER = 0,
    PSRAM_SPI,
    SWAP
};

// Butter/QSPI (RP2350 XIP CS1) backing for a vram page offset.  Used by the
// Profi memory layout on butter boards: pages 8+ are pool/accessor-backed vram
// (same LRU + accessor-window machinery as SPI-PSRAM boards) instead of direct
// XIP pointers — direct pointers push the whole Z80 working set through the
// 16KB XIP cache that also serves the emulator's own flash code, and the two
// evict each other (FPS drop / negative IDL on Profi CP/M).  SPI PSRAM keeps
// priority: the two backends never coexist on real boards, but if both probes
// report a size the established SPI dispatch stays authoritative.
static inline bool vram_butter(uint32_t ba) {
    return psram_size() == 0 && butter_psram_size() >= ba + MEM_PG_SZ;
}

// Uncached, non-allocating alias (XIP_NOCACHE_NOALLOC window) of a butter
// backing offset.  ALL vram-backing traffic (page swaps, accessor per-byte
// access, snapshot load/save) goes through this alias so it never allocates
// or evicts XIP cache lines — the 16KB cache stays dedicated to host code.
// Coherence rule: the backing strip (pages 0..MEM_PG_CNT+1) must NEVER be
// touched through the cached alias; setup() cleans the cache once after the
// butter size probe (which writes markers through the cached alias).
#ifndef PSRAM_NC_OFFSET
#define PSRAM_NC_OFFSET 0x04000000u   // the Windows host build maps no alias: 0 there
#endif
static inline uint8_t* butter_nc(uint32_t ba) {
    return (uint8_t*)((uintptr_t)PSRAM_DATA + PSRAM_NC_OFFSET + ba);
}

class mem_desc_t {
    static std::list<mem_desc_t> pages; // a pool of assigned pages
    static uint8_t* plugged_in[4]; // pointers are plugged to 64k space (do not revoke 'em)
public:
    // Per-CPU-slot dirty hooks: writebyte() does `*bank_dirty[slot] = true` on
    // every RAM write; sync(bank) re-points BOTH 8K slots of the 16K bank at
    // the plugged page's dirty flag (ROM slots point at dirty_sink).  A false
    // positive (stale pointer marking an unrelated page) only costs a redundant
    // write-back; pool pages are only ever plugged via sync(), so no dirty
    // write is missed.  8 entries = 8K slot granularity (Next MMU); a legacy
    // 16K bank owns the slot pair 2b/2b+1, both pointing at the same flag.
    static bool* bank_dirty[8];
    static bool  dirty_sink;
    static inline void mark_bank_dirty(uint8_t slot) { if (slot < 8) *bank_dirty[slot] = true; }
#if MEM_ACCESS_TRACE
    // Same pointer-per-slot scheme for the access counters (feasibility study
    // for the accessor-mode bank window — see [ACC] log in Video.cpp).
    static uint32_t* bank_access[8];
    static uint32_t  access_sink;
#endif
    // Accessor-mode bank window: sync() of an SPI-backed non-resident page
    // does NOT load 16KB — it marks the slot (ramCurrent[bank] = nullptr) and
    // records the desc here; readbyte/writebyte serve the bank per-byte over
    // SPI and promote it into the pool after MEM_ACC_PROMOTE_AT accesses.
    // hw data (Death World / Valley / Single War): 85-100% of trampoline bank
    // visits touch <128 bytes, so most visits never pay the 1.45ms page load.
    static mem_desc_t acc_bank[4];   // desc accessor-mapped per CPU slot
    // Count an accessor access; true = promotion threshold reached.  The
    // counter lives in the page desc and accumulates ACROSS visits (hw showed
    // pages trampolined often-but-lightly racking up 27k per-byte ops with a
    // per-visit counter); it resets only when the page is loaded into the pool.
    inline bool acc_tick() { return ++_int->acc_hits >= 128; }
    // True when this page has crossed the promotion threshold — used by the
    // idle-window promoter to re-validate a deferred request (the bank may
    // have been re-synced to a colder page since the request was queued).
    inline bool acc_hot() { return _int->acc_hits >= 128; }
private:
    // ONE PER ZX RAM PAGE, and MEM_PG_CNT goes up to 2048 (Machine > Murmuzavr >
    // 32 MB) — so 2050 of these exist and every byte of the struct is SRAM the rest
    // of the firmware cannot have.  Hence:
    //  - `vram_off` is NOT stored: the backing offset is always page * MEM_PG_SZ, so
    //    only the page index is kept (uint16 covers the 2048-page ceiling that
    //    Config::load clamps to, plus the two hidden ROM pages),
    //  - the layout is ordered so the whole thing is 12 bytes with no padding,
    //  - allocation goes through the pooled operator new below.
    // At 2050 pages that is 24 KB instead of 49 KB; the old 20-byte struct in a
    // 24-byte malloc chunk left too little heap for VIDEO::Init + the WD1793 track
    // buffer and Murmuzavr 32 MB panicked with "Out of memory" during setup().
    // `dirty` keeps its own addressable byte — bank_dirty[] holds a bool* to it and
    // writebyte() stores through that on every RAM write.
    struct mem_desc_int_t {
        uint8_t* p;
        uint16_t page_idx; // backing-store page number; offset = page_idx * MEM_PG_SZ
        uint16_t acc_hits; // accessor-mode accesses, CUMULATIVE across bank visits
                      // (reset only when the page is loaded into the pool) — a
                      // page trampolined often-but-lightly still accumulates to
                      // the promotion threshold instead of staying per-byte forever
        uint8_t mem_type; // a mem_type_t, stored narrow (compares/assigns unchanged)
        bool is_rom;
        bool pinned;  // if true, _sync skips this entry (never evicted while pinned)
        bool dirty;   // frame modified since last load/write-back; clean victims
                      // are evicted WITHOUT the 16KB write-back (see _sync)
#if MEM_ACCESS_TRACE
        uint32_t acc; // Z80 accesses (fetch/read/write) since last load — see [ACC] log
#endif
        mem_desc_int_t() : p(0), page_idx(0), acc_hits(0), mem_type(POINTER),
                           is_rom(false), pinned(false), dirty(true) {}
        inline uint32_t vram_off() const { return (uint32_t)page_idx * MEM_PG_SZ; }
        // Bump-allocated from pooled blocks (MemESP.cpp): descriptors are created
        // once at setup() and never destroyed, so 2050 individual malloc chunks only
        // pay a 4-byte header each and fragment the free list for nothing.
        static void* operator new(size_t sz);
        static void  operator delete(void*, size_t) {}
    };
    mem_desc_int_t* _int;
    uint8_t* to_vram(void);
    void from_vram(uint8_t* p);
    void _sync(uint8_t bank);
    uint8_t _read(uint16_t addr);
    void _write(uint16_t addr, uint8_t v);
public:
    static void reset(void);
    mem_desc_t() : _int( new mem_desc_int_t() ) {}
    mem_desc_t(const mem_desc_t& s) : _int( s._int ) {}
    mem_desc_t(uint8_t* p, uint32_t page) : _int( new mem_desc_int_t() ) {
        this->_int->p = p;
        this->_int->page_idx = (uint16_t)page;
    }
    void operator=(const mem_desc_t& s) {
        _int = s._int;
    }
    inline uint8_t* direct(void) {
        return _int->p;
    }
    // Force-load this SPI page from PSRAM into the pool without claiming a CPU
    // bank slot.  Bank=255 is a sentinel: _sync checks plugged_in[0..3] only,
    // so 255 is never matched → all 4 active bank pointers are checked (a page
    // currently mapped into any CPU bank is safely skipped rather than evicted).
    // Nop when already POINTER (page already in SRAM).  Called at DS80 activate
    // to ensure the color-attr page is in SRAM for fast direct rendering.
    inline void preload() { if (_int->mem_type != POINTER) _sync(255); }
    inline mem_type_t memType(void) { return (mem_type_t)_int->mem_type; }
    inline uint32_t   spiBase(void) { return _int->vram_off(); }
    // Load into an SRAM pool frame (evicting a victim if needed) and plug the
    // slot bookkeeping.  Use when the caller NEEDS a real pointer (MB-02 page
    // memset/getPage, accessor promotion); CPU bank-switch sites use sync().
    inline uint8_t* materialize(uint8_t bank) {
        if (_int->mem_type != POINTER) {
            _sync(bank);
        }
        uint8_t* res = _int->p;
        if (bank < 4) {
            plugged_in[bank] = res;
            bool* d = _int->is_rom ? &dirty_sink : &_int->dirty;
            bank_dirty[bank * 2] = d;
            bank_dirty[bank * 2 + 1] = d;
#if MEM_ACCESS_TRACE
            uint32_t* a = _int->is_rom ? &access_sink : &_int->acc;
            bank_access[bank * 2] = a;
            bank_access[bank * 2 + 1] = a;
#endif
        }
        return res;
    }
    // Accessor-aware plug for CPU bank switches: an SPI-backed non-resident
    // page is NOT loaded — the slot goes to accessor mode (returns nullptr,
    // which the callers store into ramCurrent[bank]; readbyte/writebyte and
    // romPeek detect it).  SD-swap pages and banks ≥4 keep the eager load
    // (per-byte SD access would be catastrophically slow).
    inline uint8_t* sync(uint8_t bank) {
        if (bank < 4 && _int->mem_type != POINTER &&
            (psram_size() >= _int->vram_off() + MEM_PG_SZ ||
             vram_butter(_int->vram_off()))) {
            acc_bank[bank] = *this;
            plugged_in[bank] = 0;
            // accessor writes go straight to backing
            bank_dirty[bank * 2] = &dirty_sink;
            bank_dirty[bank * 2 + 1] = &dirty_sink;
            return nullptr;
        }
        return materialize(bank);
    }
    inline uint8_t read(uint16_t addr) {
        if (_int->mem_type != POINTER) {
            return _read(addr);
        }
        return _int->p[addr];
    }
    inline void write(uint16_t addr, uint8_t v) {
        if (_int->mem_type != POINTER) {
            return _write(addr, v);
        }
        _int->dirty = true;
        _int->p[addr] = v;
    }
     // virtual RAM - PSRAM or swap
    inline void assign_vram(uint32_t page, mem_type_t mem_type) {
        this->_int->p = 0;
        this->_int->page_idx = (uint16_t)page;
        this->_int->mem_type = mem_type;
    }
    static inline uint8_t* revoke_1_ram_page() {
        auto it = pages.begin();
        if (it == pages.end()) return 0;
        it->sync(5); // TODO: optimize it
        uint8_t* p = it->_int->p;
        if (!it->_int->mem_type != POINTER) {
            it->to_vram();
        }
        pages.erase(it);
        return p;
    }
    inline void assign_ram(uint8_t* p, uint32_t page, bool locked) {
        this->_int->p = p;
        this->_int->page_idx = (uint16_t)page;
        this->_int->mem_type = POINTER;
        if (!locked) {
            pages.push_back(*this);
        }
    }
    // Mark this pool entry as pinned: _sync() skips it when looking for a
    // victim to evict.  The page stays in the pool (pool size unchanged) so
    // other banks can still find enough evictable slots.  Nop for non-pool pages.
    inline void pin()   { _int->pinned = true;  }
    inline void unpin() { _int->pinned = false; }
    // Direct handle to the dirty flag — for writers that modify the frame
    // behind writebyte's back through a raw pointer (MB-02 page0 window).
    // An unmarked write means the eviction write-back is skipped → data loss.
    inline bool* dirty_ptr() { return &_int->dirty; }
    inline void assign_rom(const uint8_t* p) { // TODO: prev?
        this->_int->p = (uint8_t*)p;
        this->_int->page_idx = 0;
        this->_int->mem_type = POINTER;
        this->_int->is_rom = true;
    }
    inline bool is_rom(void) {
        return this->_int->is_rom;
    }
    void from_file(FIL* f, size_t sz);
    void to_file(FIL* f, size_t sz);
    // A piece of the page in whatever store backs it (SRAM, butter, SPI PSRAM, SD
    // swap) — .pss sparse pages (src/speccy/core/Pss.cpp). Cold code.
    void read_chunk(uint32_t off, uint8_t* buf, uint32_t n);
    void write_chunk(uint32_t off, const uint8_t* buf, uint32_t n);
    void from_mem(mem_desc_t& ram, size_t sz);
    void cleanup();
};

class MemESP
{
public:
    static mem_desc_t rom[64];
    static mem_desc_t* ram;

    static bool newSRAM;

    // 8 slots of 8K each (Next MMU granularity); legacy 16K pages occupy
    // slot pairs via plug16()/plugPtr16(). Hot paths index by addr >> 13.
    static uint8_t* ramCurrent[8];
    static void nextContendRefresh();   // Next: ramContended[] from mmu[] (NextReg::contendRecalc)
    static bool ramContended[8];

    // Plug a 16K block into 16K bank s16 (0..3) — both 8K slots at once.
    // A nullptr block (accessor-mode bank from sync()) keeps BOTH slots null:
    // readbyte/writebyte/romPeek detect the null slot and serve per-byte.
    inline static void plugPtr16(uint8_t s16, uint8_t* p) {
        ramCurrent[s16 * 2] = p;
        ramCurrent[s16 * 2 + 1] = p ? p + 0x2000 : nullptr;
        // legacy paging took over slots 0/1 — drop the Next ROM write protect
        if (s16 == 0) next_rom_mask = 0;
    }
    inline static void plug16(uint8_t s16, uint8_t* p, bool contended) {
        plugPtr16(s16, p);
        ramContended[s16 * 2] = contended;
        ramContended[s16 * 2 + 1] = contended;
    }

    static uint8_t notMore128;
    static uint32_t page0ram;
    static uint32_t bankLatch;
    static uint8_t videoLatch;
    static uint8_t romLatch;
    static uint8_t pagingLock;

    static uint8_t romInUse;

    // +2A/+3 secondary paging (#1FFD). plus3Remap() is the ONE place that turns
    // (port1FFD, bankLatch, romLatch) into ramCurrent[]/ramContended[]/romInUse; it is
    // out-of-line and cold (it runs on a paging write, never per instruction), and it
    // takes the latch as an argument so MemESP does not have to include Ports.h.
    // p3special mirrors "an all-RAM configuration is active": recoverPage0() must not
    // touch page 0 then, because in configurations 1-3 it holds RAM page 4, which the
    // generic page0ram path (which always maps ram[0]) would get wrong.
    static uint8_t p3special;
    static void plus3Remap(uint8_t p1ffd);

    // ROM overlay registry (RomOverlay.h). A base+patch ROM variant (e.g. TR-DOS
    // 5.03/5.04TM, 48K Spanish) leaves rom[bank] pointing at its base ROM and
    // registers a flash overlay keyed by the base pointer. On a page-0 ROM read,
    // MemESP looks up the overlay by the bank pointer and substitutes patched bytes.
    // Keyed by base pointer so multiple families (rom[0] machine ROM + rom[4] TR-DOS)
    // coexist. Empty registry => zero hot-path cost (default ROMs).
    static const uint8_t* overlayBase[8];
    static const uint8_t* overlayPtr[8];
    // Flat 16 KB copy of base+overlay in butter PSRAM, or nullptr while the
    // binary-search path (rom_overlay_byte) still serves this entry. Filled
    // lazily by materializeOverlays() (ESPectrum::loop), cached per (base, ov)
    // across re-registrations — GMX/ProfROM re-register on every bank switch.
    static const uint8_t* overlayFlat[8];
    static void materializeOverlays();
    static size_t overlayFlatBytes();     // butter PSRAM held by materialised ROM overlays
    static uint8_t        overlayCount;
    // ov == nullptr unregisters `base`. Call at ROM-bank assignment.
    static void registerOverlay(const uint8_t* base, const uint8_t* ov);
    static inline void clearOverlays() { overlayCount = 0; }  // call before re-binding a machine's ROMs
    static inline const uint8_t* overlayFor(const uint8_t* p) {
        for (uint8_t i = 0; i < overlayCount; i++)
            if (overlayBase[i] == p) return overlayPtr[i];
        return nullptr;
    }
    // Read the byte at Z80 address `addr` through slot pointer `p`, applying
    // any registered page-0 ROM overlay. Use at EVERY ROM read fast path
    // (readbyte, fetchOpcode, the Z80 core's inline fetch) so overlays are
    // consistent. `slot` = addr >> 13, `p` = ramCurrent[slot], `addr` is the
    // FULL Z80 address (masking is internal).
    // The overlay registry is keyed by the 16K base pointer, so the page-0
    // lookup uses ramCurrent[0] (slot 1 holds base+0x2000, which would miss).
    static inline uint8_t romPeek(uint8_t slot, uint8_t* p, uint16_t addr) {
        if (__builtin_expect(overlayCount != 0, 0) && slot <= 1) {
            for (uint8_t i = 0; i < overlayCount; i++)
                if (overlayBase[i] == ramCurrent[0]) {
                    const uint8_t* f = overlayFlat[i];
                    return f ? f[addr & 0x3fff]
                             : rom_overlay_byte(overlayPtr[i], ramCurrent[0], addr & 0x3fff);
                }
        }
        // Accessor-mode bank (sync() deferred the 16KB load): serve per-byte.
        if (__builtin_expect(p == nullptr, 0)) return accessorRead(slot >> 1, addr & 0x3fff);
        return p[addr & 0x1fff];
    }

    // Accessor-mode bank window (cold paths, MemESP.cpp): per-byte SPI access
    // + promotion into the pool after MEM_ACC_PROMOTE_AT accesses.
    static uint8_t accessorRead(uint8_t bank, uint16_t off);
    static void    accessorWrite(uint8_t bank, uint16_t off, uint8_t v);
    static void    promoteBank(uint8_t bank);
    // Deferred promotions (Profi on butter): bank-trampoline storms used to
    // cluster several 16KB pool swaps into one emulated frame (negative IDL).
    // Only the FIRST promotion of a frame runs inline; the rest are queued and
    // executed by idleService() in the frame's idle window — the accessor
    // window keeps serving the bank per-byte meanwhile, so this is purely a
    // scheduling change.  promoFrameReset() is called once per frame.
    static void promoFrameReset(uint8_t inlineBudget);
    static void idleService(uint64_t deadline_us);
    // For code that needs a raw ramCurrent[slot] pointer (tape flashload,
    // debugger poke): force an accessor-mode bank into a real SRAM frame.
    // Takes an 8K slot index; the promotion works on the owning 16K bank.
    static inline void ensureResident(uint8_t slot) {
        if (slot < 8 && ramCurrent[slot] == nullptr) promoteBank(slot >> 1);
    }

    // ===== ZX Spectrum Next MMU (8K pages, nextreg 0x50-0x57) =====
    // 224 pages = 1.75 MB. Banks 0-7 (classic 128K) stay in static SRAM via
    // ram[0..7]; banks 8-111 are carved from butter PSRAM at boot
    // (buildNextRam), accounted through butter_pages so DivMMC/GS/arena
    // compose on top exactly like Murmuzavr pages would.
    static constexpr int NEXT_PAGES = 224;
    // The page mapping is DERIVED, not a 224-entry pointer table (that was 896 B
    // of .bss on every board, read on the hottest Next paths): banks 0-7 are the
    // static SRAM ZX pages and everything from page 16 up is ONE linear butter
    // strip — exactly what buildNextRam() stores. nextRamReady is the "mapping
    // exists at all" answer the callers used a null table entry for.
    static uint8_t* nextPg[16];
    static uint8_t* nextStrip;
    // Hot-page pool (Next only, NEXT_HOT_FRAMES 8K SRAM frames; MemESP.cpp): the
    // pages the CPU keeps mapped are copied out of butter PSRAM into SRAM so the
    // Z80 and the renderers stop paying XIP misses on them. nextHotIdx is null
    // until the pool exists; entry 0xFF = the page lives at its home address.
    // nextPgPtr is the ONE resolver every Next consumer uses, so the pool is
    // invisible to them; pointers already handed out (ramCurrent, wrOverlay,
    // l2RdPtr) are swapped by the pool itself on every promotion/eviction.
    static constexpr int NEXT_HOT_MAX = 8;
    static uint8_t* nextHotIdx;
    static uint8_t* nextHotFrame[NEXT_HOT_MAX];
    __attribute__((always_inline)) static inline uint8_t* nextHomePtr(unsigned pg) {
        return (pg < 16) ? nextPg[pg] : nextStrip + ((size_t)(pg - 16) << 13);
    }
    __attribute__((always_inline)) static inline uint8_t* nextPgPtr(unsigned pg) {
        if (!nextRamReady || pg >= NEXT_PAGES) return nullptr;
        if (nextHotIdx) {
            const uint8_t f = nextHotIdx[pg];
            if (f != 0xFF) return nextHotFrame[f];
        }
        return nextHomePtr(pg);
    }
    static void nextHotTick();     // once per frame (VIDEO::EndFrame, Next branch)
    static void nextHotFlush();    // write every frame back home and empty the pool
    static void nextHotRelease();  // ...and free the frames (UI entry); re-taken by nextHotTick
    // First byte after the RAM pages: the ROM/Multiface/Alt-ROM region.
    __attribute__((always_inline)) static inline uint8_t* nextRomRegion() {
        return nextStrip + ((size_t)(NEXT_PAGES - 16) << 13);
    }
    static uint8_t  mmu[8];        // current MMU mapping; 0xE0-0xFF = ROM (slots 0/1)
    // Slots 2-7 showing no SRAM (an MMU page >= 0xE0, an unbacked Layer 2 read
    // page): zxnext.vhd raises no SRAM request there and the CPU reads the
    // board's port-A data latch, i.e. the LAST byte read from SRAM. Non-zero
    // only on the Next; it turns the fast paths off (Video.cpp tsFastMemRecalc)
    // so every read reaches gsDmaPeek8 (CPU.cpp), which returns nextStaleByte().
    static uint8_t  next_stale_mask;
    static uint8_t  nextStaleByte();
    static bool     nextRamReady;  // buildNextRam() completed

    static bool buildNextRam();    // carve Next RAM out of butter PSRAM
    static void applyMMU(uint8_t slot, uint8_t page);
    // Next: while the divMMC window is paged in, slots 0/1 of ramCurrent[] ARE
    // the divMMC pages (page0_lo / page0_hi), so the fast read/fetch path
    // (TsFastMem.h, g_ts_fastrd == 2) can load from them directly. Every
    // generic accessor still tests divmmc_mapped first, and writes below
    // 0x4000 never take the fast path, so nothing else reads these two
    // pointers as the MMU's. Plugged by DivMMC::applyMapping and re-plugged
    // by applyMMU for slots 0/1; the window going away re-applies the MMU.
    static inline void nextDivPlug() {
        ramCurrent[0] = page0_lo;
        ramCurrent[1] = page0_hi;
    }
    static void resetNextMapping();// apply power-on MMU defaults

    // Write redirection per 8K slot, bit n = slot n: the Layer 2 window
    // (port 0x123B, slots 0/1, or 0-5 in the 48K segment mode) and the TBBLUE
    // config window (slots 0/1). A slot whose READ is mapped to Layer 2 while
    // its write is not carries the MMU page here, since ramCurrent then points
    // into Layer 2 (NextReg::updateLayer2Window).
    static uint8_t* wrOverlay[8];
    static uint8_t  wr_overlay_mask;
    // Layer 2 read mapping (port 0x123B bit 2): applyMMU puts these pointers
    // in ramCurrent for the slots in the mask (the divMMC window still wins).
    static uint8_t* l2RdPtr[8];      // null = past the RAM: open bus
    static uint8_t  l2_rd_mask;
    static uint8_t  l2_wr_mmu_mask;  // read-mapped slots whose writes go to the MMU page

    // Next ROMs loaded from SD into butter PSRAM (enNextZX.rom, 4x16K +
    // enNxtmmc.rom, 8K). PSRAM is writable, so ROM-mapped slots 0/1 are
    // write-protected via next_rom_mask in writebyte().
    static uint8_t* nextRomBase;     // 64K NextZXOS ROM set (null = 128K fallback)
    static uint8_t* nextDivRomBase;  // 8K Next MMC ROM (null = esxdos fallback)
    // Next Multiface (RAMPAGE 5, NextMf.h): enNextMf.rom (8K, read-only) and
    // 8K of MF RAM, right after the Alt-ROM banks. mfRomBase stays null until
    // the ROM is loaded (SD, or TBBLUE.FW writing RAMPAGE 5 in config mode);
    // an MF NMI is refused without it.
    static uint8_t* mfRomBase;
    static uint8_t* mfRamBase;
    static uint8_t  next_rom_mask;   // bit N: slot N (0/1) has ROM mapped
    static bool     next_rom3;       // Next: ROM in 0000-3FFF counts as ROM3 for divMMC automap (NextReg applyRomSelect)
    static bool loadNextRoms();      // load ROM files from SD (after buildNextRam)

    // Alt-ROM (nextreg 0x8C): two hidden 16K RAM banks that can replace the
    // machine ROM for reads or capture its writes (NextZXOS patches itself
    // through this). 32K carved right after the ROM region.
    static uint8_t* altRomBase;      // 2 x 16K (bank 0 = 128k-style, 1 = 48k-style)
    static bool altrom_rd;           // reads of slots 0/1 come from alt-rom
    static bool altrom_wr;           // writes to slots 0/1 go into alt-rom
    static uint8_t altrom_sel;       // which 16K alt bank is active (0/1)

    // ===== TBBLUE boot chain (FPGA bootrom + config mode) =====
    // Real power-on: the FPGA's 8K bootrom is readable (mirrored) over
    // 0x0000-0x3FFF and config_mode = 1 (zxnext.vhd:1102). The bootrom loads
    // TBBLUE.FW from SD; the firmware streams the ROM images through the
    // NR 0x04 RAMPAGE window (writes bypass the bootrom read overlay —
    // zxnext.vhd:1856 gates READS only), then exits via NR 0x03. Armed only
    // when TBBLUE.FW is present on the card (see ESPectrum::setup); the
    // direct enNextZX.rom load remains the fallback boot.
    static const uint8_t* nextBootRom;  // 8K image (flash) or nullptr = direct boot
    static bool nextBootRomActive;      // reads of 0x0000-0x3FFF come from bootrom
    static bool nextConfigMode;         // NR 0x03 config_mode flip-flop
    static uint8_t nextConfigPage;      // NR 0x04 RAMPAGE latch
    static void applyNextConfigWindow();
    static uint8_t* configBackingHalf(uint8_t page, uint8_t half);

    static uint8_t* page0_lo;      // 0x0000-0x1FFF when DivMMC/MB02 mapped
    static uint8_t* page0_hi;      // 0x2000-0x3FFF when DivMMC/MB02 mapped
    static bool divmmc_lo_rom;     // page0_lo is the MMC ROM — drop writes.
                                   // The old ">= 0x11000000 means RAM bank"
                                   // heuristic broke on Next, where enNxtmmc
                                   // itself lives in writable butter: stray
                                   // guest writes through the CONMEM window
                                   // scribbled it ("enNxtmmc.rom invalid"
                                   // after F11, .tap ISR derail — hw
                                   // 2026-08-09)
    static bool divmmc_mapped;     // DivMMC/MB02 memory currently visible at page 0
    static bool* divmmc_hi_dirty;  // swap mode: points to slot_dirty[] for page0_hi slot
    static bool* divmmc_lo_dirty;  // swap mode: points to slot_dirty[] for page0_lo slot
    static bool mb02_write_gate;   // MB-02: true = SRAM writable, false = read-only
    static bool* mb02_page_dirty;  // MB-02: dirty flag of the SRAM page mapped at
                                   // page0 (pool frame!) — writes through the
                                   // page0 window must mark it or the eviction
                                   // write-back is skipped (BS-DOS data loss)

    static uint8_t readbyte(uint16_t addr);
    static uint16_t readword(uint16_t addr);
    static void writebyte(uint16_t addr, uint8_t data);
    static void writeword(uint16_t addr, uint16_t data);

    // What the Z80 ACTUALLY sees, for the cold paths that show memory to a human
    // (the debugger, its disassembly and hex panels, the dump file). Unlike
    // readbyte/writebyte these go through the TC2068 SCLD window — that hook
    // deliberately lives in Z80Ops and not in readbyte, which is inlined into
    // ~170 sites (see Timex.h) — and they never trip a memory breakpoint: a
    // debugger reading memory must not fire the breakpoints it is displaying.
    static uint8_t dbgPeek(uint16_t addr);
    static void    dbgPoke(uint16_t addr, uint8_t data);

    // Cold out-of-line breakpoint checks: keep the 20-entry scan and the
    // portBasedBP store out of every inlined readbyte/writebyte expansion
    // in the Z80 core (icache footprint), entered only when mem BPs exist.
    static void checkMemReadBP(uint16_t addr);
    static void checkMemWriteBP(uint16_t addr);

    static int getByteContention(uint16_t addr);

    inline static void recoverPage0() {
        if (__builtin_expect(p3special != 0, 0)) return;   // +3 all-RAM: plus3Remap owns page 0
        // Pointer-only, like the pre-8K code: page-0 contention never changes.
        MemESP::plugPtr16(0, MemESP::newSRAM ? MemESP::ram[MEM_PG_CNT + MemESP::romLatch].sync(0) :
                             (MemESP::page0ram ? MemESP::ram[0].sync(0) : MemESP::rom[MemESP::romInUse].direct()));
    }
};

// Inline memory access functions

// ==== Функция получения задержки для адреса ====
inline int MemESP::getByteContention(uint16_t addr) {
    if (addr < 0xC000) return 0;

    int res = 0;
    uint16_t offset = addr - 0xC000;
    uint8_t val = (offset < 512) ? romDd10[offset] : romDd11[offset - 512];
    if (val == 0xEE) res = 4;
    else if (val == 0xFE) res = 3;
    else if (val == 0xBE) res = 2;
    else res = 1;

    return res;
}

inline uint8_t MemESP::readbyte(uint16_t addr) {
    if (__builtin_expect(Config::numMemReadBP != 0, 0))
        checkMemReadBP(addr);
    uint8_t slot = addr >> 13;
    if (slot <= 1 && divmmc_mapped) {
        return (slot == 0) ? page0_lo[addr] : page0_hi[addr & 0x1FFF];
    }
#if MEM_ACCESS_TRACE
    // Count only RAM accesses (flash ROM at 0x10xxxxxx is plugged via direct()
    // without sync(), so its slot pointer would be stale — the guard filters it).
    if (ramCurrent[slot] >= (uint8_t*)0x11000000) *mem_desc_t::bank_access[slot] += 1;
#endif
    return romPeek(slot, ramCurrent[slot], addr);
}

inline uint16_t MemESP::readword(uint16_t addr) {
    return ((readbyte(addr + 1) << 8) | readbyte(addr));
}

inline void MemESP::writebyte(uint16_t addr, uint8_t data)
{
    if (__builtin_expect(Config::numMemWriteBP != 0, 0))
        checkMemWriteBP(addr);
    uint8_t slot = addr >> 13;
    if (slot <= 1 && ramCurrent[0] == g_alfWindow) return; // ALF lazy cart: page 0 is ROM
    if (slot <= 1 && divmmc_mapped) {
        if (slot == 0) {
            // 0x0000-0x1FFF: writable only when MAPRAM (RAM bank) / MB-02 SRAM
            // In swap mode: divmmc_lo_dirty != null means heap bank (MAPRAM)
            if (divmmc_lo_rom) return;       // MMC ROM half — real hw drops
            if (divmmc_lo_dirty) {
                page0_lo[addr] = data;
                *divmmc_lo_dirty = true;
            } else if (page0_lo >= (uint8_t*)0x11000000) {
                if (!mb02_write_gate) return; // MB-02 write protect
                page0_lo[addr] = data;
                if (mb02_page_dirty) *mb02_page_dirty = true; // pool frame → mark for write-back
            }
        } else {
            // 0x2000-0x3FFF: always RAM bank, writable
            if (page0_hi >= (uint8_t*)0x11000000 && !mb02_write_gate) return;
            page0_hi[addr & 0x1FFF] = data;
            if (divmmc_hi_dirty) *divmmc_hi_dirty = true;
            else if (mb02_page_dirty) *mb02_page_dirty = true; // pool frame → mark for write-back
        }
        return;
    }
    // Next Layer 2 write window (port 0x123B) / TBBLUE config window. AFTER the
    // divMMC window: zxnext.vhd ~3085 serves divmmc_rom_en / divmmc_ram_en
    // before sram_layer2_map_en. The other way round, an esxDOS IM1 handler
    // under a Layer 2 game saved its SP into Layer 2 instead of divMMC RAM
    // and returned to 0x0000 (hw 2026-10-08, LmixLoRs.snx -> NextZXOS menu).
    if ((wr_overlay_mask >> slot) & 1) {
        wrOverlay[slot][addr & 0x1fff] = data;
        return;
    }
    // Next ROM lives in writable PSRAM — enforce read-only (after DivMMC:
    // an automapped page 0 must stay writable per the rules above).
    // Alt-ROM write-redirect captures these writes instead (nextreg 0x8C
    // bit 6 — NextZXOS patches its ROMs through this).
    if (slot <= 1 && (next_rom_mask & (1 << slot))) {
        if (altrom_wr && altRomBase)
            altRomBase[(uint32_t)altrom_sel * 0x4000 + ((uint32_t)slot << 13) + (addr & 0x1fff)] = data;
        return;
    }
    uint8_t* p = ramCurrent[slot];
    // Accessor-mode bank: write goes straight to the SPI backing store
    // (must be checked before the ROM filter — nullptr < 0x11000000).
    if (__builtin_expect(p == nullptr, 0)) {
        accessorWrite(slot >> 1, addr & 0x3fff, data);
        return;
    }
    if (p < (uint8_t*)0x11000000) return;
    *mem_desc_t::bank_dirty[slot] = true;
#if MEM_ACCESS_TRACE
    *mem_desc_t::bank_access[slot] += 1;
#endif
    // NOTE: the Profi CP/M BOOTFDD has 0x801A = 0xC9, which the BIOS reads via
    // `LD A,(0x801A); AND A; JR NZ` to select the floppy boot path (A≠0).
    // An earlier experiment patched this byte to 0x00 to force the HDD boot path,
    // but the HDD CP/M boot is not implemented and that patch hung the boot.
    // We boot CP/M from floppy; the IDE HDD remains accessible as a data drive
    // from within CP/M via the Profi IDE ports — so no 0x801A patch here.
#if PROFI_PORT_TRACE
    // Intercept writes to the currently-displayed DS80 page (colour or pixel).
    // Fires when the Z80 writes to the page the VGA/HDMI renderer is reading from.
    {
        extern uint8_t* ds80_dbg_clrmem;
        extern uint8_t* ds80_dbg_grmem;
        extern int      ds80_dbg_wr_cnt;
        uint8_t* p16 = ramCurrent[slot & ~1]; // dbg pointers are 16K page bases
        if ((ds80_dbg_clrmem && p16 == ds80_dbg_clrmem) ||
            (ds80_dbg_grmem  && p16 == ds80_dbg_grmem)) {
            if (ds80_dbg_wr_cnt < 30) {
                bool is_clr = (ds80_dbg_clrmem && p16 == ds80_dbg_clrmem);
                // _ds80_dbg_get_pc() is defined in Ports.cpp (which includes z80.h).
                extern uint16_t _ds80_dbg_get_pc(void);
                Debug::log("[WR→DISPLAY_%s] a=%04X d=%02X PC=%04X #%d",
                    is_clr ? "CLR" : "PIX", addr, data,
                    _ds80_dbg_get_pc(), ++ds80_dbg_wr_cnt);
            }
        }
    }
#endif
    p[addr & 0x1fff] = data;
}

inline void MemESP::writeword(uint16_t addr, uint16_t data) {
    writebyte(addr, (uint8_t)data);
    writebyte(addr + 1, (uint8_t)(data >> 8));
}


#endif