// ES8311 codec init for the Olimex RP2350pc — see es8311_init.h for the
// provenance (esp-bsp, Apache-2.0) and the fixed configuration (slave mode,
// MCLK from SCLK/BCLK, 16-bit I2S Philips, fs 31250 Hz).

#include <hardware/i2c.h>
#include <hardware/gpio.h>
#include <pico/time.h>
#include "es8311_init.h"

#define ES8311_I2C_ADDR 0x18 // CE pin pulled down

// ES8311 register addresses (Espressif es8311_reg.h naming)
#define ES8311_RESET_REG00      0x00 // reset digital, csm, clock manager etc.
#define ES8311_CLK_MANAGER_REG01 0x01 // select clk src for mclk, enable clocks
#define ES8311_CLK_MANAGER_REG02 0x02 // clk pre-divider and clk multiplier
#define ES8311_CLK_MANAGER_REG03 0x03 // adc fs_mode and osr
#define ES8311_CLK_MANAGER_REG04 0x04 // dac osr
#define ES8311_CLK_MANAGER_REG05 0x05 // clk divider for adc and dac
#define ES8311_CLK_MANAGER_REG06 0x06 // sclk (bclk) inverter and divider
#define ES8311_CLK_MANAGER_REG07 0x07 // tri-state, adc lrck divider
#define ES8311_CLK_MANAGER_REG08 0x08 // dac lrck divider
#define ES8311_SDPIN_REG09      0x09 // dac serial digital port (I2S in)
#define ES8311_SDPOUT_REG0A     0x0A // adc serial digital port (I2S out)
#define ES8311_SYSTEM_REG0D     0x0D // system, power up/down
#define ES8311_SYSTEM_REG0E     0x0E // system, power up/down
#define ES8311_SYSTEM_REG12     0x12 // system, enable DAC
#define ES8311_SYSTEM_REG13     0x13 // system, HP drive
#define ES8311_ADC_REG1C        0x1C // ADC equalizer, hpf (ADC unused here)
#define ES8311_DAC_REG31        0x31 // DAC mute
#define ES8311_DAC_REG32        0x32 // DAC volume
#define ES8311_DAC_REG37        0x37 // DAC ramp rate / equalizer

static i2c_inst_t *es8311_i2c = NULL;
static bool es8311_is_ready = false;
// First failing readback (for the caller's log)
uint8_t es8311_last_rb_reg = 0, es8311_last_rb_val = 0;

static i2c_inst_t *get_i2c_inst(uint sda_pin) {
    // Even GPIO pairs (0/1, 4/5, 8/9...) -> i2c0 (32/33 -> i2c0, as wired).
    return ((sda_pin >> 1) & 1) ? i2c1 : i2c0;
}

static bool es8311_write_reg(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = { reg, val };
    return i2c_write_blocking(es8311_i2c, ES8311_I2C_ADDR, buf, 2, false) == 2;
}

static bool es8311_read_reg(uint8_t reg, uint8_t *val) {
    // Repeated-start read: write the register address with nostop=true, then
    // read back in the same transaction.
    if (i2c_write_blocking(es8311_i2c, ES8311_I2C_ADDR, &reg, 1, true) != 1)
        return false;
    return i2c_read_blocking(es8311_i2c, ES8311_I2C_ADDR, val, 1, false) == 1;
}

// Two clean ACKs, not one — same rationale as pcm5122_detect(): a residual
// glitch on the shared DDC FET lines must not be misread as the codec.
static bool es8311_probe(void) {
    uint8_t rxdata;
    for (int i = 0; i < 2; ++i) {
        if (i2c_read_timeout_us(es8311_i2c, ES8311_I2C_ADDR, &rxdata, 1, false, 5000) < 0)
            return false;
    }
    return true;
}

static bool es8311_i2c_setup(uint sda_pin, uint scl_pin) {
    es8311_i2c = get_i2c_inst(sda_pin);
    i2c_init(es8311_i2c, 100000);
    // GPIO_FUNC_I2C is open-drain: safe next to the HDMI DDC FETs on GP32/33.
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    // Board carries 2.2k pull-ups; the internal ones only help at probe time.
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
    return true;
}

int es8311_init(uint sda_pin, uint scl_pin) {
    es8311_is_ready = false;
    if (!es8311_i2c) {
        es8311_i2c_setup(sda_pin, scl_pin);
    }

    if (!es8311_probe()) {
        return -1;
    }

    // ---- Reset (esp-bsp es8311_init) ----
    // 0x1F = reset the digital block / clock manager, then power-on 0x80.
    if (!es8311_write_reg(ES8311_RESET_REG00, 0x1F)) return -1;
    sleep_ms(20);
    if (!es8311_write_reg(ES8311_RESET_REG00, 0x00)) return -1;
    if (!es8311_write_reg(ES8311_RESET_REG00, 0x80)) return -1; // power-on

    // ---- Clocks: source BCLK (a.k.a. SCLK), no MCLK pin ----
    // REG01 = 0x3F (enable all codec clocks) | BIT7 = internal MCLK taken from
    // the SCLK/BCLK pin (esp-bsp: mclk_from_mclk_pin = false). mclk_int = BCLK
    // = 32 x fs; 16-bit in == 16-bit out is required for that mode.
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0xBF)) return -1;

    // Coefficient for mclk = BCLK, 16-bit, fs in the single-speed
    // 8-48 kHz range — same ratios as esp-bsp's table row {1536000, 48000}
    // (BCLK = 32 x 48000): pre_div=1, adc/dac divider=1, OSR=16.
    // Everything below is a RATIO to BCLK, so it is valid for any fs
    // the host PIO emits (31250 at 50 Hz, 31500 at 60 Hz), as long as
    // BCLK = 32 x LRCK (r6, 16-bit-in-32-BCLK frame) or 64 x LRCK
    // (r7 PICO_PC2_I2S_64FS, standard 64-BCLK frame) — the mclk multiplier
    // below is what keeps dig_mclk at 8 MHz = 256 x fs in both cases.
    #ifdef PICO_PC2_I2S_64FS
    // 64fs experiment (r7): BCLK = 2 MHz = 64 x fs; pre_div=1 (<<5),
    // pre_multi=0x02 (x4) -> dig_mclk = 2 MHz x 4 = 8 MHz = 256 x fs.
    // x8 (0x18) would give 16 MHz — over the ES8311 mclk limit.
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG02, (1 - 1) << 5 | 0x02 << 3)) return -1;
    #else
    // REG02: pre-divider 1 (<<5), pre_multi=0x03 (x8) -> dig_mclk = 8.000 MHz
    // = 256 x fs @ fs=31250 (BCLK = 1 MHz = 32 x fs). pre_multi 4x would give
    // only 128 x fs — below the DAC/LRCK ratio minimum of 256 in the ES8311
    // datasheet (mute symptom; esp-bsp row {1536000,48000} -> REG02 = 0x18).
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG02, (1 - 1) << 5 | 0x03 << 3)) return -1;
    #endif
    // REG03: fs_mode = single speed (<<6), ADC OSR = 16
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG03, 0x00 << 6 | 0x10)) return -1;
    // REG04: DAC OSR = 16
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG04, 0x10)) return -1;
    // REG05: ADC divider 1, DAC divider 1
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG05, (1 - 1) << 4 | (1 - 1))) return -1;
    // REG06: SCLK not inverted (bit5 = 0), BCLK divider = master-mode only;
    // in slave mode BCLK is an input, keep the esp-bsp table value (div 4).
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG06, 0x03)) return -1;
    // REG07/08: ADC/DAC LRCK divider — used for master-mode LRCK generation
    // only; in slave mode LRCK is an input. esp-bsp table values.
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG07, 0x00)) return -1;
    if (!es8311_write_reg(ES8311_CLK_MANAGER_REG08, 0xFF)) return -1;

    // ---- Serial port format: slave, I2S (Philips), 16/16 bit ----
    // REG00 bit6 = 0: serial port in slave mode (host PIO is the master).
    uint8_t reg00;
    if (!es8311_read_reg(ES8311_RESET_REG00, &reg00)) return -1;
    if (!es8311_write_reg(ES8311_RESET_REG00, reg00 & ~0x40)) return -1;
    // REG09/0A: bits [5:4] = 00 -> I2S Philips format; bits [3:2] = 3 -> 16-bit
    if (!es8311_write_reg(ES8311_SDPIN_REG09, 0x0C)) return -1;
    if (!es8311_write_reg(ES8311_SDPOUT_REG0A, 0x0C)) return -1;

    // ---- Power up the DAC side ----
    if (!es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01)) return -1; // power up analog circuitry
    if (!es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02)) return -1; // enable analog PGA + ADC modulator
    if (!es8311_write_reg(ES8311_SYSTEM_REG12, 0x00)) return -1; // power up DAC
    if (!es8311_write_reg(ES8311_SYSTEM_REG13, 0x10)) return -1; // enable output to HP drive
    if (!es8311_write_reg(ES8311_ADC_REG1C, 0x6A))    return -1; // ADC eq bypass, DC-offset cancel

    // ---- DAC un-mute, moderate volume, eq bypass ----
    // REG31: clear the soft-mute bits (5/6)
    uint8_t reg31;
    if (!es8311_read_reg(ES8311_DAC_REG31, &reg31)) return -1;
    if (!es8311_write_reg(ES8311_DAC_REG31, reg31 & ~0x60)) return -1;
    // REG32: DAC digital volume, 0x00 = -inf .. 0xFF = 0 dB (esp-bsp mapping:
    // volume% * 256 / 100). 0x66 ~ 40 %: moderate until the host volume lands.
    if (!es8311_write_reg(ES8311_DAC_REG32, 0x66)) return -1;
    // REG37: DAC ramp/eq — bypass equalizer (esp-bsp), default fade rate
    if (!es8311_write_reg(ES8311_DAC_REG37, 0x08)) return -1;

    // ---- Readback check: the registers that define the I2S interface ----
    static const struct { uint8_t reg; uint8_t want; } check_regs[] = {
        { ES8311_CLK_MANAGER_REG01, 0xBF }, // MCLK src = BCLK, clocks on
        #ifdef PICO_PC2_I2S_64FS
        { ES8311_CLK_MANAGER_REG02, 0x10 }, // pre-div 1, multi 4x (256 x fs @ BCLK 2 MHz)
        #else
        { ES8311_CLK_MANAGER_REG02, 0x18 }, // pre-div 1, multi 8x (256 x fs @ BCLK 1 MHz)
        #endif
        { ES8311_RESET_REG00,       0x80 }, // power on, serial port = SLAVE (bit6 = 0)
        { ES8311_SDPIN_REG09,       0x0C }, // I2S Philips, 16-bit in
        { ES8311_SDPOUT_REG0A,      0x0C }, // I2S Philips, 16-bit out
        { ES8311_SYSTEM_REG12,      0x00 }, // DAC powered up
        { ES8311_DAC_REG32,         0x66 }, // moderate volume
    };
    for (unsigned i = 0; i < sizeof(check_regs) / sizeof(check_regs[0]); ++i) {
        uint8_t rb;
        if (!es8311_read_reg(check_regs[i].reg, &rb)) return -1;
        if (rb != check_regs[i].want) {
            es8311_last_rb_reg = check_regs[i].reg;
            es8311_last_rb_val = rb;
            return -1;
        }
    }

    es8311_is_ready = true;
    return 0;
}

void es8311_set_dac_volume(uint8_t volume) {
    if (!es8311_i2c) return;
    // Firmware scale 0 (loudest) .. 16 (silent) -> REG32 0xFF (0 dB) .. 0x00.
    int reg = 255 - (int)volume * 16;
    if (reg < 0) reg = 0;
    es8311_write_reg(ES8311_DAC_REG32, (uint8_t)reg);
}

bool es8311_ready(void) {
    return es8311_is_ready;
}
