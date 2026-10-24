# On-chip WiFi (the *_W and *_WS boards) and the W boards' PIO block assignment.

# On-chip WiFi (*_W: Pico 2 W / Pimoroni Pico Plus 2 W; *_WS: Waveshare RP2350B-Plus-W with a
# Raspberry Pi Radio Module 2 — a CYW43439 either way).
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
    # lwIP in POLL mode (NO_SYS, src/drivers/board/lwipopts.h): every stack callback runs inside
    # cyw43_arch_poll() from core0 — WifiNet::poll() once per frame plus the waits in
    # WifiNet/WifiSock — so the emulator, not a background IRQ context, decides when
    # the network runs. All lwIP memory is heap/net-arena via Buffer::palloc (see
    # lwipopts.h); the static footprint is a few hundred bytes.
    target_link_libraries(${PROJECT_NAME} PRIVATE pico_cyw43_arch_lwip_poll)
    # The gSPI bus driver is OUR copy, src/net/cyw43_bus_pio_spi_speccy.c (picked up
    # by the src/ glob; a no-op TU on the non-W boards): the SDK's busy-waits in
    # cyw43_spi_transfer() hammer the FASTPERI bridge and starve the HDMI feeder
    # DMA — see the PICO-SPEC PATCH header in that file. Strip the SDK's source so
    # the two do not both define cyw43_spi_transfer & co.
    get_target_property(_cyw43_bus_srcs cyw43_driver_picow INTERFACE_SOURCES)
    list(FILTER _cyw43_bus_srcs EXCLUDE REGEX "cyw43_bus_pio_spi\\.c$")
    set_target_properties(cyw43_driver_picow PROPERTIES INTERFACE_SOURCES "${_cyw43_bus_srcs}")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PICOSPECCY_WIFI=0)
endif()
# Board identity for the sources. MURM2 already defines MURM2=1 in its pin arm;
# MURM1 defines nothing and is the `#else` fallback everywhere, so the Murmulator W
# boards need their own macros for src/drivers/board/BoardPins.cpp to tell the
# modules apart. The PICO_DV/PICO_PC ones only label the image. PICOSPECCY_WIFI_WS
# is the module layout: radio above GPIO31 (Waveshare) vs on GP23-25/29 (stock).
foreach(_wb MURM_W MURM2_W PICO_DV_W PICO_PC_W MURM_WS MURM2_WS PICO_DV_WS PICO_PC_WS)
    if (${_wb})
        target_compile_definitions(${PROJECT_NAME} PRIVATE ${_wb}=1)
    endif()
endforeach()
if (PICOSPECCY_WIFI_WS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PICOSPECCY_WIFI_WS=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PICOSPECCY_WIFI_WS=0)
endif()

# PIO block assignment on the Waveshare (*_WS) boards. The stock *_W layout needs
# none of this: its radio is on GP23-25/29, below GPIO32, so it takes whatever
# block has room at gpio_base 0 and every carrier peripheral stays where it is on
# the plain board. On the Waveshare module: an RP2350 PIO reaches 32 CONSECUTIVE
# GPIOs from its gpio_base (0 or 16), and the radio sits above GPIO31 — so CYW43
# needs a block at base 16, which pio1 (keyboard on GP0-3) and pio2 (HDMI on
# GP6-19) cannot be. That leaves pio0, and whatever else has moved above GPIO31
# has to join it there:
#
#   MURM_WS  — audio is on GPIO40/41/42, so I2S moves pio1 -> pio0.
#              pio0: CYW43 (5-7) + I2S (9, or 17 for CS4334)   = 14..24 / 32
#              pio1: PS/2 (7) + NESPAD (7)                     = 14 / 32
#   MURM2_WS — audio stays on GP9/10/11 (below GP23, so unmoved), but the pad's
#              data pair is on GPIO40/41, so NESPAD moves pio1 -> pio0.
#              pio0: CYW43 (5-7) + NESPAD (7)                  = 12..14 / 32
#              pio1: PS/2 (7) + I2S (9, or 17)                 = 16..24 / 32
#   PICO_DV_WS — audio is on the GP26/27/28 header positions, i.e. GPIO40/41/42
#              here, so I2S moves pio1 -> pio0 as on MURM_WS; the pad is not used
#              on this carrier at all. pio1 keeps the PS/2 keyboard and — the one
#              thing unique to a Pico DV base — the SD card (SDCARD_PIO=pio1).
#              pio0: CYW43 (5-7) + I2S (9, or 17 for CS4334)   = 14..24 / 32
#              pio1: PS/2 (7) + SD SPI (4)                     = 11 / 32
#   PICO_PC_WS — nothing PIO moves: the audio block that goes to GPIO40/41/42 is
#              PWM / beeper / MIDI UART (this carrier has no I2S), the pad (GP5/9/
#              20/21) and keyboard stay on pio1 at base 0. pio0 (or pio2 under
#              VGA) holds the radio alone. With HSTX — the AUTO default here, as
#              on PICO_PC/MURM2_WS — the display claims NO PIO at all (TMDS
#              expander; VGA on HSTX likewise), and RAW keeps only the address
#              converter on pio2, so auxPio's pick stays free in every case.
#
# pio2 keeps HDMI (18 / 32) on all three. cyw43_arch_init() must therefore run
# AFTER graphics_init() and the keyboard have fixed their blocks at base 0, or the
# SDK's pio_claim_free_sm_and_add_program_for_gpio_range() can take the wrong one.
# (Only ONE display driver is initialised — graphics_init() picks VGA or HDMI —
# so under VGA the radio and I2S move to pio2 instead; BoardPins::auxPio is that
# decision and every one of them reads it.)
if (MURM_WS OR PICO_DV_WS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE I2S_PIO=pio0)
endif()
if (MURM2_WS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NESPAD_PIO=pio0)
endif()
