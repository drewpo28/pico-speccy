#pragma once

// Board-specific GPIO assignment helpers, RP2350 only.
//
// Centralises (a) the authoritative RP2350 UART pinmux (which GPIO can be TX/RX
// of which UART instance) and (b) the per-board lists of *usable* UART TX/RX
// pairs offered to the user (e.g. the ZiFi pin picker). Kept separate from any
// one feature so it can be reused (MIDI TX, debug UART, …).
//
// Pin-selection convention shared by callers/Config:
//   0xFE = "use board default"   0xFF = OFF (no pins)   else = explicit TX pin.


#include <inttypes.h>

namespace BoardPins {

// A selectable UART pair. note = what gets displaced if chosen ("" = free).
struct UartPair { uint8_t tx; uint8_t rx; const char* note; };

constexpr uint8_t PIN_DEFAULT = 0xFE; // sentinel: use the board default
constexpr uint8_t PIN_OFF     = 0xFF; // sentinel: disabled / no pins

// Upper bound on zifiPairCount() across all boards — lets callers size static
// tables (the transport radio's runtime option list).
constexpr int ZIFI_MAX_PAIRS = 12;

// Authoritative RP2350 pinmux. Returns the UART instance (0 or 1) that the
// given *even* TX pin belongs to, or -1 if the pin can't be a UART TX.
int uartInstanceForTx(uint8_t tx);

// Candidate ZiFi UART TX/RX pairs for the board this firmware is built for.
// Index 0 is the board default. count==0 means the board has no ZiFi support.
int             zifiPairCount();
const UartPair* zifiPair(int index);     // nullptr if out of range
uint8_t         zifiDefaultTx();
uint8_t         zifiDefaultRx();

// Resolve a stored Config value (PIN_DEFAULT/PIN_OFF/explicit) to the actual
// pins to program. Returns false when OFF (out_tx/out_rx left = PIN_OFF).
bool resolveZifiPins(uint8_t cfg_tx, uint8_t cfg_rx, uint8_t& out_tx, uint8_t& out_rx);

// True when the ZiFi NIC is enabled AND its active UART claims `pin`. Conflicting
// peripherals (NESPAD/MIDI/WAV/PCM/AY) call this at boot to yield the pin to ZiFi
// (skip their own init). A pin change to a conflicting pair triggers a reboot so
// this re-evaluates cleanly. Reads Config — safe to call after Config::load().
bool zifiOwnsPin(uint8_t pin);

// Note string of the currently-selected pair ("" if free/OFF/unknown). Non-empty
// means the active pins displace another peripheral — callers prompt a reboot so
// the yield-at-boot guards take effect.
const char* zifiActiveNote();

// ── Debug > UART console (Config::dbg_uart) ──────────────────────────────────
// TX-only console on the board's DBG_UART_TX_PIN (CMake). Where that pin is the
// PS/2 clock (MURM1 GP0, PICO_PC GP0) the keyboard moves to DBG_UART_KBD_CLOCK_PIN
// while the console is on. Runtime since 2026-09-06 (the <BOARD>_DBG_UART build
// options are gone); see Debug::uart* for the transport itself.
uint8_t dbgUartTxPin();                 // PIN_OFF when the board defines none
int     dbgUartInstance();              // 0/1, -1 when none
uint8_t dbgUartKbdClockPin();           // relocated PS/2 clock, PIN_OFF if the console
                                        // does not touch the keyboard on this board
// True when the console is live or about to be (scratch tag at main() entry, Config
// after load) AND it displaces `pin`: the TX pin itself, or the pair the keyboard
// was moved onto. Same contract as zifiOwnsPin — conflicting peripherals (NESPAD,
// WAV loader, MIDI) call it at boot and skip their own init.
bool    dbgUartOwnsPin(uint8_t pin);
// Whether the console can run at all beside the configured ZiFi UART: ZiFi has
// priority (its pins were chosen by the user and the NIC/WiFi need them), so the
// console yields when ZiFi's GPIO UART uses the same instance, the console TX pin,
// or the relocated keyboard pair. Reads Config — call after Config::load().
bool    dbgUartBlockedByZifi();
// The ZiFi pin picker's twin for the note strings: what the console displaces on
// this board ("" if nothing).
const char* dbgUartNote();

} // namespace BoardPins

// PS/2 keyboard pin pair, implemented in main.cpp next to the driver instance.
// Boards that share the default pair with another peripheral define
// KBD_ALT_CLOCK_PIN/KBD_ALT_DATA_PIN (ZERO2: GP2/3 doubles as the PCM5122 DAC's
// control I2C, alt pair GP14/15). Everywhere else the setter is a no-op.
extern "C" void     board_kbd_set_alt_pins(bool alt);
extern "C" unsigned board_kbd_clock_pin(void);   // live CLOCK pin (DATA = +1)
// Debug > UART console: apply Config::dbg_uart (start/stop, ZiFi yield, scratch
// tag, PS/2 re-pin). main.cpp; called from ESPectrum::setup right after the
// framebuffer reservation, before any pin-yielding peripheral initialises.
extern "C" void     board_dbg_uart_apply(void);

