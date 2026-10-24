// Host implementations of the Pico SDK functions and the board-level symbols that
// src/main.cpp and the linker script provide on the RP2350.
//
// Memory map: the emulator decides "ROM or RAM" by pointer address (< 0x11000000 is
// flash, see the 0x11000000 tests across src/). The host link therefore puts code and
// .rodata at 0x10000000 and .data/.bss at 0x20000000 (host/CMakeLists.txt), so the
// same tests hold on a PC. The "flash" the firmware writes (custom ROMs, the GM.DLS
// bank) is an array in its own section just below 0x11000000.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

#include "pico_host.h"

extern "C" {

// ---- time ------------------------------------------------------------------------------
static const auto s_t0 = std::chrono::steady_clock::now();
uint64_t time_us_64(void) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - s_t0).count();
}
void busy_wait_us(uint64_t us) {
    const uint64_t e = time_us_64() + us;
    while (time_us_64() < e) {}
}
void sleep_us(uint64_t us) {
    if (us >= 2000) std::this_thread::sleep_for(std::chrono::microseconds(us));
    else busy_wait_us(us);
}
void sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---- cores -----------------------------------------------------------------------------
__thread uint32_t host_cpuid = 0;
uint get_core_num(void) { return host_cpuid; }

void panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "*** PANIC *** ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}
void panic_unsupported(void) { panic("unsupported"); }

// core1 = a second thread. The emulator thread is core0.
void multicore_launch_core1(void (*entry)(void)) {
    std::thread([entry] { host_cpuid = 1; entry(); }).detach();
}
void multicore_reset_core1(void) {}

// ---- spin locks: real mutual exclusion (both "cores" are threads) ----------------------
static spin_lock_t s_locks[32];
spin_lock_t* spin_lock_instance(uint n) { return &s_locks[n & 31]; }
uint spin_lock_get_num(spin_lock_t* l) { return (uint)(l - s_locks); }
static std::atomic<uint32_t> s_claimed{0};
int spin_lock_claim_unused(bool required) {
    for (int i = 16; i < 32; i++) {
        const uint32_t bit = 1u << i;
        if (!(s_claimed.fetch_or(bit) & bit)) return i;
    }
    if (required) panic("no spin locks");
    return -1;
}
void spin_lock_claim(uint n) { s_claimed |= 1u << (n & 31); }
void spin_lock_unclaim(uint n) { s_claimed &= ~(1u << (n & 31)); }
spin_lock_t* spin_lock_init(uint n) { s_locks[n & 31] = 0; return &s_locks[n & 31]; }
void spin_lock_unsafe_blocking(spin_lock_t* l) {
    while (__atomic_exchange_n(l, 1u, __ATOMIC_ACQUIRE)) std::this_thread::yield();
}
void spin_unlock_unsafe(spin_lock_t* l) { __atomic_store_n(l, 0u, __ATOMIC_RELEASE); }
uint32_t spin_lock_blocking(spin_lock_t* l) { spin_lock_unsafe_blocking(l); return 0; }
void spin_unlock(spin_lock_t* l, uint32_t) { spin_unlock_unsafe(l); }
bool is_spin_locked(spin_lock_t* l) { return __atomic_load_n(l, __ATOMIC_ACQUIRE) != 0; }

// ---- random ----------------------------------------------------------------------------
uint32_t get_rand_32(void) {
    static std::mt19937 g{std::random_device{}()};
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    return g();
}
uint64_t get_rand_64(void) { return ((uint64_t)get_rand_32() << 32) | get_rand_32(); }

// ---- register blocks and instances -----------------------------------------------------
uint32_t host_sysinfo[4] = { 0, 0 /* package_sel: RP2350B */, 0, 0 };
watchdog_hw_t host_watchdog_hw;
timer_hw_t host_timer_hw;
systick_hw_t host_systick_hw;
qmi_hw_t host_qmi_hw;
xip_ctrl_hw_t host_xip_ctrl_hw;
pll_hw_t host_pll_hw;
pads_qspi_hw_t host_pads_qspi_hw;
powman_hw_t host_powman_hw;
pio_hw_t host_pio[3];
static uart_hw_t s_uart_hw[2];
uart_inst_t* const host_uart0 = (uart_inst_t*)&s_uart_hw[0];
uart_inst_t* const host_uart1 = (uart_inst_t*)&s_uart_hw[1];
uart_hw_t* uart_get_hw(uart_inst_t* u) { return (uart_hw_t*)u; }

// ---- clocks: the RP2350 numbers the firmware expects -----------------------------------
static uint32_t s_sys_hz = 252u * 1000000u;
uint32_t clock_get_hz(enum clock_index i) {
    return i == clk_sys || i == clk_peri ? s_sys_hz : 48000000u;
}
bool set_sys_clock_khz(uint32_t khz, bool) { s_sys_hz = khz * 1000u; return true; }

// ---- watchdog: a reboot restarts the process -------------------------------------------
extern char** host_argv;
static void host_restart(void) {
    fprintf(stderr, "host: firmware reboot -> restarting the process\n");
    fflush(stdout);
    fflush(stderr);
    execv("/proc/self/exe", host_argv);
    _exit(0);   // execv failed: just leave
}
void watchdog_reboot(uint32_t, uint32_t, uint32_t) { host_restart(); }
void watchdog_enable(uint32_t, bool) { host_restart(); }

// ---- flash: writes go straight to the addresses (they sit in writable memory) ----------
static void flash_unprotect(uint8_t* p, size_t n) {
    const uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE);
    const uintptr_t a = (uintptr_t)p & ~(pg - 1);
    const uintptr_t e = ((uintptr_t)p + n + pg - 1) & ~(pg - 1);
    mprotect((void*)a, e - a, PROT_READ | PROT_WRITE);
}
void flash_range_erase(uint32_t off, size_t n) {
    uint8_t* p = (uint8_t*)(uintptr_t)(XIP_BASE + off);
    flash_unprotect(p, n);
    memset(p, 0xFF, n);
}
void flash_range_program(uint32_t off, const uint8_t* d, size_t n) {
    uint8_t* p = (uint8_t*)(uintptr_t)(XIP_BASE + off);
    flash_unprotect(p, n);
    memcpy(p, d, n);
}
void flash_get_unique_id(uint8_t* id) { for (int i = 0; i < 8; i++) id[i] = (uint8_t)(0xA0 + i); }
void flash_do_cmd(const uint8_t*, uint8_t* rx, size_t n) { if (rx) memset(rx, 0, n); }

}  // extern "C"
