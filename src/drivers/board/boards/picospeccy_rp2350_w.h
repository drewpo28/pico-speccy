/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// pico-speccy profile for the STOCK W module layout, built for the Pimoroni Pico
// Plus 2 W (MURM_W / MURM2_W / PICO_DV_W / PICO_PC_W, tag *p2w). The Raspberry Pi
// Pico 2 W shares the radio pins but NOT the flash size, and these images do not
// fit its 4 MB (see 3.):
//
//   1. The radio (CYW43439) sits on the SAME four GPIOs on both — WL_REG_ON 23,
//      WL_DATA 24 (DI/DO/IRQ), WL_CS 25, WL_CLK 29 — taken from the SDK's pico2_w.h
//      and pimoroni_pico_plus2_w_rp2350.h, which agree. All four are below GPIO32,
//      so the radio's PIO block stays at gpio_base 0 (WifiNet::init).
//   2. The user LED is on the radio (WL_GPIO0) on both, so there is no
//      PICO_DEFAULT_LED_PIN GPIO (the carrier arms pass 255).
//   3. Flash: 16 MB on the Pico Plus 2 W, 4 MB on the Pico 2 W — two images, picked
//      by FLASH_16MB (-16MB / -4MB in the file name). The 4 MB one is built without
//      the linker's hard GM.DLS floor (GM_BANK_HARD_MIN_KB=0): the CYW43 blob + lwIP
//      (~290 KB) put it ~94 KB under that floor (measured 2026-10-12), so a Pico 2 W
//      gets only the flash tail for a GM.DLS bank and refuses one that does not fit.
//   4. Package: the Pico Plus 2 W is an RP2350B (8 MB PSRAM on GPIO47), the Pico 2 W
//      an RP2350A (no PSRAM). Like picospeccy_rp2350b.h this header claims the B
//      package (PICO_RP2350A 0) and the firmware checks the real package at runtime
//      (ChipPackage.h, chip_is_rp2350a()) — the plain p2 images already run on
//      RP2350A Pico 2 boards that way.
//
// The 40-pin header is the Pico's, so GP26/27/28 stay where the carriers expect
// them; only GP23/24/25/29 belong to the module and no carrier uses them.

#ifndef _BOARDS_PICOSPECCY_RP2350_W_H
#define _BOARDS_PICOSPECCY_RP2350_W_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define PICOSPECCY_RP2350B
#define PICOSPECCY_RP2350_W

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
// no PICO_DEFAULT_LED_PIN - the LED is on the wireless chip (CYW43_WL_GPIO_LED_PIN)
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

// 16 MB: the Pimoroni Pico Plus 2 W (see 3. above).
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// NOTE: deliberately NO PICO_SMPS_MODE_PIN / PICO_VBUS_PIN / PICO_VSYS_PIN: 23/24/29
// are the radio's here, VBUS is the radio's GPIO2 and VSYS is 29 on one module and
// 43 on the other. Nothing in the firmware reads them.

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

// --- WIRELESS (CYW43439) ---
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)
#define PICO_CYW43_SUPPORTED 1

#ifndef CYW43_PIN_WL_DYNAMIC
#define CYW43_PIN_WL_DYNAMIC 0
#endif
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 23u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 24u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN 24u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 24u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK 29u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS 25u
#endif

#ifndef CYW43_WL_GPIO_COUNT
#define CYW43_WL_GPIO_COUNT 3
#endif
#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0
#endif
// VBUS sense is the radio's GPIO2 on both modules.
#ifndef CYW43_WL_GPIO_VBUS_PIN
#define CYW43_WL_GPIO_VBUS_PIN 2
#endif

#endif
