// Host stand-ins for the board-level symbols src/main.cpp, the linker script and the
// hardware drivers (PSRAM, USB host, SD background reads) provide on the RP2350.
// None of that hardware exists here: PSRAM reports 0 bytes, USB reports nothing
// mounted, and the SD background-read machinery is never armed (g_sd_c1_live = 0),
// so every caller takes its plain path.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdio>
#include "host_posix.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "pico_host.h"
#include "ff.h"
#include "drivers/input/keyboard.h"
#include "drivers/usbhost/UsbMsc.h"

// ---- linker-script symbols -------------------------------------------------------------
// The flash-region markers and overlay boundaries only need to be distinct addresses
// below 0x11000000; nothing on the host reads through them except the bank region,
// which is the writable "flash" area below.
extern "C" {
// A 4 MB writable "flash" image holding the GM.DLS bank region. Only the bank lives
// here; flash_range_program() reaches it as XIP_BASE + offset like on the chip.
__attribute__((aligned(4096)))
uint8_t host_flash_image[0x400000];

char __HeapLimit[1];
}
asm(".globl __flash_binary_start\n .set __flash_binary_start, host_flash_image\n"
    ".globl __flash_binary_end\n .set __flash_binary_end, host_flash_image + 0x200000\n"
    ".globl __psramrom_start\n .set __psramrom_start, host_flash_image + 0x200000\n"
    ".globl __psramrom_end\n .set __psramrom_end, host_flash_image + 0x200000\n"
    ".globl __gm_bank_start\n .set __gm_bank_start, host_flash_image + 0x200000\n"
    ".globl __gm_bank_end\n .set __gm_bank_end, host_flash_image + 0x400000\n");

// libc malloc wrap (pico_malloc wraps malloc/free on the firmware).
extern "C" void* __real_malloc(size_t n) { return malloc(n); }
extern "C" void __real_free(void* p) { free(p); }

// ---- src/main.cpp ----------------------------------------------------------------------
// The 8 MB QSPI ("butter") PSRAM of a Pico DV board, at the RP2350's own addresses:
// cached window 0x11000000 and its uncached alias +0x04000000 (MemESP::butter_nc,
// the Gigascreen scanline window). Both map the same memory.
static constexpr size_t HOST_BUTTER = 8u << 20;
uint8_t* PSRAM_DATA = (uint8_t*)0x11000000;
__attribute__((constructor)) static void host_butter_map() {
#ifdef _WIN32
    // Windows: the window is part of the image (the gap section of
    // host/win/pico-speccy.ld), one mapping without an alias (host_config.h).
    if (host_mem_rw((void*)0x11000000, HOST_BUTTER) != 0) {
        fprintf(stderr, "host: psram window at 0x11000000 is not usable\n");
        exit(1);
    }
#else
    const int fd = memfd_create("pico-speccy-psram", 0);
    if (fd < 0 || ftruncate(fd, HOST_BUTTER) != 0) { perror("host: psram"); exit(1); }
    for (uintptr_t a : { (uintptr_t)0x11000000, (uintptr_t)0x15000000 }) {
        void* p = mmap((void*)a, HOST_BUTTER, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (p != (void*)a) { perror("host: psram mmap"); exit(1); }
    }
#endif
}
uint16_t control_bits = 0;
uint8_t flash_qe = 0;
uint8_t flash_qe_diag[6];
uint8_t rx[4] = { 0xEF, 0x40, 0x16, 0x16 };   // JEDEC id: a 4 MB Winbond part
uint8_t psram_pin = 255;
char** host_argv = nullptr;

static bool s_butter_off = false;
uint32_t butter_psram_size() { return s_butter_off ? 0 : HOST_BUTTER; }
bool butter_psram_probed() { return true; }
void board_psram_disable() { s_butter_off = true; }
void board_set_clock_and_timing(unsigned) {}
const char* flash_qe_text() { return "n/a"; }
void flash_timings(int) {}
void flash_timings_transition(int, int) {}
uint8_t nes_pad2_for_alf() { return 0xFF; }


extern "C" {
void board_dbg_uart_apply(void) {}
void send_to_595(uint16_t) {}
unsigned board_kbd_clock_pin(void) { return 14; }
uint32_t uptime_seconds(void) { return (uint32_t)(time_us_64() / 1000000u); }
uint8_t linkVGA01 = 0;
}

// ---- SPI PSRAM: absent -----------------------------------------------------------------
extern "C" {
uint32_t init_psram() { return 0; }
uint32_t psram_size() { return 0; }
uint32_t psram_probed_size() { return 0; }
void psram_set_disabled(bool) {}
void psram_update_clkdiv() {}
void psram_cleanup() {}
void write8psram(uint32_t, uint8_t) {}
void write16psram(uint32_t, uint16_t) {}
void write32psram(uint32_t, uint32_t) {}
uint8_t read8psram(uint32_t) { return 0xFF; }
uint16_t read16psram(uint32_t) { return 0xFFFF; }
uint32_t read32psram(uint32_t) { return 0xFFFFFFFFu; }
void psram_id(uint8_t r[8]) { memset(r, 0, 8); }
void writepsram(uint32_t, uint8_t*, size_t) {}
void readpsram(uint8_t* b, uint32_t, size_t n) { memset(b, 0xFF, n); }
void psram_read_range(uint32_t, uint8_t* d, size_t n) { memset(d, 0xFF, n); }
void psram_write_range(uint32_t, const uint8_t*, size_t) {}
void psram_read_page(uint32_t, uint8_t* d) { memset(d, 0xFF, 16384); }
void psram_write_page(uint32_t, const uint8_t*) {}
void psram_write_page_async(uint32_t, const uint8_t*) {}
void psram_async_join(void) {}
}

// ---- USB host: nothing attached --------------------------------------------------------
namespace UsbMsc {
void armHotplug() {}
bool ready() { return false; }
Event takeEvent() { return Event::None; }
bool waitReady(uint32_t) { return false; }
}
extern "C" {
int hid_app_format_devices_info(char* buf, int sz) { if (sz > 0) buf[0] = 0; return 0; }
int xinput_app_format_devices_info(char* buf, int sz) { if (sz > 0) buf[0] = 0; return 0; }
void usb_kbd_resync_stats(unsigned* inst, unsigned* flags, unsigned* fixes,
                          unsigned* unproven, unsigned* stale, unsigned* demoted) {
    *inst = *flags = *fixes = *unproven = *stale = *demoted = 0;
}
bool usb_keyboard_mounted(void) { return true; }   // the SDL keyboard is always there
}

// The physical keyboard is read through the SDL front end (host_main.cpp), which
// feeds the emulator's VK queue directly; the PS/2 driver path stays empty.
bool fabgl::Keyboard::dataAvailable() { return false; }
bool fabgl::Keyboard::blockingGetVirtualKey(VirtualKeyItem*) { return false; }

// ---- SD card: background reads never armed --------------------------------------------
extern "C" {
volatile uint8_t g_sd_c1_live = 0;
volatile uint8_t g_sd_as_state = 0;
volatile uint8_t g_sd_as_need = 0;
volatile unsigned int g_sd_as_done = 0;
volatile uint32_t g_sd_as_seq = 0;
volatile uint32_t g_sd_as_c1_us = 0, g_sd_as_c1_max = 0, g_sd_as_c0wait_us = 0, g_sd_as_c0wait_max = 0;
volatile uint32_t g_disk_write_gen = 0;
void sd_async_abort(void) {}
int sdcard_alive(void) { return 1; }
void sdcard_led_set_active(const void*) {}
void sdcard_led_tag(const void*, const char*) {}
void sdcard_led_untag(const void*) {}
void sdcard_set_led_blink(int) {}
}
BYTE* g_sd_as_buf = nullptr;
LBA_t g_sd_as_sector = 0;
UINT g_sd_as_count = 0;
