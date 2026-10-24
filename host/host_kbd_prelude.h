// Prelude for the host's copy of the USB-keyboard mapping (map_key, kbdExtraMapping,
// process_kbd_report), which host/CMakeLists.txt extracts from src/main.cpp at
// configure time so the key map has one source. SDL scancodes ARE USB HID usage ids,
// so the SDL front end builds HID boot reports and hands them to process_kbd_report.
#pragma once
#include <cstdint>
#include "drivers/input/keyboard.h"
#include "app/ESPectrum.h"
#include "app/Config.h"
#include "speccy/machines/TsConf/ZxEvoAvr.h"
#include "host_hid_keys.h"   // generated: the HID_KEY_* usage ids from TinyUSB's hid.h

typedef struct { uint8_t modifier; uint8_t reserved; uint8_t keycode[6]; } hid_keyboard_report_t;
typedef enum {
    KEYBOARD_MODIFIER_LEFTCTRL = 1 << 0, KEYBOARD_MODIFIER_LEFTSHIFT = 1 << 1,
    KEYBOARD_MODIFIER_LEFTALT = 1 << 2, KEYBOARD_MODIFIER_LEFTGUI = 1 << 3,
    KEYBOARD_MODIFIER_RIGHTCTRL = 1 << 4, KEYBOARD_MODIFIER_RIGHTSHIFT = 1 << 5,
    KEYBOARD_MODIFIER_RIGHTALT = 1 << 6, KEYBOARD_MODIFIER_RIGHTGUI = 1 << 7,
} hid_keyboard_modifier_bm_t;

struct input_bits_t { bool a: true; bool b: true; bool select: true; bool start: true;
                      bool right: true; bool left: true; bool up: true; bool down: true; };
extern input_bits_t gamepad2_bits;
void back2joy2(fabgl::VirtualKey virtualKey, bool down);
#define JPAD (Config::secondJoy == 3 ? back2joy2: joyPushData)
