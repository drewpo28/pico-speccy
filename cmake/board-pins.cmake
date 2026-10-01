# Per-board GPIO map: every pin the firmware drives, as compile definitions.
# The RP2350B-Plus-W overrides come first, the carrier arms read them.

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

        # Debug > UART console: TX-only on GP0 (UART0, a FREE pin here). Collides only
        # with the ZiFi pair 0/1 (and with any other UART0 ZiFi pair) — the console
        # yields to ZiFi in that case.
        DBG_UART_TX_PIN=0

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
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        CPU_MHZ=${CPU_MHZ}

        PICO_PC=1

        # GP-5  UXT1-10
        # GP-8  UXT1-6
        # GP-9  UXT1-5
        # GP-21 UXT1-4
        # GP-20 UXT1-3

        # QWST1 3/4 - GP2/3

        # DBG1 1/2  (GP0=UART0_TX, GP1=UART0_RX) — the on-board Debug header.
        KBD_CLOCK_PIN=0
        KBD_DATA_PIN=1
        # Debug > UART console: TX on the DBG1 header (UART0). GP0 is the PS/2 clock,
        # so the keyboard moves to the FREE pair GP10/GP11 while the console is on.
        DBG_UART_TX_PIN=0
        DBG_UART_KBD_CLOCK_PIN=10

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
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        CPU_MHZ=${CPU_MHZ}

        PICO_DV=1

        # PS/2 keyboard (moved from GP0/1 → GP14/15; GP0/1 freed for ZiFi UART0)
        KBD_CLOCK_PIN=14
        KBD_DATA_PIN=15

        LOAD_WAV_PIO=20
        # Debug > UART console: TX-only on GP20 (UART1). The board header (pico2.h)
        # ALWAYS defines PICO_DEFAULT_UART=0 on GP0/GP1 — which is the ZiFi UART on
        # PICO_DV — so the console must never use uart_default/stdio_init_all.
        # GP20 is also the WAV loader input, which yields at boot while the console is on.
        DBG_UART_TX_PIN=20

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
        MIDI_TX_PIN=20

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
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        CPU_MHZ=${CPU_MHZ}
        ZERO2=1

        # Debug > UART console: TX-only on GP0 (UART0, a FREE pin here). NOT GP20/21
        # as the old ZERO2_DBG_UART option had it: GP21 is PCM5122_I2S_DATA and UART1
        # is the instance of the default ZiFi pair (24/25) — both collided.
        DBG_UART_TX_PIN=0

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
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PORT_VERSION="${PORT_VERSION}" PORT_VERSION_LEN=${PORT_VERSION_LEN}
        CPU_MHZ=${CPU_MHZ}

        KBD_CLOCK_PIN=0
        KBD_DATA_PIN=1
        # Debug > UART console: TX-only on GP0 (UART0, the J6 Debug header). GP0 is the
        # PS/2 clock, so the keyboard moves to GP16/GP17 while the console is on — which
        # is NES_GPIO_DATA, so the NESPAD yields at boot (see nespad_begin in main.cpp).
        DBG_UART_TX_PIN=0
        DBG_UART_KBD_CLOCK_PIN=16

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

        # NES Gamepad — yields at RUNTIME while the UART console is on (KBD moves to
        # GP16/GP17, which is NES_GPIO_DATA); see the nespad_begin() guard in main.cpp.
        USE_NESPAD
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
