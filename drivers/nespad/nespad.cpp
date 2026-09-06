#include <cstdio>
#include "hardware/pio.h"

#define nespad_wrap_target 0
#define nespad_wrap 6

static const uint16_t nespad_program_instructions[] = {
    //     .wrap_target
    0x80a0, //  0: pull   block  
    0xea01, //  1: set    pins, 1         side 0 [10]
    0xe02f, //  2: set    x, 15           side 0
    0xe000, //  3: set    pins, 0         side 0
    0x4402, //  4: in     pins, 2         side 0 [4]      <--- 2
    0xf500, //  5: set    pins, 0         side 1 [5]
    0x0044, //  6: jmp    x--, 4          side 0
            //     .wrap
};

static const struct pio_program nespad_program = {
    .instructions = nespad_program_instructions,
    .length =  7,
    .origin = -1,
};

static inline pio_sm_config nespad_program_get_default_config(uint offset) {
  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, offset + nespad_wrap_target, offset + nespad_wrap);
  sm_config_set_sideset(&c, 1, false, false);
  return c;
}

// Which PIO block the pad runs on. pio1 everywhere by default (beside the PS/2
// keyboard and I2S); MURM2_W moves it to pio0 because that board's pad data pins
// land on GPIO40/41 and pio1 is pinned to gpio_base 0 by the keyboard on GP2/3.
#ifndef NESPAD_PIO
#define NESPAD_PIO pio1
#endif

static PIO pio = NESPAD_PIO;
static uint8_t sm = -1;
uint32_t nespad_state  = 0;  // Joystick 1
uint32_t nespad_state2 = 0;  // Joystick 2

bool nespad_begin(uint32_t cpu_khz, uint8_t clkPin, uint8_t dataPin,uint8_t latPin) {
  if (pio_can_add_program(pio, &nespad_program) &&
      ((sm = pio_claim_unused_sm(pio, true)) >= 0)) {
    uint offset = pio_add_program(pio, &nespad_program);
    pio_sm_config c = nespad_program_get_default_config(offset);

    // An RP2350 PIO block reaches 32 CONSECUTIVE GPIOs starting at its gpio_base
    // (0 or 16), so a pad wired above GPIO31 (MURM2_W: data on 40/41) needs the
    // window moved up. Pin numbers stay ABSOLUTE either way: with PICO_RP2350A 0
    // the SDK defaults PICO_PIO_USE_GPIO_BASE to 1, and its own note on
    // sm_config_ pin arguments says those helpers then "always take real pin
    // numbers in the full range" 0-47. Same shape as the ZERO2 display path in
    // hdmi_init() (drivers/hdmi/hdmi.c), which runs on hardware at GPIO32-39.
    uint8_t maxPin = clkPin;
    if (latPin      > maxPin) maxPin = latPin;
    if (dataPin + 1 > maxPin) maxPin = dataPin + 1;
    if (maxPin >= 32) pio_set_gpio_base(pio, 16);

    sm_config_set_sideset_pins(&c, clkPin);
    sm_config_set_in_pins(&c, dataPin);
    sm_config_set_set_pins(&c, latPin, 1);
    pio_gpio_init(pio, clkPin);
    pio_gpio_init(pio, dataPin);
    pio_gpio_init(pio, dataPin+1);  // +1 Pin for Joystick2
    pio_gpio_init(pio, latPin);
    gpio_set_pulls(dataPin, true, false); // Pull data high, 0xFF if unplugged
    gpio_set_pulls(dataPin+1, true, false); // Pull data high, 0xFF if unplugged for Joystick2

    // 64-bit masks: `1 << 40` is undefined behaviour on a 32-bit int, and these
    // pins really are above 31 on MURM2_W. hdmi.c hit the same trap on ZERO2 and
    // its comment says so. The 64-bit variants take absolute GPIO bit positions
    // and are correct for low pins too, so there is one path, not two.
    const uint64_t outMask = ((uint64_t)1 << clkPin) | ((uint64_t)1 << latPin);
    const uint64_t allMask = outMask | ((uint64_t)1 << dataPin)
                                     | ((uint64_t)1 << (dataPin + 1));
    pio_sm_set_pindirs_with_mask64(pio, sm, outMask, allMask);
    sm_config_set_in_shift(&c, true, true, 32); // R shift, autopush @ 8 bits (@ 16 bits for 2 Joystick)

    sm_config_set_clkdiv_int_frac(&c, cpu_khz / 1000, 0); // 1 MHz clock



    pio_sm_clear_fifos(pio, sm);

    // On RP2350 this can refuse the configuration (PICO_ERROR_BAD_ALIGNMENT) when
    // the pins straddle the gpio_base window — the one way the block choice above
    // can be wrong, and otherwise silent: the pad would just read 0xFF forever.
    int rc = pio_sm_init(pio, sm, offset, &c);
    if (rc) {
        printf("NESPAD: pio_sm_init failed rc=%d (clk=%u dat=%u lat=%u base=%u)\n",
               rc, (unsigned)clkPin, (unsigned)dataPin, (unsigned)latPin,
               (unsigned)pio_get_gpio_base(pio));
        pio_remove_program(pio, &nespad_program, offset);
        pio_sm_unclaim(pio, sm);
        return false;
    }
    pio_sm_set_enabled(pio, sm, true);
    pio->txf[sm]=0;
    return true; // Success
  }
  return false;
}



// nespad read. Ideally should be called ~100 uS after
// nespad_read_start(), but can be sooner (will block until ready), or later
// (will introduce latency). Sets value of global nespad_state variable, a
// bitmask of button/D-pad state (1 = pressed). 0x80=Right, 0x40=Left,
// 0x20=Down, 0x10=Up, 0x08=Start, 0x04=Select, 0x02=B, 0x01=A. Must first
// call nespad_begin() once to set up PIO. Result will be 0 if PIO failed to
// init (e.g. no free state machine).

void nespad_read()
{
  if (sm<0) return;
  if (pio_sm_is_rx_fifo_empty(pio, sm)) return;

  // Right-shift was used in sm config so bit order matches NES controller
  // bits used elsewhere in picones, but does require shifting down...
  uint32_t temp=pio->rxf[sm]^ 0xFFFFFFFF;
  pio->txf[sm]=0;
  nespad_state  = temp & 0x555555;         //  Joy1
  nespad_state2 = temp >> 1 & 0x555555;  //  Joy2
}


