#pragma once

// On-chip Bluetooth (PICOSPECCY_WIFI: the CYW43439 of a W/WS board) through BTstack.
//
// Stage 1 — discovery: classic inquiry + LE scan side by side, every answer logged
// and kept in a small device table (name, address, kind).
// Stage 2 — audio out: an A2DP SOURCE. The final mix of pwm_audio's 31250 Hz tick
// (core1) goes into a lock-free ring; core0 resamples it to the rate the sink
// picked (44.1/48 kHz), encodes SBC and sends it. The local output keeps playing.
//
// Threading: everything here runs on core0 inside cyw43_arch_poll() (WifiNet::poll)
// or pump(), the same poll-mode loop lwIP uses. Compiled out when PICOSPECCY_WIFI=0.

#if PICOSPECCY_WIFI

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

// ── Added devices ────────────────────────────────────────────────────────────
// Every device that connected once is remembered (most recent first), up to
// KNOWN_MAX per kind, in CONFIG_DIR/bt_audio.txt and bt_input.txt.
static const int KNOWN_MAX = 8;
enum : int { KIND_AUDIO = 0, KIND_INPUT = 1, KIND_TAPE = 2 };   // the three device lists
struct Known {
    uint8_t addr[6];
    char    name[32];
    bool    le;          // input only
    uint8_t addrType;    // input only (LE)
};
int  known(bool input, Known* out, int max);
bool isKnown(bool input, const uint8_t addr[6]);
// Is this the device the audio (input) side is connected / connecting to?
bool connectedTo(bool input, const uint8_t addr[6]);

// ── Audio (A2DP source) ──────────────────────────────────────────────────────
enum AudioState : uint8_t { A_IDLE, A_CONNECTING, A_STREAMING, A_FAILED };
AudioState audioState();
const char* audioName();          // the device being / last connected
// Connect to `addr` and start streaming. Asynchronous: watch audioState().
bool audioConnect(const uint8_t addr[6], const char* name);
void audioDisconnect();
// The most recently connected audio device (false = none added).
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
// What is known about the connected (or connecting) input device; nullptr when
// none. Battery and the strings come from BLE devices only (Battery Service,
// Device Information Service) and arrive a moment after the connection.
struct InputInfo {
    char     kind[12];       // "mouse", "keyboard", "gamepad", ... ("" = not read yet)
    int8_t   battery;        // percent, -1 = unknown
    char     maker[24];
    char     model[24];
    char     fw[16];
    uint16_t descLen;        // HID descriptor size
};
const InputInfo* inputInfo();
// The A2DP stream in use (false = not streaming).
struct AudioInfo { int fs, bitpool, blocks, subbands; bool joint; };
bool audioInfo(AudioInfo& a);

// ── Tape from a phone (A2DP SINK) ─────────────────────────────────────────────
// docs/bt-tape-plan.md. The board shows up as a Bluetooth speaker ("Pico-Speccy");
// a phone pairs and plays a tape's audio into it. Stages 1-2: the link, the SBC
// decoder and the edge stream (src/speccy/devices/tape/BtTapeEdges.h); nothing
// consumes the edges yet (stage 3), so they feed a half-pulse histogram in the log.
// Sink and source are exclusive: listening drops the headphones (their "audio on"
// marker stays, so they come back when listening stops), and audioConnect() stops
// listening. While NOT listening a phone that tries to play to us is disconnected.
// The sink advertises bitpool BT_TAPE_MIN_BITPOOL..53: below 41 a 0 dBFS tape
// signal breaks up in the codec (tools/sbc_tape_test.cpp).
static const int BT_TAPE_MIN_BITPOOL = 41;
enum TapeState : uint8_t { T_OFF, T_LISTEN, T_CONNECTED, T_STREAMING };
bool tapeListen(bool on);     // false when the radio did not come up
void tapePairing(bool on);    // discoverable (a pairing page is open) or not
// Listen and page a phone that paired before (CONFIG_DIR/bt_tape.txt). Async: the
// phone may also refuse and still connect to us when it plays.
bool tapeConnect(const uint8_t addr[6], const char* name);
bool tapeConnecting();
bool tapeConnectedTo(const uint8_t addr[6]);
int  tapeKnown(Known* out, int max);   // phones that played to us, most recent first
uint8_t tapePeakPct();                 // decoded level, % of full scale (0 = no stream)
TapeState tapeState();
const char* tapeName();       // the phone connected / last connected
struct TapeInfo {
    int      fs;              // negotiated SBC configuration
    uint8_t  bpMin, bpMax;
    uint8_t  blocks, subbands, mode;   // mode: 0 mono, 1 dual, 2 stereo, 3 joint
    uint8_t  frameBpMin, frameBpMax;   // bitpool actually seen in the frames (last 5 s)
    uint32_t pkts, frames, lost, bad;  // totals since the stream started
    uint8_t  peakPct;                  // decoded level, % of full scale
};
bool tapeInfo(TapeInfo& t);   // false = no stream configured
// The edge stream (core0 only: produced inside the radio poll, consumed by the
// tape player). Each edge: time in 1/256 sample units since the stream started,
// bit 0 = the level after it (BtTapeEdges.h). Attach to receive them; while
// detached they go to the log's histogram. Lost RTP packets are NOT filled in.
void     tapeEdgeAttach(bool on);
int      tapeEdgesAvail();
bool     tapeEdgePop(uint32_t& e);
uint32_t tapeSampleClockQ8();   // samples decoded so far, Q8 (the producer's "now")
int      tapeSampleRate();      // 44100 / 48000, 0 = no stream
uint32_t tapeEdgeDrops();       // edges lost to a full ring

// The most recently connected input device (false = none added). Every added one
// reconnects by itself — classic when it pages us (keypress), LE as soon as it
// advertises — one input device at a time.
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

#endif  // PICOSPECCY_WIFI
