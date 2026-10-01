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
    // while the console is off). Called from the frame-pacing idle in
    // ESPectrum::loop; Debug::log itself never waits on the wire (hw
    // 2026-09-01: the 1 Hz HDMIAU line spent ~6 ms/line spinning on FIFO
    // space INSIDE the frame — a one-sample audio kink every ~0.9 s and IDL<0
    // on otherwise-fine frames, visible only with the console on).
    static void pumpUart();

    // ── UART console (Debug > UART console, Config::dbg_uart) ─────────────────
    // A RUNTIME feature since 2026-09-06 (the <BOARD>_DBG_UART build options are
    // gone). TX-only, 115200 8N1, on the board's DBG_UART_TX_PIN; nothing ever
    // reads it. Debug::log, fault_log and every printf (via a private stdio
    // driver) land in a 4 KB heap ring that pumpUart() drains — so with the
    // console OFF the firmware pays 8 bytes of .bss, and with it ON 4 KB of heap.
    //
    // Two ways to come up: (1) uartStart() from ESPectrum::setup once Config is
    // loaded (cold boot with the option on: the lines before that are lost);
    // (2) at main() ENTRY when the previous session left the "wanted" tag in a
    // watchdog scratch register — it survives F12 / a crash reboot, so the
    // early boot (chip_reset, flash timing, PSRAM, VIDEO::Init) IS logged on
    // every warm reboot. uartSetWanted() keeps that tag in step with Config.
    static bool uartActive();
    static bool uartBootWanted();                 // scratch tag says: start at main() entry
    static void uartSetWanted(bool on);           // write/clear the scratch tag
    static bool uartStart(unsigned uart_index, unsigned tx_pin);   // false: no heap for the ring
    static void uartStop();                       // console off, pin back to SIO input, ring freed
    static void uartReclock();                    // re-derive the baud after a clk_sys change
    static void uartFlushSync();                  // blocking drain (before a reboot)
    static unsigned uartTxPin();                  // live TX pin, 0xFF when off

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
    // UART path. No-op when the UART console is off.
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