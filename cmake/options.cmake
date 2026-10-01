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
# ON = RAW (the old spelling); AUTO = TMDS on PICO_PC and MURM2/MURM2_W.
set(HDMI_HSTX AUTO CACHE STRING "HDMI back-end: AUTO / OFF / RAW / TMDS (ON = RAW)")
set_property(CACHE HDMI_HSTX PROPERTY STRINGS AUTO OFF RAW TMDS ON)
option(MURM "Murmulator 1.x + Pi Pico 2" OFF)
option(MURM2 "Murmulator 2.0 + Pi Pico 2" OFF)
option(PICO_PC "Olimex RP2040-PICO-PC + Pi Pico 2" OFF)
option(PICO_DV "Pimoroni Pico DV Demo Base + Pi Pico 2" OFF)
option(ZERO2 "Waveshare RP2350-PiZero" OFF)
# Waveshare RP2350B-Plus-W in a Murmulator socket. The module is an RP2350B with
# 16 MB flash, PSRAM pads on GPIO47 and a Raspberry Pi Radio Module 2 (CYW43439)
# on board, so these are the SAME carriers as MURM/MURM2 with a different module:
# each option turns its carrier on and then overrides the handful of pins that the
# module moves. See src/drivers/board/boards/picospeccy_rp2350b_w.h.
option(MURM_W "Murmulator 1.x + Waveshare RP2350B-Plus-W (on-chip CYW43439 WiFi)" OFF)
option(MURM2_W "Murmulator 2.0 + Waveshare RP2350B-Plus-W (on-chip CYW43439 WiFi)" OFF)
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
option(WD1793_IN_RAM "WD1793: keep the FDC step/read/write/_do code in SRAM (~6.8 KB). OFF since 2026-09-30: flash is fast enough now that the Z80 core is in SRAM" OFF)
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
