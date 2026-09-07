#pragma once

// On-chip WiFi (MURM_W / MURM2_W: Raspberry Pi Radio Module 2 = CYW43439) with
// lwIP, in poll mode.
//
// This is the station/SNTP half of the on-chip network transport; WifiSock is the
// TCP half. Neither is called by the application layer directly: `ZiFiAT` and
// `ZiFiSock` stay the public faces and dispatch here whenever
// Config::zifi_transport == 2 ("On-chip WiFi" in Network -> Transport), so FTP,
// SSH, HTTPS, the catalog and the FTP server work unchanged on either radio.
//
// Compiled out entirely when PICOSPECCY_WIFI is 0: src/*.cpp is swept up by a
// GLOB in CMakeLists.txt, exactly like the `#if ZIFI_NET_CLIENT` sources.

#if PICOSPECCY_WIFI

#include <stdint.h>
#include <stddef.h>
#include <string>

namespace WifiNet {

// Bring up the CYW43 bus + lwIP. Call ONCE, from main(), only AFTER core1 has
// finished graphics_init() (HDMI owns pio2) and after the Config::cpu_mhz clock
// switch (the bus divider is derived from clk_sys at init). It pins pio0 to
// gpio_base 16 itself before the SDK picks a block. Failure is logged, never
// fatal. Returns true when the radio answered.
bool init();

// True once init() has succeeded (the radio and lwIP are up; not "connected").
bool ready();

// True when the on-chip radio is the configured network transport
// (Config::zifi_transport == 2). ZiFiAT/ZiFiSock dispatch on this.
bool selected();

// Service the driver + lwIP. Cheap when idle. Called once per emulated frame
// from ESPectrum::loop and inside every blocking wait of WifiNet/WifiSock.
void poll();

// ── Station ──────────────────────────────────────────────────────────────────
// Join `ssid` (empty/NULL password = open network) and wait for DHCP, up to
// timeout_ms. 0 = up with an IP; PICO_ERROR_TIMEOUT / a negative CYW43_LINK_*
// code otherwise. Blocking, pumps poll(); OSD/main-thread only.
int  connect(const char* ssid, const char* pass, uint32_t timeout_ms);
void disconnect();
bool isConnected();                        // associated AND has an IP
bool ipString(char* out, size_t cap);      // dotted IP; false when not connected
const char* ssid();                        // the SSID we joined (or tried to)

// Active scan, blocking up to timeout_ms; fills out[] with up to maxn unique
// SSIDs (strongest first as they arrive), returns the count.
int  scan(std::string* out, int maxn, uint32_t timeout_ms);

// ── SNTP (own 48-byte UDP client; pool.ntp.org) ──────────────────────────────
// Blocking: DNS + query with retries, up to ~12 s. On success pushes the time
// into RTC::setDateTime (tz = hours) and fills out_str "YYYY-MM-DD HH:MM:SS".
bool sntpSync(int tz, std::string& out_str);

// Non-blocking boot-time variant (join + SNTP), one step per autoPoll():
void autoBegin(const char* ssid, const char* pass, int tz);
void autoPoll();
bool autoBusy();

// Optional line sink for the UI (the same right-pane log ZiFiAT feeds).
typedef void (*LogCb)(const char* line);
void setLog(LogCb cb);

// The user LED on the radio module itself (LED1). No-op until ready().
void ledSet(bool on);

// Log which PIO block / state machines and DMA channels are claimed right now.
// The interesting moment is immediately before and after init(): it is the only
// direct evidence that the radio landed on the block the board expects, and a
// permanent regression detector for the gpio_base story. `when` labels the line.
void logPioDmaClaims(const char* when);

}  // namespace WifiNet

#endif  // PICOSPECCY_WIFI
