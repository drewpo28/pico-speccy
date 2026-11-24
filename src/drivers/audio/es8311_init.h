// ES8311 low-power stereo audio CODEC — bare-metal init for the Olimex RP2350pc.
//
// Register map and slave-mode init sequence ported from Espressif's esp-bsp
// ES8311 driver (components/es8311/es8311.c + priv_include/es8311_reg.h):
//   SPDX-License-Identifier: Apache-2.0
//   Copyright 2015-2026 Espressif Systems (Shanghai) CO LTD
// The Pico SDK I2C plumbing around it follows this project's pcm5122_init.c.
//
// Configuration baked in (matches the PICO_PC2 wiring, cmake/board-pins.cmake):
//   - host is I2S MASTER (PIO); the codec runs in SLAVE serial-port mode
//   - NO MCLK pin: the codec derives its internal clock from SCLK/BCLK
//     (32 x fs, 16-bit stereo). GPOUT1/GP23 is never driven — DVI stays intact.
//   - BCLK:LRCK ratio 32 is ratio-configured, so any fs works (31250, 31500...).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <hardware/gpio.h>

// Full ES8311 initialization via I2C (addr 0x18): probe (2x ACK), soft reset,
// clocks from BCLK, 16-bit I2S Philips, DAC powered up and un-muted, moderate
// DAC volume. Returns 0 on success, -1 if the codec does not ACK the probe,
// -2 if the post-init register readback mismatches (logged by the caller).
int es8311_init(uint sda_pin, uint scl_pin);

// DAC digital volume (register 0x32). volume follows the firmware scale used
// by i2s_volume(): 0 = loudest (~0 dB), 16 = silent (register 0x00). No-op
// until es8311_init() has succeeded.
void es8311_set_dac_volume(uint8_t volume);

// True once es8311_init() completed the register readback successfully.
bool es8311_ready(void);

// First failing readback after a non-zero es8311_init() return: register
// address and the value read (for the caller's failure log). Zero/zero on the
// happy path.
extern uint8_t es8311_last_rb_reg, es8311_last_rb_val;

#ifdef __cplusplus
}
#endif
