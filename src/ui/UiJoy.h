// pico-speccy — the joystick keyboard-mapping page of the new UI (see UiJoy.cpp).
#pragma once


namespace nm {

// The spatial pad-mapping page: 14 cells laid out like a real pad, Enter assigns
// a ZX key to the focused control, Del clears it, JoyTest lights the physical
// buttons live. Replaces OSD::joyDialog while the new UI is up.
void joyMappingPage();

} // namespace nm

