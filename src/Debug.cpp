#include "Debug.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/stdio/driver.h"
#include <stdlib.h>
#include "FileUtils.h"

static uint32_t log_counter = 0;

bool Debug::log_enabled = false;

void Debug::led_blink()
{
#if DEBUG && defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    for (int i = 0; i < DEFAULT_BLINK_COUNT; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
#endif
}

void Debug::led_on()
{
#if DEBUG && defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    gpio_put(PICO_DEFAULT_LED_PIN, true);
#endif
}

void Debug::led_off()
{
#if DEBUG && defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    gpio_put(PICO_DEFAULT_LED_PIN, false);
#endif
}

// ── UART console (Debug > UART console) ──────────────────────────────────────
// Runtime, TX-only. s_dbg_uart is the single "console is live" flag: every writer
// tests it first, so the whole path costs one predicted branch while the console
// is off. The ring is heap-allocated by uartStart() (4 KB — see DBG_TX_RING), so
// the .bss cost of the feature is the handful of pointers below; the old
// DBG_UART builds carried the ring statically, ordinary builds nothing at all.
//
// Debug::log never waits on the wire: bytes go into the ring (drop on overflow),
// Debug::pumpUart() drains it from the frame-pacing idle, and each log call
// drains what fits the 32-byte FIFO for free so boot-time logging still flows
// without the main loop. A bounded spin was tried first (hw 2026-09-01): the 1 Hz
// HDMIAU line cost ~6 ms inside the frame once the FIFO filled — audio kink every
// ~0.9 s and IDL<0 on otherwise-fine frames.
//
// printf goes the same way: a private stdio_driver_t (s_dbg_stdio) hands its
// characters to the ring, so the ~40 TUs that printf (TinyUSB HID bring-up
// hints, the FDC, …) can never block on uart_write_blocking either — the SDK's
// stdio_uart driver is deliberately NOT linked (CMake pico_enable_stdio_uart 0).
// It must never bind to PICO_DEFAULT_UART either: the pico2 board header puts
// that on GP0/1, which is the ZiFi UART on PICO_DV.
#define DBG_TX_RING 4096
static uart_inst_t*     s_dbg_uart = nullptr;    // live console, or nullptr
static char*            s_dbg_ring = nullptr;
static volatile uint32_t s_dbg_w = 0, s_dbg_r = 0;
static uint8_t          s_dbg_tx_pin = 0xFF;

// Watchdog scratch tag: "the console was on" — survives every watchdog reboot
// (F12, esp_hard_reset, the fault handlers), NOT a RUN/POR reset (the block is
// cleared, and watchdog_caused_reboot() is false anyway). scratch[2] is the MIDI
// reflash request, [3] the uptime carry, [4..7] belong to the SDK's own reboot
// magic (watchdog_caused_reboot reads [4]) — so [1].
#define DBG_UART_SCRATCH   1
#define DBG_UART_TAG_ON    0xDB614A70u
#define DBG_UART_TAG_OFF   0xDB610FF0u

static inline void dbg_uart_drain_fifo(void)
{
    while (s_dbg_r != s_dbg_w && uart_is_writable(s_dbg_uart)) {
        uart_get_hw(s_dbg_uart)->dr = (uint8_t)s_dbg_ring[s_dbg_r];
        s_dbg_r = (s_dbg_r + 1) & (DBG_TX_RING - 1);
    }
}

static inline bool dbg_uart_put(char c)
{
    uint32_t w = s_dbg_w, nx = (w + 1) & (DBG_TX_RING - 1);
    if (nx == s_dbg_r) return false;     // ring full → drop, never wait
    s_dbg_ring[w] = c;
    s_dbg_w = nx;
    return true;
}

// Synchronous variant for the fault path only: we are crashing, blocking is
// fine, and the line must reach the wire. Flushes the ring first so the
// crash line lands in order after whatever was still queued.
static inline void dbg_uart_put_sync(char c)
{
    for (uint32_t spin = 0; !uart_is_writable(s_dbg_uart); ++spin)
        if (spin >= 200000u) return;
    uart_get_hw(s_dbg_uart)->dr = (uint8_t)c;
}

static void dbg_uart_flush_sync(void)
{
    while (s_dbg_r != s_dbg_w) {
        uint32_t spin = 0;
        while (!uart_is_writable(s_dbg_uart))
            if (++spin >= 200000u) return;
        uart_get_hw(s_dbg_uart)->dr = (uint8_t)s_dbg_ring[s_dbg_r];
        s_dbg_r = (s_dbg_r + 1) & (DBG_TX_RING - 1);
    }
}

// stdio driver: printf → ring. CRLF translation is left to the SDK (crlf_enabled),
// which inserts the '\r' before handing us the '\n'.
static void dbg_stdio_out_chars(const char* buf, int len)
{
    if (!s_dbg_uart) return;
    for (int i = 0; i < len; i++)
        if (!dbg_uart_put(buf[i])) break;
    dbg_uart_drain_fifo();
}
static void dbg_stdio_out_flush(void)
{
    if (s_dbg_uart) dbg_uart_drain_fifo();
}
static stdio_driver_t s_dbg_stdio = {
    .out_chars = dbg_stdio_out_chars,
    .out_flush = dbg_stdio_out_flush,
    .in_chars  = nullptr,
    .set_chars_available_callback = nullptr,
    .next = nullptr,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .last_ended_with_cr = false,
    .crlf_enabled = true,
#endif
};

void Debug::pumpUart() { if (s_dbg_uart) dbg_uart_drain_fifo(); }

bool     Debug::uartActive() { return s_dbg_uart != nullptr; }
unsigned Debug::uartTxPin()  { return s_dbg_uart ? s_dbg_tx_pin : 0xFFu; }

bool Debug::uartBootWanted()
{
    return watchdog_caused_reboot() && watchdog_hw->scratch[DBG_UART_SCRATCH] == DBG_UART_TAG_ON;
}

void Debug::uartSetWanted(bool on)
{
    watchdog_hw->scratch[DBG_UART_SCRATCH] = on ? DBG_UART_TAG_ON : DBG_UART_TAG_OFF;
}

bool Debug::uartStart(unsigned uart_index, unsigned tx_pin)
{
    if (s_dbg_uart) return true;
    if (!s_dbg_ring) {
        s_dbg_ring = (char*)malloc(DBG_TX_RING);
        if (!s_dbg_ring) return false;   // caller logs to SD; the console stays off
    }
    s_dbg_w = s_dbg_r = 0;
    uart_inst_t* u = uart_index ? uart1 : uart0;
    uart_init(u, 115200);
    uart_set_format(u, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(u, true);
    gpio_set_function(tx_pin, UART_FUNCSEL_NUM(u, tx_pin));
    s_dbg_tx_pin = (uint8_t)tx_pin;
    s_dbg_uart = u;                       // publish LAST: writers test this pointer
    stdio_set_driver_enabled(&s_dbg_stdio, true);
    return true;
}

void Debug::uartStop()
{
    if (!s_dbg_uart) return;
    dbg_uart_flush_sync();
    stdio_set_driver_enabled(&s_dbg_stdio, false);
    uart_inst_t* u = s_dbg_uart;
    s_dbg_uart = nullptr;                 // unpublish FIRST
    uart_deinit(u);
    gpio_set_function(s_dbg_tx_pin, GPIO_FUNC_SIO);
    gpio_set_dir(s_dbg_tx_pin, false);
    s_dbg_tx_pin = 0xFF;
    // A writer on the other core that had already tested s_dbg_uart is inside a
    // sub-microsecond ring store; give it time before the ring goes back to the heap.
    busy_wait_us(200);
    free(s_dbg_ring);
    s_dbg_ring = nullptr;
    s_dbg_w = s_dbg_r = 0;
}

// clk_peri follows clk_sys (set_sys_clock_pll re-derives it), so the divider
// programmed at 150 MHz is wrong at 378. uart_set_baudrate keeps the FIFO
// contents, unlike uart_init; drain first so the queued tail is not garbled.
void Debug::uartReclock()
{
    if (!s_dbg_uart) return;
    dbg_uart_flush_sync();
    uart_tx_wait_blocking(s_dbg_uart);
    uart_set_baudrate(s_dbg_uart, 115200);
}

void Debug::uartFlushSync()
{
    if (!s_dbg_uart) return;
    dbg_uart_flush_sync();
    uart_tx_wait_blocking(s_dbg_uart);   // 32 B @ 115200 ≈ 3 ms > the watchdog delay
}

void Debug::log(const char* fmt, ...)
{
    if (!s_dbg_uart) return;   // console off: no sink, so don't even format
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;

    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n' && !dbg_uart_put('\r')) break; // CRLF for terminals
        if (!dbg_uart_put(buf[i])) break;                 // ring full → drop remainder
    }
    if (dbg_uart_put('\r'))
        dbg_uart_put('\n');
    dbg_uart_drain_fifo();   // free: fills the 32-byte FIFO, never waits
}

#if NEO8_TRAP
#include "MemESP.h"
static uint16_t s_n8_ring[64];
static uint32_t s_n8_pos = 0;
static bool     s_n8_fired = false;

void __not_in_flash_func(Debug::neo8TrapStep)(uint16_t pc, uint16_t sp, uint16_t ix, uint16_t iy) {
    s_n8_ring[s_n8_pos++ & 63] = pc;
    if (s_n8_fired) return;
    if (pc < 0x4000 || pc >= 0x5B00) return;
    // No page0ram condition: the wild run can START before the overlay is
    // mapped (previous capture caught only a RET back into already-wild code).
    s_n8_fired = true;
    Debug::log("NEO8v2: wild jump to %04X (page0ram=%d bank=%d SP=%04X IX=%04X IY=%04X) — last PCs:",
               (unsigned)pc, (int)MemESP::page0ram, (int)MemESP::bankLatch,
               (unsigned)sp, (unsigned)ix, (unsigned)iy);
    for (int i = 63; i >= 0; i--) {
        Debug::log("NEO8v2:   pc-%02d = %04X", i,
                   (unsigned)s_n8_ring[(s_n8_pos - 1 - i) & 63]);
    }
    // Frame/stack snapshot: the epilogue thunk (0x026D) does LD SP,IX;
    // POP IX; POP DE; INC SP; INC SP; RET — so the caller's return slot the
    // RET consumed is at final-SP−2. Dump around both SP and IY (FATFS ptr).
    for (int off = -16; off <= 14; off += 2) {
        uint16_t a = sp + off;
        Debug::log("NEO8v2:   [SP%+03d %04X] = %02X%02X", off, (unsigned)a,
                   (unsigned)MemESP::readbyte(a + 1), (unsigned)MemESP::readbyte(a));
    }
    for (int off = 0; off < 16; off += 2) {
        uint16_t a = iy + off;
        Debug::log("NEO8v2:   [IY+%02d %04X] = %02X%02X", off, (unsigned)a,
                   (unsigned)MemESP::readbyte(a + 1), (unsigned)MemESP::readbyte(a));
    }
}
#endif

void Debug::fault_log(const char* fmt, ...)
{
    // Per-core static buffers: the fault stack may itself be the problem
    // (overflow), and both cores can fault near-simultaneously.
    if (!s_dbg_uart) return;   // no exception-safe sink without the console
    static char bufs[2][192];
    char* buf = bufs[*(volatile uint32_t*)0xD0000000u & 1];  // SIO CPUID

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(bufs[0]), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(bufs[0]) - 1) n = sizeof(bufs[0]) - 1;

    // Crashing: block as needed, and get the queued backlog out first so the
    // fault lines land in order.
    dbg_uart_flush_sync();
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') dbg_uart_put_sync('\r');
        dbg_uart_put_sync(buf[i]);
    }
    dbg_uart_put_sync('\r');
    dbg_uart_put_sync('\n');
}

void Debug::log2SD_impl(const string& data)
{
    if (!FileUtils::fsMount) return;
    static const char* nvs = STORAGE_LOG;

    // Заголовок сессии при первой записи
    bool first = (log_counter == 0);

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%08u %7u ", (unsigned)log_counter++,
             (unsigned)(to_ms_since_boot(get_absolute_time())));

    std::string logEntry;
    if (first) {
        const char* reason = watchdog_caused_reboot() ? "WATCHDOG" : "POWER-ON";
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "--- BOOT (%s) ---\n", reason);
        logEntry = std::string(hdr);
    }
    logEntry += std::string(prefix) + data + "\n";

    // Лимит 200KB — перезаписываем с начала при переполнении
    FIL* handle = fopen2(nvs, FA_WRITE | FA_OPEN_APPEND);
    if (!handle) {
        FileUtils::mkdirParents(CONFIG_DIR);
        handle = fopen2(nvs, FA_WRITE | FA_OPEN_APPEND);
    }
    if (handle) {
        if (f_size(handle) >= 204800) {
            fclose2(handle);
            handle = fopen2(nvs, FA_WRITE | FA_CREATE_ALWAYS);
            if (!handle) return;
        }
        UINT btw;
        f_write(handle, logEntry.c_str(), logEntry.size(), &btw);
        fclose2(handle);
    }
}

void Debug::log2SD_impl(const char* fmt, ...)
{
    if (!FileUtils::fsMount) return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    log2SD_impl(std::string(buf));
}

extern "C" void debug_log2sd(const char* fmt, ...)
{
    if (!Debug::log_enabled) return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Debug::log2SD_impl(std::string(buf));
}