// You may use, distribute and modify this code under the
// terms of the GPLv2 license, which unfortunately won't be
// written for another century.
//
// SPDX-License-Identifier: GPL-2.0-or-later
//
#pragma once

#ifndef _PS2KBD_H
#define _PS2KBD_H

#include "tusb.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include <functional>


typedef struct {
  uint8_t code;
  bool release;
  uint8_t page;
} Ps2KbdAction_;


class Ps2Kbd_Mrmltr {
private:
  PIO _pio;                        // pio0 or pio1
  uint _sm;                        // pio state machine index
  uint _base_gpio;                 // CLOCK gpio (DATA is _base_gpio + 1)
  uint _offset = 0;                // where the patched program sits in PIO memory
  bool _started = false;           // init_gpio() has run at least once
  hid_keyboard_report_t _report;   // HID report structure
  Ps2KbdAction_ _actions[2];
  uint _action;
  bool _double;
  bool _overflow;
  
  std::function<void(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev)> _keyHandler;

  inline void clearActions() {
    _actions[0].page = 0;
    _actions[0].release = false;
    _actions[0].code = 0;
    _actions[1].page = 0;
    _actions[1].release = false;
    _actions[1].code = 0;
    _action = 0;
  }
  
  void __not_in_flash_func(handleHidKeyPress)(uint8_t hidKeyCode);
  void __not_in_flash_func(handleHidKeyRelease)(uint8_t hidKeyCode);
  
  void __not_in_flash_func(handleActions)();
  uint8_t __not_in_flash_func(hidCodePage0)(uint8_t ps2code);
  uint8_t __not_in_flash_func(hidCodePage1)(uint8_t ps2code);
  void clearHidKeys();
  
public:

  Ps2Kbd_Mrmltr(
    PIO pio,
    uint base_gpio,
    std::function<void(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev)> keyHandler);
  
  void init_gpio();                 // on the pair given to the constructor
  // Bring the keyboard up on `base_gpio` (CLOCK; DATA = base_gpio + 1). Safe to
  // call again while running: the SM is stopped, the program reloaded for the new
  // pin and the old pins released. Used to move off GP2/3 when a board wants them
  // for something else (ZERO2's PCM5122 control I2C).
  void init_gpio(uint base_gpio);
  uint clock_gpio() const { return _base_gpio; }
  uint data_gpio()  const { return _base_gpio + 1; }

  void __not_in_flash_func(tick)();
};

#endif