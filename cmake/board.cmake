# Board selection: the W modules, the default board, PICO_BOARD / platform,
# FLASH_SIZE and BOARD_TAG. Must run before pico_sdk_import.cmake.

# A W target IS its carrier, plus a different plugged-in module: turn the carrier
# on so every pin arm, display matrix and runtime `#if MURM2` (cmake/board-pins.cmake) keeps working,
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
    # above GPIO31), pio1 is pinned to base 0 by the keyboard, so it needs the block
    # the DISPLAY is not using: pio0 with HDMI on pio2, pio2 with VGA on pio0 — a
    # VGA_HDMI image works either way (BoardPins::auxPio decides at boot, and I2S /
    # NESPAD follow it). The single-output SOFTTV/TV/TFT drivers take pio0 at base 0
    # and their images never build hdmi.c, so pio2 would be free there too — but
    # that combination has not been wired or tested, so refuse it rather than ship
    # a WiFi board with a silently dead radio.
    if(SOFTTV OR TV OR TFT OR ILI9341 OR ST7789)
        message(FATAL_ERROR
            "MURM_W/MURM2_W are built VGA_HDMI only for now: the SOFTTV/TV/TFT "
            "drivers have not been paired with the CYW43's PIO block. "
            "Build these boards with -DVGA_HDMI=ON (the default).")
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
ELSEIF(PICOSPECCY_WIFI)
    # Waveshare RP2350B-Plus-W: same RP2350B package as the header below, plus the
    # on-board CYW43439, 16 MB of flash and PSRAM pads on GPIO47.
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/src/drivers/board/boards)
    set(PICO_BOARD picospeccy_rp2350b_w CACHE STRING "Board type" FORCE)
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

# 16 MB on the RP2350B-Plus-W module, 4 MB everywhere else. This is what buys room
# for the ~230 KB CYW43 firmware blob: the GM.DLS bank takes everything above the
# firmware (rp2350-memmap.ld), and on 4 MB the firmware has ~2.4 MB before it eats
# into the GM_BANK_MIN_KB floor — a bound main has already overflowed once — against
# ~14.3 MB here, where the same rule leaves the bank a 13.6 MB tail by itself.
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
