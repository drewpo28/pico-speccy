#include "CodeOverlay.h"

#if TSCONF_CODE_OVERLAY || GS_CODE_OVERLAY || DMA_CODE_OVERLAY || VDAC2_CODE_OVERLAY

#include <cstring>
#include <cstdint>
#include "hardware/sync.h"
#include "Debug.h"
#include "HeapRegions.h"

extern "C" {
// rp2350-memmap.ld. A window that is not built still exports its symbols, with
// start == end, so the code below needs no per-window #if.
extern char __ftovl_win_start[], __ftovl_win_end[];
extern char __ftovl_start[], __ftovl_end[], __ftovl_source[];
extern char __tsovl_win_start[], __tsovl_win_end[];
extern char __tsovl_start[], __tsovl_end[], __tsovl_source[];
extern char __tsovl_bss_start[], __tsovl_bss_end[];
extern char __dmaovl_win_start[], __dmaovl_win_end[];
extern char __dmaovl_start[], __dmaovl_end[], __dmaovl_source[];
extern char __gsovl_win_start[], __gsovl_win_end[];
extern char __gsovl_start[], __gsovl_end[], __gsovl_source[];
extern char __gsovl_bss_start[], __gsovl_bss_end[];
extern char __ngsovl_win_start[], __ngsovl_win_end[];
extern char __ngsovl_start[], __ngsovl_end[], __ngsovl_source[];
extern char __ngsovl_bss_start[], __ngsovl_bss_end[];
extern char __StackLimit[];
extern char __end__[];          // heap start (the linker also aliases it as `end`,
                                // which cannot be used here: <string> drags in
                                // std::end and the name becomes ambiguous)
}

// The heap is a LIST OF REGIONS, not one ceiling. Every window this boot does
// not load is heap; a window that IS loaded splits the heap around it. So the
// regions are: the base [__end__, first loaded window) and then every run of
// released windows between/above loaded ones — at most three regions with the
// four windows laid out [.tsovl][.dmaovl][.ngsovl][.gsovl] under the stack
// (.ngsovl is never resident without .gsovl, so it never splits a run).
//
// Our _sbrk grows the heap inside the current region and, when a request does
// not fit, JUMPS to the next region that can take it. newlib's malloc is
// dlmalloc 2.6.4 (_mallocr.c, malloc_extend_top) and it was written for exactly
// this: a MORECORE that returns an address other than the old top end is the
// "foreign sbrk" case — it fences the old top with two in-use fenceposts, frees
// it into the bins, and starts a new top at the returned address. The bytes
// left in the old region are therefore not lost. (Before this the heap ended
// at the LOWEST loaded window: on a TS-Conf boot the released Z80 DMA + GS
// windows — 32 KB — sat above the ceiling and reached nobody.)
//
// Two consequences to keep straight:
//  - a jump is permanent for the allocator (its top moves up), so the cursor
//    only ever moves to a HIGHER region; getFreeHeap() counts the regions the
//    cursor has not reached yet as free, because they are;
//  - dlmalloc rounds every non-initial extension up to a 4 KB page, so a region
//    can only be entered by a request whose rounded size fits it, and its last
//    <4 KB are never handed out. That is the same loss the single ceiling always
//    had at its own top; it is why the heap probes round a stranded region down.
static HeapRegions s_hr = { { { __end__, __ftovl_win_start, __end__ } }, 1, 0, nullptr, __ftovl_win_start };
static bool  s_ft_loaded = false;
static bool  s_ts_loaded = false;
static bool  s_dma_loaded = false;
static bool  s_gs_loaded = false;
static bool  s_ngs_loaded = false;

static void regionsRebuild() {
    const HrWindow w[5] = {
        { __ftovl_win_start,  __ftovl_win_end,  s_ft_loaded  },
        { __tsovl_win_start,  __tsovl_win_end,  s_ts_loaded  },
        { __dmaovl_win_start, __dmaovl_win_end, s_dma_loaded },
        { __ngsovl_win_start, __ngsovl_win_end, s_ngs_loaded },
        { __gsovl_win_start,  __gsovl_win_end,  s_gs_loaded  },
    };
    hr_rebuild(&s_hr, __end__, w, 5, __StackLimit);
}

// Replaces the SDK's __weak _sbrk (pico_clib_interface/newlib_interface.c),
// which bounds on &__StackLimit unconditionally. newlib's malloc is the only
// caller, and pico_malloc's mutex serialises it exactly as before.
extern "C" void* _sbrk(int incr) { return hr_sbrk(&s_hr, __end__, incr); }

// The live heap ceiling, for the heap probes in OSDMain.cpp. They used to read
// the link-time &__HeapLimit, which is __StackLimit — up to 40 KB above the real
// ceiling while both windows are reserved. Over-reporting there is not cosmetic:
// getContiguousHeap() is an allocation GATE, and a gate that promises memory
// _sbrk() then refuses turns into pico_malloc's panic-on-OOM.
extern "C" char* heap_ceiling_now() { return s_hr.ceiling; }
// The regions the cursor has not reached yet are free by construction — nothing
// has ever been handed out there; these are what the probes add.
extern "C" size_t heap_stranded_bytes()   { return hr_stranded_bytes(&s_hr); }
extern "C" size_t heap_stranded_largest() { return hr_stranded_largest(&s_hr); }
static bool windowTouched(char* ws)       { return hr_window_touched(&s_hr, ws) != 0; }

static void logRegions() {
    for (unsigned i = 0; i < s_hr.n; i++)
        Debug::log("[OVL] heap region %u: %08lX..%08lX (%u B)%s", i,
                   (unsigned long)(uintptr_t)s_hr.reg[i].lo, (unsigned long)(uintptr_t)s_hr.reg[i].hi,
                   (unsigned)(s_hr.reg[i].hi - s_hr.reg[i].lo), i == s_hr.cur ? " <- sbrk" : "");
}

static void loadWindow(char* dst, const char* src, size_t n) {
    if (n) memcpy(dst, src, n);
    // The TS window carries a NOLOAD data tail (TS_OVL_BSS): crt0 never zeroes
    // it (it is not .bss), and the heap may have used the bytes before a
    // mid-session claim.
    if (dst == __tsovl_start) memset(__tsovl_bss_start, 0, (size_t)(__tsovl_bss_end - __tsovl_bss_start));
    // The GS and NeoGS windows carry the same kind of tail (GS_OVL_BSS /
    // NGS_OVL_BSS): loaded once at boot, over bytes nothing has used yet, but
    // .bss semantics are "zero" and nothing else provides it.
    if (dst == __gsovl_start)  memset(__gsovl_bss_start,  0, (size_t)(__gsovl_bss_end  - __gsovl_bss_start));
    if (dst == __ngsovl_start) memset(__ngsovl_bss_start, 0, (size_t)(__ngsovl_bss_end - __ngsovl_bss_start));
    // The RP2350's Cortex-M33 has no instruction cache over SRAM (only the XIP
    // cache, and this is not XIP), so ordering the stores before the first fetch
    // is all that is needed. Nothing runs from a window yet either way: core1 is
    // gated on g_ts_c1_live / GS::enabled and the guest has not started.
    __dmb();
    __isb();
}

void CodeOverlay::apply(bool tsconf, bool gs, bool dma, bool ngs, bool vdac2) {
    struct W { const char* name; char* ws; char* we; char* cs; char* ce; char* src; bool want; bool* got; };
    W w[5] = {
        { "VDAC2",   __ftovl_win_start,  __ftovl_win_end,  __ftovl_start,  __ftovl_end,  __ftovl_source,  tsconf && vdac2, &s_ft_loaded },
        { "TS-Conf", __tsovl_win_start,  __tsovl_win_end,  __tsovl_start,  __tsovl_end,  __tsovl_source,  tsconf,     &s_ts_loaded  },
        { "Z80 DMA", __dmaovl_win_start, __dmaovl_win_end, __dmaovl_start, __dmaovl_end, __dmaovl_source, dma,        &s_dma_loaded },
        { "NeoGS",   __ngsovl_win_start, __ngsovl_win_end, __ngsovl_start, __ngsovl_end, __ngsovl_source, gs && ngs,  &s_ngs_loaded },
        { "GS",      __gsovl_win_start,  __gsovl_win_end,  __gsovl_start,  __gsovl_end,  __gsovl_source,  gs,         &s_gs_loaded  },
    };
    for (auto& x : w) {
        const unsigned win = (unsigned)(x.we - x.ws);
        if (!win) continue;
        if (x.want) { loadWindow(x.cs, x.src, (size_t)(x.ce - x.cs)); *x.got = true; }
        Debug::log("[OVL] %s %s: %u of %u B window @%08lX%s", x.name,
                   *x.got ? "code resident" : "window to the heap",
                   (unsigned)(x.ce - x.cs), win, (unsigned long)(uintptr_t)x.ws,
                   ((x.cs == __tsovl_start && __tsovl_bss_end > __tsovl_bss_start) ||
                    (x.cs == __gsovl_start && __gsovl_bss_end > __gsovl_bss_start) ||
                    (x.cs == __ngsovl_start && __ngsovl_bss_end > __ngsovl_bss_start)) ? " (+data tail)" : "");
    }
    regionsRebuild();
    Debug::log("[OVL] heap ceiling %08lX (+%u B over all windows reserved, +%u B stranded above a resident window)",
               (unsigned long)(uintptr_t)s_hr.ceiling,
               (unsigned)(s_hr.ceiling - __ftovl_win_start), (unsigned)heap_stranded_bytes());
    logRegions();
}

bool CodeOverlay::claimForTsconf() {
    if (s_ts_loaded) return true;
    if ((unsigned)(__tsovl_win_end - __tsovl_win_start) == 0) return true;   // overlay compiled out
    // Anything the heap handed out lives strictly below its region's high-water
    // mark, so the window is untouched exactly while that mark has not entered it.
    // A free chunk straddling the boundary cannot exist for the same reason.
    if (windowTouched(__tsovl_win_start)) {
        Debug::log("[OVL] cannot claim the TS-Conf window: heap grew into it (cursor %08lX, window %08lX) - reboot needed",
                   (unsigned long)(uintptr_t)s_hr.cursor, (unsigned long)(uintptr_t)__tsovl_win_start);
        return false;
    }
    loadWindow(__tsovl_start, __tsovl_source, (size_t)(__tsovl_end - __tsovl_start));
    s_ts_loaded = true;
    regionsRebuild();
    Debug::log("[OVL] TS-Conf window claimed mid-session (%u B resident)",
               (unsigned)(__tsovl_end - __tsovl_start));
    return true;
}

// Claim the Z80 DMA window on a live SET_DMA turn-on. Unlike TS-Conf there is no
// reboot to fall back on, so a refusal simply leaves the feature off.
bool CodeOverlay::claimForDma() {
    if (s_dma_loaded) return true;
    if ((unsigned)(__dmaovl_win_end - __dmaovl_win_start) == 0) return true;   // compiled out
    if (windowTouched(__dmaovl_win_start)) {
        Debug::log("[OVL] cannot claim the Z80 DMA window: heap grew into it (cursor %08lX, window %08lX)",
                   (unsigned long)(uintptr_t)s_hr.cursor, (unsigned long)(uintptr_t)__dmaovl_win_start);
        return false;
    }
    loadWindow(__dmaovl_start, __dmaovl_source, (size_t)(__dmaovl_end - __dmaovl_start));
    s_dma_loaded = true;
    regionsRebuild();
    Debug::log("[OVL] Z80 DMA window claimed mid-session (%u B resident)",
               (unsigned)(__dmaovl_end - __dmaovl_start));
    return true;
}

unsigned CodeOverlay::windowBytes(Which x) {
    switch (x) { case WIN_GS:  return (unsigned)(__gsovl_win_end - __gsovl_win_start);
                 case WIN_NGS: return (unsigned)(__ngsovl_win_end - __ngsovl_win_start);
                 case WIN_DMA: return (unsigned)(__dmaovl_win_end - __dmaovl_win_start);
                 case WIN_FT:  return (unsigned)(__ftovl_win_end - __ftovl_win_start);
                 default:      return (unsigned)(__tsovl_win_end - __tsovl_win_start); }
}
unsigned CodeOverlay::contentBytes(Which x) {
    switch (x) { case WIN_GS:  return (unsigned)(__gsovl_end - __gsovl_start);
                 case WIN_NGS: return (unsigned)(__ngsovl_end - __ngsovl_start);
                 case WIN_DMA: return (unsigned)(__dmaovl_end - __dmaovl_start);
                 case WIN_FT:  return (unsigned)(__ftovl_end - __ftovl_start);
                 default:      return (unsigned)(__tsovl_end - __tsovl_start); }
}
bool CodeOverlay::loaded(Which x) {
    switch (x) { case WIN_GS: return s_gs_loaded; case WIN_NGS: return s_ngs_loaded;
                 case WIN_DMA: return s_dma_loaded; case WIN_FT: return s_ft_loaded;
                 default: return s_ts_loaded; }
}

#else  // no overlay at all

extern "C" char __HeapLimit[];
extern "C" char* heap_ceiling_now() { return __HeapLimit; }
extern "C" size_t heap_stranded_bytes()   { return 0; }
extern "C" size_t heap_stranded_largest() { return 0; }

void CodeOverlay::apply(bool, bool, bool, bool, bool) {}
bool CodeOverlay::claimForTsconf()                { return true; }
bool CodeOverlay::claimForDma()                   { return true; }
unsigned CodeOverlay::windowBytes(Which)          { return 0; }
unsigned CodeOverlay::contentBytes(Which)         { return 0; }
bool CodeOverlay::loaded(Which)                   { return true; }

#endif
