# Build options: boards, displays and feature switches (debug/trace toggles are
# in cmake/trace.cmake). Included by CMakeLists.txt before anything reads them.

option(VGA_HDMI "Enable VGA/HDMI driver" OFF)
option(TFT "Enable TFT display" OFF)
option(ILI9341 "Enable TFT ILI9341 display" OFF)
option(TV "Enable TV composite output" OFF)
option(SOFTTV "Enable TV soft composite output" OFF)

# HDMI back-end. AUTO = on for the boards whose display sits on GPIO 12-19, which is
# where the RP2350's HSTX serializer lives — one firmware per board, exactly like
# quakegeneric's `IF(MURM2) if (VGA_HDMI) set(DVI_HSTX ON)`, not a second image.
# ON/OFF force it, and OFF is the A/B escape hatch back to the PIO TMDS program.
# A tri-state rather than option(): with option() an explicit -DHDMI_HSTX=OFF is
# indistinguishable from the default and the board arm in cmake/display.cmake would override it.
# Values: OFF = the PIO TMDS program; RAW = HSTX as a plain serializer fed our own
# TMDS words through the PIO address converter (HDMI_HSTX=1); TMDS = HSTX with the
# command expander and the hardware TMDS encoder, the line as a command list of
# XRGB8888 pixels and no converter at all (HDMI_HSTX=2, see hdmi_tmds_line.h);
# ON = RAW (the old spelling); AUTO = TMDS on PICO_PC and MURM2 (their W/WS variants included).
set(HDMI_HSTX AUTO CACHE STRING "HDMI back-end: AUTO / OFF / RAW / TMDS (ON = RAW)")
set_property(CACHE HDMI_HSTX PROPERTY STRINGS AUTO OFF RAW TMDS ON)
option(MURM "Murmulator 1.x + Pi Pico 2" OFF)
option(MURM2 "Murmulator 2.0 + Pi Pico 2" OFF)
option(PICO_PC "Olimex RP2040-PICO-PC + Pi Pico 2" OFF)
option(PICO_DV "Pimoroni Pico DV Demo Base + Pi Pico 2" OFF)
option(ZERO2 "Waveshare RP2350-PiZero" OFF)
# W modules: the SAME carriers with a WiFi module plugged in. Each option turns its
# carrier on and then overrides the handful of pins the module moves
# (cmake/board-pins.cmake). There are two module layouts, and they need separate images:
#
#   *_W  (tag *p2w)  — Pimoroni Pico Plus 2 W: the stock Pico 2 W layout, radio
#        (CYW43439) on GP23/24/25/29, LED on the radio, audio stays on GP26/27/28,
#        16 MB flash, 8 MB PSRAM on GPIO47. A plain Raspberry Pi Pico 2 W has the same
#        pins but only 4 MB of flash, which these images do not fit.
#        See src/drivers/board/boards/picospeccy_rp2350_w.h.
#   *_WS (tag *p2ws) — Waveshare RP2350B-Plus-W: radio on GPIO36-39, LED on GPIO23,
#        the Pico GP26/27/28 header positions carry GPIO40/41/42, 16 MB flash, PSRAM
#        pads on GPIO47. See src/drivers/board/boards/picospeccy_rp2350b_ws.h.
option(MURM_W "Murmulator 1.x + Pimoroni Pico Plus 2 W (on-chip CYW43439 WiFi)" OFF)
option(MURM2_W "Murmulator 2.0 + Pimoroni Pico Plus 2 W (on-chip CYW43439 WiFi)" OFF)
option(PICO_DV_W "Pimoroni Pico DV + Pimoroni Pico Plus 2 W (on-chip CYW43439 WiFi)" OFF)
option(PICO_PC_W "Olimex PICO-PC + Pimoroni Pico Plus 2 W (on-chip CYW43439 WiFi)" OFF)
option(MURM_WS "Murmulator 1.x + Waveshare RP2350B-Plus-W (on-chip CYW43439 WiFi)" OFF)
option(MURM2_WS "Murmulator 2.0 + Waveshare RP2350B-Plus-W (on-chip CYW43439 WiFi)" OFF)
# The Waveshare module in the Pimoroni Pico DV Demo Base: the base keeps its own layout
# (SD on SPI1, keyboard GP14/15, ZiFi header GP0/1, display base 6) but the audio block
# moves to GPIO40/41/42, exactly as on MURM_WS.
option(PICO_DV_WS "Pimoroni Pico DV + Waveshare RP2350B-Plus-W (on-chip CYW43439 WiFi)" OFF)
# And in the Olimex RP2040-PICO-PC (PCp2ws): the display stays on GPIO 12-19, so the
# HSTX back-ends work exactly as on PICO_PC; the module moves the GP26/27/28 audio
# block (PWM, beeper/595 latch, MIDI TX) to GPIO40/41/42 and the LED to GPIO23.
option(PICO_PC_WS "Olimex PICO-PC + Waveshare RP2350B-Plus-W (on-chip CYW43439 WiFi)" OFF)
# Flash size of the module: two firmware types per board. OFF = the 4 MB image every
# Pico 2 / RP2350B board takes; ON = a 16 MB image ("-16MB" in the file name) for a
# 16 MB module (Pimoroni Pico Plus 2, Waveshare RP2350B-Plus...). The 16 MB image adds
# what does not fit 4 MB: the ZXM-MoonSound YRW801 ROM embedded (YRW801_BUILTIN=1; the
# 4 MB image reads /pico-speccy/roms/yrw801.rom from SD instead), the built-in GM.DLS
# bank, and the GM.DLS bank region grows to ~14 MB by itself (rp2350-memmap.ld). The W/WS
# boards are 16 MB only and turn this on themselves (cmake/board.cmake).
option(FLASH_16MB "Build for a 16 MB flash module (built-in YRW801 + GM.DLS)" OFF)
# OFF by default and shipped as a SEPARATE z0p2 firmware ("-PIOUSB" in the file name):
# Pico-PIO-USB costs 17.7 KB of SRAM that cannot be reclaimed at runtime — 11.0 KB of
# .time_critical code (the bit-banged host has to run from RAM) plus 6.7 KB of .bss,
# mostly its 32-entry endpoint pool — which is nearly the whole 20 KB by which z0p2's
# free heap trailed DVp2's (measured from the map files, 2026-08-13). Boards that do
# not use the second Type-C as a host should not pay it, so the default build does not.
option(ZERO2_PIO_USB "ZERO2: add a USB host on the second Type-C (J2, PIO-USB GP28/GP29) — costs ~18 KB SRAM" OFF)
option(FT812_RENDER_IN_RAM "TS-Conf VDAC2: put the FT812 rasterizer's per-pixel path in SRAM (~14 KB) instead of flash" OFF)
option(ZIFI_NET_CLIENT "Enable FTP/SFTP/SSH client over ZiFi (uses mbedTLS)" ON)

# HDMI signal-integrity knobs (src/drivers/hdmi/hdmi.c). Both ON is the combination
# hw-confirmed on a capture card (2026-08-06); turning either off cannot break the
# stream, so a marginal link can be A/B'd by rebuild. A third knob, a ±2-code level
# snap, was tried and removed — it broke the new UI's paint on hardware for reasons
# that were never explained; see the "WHAT WAS TRIED AND REMOVED" note in hdmi.c
# before considering it again.
option(HDMI_TMDS_LEVEL_CLAMP "HDMI: keep channel levels inside [0x08..0xF6] (costs ~3% black, ~4% white)" ON)
option(HDMI_SOFT_CLK "HDMI: BUILD DEFAULT for Video > HDMI > Clock drive — ON = Soft (8 mA + slow slew), OFF = Normal (12 mA + fast). The user setting (Config::hdmi_clock_drive) overrides it." OFF)

# TS-Conf DRAM model (TsConf.cpp): 14 MHz wait states per cache miss, CPU and
# video accesses stealing DRAM cycles from a running DMA, blit = 3 cycles/word.
# OFF = the flat per-word DMA cost and a wait-free 14 MHz Z80 (pre-2026-09-13).
option(TSCONF_DRAM_MODEL "TS-Conf: model DRAM contention (CPU/video vs DMA) and 14 MHz wait states" ON)


# TS-Conf whole-line renderer on core1 (Video.cpp tsRenderCore1Pump): core0 posts
# one job per content line, core1's render loop executes it. OFF = render on core0.
# The whole-line renderer's GENERIC path — tsRenderExec (7.4 KB) + tsuComposeLine
# (1.5 KB + a 1.3 KB outlined lambda) — lived in FLASH until 2026-09-08, while
# only the TSU-free fast paths (tsFast256/tsFast16) were RAM-resident. On a TSU
# title that generic path IS the frame: it runs on core1 and fetches its own code
# through the XIP cache that its own tile reads (84 PSRAM line fills per line)
# are thrashing — flash and butter PSRAM share one cache and one port. Same class
# as the two biggest wins in this project's history (the flash memcpy in the HDMI
# line ISR, the OPL3 tables). ~10 KB of SRAM: turn it OFF first on a board that
# runs out of contiguous heap at VIDEO::Init.
# TS_RENDER_CORE1 is GONE: it never compiled anything out — it only set the
# initial value of the runtime flag ts_c1_enabled, which tsC1PlacementPoll()
# already drives (core0 sync while a NeoGS is playing, core1 queue otherwise).
# The queue is started lazily and only on TS-Conf, so there was nothing to gate.
# Which ZX page blocks leave SRAM for butter PSRAM on butter boards, their SRAM going
# to the framebuffer (ESPectrum.cpp donateZxPagesToFb). Block-granular: 0x0F = pages
# 0-3, 0x50 = pages 4/6. 0x5F = the 2026-10-13 default; 0x50 keeps 0-3 (the usual
# game code/data pages) in SRAM; 0x00 = the pre-v1.0.10 layout (FB on the heap).
# ZX Spectrum Next: 8K SRAM frames of the hot-page pool (MemESP.cpp, nextHotTick):
# pages the CPU keeps mapped are copied from butter PSRAM into SRAM. 8 = 4 x 16K;
# 0 = off. Taken from the heap ~3 s into the Next session (after the renderer's
# own blocks) and grown later while the heap has 8K to spare above a 16K reserve.
# Default 0 (owner, 2026-10-16): Adventures in Videoland, the case it was turned
# on for, gained ~1 frame — not worth the SRAM it takes from the Next session.
set(NEXT_HOT_FRAMES "1" CACHE STRING "Next: SRAM frames (8K) in the hot-page pool, 0 = off, max 8")
set(ZX_PSRAM_PAGES "0x5F" CACHE STRING "ZX pages moved to butter PSRAM, block mask (0x0F pages 0-3, 0x50 pages 4/6)")
option(WD1793_IN_RAM "WD1793: keep the FDC step/read/write/_do code in SRAM (~6.8 KB). OFF since 2026-09-30: flash is fast enough now that the Z80 core is in SRAM" OFF)
option(DIVMMC_HOT_IN_RAM "DivMMC/DivSD: keep the SD SPI read path (port 0xEB, mmc_read) in SRAM (~0.9 KB; every IN (EB) otherwise fetches flash code through the XIP cache the Next's butter RAM keeps busy)" ON)
option(TSCONF_HOT_IN_RAM "TS-Conf: keep the #nnAF port / DMA / INT-source code in SRAM (~4 KB; ~0.8 ms/frame on port-heavy titles)" ON)
option(TSCONF_RENDER_IN_RAM "TS-Conf: keep the whole-line renderer (tsRenderExec + tsuComposeLine) in SRAM (~10 KB)" ON)

# Debug UART console is a RUNTIME setting now (Debug > UART console, Config::dbg_uart),
# not a build option: the per-board <BOARD>_DBG_UART toggles were removed 2026-09-06.
# Each board block in cmake/board-pins.cmake defines DBG_UART_TX_PIN (TX-only — nothing ever reads the
# console) and, where that pin belongs to the PS/2 port, DBG_UART_KBD_CLOCK_PIN = the
# pair the keyboard moves to while the console is on. See Debug::uart* + BoardPins.

# set(TFT ON)
# set(TFT_INV ON)
# set(ILI9341 ON)
# set(ST7789 ON)
# set(SOFTTV ON)

# Display output (default: VGA_HDMI; override with -D flags, e.g. cmake -DSOFTTV=ON)
if(NOT VGA_HDMI AND NOT SOFTTV AND NOT TFT AND NOT ILI9341 AND NOT TV)
    set(VGA_HDMI ON)
endif()

# Framebuffer chunking exercise (Video.cpp ensureMainFB): force the main
# framebuffer into N whole-row chunks even when one block would fit, so the
# fragmented-heap path runs on an ordinary boot. 0 = normal (one block first).
set(FB_FORCE_CHUNKS "0" CACHE STRING "Debug: split the main framebuffer into N whole-row chunks (0 = off, 2/4/8)")
