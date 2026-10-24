// Host stand-in for the parts of the Raspberry Pi Pico SDK the emulator sources use.
// Every <pico/...> and <hardware/...> header in host/include includes this one file.
//
// Rules: timing is real (time_us_* from a monotonic clock), synchronisation is real
// where it guards cross-thread data (spin locks = one recursive-free mutex each),
// everything that touches silicon (clocks, pads, flash, QMI, watchdog) is a no-op
// that reports success, and register blocks are plain zeroed structs so that the
// odd direct register read compiles and yields 0.
#pragma once
#include <stddef.h>
#define MHZ 1000000
#define KHZ 1000

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- pico/platform.h, pico.h -------------------------------------------------
typedef unsigned int uint;

#define __not_in_flash(group)
#define __not_in_flash_func(func_name) func_name
#define __no_inline_not_in_flash_func(func_name) __attribute__((noinline)) func_name
#define __time_critical_func(func_name) func_name
#define __scratch_x(group)
#define __scratch_y(group)
#define __in_flash(group)
#define __uninitialized_ram(group) group
#ifndef __force_inline
#define __force_inline inline __attribute__((always_inline))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __used
#define __used __attribute__((used))
#endif
#ifndef __noinline
#define __noinline __attribute__((noinline))
#endif
#ifndef __weak
#define __weak __attribute__((weak))
#endif
#ifndef __printflike
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#endif
#ifndef __noreturn
#define __noreturn __attribute__((noreturn))
#endif
#ifndef count_of
#define count_of(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(a, b) ((b) < (a) ? (b) : (a))
#endif
#ifndef MAX
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#endif
#ifndef __compiler_memory_barrier
#define __compiler_memory_barrier() __asm__ volatile("" ::: "memory")
#endif

static inline void __dmb(void) { __sync_synchronize(); }
static inline void __dsb(void) { __sync_synchronize(); }
static inline void __isb(void) { __sync_synchronize(); }
static inline void __sev(void) {}
static inline void __wfe(void) {}
static inline void __wfi(void) {}
static inline void __nop(void) {}
static inline void __breakpoint(void) {}
static inline void tight_loop_contents(void) {}

uint get_core_num(void);                       // 0 = emulator thread, 1 = render thread
void panic(const char* fmt, ...) __attribute__((noreturn));
void panic_unsupported(void) __attribute__((noreturn));
#define hard_assert(x) do { if (!(x)) panic("hard_assert"); } while (0)
#define invalid_params_if(a, b) ((void)0)
#define valid_params_if(a, b) ((void)0)

// ---- pico/time.h ------------------------------------------------------------
typedef uint64_t absolute_time_t;
uint64_t time_us_64(void);
static inline uint32_t time_us_32(void) { return (uint32_t)time_us_64(); }
void sleep_us(uint64_t us);
void sleep_ms(uint32_t ms);
void busy_wait_us(uint64_t us);
static inline void busy_wait_us_32(uint32_t us) { busy_wait_us(us); }
static inline void busy_wait_ms(uint32_t ms) { busy_wait_us((uint64_t)ms * 1000); }
static inline absolute_time_t get_absolute_time(void) { return time_us_64(); }
static inline uint64_t to_us_since_boot(absolute_time_t t) { return t; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) { return time_us_64() + (uint64_t)ms * 1000; }
static inline absolute_time_t make_timeout_time_us(uint64_t us) { return time_us_64() + us; }
static inline absolute_time_t delayed_by_us(absolute_time_t t, uint64_t us) { return t + us; }
static inline absolute_time_t delayed_by_ms(absolute_time_t t, uint32_t ms) { return t + (uint64_t)ms * 1000; }
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) { return (int64_t)(to - from); }
static inline bool time_reached(absolute_time_t t) { return time_us_64() >= t; }
static inline bool is_nil_time(absolute_time_t t) { return t == 0; }
#define nil_time ((absolute_time_t)0)
#define at_the_end_of_time ((absolute_time_t)UINT64_MAX)
static inline void update_us_since_boot(absolute_time_t* t, uint64_t us) { *t = us; }

typedef struct repeating_timer repeating_timer_t;
typedef bool (*repeating_timer_callback_t)(repeating_timer_t* rt);
struct repeating_timer {
    int64_t delay_us;
    void* pool;
    int alarm_id;
    repeating_timer_callback_t callback;
    void* user_data;
};
typedef int32_t alarm_id_t;
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void* user_data);
bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t cb, void* user_data, repeating_timer_t* out);
static inline bool add_repeating_timer_ms(int32_t delay_ms, repeating_timer_callback_t cb, void* user_data, repeating_timer_t* out) {
    return add_repeating_timer_us((int64_t)delay_ms * 1000, cb, user_data, out);
}
bool cancel_repeating_timer(repeating_timer_t* timer);
alarm_id_t add_alarm_in_us(uint64_t us, alarm_callback_t cb, void* user_data, bool fire_if_past);
static inline alarm_id_t add_alarm_in_ms(uint32_t ms, alarm_callback_t cb, void* user_data, bool fire_if_past) {
    return add_alarm_in_us((uint64_t)ms * 1000, cb, user_data, fire_if_past);
}
bool cancel_alarm(alarm_id_t id);

// ---- hardware/sync.h ------------------------------------------------------------
typedef volatile uint32_t spin_lock_t;
spin_lock_t* spin_lock_instance(uint lock_num);
uint spin_lock_get_num(spin_lock_t* lock);
int spin_lock_claim_unused(bool required);
void spin_lock_claim(uint lock_num);
void spin_lock_unclaim(uint lock_num);
spin_lock_t* spin_lock_init(uint lock_num);
uint32_t spin_lock_blocking(spin_lock_t* lock);
void spin_unlock(spin_lock_t* lock, uint32_t saved_irq);
void spin_lock_unsafe_blocking(spin_lock_t* lock);
void spin_unlock_unsafe(spin_lock_t* lock);
bool is_spin_locked(spin_lock_t* lock);
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t status) { (void)status; }
static inline void restore_interrupts_from_disabled(uint32_t status) { (void)status; }
#define PICO_SPINLOCK_ID_OS1 22

// ---- pico/stdio/driver.h ------------------------------------------------------------
typedef struct stdio_driver {
    void (*out_chars)(const char* buf, int len);
    void (*out_flush)(void);
    int (*in_chars)(char* buf, int len);
    void (*set_chars_available_callback)(void (*fn)(void*), void* param);
    struct stdio_driver* next;
    bool last_ended_with_cr;
    bool crlf_enabled;
} stdio_driver_t;
static inline void stdio_set_driver_enabled(stdio_driver_t* d, bool e) { (void)d; (void)e; }
static inline bool stdio_init_all(void) { return true; }
static inline void stdio_flush(void) { fflush(stdout); }

// ---- pico/error.h -------------------------------------------------------------------
#ifndef PICO_OK
enum { PICO_OK = 0, PICO_ERROR_NONE = 0, PICO_ERROR_GENERIC = -1, PICO_ERROR_TIMEOUT = -2 };
#endif

// ---- pico/rand.h --------------------------------------------------------------------
uint32_t get_rand_32(void);
uint64_t get_rand_64(void);

// ---- hardware/flash.h, xip cache, addressmap ------------------------------------------
#define FLASH_PAGE_SIZE (1u << 8)
#define FLASH_SECTOR_SIZE (1u << 12)
#define FLASH_BLOCK_SIZE (1u << 16)
#define FLASH_UNIQUE_ID_SIZE_BYTES 8
#ifndef XIP_BASE
#define XIP_BASE 0x10000000u
#endif
#define XIP_NOCACHE_NOALLOC_BASE 0x14000000u
#define SRAM_BASE 0x20000000u
// SYSINFO package select: bit 0 = RP2350A. The host reports an RP2350B (48 GPIOs).
extern uint32_t host_sysinfo[4];
#define SYSINFO_BASE ((uintptr_t)host_sysinfo)
#define SYSINFO_PACKAGE_SEL_OFFSET 4
// SIO CPUID (offset 0) is how code asks "which core am I": per-thread on the host.
extern __thread uint32_t host_cpuid;
#define SIO_BASE ((uintptr_t)&host_cpuid)
#define SRAM_END 0x20082000u
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4u * 1024 * 1024)
#endif
void flash_range_erase(uint32_t flash_offs, size_t count);
void flash_range_program(uint32_t flash_offs, const uint8_t* data, size_t count);
void flash_get_unique_id(uint8_t* id_out);
void flash_do_cmd(const uint8_t* txbuf, uint8_t* rxbuf, size_t count);
static inline void xip_cache_invalidate_all(void) {}
static inline void xip_cache_invalidate_range(uint32_t a, uint32_t n) { (void)a; (void)n; }
static inline void xip_cache_clean_all(void) {}
static inline void xip_cache_clean_range(uint32_t a, uint32_t n) { (void)a; (void)n; }

// ---- hardware/watchdog.h ------------------------------------------------------------
typedef struct { volatile uint32_t ctrl, load, reason, scratch[8], tick; } watchdog_hw_t;
extern watchdog_hw_t host_watchdog_hw;
#define watchdog_hw (&host_watchdog_hw)
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);
void watchdog_enable(uint32_t delay_ms, bool pause_on_debug);
static inline void watchdog_disable(void) {}
static inline void watchdog_update(void) {}
static inline bool watchdog_caused_reboot(void) { return false; }
static inline bool watchdog_enable_caused_reboot(void) { return false; }

// ---- hardware/clocks.h, pll, vreg ------------------------------------------------------
enum clock_index { clk_gpout0 = 0, clk_gpout1, clk_gpout2, clk_gpout3, clk_ref, clk_sys, clk_peri, clk_hstx, clk_usb, clk_adc, CLK_COUNT };
uint32_t clock_get_hz(enum clock_index clk_index);
static inline bool clock_configure(enum clock_index c, uint32_t s, uint32_t a, uint32_t f, uint32_t t) { (void)c; (void)s; (void)a; (void)f; (void)t; return true; }
static inline void clock_configure_undivided(enum clock_index c, uint32_t s, uint32_t a, uint32_t f) { (void)c; (void)s; (void)a; (void)f; }
static inline void clock_stop(enum clock_index c) { (void)c; }
static inline void clock_gpio_init(uint g, uint s, float d) { (void)g; (void)s; (void)d; }
bool set_sys_clock_khz(uint32_t freq_khz, bool required);
static inline void set_sys_clock_pll(uint32_t vco, uint pd1, uint pd2) { (void)vco; (void)pd1; (void)pd2; }
static inline bool check_sys_clock_khz(uint32_t f, uint* v, uint* p1, uint* p2) { (void)f; if (v) *v = 0; if (p1) *p1 = 1; if (p2) *p2 = 1; return true; }
typedef enum { VREG_VOLTAGE_0_85 = 6, VREG_VOLTAGE_1_10 = 11, VREG_VOLTAGE_1_15, VREG_VOLTAGE_1_20, VREG_VOLTAGE_1_25,
               VREG_VOLTAGE_1_30, VREG_VOLTAGE_1_35, VREG_VOLTAGE_1_40, VREG_VOLTAGE_1_50, VREG_VOLTAGE_1_60,
               VREG_VOLTAGE_1_65, VREG_VOLTAGE_1_70, VREG_VOLTAGE_1_80, VREG_VOLTAGE_1_90, VREG_VOLTAGE_2_00,
               VREG_VOLTAGE_2_35, VREG_VOLTAGE_2_50, VREG_VOLTAGE_2_65, VREG_VOLTAGE_2_80, VREG_VOLTAGE_3_00,
               VREG_VOLTAGE_3_15, VREG_VOLTAGE_3_30, VREG_VOLTAGE_DEFAULT = VREG_VOLTAGE_1_10, VREG_VOLTAGE_MAX = VREG_VOLTAGE_3_30 } vreg_voltage_t;
static inline void vreg_set_voltage(vreg_voltage_t v) { (void)v; }
static inline vreg_voltage_t vreg_get_voltage(void) { return VREG_VOLTAGE_1_10; }
static inline void vreg_disable_voltage_limit(void) {}

// ---- hardware/gpio.h ---------------------------------------------------------------
enum gpio_function_rp2350 { GPIO_FUNC_HSTX = 0, GPIO_FUNC_SPI = 1, GPIO_FUNC_UART = 2, GPIO_FUNC_I2C = 3, GPIO_FUNC_PWM = 4,
                            GPIO_FUNC_SIO = 5, GPIO_FUNC_PIO0 = 6, GPIO_FUNC_PIO1 = 7, GPIO_FUNC_PIO2 = 8, GPIO_FUNC_GPCK = 9,
                            GPIO_FUNC_XIP_CS1 = 9, GPIO_FUNC_USB = 10, GPIO_FUNC_UART_AUX = 11, GPIO_FUNC_NULL = 0x1f };
typedef enum gpio_function_rp2350 gpio_function_t;
#define GPIO_OUT 1
#define GPIO_IN 0
enum gpio_slew_rate { GPIO_SLEW_RATE_SLOW = 0, GPIO_SLEW_RATE_FAST = 1 };
enum gpio_drive_strength { GPIO_DRIVE_STRENGTH_2MA = 0, GPIO_DRIVE_STRENGTH_4MA, GPIO_DRIVE_STRENGTH_8MA, GPIO_DRIVE_STRENGTH_12MA };
#define NUM_BANK0_GPIOS 48
static inline void gpio_init(uint g) { (void)g; }
static inline void gpio_deinit(uint g) { (void)g; }
static inline void gpio_set_function(uint g, gpio_function_t f) { (void)g; (void)f; }
static inline gpio_function_t gpio_get_function(uint g) { (void)g; return GPIO_FUNC_NULL; }
static inline void gpio_set_dir(uint g, bool out) { (void)g; (void)out; }
static inline void gpio_put(uint g, bool v) { (void)g; (void)v; }
static inline bool gpio_get(uint g) { (void)g; return true; }
static inline bool gpio_get_out_level(uint g) { (void)g; return false; }
static inline void gpio_pull_up(uint g) { (void)g; }
static inline void gpio_pull_down(uint g) { (void)g; }
static inline void gpio_disable_pulls(uint g) { (void)g; }
static inline void gpio_set_pulls(uint g, bool u, bool d) { (void)g; (void)u; (void)d; }
static inline void gpio_set_input_enabled(uint g, bool e) { (void)g; (void)e; }
static inline void gpio_set_slew_rate(uint g, enum gpio_slew_rate s) { (void)g; (void)s; }
static inline void gpio_set_drive_strength(uint g, enum gpio_drive_strength s) { (void)g; (void)s; }
static inline void gpio_set_irq_enabled(uint g, uint32_t e, bool on) { (void)g; (void)e; (void)on; }
static inline void gpio_init_mask(uint64_t m) { (void)m; }
static inline void gpio_set_dir_out_masked(uint64_t m) { (void)m; }
static inline void gpio_set_dir_in_masked(uint64_t m) { (void)m; }
static inline void gpio_put_masked(uint64_t m, uint64_t v) { (void)m; (void)v; }
static inline uint32_t gpio_get_all(void) { return 0xFFFFFFFFu; }

// ---- hardware/uart.h -----------------------------------------------------------------
typedef struct uart_inst uart_inst_t;
typedef struct { volatile uint32_t dr, rsr, _pad[4], fr, _pad2, ilpr, ibrd, fbrd, lcr_h, cr, ifls, imsc, ris, mis, icr, dmacr; } uart_hw_t;
extern uart_inst_t* const host_uart0;
extern uart_inst_t* const host_uart1;
#define uart0 host_uart0
#define uart1 host_uart1
#define UART0_IRQ 33
#define UART1_IRQ 34
typedef enum { UART_PARITY_NONE, UART_PARITY_EVEN, UART_PARITY_ODD } uart_parity_t;
static inline uint uart_init(uart_inst_t* u, uint baud) { (void)u; return baud; }
static inline void uart_deinit(uart_inst_t* u) { (void)u; }
static inline uint uart_set_baudrate(uart_inst_t* u, uint baud) { (void)u; return baud; }
static inline void uart_set_format(uart_inst_t* u, uint d, uint s, uart_parity_t p) { (void)u; (void)d; (void)s; (void)p; }
static inline void uart_set_fifo_enabled(uart_inst_t* u, bool e) { (void)u; (void)e; }
static inline void uart_set_hw_flow(uart_inst_t* u, bool c, bool r) { (void)u; (void)c; (void)r; }
static inline void uart_set_irq_enables(uart_inst_t* u, bool rx, bool tx) { (void)u; (void)rx; (void)tx; }
static inline void uart_set_irqs_enabled(uart_inst_t* u, bool rx, bool tx) { (void)u; (void)rx; (void)tx; }
static inline bool uart_is_writable(uart_inst_t* u) { (void)u; return true; }
static inline bool uart_is_readable(uart_inst_t* u) { (void)u; return false; }
static inline bool uart_is_enabled(uart_inst_t* u) { (void)u; return false; }
static inline void uart_putc_raw(uart_inst_t* u, char c) { (void)u; (void)c; }
static inline void uart_putc(uart_inst_t* u, char c) { (void)u; (void)c; }
static inline void uart_puts(uart_inst_t* u, const char* s) { (void)u; (void)s; }
static inline char uart_getc(uart_inst_t* u) { (void)u; return 0; }
static inline void uart_write_blocking(uart_inst_t* u, const uint8_t* s, size_t n) { (void)u; (void)s; (void)n; }
static inline void uart_read_blocking(uart_inst_t* u, uint8_t* d, size_t n) { (void)u; memset(d, 0, n); }
static inline void uart_tx_wait_blocking(uart_inst_t* u) { (void)u; }
static inline uint uart_get_index(uart_inst_t* u) { return u == uart1 ? 1 : 0; }
static inline uart_inst_t* uart_get_instance(uint i) { return i ? uart1 : uart0; }
uart_hw_t* uart_get_hw(uart_inst_t* u);
#define UART_FUNCSEL_NUM(u, gpio) GPIO_FUNC_UART
#define UART_UARTFR_TXFF_BITS 0x20u
#define UART_UARTFR_RXFE_BITS 0x10u
#define UART_UARTFR_BUSY_BITS 0x08u
#define UART_UARTIMSC_RXIM_BITS 0x10u
#define UART_UARTIMSC_RTIM_BITS 0x40u

// ---- hardware/irq.h, exception.h -----------------------------------------------------
typedef void (*irq_handler_t)(void);
static inline void irq_set_enabled(uint n, bool e) { (void)n; (void)e; }
static inline bool irq_is_enabled(uint n) { (void)n; return false; }
static inline void irq_set_priority(uint n, uint8_t p) { (void)n; (void)p; }
static inline void irq_set_exclusive_handler(uint n, irq_handler_t h) { (void)n; (void)h; }
static inline void irq_add_shared_handler(uint n, irq_handler_t h, uint8_t p) { (void)n; (void)h; (void)p; }
static inline void irq_remove_handler(uint n, irq_handler_t h) { (void)n; (void)h; }
static inline void irq_set_pending(uint n) { (void)n; }
static inline irq_handler_t irq_get_exclusive_handler(uint n) { (void)n; return NULL; }
#define PICO_HIGHEST_IRQ_PRIORITY 0x00
#define PICO_DEFAULT_IRQ_PRIORITY 0x80
#define PICO_LOWEST_IRQ_PRIORITY 0xff
enum exception_number { NMI_EXCEPTION = -14, HARDFAULT_EXCEPTION = -13, MEMMANAGE_EXCEPTION = -12, BUSFAULT_EXCEPTION = -11,
                        USAGEFAULT_EXCEPTION = -10, SECUREFAULT_EXCEPTION = -9, SVCALL_EXCEPTION = -5, PENDSV_EXCEPTION = -2, SYSTICK_EXCEPTION = -1 };
typedef void (*exception_handler_t)(void);
static inline exception_handler_t exception_set_exclusive_handler(enum exception_number n, exception_handler_t h) { (void)n; (void)h; return NULL; }
static inline void exception_restore_handler(enum exception_number n, exception_handler_t h) { (void)n; (void)h; }

// ---- hardware/adc.h ------------------------------------------------------------------
static inline void adc_init(void) {}
static inline void adc_gpio_init(uint g) { (void)g; }
static inline void adc_select_input(uint i) { (void)i; }
static inline void adc_set_temp_sensor_enabled(bool e) { (void)e; }
static inline uint16_t adc_read(void) { return 876; }   // ~27 C by the datasheet formula

// ---- pico/multicore.h --------------------------------------------------------------------
void multicore_launch_core1(void (*entry)(void));
void multicore_reset_core1(void);
static inline void multicore_lockout_victim_init(void) {}
static inline bool multicore_lockout_victim_is_initialized(uint core) { (void)core; return false; }
static inline void multicore_lockout_start_blocking(void) {}
static inline void multicore_lockout_end_blocking(void) {}
static inline bool multicore_lockout_start_timeout_us(uint64_t us) { (void)us; return true; }
static inline bool multicore_lockout_end_timeout_us(uint64_t us) { (void)us; return true; }
static inline void multicore_fifo_push_blocking(uint32_t v) { (void)v; }
static inline uint32_t multicore_fifo_pop_blocking(void) { return 0; }
static inline bool multicore_fifo_rvalid(void) { return false; }
static inline bool multicore_fifo_wready(void) { return true; }
static inline void multicore_fifo_drain(void) {}

// ---- pico/bootrom.h ----------------------------------------------------------------------
static inline void reset_usb_boot(uint32_t m, uint32_t d) { (void)m; (void)d; exit(0); }
static inline void rom_reset_usb_boot(uint32_t m, uint32_t d) { (void)m; (void)d; exit(0); }
typedef void (*rom_flash_exit_xip_fn)(void);
typedef void (*rom_flash_flush_cache_fn)(void);
typedef void (*rom_connect_internal_flash_fn)(void);
static inline void* rom_func_lookup_inline(uint32_t code) { (void)code; return NULL; }
static inline void* rom_func_lookup(uint32_t code) { (void)code; return NULL; }
#define ROM_TABLE_CODE(c1, c2) ((c1) | ((c2) << 8))
#define ROM_FUNC_FLASH_EXIT_XIP ROM_TABLE_CODE('E', 'X')
#define ROM_FUNC_FLASH_FLUSH_CACHE ROM_TABLE_CODE('F', 'C')
#define ROM_FUNC_CONNECT_INTERNAL_FLASH ROM_TABLE_CODE('I', 'F')

// ---- register blocks read directly by the sources ---------------------------------------
// timer_hw->timerawl is read as a free-running microsecond counter in hot paths;
// it is the one register that has to be live (only C++ sources read it).
#ifdef __cplusplus
struct host_live_us { operator uint32_t() const { return (uint32_t)time_us_64(); } };
#define HOST_TIMERAWL host_live_us timerawl;
#else
#define HOST_TIMERAWL volatile uint32_t timerawl;
#endif
typedef struct { volatile uint32_t timehw, timelw, timehr, timelr, alarm[4], armed, timerawh; HOST_TIMERAWL
                 volatile uint32_t dbgpause, pause, locked, source, intr, inte, intf, ints; } timer_hw_t;
typedef struct { volatile uint32_t csr, rvr, cvr, calib; } systick_hw_t;
typedef struct { volatile uint32_t direct_csr, direct_tx, direct_rx;
                 struct { volatile uint32_t timing, rfmt, rcmd, wfmt, wcmd; } m[2];
                 volatile uint32_t atrans[8]; } qmi_hw_t;
typedef struct { volatile uint32_t ctrl, stat, ctr_hit, ctr_acc, stream_addr, stream_ctr, stream_fifo; } xip_ctrl_hw_t;
typedef struct { volatile uint32_t cs, pwr, fbdiv_int, prim, intr, inte, intf, ints; } pll_hw_t;
typedef struct { volatile uint32_t voltage_select, io[6]; } pads_qspi_hw_t;
typedef struct { volatile uint32_t regs[64]; volatile uint32_t chip_reset, wdsel, seq_cfg; } powman_hw_t;
extern timer_hw_t host_timer_hw;
extern systick_hw_t host_systick_hw;
extern qmi_hw_t host_qmi_hw;
extern xip_ctrl_hw_t host_xip_ctrl_hw;
extern pll_hw_t host_pll_hw;
extern pads_qspi_hw_t host_pads_qspi_hw;
extern powman_hw_t host_powman_hw;
#define timer_hw (&host_timer_hw)
#define systick_hw (&host_systick_hw)
#define qmi_hw (&host_qmi_hw)
#define xip_ctrl_hw (&host_xip_ctrl_hw)
#define pll_sys_hw (&host_pll_hw)
#define pll_usb_hw (&host_pll_hw)
#define pads_qspi_hw (&host_pads_qspi_hw)
#define powman_hw (&host_powman_hw)

static inline void hw_set_bits(volatile uint32_t* a, uint32_t m) { *a |= m; }
static inline void hw_clear_bits(volatile uint32_t* a, uint32_t m) { *a &= ~m; }
static inline void hw_xor_bits(volatile uint32_t* a, uint32_t m) { *a ^= m; }
static inline void hw_write_masked(volatile uint32_t* a, uint32_t v, uint32_t m) { *a = (*a & ~m) | (v & m); }

#ifdef __cplusplus
}
#endif

// ---- PIO / DMA / PWM / SPI / I2C: types only — no host code drives them -------------
#ifdef __cplusplus
extern "C" {
#endif
typedef struct pio_hw_host { int unused; } pio_hw_t;
typedef pio_hw_t* PIO;
extern pio_hw_t host_pio[3];
#define pio0 (&host_pio[0])
#define pio1 (&host_pio[1])
#define pio2 (&host_pio[2])
typedef struct { const uint16_t* instructions; uint8_t length; int8_t origin; uint8_t pio_version; } pio_program_t;
typedef struct { uint32_t clkdiv, execctrl, shiftctrl, pinctrl; } pio_sm_config;
typedef struct { volatile uint32_t ctrl; } dma_channel_hw_t;
typedef struct { uint32_t ctrl; } dma_channel_config;
typedef struct spi_inst spi_inst_t;
typedef struct i2c_inst i2c_inst_t;
#define spi0 ((spi_inst_t*)0)
#define spi1 ((spi_inst_t*)1)
#define i2c0 ((i2c_inst_t*)0)
#define i2c1 ((i2c_inst_t*)1)
#ifdef __cplusplus
}
#endif
