#pragma once

// On-chip WiFi (MURM_W / MURM2_W: Raspberry Pi Radio Module 2 = CYW43439).
//
// Phase 1 scope: bring the radio's bus up and report what it claimed. There is
// no lwIP here yet and no TCP/IP of any kind — the socket and AT-modem layers
// (WifiSock, AtModem) land on top of this, and `ZiFiSock`/`ZiFiAT` keep being the
// public faces the app layer talks to.
//
// The whole implementation is compiled out when PICOSPECCY_WIFI is 0, so the
// other five boards pay nothing for this file existing: src/*.cpp is swept up by
// a GLOB in CMakeLists.txt, exactly like the `#if ZIFI_NET_CLIENT` sources.

#if PICOSPECCY_WIFI

#include <stdint.h>

namespace WifiNet {

// Bring up the CYW43 bus. Call ONCE, from the main loop's context, and only
// AFTER video (core1 graphics_init) and the keyboard/gamepad have claimed their
// PIO blocks — the SDK picks a block that can reach the radio's pins, and it can
// only land on the right one if the others are already fixed at gpio_base 0.
// Safe to call on a board without a radio: it is a no-op returning false.
// Returns true when the radio answered.
bool init();

// True once init() has succeeded.
bool ready();

// The user LED on the radio module itself (LED1). No-op until ready().
void ledSet(bool on);

// Log which PIO block / state machines and DMA channels are claimed right now.
// The interesting moment is immediately before and after init(): it is the only
// direct evidence that the radio landed on the block the board expects, and a
// permanent regression detector for the gpio_base story. `when` labels the line.
void logPioDmaClaims(const char* when);

}  // namespace WifiNet

#endif  // PICOSPECCY_WIFI
