// Force-included into every translation unit of the host build (host/CMakeLists.txt).
// Mirrors what the firmware's CMake passes for a PICO_DV VGA_HDMI image, with every
// trace, code overlay and RAM-placement switch off. HOST_BUILD is the one macro
// firmware sources may test for host-only behaviour.
#pragma once
#define HOST_BUILD 1

#define PORT_VERSION "1.1.0-host"
#define PORT_VERSION_LEN 10
#define CPU_MHZ 252
#define PICO_DV 1
#define VGA_HDMI 1
#define KBDUSB 1
#define FLASH_SIZE 4096
#define CONFIG_BOARD_TAG "host"
#define PICO_BUILD_NAME "pico-speccy-host"
#define PICO_BUILD_NAME_SHORT "host"
#define PICO_BUILD_VARIANT_TAG ""
#define PICO_GIT_BRANCH "host"
#define PICO_GIT_COMMIT "host"
#define PICOSPECCY_WIFI 1   // the PC network behind the on-chip interfaces (host_net.cpp)
#define PICOSPECCY_WIFI_WS 0
#ifndef ZIFI_NET_CLIENT
#define ZIFI_NET_CLIENT 0   // CMake sets 1 when the Pico SDK's mbedTLS is found
#endif

#define PICO_STACK_SIZE 0x2000
#define PICO_CORE1_STACK_SIZE 0x800

// PICO_DV pins (cmake/board-pins.cmake) — read by BoardPins and a few menus.
#define KBD_CLOCK_PIN 14
#define KBD_DATA_PIN 15
#define LOAD_WAV_PIO 20
#define DBG_UART_TX_PIN 20
#define CLK_AY_PIN1 21
#define LATCH_595_PIN 26
#define CLK_595_PIN 27
#define DATA_595_PIN 28
#define CLK_AY_PIN2 29
#define I2S_DATA_PIO 26
#define I2S_BCK_PIO 27
#define I2S_LCK_PIO 28
#define PWM_PIN0 26
#define PWM_PIN1 27
#define BEEPER_PIN 28
#define MIDI_TX_PIN 20
#define SDCARD_PIN_SPI0_MISO 19
#define SDCARD_PIN_SPI0_SCK 5
#define SDCARD_PIN_SPI0_MOSI 18
#define SDCARD_PIN_SPI0_CS 22
#define PSRAM_SPINLOCK 255
#define PSRAM_ASYNC 255
#define PSRAM_PIN_CS 255
#define PSRAM_PIN_SCK 255
#define PSRAM_PIN_MOSI 255
#define PSRAM_PIN_MISO 255
#define NES_GPIO_CLK 8
#define NES_GPIO_LAT 9
#define NES_GPIO_DATA 20
#define NES_GPIO_DATA2 21
#define VGA_BASE_PIN 6
#define HDMI_BASE_PIN 6
#define DEFAULT_THROTTLING 0
#define PICO_DEFAULT_LED_PIN 25
#define BUTTER_PSRAM_GPIO 47

// Memory-layout switches: no code overlays, nothing pinned to SRAM.
#define TSCONF_CODE_OVERLAY 0
#define GS_CODE_OVERLAY 0
#define VDAC2_CODE_OVERLAY 0
#define NEXT_CODE_OVERLAY 0
#define DMA_CODE_OVERLAY 0
#define WD1793_IN_RAM 0
#define DIVMMC_HOT_IN_RAM 0
#define TSCONF_HOT_IN_RAM 0
#define TSCONF_RENDER_IN_RAM 0
#define FT812_RENDER_IN_RAM 0
#define AY_STEREO_SLOT 0
#define TS_DRAM_MODEL 1
#define TS_INT_RING_N 512
#define ZX_PSRAM_PAGES 0x5F
#define NEXT_HOT_FRAMES 1
#define FB_FORCE_CHUNKS 0
#define PERF_TRACE 0
#define PERF_HIST 0

// cmake/board.cmake's image flags, as on a 4 MB image: MoonSound is on and reads its
// YRW801 ROM from the card, the GM.DLS bank comes from the card too.
#define MOONSOUND 1
#define YRW801_BUILTIN 0
#define GM_BANK_BUILTIN 0

// C11 keyword used by shared C headers that are also included from C++.
#ifdef __cplusplus
#define _Static_assert static_assert
#endif

#ifdef _WIN32
// Windows (mingw-w64): what glibc's headers give the firmware sources for free.
#include <stddef.h>
typedef unsigned int uint;
// The PSRAM window is one mapping here (an image section, host/win/pico-speccy.ld):
// its "uncached alias" is the same address.
#define PSRAM_NC_OFFSET 0u
// <malloc.h> mallinfo() and sbrk(), answered by host_winheap.c's dlmalloc.
#ifdef __cplusplus
extern "C" {
#endif
struct mallinfo {
    size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
struct mallinfo mallinfo(void);
void* sbrk(ptrdiff_t inc);
#ifdef __cplusplus
}
#endif
#endif
