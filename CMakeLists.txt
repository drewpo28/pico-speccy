# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")
cmake_minimum_required(VERSION 3.13)

include(cmake/options.cmake)
include(cmake/trace.cmake)

# ALF is always available (4MB flash) — small 256KB default cart + shared flash region.

set(CPU_MHZ 252)
set (PORT_VERSION "1.0.8")
string(LENGTH "${PORT_VERSION}" PORT_VERSION_LEN)

set(KBDUSB ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

SET(PROJECT_NAME speccy)
include(cmake/board.cmake)

include(pico_sdk_import.cmake)

set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

project(${PROJECT_NAME} C CXX ASM)

# Core0 stack lives at the top of SCRATCH_Y (.scratch_y is empty), and SCRATCH_X
# (core1/video stack) sits directly below it. The default 2 KB core0 stack is too
# tight for the OSD menu call chain (file browser → text-edit → file-info/create →
# std::string + format parsers): it overflowed into SCRATCH_X, corrupting core1's
# video stack — manifesting as SIGBUS (MSTKERR) crashes (F1 file info, F9 create
# TRD) and a garbled screen. Grow core0 to 4 KB (fills the otherwise-empty 4 KB
# SCRATCH_Y exactly); pin core1 at 2 KB so SCRATCH_X is unchanged.
#
# PICO_USE_STACK_GUARDS: hw-traced 2026-07-21 — even 4 KB overflowed again in
# usbRoot mode (Tape::LoadTape → FatFs → usb_disk_read → tuh_task pump), and the
# silent descent into SCRATCH_X trashed core1's live IRQ frames: core1 SIGBUS
# storms with garbage PC (values = core0 locals), then double-fault LOCKUP on
# both cores. The guard (MSPLIM on RP2350, on both cores)
# turns any future overflow into an immediate fault AT the boundary — the
# overflowing core traps with stackOvf=1 instead of corrupting its neighbour.
add_compile_definitions(PICO_STACK_SIZE=0x2000 PICO_CORE1_STACK_SIZE=0x800
                        PICO_TIME_DEFAULT_ALARM_POOL_MAX_TIMERS=8   # 2 repeating audio timers + sleep_ms; was 16 (384 B of .bss)
                        PICO_USE_STACK_GUARDS=1)

# ZiFi network client (FTP/SFTP/SSH) uses the SDK-vendored mbedTLS as a crypto
# primitive library. Point the SDK's mbedTLS at our trimmed config via an
# ABSOLUTE path BEFORE pico_sdk_init() — the pico_mbedtls CMakeLists reads
# PICO_MBEDTLS_CONFIG_FILE at SDK-init time and propagates it (as
# MBEDTLS_CONFIG_FILE) to both our TUs and the mbedTLS library's own sources.
# Absolute path means it resolves regardless of per-target include dirs. Harmless
# on builds that don't link pico_mbedtls (ZIFI_NET_CLIENT=OFF).
if (ZIFI_NET_CLIENT)
    set(PICO_MBEDTLS_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/src/drivers/board/mbedtls_config_picospeccy.h")
endif ()

# TinyUSB 0.21.0 vendored at external/tinyusb (subset: LICENSE, src/, hw/bsp/
# rp2040 + family_support — what the SDK's tinyusb glue consumes). Must be set
# BEFORE pico_sdk_init(): the SDK-bundled TinyUSB (0.18 in SDK 2.2.0) paces host
# bulk at ~1 × 64 B packet per 1 ms frame, capping USB MSC/CDC near ~64 KB/s;
# the 0.21 RP2 HCD rework lifts Speed Test to 0.89/0.75 MB/s (hw-confirmed).
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/external/tinyusb)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# PICOSPECCY_ZERO2_PIO_USB_HOST_V1: Pico-PIO-USB (cloned by the overlay's prepare script, pinned commit).
if(ZERO2 AND ZERO2_PIO_USB)
    set(PICO_PIO_USB_DIR "${CMAKE_CURRENT_LIST_DIR}/external/Pico-PIO-USB")
    if(NOT EXISTS "${PICO_PIO_USB_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Pico-PIO-USB dependency is missing at ${PICO_PIO_USB_DIR}. "
            "Run the supplied prepare/build script.")
    endif()
    add_subdirectory("${PICO_PIO_USB_DIR}" "${CMAKE_BINARY_DIR}/pico-pio-usb")
endif()

# Pico hardware drivers: each is its own CMake library under src/drivers/<name>/.
# They are linked, not globbed into the firmware target (see the filter below).
set(PICO_DRIVER_LIBS audio graphics hdmi nespad ps2 ps2kbd psram sdcard st7789 tv
                     tv-software vga-nextgen)
add_subdirectory(src/drivers/audio)
add_subdirectory(src/drivers/nespad)
add_subdirectory(src/drivers/sdcard)
add_subdirectory(src/drivers/vga-nextgen)
add_subdirectory(src/drivers/st7789)
add_subdirectory(src/drivers/hdmi)
add_subdirectory(src/drivers/tv)
add_subdirectory(src/drivers/tv-software)
add_subdirectory(src/drivers/graphics)
add_subdirectory(src/drivers/psram)
# Vendored third-party libraries.
add_subdirectory(external/picomp3lib)
add_subdirectory(external/fatfs)

#target_compile_definitions(${PROJECT_NAME} PRIVATE VGA)

# INCLUDE FILES THAT SHOULD BE COMPILED:
# CONFIGURE_DEPENDS: the generator re-runs the glob on every build, so a new file
# (e.g. under src/ui/) is picked up by a plain `ninja -C build` without a manual
# re-configure. Costs one directory stat per build — negligible next to the link.
file(GLOB_RECURSE SRC CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.c"
    # Vendored code compiled straight into the firmware (flags: cmake/source-flags.cmake).
    "external/miniz/*.c"
    "external/redcode/*.c"
    "external/libxmp/*.c"
    "external/tjpgd/*.c"
)
# The driver libraries under src/drivers/ are separate CMake targets.
string(REPLACE ";" "|" _drv_re "${PICO_DRIVER_LIBS}")
list(FILTER SRC EXCLUDE REGEX "/src/drivers/(${_drv_re})/")

message(STATUS "Add source files:")
foreach (SRC_FILE IN LISTS SRC)
    message(STATUS "${SRC_FILE}")
endforeach ()
message(STATUS "")

add_executable(${PROJECT_NAME} ${SRC})

# PICOSPECCY_ZERO2_PIO_USB_HOST_V1: swap TinyUSB's native RP2 HCD for the PIO one. Root hub 1 == the PIO port
# (hcd_pio_usb.c maps rhport N -> PIO root N-1); the native controller has no HCD in
# this build at all. Pico-PIO-USB needs a WHOLE PIO (3 SMs, 32 instructions) plus one
# DMA channel, which main.cpp picks at runtime; it takes pio2, and ZERO2_PIO_USB_HOST
# is therefore also what pushes HDMI back to pio0 (see PIO_VIDEO in src/drivers/hdmi/hdmi.h
# — with both on pio2 the firmware rebooted forever on a core1 PIO panic). pio1 is the
# PS/2 keyboard + I2S/WAV either way.
if(ZERO2 AND ZERO2_PIO_USB)
    # Both HCDs live in the image: src/drivers/usbhost/usb_hcd_native_wrap.c and src/drivers/usbhost/usb_hcd_pio_wrap.c
    # (picked up by the src/*.c glob) compile hcd_rp2040.c / hcd_pio_usb.c with their
    # hcd_* entry points renamed, and src/drivers/usbhost/usb_hcd_router.c dispatches by rhport. Do NOT
    # add hcd_pio_usb.c here as well — the wrapper is its only translation unit.
    target_link_libraries(${PROJECT_NAME} PRIVATE pico_pio_usb)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        ZERO2_PIO_USB_HOST=1
        ZERO2_PIO_USB_PIO_NUM=2
        PICO_DEFAULT_PIO_USB_DP_PIN=28   # D- is GP29 (PIO_USB_PINOUT_DPDM)
    )
endif()

include(cmake/source-flags.cmake)

# pull in common dependencies
target_link_libraries(${PROJECT_NAME} PRIVATE
    psram
    audio
    graphics
    picomp3lib

    nespad
    sdcard
    fatfs

    pico_runtime
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_i2c
    hardware_interp
    hardware_timer
    hardware_clocks
    hardware_pwm
    hardware_uart
    hardware_flash
    hardware_xip_cache
    hardware_exception
    hardware_adc
    stdc++

    tinyusb_host tinyusb_board
)

# Project includes are spelled from src/ ("speccy/core/Ports.h") or from external/
# ("miniz/miniz.h"); a header in the same directory as its includer is spelled bare.
# src/drivers/board holds the config headers the SDK looks up by bare name
# (tusb_config.h, lwipopts.h, FreeRTOSConfig.h).
target_include_directories(${PROJECT_NAME} PRIVATE
        src
        external
        src/drivers/board
        external/embeded-midi-synth
)

include(cmake/wifi.cmake)

# create map/bin/hex file etc.
pico_add_extra_outputs(${PROJECT_NAME})
# No SDK stdio drivers: the Debug > UART console registers its OWN stdio_driver_t
# (Debug.cpp) that feeds the non-blocking TX ring, and it must never bind to
# PICO_DEFAULT_UART (GP0/1 in the pico2 board header = the ZiFi UART on PICO_DV).
pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

# add url via pico_set_program_url

include(cmake/memory-layout.cmake)

target_compile_definitions(${PROJECT_NAME} PRIVATE CONFIG_BOARD_TAG="${CONFIG_BOARD_TAG}")

include(cmake/debug-defines.cmake)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
target_compile_options(${PROJECT_NAME} PUBLIC
        -Wno-error=suggest-attribute=format
        -Wno-error=cast-qual
        -Wno-error=unused-parameter
        -Wno-error=conversion
        -Wno-error=format=
        -Wno-error=sign-compare
        -Wno-error=missing-field-initializers
        -Wno-error=switch
        -Wno-error=implicit-fallthrough=
        -Wno-error=stringop-truncation
        -Wno-error=restrict
        -w
)
endif ()

# ALF is now a standard runtime machine (256KB default cart "Elf-1" in flash; bigger
# cartridges load into the shared flash region from SD/Web). CARTRIDGE_AS_CUSTOM is
# gone — custom ROMs use their own dedicated slots.
# ALF no longer carries a 1.6MB cartridge in the image, so the GM.DLS gm_bank
# partition stays available alongside ALF (they share the region at
# runtime — one occupant at a time; see SharedFlash/RomManager).

IF(KBDUSB)
    add_subdirectory(src/drivers/ps2kbd)
    target_link_libraries(${PROJECT_NAME} PRIVATE ps2kbd)
    target_compile_definitions(${PROJECT_NAME} PRIVATE KBDUSB)
ELSE()
    add_subdirectory(src/drivers/ps2)
    target_link_libraries(${PROJECT_NAME} PRIVATE ps2)
    target_compile_definitions(${PROJECT_NAME} PRIVATE KBDPS2)
ENDIF()

if (BUTTER_PSRAM_GPIO)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BUTTER_PSRAM_GPIO=${BUTTER_PSRAM_GPIO})
endif()
# ZiFi (ESP-01S WiFi NIC emulation).
# TX/RX pins are chosen at RUNTIME (Network → GPIO picker, stored in NVS) from a
# per-board candidate list defined in src/drivers/board/BoardPins.cpp. No compile-time pin
# define is needed anymore; the old -DZIFI_TX_PIN/-DZIFI_RX_PIN are obsolete.

# ZiFi network client (FTP/SFTP/SSH over the ESP-01S TCP sockets).
# SSH crypto runs on the Pico via mbedTLS primitives (NOT wolfSSH, NOT mbedTLS's
# TLS stack; see src/drivers/board/mbedtls_config_picospeccy.h). New sources (ZiFiSock/Ftp/Ssh/
# Sftp.cpp) auto-compile via the GLOB; each file body is wrapped in
# `#if ZIFI_NET_CLIENT` so ZIFI_NET_CLIENT=OFF builds get zero footprint.
if (ZIFI_NET_CLIENT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZIFI_NET_CLIENT=1)
    if (ZIFI_NET_VERBOSE)
        target_compile_definitions(${PROJECT_NAME} PRIVATE ZIFI_NET_VERBOSE=1)
    endif ()
    # MBEDTLS_CONFIG_FILE is supplied by the SDK from PICO_MBEDTLS_CONFIG_FILE
    # (set before pico_sdk_init above) — do not redefine it here, that collides
    # with pico_mbedtls_headers' INTERFACE define.
    target_link_libraries(${PROJECT_NAME} PRIVATE
        pico_mbedtls          # crypto primitives from pico-sdk's vendored mbedTLS
        pico_rand)            # hardware RNG for SSH ephemeral keys / padding
    message(STATUS "ZiFi network client (FTP/SFTP/SSH) enabled with mbedTLS crypto")
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZIFI_NET_CLIENT=0)
endif ()

include(cmake/board-pins.cmake)

include(cmake/display.cmake)

include(cmake/build-name.cmake)
