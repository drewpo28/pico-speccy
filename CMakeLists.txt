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

option(VGA_HDMI "Enable VGA/HDMI driver" OFF)
option(TFT "Enable TFT display" OFF)
option(ILI9341 "Enable TFT ILI9341 display" OFF)
option(TV "Enable TV composite output" OFF)
option(SOFTTV "Enable TV soft composite output" OFF)
option(MURM "Murmulator 1.x + Pi Pico 2" OFF)
option(MURM2 "Murmulator 2.0 + Pi Pico 2" OFF)
option(PICO_PC "Olimex RP2040-PICO-PC + Pi Pico 2" OFF)
option(PICO_DV "Pimoroni Pico DV Demo Base + Pi Pico 2" OFF)
option(ZERO2 "Waveshare RP2350-PiZero" OFF)
# Waveshare RP2350B-Plus-W in a Murmulator socket. The module is an RP2350B with
# 16 MB flash, PSRAM pads on GPIO47 and a Raspberry Pi Radio Module 2 (CYW43439)
# on board, so these are the SAME carriers as MURM/MURM2 with a different module:
# each option turns its carrier on and then overrides the handful of pins that the
# module moves. See src/boards/picospeccy_rp2350b_w.h.
option(MURM_W "Murmulator 1.x + Waveshare RP2350B-Plus-W (on-chip CYW43439 WiFi)" OFF)
option(MURM2_W "Murmulator 2.0 + Waveshare RP2350B-Plus-W (on-chip CYW43439 WiFi)" OFF)
# OFF by default and shipped as a SEPARATE z0p2 firmware ("-PIOUSB" in the file name):
# Pico-PIO-USB costs 17.7 KB of SRAM that cannot be reclaimed at runtime — 11.0 KB of
# .time_critical code (the bit-banged host has to run from RAM) plus 6.7 KB of .bss,
# mostly its 32-entry endpoint pool — which is nearly the whole 20 KB by which z0p2's
# free heap trailed DVp2's (measured from the map files, 2026-08-13). Boards that do
# not use the second Type-C as a host should not pay it, so the default build does not.
option(ZERO2_PIO_USB "ZERO2: add a USB host on the second Type-C (J2, PIO-USB GP28/GP29) — costs ~18 KB SRAM" OFF)
option(ZIFI_NET_CLIENT "Enable FTP/SFTP/SSH client over ZiFi (uses mbedTLS)" ON)

# HDMI signal-integrity knobs (drivers/hdmi/hdmi.c). Both ON is the combination
# hw-confirmed on a capture card (2026-08-06); turning either off cannot break the
# stream, so a marginal link can be A/B'd by rebuild. A third knob, a ±2-code level
# snap, was tried and removed — it broke the new UI's paint on hardware for reasons
# that were never explained; see the "WHAT WAS TRIED AND REMOVED" note in hdmi.c
# before considering it again.
option(HDMI_TMDS_LEVEL_CLAMP "HDMI: keep channel levels inside [0x08..0xF6] (costs ~3% black, ~4% white)" ON)
option(HDMI_SOFT_CLK "HDMI: drive the clock pair at 8 mA + slow slew instead of 12 mA + fast" ON)

# Per-subsystem port-access tracing toggles (debugging). OFF by default — when
# ON, gated `#if XXX_TRACE` Debug::log() calls in the sources become active.
# Add new toggles here following the same option(...) + compile-definition pattern.
option(IDE_PORT_TRACE "Enable PROFI IDE/HDD port access tracing" OFF)
option(FDD_PORT_TRACE "Enable WD1793 (ВГ93) FDD command/port tracing" OFF)
option(RTC_PORT_TRACE "Enable MC146818 Mr Gluk RTC port access tracing" OFF)
option(PROFI_PORT_TRACE "Enable 0x7FFD/0xDFFD paging-port write tracing" OFF)
option(MC7FFD_TRACE "One-shot capture of #7FFD write tstates when a frame carries >=90 of them (beam-raced multicolor phase diagnosis)" OFF)
option(SCORP_FF_TRACE "Trace Scorpion IN/OUT on port #FF (ТЕСТ SCORPION port-FF diagnostic), capped" OFF)
option(VDISK_TRACE "Trace the SMUC virtual-FDD bridge: #7FBA select + WD1793 RDSEC/WRSEC + HDD LBA, correlated, low-noise" OFF)
option(SMUC_TRACE "Trace SMUC port accesses, including ones the DOSEN/SYSEN gate rejected (capped)" OFF)
option(GMX_TRACE "Trace Scorpion GMX paging: port #00 magic/reset, 7EFD plane, 1FFD writes, romInUse transitions, TR-DOS trap entry/exit (capped)" OFF)
option(TSFM_TRACE "1 Hz TSFM budget probe: status-read/write counts per frame + how late in the frame the FM player runs" OFF)
option(PAGE_TRACE "Trace ROM selection: every 0x7FFD write, TR-DOS map/unmap, and an alarm when an IM1 interrupt enters the 128K ROM with a dead 0x5B00 stub" OFF)
option(SND_PORT_TRACE "Enable per-port I/O histogram (sound-DAC port hunting)" OFF)
option(PERF_TRACE "Enable per-60-frame CPU/HDMI/FPS performance logging" OFF)
option(NEG2_TRACE "Enable Profi negative-IDL attribution ([NEG2] lines)" OFF)
option(MEM_ACCESS_TRACE "Count Z80 accesses per RAM page between load and eviction (accessor-mode feasibility study; slows readbyte/writebyte)" OFF)
option(GS_PERF_TRACE "Enable General Sound per-second perf counters (GS.cpp)" OFF)
option(HDMI_AUDIO_TRACE "Enable the 1 Hz HDMIAU: audio-health log line (hdmi_audio_health_dump)" OFF)
option(OPL_PERF_TRACE "Enable the ~6 s 'OPL: gen us/fr' YMF262 cost line (ESPectrum.cpp)" OFF)
option(GS_DEBUG_TRACE "Enable General Sound port-IO trace ring + auto-dumps (~50 KB SRAM)" OFF)
option(NGS_TRACE "Enable NeoGS 1 Hz health line (GS-Z80 PC / SD counters; ~2-4 ms UART stall per line)" OFF)
option(NGS_SD_TRACE "Also log every guest SD command (NGS_TRACE level 2; thousands of lines per directory scan)" OFF)
option(ZC_PORT_TRACE "Enable Z-Controller/DivSD SPI-SD command tracing (CS edges + command frames)" OFF)
option(NEO8_TRAP "Wild-jump hunter: log PC history + frame snapshot when execution enters screen memory (per-instruction hook; debug builds only)" OFF)
option(ZIFI_TRACE "Enable ZiFi (ESP-01S WiFi NIC) port/UART tracing" OFF)
option(ZIFI_NET_VERBOSE "Per-packet trace of the ZiFi net client (+IPD/chanSend); floods logs" OFF)

# Per-board Debug-Probe UART toggles (logging over UART). Declared here together;
# the board-specific `if(<board>_DBG_UART)` blocks below consume them (they call
# pico_enable_stdio_uart() + remap pins, which needs the target to exist first).
# Each enables stdio over the board's Debug-Probe header — see the block for pins.
set(MURM1_DBG_UART   OFF CACHE BOOL "Enable Debug Probe UART0 on GP0/GP1 (MURM1) — moves KBD to GP16/GP17, disables NESPAD")
set(PICO_PC_DBG_UART OFF CACHE BOOL "Enable Debug Probe UART0 on DBG1 header GP0/GP1 (PICO_PC)")
set(PICO_DV_DBG_UART OFF CACHE BOOL "Enable Debug Probe UART1 on GP20/GP21 (PICO_DV)")
set(ZERO2_DBG_UART   OFF CACHE BOOL "Enable Debug Probe UART1 on GP20/GP21 (ZERO2)")

# set(TFT ON)
# set(TFT_INV ON)
# set(ILI9341 ON)
# set(ST7789 ON)
# set(SOFTTV ON)

# Display output (default: VGA_HDMI; override with -D flags, e.g. cmake -DSOFTTV=ON)
if(NOT VGA_HDMI AND NOT SOFTTV AND NOT TFT AND NOT ILI9341 AND NOT TV)
    set(VGA_HDMI ON)
endif()
# ALF is always available (4MB flash) — small 256KB default cart + shared flash region.

set(CPU_MHZ 378)
set (PORT_VERSION "1.0.4")
string(LENGTH "${PORT_VERSION}" PORT_VERSION_LEN)

# A W target IS its carrier, plus a different plugged-in module: turn the carrier
# on so every pin arm, display matrix and runtime `#if MURM2` below keeps working,
# and raise PICOSPECCY_WIFI as the single "this image has a radio" switch. Must run
# BEFORE the default-board guard, or -DMURM_W=ON alone would also default MURM2 on.
if(MURM_W)
    set(MURM ON)
endif()
if(MURM2_W)
    set(MURM2 ON)
endif()
if(MURM_W OR MURM2_W)
    set(PICOSPECCY_WIFI ON)
    # The radio is only reachable from a PIO block at gpio_base 16 (its pins are
    # above GPIO31), and pio1/pio2 are pinned to base 0 by the keyboard and HDMI —
    # so it has to be pio0. Every non-HDMI display driver ALSO wants pio0, at base
    # 0: VGA (PIO_VGA, pins from VGA_BASE_PIN 6/12), SOFTTV (PIO_VIDEO = pio0) and
    # ST7789/TFT. Those cannot coexist with the radio in the same image, and the
    # failure would be silent — cyw43_arch_init() simply never finds a block, and
    # you get a WiFi board with no WiFi. Refuse the combination at configure time.
    if(SOFTTV OR TV OR TFT OR ILI9341 OR ST7789)
        message(FATAL_ERROR
            "MURM_W/MURM2_W support HDMI output only: the CYW43 radio needs pio0 at "
            "gpio_base 16, and the SOFTTV/TV/TFT drivers take pio0 at gpio_base 0. "
            "Build these boards with -DVGA_HDMI=ON (the default).")
    endif()
endif()

# Target board (default: MURM2; override with -D flags, e.g. cmake -DPICO_DV=ON)
if(NOT MURM AND NOT MURM2 AND NOT PICO_PC AND NOT PICO_DV AND NOT ZERO2)
    set(MURM2 ON)
endif()

set(KBDUSB ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pull in Raspberry Pi Pico SDK (must be before project).
# RP2350 only — RP2040 support was dropped, every board below is an RP2350 target.
IF(ZERO2)
    # For Board RP2350-PiZero
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/src/boards)
    set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM rp2350-arm-s)
    set(CPU_MHZ 252)
ELSEIF(PICOSPECCY_WIFI)
    # Waveshare RP2350B-Plus-W: same RP2350B package as the header below, plus the
    # on-board CYW43439, 16 MB of flash and PSRAM pads on GPIO47.
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/src/boards)
    set(PICO_BOARD picospeccy_rp2350b_w CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM rp2350-arm-s)
else()
    # All remaining boards (MURM2, PICO_PC, PICO_DV, MURM1/default) build for the
    # RP2350B (48-GPIO) package so GPIO 30..47 are valid for any peripheral.
    # pico2 forces PICO_RP2350A=1, so we use a local B-variant header
    # (a CMake `set(PICO_RP2350A 0)` is ignored by the SDK — it must be the header).
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/src/boards)
    set(PICO_BOARD picospeccy_rp2350b CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM rp2350-arm-s)
endif()

message(STATUS "PICO_BOARD: ${PICO_BOARD}")

include(pico_sdk_import.cmake)

SET(PROJECT_NAME speccy)
# 16 MB on the RP2350B-Plus-W module, 4 MB everywhere else. This is what buys room
# for the ~230 KB CYW43 firmware blob: with __gmx_rom_in_flash the gm_bank partition
# takes 1.6875 MB off the top (rp2350-memmap.ld), leaving ~2.3 MB of firmware
# partition on 4 MB — which main has already overflowed once — against ~14.3 MB here.
if(PICOSPECCY_WIFI)
    set(FLASH_SIZE 16384)
else()
    set(FLASH_SIZE 4096)
endif()

# Board tag: prefix of the firmware file name and the per-board NVS config dir.
# No chip suffix anymore — every target is RP2350.
IF(MURM2_W)
    set(BOARD_TAG "m2p2w")
    set (BUTTER_PSRAM_GPIO 47)      # module pads; 0 until a chip is soldered on
ELSEIF(MURM_W)
    set(BOARD_TAG "m1p2w")
    set (BUTTER_PSRAM_GPIO 47)
ELSEIF(MURM2)
    set(BOARD_TAG "m2p2")
    set (BUTTER_PSRAM_GPIO 8)
ELSEIF(PICO_PC)
    set(BOARD_TAG "PCp2")
    set (BUTTER_PSRAM_GPIO 8)
ELSEIF(PICO_DV)
    set(BOARD_TAG "DVp2")
    set (BUTTER_PSRAM_GPIO 47)
ELSEIF(ZERO2)
    set(BOARD_TAG "z0p2")
    set (BUTTER_PSRAM_GPIO 47)
ELSE()
    set(BOARD_TAG "m1p2")
    set (BUTTER_PSRAM_GPIO 19)
ENDIF()
SET(BUILD_NAME "${BOARD_TAG}-${PROJECT_NAME}")
set(CONFIG_BOARD_TAG "${BOARD_TAG}")

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
                        PICO_USE_STACK_GUARDS=1)

# ZiFi network client (FTP/SFTP/SSH) uses the SDK-vendored mbedTLS as a crypto
# primitive library. Point the SDK's mbedTLS at our trimmed config via an
# ABSOLUTE path BEFORE pico_sdk_init() — the pico_mbedtls CMakeLists reads
# PICO_MBEDTLS_CONFIG_FILE at SDK-init time and propagates it (as
# MBEDTLS_CONFIG_FILE) to both our TUs and the mbedTLS library's own sources.
# Absolute path means it resolves regardless of per-target include dirs. Harmless
# on builds that don't link pico_mbedtls (ZIFI_NET_CLIENT=OFF).
if (ZIFI_NET_CLIENT)
    set(PICO_MBEDTLS_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/src/mbedtls_config_picospeccy.h")
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

add_subdirectory(drivers/audio)
add_subdirectory(drivers/picomp3lib)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/vga-nextgen)
add_subdirectory(drivers/st7789)
add_subdirectory(drivers/hdmi)
add_subdirectory(drivers/tv)
add_subdirectory(drivers/tv-software)
add_subdirectory(drivers/graphics)
add_subdirectory(drivers/psram)

#target_compile_definitions(${PROJECT_NAME} PRIVATE VGA)

# INCLUDE FILES THAT SHOULD BE COMPILED:
# CONFIGURE_DEPENDS: the generator re-runs the glob on every build, so a new file
# (e.g. under src/ui/) is picked up by a plain `ninja -C build` without a manual
# re-configure. Costs one directory stat per build — negligible next to the link.
file(GLOB_RECURSE SRC CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.c"
    "fabgl/*.cpp"
    "src/roms/*.cpp"
    "src/roms/*.c"
    "src/miniz/*.c"
    "src/GS/*.cpp"
    "src/GS/*.c"
)

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
# is therefore also what pushes HDMI back to pio0 (see PIO_VIDEO in drivers/hdmi/hdmi.h
# — with both on pio2 the firmware rebooted forever on a core1 PIO panic). pio1 is the
# PS/2 keyboard + I2S/WAV either way.
if(ZERO2 AND ZERO2_PIO_USB)
    # Both HCDs live in the image: src/usb_hcd_native_wrap.c and src/usb_hcd_pio_wrap.c
    # (picked up by the src/*.c glob) compile hcd_rp2040.c / hcd_pio_usb.c with their
    # hcd_* entry points renamed, and src/usb_hcd_router.c dispatches by rhport. Do NOT
    # add hcd_pio_usb.c here as well — the wrapper is its only translation unit.
    target_link_libraries(${PROJECT_NAME} PRIVATE pico_pio_usb)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        ZERO2_PIO_USB_HOST=1
        ZERO2_PIO_USB_PIO_NUM=2
        PICO_DEFAULT_PIO_USB_DP_PIN=28   # D- is GP29 (PIO_USB_PINOUT_DPDM)
    )
endif()

# КРИТИЧНАЯ ОПТИМИЗАЦИЯ: принудительно -O3 для горячего кода независимо от CMAKE_BUILD_TYPE
# Это важно для MinSizeRel (где по умолчанию -Os), чтобы не терять FPS
set_source_files_properties(src/AySound.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops")
set_source_files_properties(src/SAASound.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops")
# YM2203 FM (TurboSound FM): 24 operators at the AY sample rate is the whole cost.
# -O2, not -O3 -funroll-loops, for all three FM cores: -O3+unroll doubles the
# hot code (17.3->8.1 KB at -O2, measured 2026-09-02) for ~14% worst-case speed
# (host bench 226->258 ns/sample all-channels-loud; output bit-identical). The
# code is flash-resident since 2026-09-06 (was __not_in_flash "audio"), so the
# size now costs flash, not SRAM. EG-skip and half-rate are the levers that
# made dense scores affordable, not -O3.
set_source_files_properties(src/OpnFm.cpp PROPERTIES
    COMPILE_FLAGS "-O2")
# YMF262 (OPL3, VGM-player card): 36 operators at the AY sample rate.
set_source_files_properties(src/OplFm.cpp PROPERTIES
    COMPILE_FLAGS "-O2")
# 2x SN76489 (VGM-player card): ~7 chip ticks per output sample.
set_source_files_properties(src/SnSound.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops")
# YM2413 (OPLL, VGM-player card): 18 operators at the AY sample rate.
set_source_files_properties(src/OpllFm.cpp PROPERTIES
    COMPILE_FLAGS "-O2")
set_source_files_properties(src/MidiSynth.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops")
# Wavetable MIDI engine wrapper (external/embeded-midi-synth) — hot audio path.
set_source_files_properties(src/midi_wt.c PROPERTIES
    COMPILE_FLAGS "-O3 -ffast-math -funroll-loops")
set_source_files_properties(src/Video.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops")
# Z80 эмулятор — самый горячий код, критично для производительности
set_source_files_properties(src/Z80_JLS.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops")
set_source_files_properties(src/CPU.cpp PROPERTIES
    COMPILE_FLAGS "-O3")
set_source_files_properties(src/Ports.cpp PROPERTIES
    COMPILE_FLAGS "-O3")
set_source_files_properties(src/wd1793.cpp PROPERTIES
    COMPILE_FLAGS "-O3")
set_source_files_properties(src/ESPectrum.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops")
set_source_files_properties(src/pwm_audio.cpp PROPERTIES
    COMPILE_FLAGS "-O3")

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

target_include_directories(${PROJECT_NAME} PRIVATE
        src
        src/roms
        external/embeded-midi-synth
)

# On-chip WiFi (MURM_W / MURM2_W: Raspberry Pi Radio Module 2 = CYW43439).
# This replaces a hook that had been dormant since it was written: it keyed off
# PICO_CYW43_SUPPORTED, which no board header here defined, and it used the
# keyword-less target_link_libraries() signature that CMake refuses to mix with
# the PRIVATE form used everywhere else on this target — so it would have failed
# the configure the first time it ever fired.
#
# Phase 1 links the bus driver only (pico_cyw43_arch_none): that brings up the
# radio and its PIO/DMA claims for ~0 heap, with no lwIP and no ~30 KB working
# set, which is what makes the pin/gpio_base bring-up cheap to test. lwIP arrives
# with WifiNet/WifiSock (pico_cyw43_arch_lwip_poll + pico_lwip_dns/sntp).
if (PICOSPECCY_WIFI)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PICOSPECCY_WIFI=1
        # The SDK's fixed divider assumes 150 MHz; WifiNet::init() computes one
        # for the real clk_sys (378/504 MHz) — see the gSPI note there.
        CYW43_PIO_CLOCK_DIV_DYNAMIC=1
        CYW43_HOST_NAME="pico-speccy")
    # lwIP in POLL mode (NO_SYS, src/lwipopts.h): every stack callback runs inside
    # cyw43_arch_poll() from core0 — WifiNet::poll() once per frame plus the waits in
    # WifiNet/WifiSock — so the emulator, not a background IRQ context, decides when
    # the network runs. All lwIP memory is heap/net-arena via Buffer::palloc (see
    # lwipopts.h); the static footprint is a few hundred bytes.
    target_link_libraries(${PROJECT_NAME} PRIVATE pico_cyw43_arch_lwip_poll)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PICOSPECCY_WIFI=0)
endif()
# Board identity for the sources. MURM2 already defines MURM2=1 in its pin arm;
# MURM1 defines nothing and is the `#else` fallback everywhere, so MURM_W needs
# its own macro for src/BoardPins.cpp to tell the two Murmulator-1 modules apart.
if (MURM_W)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MURM_W=1)
endif()
if (MURM2_W)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MURM2_W=1)
endif()

# PIO block assignment on the W boards. An RP2350 PIO reaches 32 CONSECUTIVE
# GPIOs from its gpio_base (0 or 16), and the radio sits above GPIO31 — so CYW43
# needs a block at base 16, which pio1 (keyboard on GP0-3) and pio2 (HDMI on
# GP6-19) cannot be. That leaves pio0, and whatever else has moved above GPIO31
# has to join it there:
#
#   MURM_W  — audio is on GPIO40/41/42, so I2S moves pio1 -> pio0.
#             pio0: CYW43 (5-7) + I2S (9, or 17 for CS4334)   = 14..24 / 32
#             pio1: PS/2 (7) + NESPAD (7)                     = 14 / 32
#   MURM2_W — audio stays on GP9/10/11 (below GP23, so unmoved), but the pad's
#             data pair is on GPIO40/41, so NESPAD moves pio1 -> pio0.
#             pio0: CYW43 (5-7) + NESPAD (7)                  = 12..14 / 32
#             pio1: PS/2 (7) + I2S (9, or 17)                 = 16..24 / 32
#
# pio2 keeps HDMI (18 / 32) on both. cyw43_arch_init() must therefore run AFTER
# graphics_init() and the keyboard have fixed their blocks at base 0, or the SDK's
# pio_claim_free_sm_and_add_program_for_gpio_range() can take the wrong one.
if (MURM_W)
    target_compile_definitions(${PROJECT_NAME} PRIVATE I2S_PIO=pio0)
endif()
if (MURM2_W)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NESPAD_PIO=pio0)
endif()

# create map/bin/hex file etc.
pico_add_extra_outputs(${PROJECT_NAME})

# add url via pico_set_program_url

# Custom linker script reserves a fixed 128K region (RAM_PAGES01234567) for
# the 8 Z80 RAM pages backing pages 0-7 (m16c_pages: pages0123/pages46/57).
# All other code/data and the heap (which holds the framebuffer) live in a
# separate RAM region. The two regions sit in different SRAM banks with
# independent AHB ports — HDMI DMA reading the framebuffer cannot stall
# CPU access to Z80 RAM and vice versa.
pico_set_linker_script(${PROJECT_NAME} "${CMAKE_SOURCE_DIR}/rp2350-memmap.ld")

target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections)
target_compile_definitions(${PROJECT_NAME} PRIVATE FLASH_SIZE=${FLASH_SIZE})

# Scorpion GMX: the boot ROM (~345 KB after dedup/overlays) is embedded in flash
# on EVERY board — QSPI(butter) PSRAM is a property of the plugged-in Pico 2
# MODULE, not of the carrier board (a Murmulator 1 takes a module with PSRAM on
# XIP CS1 = GPIO19 just fine), so the gate is the RUNTIME butter probe: without
# a chip the GMX pick falls back to Yellow PCB (Config::requestMachine +
# resolveConstraints). The linker shrinks the GM.DLS partition to 1.6875 MB
# (rp2350-memmap.ld, __gmx_rom_in_flash) so firmware + ROM fit below it — the
# stock converted gm.dls bank (~1.59 MB) still fits with ~100 KB margin; a
# previously provisioned bank at the old address fails the header check and
# re-provisions from SD. GMX_IN_FLASH=0 remains an escape hatch (drops the ROM,
# the menu rows and the partition shrink together).
target_compile_definitions(${PROJECT_NAME} PRIVATE GMX_IN_FLASH=1)
# Scorpion PROF-ROM v4.01 (romset "ZS-1024 + ProfROM"): ~181 KB of flash for the
# 256 KB image (tools/rom_pack.py prof). It rides the GM.DLS partition shrink the
# GMX ROM above already asks for, so PROFROM_IN_FLASH=1 needs no defsym of its
# own — but it does assume GMX_IN_FLASH=1; turning GMX off while leaving this on
# would put firmware + ROM back above the old partition base.
target_compile_definitions(${PROJECT_NAME} PRIVATE PROFROM_IN_FLASH=1)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--defsym=__gmx_rom_in_flash=1)
target_compile_definitions(${PROJECT_NAME} PRIVATE CONFIG_BOARD_TAG="${CONFIG_BOARD_TAG}")

# Scorpion port #FF trace (see Ports.cpp `#if SCORP_FF_TRACE`): logs every IN/OUT
# on low byte #FF with pc/trdos/sysen/border — for the "ТЕСТ SCORPION" port-FF
# diagnostic. Capped, default OFF.
if (SCORP_FF_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SCORP_FF_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE SCORP_FF_TRACE=0)
endif ()

# SMUC virtual-FDD bridge tracing (see `#if VDISK_TRACE` in Ports/IDE/wd1793):
# correlates the #7FBA virtual-drive select, the WD1793 RDSEC/WRSEC track/sector,
# and the HDD LBA in ONE low-noise log — the three facts needed to derive the
# track/sector -> LBA mapping. Leave FDD_PORT_TRACE off with this (its per-write
# [FDC SYS] flood is what garbles the correlation).
if (VDISK_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VDISK_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE VDISK_TRACE=0)
endif ()

# SMUC port tracing (see Ports.cpp `#if SMUC_TRACE`): every accepted access plus,
# crucially, every access the DOSEN/SYSEN gate REJECTED — the two are
# indistinguishable in a log otherwise.
if (SMUC_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SMUC_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE SMUC_TRACE=0)
endif ()

# IDE/HDD port tracing toggle (see Ports.cpp / IDE.cpp `#if IDE_PORT_TRACE`).
if (IDE_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IDE_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE IDE_PORT_TRACE=0)
endif ()

# WD1793 FDD command tracing toggle (see wd1793.cpp `#if FDD_PORT_TRACE`).
if (FDD_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FDD_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE FDD_PORT_TRACE=0)
endif ()

# ROM-selection tracing (Ports.cpp `#if PAGE_TRACE`, Z80_JLS.cpp check_trdos +
# interrupt). Answers "which ROM was mapped when the guest took its interrupt"
# — see the ExTracker 3.07 crash: the tracker EI/HALTs during its GS detect and
# needs ROM 1 (48 BASIC) at 0x0038, because it has already overwritten the 128K
# ROM's own 0x5B00 trampoline.
if (PAGE_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PAGE_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PAGE_TRACE=0)
endif ()

# RTC (MC146818 Mr Gluk TimeKeeper) port tracing — logs every IN/OUT with low
# byte 0xF7 (and the EFF7 enable), to discover Gluk's real RTC port/register use.
if (RTC_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RTC_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE RTC_PORT_TRACE=0)
endif ()

# HDMI signal-integrity knobs. Always defined (0 or 1) so the driver's own
# `#ifndef` defaults never silently win over an OFF chosen here.
foreach (HDMI_SI_OPT HDMI_TMDS_LEVEL_CLAMP HDMI_SOFT_CLK)
    if (${HDMI_SI_OPT})
        target_compile_definitions(${PROJECT_NAME} PRIVATE ${HDMI_SI_OPT}=1)
    else ()
        target_compile_definitions(${PROJECT_NAME} PRIVATE ${HDMI_SI_OPT}=0)
    endif ()
endforeach ()

# Per-60-frame CPU/HDMI/FPS performance logging (see Video.cpp `#if PERF_TRACE`).
if (PERF_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PERF_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PERF_TRACE=0)
endif ()

# Negative-IDL attribution on Profi ([NEG2] lines, ESPectrum.cpp): worst-frame
# bookkeeping every frame + a log line per 60-frame window with overruns.
if (NEG2_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NEG2_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE NEG2_TRACE=0)
endif ()

# Per-page Z80 access counting between page load and eviction (see MemESP.h/.cpp
# `#if MEM_ACCESS_TRACE`, [ACC] log in Video.cpp). Feasibility study for the
# accessor-mode bank window idea: if evicted trampoline pages show only a few
# hundred accesses, serving them per-byte over SPI beats the 16KB page load.
if (MEM_ACCESS_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MEM_ACCESS_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE MEM_ACCESS_TRACE=0)
endif ()

# General Sound perf counters toggle (see GS.cpp `GS_PERF()` / `#if GS_PERF_TRACE`).
if (GS_PERF_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GS_PERF_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE GS_PERF_TRACE=0)
endif ()

# General Sound port-IO trace ring + auto-dump triggers (see GS.cpp
# `#ifdef GS_DEBUG_TRACE`). Costs ~50 KB SRAM — diagnostic builds only;
# the code tests presence with #ifdef, so define it only when ON.
if (GS_DEBUG_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GS_DEBUG_TRACE=1)
endif ()

# NeoGS 1 Hz health line (GS-Z80 PC / mapping / SD counters — GS.cpp pollPerf).
# The UART write blocks core0 ~2-4 ms per line = once-per-second stutter, so
# it's a diagnostic toggle, not a default.
# NGS_SD_TRACE raises the level to 2, which additionally logs one line per SD
# command the guest issues (NgsSd.cpp). That is the tool for "player X doesn't
# see its files": it shows whether the guest reaches the SD layer at all and
# which sector its scan dies on. Very heavy — a directory scan issues thousands
# of commands and each line stalls core1 on the print mutex, so a scan takes
# minutes. Usually only the first screenful and the last line matter.
if (NGS_SD_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NGS_TRACE=2)
elseif (NGS_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NGS_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE NGS_TRACE=0)
endif ()

# HDMI audio 1 Hz health line + OPL3 per-frame cost meter. Their counters are
# cheap, but each printed line costs UART time on DBG_UART builds — off unless
# hunting an audio regression.
if (HDMI_AUDIO_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_AUDIO_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI_AUDIO_TRACE=0)
endif ()
if (OPL_PERF_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE OPL_PERF_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE OPL_PERF_TRACE=0)
endif ()

# Z-Controller/DivSD SPI-SD command tracing (DivMMC.cpp mmc engine).
if (ZC_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZC_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZC_PORT_TRACE=0)
endif ()

# TEMPORARY Neo8 wild-jump hunter (Debug.cpp / Z80_JLS.cpp) — per-instruction
# hook, debug builds only.
if (NEO8_TRAP)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NEO8_TRAP=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE NEO8_TRAP=0)
endif ()

# 0x7FFD/0xDFFD paging-port write tracing (see Ports.cpp `#if PROFI_PORT_TRACE`).
if (PROFI_PORT_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PROFI_PORT_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PROFI_PORT_TRACE=0)
endif ()

# Scorpion GMX paging trace (see Ports.cpp / Z80_JLS.cpp `#if GMX_TRACE`).
if (GMX_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GMX_TRACE=1)
endif ()
# One-shot #7FFD multicolor phase capture (see Ports.cpp `#if MC7FFD_TRACE`).
if (MC7FFD_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MC7FFD_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE MC7FFD_TRACE=0)
endif ()

# TSFM guest-T budget probe (see Ports.cpp `#if TSFM_TRACE`).
if (TSFM_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSFM_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE TSFM_TRACE=0)
endif ()

# ZiFi port/UART tracing (see ZiFi.cpp / ZiFiAT.cpp `#if ZIFI_TRACE`).
if (ZIFI_TRACE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZIFI_TRACE=1)
else ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ZIFI_TRACE=0)
endif ()

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
    add_subdirectory(drivers/ps2kbd)
    target_link_libraries(${PROJECT_NAME} PRIVATE ps2kbd)
    target_compile_definitions(${PROJECT_NAME} PRIVATE KBDUSB)
ELSE()
    add_subdirectory(drivers/ps2)
    target_link_libraries(${PROJECT_NAME} PRIVATE ps2)
    target_compile_definitions(${PROJECT_NAME} PRIVATE KBDPS2)
ENDIF()

if (BUTTER_PSRAM_GPIO)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BUTTER_PSRAM_GPIO=${BUTTER_PSRAM_GPIO})
endif()
# ZiFi (ESP-01S WiFi NIC emulation).
# TX/RX pins are chosen at RUNTIME (Network → GPIO picker, stored in NVS) from a
# per-board candidate list defined in src/BoardPins.cpp. No compile-time pin
# define is needed anymore; the old -DZIFI_TX_PIN/-DZIFI_RX_PIN are obsolete.

# ZiFi network client (FTP/SFTP/SSH over the ESP-01S TCP sockets).
# SSH crypto runs on the Pico via mbedTLS primitives (NOT wolfSSH, NOT mbedTLS's
# TLS stack; see src/mbedtls_config_picospeccy.h). New sources (ZiFiSock/Ftp/Ssh/
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

# redcode/Z80 build configuration for GS (LGPLv3 library vendored in src/GS/)
set_source_files_properties(src/GS/Z80_redcode.c PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops -DZ80_STATIC -DGS_Z80_INSN_IN_SRAM -DZ80_EXTERNAL_HEADER=\\\"Z80_compat.h\\\"")
set_source_files_properties(src/GS/GS.cpp PROPERTIES
    COMPILE_FLAGS "-O3 -funroll-loops -DZ80_STATIC -DZ80_EXTERNAL_HEADER=\\\"Z80_compat.h\\\"")
# minimp3 decode (NeoGS MP3 path): -O3 buys ~2x over the default -Os — one MP3
# frame must fit comfortably inside a frame-pacing idle slot on core0.
set_source_files_properties(src/GS/NgsMp3.cpp PROPERTIES COMPILE_FLAGS "-O3")

# ── RP2350B-Plus-W pin overrides (MURM_W / MURM2_W) ──────────────────────────
# The module's 40-pin header is Pico-compatible for GPIO0..GPIO22, so keyboard,
# SD, display, MIDI/WAV and (on MURM2) audio all keep their numbers. Three things
# move, and the carrier arms below take them from these variables:
#
#   * the three positions a Pico exposes as GP26/27/28 carry GPIO40/41/42 here.
#     That is MURM1's whole audio block, and MURM2's NESPAD data pair.
#   * the user LED is GPIO23 (LED2), not GPIO25.
#   * GP29 is not a header pin on any Pico, so MURM1/MURM2's CLK_AY_PIN2=29 never
#     reached the carrier anyway; here GPIO29 is a bottom pad and GPIO36-39 are
#     the radio's WL_ON/WL_D/WL_CS/WL_CLK (schematic), so the AY clock output is dropped
#     rather than pointed at a pin we do not own.
#
# GPIO24-35 and 43-45 exist on bottom pads that a Murmulator socket cannot reach.
if(PICOSPECCY_WIFI)
    set(W_HDR26 40)                 # what the Pico header position of GP26 carries
    set(W_HDR27 41)                 # ... GP27
    set(W_HDR28 42)                 # ... GP28
    set(W_LED_PIN 23)               # LED2
    # GP29 is not a header pin on any Pico, so this AY clock output never reached
    # a Murmulator carrier in the first place; on the W module GPIO29 is a bottom
    # pad and GPIO36-39 are the radio's (WL_ON/WL_D/WL_CS/WL_CLK), so there is no pin to
    # point it at. 255 = absent, the same sentinel PSRAM_PIN_* and ZERO2's LED use;
    # PinSerialData_595.c skips it exactly like a pin ZiFi has claimed.
    set(W_AY_CLK2 255)
else()
    set(W_HDR26 26)
    set(W_HDR27 27)
    set(W_HDR28 28)
    set(W_LED_PIN 25)
    set(W_AY_CLK2 29)
endif()

# MURM_W only: the carrier's own PIO SPI PSRAM comes off. pio0 is the only block
# that can reach both the radio (GPIO36-39) and MURM1's relocated audio
# (GPIO40/41/42) — RP2350 PIO sees 32 consecutive GPIOs from gpio_base, and pio1
# (keyboard GP0/1, NESPAD GP14-16) and pio2 (HDMI GP6-13) are pinned to base 0.
# pio0 then has to hold CYW43 (5-7 instructions) plus I2S (9, or 17 for CS4334),
# and the SPI PSRAM's two programs are another 18-20 — 34..44 of 32. Since the
# module carries QSPI PSRAM pads on GPIO47 (memory-mapped butter PSRAM, which is
# strictly faster than the PIO SPI part), dropping the carrier chip is the right
# half of the trade; it also frees 3 DMA channels and GP18-21.
if(MURM_W)
    set(M1_PSRAM_DEFS
        PSRAM_SPINLOCK=255
        PSRAM_ASYNC=255
        PSRAM_PIN_CS=255
        PSRAM_PIN_SCK=255
        PSRAM_PIN_MOSI=255
        PSRAM_PIN_MISO=255)
else()
    set(M1_PSRAM_DEFS
        PSRAM
        PSRAM_SPINLOCK=1
        PSRAM_ASYNC=1
        PSRAM_MAX_SCK_MHZ=63
        PSRAM_PIN_CS=18
        PSRAM_PIN_SCK=19
        PSRAM_PIN_MOSI=20
        PSRAM_PIN_MISO=21)
endif()

IF(MURM2)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        MURM2=1
        CPU_MHZ=${CPU_MHZ}

        KBD_CLOCK_PIN=2
        KBD_DATA_PIN=3

        LOAD_WAV_PIO=22

        CLK_AY_PIN1=21 # not in use
        LATCH_595_PIN=9
        CLK_595_PIN=10
        DATA_595_PIN=11
        CLK_AY_PIN2=${W_AY_CLK2}

        I2S_DATA_PIO=9 #
        I2S_BCK_PIO=10 #
        I2S_LCK_PIO=11

        BEEPER_PIN=9 #
        PWM_PIN0=10 #
        PWM_PIN1=11
        MIDI_TX_PIN=22

        # SDCARD
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=6
        SDCARD_PIN_SPI0_MOSI=7
        SDCARD_PIN_SPI0_MISO=4

        #PSRAM
        # PSRAM_MUTEX=1
        PSRAM_SPINLOCK=255
        PSRAM_ASYNC=255

        PSRAM_PIN_CS=255
        PSRAM_PIN_SCK=255
        PSRAM_PIN_MOSI=255
        PSRAM_PIN_MISO=255

        # NES Gamepad
        USE_NESPAD
        NES_GPIO_CLK=20
        NES_GPIO_LAT=21
        NES_GPIO_DATA=${W_HDR26}     # +1 (joy2) is implicit: W_HDR27

        # VGA 8 pins starts from pin:
        VGA_BASE_PIN=12

        # HDMI 8 pins starts from pin:
        HDMI_BASE_PIN=12


        # TFT
        TFT_CS_PIN=12
        TFT_RST_PIN=14
        TFT_LED_PIN=15
        TFT_DC_PIN=16
        TFT_DATA_PIN=18
        TFT_CLK_PIN=19
        TFT_INV=${TFT_INV}

        DEFAULT_THROTTLING=0
        PICO_DEFAULT_LED_PIN=${W_LED_PIN}
    )
ELSEIF(PICO_PC)
    # Debug-Probe UART toggle PICO_PC_DBG_UART declared at top. Enables UART0 on the
    # on-board DBG1 header (GP0=TX, GP1=RX) of Olimex RP2040-PICO-PC.
    # DBG1 shares pins with KBD_CLOCK/KBD_DATA → keyboard is auto-remapped to FREE
    # pins GP10/GP11 when DBG_UART is enabled.
    if(PICO_PC_DBG_UART)
        pico_enable_stdio_uart(${PROJECT_NAME} 1)
        set(PICO_PC_KBD_CLOCK 10) # GP0 taken by UART TX → move KBD to FREE pin
        set(PICO_PC_KBD_DATA 11)  # GP1 taken by UART RX → move KBD to FREE pin
    else()
        set(PICO_PC_KBD_CLOCK 0)
        set(PICO_PC_KBD_DATA 1)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        $<$<BOOL:${PICO_PC_DBG_UART}>:PICO_DEFAULT_UART=0>
        $<$<BOOL:${PICO_PC_DBG_UART}>:PICO_DEFAULT_UART_TX_PIN=0>
        $<$<BOOL:${PICO_PC_DBG_UART}>:PICO_DEFAULT_UART_RX_PIN=1>
        $<$<BOOL:${PICO_PC_DBG_UART}>:DBG_UART_ENABLED=1>

        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        CPU_MHZ=${CPU_MHZ}

        PICO_PC=1

        # GP-5  UXT1-10
        # GP-8  UXT1-6
        # GP-9  UXT1-5
        # GP-21 UXT1-4
        # GP-20 UXT1-3

        # QWST1 3/4 - GP2/3

        # DBG1 1/2  (GP0=UART0_TX, GP1=UART0_RX) — used by PICO_PC_DBG_UART
        KBD_CLOCK_PIN=${PICO_PC_KBD_CLOCK}
        KBD_DATA_PIN=${PICO_PC_KBD_DATA}

        LOAD_WAV_PIO=5 # UXT1-10

        CLK_AY_PIN1=21 # not in use
        LATCH_595_PIN=26
        CLK_595_PIN=27
        DATA_595_PIN=28
        CLK_AY_PIN2=29

        I2S_DATA_PIO=26 # not implemented
        I2S_BCK_PIO=26  # I2S_BCK_PIO == I2S_LCK_PIO == I2S_DATA_PIO - unsupported mark
        I2S_LCK_PIO=26

        BEEPER_PIN=26 # not implemented
        PWM_PIN0=27 # R
        PWM_PIN1=28 # L
        MIDI_TX_PIN=26

        # SDCARD
        SDCARD_PIN_SPI0_MISO=4
        SDCARD_PIN_SPI0_SCK=6
        SDCARD_PIN_SPI0_MOSI=7
        SDCARD_PIN_SPI0_CS=22

        #PSRAM             # not implemented
        PSRAM_SPINLOCK=255 # not implemented
        PSRAM_ASYNC=255    # not implemented

        PSRAM_PIN_CS=255   # not implemented
        PSRAM_PIN_SCK=255  # not implemented
        PSRAM_PIN_MOSI=255 # not implemented
        PSRAM_PIN_MISO=255 # not implemented

        # NES Gamepad
        USE_NESPAD
        NES_GPIO_CLK=5    # UXT1-10
        NES_GPIO_LAT=9    # UXT1-5
        NES_GPIO_DATA=20  # UXT1-3
        NES_GPIO_DATA2=21 # UXT1-4

        # VGA 8 pins starts from pin:
        VGA_BASE_PIN=12

        # HDMI 8 pins starts from pin:
        HDMI_BASE_PIN=12

        DEFAULT_THROTTLING=0
        PICO_DEFAULT_LED_PIN=25
    )
ELSEIF(PICO_DV)
    # Debug-Probe UART toggle PICO_DV_DBG_UART declared at top. Enables UART1 on GP20/GP21.
    # Note: GP20 is also LOAD_WAV_PIO default — auto-remapped to GP3 when UART enabled.
    if(PICO_DV_DBG_UART)
        pico_enable_stdio_uart(${PROJECT_NAME} 1)
        set(PICO_DV_LOAD_WAV_PIO 255)  # GP20 taken by UART → use FREE pin
        set(MIDI_TX_PIN 255) # GP21 taken by UART → use FREE pin
    else()
        set(PICO_DV_LOAD_WAV_PIO 20)
        set(MIDI_TX_PIN 21)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        $<$<BOOL:${PICO_DV_DBG_UART}>:PICO_DEFAULT_UART=1>
        $<$<BOOL:${PICO_DV_DBG_UART}>:PICO_DEFAULT_UART_TX_PIN=20>
        $<$<BOOL:${PICO_DV_DBG_UART}>:PICO_DEFAULT_UART_RX_PIN=21>
        # Console UART intentionally enabled. The board header (pico2.h) ALWAYS
        # defines PICO_DEFAULT_UART=0 on GP0/GP1 — which is exactly the ZiFi UART
        # on PICO_DV — so Debug::log/stdio must gate on this explicit flag, NOT on
        # PICO_DEFAULT_UART, or logs corrupt the ESP AT stream and FTP won't start.
        $<$<BOOL:${PICO_DV_DBG_UART}>:DBG_UART_ENABLED=1>

        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        CPU_MHZ=${CPU_MHZ}

        PICO_DV=1

        # PS/2 keyboard (moved from GP0/1 → GP14/15; GP0/1 freed for ZiFi UART0)
        KBD_CLOCK_PIN=14
        KBD_DATA_PIN=15

        LOAD_WAV_PIO=${PICO_DV_LOAD_WAV_PIO}

        CLK_AY_PIN1=21 # not in use
        LATCH_595_PIN=26
        CLK_595_PIN=27
        DATA_595_PIN=28
        CLK_AY_PIN2=29

        I2S_DATA_PIO=26 #
        I2S_BCK_PIO=27 #
        I2S_LCK_PIO=28

        PWM_PIN0=26 # R
        PWM_PIN1=27 # L
        BEEPER_PIN=28 # not implemented
        MIDI_TX_PIN=${MIDI_TX_PIN}

        # SDCARD
        SDCARD_SPI_BUS=spi1
        SDCARD_PIN_SPI0_MISO=19
        SDCARD_PIN_SPI0_SCK=5
        SDCARD_PIN_SPI0_MOSI=18
        SDCARD_PIN_SPI0_CS=22

        SDCARD_PIO=pio1
        SDCARD_PIO_SM=0

        #PSRAM             # not implemented
        PSRAM_SPINLOCK=255 # not implemented
        PSRAM_ASYNC=255    # not implemented

        PSRAM_PIN_CS=255   # not implemented
        PSRAM_PIN_SCK=255  # not implemented
        PSRAM_PIN_MOSI=255 # not implemented
        PSRAM_PIN_MISO=255 # not implemented

        # NES Gamepad
        NES_GPIO_CLK=8    # UXT1-6
        NES_GPIO_LAT=9    # UXT1-5
        NES_GPIO_DATA=20  # UXT1-3
        NES_GPIO_DATA2=21 # UXT1-4

        # VGA 8 pins starts from pin:
        VGA_BASE_PIN=6

        # HDMI 8 pins starts from pin:
        HDMI_BASE_PIN=6

        DEFAULT_THROTTLING=0
        PICO_DEFAULT_LED_PIN=25
)
ELSEIF(ZERO2)
    # Debug-Probe UART toggle ZERO2_DBG_UART declared at top (UART1 on GP20/GP21).
    if(ZERO2_DBG_UART)
        pico_enable_stdio_uart(${PROJECT_NAME} 1)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        $<$<BOOL:${ZERO2_DBG_UART}>:PICO_DEFAULT_UART=1>
        $<$<BOOL:${ZERO2_DBG_UART}>:PICO_DEFAULT_UART_TX_PIN=20>
        $<$<BOOL:${ZERO2_DBG_UART}>:PICO_DEFAULT_UART_RX_PIN=21>
        $<$<BOOL:${ZERO2_DBG_UART}>:DBG_UART_ENABLED=1>

        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        CPU_MHZ=${CPU_MHZ}
        ZERO2=1

        # PS/2 keyboard. GP2/3 is the default pair and is shared with the PCM5122
        # DAC board's control I2C — when that board is detected at boot (or picked
        # in Audio > Driver) the keyboard is moved to the ALT pair at runtime; see
        # board_kbd_set_alt_pins() in main.cpp.
        KBD_CLOCK_PIN=2
        KBD_DATA_PIN=3
        KBD_ALT_CLOCK_PIN=14
        KBD_ALT_DATA_PIN=15

        LOAD_WAV_PIO=17

        # PCM5122 Audio Board (I2S)
       PCM5122_I2S_DATA=21
       PCM5122_I2S_BCK=18
       PCM5122_I2S_LCK=19
        # PCM5122 Audio Board (I2C control)
       PCM5122_I2C_SDA=2
       PCM5122_I2C_SCL=3

        CLK_AY_PIN1=8 # not in use
        LATCH_595_PIN=10
        CLK_595_PIN=11
        DATA_595_PIN=12
        CLK_AY_PIN2=7

        I2S_DATA_PIO=10 # DIN
        I2S_BCK_PIO=11 # BCK
        I2S_LCK_PIO=12 # LRCK

        PWM_PIN0=10
        PWM_PIN1=11
        BEEPER_PIN=12
        MIDI_TX_PIN=22

        # SDCARD
        SDCARD_SPI_BUS=spi1
        SDCARD_PIN_SPI0_SCK=30
        SDCARD_PIN_SPI0_MOSI=31
        SDCARD_PIN_SPI0_MISO=40
        SDCARD_PIN_SPI0_CS=43

        #PSRAM             # not implemented
        PSRAM_SPINLOCK=255 # not implemented
        PSRAM_ASYNC=255    # not implemented

        PSRAM_PIN_CS=255   # not implemented
        PSRAM_PIN_SCK=255  # not implemented
        PSRAM_PIN_MOSI=255 # not implemented
        PSRAM_PIN_MISO=255 # not implemented

        # NES Gamepad
        USE_NESPAD
        NES_GPIO_CLK=4
        NES_GPIO_LAT=5
        NES_GPIO_DATA=7
        NES_GPIO_DATA2=8

        # VGA 8 pins starts from pin:
        VGA_BASE_PIN=32

        # HDMI 8 pins starts from pin:
        HDMI_BASE_PIN=32

        PICO_DEFAULT_LED_PIN=255
        DEFAULT_THROTTLING=0
    )
ELSE()
    # Debug-Probe UART toggle MURM1_DBG_UART declared at top (UART0 on GP0/GP1;
    # moves KBD to GP16/GP17, disables NESPAD).
    if(MURM1_DBG_UART)
        pico_enable_stdio_uart(${PROJECT_NAME} 1)
        set(MURM1_KBD_CLOCK_PIN 16)
        set(MURM1_KBD_DATA_PIN  17)
    else()
        set(MURM1_KBD_CLOCK_PIN 0)
        set(MURM1_KBD_DATA_PIN  1)
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        $<$<BOOL:${MURM1_DBG_UART}>:PICO_DEFAULT_UART=0>
        $<$<BOOL:${MURM1_DBG_UART}>:PICO_DEFAULT_UART_TX_PIN=0>
        $<$<BOOL:${MURM1_DBG_UART}>:PICO_DEFAULT_UART_RX_PIN=1>
        $<$<BOOL:${MURM1_DBG_UART}>:DBG_UART_ENABLED=1>

        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        CPU_MHZ=${CPU_MHZ}

        KBD_CLOCK_PIN=${MURM1_KBD_CLOCK_PIN}
        KBD_DATA_PIN=${MURM1_KBD_DATA_PIN}

        LOAD_WAV_PIO=22

        # Audio: GP26/27/28 on a Pi Pico 2, GPIO40/41/42 on the RP2350B-Plus-W —
        # same three header positions, different GPIO numbers (see W_HDR* above).
        I2S_DATA_PIO=${W_HDR26} #
        I2S_BCK_PIO=${W_HDR27} #
        I2S_LCK_PIO=${W_HDR28}

        CLK_AY_PIN1=21 # not in use
        LATCH_595_PIN=${W_HDR26}
        CLK_595_PIN=${W_HDR27}
        DATA_595_PIN=${W_HDR28}
        CLK_AY_PIN2=${W_AY_CLK2}

        PWM_PIN0=${W_HDR26} #
        PWM_PIN1=${W_HDR27} #
        BEEPER_PIN=${W_HDR28}
        MIDI_TX_PIN=22

        # SDCARD
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=2
        SDCARD_PIN_SPI0_MOSI=3
        SDCARD_PIN_SPI0_MISO=4

        # PSRAM_MUTEX=1
        # The carrier's PIO SPI PSRAM on GP18-21 — present on MURM, dropped on
        # MURM_W to keep pio0 inside 32 instructions (see M1_PSRAM_DEFS above).
        # PSRAM_MAX_SCK_MHZ, when it is set: at 126 MHz (default) sys_clk=378 gives
        # clkdiv=1.5 — a fractional divider that makes SCK jitter and corrupts this
        # board's APS6404 8 MB chip (~930 KB/1 MB bad at boot, size probe flaps
        # 8M↔1M). 94 MHz targets clkdiv=2.0 at 378 MHz (round(378/188)=2 →
        # SCK=94.5 MHz, still an integer divider with a clean waveform — same reason
        # 63 MHz / clkdiv=3.0 was safe, one step faster: +50% bandwidth).
        # init_psram() runs an at-speed write/verify memtest and drops the target
        # back to PSRAM_FALLBACK_SCK_MHZ (63) if this chip fails at 94.5 MHz;
        # psram_update_clkdiv() keeps the chosen target after CPU overclock too.
        ${M1_PSRAM_DEFS}

        # NES Gamepad — disabled when MURM1_DBG_UART=ON (KBD moves to GP16/GP17, conflicts with NES_GPIO_DATA=16)
        $<$<NOT:$<BOOL:${MURM1_DBG_UART}>>:USE_NESPAD>
        NES_GPIO_CLK=14
        NES_GPIO_DATA=16
        NES_GPIO_LAT=15

        # VGA 8 pins starts from pin:
        VGA_BASE_PIN=6

        # HDMI 8 pins starts from pin:
        HDMI_BASE_PIN=6

        # TFT
        TFT_CS_PIN=6
        TFT_RST_PIN=8
        TFT_LED_PIN=9
        TFT_DC_PIN=10
        TFT_DATA_PIN=12
        TFT_CLK_PIN=13
        TFT_INV=${TFT_INV}

        DEFAULT_THROTTLING=0
        PICO_DEFAULT_LED_PIN=${W_LED_PIN}
    )
ENDIF()

# DISPLAY_TAG is the same choice in one word, for the compact name below.
IF(TFT)
    target_link_libraries(${PROJECT_NAME} PRIVATE st7789)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TFT)
    IF(ILI9341)
        SET(BUILD_NAME "${BUILD_NAME}-ILI9341")
        SET(DISPLAY_TAG "ILI9341")
        target_compile_definitions(${PROJECT_NAME} PRIVATE ILI9341)
    ELSE()
        IF(TFT_INV)
            SET(BUILD_NAME "${BUILD_NAME}-ST7789V")
            SET(DISPLAY_TAG "ST7789V")
        ELSE()
            SET(BUILD_NAME "${BUILD_NAME}-ST7789")
            SET(DISPLAY_TAG "ST7789")
        ENDIF()
    ENDIF()
ELSEIF(TV)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TV)
    target_link_libraries(${PROJECT_NAME} PRIVATE tv)
    SET(BUILD_NAME "${BUILD_NAME}-TV")
    SET(DISPLAY_TAG "TV")
ELSEIF(SOFTTV)
	target_compile_definitions(${PROJECT_NAME} PRIVATE SOFTTV)
	target_link_libraries(${PROJECT_NAME} PRIVATE tv-software)
	SET(BUILD_NAME "${BUILD_NAME}-TV-SOFT")
	SET(DISPLAY_TAG "TVSOFT")
ELSE()
    target_link_libraries(${PROJECT_NAME} PRIVATE hdmi)
    target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGA_HDMI)
    SET(BUILD_NAME "${BUILD_NAME}-VGA-HDMI")
    # ONE image drives both outputs and the pick is made at boot (SELECT_VGA, from
    # linkVGA01 or Config::video_driver), so this tag is only the fallback: the OSD
    # substitutes the output that actually came up. Every other DISPLAY_TAG above is
    # a real build-time choice and needs no such treatment.
    SET(DISPLAY_TAG "VGAHDMI")
ENDIF()

# The PIO-USB host is a build-time variant of the same board, not a display mode, so
# it tags the name after the display part: z0p2-speccy-VGA-HDMI-PIOUSB-<version>.
SET(VARIANT_TAG "")
IF(ZERO2 AND ZERO2_PIO_USB)
    SET(BUILD_NAME "${BUILD_NAME}-PIOUSB")
    SET(VARIANT_TAG "+PIOUSB")
    SET(DISPLAY_TAG "${DISPLAY_TAG}${VARIANT_TAG}")
ENDIF()

SET(BUILD_NAME "${BUILD_NAME}-${PORT_VERSION}")

# Compact form for the OSD's Hardware Info row — z0p2+HDMI+1.0.2 — where the full
# file name beside an aligned label overruns the narrowest page nm:: supports (40
# cols). Built from the same pieces rather than parsed back out of BUILD_NAME, so the
# two cannot drift apart. The file name itself is unchanged — release assets use it.
SET(BUILD_NAME_SHORT "${BOARD_TAG}+${DISPLAY_TAG}+${PORT_VERSION}")

# ALF is a standard runtime machine (not a separate build), so no "-ALF" suffix.

message(STATUS "BUILD_NAME: ${BUILD_NAME}")

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_BUILD_NAME="${BUILD_NAME}"
    PICO_BUILD_NAME_SHORT="${BUILD_NAME_SHORT}"
    PICO_BUILD_VARIANT_TAG="${VARIANT_TAG}"
    PICO_GIT_BRANCH="${GIT_BRANCH}"
    PICO_GIT_COMMIT="${GIT_COMMIT_HASH}"
)
