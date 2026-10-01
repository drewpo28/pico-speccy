// pico-speccy — the joystick keyboard-mapping page of the new UI (see UiJoy.cpp).
#pragma once

#include <stdint.h>
#include "UiModel.h"


namespace nm {

// The spatial pad-mapping page: 14 cells laid out like a real pad, Enter assigns
// a ZX key to the focused control, Del clears it, JoyTest lights the physical
// buttons live. Replaces OSD::joyDialog while the new UI is up.
void joyMappingPage();

// Joystick > Profile (a K_PICK list): named type + map sets in joystick.cfg.
const Option* joyprof_rows(uint8_t& cnt);
void          joyprof_key(int32_t tag, uint8_t key);
const char*   joyprof_vlabel();
int32_t       joyprof_current();          // row of the live profile, -1 = none
void          joyProfilesSessionBegin();  // the list is cached per menu session
void          joyProfilesSessionEnd();

} // namespace nm

