# Board selection: the W modules, the default board, PICO_BOARD / platform,
# FLASH_SIZE and BOARD_TAG. Must run before pico_sdk_import.cmake.

# A W target IS its carrier, plus a different plugged-in module: turn the carrier
# on so every pin arm, display matrix and runtime `#if MURM2` (cmake/board-pins.cmake) keeps working,
# and raise PICOSPECCY_WIFI as the single "this image has a radio" switch. Must run
# BEFORE the default-board guard, or -DMURM_W=ON alone would also default MURM2 on.
# Two module layouts (cmake/options.cmake): *_W = stock Pico 2 W / Pimoroni Pico
# Plus 2 W (radio GP23/24/25/29), *_WS = Waveshare RP2350B-Plus-W (radio GPIO36-39);
# PICOSPECCY_WIFI_WS tells them apart everywhere below.
if(MURM_W OR MURM_WS)
    set(MURM ON)
endif()
if(MURM2_W OR MURM2_WS)
    set(MURM2 ON)
endif()
if(PICO_DV_W OR PICO_DV_WS)
    set(PICO_DV ON)
endif()
if(PICO_PC_W OR PICO_PC_WS)
    set(PICO_PC ON)
endif()
set(PICOSPECCY_WIFI_WS OFF)
if(MURM_WS OR MURM2_WS OR PICO_DV_WS OR PICO_PC_WS)
    set(PICOSPECCY_WIFI_WS ON)
endif()
if((MURM_W OR MURM2_W OR PICO_DV_W OR PICO_PC_W) AND PICOSPECCY_WIFI_WS)
    message(FATAL_ERROR "Pick ONE W module: the *_W (Pico 2 W / Pico Plus 2 W) and "
        "*_WS (Waveshare RP2350B-Plus-W) options cannot be combined.")
endif()
if(MURM_W OR MURM2_W OR PICO_DV_W OR PICO_PC_W OR PICOSPECCY_WIFI_WS)
    set(PICOSPECCY_WIFI ON)
    # The radio needs a PIO block of its own: on the Waveshare module its pins are
    # above GPIO31 (gpio_base 16), on the stock layout below it (base 0). pio1 is the
    # keyboard's, so it takes the block the DISPLAY is not using: pio0 with HDMI on
    # pio2, pio2 with VGA on pio0 — a VGA_HDMI image works either way
    # (BoardPins::auxPio decides at boot, and I2S / NESPAD follow it). The
    # single-output SOFTTV/TV/TFT drivers take pio0 at base 0 and their images never
    # build hdmi.c, so pio2 would be free there too — but that combination has not
    # been wired or tested, so refuse it rather than ship a WiFi board with a
    # silently dead radio.
    if(SOFTTV OR TV OR TFT OR ILI9341 OR ST7789)
        message(FATAL_ERROR
            "The W/WS boards are built VGA_HDMI only for now: the "
            "SOFTTV/TV/TFT drivers have not been paired with the CYW43's PIO "
            "block. Build these boards with -DVGA_HDMI=ON (the default).")
    endif()
endif()

# Target board (default: MURM2; override with -D flags, e.g. cmake -DPICO_DV=ON)
if(NOT MURM AND NOT MURM2 AND NOT PICO_PC AND NOT PICO_DV AND NOT ZERO2)
    set(MURM2 ON)
endif()

# Pull in Raspberry Pi Pico SDK (must be before project).
# RP2350 only — RP2040 support was dropped, every board below is an RP2350 target.
IF(ZERO2)
    # For Board RP2350-PiZero
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/src/drivers/board/boards)
    set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM rp2350-arm-s)
ELSEIF(PICOSPECCY_WIFI_WS)
    # Waveshare RP2350B-Plus-W: same RP2350B package as the header below, plus the
    # on-board CYW43439, 16 MB of flash and PSRAM pads on GPIO47.
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/src/drivers/board/boards)
    set(PICO_BOARD picospeccy_rp2350b_ws CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM rp2350-arm-s)
ELSEIF(PICOSPECCY_WIFI)
    # Pico 2 W / Pimoroni Pico Plus 2 W: the stock W layout (radio GP23/24/25/29).
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/src/drivers/board/boards)
    set(PICO_BOARD picospeccy_rp2350_w CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM rp2350-arm-s)
else()
    # All remaining boards (MURM2, PICO_PC, PICO_DV, MURM1/default) build for the
    # RP2350B (48-GPIO) package so GPIO 30..47 are valid for any peripheral.
    # pico2 forces PICO_RP2350A=1, so we use a local B-variant header
    # (a CMake `set(PICO_RP2350A 0)` is ignored by the SDK — it must be the header).
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/src/drivers/board/boards)
    set(PICO_BOARD picospeccy_rp2350b CACHE STRING "Board type" FORCE)
    set(PICO_PLATFORM rp2350-arm-s)
endif()

message(STATUS "PICO_BOARD: ${PICO_BOARD}")

# 16 MB on the W modules (Waveshare RP2350B-Plus-W, Pimoroni Pico Plus 2 W), 4 MB
# everywhere else. A plain 4 MB Pico 2 W is NOT covered by the *_W images: the radio
# stack does not fit 4 MB above the hard GM.DLS floor. This is what buys room
# for the ~230 KB CYW43 firmware blob: the GM.DLS bank takes everything above the
# firmware (rp2350-memmap.ld), and on 4 MB the firmware has ~2.4 MB before it eats
# into the GM_BANK_MIN_KB floor — a bound main has already overflowed once — against
# ~14.3 MB here, where the same rule leaves the bank a 13.6 MB tail by itself.
# Any other board builds in two types, 4 MB (default) and 16 MB (-DFLASH_16MB=ON);
# the board header's own 4 MB default is overridden here, before the SDK reads it
# (the SDK then defines PICO_FLASH_SIZE_BYTES for the C code from this value too).
if(PICOSPECCY_WIFI)
    set(FLASH_16MB ON)
endif()
if(FLASH_16MB)
    set(FLASH_SIZE 16384)
    set(PICO_FLASH_SIZE_BYTES "16 * 1024 * 1024")
    set(YRW801_BUILTIN 1)
    set(GM_BANK_BUILTIN 1)
else()
    set(FLASH_SIZE 4096)
    set(YRW801_BUILTIN 0)
    set(GM_BANK_BUILTIN 0)
endif()
# MOONSOUND: the ZXM-MoonSound card (YMF278B wave half), on every image.
# YRW801_BUILTIN: its 2 MB YRW801 ROM embedded in flash (16 MB only); the 4 MB image
# reads it from the SD card (src/speccy/devices/sound/MoonRom.h).
set(MOONSOUND 1)
add_compile_definitions(MOONSOUND=${MOONSOUND} YRW801_BUILTIN=${YRW801_BUILTIN})
# GM_BANK_BUILTIN: the GM.DLS bank embedded in flash (src/speccy/roms/gmbank).
add_compile_definitions(GM_BANK_BUILTIN=${GM_BANK_BUILTIN})

# Board tag: prefix of the firmware file name and the per-board NVS config dir.
# No chip suffix anymore — every target is RP2350.
IF(MURM2_WS)
    set(BOARD_TAG "m2p2ws")
    set (BUTTER_PSRAM_GPIO 47)      # module pads; 0 until a chip is soldered on
ELSEIF(MURM_WS)
    set(BOARD_TAG "m1p2ws")
    set (BUTTER_PSRAM_GPIO 47)
# Before the plain carrier arms on purpose: a W/WS option turns its carrier on, so
# the carrier arm would otherwise claim the tag. (The board header is not a risk
# here — that is picked by PICOSPECCY_WIFI(_WS), further up.)
ELSEIF(PICO_DV_WS)
    set(BOARD_TAG "DVp2ws")
    set (BUTTER_PSRAM_GPIO 47)      # the one pin PICO_DV and the module agree on
ELSEIF(PICO_PC_WS)
    set(BOARD_TAG "PCp2ws")
    set (BUTTER_PSRAM_GPIO 47)      # module pads, not the Pico 2 carrier's GP8
# Pimoroni Pico Plus 2 W (stock W layout): the carrier's own PSRAM pin, as on the
# plain board. main() probes BUTTER_PSRAM_GPIO only on an RP2350A; on the RP2350B
# Pico Plus 2 W it probes GPIO47, where its 8 MB chip is.
ELSEIF(MURM2_W)
    set(BOARD_TAG "m2p2w")
    set (BUTTER_PSRAM_GPIO 8)
ELSEIF(MURM_W)
    set(BOARD_TAG "m1p2w")
    set (BUTTER_PSRAM_GPIO 19)
ELSEIF(PICO_DV_W)
    set(BOARD_TAG "DVp2w")
    set (BUTTER_PSRAM_GPIO 47)
ELSEIF(PICO_PC_W)
    set(BOARD_TAG "PCp2w")
    set (BUTTER_PSRAM_GPIO 8)
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
