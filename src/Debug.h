#ifndef Debug_h
#define Debug_h

#include <stdio.h>
#include <inttypes.h>
#include <string>
#include <cstring>

using namespace std;

#define DEFAULT_BLINK_COUNT 5

// Mirror of DEBUG_LOG_PATH from FileUtils.h. Kept as a literal here so Debug.h
// stays a leaf header (FileUtils.h pulls MemESP.h which depends on Config).
#define STORAGE_LOG "/.config/pico-speccy/debug.log"

// Current stack pointer — for stack-depth watermarks in Debug::log lines.
static inline uint32_t debug_sp() { uint32_t sp; __asm volatile("mov %0, sp" : "=r"(sp)); return sp; }

class Debug
{
public:

    // Runtime toggle for log2SD writes. Persisted via Config save/load.
    // Owned by Debug to keep this a leaf header (Config.h includes Debug.h).
    static bool log_enabled;

    static void led_blink();
    static void led_on();
    static void led_off();

    static void log(const char* fmt, ...);
    // Drain the non-blocking debug-UART TX ring into the UART FIFO (no-op
    // without DBG_UART). Called from the frame-pacing idle in ESPectrum::loop;
    // Debug::log itself never waits on the wire (hw 2026-09-01: the 1 Hz
    // HDMIAU line spent ~6 ms/line spinning on FIFO space INSIDE the frame —
    // a one-sample audio kink every ~0.9 s and IDL<0 on otherwise-fine
    // frames, visible only on DBG_UART builds).
    static void pumpUart();

#if NEO8_TRAP
    // Temporary wild-jump hunter (Neo8 SDz crash): call per executed
    // instruction; logs the recent-PC history + frame/stack snapshot once
    // when execution enters the screen area while the Pentagon page0-RAM
    // overlay is active.
    static void neo8TrapStep(uint16_t pc, uint16_t sp, uint16_t ix, uint16_t iy);
#endif

    // Exception-safe variant for fault handlers: NO stdio/printf (the stdio
    // path takes print_mutex and WFEs — blocking in exception context; if the
    // other core died holding the mutex, the handler freezes the machine, and
    // a mutex assert inside an already-faulted core escalates to a double
    // fault → LOCKUP). Formats into a per-core static buffer (not the — maybe
    // overflowed — fault stack) and emits via the bounded lock-free debug
    // UART path. No-op when the debug UART is off.
    static void fault_log(const char* fmt, ...);

    // Runtime-gated. The flag check is inlined here so when logging is
    // disabled the call collapses to a single branch on a global bool.
    static void log2SD_impl(const string& data);
    static void log2SD_impl(const char* fmt, ...);

    static inline void log2SD(const string& data) {
        if (__builtin_expect(log_enabled, 0)) log2SD_impl(data);
    }
    template <typename... Args>
    static inline void log2SD(const char* fmt, Args... args) {
        if (__builtin_expect(log_enabled, 0)) log2SD_impl(fmt, args...);
    }
};

#ifdef __cplusplus
extern "C" {
#endif
void debug_log2sd(const char* fmt, ...);
#ifdef __cplusplus
}
#endif

#endif