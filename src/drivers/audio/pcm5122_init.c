#include <hardware/i2c.h>
#include <hardware/gpio.h>
#include <pico/time.h>
#include "pcm5122_init.h"

#define PCM5122_ADDR 0x4C

// PCM5122 register addresses (page 0)
#define REG_PAGE_SELECT     0
#define REG_RESET           1
#define REG_STANDBY         2
#define REG_MUTE            3
#define REG_PLL_EN          4
#define REG_PLL_REF        13
#define REG_DAC_REF        14
#define REG_CLK_ERR_DETECT 37
#define REG_I2S_FORMAT     40
#define REG_DVOL_L         61
#define REG_DVOL_R         62
#define REG_AUTO_MUTE      65

static i2c_inst_t *pcm5122_i2c = NULL;

static i2c_inst_t *get_i2c_inst(uint sda_pin) {
    // Even GPIO pairs (0/1, 4/5, 8/9...) -> i2c0
    // Odd GPIO pairs (2/3, 6/7, 10/11...) -> i2c1
    return ((sda_pin >> 1) & 1) ? i2c1 : i2c0;
}

static bool pcm5122_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_write_blocking(pcm5122_i2c, PCM5122_ADDR, buf, 2, false) == 2;
}

static bool pcm5122_i2c_setup(uint sda_pin, uint scl_pin) {
    pcm5122_i2c = get_i2c_inst(sda_pin);
    i2c_init(pcm5122_i2c, 100000);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
    return true;
}

static void pcm5122_i2c_teardown(uint sda_pin, uint scl_pin) {
    if (pcm5122_i2c) {
        i2c_deinit(pcm5122_i2c);
        pcm5122_i2c = NULL;
    }
    gpio_deinit(sda_pin);
    gpio_deinit(scl_pin);
}

// On ZERO2 these pins double as the PS/2 port, so what is actually on them may
// be a KEYBOARD, and at probe time it may be mid-scancode (the hotkey release
// that follows a watchdog reboot, or the BAT 0xAA after power-on). Wait for the
// lines to go quiet before driving I2C into it: probing over live keyboard
// traffic both strands the keyboard mid-command and lets its clocking be
// misread as bus activity. Bounded — a stuck-low line just falls through to
// the (failing) probe.
static void pcm5122_wait_quiet(uint sda_pin, uint scl_pin) {
    gpio_init(sda_pin);
    gpio_init(scl_pin);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
    busy_wait_us(10);   // pull-up settle before the first sample
    absolute_time_t deadline = make_timeout_time_us(20000);
    absolute_time_t quiet    = make_timeout_time_us(200);
    while (!time_reached(quiet)) {
        if (!gpio_get(sda_pin) || !gpio_get(scl_pin))
            quiet = make_timeout_time_us(200);
        if (time_reached(deadline))
            break;
        tight_loop_contents();
    }
}

bool pcm5122_detect(uint sda_pin, uint scl_pin) {
    pcm5122_wait_quiet(sda_pin, scl_pin);
    pcm5122_i2c_setup(sda_pin, scl_pin);

    // Two clean ACKs, not one: a false positive here moves the PS/2 keyboard to
    // the alternate pins for the whole session (dead keyboard on a DAC-less
    // board), and residual keyboard clocking could fake a single ACK. A real
    // DAC acknowledges its address deterministically.
    uint8_t rxdata;
    for (int i = 0; i < 2; ++i) {
        int ret = i2c_read_timeout_us(pcm5122_i2c, PCM5122_ADDR, &rxdata, 1, false, 5000);
        if (ret < 0) {
            pcm5122_i2c_teardown(sda_pin, scl_pin);
            return false;
        }
    }
    return true;
}

bool pcm5122_present(uint sda_pin, uint scl_pin) {
    static int cached = -1;             // -1 = not probed yet
    if (cached < 0) cached = pcm5122_detect(sda_pin, scl_pin) ? 1 : 0;
    return cached != 0;
}

void pcm5122_release(uint sda_pin, uint scl_pin) {
    // Only when the bus was actually brought up: the teardown deinits the pins,
    // and on ZERO2 they belong to the PS/2 keyboard whenever the DAC is unused.
    if (pcm5122_i2c) pcm5122_i2c_teardown(sda_pin, scl_pin);
}

bool pcm5122_init(uint sda_pin, uint scl_pin) {
    // If detect() wasn't called first, set up I2C
    if (!pcm5122_i2c) {
        pcm5122_i2c_setup(sda_pin, scl_pin);
    }

    // Verify chip is present
    uint8_t rxdata;
    int ret = i2c_read_timeout_us(pcm5122_i2c, PCM5122_ADDR, &rxdata, 1, false, 5000);
    if (ret < 0) {
        pcm5122_i2c_teardown(sda_pin, scl_pin);
        return false;
    }

    // Select page 0
    pcm5122_write_reg(REG_PAGE_SELECT, 0x00);

    // Reset the chip (same as working MicroPython code: reg 1 = 0x11)
    pcm5122_write_reg(REG_RESET, 0x11);
    sleep_ms(100);

    // PLL reference clock = BCK (3-wire mode, no SCK)
    pcm5122_write_reg(REG_PLL_REF, 0x10);

    // Ignore SCK errors (no SCK connected in 3-wire mode)
    pcm5122_write_reg(REG_CLK_ERR_DETECT, 0x08);

    // Wake up (exit standby)
    pcm5122_write_reg(REG_STANDBY, 0x00);

    // Unmute
    pcm5122_write_reg(REG_MUTE, 0x00);

    // Volume: 0x30 = 0dB
    pcm5122_write_reg(REG_DVOL_L, 0x30);
    pcm5122_write_reg(REG_DVOL_R, 0x30);

    return true;
}
