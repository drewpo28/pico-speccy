/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// pico-speccy profile for the Waveshare RP2350B-Plus-W module.
//
// Same silicon as picospeccy_rp2350b.h (RP2350B, 48 GPIO) — this module really is
// a B package, so nothing about the package assumptions changes. It differs from a
// plain Pi Pico 2 in four ways that matter to the firmware:
//
//   1. A Raspberry Pi Radio Module 2 (RM2 = CYW43439) is on board. Its four host
//      pins are NOT the Pico 2 W defaults (23/24/25/29) but GPIO36-39 — see the
//      WIRELESS block below, read off the Waveshare schematic.
//   2. 16 MB of flash instead of 4 MB. That is what makes the ~230 KB CYW43
//      firmware blob affordable: with __gmx_rom_in_flash the gm_bank partition
//      takes 1.6875 MB off the top, leaving ~2.3 MB on a 4 MB board (which main
//      already overflowed once, see rp2350-memmap.ld) against ~14.3 MB here.
//   3. QSPI PSRAM pads with CS on GPIO47 — the same pin PICO_DV/ZERO2 already use
//      for butter PSRAM. The chip is NOT fitted from the factory; the board arms
//      in CMakeLists.txt set BUTTER_PSRAM_GPIO 47 and butter_psram_size() answers
//      0 until one is soldered on.
//   4. The user LED (LED2) is GPIO23, a real RP2350 GPIO — not GPIO25 as on a
//      Pico, and not a CYW43 GPIO. (LED1 is on the radio module's own GPIO0.)
//
// Header pinout is Pico-compatible for GPIO0..GPIO22, but the three positions a
// Pico exposes as GP26/27/28 carry GPIO40/41/42 here, and GPIO24-35/43-45 are on
// bottom pads that a Murmulator socket does not reach. The per-board pin arms in
// CMakeLists.txt carry that remap; nothing in this header does.

#ifndef _BOARDS_PICOSPECCY_RP2350B_W_H
#define _BOARDS_PICOSPECCY_RP2350B_W_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define PICOSPECCY_RP2350B
#define PICOSPECCY_RP2350B_W

// --- RP2350 VARIANT ---
#define PICO_RP2350A 0

// --- UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
// LED2, wired to a real RP2350 GPIO (GPIO25 is a free bottom pad on this module).
// Every consumer is guarded with `#if defined(PICO_DEFAULT_LED_PIN) && != 255`
// (Buffer.cpp, Debug.cpp, MemESP.cpp), so 255 would also be a valid choice.
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 23
#endif
// no PICO_DEFAULT_WS2812_PIN

// --- I2C ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 4
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 5
#endif

// --- SPI ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 18
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 19
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 16
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 17
#endif

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- PSRAM (solder pads, not fitted from the factory) ---
#ifndef PICO_PSRAM_CS_PIN
#define PICO_PSRAM_CS_PIN 47
#endif

// --- WIRELESS (Raspberry Pi Radio Module 2 / CYW43439) ---
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)
#define PICO_CYW43_SUPPORTED 1

// RM2 host pins, read off RP2350B-Plus-W.pdf (Waveshare schematic, sheet 1):
//   GPIO36 WL_ON  -> RM2 B5_WLON       GPIO37 WL_D -> A5_DI / A6_DO (via R20) / B3_IRQ
//   GPIO38 WL_CS  -> RM2 B2_CS         GPIO39 WL_CLK -> A3
// None of the four is a header pin or a bottom pad, so they are fixed for the board:
// static pins, exactly like pico2_w.h (CYW43_PIN_WL_DYNAMIC 0). Each stays
// overridable from the build (-DCYW43_DEFAULT_PIN_WL_CLOCK=nn) via the #ifndef.
#ifndef CYW43_PIN_WL_DYNAMIC
#define CYW43_PIN_WL_DYNAMIC 0
#endif
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 36u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 37u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN 37u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 37u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK 39u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS 38u
#endif

// LED1 hangs off the radio module's own GPIO0 (LED2 is PICO_DEFAULT_LED_PIN
// above). Reachable only once cyw43_arch_init() has succeeded, via
// cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, ...) — which makes it a useful
// "the radio is actually alive" indicator during bring-up.
#ifndef CYW43_WL_GPIO_COUNT
#define CYW43_WL_GPIO_COUNT 3
#endif
#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0
#endif

// NOTE: deliberately NO PICO_SMPS_MODE_PIN / PICO_VBUS_PIN / PICO_VSYS_PIN.
// Those are 23/24/29 on a Pico; here 23 is LED2 and 24/29 are ordinary pads (and
// 36-39 are RM2's WL_ON/WL_D/WL_CS/WL_CLK), so the Pico meanings do not carry over.

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
