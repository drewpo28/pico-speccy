#pragma once

// On-chip Bluetooth (PICOSPECCY_BT: the CYW43439 of a W/WS board) through BTstack.
//
// Stage 1 — discovery: classic inquiry + LE scan side by side, every answer logged
// and kept in a small device table (name, address, kind).
// Stage 2 — audio out: an A2DP SOURCE. The final mix of pwm_audio's 31250 Hz tick
// (core1) goes into a lock-free ring; core0 resamples it to the rate the sink
// picked (44.1/48 kHz), encodes SBC and sends it. The local output keeps playing.
//
// Threading: everything here runs on core0 inside cyw43_arch_poll() (WifiNet::poll)
// or pump(), the same poll-mode loop lwIP uses. Compiled out when PICOSPECCY_BT=0.

#if PICOSPECCY_BT

#include <stdint.h>

namespace BtHost {

typedef void (*LogCb)(const char* line);
void setLog(LogCb cb);

// ── Discovery ────────────────────────────────────────────────────────────────
// Power the controller up (BT firmware download on first use, ~1 s) and start a
// discovery of `seconds`. `classicOnly` skips the LE half (the headphone list).
// False when the radio is not there or HCI did not come up; the reason is logged.
bool scanBegin(int seconds, bool classicOnly = false);
bool scanBusy();
void scanStop();
int  found();

struct Dev {
    uint8_t  addr[6];
    char     name[32];
    uint32_t cod;       // classic Class of Device, 0 for LE
    bool     le;
    int8_t   rssi;
    uint8_t  addrType;  // LE: public / random (bd_addr_type_t)
    bool     hid;       // LE: advertises the HID service or a HID appearance
};
int        devCount();
const Dev* dev(int i);
// Classic device whose CoD says audio (major class 0x04)?
bool isAudio(const Dev& d);
// Keyboard / mouse / joystick: classic CoD major class 0x05, or LE HID.
bool isHid(const Dev& d);

// ── Audio (A2DP source) ──────────────────────────────────────────────────────
enum AudioState : uint8_t { A_IDLE, A_CONNECTING, A_STREAMING, A_FAILED };
AudioState audioState();
const char* audioName();          // the device being / last connected
// Connect to `addr` and start streaming. Asynchronous: watch audioState().
bool audioConnect(const uint8_t addr[6], const char* name);
void audioDisconnect();
// The device remembered from the last successful connection ("" = none).
bool remembered(uint8_t addr[6], char* name, int cap);
// Forget a device: drop its pairing key (it has to pair again), and if it is the
// remembered one, forget that too. Disconnects it first if it is connected.
void forget(const uint8_t addr[6]);

// ── Input (HID host: classic hid_host + BLE HID-over-GATT) ────────────────────
// Reports go into the same funnels the USB devices use (hid_app.cpp): keyboard ->
// process_kbd_report, mouse -> mouse_apply, pad -> the joystick virtual keys. The
// layout comes from the device's own HID descriptor (btstack_hid_parser).
enum InputState : uint8_t { I_IDLE, I_CONNECTING, I_READY, I_FAILED };
InputState inputState();
const char* inputName();
bool inputConnect(const Dev& d);
void inputDisconnect();
// Remembered input device ("" = none): it is reconnected by itself — a classic one
// when it pages us (keypress), an LE one as soon as it advertises.
bool inputRemembered(uint8_t addr[6], char* name, int cap);

// Once per frame: powers the radio up after boot when an input device is
// remembered, so a paired keyboard works without opening the menu.
void bootTick();

// Pump the radio while streaming (frame-pacing waits): cheap and rate-limited.
bool streaming();
void pump();

// Hardware Info: "off" / "starting" / "up" / "failed".
const char* stateStr();

}  // namespace BtHost

#endif  // PICOSPECCY_BT
