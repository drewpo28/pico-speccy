#include "CodeOverlay.h"

#if TSCONF_CODE_OVERLAY || GS_CODE_OVERLAY

#include <cstring>
#include <cstdint>
#include "hardware/sync.h"
#include "Debug.h"

extern "C" {
// rp2350-memmap.ld. A window that is not built still exports its symbols, with
// start == end, so the code below needs no per-window #if.
extern char __tsovl_win_start[], __tsovl_win_end[];
extern char __tsovl_start[], __tsovl_end[], __tsovl_source[];
extern char __gsovl_win_start[], __gsovl_win_end[];
extern char __gsovl_start[], __gsovl_end[], __gsovl_source[];
extern char __StackLimit[];
extern char __end__[];          // heap start (the linker also aliases it as `end`,
                                // which cannot be used here: <string> drags in
                                // std::end and the name becomes ambiguous)
}

// Heap ceiling enforced by our _sbrk below. Starts at the BOTTOM of the LOWEST
// window — every window reserved, i.e. exactly what the linker gives us with the
// overlays compiled out — and is only ever RAISED. See the header for why that
// direction is the safe one.
static char* s_ceiling  = __tsovl_win_start;
static char* s_heap_end = nullptr;      // our own copy of the sbrk cursor
static bool  s_ts_loaded = false;
static bool  s_gs_loaded = false;

// Replaces the SDK's __weak _sbrk (pico_clib_interface/newlib_interface.c),
// which bounds on &__StackLimit unconditionally. Identical otherwise — newlib's
// malloc is the only caller, and pico_malloc's mutex serialises it exactly as
// before.
extern "C" void* _sbrk(int incr) {
    if (s_heap_end == nullptr) s_heap_end = __end__;
    char* const prev = s_heap_end;
    char* const next = s_heap_end + incr;
    if (__builtin_expect(next > s_ceiling, false)) return (void*)-1;
    s_heap_end = next;
    return (void*)prev;
}

// The live heap ceiling, for the heap probes in OSDMain.cpp. They used to read
// the link-time &__HeapLimit, which is __StackLimit — up to 40 KB above the real
// ceiling while both windows are reserved. Over-reporting there is not cosmetic:
// getContiguousHeap() is an allocation GATE, and a gate that promises memory
// _sbrk() then refuses turns into pico_malloc's panic-on-OOM.
extern "C" char* heap_ceiling_now() { return s_ceiling; }

// Lowest reserved window base, else the stack limit. See the ordering note in
// the header: a released window only reaches the heap while everything below it
// is released too.
static void recomputeCeiling() {
    if (s_ts_loaded)      s_ceiling = __tsovl_win_start;
    else if (s_gs_loaded) s_ceiling = __gsovl_win_start;
    else                  s_ceiling = __StackLimit;
}

static void loadWindow(char* dst, const char* src, size_t n) {
    if (n) memcpy(dst, src, n);
    // The RP2350's Cortex-M33 has no instruction cache over SRAM (only the XIP
    // cache, and this is not XIP), so ordering the stores before the first fetch
    // is all that is needed. Nothing runs from a window yet either way: core1 is
    // gated on g_ts_c1_live / GS::enabled and the guest has not started.
    __dmb();
    __isb();
}

void CodeOverlay::apply(bool tsconf, bool gs) {
    const unsigned tw = (unsigned)(__tsovl_win_end - __tsovl_win_start);
    const unsigned tu = (unsigned)(__tsovl_end - __tsovl_start);
    const unsigned gw = (unsigned)(__gsovl_win_end - __gsovl_win_start);
    const unsigned gu = (unsigned)(__gsovl_end - __gsovl_start);

    if (gs && gw) {
        loadWindow(__gsovl_start, __gsovl_source, gu);
        s_gs_loaded = true;
    }
    if (tsconf && tw) {
        loadWindow(__tsovl_start, __tsovl_source, tu);
        s_ts_loaded = true;
    }
    recomputeCeiling();

    if (tw) Debug::log("[OVL] TS-Conf %s: %u of %u B window @%08lX",
                       s_ts_loaded ? "code resident" : "window to the heap",
                       tu, tw, (unsigned long)(uintptr_t)__tsovl_win_start);
    if (gw) Debug::log("[OVL] GS/NeoGS %s: %u of %u B window @%08lX",
                       s_gs_loaded ? "code resident" : "window to the heap",
                       gu, gw, (unsigned long)(uintptr_t)__gsovl_win_start);
    Debug::log("[OVL] heap ceiling %08lX (+%u B over both windows reserved)",
               (unsigned long)(uintptr_t)s_ceiling,
               (unsigned)(s_ceiling - __tsovl_win_start));
}

bool CodeOverlay::claimForTsconf() {
    if (s_ts_loaded) return true;
    if ((unsigned)(__tsovl_win_end - __tsovl_win_start) == 0) return true;   // overlay compiled out
    // Anything the heap handed out lives strictly below the sbrk cursor, so the
    // window is untouched exactly while the cursor has not entered it. A free
    // chunk straddling the boundary cannot exist for the same reason.
    if (s_heap_end != nullptr && s_heap_end > __tsovl_win_start) {
        Debug::log("[OVL] cannot claim the TS-Conf window: heap grew into it (%08lX > %08lX) - reboot needed",
                   (unsigned long)(uintptr_t)s_heap_end, (unsigned long)(uintptr_t)__tsovl_win_start);
        return false;
    }
    loadWindow(__tsovl_start, __tsovl_source, (size_t)(__tsovl_end - __tsovl_start));
    s_ts_loaded = true;
    recomputeCeiling();
    Debug::log("[OVL] TS-Conf window claimed mid-session (%u B resident)",
               (unsigned)(__tsovl_end - __tsovl_start));
    return true;
}

unsigned CodeOverlay::windowBytes(bool gs) {
    return gs ? (unsigned)(__gsovl_win_end - __gsovl_win_start)
              : (unsigned)(__tsovl_win_end - __tsovl_win_start);
}
unsigned CodeOverlay::contentBytes(bool gs) {
    return gs ? (unsigned)(__gsovl_end - __gsovl_start)
              : (unsigned)(__tsovl_end - __tsovl_start);
}
bool CodeOverlay::loaded(bool gs) { return gs ? s_gs_loaded : s_ts_loaded; }

#else  // no overlay at all

extern "C" char __HeapLimit[];
extern "C" char* heap_ceiling_now() { return __HeapLimit; }

void CodeOverlay::apply(bool, bool)      {}
bool CodeOverlay::claimForTsconf()       { return true; }
unsigned CodeOverlay::windowBytes(bool)  { return 0; }
unsigned CodeOverlay::contentBytes(bool) { return 0; }
bool CodeOverlay::loaded(bool)           { return true; }

#endif
