/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021, Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"
#include "Debug.h"
#include "Config.h"
#include "ESPectrum.h"
#include "hid_rip.h"
#include "pico/time.h"
#if defined(ZERO2_PIO_USB_HOST)
#include "pio_usb.h"   // pio_usb_host_ep_debug() for the keyboard health line
#endif   // time_us_32() for the stuck-key resync timer

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

// If your host terminal support ansi escape code such as TeraTerm
// it can be use to simulate mouse cursor movement within terminal
#define USE_ANSI_ESCAPE   0

#define MAX_REPORT  4

static uint8_t const keycode2ascii[128][2] =  { HID_KEYCODE_TO_ASCII };

// Each HID instance can has multiple reports
static struct
{
  uint8_t report_count;
  tuh_hid_report_info_t report_info[MAX_REPORT];
  uint16_t vid;
  uint16_t pid;
}hid_info[CFG_TUH_HID];

#define HID_SNAP_DESC_BYTES   48
#define HID_SNAP_REPORT_BYTES 32

// Handler markers — what code path actually got the report.
enum hid_handler_t : uint8_t {
  HID_HANDLER_NONE = 0,        // no report received yet
  HID_HANDLER_KBD,             // boot keyboard
  HID_HANDLER_MOUSE,           // boot mouse
  HID_HANDLER_DS4,             // Sony DualShock 4 vendor handler
  HID_HANDLER_DS5,             // Sony DualSense vendor handler
  HID_HANDLER_F710_DINPUT,     // Logitech F710 D-input vendor handler
  HID_HANDLER_GP_2563_0575,    // 2563:0575 vendor handler
  HID_HANDLER_GP_FEED_2320,    // FEED:2320 vendor handler
  HID_HANDLER_GP_0810_0001,    // 0810:0001 DragonRise/GameStick wireless
  HID_HANDLER_GENERIC_GAMEPAD, // tinyusb-parsed Joystick/Gamepad
  HID_HANDLER_GENERIC_CONS,    // Consumer control
  HID_HANDLER_NO_RPT_INFO,     // composite report id not found
  HID_HANDLER_UNKNOWN_USAGE,   // parsed report info but page/usage not handled
};

// Decoded gamepad state bits (set by vendor handlers, read by OSD dialog).
#define HID_DEC_UP     0x0001
#define HID_DEC_DOWN   0x0002
#define HID_DEC_LEFT   0x0004
#define HID_DEC_RIGHT  0x0008
#define HID_DEC_A      0x0010
#define HID_DEC_B      0x0020
#define HID_DEC_X      0x0040
#define HID_DEC_Y      0x0080
#define HID_DEC_L      0x0100
#define HID_DEC_R      0x0200
#define HID_DEC_START  0x0400
#define HID_DEC_SELECT 0x0800

struct hid_snap_t {
  bool     mounted;
  uint8_t  dev_addr;
  uint8_t  itf_protocol;
  uint16_t vid;
  uint16_t pid;
  uint16_t desc_len;
  uint8_t  desc_saved;
  uint8_t  desc[HID_SNAP_DESC_BYTES];
  uint16_t last_report_len;
  uint8_t  last_report_saved;
  uint8_t  last_report[HID_SNAP_REPORT_BYTES];
  uint32_t report_total;
  uint8_t  last_handler;     // hid_handler_t
  uint16_t decoded;          // HID_DEC_* bitmask, only valid for vendor handlers
  bool     decoded_valid;
};
static hid_snap_t hid_snap[CFG_TUH_HID];

// PICOSPECCY_ZERO2_PIO_USB_HOST_V1: one-shot latch for the "no report info" diagnostic (see
// process_generic_report); cleared on mount/unmount so a re-plug logs again.
static bool no_rpt_info_logged[CFG_TUH_HID];

// Helper for vendor handlers to publish their decoded state to the OSD.
static inline void hid_snap_set_handler(uint8_t instance, uint8_t h) {
  if (instance < CFG_TUH_HID) hid_snap[instance].last_handler = h;
}
static inline void hid_snap_set_decoded(uint8_t instance, uint8_t h, uint16_t bits) {
  if (instance < CFG_TUH_HID) {
    hid_snap[instance].last_handler = h;
    hid_snap[instance].decoded = bits;
    hid_snap[instance].decoded_valid = true;
  }
}

// True once the USB host has enumerated a keyboard-class HID. Lets the boot-time
// factory-reset probe end its wait as soon as a keyboard is actually available
// (a fixed blind timeout used to close before slow sticks/hubs finished enumerating).
extern "C" bool usb_keyboard_mounted(void) {
  for (uint8_t i = 0; i < CFG_TUH_HID; i++) {
    if (!hid_snap[i].mounted) continue;
    if (hid_snap[i].itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) return true;
    if (hid_snap[i].last_handler == HID_HANDLER_KBD) return true;
  }
  return false;
}

// Sony DualShock 4 (PS4 wired): VID=054C, PID=05C4 (v1) / 09CC (v2).
// 507-byte report descriptor with vendor-page reports overflows TinyUSB's
// parser, so we bypass the rpt_info table for these and decode the 64-byte
// input report (id=0x01) by hand.
static inline bool is_dualshock4(uint16_t vid, uint16_t pid)
{
  return vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC);
}

// Sony DualSense (PS5): VID=054C, PID=0CE6. Same descriptor-overflow
// problem as DS4; layout differs (L2/R2 analog moved to bytes 4-5,
// face/dpad to byte 7, shoulders to byte 8).
static inline bool is_dualsense(uint16_t vid, uint16_t pid)
{
  return vid == 0x054C && pid == 0x0CE6;
}

// "FEED:2320" — clone gamepad / arcade stick.
// Composite report id=0x07 (page=Desktop usage=Joystick), 8 bytes total.
// Tinyusb parses 4 reports for this device but stock generic HID parser
// reads bytes wrong. Layout AFTER stripping report id 0x07 (7 bytes):
//   [0..3] LX, LY, RX, RY (8-bit, 0x80 center)
//   [4]    hat low-nibble (0=U, 2=R, 4=D, 6=L, 8=neutral)
//   [5]    A 0x01, B 0x02, Y 0x04, X 0x08, L 0x10, R 0x20
//   [6]    SELECT 0x04, START 0x08
// Adapted from frank-nes known_hid_maps[gamepad_FEED_2320] (which has
// dpad_x=5 because frank-nes counts byte 0 as report id; our masks shift
// down by 1 since process_generic_report already stripped it).
static inline bool is_gp_feed_2320(uint16_t vid, uint16_t pid)
{
  return vid == 0xFEED && pid == 0x2320;
}

// Generic Bluetooth gamepad clone (VID 2563 PID 0575 — 8BitDo / cheap clones):
// Simple HID report (no report-id prefix), layout (verified on real device):
//   [0]    Y 0x01, B 0x02, A 0x04, X 0x08, L 0x10, R 0x20
//   [1]    SELECT 0x01, START 0x02
//   [2]    hat low-nibble (0=U, 2=R, 4=D, 6=L, 8=neutral; 1/3/5/7 diagonals)
//   [3..6] LX, LY, RX, RY (8-bit, 0x80 center)
// Layout taken from frank-nes known_hid_maps[] (gamepad_2563_0575).
static inline bool is_gp_2563_0575(uint16_t vid, uint16_t pid)
{
  return vid == 0x2563 && pid == 0x0575;
}

// Logitech F710 / Cordless RumblePad 2 in DirectInput mode (slide
// switch on back set to "D"): VID=046D, PID=C219. Report id 0x01 +
// 7-byte payload after strip. Layout from SDL_gamecontrollerdb +
// OSX HID plist + verified empirically (Start=byte5 0x20, Back=0x10):
//   [0..3] LX, LY, RX, RY (8-bit, 0x80 center)
//   [4] low-nibble  hat (0=U, 2=R, 4=D, 6=L, 8=neutral; 1/3/5/7 diagonals)
//   [4] high-nibble X=0x10, A=0x20, B=0x40, Y=0x80
//   [5] LB=0x01 RB=0x02 LT=0x04 RT=0x08 BACK=0x10 START=0x20 L3=0x40 R3=0x80
//   [6] vendor byte (~0x74 idle)
// In D-mode triggers are digital (no analog axis) per Logitech.
// In XInput mode (slide switch "X") it enumerates as 046D:C21F and is
// handled by xinput_host as XBOX360_WIRED.
static inline bool is_logitech_f710_dinput(uint16_t vid, uint16_t pid)
{
  return vid == 0x046D && pid == 0xC219;
}

// "Game Stick Lite" wireless 2.4GHz gamepad — DragonRise-based clone.
// VID=0810, PID=0001. Composite HID: two interfaces (kbd + mouse boot
// protocol), both carrying the same 8-byte joystick report.
// Report layout (Report ID already stripped by process_generic_report):
//   [0]    LX (0x80=center)
//   [1]    LY (0x80=center)
//   [2]    RX (0x80=center)
//   [3]    RY (0x80=center)
//   [4]    low-nibble  hat  (0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW, 0xF=neutral)
//          high-nibble      X=0x10, A=0x20, B=0x40, Y=0x80
//   [5]    L=0x01, R=0x02, LT=0x04, RT=0x08, SELECT=0x10, START=0x20
//   [6]    (reserved / L3=0x01, R3=0x02 on some revisions)
static inline bool is_gp_0810_0001(uint16_t vid, uint16_t pid)
{
  return vid == 0x0810 && pid == 0x0001;
}

struct input_bits_t {
  bool a: true;
  bool b: true;
  bool select: true;
  bool start: true;
  bool right: true;
  bool left: true;
  bool up: true;
  bool down: true;
};
///extern input_bits_t keyboard_bits;
extern input_bits_t gamepad1_bits;

void process_kbd_report(
  hid_keyboard_report_t const *report,
  hid_keyboard_report_t const *prev_report
);

static void process_mouse_report(hid_mouse_report_t const * report, uint16_t len);
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len);
static void process_hid_gamepad(uint8_t const* report, uint16_t len);
static void process_ds4_gamepad(uint8_t instance, uint8_t const* report, uint16_t len);
static void process_ds5_gamepad(uint8_t instance, uint8_t const* report, uint16_t len);
static void process_f710_dinput(uint8_t instance, uint8_t const* report, uint16_t len);
static void process_gp_2563_0575(uint8_t instance, uint8_t const* report, uint16_t len);
static void process_gp_feed_2320(uint8_t instance, uint8_t const* report, uint16_t len);
static void process_gp_0810_0001(uint8_t instance, uint8_t const* report, uint16_t len);

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

static inline uint32_t kbd_now_ms(void) { return time_us_32() / 1000u; }
// Rate-limited re-arm complaint: a blocking 115200 UART line is ~4 ms, and these sit
// in the report path, so printing per event starves tuh_task() and drops the very
// reports it is complaining about. hid_app_rearm_tick() retries anyway.
static void hid_log_rearm_refused(uint8_t instance) {
  static uint32_t last_ms = 0;
  const uint32_t now = kbd_now_ms();
  if ((uint32_t)(now - last_ms) < 1000) return;
  last_ms = now;
  printf("HID instance %u: re-arm refused (watchdog will retry)\r\n", instance);
}

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
  // Interface protocol (hid_interface_protocol_enum_t)
  const char* protocol_str[] = { "None", "Keyboard", "Mouse" };
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  hid_info[instance].vid = vid;
  hid_info[instance].pid = pid;

  if (instance < CFG_TUH_HID) {
    hid_snap_t& s = hid_snap[instance];
    s.mounted = true;
    s.dev_addr = dev_addr;
    s.itf_protocol = itf_protocol;
    s.vid = vid;
    s.pid = pid;
    s.desc_len = desc_len;
    s.desc_saved = 0;
    s.last_report_len = 0;
    s.last_report_saved = 0;
    s.report_total = 0;
    s.last_handler = HID_HANDLER_NONE;
    s.decoded = 0;
    s.decoded_valid = false;
    no_rpt_info_logged[instance] = false;
    if (desc_report && desc_len) {
      uint16_t n = desc_len < HID_SNAP_DESC_BYTES ? desc_len : HID_SNAP_DESC_BYTES;
      memcpy(s.desc, desc_report, n);
      s.desc_saved = (uint8_t)n;
    }
  }

  // By default host stack will use activate boot protocol on supported interface.
  // Therefore for this simple example, we only need to parse generic report descriptor (with built-in parser)
  if ( itf_protocol == HID_ITF_PROTOCOL_NONE )
  {
    hid_info[instance].report_count = tuh_hid_parse_report_descriptor(hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);

    // Fallback: some pads (DS4/DS5, long composite descriptors) trip the
    // stock tinyusb parser and return 0 reports. Re-parse with the RIP
    // parser ported from fruit-bat/pico-hid-host (via frank-nes).
    if (hid_info[instance].report_count == 0 && desc_report && desc_len) {
      tuh_hid_report_info_plus_t rip_info[MAX_REPORT];
      uint8_t n = tuh_hid_parse_report_descriptor_plus(rip_info, MAX_REPORT, desc_report, desc_len);
      if (n > 0 && n <= MAX_REPORT) {
        for (uint8_t i = 0; i < n; i++) {
          hid_info[instance].report_info[i].report_id  = rip_info[i].report_id;
          hid_info[instance].report_info[i].usage      = (uint8_t)rip_info[i].usage;
          hid_info[instance].report_info[i].usage_page = rip_info[i].usage_page;
        }
        hid_info[instance].report_count = n;
      }
    }
  }

  // 0810:0001 announces boot kbd/mouse protocol but is a gamepad dongle.
  // TinyUSB sends SET_PROTOCOL(BOOT) which makes the dongle go silent.
  // Switch to report protocol so it starts sending HID reports.
  if (is_gp_0810_0001(vid, pid)) {
    if (itf_protocol != HID_ITF_PROTOCOL_NONE) {
      tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_REPORT);
      // tuh_hid_receive_report will be called below — that's fine,
      // it will re-arm after set_protocol completes asynchronously.
    }
  }
  
  // request to receive report
  // tuh_hid_report_received_cb() will be invoked when report is available
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    hid_log_rearm_refused(instance);
  }
}



// Defined below, next to prev_report (the keyboard state it resyncs).
static void kbd_resync_tick(void);
static bool    kbd_resync_off   = false;  // GET_REPORT unusable on this device
static uint8_t kbd_resync_fails = 0;      // consecutive GET_REPORT failures
static uint32_t hid_rearm_recoveries = 0;   // recoveries done by hid_app_rearm_tick()
static uint32_t hid_desync_recoveries = 0;  // wedged endpoints healed (see below)
static uint32_t hid_last_reports[CFG_TUH_HID] = { 0 };
static uint32_t hid_stale_since_ms[CFG_TUH_HID] = { 0 };

// Watchdog: re-arm any mounted HID interface whose IN endpoint is idle.
//
// The ONLY re-arm points are tuh_hid_mount_cb() and the tail of
// tuh_hid_report_received_cb(), so one failed tuh_hid_receive_report() leaves the
// interface permanently unarmed and the device silently dead until it is re-plugged.
// That call fails whenever the endpoint cannot be claimed or the HCD refuses the
// transfer — which is exactly what heavy competing traffic provokes: loading a file
// from a USB stick shares the bus (and, on the ZERO2 PIO-USB port, the same 1 ms SOF
// budget) with the keyboard's interrupt endpoint (hw 2026-07-31: "after loading from
// USB flash the keyboard sometimes stops working").
//
// tuh_hid_receive_ready() is false while a transfer is pending, so in normal
// operation this does nothing; it only fires when an interface really lost its arm.
// Must be called from main-loop context (never from a tuh callback).

extern "C" void hid_app_rearm_tick(void)
{
  kbd_resync_tick();

  extern volatile uint32_t g_tusb_assert_count;   // main.cpp: TU_ASSERT counter
  static uint32_t last_assert_seen = 0;
  const uint32_t now = kbd_now_ms();

  for (uint8_t i = 0; i < CFG_TUH_HID; i++) {
    if (!hid_snap[i].mounted) { hid_stale_since_ms[i] = now; continue; }
    const uint8_t daddr = hid_snap[i].dev_addr;

    // Track how long this interface has produced nothing.
    if (hid_snap[i].report_total != hid_last_reports[i]) {
      hid_last_reports[i] = hid_snap[i].report_total;
      hid_stale_since_ms[i] = now;
    }

    if (tuh_hid_receive_ready(daddr, i)) {
      // No transfer pending: the interface really is unarmed (a refused
      // tuh_hid_receive_report() is otherwise permanent — see the comment above).
      if (tuh_hid_receive_report(daddr, i)) {
        hid_rearm_recoveries++;
        Debug::log("HID instance %u: IN endpoint was unarmed, re-armed (total %u)",
                   i, (unsigned)hid_rearm_recoveries);
      }
      continue;
    }

    // TinyUSB says a transfer is in flight. That is the NORMAL state — including for
    // several hundred milliseconds after the HCD finished, because the completion
    // event waits in the queue until tuh_task() runs and this firmware starves it
    // (OSD, file loading, SD writes). Aborting here on a hunch destroys a completed
    // report and wedges the endpoint for real: doing exactly that produced a cascade
    // of "cannot request to receive report" on hw 2026-07-31.
    //
    // The one thing that genuinely wedges an endpoint is a DROPPED event, and events
    // are only ever dropped by queue_event()'s TU_ASSERT (our TU_ASSERT is a counting
    // no-op). So require hard evidence: the assert counter must have moved AND the
    // interface must have been silent for over a second.
    const uint32_t silent = (uint32_t)(now - hid_stale_since_ms[i]);
    bool evidence = false;

    if (g_tusb_assert_count != last_assert_seen && silent > 1000) {
      evidence = true;                      // an event really was dropped
    }
#if defined(ZERO2_PIO_USB_HOST)
    else if (silent > 5000) {
      // Nothing for 5 s, which is far beyond any queue latency: if the HCD has no
      // pending transfer for THIS interface's endpoint, the stack and the driver
      // disagree and only we can break the tie. (has_transfer set = the device is
      // simply NAKing, i.e. an idle keyboard — leave it alone.)
      const uint8_t ep_in = tuh_hid_ep_in(daddr, i);
      const uint32_t epdbg = ep_in ? pio_usb_host_ep_debug(daddr, ep_in) : ~0u;
      if (epdbg != ~0u && !(epdbg & 1u)) {
        evidence = true;
        Debug::log("HID instance %u: armed in stack but idle in HCD for %ums (ep=%02X st=%08X)",
                   i, (unsigned)silent, ep_in, (unsigned)epdbg);
      }
    }
#endif
    if (!evidence) continue;

    last_assert_seen = g_tusb_assert_count;
    tuh_hid_receive_abort(daddr, i);
    if (tuh_hid_receive_report(daddr, i)) {
      hid_desync_recoveries++;
      hid_stale_since_ms[i] = now;
      Debug::log("HID instance %u: wedged endpoint recovered (asserts=%u), re-armed (total %u)",
                 i, (unsigned)g_tusb_assert_count, (unsigned)hid_desync_recoveries);
      kbd_resync_off = false;   // its control transfers may work again now
      kbd_resync_fails = 0;
    }
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  printf("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
  if (instance < CFG_TUH_HID) {
    hid_snap[instance].mounted = false;
    no_rpt_info_logged[instance] = false;
  }
}

static hid_keyboard_report_t prev_report = { 0 , 0 , {0}};

// ── Stuck-key recovery for boot keyboards ───────────────────────────────────
// A boot keyboard reports STATE, but only when it CHANGES: lose the report that
// carries a release and the key stays down forever as far as we are concerned. In
// BASIC that reads as a dead keyboard with one key jammed on; the ZX ROM keeps
// seeing the matrix bit. Reports do get lost on a flaky bus — a low-speed keyboard
// behind a hub on the PIO port is the worst case, since every transaction there is
// prefixed by a PRE token and the bus switches speed mid-transaction.
//
// GET_REPORT over the control pipe returns the CURRENT state, so it can resync
// without ever inventing a release: only keys the device no longer reports are
// released. Issued only while we believe a key is held and the interrupt endpoint has
// been silent for a while, so an idle keyboard costs nothing and a genuine long hold
// (games) is preserved.
//
// TRUST IS NOT FREE: a plain long hold IS 400 ms of interrupt silence with a key
// held, so the resync fires on every auto-repeat. A keyboard whose GET_REPORT
// answer is not a boot report (report-ID-prefixed, an NKRO bitmap, a different
// report altogether — hw report 2026-08-23, Rapoo on z0p2) feeds constant junk
// bytes into process_kbd_report as keycodes: a fixed 0x24 in the reply presses
// HID '7', nothing on the silent interrupt pipe ever contradicts it, and the ZX
// ROM's own typematic repeats '7' forever while the really-held key "releases"
// (it is absent from the junk). So the device must EARN the resync first:
// kbd_resync_tick probes GET_REPORT once while we believe NO key is held — a
// proper boot keyboard answers all-idle (modifier 0, six zero keycodes; the
// reserved byte is OEM-defined and ignored). Junk instead disables the resync
// for this device for the session (the pico-spec behavior, where such keyboards
// work); a stall goes through the existing 5-strikes path.
#define KBD_RESYNC_SILENCE_MS 400   // silence with a key held before we ask the device
#define KBD_PROBE_SILENCE_MS 1000   // idle silence before the one-time trust probe
static uint8_t  kbd_resync_daddr    = 0;
static uint8_t  kbd_resync_instance = 0xFF;
static uint32_t kbd_last_report_ms  = 0;
static uint8_t  kbd_resync_buf[8];
static bool     kbd_resync_busy     = false;
static bool     kbd_resync_probing  = false;  // current GET_REPORT is the trust probe
static bool     kbd_resync_verified = false;  // idle probe answered a clean boot report
static uint32_t kbd_resync_fixes    = 0;
static uint32_t kbd_report_seq      = 0;      // interrupt reports processed, ever
static uint32_t kbd_resync_seq      = 0;      // kbd_report_seq when the GET_REPORT went out
static uint32_t kbd_resync_stale    = 0;      // replies discarded as overtaken

static bool kbd_state_has_key(void) {
  if (prev_report.modifier) return true;
  for (uint8_t kc : prev_report.keycode) if (kc) return true;
  return false;
}

// Health line for the keyboard interface: printed at most every 2 s and ONLY while we
// believe a key is held (i.e. exactly the "keyboard is stuck/hung" situation), so it
// costs nothing in normal use. Tells the three failure modes apart:
//   ep=---- (~0)          the PIO layer has no endpoint for it -> arm/enumeration lost
//   ep flags bit0=1, failed=0, no reports  the device NAKs: our state is stale, the
//                                          GET_REPORT resync is what must fix it
//   failed>0 / timeouts rising             bus errors (PRE/LS through the hub)
static void kbd_health_log(void) {
#if defined(ZERO2_PIO_USB_HOST)
  static uint32_t last_ms = 0;
  const uint32_t now = kbd_now_ms();
  if (kbd_resync_instance == 0xFF) return;

  // Print while a key is held (the "stuck key" case) and also when the interface has
  // simply gone quiet for a long time (the "keyboard died and nothing is logged at all"
  // case — an idle keyboard is legitimately silent, hence the slow 10 s cadence).
  const bool held  = kbd_state_has_key();
  const uint32_t silent = now - kbd_last_report_ms;
  if (!held && silent < 5000) return;
  if ((uint32_t)(now - last_ms) < (held ? 2000u : 10000u)) return;
  last_ms = now;

  tuh_bus_info_t bus = { 0 };
  tuh_bus_info_get(kbd_resync_daddr, &bus);
  const uint8_t inst = kbd_resync_instance;
  const uint8_t ep_in = tuh_hid_ep_in(kbd_resync_daddr, inst);
  const uint32_t epdbg = pio_usb_host_ep_debug(kbd_resync_daddr, ep_in);
  extern volatile uint32_t g_tusb_assert_count;   // dropped events land here
  Debug::log("HID kbd: rhport=%u daddr=%u inst=%u ep=%02X held=%u silent=%ums reports=%u "
             "resync(fix=%u fail=%u off=%u ver=%u stale=%u) rearm=%u desync=%u asserts=%u ready=%u epst=%08X",
             bus.rhport, kbd_resync_daddr, inst, ep_in, (unsigned)held,
             (unsigned)silent,
             (unsigned)(inst < CFG_TUH_HID ? hid_snap[inst].report_total : 0),
             (unsigned)kbd_resync_fixes, (unsigned)kbd_resync_fails,
             (unsigned)kbd_resync_off, (unsigned)kbd_resync_verified,
             (unsigned)kbd_resync_stale,
             (unsigned)hid_rearm_recoveries,
             (unsigned)hid_desync_recoveries, (unsigned)g_tusb_assert_count,
             (unsigned)tuh_hid_receive_ready(kbd_resync_daddr, inst),
             (unsigned)epdbg);
#endif
}

static void kbd_resync_tick(void) {
  kbd_health_log();
  if (kbd_resync_off || kbd_resync_busy) return;
  if (kbd_resync_instance == 0xFF) return;

  if (!kbd_resync_verified) {
    // Trust probe (see the block comment above): only while our state says no key
    // is held, ~1 s after the keyboard last spoke. Inconclusive runs just retry.
    if (kbd_state_has_key()) return;
    if ((uint32_t)(kbd_now_ms() - kbd_last_report_ms) < KBD_PROBE_SILENCE_MS) return;
    memset(kbd_resync_buf, 0, sizeof(kbd_resync_buf));
    if (tuh_hid_get_report(kbd_resync_daddr, kbd_resync_instance, 0,
                           HID_REPORT_TYPE_INPUT, kbd_resync_buf,
                           sizeof(kbd_resync_buf))) {
      kbd_resync_busy = true;
      kbd_resync_probing = true;
    } else if (++kbd_resync_fails >= 5) {
      kbd_resync_off = true;
      Debug::log("HID kbd: GET_REPORT unavailable, stuck-key resync disabled");
    }
    return;
  }

  if (!kbd_state_has_key()) return;
  if ((uint32_t)(kbd_now_ms() - kbd_last_report_ms) < KBD_RESYNC_SILENCE_MS) return;

  memset(kbd_resync_buf, 0, sizeof(kbd_resync_buf));
  kbd_resync_seq = kbd_report_seq;      // anything newer than this overtakes the reply
  if (tuh_hid_get_report(kbd_resync_daddr, kbd_resync_instance, 0,
                         HID_REPORT_TYPE_INPUT, kbd_resync_buf,
                         sizeof(kbd_resync_buf))) {
    kbd_resync_busy = true;
  } else if (++kbd_resync_fails >= 5) {
    // Control pipe unavailable/stalling: fall back to kicking the interrupt endpoint,
    // which at least revives delivery if the transfer itself got stuck.
    kbd_resync_off = true;
    Debug::log("HID kbd: GET_REPORT unavailable, stuck-key resync disabled");
    tuh_hid_receive_abort(kbd_resync_daddr, kbd_resync_instance);
    tuh_hid_receive_report(kbd_resync_daddr, kbd_resync_instance);
  }
}

void tuh_hid_get_report_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t report_id,
                                    uint8_t report_type, uint16_t len)
{
  (void) dev_addr; (void) idx; (void) report_id; (void) report_type;
  kbd_resync_busy = false;
  const bool probing = kbd_resync_probing;
  kbd_resync_probing = false;
  if (len < sizeof(hid_keyboard_report_t)) {   // 0 = stalled/failed
    if (++kbd_resync_fails >= 5) {
      kbd_resync_off = true;
      Debug::log("HID kbd: GET_REPORT rejected, stuck-key resync disabled");
    }
    return;
  }
  kbd_resync_fails = 0;
  if (probing) {
    // Trust probe: we believed no key was held when it was issued. If a real key
    // arrived meanwhile the run is inconclusive — retry later. Otherwise the reply
    // must be the all-idle boot report; anything else is a device whose GET_REPORT
    // does not speak boot protocol, and trusting it types phantom keys (the
    // "holding any key repeats '7'" Rapoo, hw 2026-08-23).
    if (kbd_state_has_key()) return;
    hid_keyboard_report_t const* r = (hid_keyboard_report_t const*) kbd_resync_buf;
    bool idle = (r->modifier == 0);
    for (uint8_t kc : r->keycode) if (kc) idle = false;
    if (idle) {
      kbd_resync_verified = true;
      Debug::log("HID kbd: GET_REPORT probe clean, stuck-key resync enabled");
    } else {
      kbd_resync_off = true;
      Debug::log("HID kbd: GET_REPORT junk while idle "
                 "(%02X %02X %02X %02X %02X %02X %02X %02X), stuck-key resync disabled",
                 kbd_resync_buf[0], kbd_resync_buf[1], kbd_resync_buf[2], kbd_resync_buf[3],
                 kbd_resync_buf[4], kbd_resync_buf[5], kbd_resync_buf[6], kbd_resync_buf[7]);
    }
    return;
  }
  hid_keyboard_report_t const* now = (hid_keyboard_report_t const*) kbd_resync_buf;

  // A control-pipe answer is a SNAPSHOT, and the interrupt pipe can overtake it:
  // the user releases the key while the GET_REPORT is in flight, the (empty)
  // interrupt report is processed first, and the older reply then arrives still
  // holding the key. Applying it re-PRESSES a key nobody is holding — and since
  // the device is now idle and silent, nothing on the interrupt pipe ever
  // contradicts it. That is the "the down arrow sometimes jams" report (hw
  // 2026-08-29, seen in the Skvosh game, where arrows are held past the 400 ms
  // resync threshold constantly). Anything that arrived after the request went
  // out makes the reply worthless.
  if (kbd_report_seq != kbd_resync_seq) {
    kbd_resync_stale++;
    kbd_last_report_ms = kbd_now_ms();
    return;
  }

  // A resync may only RELEASE keys the device no longer reports — it must never
  // press one. The block comment above always claimed this; process_kbd_report()
  // applies the reply in BOTH directions, so state it as code: act on the
  // intersection of what we believe is held and what the device says.
  hid_keyboard_report_t merged;
  memset(&merged, 0, sizeof(merged));
  merged.modifier = prev_report.modifier & now->modifier;
  uint8_t nk = 0;
  for (uint8_t kc : prev_report.keycode) {
    if (!kc) continue;
    for (uint8_t k2 : now->keycode)
      if (k2 == kc) { merged.keycode[nk++] = kc; break; }
  }

  if (memcmp(&merged, &prev_report, sizeof(prev_report)) != 0) {
    kbd_resync_fixes++;
    Debug::log("HID kbd: state resynced via GET_REPORT (total %u, stale %u, "
               "reply %02X %02X %02X %02X %02X %02X %02X %02X)",
               (unsigned)kbd_resync_fixes, (unsigned)kbd_resync_stale,
               kbd_resync_buf[0], kbd_resync_buf[1], kbd_resync_buf[2], kbd_resync_buf[3],
               kbd_resync_buf[4], kbd_resync_buf[5], kbd_resync_buf[6], kbd_resync_buf[7]);
    process_kbd_report(&merged, &prev_report);
    prev_report = merged;
  }
  kbd_last_report_ms = kbd_now_ms();
}


#include "ff.h"

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  // A zero-length interrupt-IN report is legal idle traffic for some devices: a Dell
  // wireless receiver (413C:301D) sends ZLPs on its third HID interface instead of
  // NAKing. Every dispatch below indexes report[0] — the boot-keyboard branch casts
  // the buffer to an 8-byte struct, and process_generic_report() does report++/len--
  // so len wraps to 0xFFFF — i.e. an out-of-bounds read either way. It also used to
  // emit one "Couldn't find the report info" printf PER report; at ~4 ms of blocking
  // UART each that starves tuh_task(), so real keyboard reports get dropped and keys
  // stick (worst through a hub, where the receiver shares the frame budget).
  if (report == NULL || len == 0) {
    if (!tuh_hid_receive_report(dev_addr, instance))
      hid_log_rearm_refused(instance);
    return;
  }

  // 0810:0001 registers with boot kbd/mouse protocol but sends joystick
  // reports — intercept before the boot-protocol switch.
  if (is_gp_0810_0001(hid_info[instance].vid, hid_info[instance].pid)) {
    // Update snap so OSD can show reports rx / last bytes
    if (instance < CFG_TUH_HID && report && len) {
      hid_snap_t& s = hid_snap[instance];
      s.report_total++;
      s.last_report_len = len;
      uint16_t n = len < HID_SNAP_REPORT_BYTES ? len : HID_SNAP_REPORT_BYTES;
      memcpy(s.last_report, report, n);
      s.last_report_saved = (uint8_t)n;
    }
    if (len >= 8 && (report[0] == 0x01 || report[0] == 0x02)) {
      process_gp_0810_0001(instance, report + 1, len - 1);
    }
    if (!tuh_hid_receive_report(dev_addr, instance))
      hid_log_rearm_refused(instance);
    return;
  }
  
  if (instance < CFG_TUH_HID && report && len) {
    hid_snap_t& s = hid_snap[instance];
    s.report_total++;
    s.last_report_len = len;
    uint16_t n = len < HID_SNAP_REPORT_BYTES ? len : HID_SNAP_REPORT_BYTES;
    memcpy(s.last_report, report, n);
    s.last_report_saved = (uint8_t)n;
  }
/*
  FIL f;
  f_open(&f, "1.log", FA_OPEN_APPEND | FA_WRITE);
  char tmp[64];
  snprintf(tmp, 64, "USB report itf_protocol %d; len: %d mod: %02Xh kc0: %02Xh\n",
      itf_protocol,
      len,
      ((hid_keyboard_report_t const*)report)->modifier,
      ((hid_keyboard_report_t const*)report)->keycode[0]
  );
  UINT bw;
  f_write(&f, tmp, strlen(tmp), &bw);
  f_close(&f);
*/
  switch (itf_protocol)
  {
    case HID_ITF_PROTOCOL_KEYBOARD:
      TU_LOG2("HID receive boot keyboard report\r\n");
      hid_snap_set_handler(instance, HID_HANDLER_KBD);
      // Remember who to ask for a state resync, and when we last heard from it.
      // A different device (re-plug, another keyboard) starts untrusted again.
      if (kbd_resync_daddr != dev_addr || kbd_resync_instance != instance) {
        kbd_resync_verified = false;
        kbd_resync_probing  = false;
        kbd_resync_off      = false;
        kbd_resync_fails    = 0;
      }
      kbd_resync_daddr    = dev_addr;
      kbd_resync_instance = instance;
      kbd_last_report_ms  = kbd_now_ms();
      kbd_report_seq++;               // lets a resync reply see it was overtaken
      process_kbd_report( (hid_keyboard_report_t const*) report, &prev_report );
      prev_report = *(hid_keyboard_report_t const*)report;
    break;

    case HID_ITF_PROTOCOL_MOUSE:
      TU_LOG2("HID receive boot mouse report\r\n");
      hid_snap_set_handler(instance, HID_HANDLER_MOUSE);
      process_mouse_report( (hid_mouse_report_t const*) report, len );
    break;

    default:
      // Generic report requires matching ReportID and contents with previous parsed report info
      process_generic_report(dev_addr, instance, report, len);
    break;
  }

  // continue to request to receive report
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    hid_log_rearm_refused(instance);
  }
}

//--------------------------------------------------------------------+
// Keyboard
//--------------------------------------------------------------------+

// look up new key in previous keys
static inline bool find_key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
  for(uint8_t i=0; i<6; i++)
  {
    if (report->keycode[i] == keycode)  return true;
  }

  return false;
}

//--------------------------------------------------------------------+
// Mouse
//--------------------------------------------------------------------+

void cursor_movement(int8_t x, int8_t y, int8_t wheel)
{
#if USE_ANSI_ESCAPE
  // Move X using ansi escape
  if ( x < 0)
  {
    printf(ANSI_CURSOR_BACKWARD(%d), (-x)); // move left
  }else if ( x > 0)
  {
    printf(ANSI_CURSOR_FORWARD(%d), x); // move right
  }

  // Move Y using ansi escape
  if ( y < 0)
  {
    printf(ANSI_CURSOR_UP(%d), (-y)); // move up
  }else if ( y > 0)
  {
    printf(ANSI_CURSOR_DOWN(%d), y); // move down
  }

  // Scroll using ansi escape
  if (wheel < 0)
  {
    printf(ANSI_SCROLL_UP(%d), (-wheel)); // scroll up
  }else if (wheel > 0)
  {
    printf(ANSI_SCROLL_DOWN(%d), wheel); // scroll down
  }

  printf("\r\n");
#else
  printf("(%d %d %d)\r\n", x, y, wheel);
#endif
}

#include "ESPectrum.h"

static void process_mouse_report(hid_mouse_report_t const * report, uint16_t len)
{
    ESPectrum::mouseSeen = true;
    ESPectrum::mouseButtonL = report->buttons & MOUSE_BUTTON_LEFT;
    ESPectrum::mouseButtonR = report->buttons & MOUSE_BUTTON_RIGHT;
    ESPectrum::mouseButtonM = report->buttons & MOUSE_BUTTON_MIDDLE;
    ESPectrum::mouseX += report->x >> 2;
    ESPectrum::mouseY -= report->y >> 2; // TODO: DPI
    // Kempston wheel-mouse: #FADF bits 4-7 are a free-running notch counter.
    // 3-byte boot-protocol reports (buttons/x/y) have NO wheel field — reading
    // report->wheel there picks up garbage past the report → phantom scrolls.
    if (len >= 4) ESPectrum::mouseWheel += (int8_t)report->wheel;
    // Serial (COM) mouse packets consume deltas from these accumulators.
    // HID y is down-positive, which matches the Microsoft Mouse convention
    // (the FPGA negates PS/2 y for the same reason: MS_Y => -ms_delta_y).
    ESPectrum::mouseDX += report->x;
    ESPectrum::mouseDY += report->y;
  /**

  //------------- button state  -------------//
  uint8_t button_changed_mask = report->buttons ^ prev_report.buttons;
  if ( button_changed_mask & report->buttons)
  {
    printf(" %c%c%c ",
       report->buttons & MOUSE_BUTTON_LEFT   ? 'L' : '-',
       report->buttons & MOUSE_BUTTON_MIDDLE ? 'M' : '-',
       report->buttons & MOUSE_BUTTON_RIGHT  ? 'R' : '-');
  }

  //------------- cursor movement -------------//
  cursor_movement(report->x, report->y, report->wheel);
  */
}

//--------------------------------------------------------------------+
// HID Gamepad / Joystick
//--------------------------------------------------------------------+
// Decoded layout for cheap SNES-style USB pads (e.g. VID=081F PID=E401).
// 8-byte report:
//   [0] X axis  : 0x00=Left, 0x7F=center, 0xFF=Right
//   [1] Y axis  : 0x00=Up,   0x7F=center, 0xFF=Down
//   [5] high nibble = face buttons : 0x10=X, 0x20=A, 0x40=B, 0x80=Y
//   [6] shoulders/system           : 0x01=L, 0x02=R, 0x10=Select, 0x20=Start
// Mapping to emulator: D-pad and B (fire) feed Kempston via VK_DPAD_*; A is
// alt-fire; Start opens OSD (F1); Select is Backspace in menu.
static void process_hid_gamepad(uint8_t const* report, uint16_t len)
{
    if (len < 7) return;

    bool left  = report[0] < 0x40;
    bool right = report[0] > 0xC0;
    bool up    = report[1] < 0x40;
    bool down  = report[1] > 0xC0;

    uint8_t b5 = report[5];
    uint8_t b6 = report[6];
    bool btn_a     = (b5 & 0x20) != 0;
    bool btn_b     = (b5 & 0x40) != 0;
    bool btn_x     = (b5 & 0x10) != 0;
    bool btn_y     = (b5 & 0x80) != 0;
    bool btn_l     = (b6 & 0x01) != 0;
    bool btn_r     = (b6 & 0x02) != 0;
    bool btn_lt    = (b6 & 0x04) != 0;  // LT/RT: DragonRise-family layout,
    bool btn_rt    = (b6 & 0x08) != 0;  // zero on pads without triggers
    bool btn_sel   = (b6 & 0x10) != 0;
    bool btn_start = (b6 & 0x20) != 0;

    // D-pad (edge-detected, drives Kempston via VK_DPAD_*)
    if (up != gamepad1_bits.up) {
        joyPushData(fabgl::VirtualKey::VK_MENU_UP, up);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_UP, up);
    }
    if (down != gamepad1_bits.down) {
        joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, down);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_DOWN, down);
    }
    if (left != gamepad1_bits.left) {
        joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, left);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_LEFT, left);
    }
    if (right != gamepad1_bits.right) {
        joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, right);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_RIGHT, right);
    }

    // B = fire (yellow bottom button — most ergonomic on SNES-style pad)
    if (btn_b != gamepad1_bits.b) {
        joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, btn_b);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_FIRE, btn_b);
    }
    // A = alt-fire
    if (btn_a != gamepad1_bits.a) {
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, btn_a);
    }
    // Start → OSD menu (F1), also DPAD_START for joydef
    if (btn_start != gamepad1_bits.start) {
        joyPushData(fabgl::VirtualKey::VK_MENU_HOME, btn_start);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_START, btn_start);
    }
    // Select → backspace in menu / DPAD_SELECT
    if (btn_sel != gamepad1_bits.select) {
        joyPushData(fabgl::VirtualKey::VK_MENU_BS, btn_sel);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_SELECT, btn_sel);
    }
    // X / Y / L / R — fed to joydef via VK_JOY_X/Y/Z/C-style dropdowns. Use the
    // remaining DPAD slots so users can rebind in the joystick definition menu.
    static bool prev_x = false, prev_y = false, prev_l = false, prev_r = false, prev_lt = false, prev_rt = false;
    if (btn_x != prev_x) { kbdPushData(fabgl::VirtualKey::VK_JOY_X, btn_x); prev_x = btn_x; }
    if (btn_y != prev_y) { kbdPushData(fabgl::VirtualKey::VK_JOY_Y, btn_y); prev_y = btn_y; }
    if (btn_l != prev_l) { kbdPushData(fabgl::VirtualKey::VK_JOY_Z, btn_l); prev_l = btn_l; }
    if (btn_r != prev_r) { kbdPushData(fabgl::VirtualKey::VK_JOY_C, btn_r); prev_r = btn_r; }
    if (btn_lt != prev_lt) { kbdPushData(fabgl::VirtualKey::VK_JOY_L2, btn_lt); prev_lt = btn_lt; }
    if (btn_rt != prev_rt) { kbdPushData(fabgl::VirtualKey::VK_JOY_R2, btn_rt); prev_rt = btn_rt; }

    gamepad1_bits.up = up;
    gamepad1_bits.down = down;
    gamepad1_bits.left = left;
    gamepad1_bits.right = right;
    gamepad1_bits.a = btn_a;
    gamepad1_bits.b = btn_b;
    gamepad1_bits.start = btn_start;
    gamepad1_bits.select = btn_sel;
}

//--------------------------------------------------------------------+
// DualShock 4 (Sony PS4) — VID 054C, PID 05C4/09CC
//--------------------------------------------------------------------+
// Report layout after stripping the leading id=0x01 byte (USB mode):
//   [0..3] LX, LY, RX, RY (0x80 = center)
//   [4]    low nibble = D-pad (0=N..7=NW, 8=neutral)
//          high nibble = Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80
//   [5]    L1 0x01, R1 0x02, L2 0x04, R2 0x08, Share 0x10,
//          Options 0x20, L3 0x40, R3 0x80
//   [6]    PS 0x01, TouchClick 0x02, plus 6-bit counter
// Mapping mirrors process_hid_gamepad(): D-pad+Cross drive Kempston,
// Circle = alt-fire, Square/Triangle/L1/R1 → VK_JOY_X/Y/Z/C for joydef.
static void process_ds4_gamepad(uint8_t instance, uint8_t const* report, uint16_t len)
{
    if (len < 7) return;

    uint8_t lx = report[0];
    uint8_t ly = report[1];
    uint8_t dpad_hat = report[4] & 0x0F;
    uint8_t face = report[4] & 0xF0;
    uint8_t b5 = report[5];

    bool stick_left  = lx < 0x40;
    bool stick_right = lx > 0xC0;
    bool stick_up    = ly < 0x40;
    bool stick_down  = ly > 0xC0;

    bool hat_up    = (dpad_hat == 7 || dpad_hat == 0 || dpad_hat == 1);
    bool hat_right = (dpad_hat == 1 || dpad_hat == 2 || dpad_hat == 3);
    bool hat_down  = (dpad_hat == 3 || dpad_hat == 4 || dpad_hat == 5);
    bool hat_left  = (dpad_hat == 5 || dpad_hat == 6 || dpad_hat == 7);
    if (dpad_hat >= 8) { hat_up = hat_down = hat_left = hat_right = false; }

    bool up    = stick_up    || hat_up;
    bool down  = stick_down  || hat_down;
    bool left  = stick_left  || hat_left;
    bool right = stick_right || hat_right;

    bool btn_square   = (face & 0x10) != 0;
    bool btn_cross    = (face & 0x20) != 0;
    bool btn_circle   = (face & 0x40) != 0;
    bool btn_triangle = (face & 0x80) != 0;
    bool btn_l1       = (b5 & 0x01) != 0;
    bool btn_r1       = (b5 & 0x02) != 0;
    bool btn_l2       = (b5 & 0x04) != 0;
    bool btn_r2       = (b5 & 0x08) != 0;
    bool btn_share    = (b5 & 0x10) != 0;
    bool btn_options  = (b5 & 0x20) != 0;

    if (up != gamepad1_bits.up) {
        joyPushData(fabgl::VirtualKey::VK_MENU_UP, up);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_UP, up);
    }
    if (down != gamepad1_bits.down) {
        joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, down);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_DOWN, down);
    }
    if (left != gamepad1_bits.left) {
        joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, left);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_LEFT, left);
    }
    if (right != gamepad1_bits.right) {
        joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, right);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_RIGHT, right);
    }

    if (btn_cross != gamepad1_bits.b) {
        joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, btn_cross);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_FIRE, btn_cross);
    }
    if (btn_circle != gamepad1_bits.a) {
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, btn_circle);
    }
    if (btn_options != gamepad1_bits.start) {
        joyPushData(fabgl::VirtualKey::VK_MENU_HOME, btn_options);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_START, btn_options);
    }
    if (btn_share != gamepad1_bits.select) {
        joyPushData(fabgl::VirtualKey::VK_MENU_BS, btn_share);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_SELECT, btn_share);
    }

    static bool prev_sq = false, prev_tr = false, prev_l1 = false, prev_r1 = false, prev_l2 = false, prev_r2 = false;
    if (btn_square   != prev_sq) { kbdPushData(fabgl::VirtualKey::VK_JOY_X, btn_square);   prev_sq = btn_square; }
    if (btn_triangle != prev_tr) { kbdPushData(fabgl::VirtualKey::VK_JOY_Y, btn_triangle); prev_tr = btn_triangle; }
    if (btn_l1       != prev_l1) { kbdPushData(fabgl::VirtualKey::VK_JOY_Z, btn_l1);       prev_l1 = btn_l1; }
    if (btn_r1       != prev_r1) { kbdPushData(fabgl::VirtualKey::VK_JOY_C, btn_r1);       prev_r1 = btn_r1; }
    if (btn_l2       != prev_l2) { kbdPushData(fabgl::VirtualKey::VK_JOY_L2, btn_l2);      prev_l2 = btn_l2; }
    if (btn_r2       != prev_r2) { kbdPushData(fabgl::VirtualKey::VK_JOY_R2, btn_r2);      prev_r2 = btn_r2; }

    gamepad1_bits.up = up;
    gamepad1_bits.down = down;
    gamepad1_bits.left = left;
    gamepad1_bits.right = right;
    gamepad1_bits.a = btn_circle;
    gamepad1_bits.b = btn_cross;
    gamepad1_bits.start = btn_options;
    gamepad1_bits.select = btn_share;

    uint16_t dec = 0;
    if (up) dec |= HID_DEC_UP;       if (down) dec |= HID_DEC_DOWN;
    if (left) dec |= HID_DEC_LEFT;   if (right) dec |= HID_DEC_RIGHT;
    if (btn_cross) dec |= HID_DEC_A;     if (btn_circle) dec |= HID_DEC_B;
    if (btn_square) dec |= HID_DEC_X;    if (btn_triangle) dec |= HID_DEC_Y;
    if (btn_l1) dec |= HID_DEC_L;        if (btn_r1) dec |= HID_DEC_R;
    if (btn_options) dec |= HID_DEC_START;  if (btn_share) dec |= HID_DEC_SELECT;
    hid_snap_set_decoded(instance, HID_HANDLER_DS4, dec);
}

//--------------------------------------------------------------------+
// DualSense (Sony PS5) — VID 054C, PID 0CE6
//--------------------------------------------------------------------+
// Report layout after stripping id=0x01 (USB mode, ~63 bytes total):
//   [0..3] LX, LY, RX, RY (0x80 center)
//   [4]    L2 analog
//   [5]    R2 analog
//   [6]    counter
//   [7]    low nibble = D-pad hat (0..7, 8=neutral)
//          high nibble = Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80
//   [8]    L1 0x01, R1 0x02, L2btn 0x04, R2btn 0x08,
//          Create 0x10, Options 0x20, L3 0x40, R3 0x80
//   [9]    PS 0x01, TouchClick 0x02, Mute 0x04
// Same emulator mapping as DS4: Cross=Fire, Circle=AltFire,
// Options=Start/F1, Create=Select/Backspace, Square/Triangle/L1/R1
// → VK_JOY_X/Y/Z/C.
static void process_ds5_gamepad(uint8_t instance, uint8_t const* report, uint16_t len)
{
    if (len < 9) return;

    uint8_t lx = report[0];
    uint8_t ly = report[1];
    uint8_t b7 = report[7];
    uint8_t b8 = report[8];
    uint8_t dpad_hat = b7 & 0x0F;
    uint8_t face = b7 & 0xF0;

    bool stick_left  = lx < 0x40;
    bool stick_right = lx > 0xC0;
    bool stick_up    = ly < 0x40;
    bool stick_down  = ly > 0xC0;

    bool hat_up    = (dpad_hat == 7 || dpad_hat == 0 || dpad_hat == 1);
    bool hat_right = (dpad_hat == 1 || dpad_hat == 2 || dpad_hat == 3);
    bool hat_down  = (dpad_hat == 3 || dpad_hat == 4 || dpad_hat == 5);
    bool hat_left  = (dpad_hat == 5 || dpad_hat == 6 || dpad_hat == 7);
    if (dpad_hat >= 8) { hat_up = hat_down = hat_left = hat_right = false; }

    bool up    = stick_up    || hat_up;
    bool down  = stick_down  || hat_down;
    bool left  = stick_left  || hat_left;
    bool right = stick_right || hat_right;

    bool btn_square   = (face & 0x10) != 0;
    bool btn_cross    = (face & 0x20) != 0;
    bool btn_circle   = (face & 0x40) != 0;
    bool btn_triangle = (face & 0x80) != 0;
    bool btn_l1       = (b8 & 0x01) != 0;
    bool btn_r1       = (b8 & 0x02) != 0;
    bool btn_l2       = (b8 & 0x04) != 0;
    bool btn_r2       = (b8 & 0x08) != 0;
    bool btn_create   = (b8 & 0x10) != 0;
    bool btn_options  = (b8 & 0x20) != 0;

    if (up != gamepad1_bits.up) {
        joyPushData(fabgl::VirtualKey::VK_MENU_UP, up);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_UP, up);
    }
    if (down != gamepad1_bits.down) {
        joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, down);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_DOWN, down);
    }
    if (left != gamepad1_bits.left) {
        joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, left);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_LEFT, left);
    }
    if (right != gamepad1_bits.right) {
        joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, right);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_RIGHT, right);
    }

    if (btn_cross != gamepad1_bits.b) {
        joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, btn_cross);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_FIRE, btn_cross);
    }
    if (btn_circle != gamepad1_bits.a) {
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, btn_circle);
    }
    if (btn_options != gamepad1_bits.start) {
        joyPushData(fabgl::VirtualKey::VK_MENU_HOME, btn_options);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_START, btn_options);
    }
    if (btn_create != gamepad1_bits.select) {
        joyPushData(fabgl::VirtualKey::VK_MENU_BS, btn_create);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_SELECT, btn_create);
    }

    static bool prev_sq = false, prev_tr = false, prev_l1 = false, prev_r1 = false, prev_l2 = false, prev_r2 = false;
    if (btn_square   != prev_sq) { kbdPushData(fabgl::VirtualKey::VK_JOY_X, btn_square);   prev_sq = btn_square; }
    if (btn_triangle != prev_tr) { kbdPushData(fabgl::VirtualKey::VK_JOY_Y, btn_triangle); prev_tr = btn_triangle; }
    if (btn_l1       != prev_l1) { kbdPushData(fabgl::VirtualKey::VK_JOY_Z, btn_l1);       prev_l1 = btn_l1; }
    if (btn_r1       != prev_r1) { kbdPushData(fabgl::VirtualKey::VK_JOY_C, btn_r1);       prev_r1 = btn_r1; }
    if (btn_l2       != prev_l2) { kbdPushData(fabgl::VirtualKey::VK_JOY_L2, btn_l2);      prev_l2 = btn_l2; }
    if (btn_r2       != prev_r2) { kbdPushData(fabgl::VirtualKey::VK_JOY_R2, btn_r2);      prev_r2 = btn_r2; }

    gamepad1_bits.up = up;
    gamepad1_bits.down = down;
    gamepad1_bits.left = left;
    gamepad1_bits.right = right;
    gamepad1_bits.a = btn_circle;
    gamepad1_bits.b = btn_cross;
    gamepad1_bits.start = btn_options;
    gamepad1_bits.select = btn_create;

    uint16_t dec = 0;
    if (up) dec |= HID_DEC_UP;       if (down) dec |= HID_DEC_DOWN;
    if (left) dec |= HID_DEC_LEFT;   if (right) dec |= HID_DEC_RIGHT;
    if (btn_cross) dec |= HID_DEC_A;     if (btn_circle) dec |= HID_DEC_B;
    if (btn_square) dec |= HID_DEC_X;    if (btn_triangle) dec |= HID_DEC_Y;
    if (btn_l1) dec |= HID_DEC_L;        if (btn_r1) dec |= HID_DEC_R;
    if (btn_options) dec |= HID_DEC_START;  if (btn_create) dec |= HID_DEC_SELECT;
    hid_snap_set_decoded(instance, HID_HANDLER_DS5, dec);
}

//--------------------------------------------------------------------+
// Logitech F710 — DirectInput mode (slide switch on back set to "D")
//--------------------------------------------------------------------+
// Layout after stripping report id 0x01 (7+ usable bytes):
//   [0..3] LX, LY, RX, RY (8-bit, 0x80 center)
//   [4]    hat low-nibble (0=U, 2=R, 4=D, 6=L, 8=neutral; 1/3/5/7 diagonals)
//   [5]    high nibble = X 0x10, A 0x20, B 0x40, Y 0x80
//   [6]    LB 0x01, RB 0x02, LT 0x04, RT 0x08, BACK 0x10, START 0x20, L3 0x40, R3 0x80
// Logitech labels are inverted vs PS-style: A=south(green), B=east(red),
// X=west(blue), Y=north(yellow). Map A=Fire (b/cross-equivalent),
// B=AltFire, Start/Back same as DS4.
static void process_f710_dinput(uint8_t instance, uint8_t const* report, uint16_t len)
{
    if (len < 7) return;

    uint8_t lx   = report[0];
    uint8_t ly   = report[1];
    uint8_t b4   = report[4];
    uint8_t hat  = b4 & 0x0F;
    uint8_t face = b4 & 0xF0;
    uint8_t b5   = report[5];

    bool stick_left  = lx < 0x40;
    bool stick_right = lx > 0xC0;
    bool stick_up    = ly < 0x40;
    bool stick_down  = ly > 0xC0;

    bool hat_up    = (hat == 7 || hat == 0 || hat == 1);
    bool hat_right = (hat == 1 || hat == 2 || hat == 3);
    bool hat_down  = (hat == 3 || hat == 4 || hat == 5);
    bool hat_left  = (hat == 5 || hat == 6 || hat == 7);
    if (hat >= 8) { hat_up = hat_down = hat_left = hat_right = false; }

    bool up    = stick_up    || hat_up;
    bool down  = stick_down  || hat_down;
    bool left  = stick_left  || hat_left;
    bool right = stick_right || hat_right;

    bool btn_x     = (face & 0x10) != 0;
    bool btn_a     = (face & 0x20) != 0;
    bool btn_b     = (face & 0x40) != 0;
    bool btn_y     = (face & 0x80) != 0;
    bool btn_lb    = (b5 & 0x01) != 0;
    bool btn_rb    = (b5 & 0x02) != 0;
    bool btn_lt    = (b5 & 0x04) != 0;
    bool btn_rt    = (b5 & 0x08) != 0;
    bool btn_back  = (b5 & 0x10) != 0;
    bool btn_start = (b5 & 0x20) != 0;

    if (up != gamepad1_bits.up) {
        joyPushData(fabgl::VirtualKey::VK_MENU_UP, up);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_UP, up);
    }
    if (down != gamepad1_bits.down) {
        joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, down);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_DOWN, down);
    }
    if (left != gamepad1_bits.left) {
        joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, left);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_LEFT, left);
    }
    if (right != gamepad1_bits.right) {
        joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, right);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_RIGHT, right);
    }

    if (btn_a != gamepad1_bits.b) {
        joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, btn_a);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_FIRE, btn_a);
    }
    if (btn_b != gamepad1_bits.a) {
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, btn_b);
    }
    if (btn_start != gamepad1_bits.start) {
        joyPushData(fabgl::VirtualKey::VK_MENU_HOME, btn_start);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_START, btn_start);
    }
    if (btn_back != gamepad1_bits.select) {
        joyPushData(fabgl::VirtualKey::VK_MENU_BS, btn_back);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_SELECT, btn_back);
    }

    static bool prev_x = false, prev_y = false, prev_lb = false, prev_rb = false, prev_lt = false, prev_rt = false;
    if (btn_x  != prev_x)  { kbdPushData(fabgl::VirtualKey::VK_JOY_X, btn_x);  prev_x  = btn_x; }
    if (btn_y  != prev_y)  { kbdPushData(fabgl::VirtualKey::VK_JOY_Y, btn_y);  prev_y  = btn_y; }
    if (btn_lb != prev_lb) { kbdPushData(fabgl::VirtualKey::VK_JOY_Z, btn_lb); prev_lb = btn_lb; }
    if (btn_rb != prev_rb) { kbdPushData(fabgl::VirtualKey::VK_JOY_C, btn_rb); prev_rb = btn_rb; }
    if (btn_lt != prev_lt) { kbdPushData(fabgl::VirtualKey::VK_JOY_L2, btn_lt); prev_lt = btn_lt; }
    if (btn_rt != prev_rt) { kbdPushData(fabgl::VirtualKey::VK_JOY_R2, btn_rt); prev_rt = btn_rt; }

    gamepad1_bits.up     = up;
    gamepad1_bits.down   = down;
    gamepad1_bits.left   = left;
    gamepad1_bits.right  = right;
    gamepad1_bits.a      = btn_b;
    gamepad1_bits.b      = btn_a;
    gamepad1_bits.start  = btn_start;
    gamepad1_bits.select = btn_back;

    uint16_t dec = 0;
    if (up) dec |= HID_DEC_UP;       if (down) dec |= HID_DEC_DOWN;
    if (left) dec |= HID_DEC_LEFT;   if (right) dec |= HID_DEC_RIGHT;
    if (btn_a) dec |= HID_DEC_A;         if (btn_b) dec |= HID_DEC_B;
    if (btn_x) dec |= HID_DEC_X;         if (btn_y) dec |= HID_DEC_Y;
    if (btn_lb) dec |= HID_DEC_L;        if (btn_rb) dec |= HID_DEC_R;
    if (btn_start) dec |= HID_DEC_START; if (btn_back) dec |= HID_DEC_SELECT;
    hid_snap_set_decoded(instance, HID_HANDLER_F710_DINPUT, dec);
}

//--------------------------------------------------------------------+
// Generic clone gamepad (VID 2563 PID 0575) — DirectInput-style 7-byte
// simple report (no id prefix). Layout from frank-nes known_hid_maps.
//--------------------------------------------------------------------+
static void process_gp_2563_0575(uint8_t instance, uint8_t const* report, uint16_t len)
{
    if (len < 7) return;

    uint8_t b0  = report[0];
    uint8_t b1  = report[1];
    uint8_t hat = report[2] & 0x0F;
    uint8_t lx  = report[3];
    uint8_t ly  = report[4];

    bool stick_left  = lx < 0x40;
    bool stick_right = lx > 0xC0;
    bool stick_up    = ly < 0x40;
    bool stick_down  = ly > 0xC0;

    bool hat_up    = (hat == 7 || hat == 0 || hat == 1);
    bool hat_right = (hat == 1 || hat == 2 || hat == 3);
    bool hat_down  = (hat == 3 || hat == 4 || hat == 5);
    bool hat_left  = (hat == 5 || hat == 6 || hat == 7);
    if (hat >= 8) { hat_up = hat_down = hat_left = hat_right = false; }

    bool up    = stick_up    || hat_up;
    bool down  = stick_down  || hat_down;
    bool left  = stick_left  || hat_left;
    bool right = stick_right || hat_right;

    bool btn_y     = (b0 & 0x01) != 0;
    bool btn_b     = (b0 & 0x02) != 0;
    bool btn_a     = (b0 & 0x04) != 0;
    bool btn_x     = (b0 & 0x08) != 0;
    bool btn_l     = (b0 & 0x10) != 0;
    bool btn_r     = (b0 & 0x20) != 0;
    bool btn_select = (b1 & 0x01) != 0;
    bool btn_start  = (b1 & 0x02) != 0;

    if (up != gamepad1_bits.up) {
        joyPushData(fabgl::VirtualKey::VK_MENU_UP, up);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_UP, up);
    }
    if (down != gamepad1_bits.down) {
        joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, down);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_DOWN, down);
    }
    if (left != gamepad1_bits.left) {
        joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, left);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_LEFT, left);
    }
    if (right != gamepad1_bits.right) {
        joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, right);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_RIGHT, right);
    }

    if (btn_a != gamepad1_bits.b) {
        joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, btn_a);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_FIRE, btn_a);
    }
    if (btn_b != gamepad1_bits.a) {
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, btn_b);
    }
    if (btn_start != gamepad1_bits.start) {
        joyPushData(fabgl::VirtualKey::VK_MENU_HOME, btn_start);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_START, btn_start);
    }
    if (btn_select != gamepad1_bits.select) {
        joyPushData(fabgl::VirtualKey::VK_MENU_BS, btn_select);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_SELECT, btn_select);
    }

    static bool prev_x = false, prev_y = false, prev_l = false, prev_r = false;
    if (btn_x != prev_x) { kbdPushData(fabgl::VirtualKey::VK_JOY_X, btn_x); prev_x = btn_x; }
    if (btn_y != prev_y) { kbdPushData(fabgl::VirtualKey::VK_JOY_Y, btn_y); prev_y = btn_y; }
    if (btn_l != prev_l) { kbdPushData(fabgl::VirtualKey::VK_JOY_Z, btn_l); prev_l = btn_l; }
    if (btn_r != prev_r) { kbdPushData(fabgl::VirtualKey::VK_JOY_C, btn_r); prev_r = btn_r; }

    gamepad1_bits.up     = up;
    gamepad1_bits.down   = down;
    gamepad1_bits.left   = left;
    gamepad1_bits.right  = right;
    gamepad1_bits.a      = btn_b;
    gamepad1_bits.b      = btn_a;
    gamepad1_bits.start  = btn_start;
    gamepad1_bits.select = btn_select;

    uint16_t dec = 0;
    if (up) dec |= HID_DEC_UP;       if (down) dec |= HID_DEC_DOWN;
    if (left) dec |= HID_DEC_LEFT;   if (right) dec |= HID_DEC_RIGHT;
    if (btn_a) dec |= HID_DEC_A;         if (btn_b) dec |= HID_DEC_B;
    if (btn_x) dec |= HID_DEC_X;         if (btn_y) dec |= HID_DEC_Y;
    if (btn_l) dec |= HID_DEC_L;         if (btn_r) dec |= HID_DEC_R;
    if (btn_start) dec |= HID_DEC_START; if (btn_select) dec |= HID_DEC_SELECT;
    hid_snap_set_decoded(instance, HID_HANDLER_GP_2563_0575, dec);
}

//--------------------------------------------------------------------+
// "FEED:2320" — clone gamepad. Composite report id=0x07 already stripped.
//--------------------------------------------------------------------+
static void process_gp_feed_2320(uint8_t instance, uint8_t const* report, uint16_t len)
{
    if (len < 7) return;

    uint8_t lx  = report[0];
    uint8_t ly  = report[1];
    uint8_t hat = report[4] & 0x0F;
    uint8_t b5  = report[5];
    uint8_t b6  = report[6];

    bool stick_left  = lx < 0x40;
    bool stick_right = lx > 0xC0;
    bool stick_up    = ly < 0x40;
    bool stick_down  = ly > 0xC0;

    bool hat_up    = (hat == 7 || hat == 0 || hat == 1);
    bool hat_right = (hat == 1 || hat == 2 || hat == 3);
    bool hat_down  = (hat == 3 || hat == 4 || hat == 5);
    bool hat_left  = (hat == 5 || hat == 6 || hat == 7);
    if (hat >= 8) { hat_up = hat_down = hat_left = hat_right = false; }

    bool up    = stick_up    || hat_up;
    bool down  = stick_down  || hat_down;
    bool left  = stick_left  || hat_left;
    bool right = stick_right || hat_right;

    bool btn_a     = (b5 & 0x01) != 0;
    bool btn_b     = (b5 & 0x02) != 0;
    bool btn_y     = (b5 & 0x04) != 0;
    bool btn_x     = (b5 & 0x08) != 0;
    bool btn_l     = (b5 & 0x10) != 0;
    bool btn_r     = (b5 & 0x20) != 0;
    bool btn_select = (b6 & 0x04) != 0;
    bool btn_start  = (b6 & 0x08) != 0;

    if (up != gamepad1_bits.up) {
        joyPushData(fabgl::VirtualKey::VK_MENU_UP, up);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_UP, up);
    }
    if (down != gamepad1_bits.down) {
        joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, down);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_DOWN, down);
    }
    if (left != gamepad1_bits.left) {
        joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, left);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_LEFT, left);
    }
    if (right != gamepad1_bits.right) {
        joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, right);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_RIGHT, right);
    }

    if (btn_a != gamepad1_bits.b) {
        joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, btn_a);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_FIRE, btn_a);
    }
    if (btn_b != gamepad1_bits.a) {
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, btn_b);
    }
    if (btn_start != gamepad1_bits.start) {
        joyPushData(fabgl::VirtualKey::VK_MENU_HOME, btn_start);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_START, btn_start);
    }
    if (btn_select != gamepad1_bits.select) {
        joyPushData(fabgl::VirtualKey::VK_MENU_BS, btn_select);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_SELECT, btn_select);
    }

    static bool prev_x = false, prev_y = false, prev_l = false, prev_r = false;
    // FEED:2320 button remap (identity). Physical bit assignment deduced from hw:
    // physical Y = btn_l, physical C = btn_y, physical Z = btn_r. X unchanged.
    if (btn_x != prev_x) { kbdPushData(fabgl::VirtualKey::VK_JOY_X, btn_x); prev_x = btn_x; }
    if (btn_l != prev_l) { kbdPushData(fabgl::VirtualKey::VK_JOY_Y, btn_l); prev_l = btn_l; }
    if (btn_y != prev_y) { kbdPushData(fabgl::VirtualKey::VK_JOY_C, btn_y); prev_y = btn_y; }
    if (btn_r != prev_r) { kbdPushData(fabgl::VirtualKey::VK_JOY_Z, btn_r); prev_r = btn_r; }

    gamepad1_bits.up     = up;
    gamepad1_bits.down   = down;
    gamepad1_bits.left   = left;
    gamepad1_bits.right  = right;
    gamepad1_bits.a      = btn_b;
    gamepad1_bits.b      = btn_a;
    gamepad1_bits.start  = btn_start;
    gamepad1_bits.select = btn_select;

    uint16_t dec = 0;
    if (up) dec |= HID_DEC_UP;       if (down) dec |= HID_DEC_DOWN;
    if (left) dec |= HID_DEC_LEFT;   if (right) dec |= HID_DEC_RIGHT;
    if (btn_a) dec |= HID_DEC_A;         if (btn_b) dec |= HID_DEC_B;
    if (btn_x) dec |= HID_DEC_X;         if (btn_y) dec |= HID_DEC_Y;
    if (btn_l) dec |= HID_DEC_L;         if (btn_r) dec |= HID_DEC_R;
    if (btn_start) dec |= HID_DEC_START; if (btn_select) dec |= HID_DEC_SELECT;
    hid_snap_set_decoded(instance, HID_HANDLER_GP_FEED_2320, dec);
}

//--------------------------------------------------------------------+
// "Game Stick Lite" 0810:0001 — DragonRise wireless clone.
// Called with Report ID already stripped; payload is 7 bytes.
//--------------------------------------------------------------------+
static void process_gp_0810_0001(uint8_t instance, uint8_t const* report, uint16_t len)
{
    // Report layout (Report ID stripped, 7 bytes follow):
    // [0] unused (0x80)
    // [1] unused (0x80)
    // [2] LX / crosspad X: 0x00=left, 0x80=center, 0xFF=right
    // [3] LY / crosspad Y: 0x00=up,   0x80=center, 0xFF=down
    // [4] high-nibble: R-stick+face (0x1=up/Y, 0x2=right/B, 0x4=down/A, 0x8=left/X)
    //     low-nibble:  hat (0xF=neutral, unused since sticks cover directions)
    // [5] LB=0x01, RB=0x02, LT=0x04, RT=0x08, Select=0x10, Start=0x20
    // [6] reserved
    if (len < 7) return;

    uint8_t lx  = report[2];
    uint8_t ly  = report[3];
    uint8_t b4  = report[4];
    uint8_t b5  = report[5];

    // Left stick / crosspad directions
    bool up    = ly < 0x40;
    bool down  = ly > 0xC0;
    bool left  = lx < 0x40;
    bool right = lx > 0xC0;

    // Face buttons (high nibble of byte[4])
    // Per user report: Y=0x10, B=0x20, A=0x40, X=0x80
    bool btn_y      = (b4 & 0x10) != 0;
    bool btn_b      = (b4 & 0x20) != 0;
    bool btn_a      = (b4 & 0x40) != 0;
    bool btn_x      = (b4 & 0x80) != 0;

    // Shoulder buttons and system
    bool btn_lb     = (b5 & 0x01) != 0;
    bool btn_rb     = (b5 & 0x02) != 0;
    bool btn_lt     = (b5 & 0x04) != 0;
    bool btn_rt     = (b5 & 0x08) != 0;
    bool btn_select = (b5 & 0x10) != 0;
    bool btn_start  = (b5 & 0x20) != 0;

    // btn_lt and btn_rt are now mapped to L2/R2

    // D-pad + OSD navigation (edge-detected)
    if (up != gamepad1_bits.up) {
        joyPushData(fabgl::VirtualKey::VK_MENU_UP, up);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_UP, up);
    }
    if (down != gamepad1_bits.down) {
        joyPushData(fabgl::VirtualKey::VK_MENU_DOWN, down);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_DOWN, down);
    }
    if (left != gamepad1_bits.left) {
        joyPushData(fabgl::VirtualKey::VK_MENU_LEFT, left);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_LEFT, left);
    }
    if (right != gamepad1_bits.right) {
        joyPushData(fabgl::VirtualKey::VK_MENU_RIGHT, right);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_RIGHT, right);
    }

    // A = fire, B = alt-fire (PlayStation-style: Cross=confirm, Circle=back)
    if (btn_a != gamepad1_bits.b) {
        joyPushData(fabgl::VirtualKey::VK_MENU_ENTER, btn_a);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_FIRE, btn_a);
    }
    if (btn_b != gamepad1_bits.a) {
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_ALTFIRE, btn_b);
    }
    if (btn_start != gamepad1_bits.start) {
        joyPushData(fabgl::VirtualKey::VK_MENU_HOME, btn_start);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_START, btn_start);
    }
    if (btn_select != gamepad1_bits.select) {
        joyPushData(fabgl::VirtualKey::VK_MENU_BS, btn_select);
        if (Config::secondJoy != 1) joyPushData(fabgl::VirtualKey::VK_DPAD_SELECT, btn_select);
    }

    // X / Y / L / R / LT / RT → rebindable extras
    static bool prev_x = false, prev_y = false, prev_l = false, prev_r = false, prev_lt = false, prev_rt = false;
    if (btn_x  != prev_x)  { kbdPushData(fabgl::VirtualKey::VK_JOY_X,  btn_x);  prev_x  = btn_x; }
    if (btn_y  != prev_y)  { kbdPushData(fabgl::VirtualKey::VK_JOY_Y,  btn_y);  prev_y  = btn_y; }
    if (btn_lb != prev_l)  { kbdPushData(fabgl::VirtualKey::VK_JOY_Z,  btn_lb); prev_l  = btn_lb; }
    if (btn_rb != prev_r)  { kbdPushData(fabgl::VirtualKey::VK_JOY_C,  btn_rb); prev_r  = btn_rb; }
    if (btn_lt != prev_lt) { kbdPushData(fabgl::VirtualKey::VK_JOY_L2, btn_lt); prev_lt = btn_lt; }
    if (btn_rt != prev_rt) { kbdPushData(fabgl::VirtualKey::VK_JOY_R2, btn_rt); prev_rt = btn_rt; }

    gamepad1_bits.up     = up;
    gamepad1_bits.down   = down;
    gamepad1_bits.left   = left;
    gamepad1_bits.right  = right;
    gamepad1_bits.a      = btn_a;
    gamepad1_bits.b      = btn_b;
    gamepad1_bits.start  = btn_start;
    gamepad1_bits.select = btn_select;

    uint16_t dec = 0;
    if (up)         dec |= HID_DEC_UP;     if (down)       dec |= HID_DEC_DOWN;
    if (left)       dec |= HID_DEC_LEFT;   if (right)      dec |= HID_DEC_RIGHT;
    if (btn_a)      dec |= HID_DEC_A;      if (btn_b)      dec |= HID_DEC_B;
    if (btn_x)      dec |= HID_DEC_X;      if (btn_y)      dec |= HID_DEC_Y;
    if (btn_lb)     dec |= HID_DEC_L;      if (btn_rb)     dec |= HID_DEC_R;
    if (btn_start)  dec |= HID_DEC_START;  if (btn_select) dec |= HID_DEC_SELECT;
    hid_snap_set_decoded(instance, HID_HANDLER_GP_0810_0001, dec);
}

//--------------------------------------------------------------------+
// Consumer Control (multimedia keys)
//--------------------------------------------------------------------+

static uint16_t prev_consumer_usage = 0;

static void process_consumer_report(uint8_t const* report, uint16_t len)
{
  uint16_t usage = 0;
  if (len >= 2) {
    usage = (uint16_t)(report[0] | (report[1] << 8));
  } else if (len == 1) {
    usage = report[0];
  }

  if (prev_consumer_usage && prev_consumer_usage != usage) {
    switch (prev_consumer_usage) {
      case HID_USAGE_CONSUMER_VOLUME_INCREMENT: kbdPushData(fabgl::VK_VOLUMEUP, false); break;
      case HID_USAGE_CONSUMER_VOLUME_DECREMENT: kbdPushData(fabgl::VK_VOLUMEDOWN, false); break;
      case HID_USAGE_CONSUMER_MUTE:             kbdPushData(fabgl::VK_VOLUMEMUTE, false); break;
      default: break;
    }
  }

  if (usage && usage != prev_consumer_usage) {
    switch (usage) {
      case HID_USAGE_CONSUMER_VOLUME_INCREMENT: kbdPushData(fabgl::VK_VOLUMEUP, true); break;
      case HID_USAGE_CONSUMER_VOLUME_DECREMENT: kbdPushData(fabgl::VK_VOLUMEDOWN, true); break;
      case HID_USAGE_CONSUMER_MUTE:             kbdPushData(fabgl::VK_VOLUMEMUTE, true); break;
      default: break;
    }
  }

  prev_consumer_usage = usage;
}

//--------------------------------------------------------------------+
// Generic Report
//--------------------------------------------------------------------+
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) dev_addr;

  if (is_dualshock4(hid_info[instance].vid, hid_info[instance].pid)
      && len >= 10 && report[0] == 0x01) {
    process_ds4_gamepad(instance, report + 1, len - 1);
    return;
  }
  if (is_dualsense(hid_info[instance].vid, hid_info[instance].pid)
      && len >= 11 && report[0] == 0x01) {
    process_ds5_gamepad(instance, report + 1, len - 1);
    return;
  }

  if (is_logitech_f710_dinput(hid_info[instance].vid, hid_info[instance].pid)
      && len >= 8 && report[0] == 0x01) {
    process_f710_dinput(instance, report + 1, len - 1);
    return;
  }

  if (is_gp_2563_0575(hid_info[instance].vid, hid_info[instance].pid)
      && len >= 7) {
    process_gp_2563_0575(instance, report, len);
    return;
  }

  if (is_gp_feed_2320(hid_info[instance].vid, hid_info[instance].pid)
      && len >= 8 && report[0] == 0x07) {
    process_gp_feed_2320(instance, report + 1, len - 1);
    return;
  }

  if (is_gp_0810_0001(hid_info[instance].vid, hid_info[instance].pid)
      && len >= 8 && (report[0] == 0x01 || report[0] == 0x02)) {
    process_gp_0810_0001(instance, report + 1, len - 1);
    return;
  }

  uint8_t const rpt_count = hid_info[instance].report_count;
  tuh_hid_report_info_t* rpt_info_arr = hid_info[instance].report_info;
  tuh_hid_report_info_t* rpt_info = NULL;

  if ( rpt_count == 1 && rpt_info_arr[0].report_id == 0)
  {
    // Simple report without report ID as 1st byte
    rpt_info = &rpt_info_arr[0];
  }else
  {
    // Composite report, 1st byte is report ID, data starts from 2nd byte
    uint8_t const rpt_id = report[0];

    // Find report id in the array
    for(uint8_t i=0; i<rpt_count; i++)
    {
      if (rpt_id == rpt_info_arr[i].report_id )
      {
        rpt_info = &rpt_info_arr[i];
        break;
      }
    }

    report++;
    len--;
  }

  if (!rpt_info)
  {
    // ONCE per instance, never per report: this sits in the HID data path, which
    // some devices drive at up to 1 kHz, and printf goes to a blocking 115200 UART
    // when <BOARD>_DBG_UART is on (~4 ms per line — enough to stall the whole main
    // loop and look like a freeze). The OSD's HID diagnostics page already reports
    // the condition via last_handler, so the line is only a bring-up hint.
    if (instance < CFG_TUH_HID && !no_rpt_info_logged[instance]) {
      no_rpt_info_logged[instance] = true;
      // report was advanced past the id byte in the composite branch above — the
      // only branch that can leave rpt_info NULL — so report[-1] is that id.
      printf("HID instance %u %04X:%04X itf_protocol=%u: no report info for id %02X, len=%u "
             "(%u parsed; further ones silenced)\r\n",
             instance, hid_info[instance].vid, hid_info[instance].pid,
             hid_snap[instance].itf_protocol, report[-1], (unsigned)(len + 1),
             hid_info[instance].report_count);
    }
    hid_snap_set_handler(instance, HID_HANDLER_NO_RPT_INFO);
    return;
  }

  // For complete list of Usage Page & Usage checkout src/class/hid/hid.h. For examples:
  // - Keyboard                     : Desktop, Keyboard
  // - Mouse                        : Desktop, Mouse
  // - Gamepad                      : Desktop, Gamepad
  // - Consumer Control (Media Key) : Consumer, Consumer Control
  // - System Control (Power key)   : Desktop, System Control
  // - Generic (vendor)             : 0xFFxx, xx
  if ( rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP )
  {
    switch (rpt_info->usage)
    {
      case HID_USAGE_DESKTOP_KEYBOARD:
        TU_LOG1("HID receive keyboard report\r\n");
        hid_snap_set_handler(instance, HID_HANDLER_KBD);
        // Assume keyboard follow boot report layout
        process_kbd_report( (hid_keyboard_report_t const*) report, &prev_report );
        prev_report = *(hid_keyboard_report_t const*)report;
      break;

      case HID_USAGE_DESKTOP_MOUSE:
        TU_LOG1("HID receive mouse report\r\n");
        hid_snap_set_handler(instance, HID_HANDLER_MOUSE);
        // Assume mouse follow boot report layout
        process_mouse_report( (hid_mouse_report_t const*) report, len );
      break;

      case HID_USAGE_DESKTOP_GAMEPAD:
      case HID_USAGE_DESKTOP_JOYSTICK:
        hid_snap_set_handler(instance, HID_HANDLER_GENERIC_GAMEPAD);
        process_hid_gamepad(report, len);
      break;

      default:
        hid_snap_set_handler(instance, HID_HANDLER_UNKNOWN_USAGE);
        break;
    }
  }
  else if ( rpt_info->usage_page == HID_USAGE_PAGE_CONSUMER && rpt_info->usage == HID_USAGE_CONSUMER_CONTROL )
  {
    hid_snap_set_handler(instance, HID_HANDLER_GENERIC_CONS);
    process_consumer_report(report, len);
  }
  else
  {
    hid_snap_set_handler(instance, HID_HANDLER_UNKNOWN_USAGE);
  }
}

//--------------------------------------------------------------------+
// HID devices info dump (for OSD "HID devices" dialog)
//--------------------------------------------------------------------+
extern "C" int hid_app_format_devices_info(char* buf, int bufsz)
{
  static const char* proto_name[] = { "Generic", "Keyboard", "Mouse" };
  int pos = 0;
  int found = 0;

  for (uint8_t inst = 0; inst < CFG_TUH_HID; inst++) {
    const hid_snap_t& s = hid_snap[inst];
    if (!s.mounted) continue;
    found++;

    const char* pname = (s.itf_protocol < 3) ? proto_name[s.itf_protocol] : "?";

    static const char* handler_name[] = {
      "(no rpt yet)",   // HID_HANDLER_NONE
      "kbd",            // KBD
      "mouse",          // MOUSE
      "DS4",            // DS4
      "DS5",            // DS5
      "F710 D-input",   // F710_DINPUT
      "GP 2563:0575",   // GP_2563_0575
      "GP FEED:2320",   // GP_FEED_2320
      "GP 0810:0001",   // GP_0810_0001
      "generic gamepad",// GENERIC_GAMEPAD
      "consumer ctrl",  // GENERIC_CONS
      "no rpt info!",   // NO_RPT_INFO
      "unknown usage",  // UNKNOWN_USAGE
    };
    const char* hname = (s.last_handler < (sizeof(handler_name)/sizeof(handler_name[0])))
                       ? handler_name[s.last_handler] : "?";

    pos += snprintf(buf + pos, bufsz - pos,
      "Inst %u  addr=%u  %s\n"
      " VID:PID = %04X:%04X\n"
      " handler = %s\n"
      " desc len = %u\n",
      inst, s.dev_addr, pname,
      s.vid, s.pid, hname, s.desc_len);

    if (s.decoded_valid) {
      uint16_t d = s.decoded;
      pos += snprintf(buf + pos, bufsz - pos,
        " dec: U%u D%u L%u R%u\n"
        "      A%u B%u X%u Y%u\n"
        "      LB%u RB%u ST%u SE%u\n",
        !!(d & HID_DEC_UP), !!(d & HID_DEC_DOWN),
        !!(d & HID_DEC_LEFT), !!(d & HID_DEC_RIGHT),
        !!(d & HID_DEC_A), !!(d & HID_DEC_B),
        !!(d & HID_DEC_X), !!(d & HID_DEC_Y),
        !!(d & HID_DEC_L), !!(d & HID_DEC_R),
        !!(d & HID_DEC_START), !!(d & HID_DEC_SELECT));
    }

    if (s.desc_saved) {
      pos += snprintf(buf + pos, bufsz - pos, " desc[0..%u]:\n ", s.desc_saved - 1);
      for (uint8_t i = 0; i < s.desc_saved && pos < bufsz - 8; i++) {
        pos += snprintf(buf + pos, bufsz - pos, "%02X ", s.desc[i]);
        if ((i & 0x07) == 0x07 && i + 1 < s.desc_saved) {
          pos += snprintf(buf + pos, bufsz - pos, "\n ");
        }
      }
      pos += snprintf(buf + pos, bufsz - pos, "\n");
    }

    if (s.itf_protocol == HID_ITF_PROTOCOL_NONE) {
      pos += snprintf(buf + pos, bufsz - pos, " parsed reports: %u\n", hid_info[inst].report_count);
      for (uint8_t i = 0; i < hid_info[inst].report_count; i++) {
        pos += snprintf(buf + pos, bufsz - pos,
          "  rpt[%u] id=%u page=%04X usage=%04X\n",
          i,
          hid_info[inst].report_info[i].report_id,
          hid_info[inst].report_info[i].usage_page,
          hid_info[inst].report_info[i].usage);
      }
    }

    pos += snprintf(buf + pos, bufsz - pos,
      " reports rx = %lu  last len = %u\n",
      (unsigned long)s.report_total, s.last_report_len);

    if (s.last_report_saved) {
      pos += snprintf(buf + pos, bufsz - pos, " last[0..%u]:\n ", s.last_report_saved - 1);
      for (uint8_t i = 0; i < s.last_report_saved && pos < bufsz - 8; i++) {
        pos += snprintf(buf + pos, bufsz - pos, "%02X ", s.last_report[i]);
        if ((i & 0x07) == 0x07 && i + 1 < s.last_report_saved) {
          pos += snprintf(buf + pos, bufsz - pos, "\n ");
        }
      }
      pos += snprintf(buf + pos, bufsz - pos, "\n");
    }

    pos += snprintf(buf + pos, bufsz - pos, "\n");
    if (pos >= bufsz - 64) break;
  }

  return pos;
}
