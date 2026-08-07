/*
ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or
visit https://zxespectrum.speccy.org/contacto

*/
#include <malloc.h>
#include <hardware/watchdog.h>
#include <hardware/uart.h>
#include <hardware/clocks.h>
#include <hardware/flash.h>
#include <hardware/vreg.h>
#include <pico/bootrom.h>
#include <pico/multicore.h>

#include "OSDMain.h"
#include "FileUtils.h"
#include "Subsystem.h"
#include "CPU.h"
#include "Video.h"
#include "Z80DMA.h"
#include "ESPectrum.h"
#include "messages.h"
#include "Config.h"
#include "Debug.h"
#include "Snapshot.h"
#include "MemESP.h"
#include "AlfCart.h"
#include "Buffer.h"
#include "Tape.h"
#include "LEDIndicators.h"
#include "sdcard.h"
#include "ZipExtract.h"
#include "pwm_audio.h"
#include "Z80_JLS/z80.h"
#include "roms.h"
#include "ff.h"
#include "diskio.h"
#include "psram_spi.h"
#include "Ports.h"
#include "audio.h"
#include "AySound.h"
#include "Midi.h"
#include "MidiSynth.h"
#include "dls_conv.h"
#include "ZiFi.h"
#include "ZiFiAT.h"
#include "UsbMsc.h"
#include "BoardPins.h"
#include "graphics.h"
#include "ui/OSDNewMenu.h"
#include "ui/UiBrowser.h"
#include "ui/UiDialog.h"
#include "ui/UiGfx.h"
#include "ui/UiStrings.h"
#include "ui/UiActions.h"   // act_debugPoke — the new Input-poke flow
#if ZIFI_NET_CLIENT
#include "ZiFiSock.h"
#include "RemoteFs.h"
#include "Ftp.h"
#include "Sftp.h"
#include "Ssh.h"
#include "HttpCatalogFs.h"
#include "HttpsGet.h"
#include "Ftpd.h"
#include "HttpCatalogFs.h"
#include "HttpsGet.h"
#endif
#include "kbd_img.h"

// GM.DLS on-device .dls -> bank conversion progress: throttle OSD redraws to whole
// percent steps so the long (~tens of s) encode shows life without flickering.
static int s_dlsConvLastPct = -1;
static void osdDlsConvProgress(int pct, void* /*user*/) {
    if (pct == s_dlsConvLastPct) return;
    s_dlsConvLastPct = pct;
    // Under the new chrome this lands in the menu/browser footer status line (the
    // progressOverride hook), where every other long operation reports.
    if (OSD::progressOverride) {
        OSD::progressDialog(MSG_MIDI_CONVERTING, "", pct, 1);
        return;
    }
    OSD::osdCenteredMsg(string(MSG_MIDI_CONVERTING) + " " + std::to_string(pct) + "%",
                        LEVEL_INFO, 0);
}

// Convert a .dls (full SD path) to a GMWB bank named <stem>.bin in CONFIG_DIR, with
// on-screen progress. Returns the bank path on success, or "" on failure / too-large-
// for-flash (shows the matching message itself). Does NOT touch Config — the caller
// selects the bank + drives provisioning. Shared by the MIDI menu and the F5 browser.
static string osdConvertDlsToBank(const string& dlsPath) {
    string base = dlsPath;
    size_t slash = base.find_last_of('/');
    if (slash != string::npos) base.erase(0, slash + 1);   // basename
    size_t dot = base.rfind('.');
    if (dot != string::npos) base.erase(dot);              // strip extension
    string outBin = string(CONFIG_DIR) + "/" + base + ".bin";

    s_dlsConvLastPct = -1;
    if (OSD::progressOverride)
        OSD::progressDialog(MSG_MIDI_CONVERTING, "", 0, 0);   // footer, not a modal box
    else
        OSD::osdCenteredMsg(string(MSG_MIDI_CONVERTING) + " 0%", LEVEL_INFO, 0);
    const bool convOk = DlsConv::convert(dlsPath.c_str(), outBin.c_str(), 31250,
                                         osdDlsConvProgress, nullptr);
    if (OSD::progressOverride) OSD::progressDialog(MSG_MIDI_CONVERTING, "", 100, 2); // close
    if (!convOk) {
        OSD::osdCenteredMsg(MSG_MIDI_CONVERT_FAIL, LEVEL_WARN, 3000);
        return "";
    }
    // A bank larger than the flash partition is written to SD but can never be
    // installed (scanBanks/provision reject it) — delete it and warn, don't select it.
    size_t bankSz = 0;
    FIL* bf = fopen2(outBin.c_str(), FA_READ);
    if (bf) { bankSz = (size_t)f_size(bf); fclose2(bf); }
    size_t cap = MidiSynth::flashBankCapacity();
    if (bankSz > cap) {
        f_unlink(outBin.c_str());
        OSD::osdCenteredMsg(string(MSG_MIDI_BANK_TOOBIG) + " (" +
                            std::to_string(bankSz >> 10) + " > " + std::to_string(cap >> 10) + " KB)",
                            LEVEL_WARN, 4000);
        return "";
    }
    OSD::osdCenteredMsg(MSG_MIDI_CONVERT_OK, LEVEL_OK, 1500);
    return outBin;
}

string OSD::convertDlsToBank(const string& dlsPath) { return osdConvertDlsToBank(dlsPath); }

extern "C" void graphics_set_scanlines(uint8_t level);
extern "C" void graphics_set_dither(bool enabled);
extern "C" volatile bool profi_ds80_active;
extern "C" const uint32_t profi_default_palette16[16];
#include "DivMMC.h"
#include "IDE.h"
#include "MB02.h"
#include "MachineSwitch.h"
#include "GS/GS.h"
#include "RTC.h"

#include <malloc.h>

#include "PinSerialData_595.h"

#include <string>
#include <cstdio>
#include <cstdarg>   // infoAppend's vsnprintf

extern "C" uint8_t TFT_FLAGS;
extern "C" uint8_t TFT_INVERSION;

void fputs(const char* b, FIL& f);

using namespace std;

#define OSD_W 248
#define OSD_H 200
#define OSD_MARGIN 4

extern Font Font6x8;
extern Font Font6x8Cyr;   // CP1251 Cyrillic face (online-catalog names)
#ifdef VGA_HDMI
extern bool SELECT_VGA;
#endif

extern int ram_pages, butter_pages, psram_pages, swap_pages;

// Shared buffer for HWInfo/ChipInfo/BoardInfo/HID (never called concurrently).
// Emulator Info outgrew it (~2.3 KB worst case) and does NOT get it enlarged:
// permanent BSS is free heap taken from every configuration forever, and 1 KB is
// exactly the margin the SRAM budget gate fights over (Gigascreen + HDMI audio on
// a butter-less board misses by ~0.5 KB). It borrows a transient heap buffer
// instead — see emu_buf below.
#define OSD_INFO_BUF_SZ 1536
static char osd_info_buf[OSD_INFO_BUF_SZ] __attribute__((aligned(4)));

bool OSD::net_launch_close = false;

unsigned short OSD::scrW = 320;
unsigned short OSD::scrH = 240;

char OSD::stats_lin1[25]; // "CPU: 00000 / IDL: 00000 ";
char OSD::stats_lin2[25]; // "FPS:000.00 / FND:000.00 ";

// // X origin to center an element with pixel_width
unsigned short OSD::scrAlignCenterX(unsigned short pixel_width) { return (scrW / 2) - (pixel_width / 2); }

// // Y origin to center an element with pixel_height
unsigned short OSD::scrAlignCenterY(unsigned short pixel_height) { return (scrH / 2) - (pixel_height / 2); }

// Inline text editor — edits text directly at pixel position (ex, ey) in the current window.
// Draws each character individually; cursor shown as highlighted block under current char.
// Returns entered string on Enter, "\x1B" on Escape, "" if Enter pressed with empty field.
// Ignores VK_MENU_* synthetic events to avoid double-fires from kbdExtraMapping.
// US-layout shifted form of a symbol/digit. map_key() returns only unshifted symbol
// VKs (e.g. VK_MINUS), so Shift+key arrives as the base char — translate it here so
// '_', '!', '+', '?', etc. can be typed. Letters are handled separately (case).
// Non-static: the new UI's line editor (src/ui/UiDialog.cpp) uses the same map.
char shiftSymUS(char c) {
    switch (c) {
        case '-': return '_'; case '=': return '+';
        case '[': return '{'; case ']': return '}'; case '\\': return '|';
        case ';': return ':'; case '\'': return '"'; case '`': return '~';
        case ',': return '<'; case '.': return '>'; case '/': return '?';
        case '1': return '!'; case '2': return '@'; case '3': return '#';
        case '4': return '$'; case '5': return '%'; case '6': return '^';
        case '7': return '&'; case '8': return '*'; case '9': return '('; case '0': return ')';
        default:  return c;   // already shifted, or a non-shiftable char
    }
}

string OSD::inlineTextEdit(int ex, int ey, int maxlen, const string& initial_text, bool mask, int viscols) {
    if (viscols <= 0 || viscols > maxlen) viscols = maxlen; // visible window
    string text = initial_text;
    bool reveal = false; // password mask: false → show '*', toggled with TAB
    auto Kbd = ESPectrum::PS2Controller.keyboard();
    // Drain any keys still in the queue (e.g. the Enter that triggered the save action)
    { fabgl::VirtualKeyItem drain; while (Kbd->virtualKeyAvailable()) Kbd->getNextVirtualKey(&drain); }
    VIDEO::vga.setFont(Font6x8);

    uint8_t blinkCtr = 7; // triggers first draw immediately (++&0x7==0)

    auto redraw = [&](bool cursorOn) {
        int cur = (int)text.length();
        bool full = (cur >= maxlen);
        // Scroll so the cursor cell stays inside the visible window.
        int scroll = (cur >= viscols) ? (cur - viscols + 1) : 0;
        if (scroll > maxlen - viscols) scroll = maxlen - viscols;
        for (int p = 0; p < viscols; p++) {
            int idx = scroll + p;
            bool isCursor = (!full && idx == cur) || (full && p == viscols - 1);
            if (isCursor && cursorOn)
                VIDEO::vga.setTextColor(zxColor(7, 1), zxColor(1, 1));
            else
                VIDEO::vga.setTextColor(zxColor(0, 1), zxColor(5, 1));
            VIDEO::vga.setCursor(ex + p * OSD_FONT_W, ey);
            // Masked password: render '*' for the typed characters until revealed.
            char dch = (idx < cur) ? text[idx] : ' ';
            if (mask && !reveal && idx < cur) dch = '*';
            char ch[2] = { dch, 0 };
            VIDEO::vga.print(ch);
        }
    };

    redraw(true);

    while (1) {
        // Blink cursor while waiting for keypress
        while (!Kbd->virtualKeyAvailable()) {
            sleep_ms(5);
            if ((++blinkCtr & 0x7) == 0)
                redraw((blinkCtr & 0x20) == 0);
        }
        blinkCtr = 7; // next tick redraws with cursor on after keypress
        fabgl::VirtualKeyItem ek;
        Kbd->getNextVirtualKey(&ek);
        if (!ek.down) continue;
        // Skip synthetic VK_MENU_* events
        if (ek.vk >= fabgl::VK_MENU_UP && ek.vk <= fabgl::VK_MENU_BS) continue;
        // TAB toggles password reveal (only relevant in masked fields).
        if (ek.vk == fabgl::VK_TAB) { if (mask) { reveal = !reveal; redraw(true); } continue; }
        if (ek.vk == fabgl::VK_RETURN || ek.vk == fabgl::VK_KP_ENTER) return text;
        if (ek.vk == fabgl::VK_ESCAPE) return "\x1B";
        if (ek.vk == fabgl::VK_BACKSPACE) {
            if (!text.empty()) { text.pop_back(); redraw(true); }
        } else if (ek.ASCII >= 32 && ek.ASCII < 127) {
            if ((int)text.length() < maxlen) {
                char c = ek.ASCII;
                bool shift = Kbd->isVKDown(fabgl::VK_LSHIFT) || Kbd->isVKDown(fabgl::VK_RSHIFT);
                if (c >= 'A' && c <= 'Z') {
                    bool caps = Kbd->isVKDown(fabgl::VK_CAPSLOCK);
                    if (!shift && !caps) c = c - 'A' + 'a';
                } else if (shift) {
                    c = shiftSymUS(c);   // '-'→'_', '1'→'!', … (map_key gives no shift variant)
                }
                text += c;
                redraw(true);
            }
        }
    }
}

uint8_t OSD::osdMaxRows() { return (OSD_H - (OSD_MARGIN * 2)) / OSD_FONT_H; }
uint8_t OSD::osdMaxCols() { return (OSD_W - (OSD_MARGIN * 2)) / OSD_FONT_W; }
unsigned short OSD::osdInsideX() { return scrAlignCenterX(OSD_W) + OSD_MARGIN; }
unsigned short OSD::osdInsideY() { return scrAlignCenterY(OSD_H) + OSD_MARGIN; }

static const uint8_t click48[12] = { 0,8,32,32,32,32,32,32,32,32,8,0 };

static const uint8_t click128[116] = {   0,8,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
                                        32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
                                        32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
                                        32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
                                        32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
                                        32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
                                        32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
                                        32,32,8,0
                                    };

IRAM_ATTR void OSD::click() {
    if (Config::tape_player /*|| Config::real_player*/)
        return; // Disable interface click on tape player mode
    pwm_audio_set_volume(ESP_VOLUME_MAX);
    if (Z80Ops::is48)
        pwm_audio_write((uint8_t*) click48, (uint8_t*) click48, 12, 0, 0);
    else
        pwm_audio_write((uint8_t*) click128, (uint8_t*) click128, 116, 0, 0);
    pwm_audio_set_volume(ESPectrum::aud_volume);
    if (CPU::paused) {
        // The new UI has its own badge; the classic centered box buried the very
        // screen it annotates. DS80 keeps the classic one (our palette would
        // recolour the guest's screen).
        if (nm::available() && !profi_ds80_active) { nm::uiPausedBadge(); return; }
        osdCenteredMsg(OSD_PAUSE, LEVEL_INFO, 0);
    }
}

// click() minus the CPU::paused PAUSE-box repaint. osdCenteredMsg(..., 0) does
// NOT save/restore a rect, so the PAUSE box would punch a permanent hole in a
// fullscreen UI on every keypress. click48/click128 are file-static here, which
// is why this lives next to click() instead of in src/ui/.
IRAM_ATTR void OSD::clickNoPause() {
    if (Config::tape_player)
        return; // Disable interface click on tape player mode
    pwm_audio_set_volume(ESP_VOLUME_MAX);
    if (Z80Ops::is48)
        pwm_audio_write((uint8_t*) click48, (uint8_t*) click48, 12, 0, 0);
    else
        pwm_audio_write((uint8_t*) click128, (uint8_t*) click128, 116, 0, 0);
    pwm_audio_set_volume(ESPectrum::aud_volume);
}
void close_all(void);
void OSD::esp_hard_reset() {
    Debug::log("esp_hard_reset called from %p", __builtin_return_address(0));
    if (Config::audio_driver == 3) send_to_595(LOW(AY_Enable));
    close_all();
    Debug::log("ehr: close_all done, arming watchdog");
#if defined(DBG_UART_ENABLED) && defined(PICO_DEFAULT_UART)
    uart_tx_wait_blocking(uart_default);   // drain FIFO — 32B @115200 ≈ 3ms > watchdog delay
#endif
    watchdog_enable(1, true);
    while (true);
}

static bool confirmReboot(const char* dlg) {
    // Hotkeys arrive with no gfx session, hence the begin/end pair (nested callers
    // wrap us in their own suspend/resume, which re-installs after us).
    nm::gfxBegin();
    const bool yes = nm::uiConfirm(dlg);
    nm::gfxEnd();
    return yes;
}

// SRAM budget gate. See OSDMain.h. Asks Subsystems::budgetCheck whether `f` fits;
// if not, either refuses (DENY) or pops up the freeable features so the user can
// turn some off → Config + reboot. Returns true only when the caller may proceed
// to enable `f` itself (it fits live); on a freeing reboot this never returns.
bool OSD::featureBudgetGate(int featureId) {
    using namespace Subsystems;
    FeatureId f = (FeatureId)featureId;

    FeatureId cand[FEAT_COUNT];
    int nCand = 0;
    size_t deficit = 0;
    BudgetResult r = budgetCheck(f, cand, &nCand, &deficit);

    if (r == BUDGET_ALLOW) return true;

    // Enough total SRAM, just not in one block: nothing to give up, only a reboot to
    // take (setup() allocates from an unfragmented heap). Persist the enable and go —
    // declining leaves the feature off, exactly like Esc out of the free-list.
    if (r == BUDGET_NEEDS_REBOOT) {
        const string dlg = (string)featureName(f) + ": " + OSD_DLG_APPLYREBOOT;
        if (!confirmReboot(dlg.c_str())) return false;
        featureSetEnabled(f, true);
        Config::save();
        esp_hard_reset();   // never returns
    }

    if (r == BUDGET_DENY || nCand == 0) {
        osdCenteredMsg((string)featureName(f) + ":\n" + MSG_BUDGET_DENY,
                       LEVEL_WARN, 4000);
        return false;
    }

    // BUDGET_NEEDS_FREE: multi-select of the freeable features — a pick list where
    // Enter toggles a candidate and the last row applies; the running free/need tally
    // lives in the title. The gfxBegin/End pair makes this safe standalone (hotkey)
    // and nested (menu/commit callers wrap the gate in their own suspend/resume,
    // which re-installs after us).
    {
        nm::gfxBegin();
        bool sel[FEAT_COUNT] = { false };
        int cur = 0;
        while (1) {
            size_t freed = 0;
            for (int i = 0; i < nCand; i++) if (sel[i]) freed += featureCost(cand[i]);
            string rows[FEAT_COUNT + 1];
            const char* items[FEAT_COUNT + 1];
            char buf[64];
            for (int i = 0; i < nCand; i++) {
                snprintf(buf, sizeof(buf), "[%c] %s (%uK)",
                         sel[i] ? '*' : ' ', featureName(cand[i]),
                         (unsigned)((featureCost(cand[i]) + 1023) / 1024));
                rows[i] = buf;
                items[i] = rows[i].c_str();
            }
            rows[nCand]  = MSG_BUDGET_APPLY;
            items[nCand] = rows[nCand].c_str();
            char title[72];
            // "need X, picked Y" — the old "free 0K / need 9K" read as "you have
            // 0K free", which is not what either number means: `freed` is what the
            // ticked boxes would reclaim, `deficit` the shortfall against this
            // feature's biggest single allocation (not against total free heap).
            snprintf(title, sizeof(title), "%s: need %uK, picked %uK",
                     featureName(f), (unsigned)((deficit + 1023) / 1024),
                     (unsigned)(freed / 1024));
            const int pick = nm::uiPickList(title, items, nCand + 1, cur);
            if (pick < 0) break;                        // Esc: not enabled
            if (pick < nCand) {                         // toggle a candidate
                sel[pick] = !sel[pick];
                cur = pick;
                continue;
            }
            if (freed < deficit) {                      // Apply with too little freed
                flushKbd();
                nm::uiToast(MSG_BUDGET_INSUFFICIENT, true, 0);
                cur = pick;
                continue;
            }
            for (int i = 0; i < nCand; i++) if (sel[i]) featureSetEnabled(cand[i], false);
            featureSetEnabled(f, true);
            Config::save();
            esp_hard_reset();                           // never returns
        }
        nm::gfxEnd();
        return false;
    }
}


#if ZIFI_NET_CLIENT
// ─── Network file-transfer client (FTP / SFTP) ──────────────────────────────

#define NET_KNOWN_HOSTS CONFIG_DIR "/known_hosts"

// Look up a stored SHA-256 fingerprint for `host` (returns "" if unknown).
static string knownHostsLookup(const string& host) {
    FIL* f = fopen2(NET_KNOWN_HOSTS, FA_READ);
    if (!f) return "";
    string line, result;
    UINT br; char c;
    while (!f_eof(f)) {
        if (f_read(f, &c, 1, &br) != FR_OK || br == 0) break;
        if (c == '\n') {
            size_t sp = line.find(' ');
            if (sp != string::npos && line.substr(0, sp) == host) { result = line.substr(sp + 1); break; }
            line.clear();
        } else if (c != '\r') line += c;
    }
    fclose2(f);
    return result;
}

static void knownHostsSave(const string& host, const string& fp) {
    FileUtils::mkdirParents(CONFIG_DIR);
    FIL* f = fopen2(NET_KNOWN_HOSTS, FA_OPEN_APPEND | FA_WRITE);
    if (!f) return;
    string line = host + " " + fp + "\n";
    UINT bw; f_write(f, line.data(), line.size(), &bw);
    fclose2(f);
}

// Host-key callback: trust-on-first-use against /known_hosts, refuse on mismatch.
static string g_net_host_for_cb;
static Ssh::TrustResult netHostKeyCb(const char* host, const char* keytype,
                                     const char* fp, bool sig_verified) {
    (void)host; (void)sig_verified;
    string stored = knownHostsLookup(g_net_host_for_cb);
    if (!stored.empty()) {
        if (stored == fp) return Ssh::TRUST;
        OSD::msgDialog(MSG_NET_HOSTKEY_BAD, fp); // mismatch → refuse
        return Ssh::REJECT;
    }
    // First contact — show fingerprint and ask to trust.
    string body = string(keytype) + "\nSHA256:" + fp + "\n" + MSG_NET_TRUST_Q;
    if (OSD::msgDialog(g_net_host_for_cb, body) == DLG_YES) {
        knownHostsSave(g_net_host_for_cb, fp);
        return Ssh::TRUST;
    }
    return Ssh::REJECT;
}

// The SSH/SFTP crypto path (mbedTLS ECDH/ECP/bignum) needs far more than the
// 4 KB core-0 stack (PICO_STACK_SIZE=0x1000), especially nested under do_OSD —
// it overflows and faults (observed: SIGBUS, stackOvf=1). So we run the whole
// network session on a large heap-allocated stack via a switch trampoline.
// ARMv8-M (RP2350/Cortex-M33): also clear MSPLIM during the window so the
// hardware stack-limit check doesn't fault on the alternate stack.
struct NetSessCtx { string host, user, pass, restorePath; uint16_t port; uint8_t proto; };

// Record the global "last F5 location" (Config::last_loc) — persisted so F5 reopens
// where you left off. Writes wifi.cfg only when the value actually changes.
static void lastLocSet(const string& v) {
    if (Config::last_loc != v) { Config::last_loc = v; Config::saveWifiConfig(); }
}

static void netSessionRun(void* p) {
    NetSessCtx* c = (NetSessCtx*)p;
    // progressDialog saves/restores its background (osdCenteredMsg with 0 does not),
    // so the notice is cleanly removed before the browser menu draws. The host-key
    // trust msgDialog (during SSH connect) saves/restores its own rect over this.
    OSD::progressDialog(MSG_NET_CONNECTING, c->host, 0, 0);
    RemoteFs* fs = nullptr;
    if (c->proto == 0) {
        Ftp* ftp = new Ftp();
        if (ftp->connect(c->host.c_str(), c->port, c->user.c_str(), c->pass.c_str())) fs = ftp;
        else delete ftp;
    } else {
        g_net_host_for_cb = c->host;
        Ssh::setHostKeyCb(netHostKeyCb);
        Sftp* sftp = new Sftp();
        if (sftp->connect(c->host.c_str(), c->port, c->user.c_str(), c->pass.c_str())) fs = sftp;
        else delete sftp;
    }
    OSD::progressDialog("", "", 0, 2); // close the "Connecting..." notice
    if (!fs) { OSD::osdCenteredMsg(MSG_NET_CONN_ERR, LEVEL_WARN, 2500); return; }
    if (!c->restorePath.empty() && c->restorePath != fs->cwdPath())
        fs->cwd(c->restorePath);               // reopen at the last folder for this remote
    OSD::remoteFileDialog(fs); // SD-indexed browser (bounded RAM); runs on this alt-stack
    fs->disconnect();
    delete fs;
    // Remember this remote + the folder we left off in, as the global last F5 location.
    char b[32]; snprintf(b, sizeof(b), "%u\t%u", (unsigned)c->port, (unsigned)c->proto);
    lastLocSet("R\t" + c->host + "\t" + b + "\t" + c->user + "\t" + OSD::net_last_path);
}

// Call fn(arg) with MSP switched to new_top (8-byte aligned highest address of a
// scratch buffer). Saves/restores SP, MSPLIM and LR. RP2350/ARM only (this whole
// file region is gated to RP2350 via ZIFI_NET_CLIENT).
extern "C" __attribute__((naked, noinline))
void net_call_on_stack(void* new_top, void (*fn)(void*), void* arg) {
    __asm volatile(
        "mrs  r12, msplim       \n" // save old MSPLIM
        "movs r3, #0            \n"
        "msr  msplim, r3        \n" // disable stack-limit check on the alt stack
        "mov  r3, sp            \n" // r3 = old SP
        "mov  sp, r0            \n" // SP = new_top
        "push {r2, r3, r12, lr} \n" // 16 bytes → keeps 8-byte alignment
        "mov  r0, r2            \n" // r0 = arg
        "blx  r1                \n" // fn(arg)
        "pop  {r2, r3, r12, lr} \n"
        "mov  sp, r3            \n" // restore SP
        "msr  msplim, r12       \n" // restore MSPLIM
        "bx   lr                \n"
    );
}

// RAII: for the duration of a (paused) network session, lend the dormant
// Gigascreen prev framebuffer to the Buffer pool so the alt-stack + TLS/socket
// working set draws from those ~52 KB instead of the scarce heap. No-op unless
// there's something to lend (Gigascreen on AND a butter-less board); on butter
// boards palloc routes the TLS set to XIP PSRAM instead, so no lease is needed.
struct NetArenaLease {
    bool held = false;
    bool released = false;
    NetArenaLease() {
        void* base; size_t size;
        if (VIDEO::gigascreenLendRegion(base, size)) {
            if (Buffer::lendArena(base, size)) held = true;
            else VIDEO::gigascreenReclaimRegion();   // lend rejected → undo the detach
        } else {
            // Nothing lendable can still mean there IS a prev-FB — just a chunked one,
            // which is not one region. Then it is released outright for the session
            // (VIDEO::gigascreenReleaseForNet) so the heap, not the arena, gets those
            // ~38 KB. No-op in every other case.
            released = VIDEO::gigascreenReleaseForNet();
        }
    }
    ~NetArenaLease() {
        if (held) { Buffer::reclaimArena(); VIDEO::gigascreenReclaimRegion(); }
        if (released) VIDEO::gigascreenRestoreAfterNet();
    }
};

// Alloc/free the network alt-stack via the Buffer pool so it can come from the
// lent arena (USE_NET_ARENA) when one is active, else heap. Returns nullptr on OOM.
static inline uint8_t* netAltStackAlloc(size_t& stksz) {
    uint8_t* p = (uint8_t*)Buffer::palloc(stksz, Buffer::NEED_POINTER | Buffer::USE_NET_ARENA);
    if (!p) { stksz = 8 * 1024; p = (uint8_t*)Buffer::palloc(stksz, Buffer::NEED_POINTER | Buffer::USE_NET_ARENA); }
    return p;
}

// Connect to a saved remote (prompting the password if it wasn't stored) and run
// the connect + browse session on a large heap stack (see net_call_on_stack). The
// crypto/listing path can't live on the 4 KB core stack.
static void netConnectRemote(const Config::Remote& r, const string& restorePath = "") {
    string pass = r.pass;
    if (!r.savepass) {
        pass.clear();
        if (!nm::uiPrompt(MSG_NET_PASS_LABEL, pass, 64, true)) return;   // masked
    }
    // Remember as the "last used" defaults (prefill the Add-Remote form next time).
    Config::net_host = r.host; Config::net_user = r.user;
    Config::net_port = r.port; Config::net_proto = r.proto;
    Config::saveWifiConfig();

    NetSessCtx ctx; ctx.host = r.host; ctx.user = r.user; ctx.pass = pass;
    ctx.port = r.port; ctx.proto = r.proto; ctx.restorePath = restorePath;

    // Modest alt stack: mbedTLS curve25519/P256/RSA-verify peaks at only a few KB,
    // and the heap must keep enough for the handshake + SSH objects + the listing.
    NetArenaLease lease;   // borrow the dormant Gigascreen prevFB if available
    size_t stksz = 12 * 1024;
    uint8_t* stk = netAltStackAlloc(stksz);
    if (!stk) { OSD::osdCenteredMsg(MSG_NET_CONN_ERR, LEVEL_WARN, 2500); return; }
    void* top = (void*)(((uintptr_t)stk + stksz) & ~(uintptr_t)7); // 8-byte aligned top
    net_call_on_stack(top, netSessionRun, &ctx);
    Buffer::pfree(stk);
}

// Shared scratch for the remotes list (avoids a big array on the 4 KB core stack;
// one network UI runs at a time). Lazy-heaped — costs 0 SRAM until a network UI
// actually opens, so it never starves the razor-thin Profi heap at VIDEO::Init.
// (Profi forces ~80 KB of SRAM pages before the framebuffer alloc; every byte of
// permanent BSS here pushes it into OOM.) ~2 KB once allocated; never freed (the
// one allocation persists for the session — it's small and reused).
static Config::Remote* remotesBuf() {
    static Config::Remote* p = nullptr;
    if (!p) p = new (std::nothrow) Config::Remote[Config::MAX_REMOTES];
    return p;
}
#define g_remotes remotesBuf()

// Add Remote: prompt protocol / host / user / port / password (+ "save password")
// and append the connection to remotes.tsv. Does not connect.
static void addRemoteForm() {
    string ssid, ip;
    if (!ZiFiAT::getStatus(ssid, ip)) { OSD::osdCenteredMsg(MSG_NET_FT_NOWIFI, LEVEL_WARN, 2200); return; }

    uint8_t pr;
    uint16_t port;
    string host, user, pass, path, alias;
    bool savepass;

    {
        // Proto picker + boxed prompts. Esc anywhere cancels; the optional fields
        // (user/pass/path/alias) accept an empty Enter.
        static const char* const protos[] = { "FTP", "SFTP" };
        const int p = nm::uiPickList(MSG_NET_PROTO_TITLE, protos, 2, Config::net_proto);
        if (p < 0) return;
        pr = (uint8_t)p;

        host = Config::net_host;
        if (!nm::uiPrompt(MSG_NET_HOST_LABEL, host)) return;
        const uint16_t defport = pr ? 22 : 21;
        char pbuf[8]; snprintf(pbuf, sizeof(pbuf), "%u", Config::net_port ? Config::net_port : defport);
        string ports = pbuf;
        if (!nm::uiPrompt(MSG_NET_PORT_LABEL, ports, 8, false, true)) return;
        port = (uint16_t)atoi(ports.c_str()); if (!port) port = defport;
        user = Config::net_user;
        if (!nm::uiPrompt(MSG_NET_USER_LABEL, user, 64, false, true)) return;
        pass.clear();
        if (!nm::uiPrompt(MSG_NET_PASS_LABEL, pass, 64, true, true)) return;
        savepass = !pass.empty() && nm::uiConfirm(MSG_REMOTE_SAVEPASS_Q);
        path.clear();
        if (!nm::uiPrompt(MSG_REMOTE_PATH_LABEL, path, 64, false, true)) return;
        alias.clear();
        if (!nm::uiPrompt(MSG_REMOTE_ALIAS_LABEL, alias, 32, false, true)) return;
    }

    if (!g_remotes) return;   // alloc failed (OOM) — bail out of the form
    int n = Config::loadRemotes(g_remotes, Config::MAX_REMOTES);
    if (n >= Config::MAX_REMOTES) { OSD::osdCenteredMsg(MSG_REMOTE_FULL, LEVEL_WARN, 2000); return; }
    g_remotes[n].host = host; g_remotes[n].user = user; g_remotes[n].port = port;
    g_remotes[n].proto = pr; g_remotes[n].savepass = savepass;
    g_remotes[n].pass = savepass ? pass : ""; g_remotes[n].alias = alias; g_remotes[n].path = path;
    Config::saveRemotes(g_remotes, n + 1);

    Config::net_host = host; Config::net_user = user; Config::net_port = port; Config::net_proto = pr;
    Config::saveWifiConfig();
}

// Remote (FTP/SFTP): list saved connections; Enter connects (prompting the
// password if not stored), F8 forgets a connection, and a trailing "[Add Remote]"
// row opens the Add-Remote form.
// Remote (FTP/SFTP) saved-connection list — rendered in the file-browser window
// (FD_SIDE_HOSTS sidebar) so it matches SD. Enter connects (alt-stack browse),
// F8 forgets, the trailing [Add Remote] row opens the form.
static void remoteHostsBrowse() {
    string ssid, ip;
    if (!ZiFiAT::getStatus(ssid, ip)) { OSD::osdCenteredMsg(MSG_NET_FT_NOWIFI, LEVEL_WARN, 2200); return; }
    if (!g_remotes) return;   // alloc failed (OOM) — can't list remotes
    int hf = 2, hb = 2;                               // keep the cursor on the chosen host
    while (1) {
        int n = Config::loadRemotes(g_remotes, Config::MAX_REMOTES);
        std::vector<string> rows;
        rows.push_back(string(2, (char)DIR_MARKER) + "..");  // row 0: ".." → locations root
        for (int i = 0; i < n; i++) {
            if (!g_remotes[i].alias.empty()) {        // show the alias when set
                rows.push_back(g_remotes[i].alias);
            } else {
                char b[96];
                snprintf(b, sizeof(b), "%s@%s:%u (%s)", g_remotes[i].user.c_str(),
                         g_remotes[i].host.c_str(), (unsigned)g_remotes[i].port,
                         g_remotes[i].proto ? "sftp" : "ftp");
                rows.push_back(b);                    // rows 1..n: a plain row (no <DIR>)
            }
        }
        rows.push_back(MSG_REMOTE_ADD_ROW);   // row n+1
        int key;
        int sel = OSD::fdChromeList(rows, MENU_ALL_TITLE,
                                    MENU_REMOTE_TITLE,
                                    OSD::FD_SIDE_HOSTS, false, &key, &hf, &hb);
        if (sel <= 0) {                               // "..", Backspace, or Esc → leave list
            if (sel < 0 && key == OSD::FDK_ESC) OSD::net_close_all = true; // Esc → close OSD
            return;                                   // ".."/Backspace → climb to locations
        }
        if (sel == n + 1) { addRemoteForm(); continue; }    // [Add Remote] row
        int hi = sel - 1;                             // host index (row 0 is "..")
        if (hi < 0 || hi >= n) continue;
        if (key == OSD::FDK_F8) {                      // F8 → forget this connection
            if (OSD::msgDialog(g_remotes[hi].host, MSG_REMOTE_FORGET_Q) == DLG_YES) {
                for (int j = hi; j < n - 1; j++) g_remotes[j] = g_remotes[j + 1];
                Config::saveRemotes(g_remotes, n - 1);
            }
            continue;
        }
        netConnectRemote(g_remotes[hi], g_remotes[hi].path); // connect → cd into saved Path
        if (OSD::net_launch_close || OSD::net_close_all) return;  // launched or Esc → unwind
    }
}

static void netDownloadArchive();   // Web Archives entry (defined below)

// Set by the F5 handler on a fresh F5 press → f5Locations restores Config::last_loc
// once (so F5 reopens where you left off). Cleared after use; the SD-back-out re-entry
// (goto) does NOT set it, so backing out always lands on the chooser.
static bool g_f5_restore = false;

// F5 last-location restore for the catalog (set by f5Locations before the alt-stack
// archSessionRun entry): open this site at this path directly, skipping the list once.
static string g_web_rsite, g_web_rpath;
static bool   g_web_restore = false;

// Split a tab-separated string into fields.
static void splitTabs(const string& s, std::vector<string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\t') { out.push_back(s.substr(start, i - start)); start = i + 1; }
}

// F5 location level — rendered IN the file-browser window (Open File + sidebar) as
// rows: Local (SD) / [USB Drive] / Remote (FTP/SFTP) / Web Archives / Add Remote.
// The USB row appears only while a mass-storage stick is enumerated. Returns true
// when the user chose a LOCAL source — SD or USB (caller then opens the browser at
// FileUtils::ALL_Path, which this function points at the chosen volume); false for
// Esc or any network action (caller closes the OSD).

// The chooser is reachable when there's any location besides the SD card: a WiFi
// link (or saved SSID) or a USB stick. With the stick AS the root volume (no SD
// at boot) "Local" already browses it, so it adds no extra location.
static inline bool f5HasChooser() {
    return ZiFiAT::connected || !Config::wifi_ssid.empty() ||
           (UsbMsc::ready() && !FileUtils::usbRoot);
}

// Last browse dir per local source, so switching SD⇄USB in the chooser returns to
// where you were on each side. ALL_Path holds whichever is active (and is the one
// persisted to NVS; a stale "USB:/..." with no stick self-heals in the browser).
static string s_f5_sd_dir  = "/";
static string s_f5_usb_dir = "USB:/";

static bool f5Locations() {
    // Net fetch progress goes to the new browser's status line for the whole F5
    // session (every return path clears it via the destructor).
    struct ProgScope {
        ProgScope()  { OSD::progressOverride = nm::uiProgressStatus; }
        ~ProgScope() { OSD::progressOverride = nullptr; }
    } progScope;
    // One-time restore of the last browse location (across all sources), so F5 reopens
    // where you left off — Local stays at ALL_Path, Web/Remote reopen their last folder.
    if (g_f5_restore) {
        g_f5_restore = false;
        const string& L = Config::last_loc;
        if (!L.empty() && (L[0] == 'W' || L[0] == 'R')) {
            // Only auto-reopen a network location when the WiFi link is actually up.
            // Right after a (power-cycle) reboot the ESP may not be associated yet;
            // diving into a blocking HTTPS revalidate/fetch then would freeze the UI
            // ("F5 hangs hard"). Not connected → skip the dive and fall through to the
            // chooser below, so F5 stays responsive.
            std::vector<string> f; splitTabs(L, f);
            if (!ZiFiAT::connected) {
                /* fall through to the chooser */
            } else if (L[0] == 'W' && f.size() >= 3) {       // W \t site \t path
                g_web_rsite = f[1]; g_web_rpath = f[2]; g_web_restore = true;
                netDownloadArchive();
                g_web_restore = false;    // clear even if netDownloadArchive bailed early
                if (OSD::net_launch_close || OSD::net_close_all) return false;
            } else if (L[0] == 'R' && f.size() >= 6 && g_remotes) {  // R \t host \t port \t proto \t user \t path
                int n = Config::loadRemotes(g_remotes, Config::MAX_REMOTES);
                uint16_t port = (uint16_t)atoi(f[2].c_str());
                uint8_t  proto = (uint8_t)atoi(f[3].c_str());
                for (int i = 0; i < n; i++)
                    if (g_remotes[i].host == f[1] && g_remotes[i].port == port &&
                        g_remotes[i].proto == proto && g_remotes[i].user == f[4]) {
                        netConnectRemote(g_remotes[i], f[5]); break;
                    }
                if (OSD::net_launch_close || OSD::net_close_all) return false;
            }
            // after the restored browse → fall through to the chooser below
        } else {
            return true;   // "L" or empty → Local (SD) at ALL_Path
        }
    }
    int lf = 2;                                       // keep the cursor on the chosen location
    while (1) {
        if (OSD::net_launch_close) return false;      // a quick-start launched → close OSD
        // Hide the USB row when the stick is the root volume — "Local" IS the stick.
        const bool usb = UsbMsc::ready() && !FileUtils::usbRoot;
        // No "Add Remote" row here: the Remote host list carries its own trailing
        // [Add Remote] row (remoteHostsBrowse), so the root stays three locations.
        std::vector<string> rows = {
            // USB-as-root: "Local" is the stick, not the (absent) SD card.
            string(1, (char)DIR_MARKER) + (FileUtils::usbRoot ? MSG_F5_USB
                                                              : MSG_F5_LOCAL),
            string(1, (char)DIR_MARKER) + MSG_F5_REMOTE,
            string(1, (char)DIR_MARKER) + MSG_F5_WEB,
        };
        if (usb)
            rows.insert(rows.begin() + 1, string(1, (char)DIR_MARKER) + MSG_F5_USB);
        int key;
        int loc;
        {
            // The chooser is a LEVEL of the fullscreen browser (same chrome), not a
            // modal; the remote/web flows a chosen row dives into own their own
            // browser chrome (fdChromeNav).
            const char* items[8];
            const char* hints[8];
            int n = 0;
            for (auto& r : rows) {
                const int i = n++;
                items[i] = r.c_str() + ((uint8_t)r[0] == DIR_MARKER ? 1 : 0);
                hints[i] = "";
            }
            // Right-pane notes, matched to the row set built above.
            int hi = 0;
            hints[hi++] = FileUtils::usbRoot ? "USB stick (root volume)" : "SD card";
            if (usb) hints[hi++] = "USB mass-storage stick";
            hints[hi++] = "Saved FTP / SFTP hosts";
            hints[hi++] = "Online archives (vtrd.in, ...)";
            loc = nm::browseLocations(items, hints, n, lf - 2);
            lf = loc >= 0 ? loc + 2 : 2;
            key = OSD::FDK_ENTER;
        }
        if (loc < 0) return false;                    // Esc → close OSD
        if (usb && loc == 1) {                        // USB Drive → browse the stick
            if (FileUtils::ALL_Path.compare(0, 4, "USB:") != 0) {
                s_f5_sd_dir = FileUtils::ALL_Path;
                FileUtils::ALL_Path = s_f5_usb_dir;
            }
            return true;
        }
        if (usb && loc > 1) loc--;                    // fold the USB row out of the indices
        if (loc == 0) {                               // Local (SD) → caller opens SD browser
            if (FileUtils::ALL_Path.compare(0, 4, "USB:") == 0) {
                s_f5_usb_dir = FileUtils::ALL_Path;
                FileUtils::ALL_Path = s_f5_sd_dir;
            }
            return true;
        }
        else if (loc == 1) remoteHostsBrowse();
        else if (loc == 2) netDownloadArchive();      // Web Archives (built-in catalog)
        if (OSD::net_launch_close || OSD::net_close_all) return false;
    }
}

// ── FTP server: share the SD card to the LAN ─────────────────────────────────
// Scrolling log ring for the on-screen terminal. Fixed char[][] (no heap churn),
// holding the most recent lines; the renderer always shows the tail (auto-scroll).
#define FTPD_LOG_LINES 40
#define FTPD_LOG_COLS  72
// Lazy-heaped (2880 B) — costs 0 SRAM until the FTP server / WiFi-connect window
// actually logs a line, so it never starves the razor-thin Profi heap at
// VIDEO::Init (Profi forces ~80 KB of SRAM pages before the framebuffer alloc).
// Readers are gated by `li < ftpd_log_count`, and ftpd_log_count only advances
// after a successful alloc here, so they never touch a null buffer.
static char (*ftpd_log)[FTPD_LOG_COLS] = nullptr;
extern "C" size_t getLargestAllocatable(void);   // defined at the bottom of this file
static int  ftpd_log_count = 0;  // total lines pushed (monotonic)
static bool ftpd_log_dirty = true;
static void ftpdLogLine(const char* s) {
    if (!ftpd_log) {
        // Gate, don't null-check: pico_malloc wraps calloc too and PANICs on OOM
        // instead of returning NULL, so "drop the line rather than crash" only
        // works if we never make an ask that can fail (see Buffer::palloc).
        if (getLargestAllocatable() < FTPD_LOG_LINES * FTPD_LOG_COLS + 8192) return;
        ftpd_log = (char(*)[FTPD_LOG_COLS])calloc(FTPD_LOG_LINES, FTPD_LOG_COLS);
        if (!ftpd_log) return;   // OOM — drop the line rather than crash
    }
    int slot = ftpd_log_count % FTPD_LOG_LINES;
    strncpy(ftpd_log[slot], s ? s : "", FTPD_LOG_COLS - 1);
    ftpd_log[slot][FTPD_LOG_COLS - 1] = '\0';
    ftpd_log_count++;
    ftpd_log_dirty = true;
}


struct FtpdCtx { const char* ip; };

// New-chrome FTP session: the log ring rendered as a live scrollable text page
// (nm::uiTextPageLive). The page text is rebuilt only when the ring changed —
// nullptr from the refresh keeps the current page, so idle polling never repaints.
#define FTPD_PAGE_SZ (FTPD_LOG_LINES * FTPD_LOG_COLS + 8)
static char* s_ftpd_page = nullptr;
static const char* ftpdPageRefresh() {
    Ftpd::poll();                         // the refresh IS the session's pump
    if (!ftpd_log_dirty) return nullptr;
    ftpd_log_dirty = false;
    if (!ftpd_log) return "";
    char* w = s_ftpd_page;
    char* const e = s_ftpd_page + FTPD_PAGE_SZ;
    const int first = ftpd_log_count > FTPD_LOG_LINES ? ftpd_log_count - FTPD_LOG_LINES : 0;
    for (int li = first; li < ftpd_log_count && w < e - 2; li++) {
        const char* s = ftpd_log[li % FTPD_LOG_LINES];
        int n = (int)strnlen(s, FTPD_LOG_COLS - 1);
        const int room = (int)(e - w) - 2;
        if (n > room) n = room;
        memcpy(w, s, n); w += n;
        *w++ = '\n';
    }
    *w = '\0';
    return s_ftpd_page;
}

// Run the server session (details panel + live log terminal) in a blocking loop
// until ESC. The Z80 stays frozen (we never call CPU::loop). Invoked on a heap
// alt-stack via net_call_on_stack — FatFS + the OSD draw chain are too deep for
// the 4 KB core stack when nested under do_OSD (same reason as the net client).
static void ftpdSessionRun(void* arg) {
    const char* ip = ((FtpdCtx*)arg)->ip;

    ftpd_log_count = 0; ftpd_log_dirty = true;
    if (!Ftpd::begin(21, ftpdLogLine)) {
        OSD::osdCenteredMsg("FTP server start failed", LEVEL_WARN, 2500);
        Ftpd::stop();
        return;
    }
    ftpdLogLine("FTP server started");
    char det[FTPD_LOG_COLS];
    snprintf(det, sizeof(det), "ftp://%s:21  user: anonymous", ip);
    ftpdLogLine(det);
    ftpdLogLine("Use ACTIVE mode. ESC to stop.");

    // The session runs as a live log page (Esc closes → the server stops). The heap
    // gate is there because pico_malloc's calloc PANICs on OOM instead of returning
    // NULL; too tight to hold the page → no server rather than a panic.
    if (getLargestAllocatable() < FTPD_PAGE_SZ + 8192 ||
        (s_ftpd_page = (char*)calloc(1, FTPD_PAGE_SZ)) == nullptr) {
        Ftpd::stop();
        nm::uiToast("Not enough memory for the FTP log", true, 2500);
        return;
    }
    char title[64];
    snprintf(title, sizeof(title), "FTP server  ftp://%s:21  anonymous", ip);
    nm::uiTextPageLive(title, ftpdPageRefresh, 2);
    free(s_ftpd_page); s_ftpd_page = nullptr;
    Ftpd::stop();
    nm::uiToast("FTP server stopped", false, 1200);
}

// Entry: require WiFi, then run the session on a heap alt-stack.
// Non-static: the new UI Network branch (src/ui/UiActions.cpp) wraps it too.
void ftpServerRun() {
    string ssid, ip;
    if (!ZiFiAT::getStatus(ssid, ip)) {
        OSD::osdCenteredMsg(MSG_NET_FT_NOWIFI, LEVEL_WARN, 2200);
        return;
    }
    FtpdCtx ctx; ctx.ip = ip.c_str(); // ip outlives the (synchronous) call below
    NetArenaLease lease;   // borrow the dormant Gigascreen prevFB if available
    size_t stksz = 8 * 1024;
    uint8_t* stk = netAltStackAlloc(stksz);
    if (!stk) { OSD::osdCenteredMsg(MSG_NET_CONN_ERR, LEVEL_WARN, 2500); return; }
    void* top = (void*)(((uintptr_t)stk + stksz) & ~(uintptr_t)7); // 8-byte aligned top
    net_call_on_stack(top, ftpdSessionRun, &ctx);
    Buffer::pfree(stk);
}

// ── Download archive (catalog server over plain HTTP) ───────────────────────
// Browses the pico-speccy catalog server (HttpCatalogFs) with the same generic
// remoteFileDialog used for FTP/SFTP. Runs on the large heap stack like the
// FTP/SFTP session — no crypto here, but the SD-indexed browser is the heavy
// part, so we keep off the 4 KB core stack for safety + consistency.
// Open one catalog site at `path`, browse, and record it as the global last location.
static void archOpenSite(const char* siteId, const string& path) {
    HttpCatalogFs* fs = new HttpCatalogFs(siteId);
    if (!path.empty() && path != fs->cwdPath()) fs->cwd(path);
    OSD::remoteFileDialog(fs);                  // catalog in the same chrome
    fs->disconnect();
    delete fs;
    lastLocSet(string("W\t") + siteId + "\t" + OSD::net_last_path);
}

static void archSessionRun(void* p) {
    (void)p;
    // Restore: open the last catalog site+path directly (then fall to the site list).
    if (g_web_restore) {
        g_web_restore = false;
        archOpenSite(g_web_rsite.c_str(), g_web_rpath);
        if (OSD::net_launch_close || OSD::net_close_all) return; // launched or Esc → unwind
    }
    OSD::progressDialog(MSG_NET_CONNECTING, "", 0, 0); // no URL (built-in catalog)
    static string site_ids[12];
    static string site_names[12];
    int n = HttpCatalogFs::fetchSites(site_ids, site_names, 12);
    OSD::progressDialog("", "", 0, 2);
    if (n <= 0) { OSD::osdCenteredMsg(MSG_ARCH_SITES_ERR, LEVEL_WARN, 2200); return; }

    // Site list rendered IN the browser window (not a popup) — ".." row returns to the
    // locations level; selecting a source opens its catalog in the same chrome.
    int sf = 2, sb = 2;                                  // keep the cursor on the chosen site
    while (1) {
        std::vector<string> rows;
        rows.push_back(string(2, (char)DIR_MARKER) + "..");                 // → locations
        for (int i = 0; i < n; i++) rows.push_back(string(1, (char)DIR_MARKER) + site_names[i]);
        int key;
        int sel = OSD::fdChromeList(rows, MENU_ALL_TITLE,
                                    MENU_ARCH_SITE_TITLE,
                                    OSD::FD_SIDE_LOCATIONS, false, &key, &sf, &sb);
        if (sel <= 0) {                             // "..", Backspace, or Esc → leave list
            if (sel < 0 && key == OSD::FDK_ESC) OSD::net_close_all = true; // Esc → close OSD
            return;                                 // ".."/Backspace → climb to locations
        }
        int si = sel - 1;                           // site index (row 0 is "..")
        if (si < 0 || si >= n) continue;
        archOpenSite(site_ids[si].c_str(), "");     // open at root
        if (OSD::net_launch_close || OSD::net_close_all) return;  // launched or Esc → unwind
    }
}

// Top-level entry: require an active WiFi link, then browse the built-in online
// catalog. The catalog URL is hardcoded (HttpCatalogFs) — nothing to configure.
static void netDownloadArchive() {
    string ssid, ip;
    if (!ZiFiAT::getStatus(ssid, ip)) {
        OSD::osdCenteredMsg(MSG_NET_FT_NOWIFI, LEVEL_WARN, 2200);
        return;
    }

    NetArenaLease lease;   // borrow the dormant Gigascreen prevFB if available
    size_t stksz = 12 * 1024;
    uint8_t* stk = netAltStackAlloc(stksz);
    if (!stk) { OSD::osdCenteredMsg(MSG_NET_CONN_ERR, LEVEL_WARN, 2500); return; }
    void* top = (void*)(((uintptr_t)stk + stksz) & ~(uintptr_t)7);
    net_call_on_stack(top, archSessionRun, nullptr);
    Buffer::pfree(stk);
}

// ── HTTP test ("curl") ──────────────────────────────────────────────────────
// A diagnostic: GET an arbitrary URL and show the result. Exercises HttpsGet ->
// TlsSock (host-TLS over the ESP plain-TCP pipe) on real hardware — the Step-0
// spike that can't be validated off-target. Output is a summary only (no SD file).
extern size_t getFreeHeap(void);

namespace {
struct HttpTestCtx { std::string url; };
// Preview accumulator passed to the sink as ctx (lives on the alt-stack, no BSS).
struct HttpPreview { char* buf; size_t cap; size_t n; };

// Collects a printable preview of the first bytes; drains the rest (return true)
// so the whole body still streams (and Content-Length is satisfied).
bool httpTestSink(void* ctx, const uint8_t* data, size_t len) {
    HttpPreview* pv = (HttpPreview*)ctx;
    while (pv->n + 1 < pv->cap && len) {
        uint8_t b = *data++; len--;
        bool printable = (b >= 32 && b < 127) || b == '\n' || b == '\t';
        pv->buf[pv->n++] = printable ? (char)b : '.';
    }
    return true;
}

void httpTestRun(void* p) {
    HttpTestCtx* c = (HttpTestCtx*)p;
    // Buffers live on this alt-stack (12 KB) — no permanent SRAM/BSS. Kept modest
    // so they don't crowd the mbedTLS handshake sharing the same stack.
    char preview[512];
    char summary[1024];
    HttpPreview pv = { preview, sizeof(preview), 0 };

    OSD::progressDialog(MSG_HTTP_TESTING, c->url, 0, 0);
    size_t heap0 = getFreeHeap();
    HttpsGet::Result r = HttpsGet::get(c->url.c_str(), httpTestSink, &pv,
                                       CONFIG_DIR "/cacert.pem");
    size_t heap1 = getFreeHeap();
    OSD::progressDialog("", "", 0, 2);
    preview[pv.n] = '\0';
    Debug::log("netHttpTest: status=%d len=%lu recv=%lu ok=%d heap before/after=%u/%u",
               r.status, (unsigned long)r.length, (unsigned long)r.received, r.ok,
               (unsigned)heap0, (unsigned)heap1);
    snprintf(summary, sizeof(summary),
             "%s\n\nstatus: %d\nContent-Length: %lu\nreceived: %lu\nok: %s\n"
             "free heap: %u -> %u\n\n--- body ---\n%s",
             c->url.c_str(), r.status, (unsigned long)r.length,
             (unsigned long)r.received, r.ok ? "yes" : "no",
             (unsigned)heap0, (unsigned)heap1, preview);
    OSD::showTextDialog(MSG_HTTP_TEST_TITLE, summary);
}
} // namespace

// Non-static: the new UI Network branch enters here with a ready URL (it asks with
// ONE scrolling field — uiEditLine pans past the box, so no scheme/host/path split).
void netHttpTestUrl(const string& url) {
    Buffer::selfTest();   // exercise the tiered allocator (logs to serial) before the GET
    string ssid, ip;
    if (!ZiFiAT::getStatus(ssid, ip)) {
        OSD::osdCenteredMsg(MSG_NET_FT_NOWIFI, LEVEL_WARN, 2200);
        return;
    }
    HttpTestCtx ctx;
    ctx.url = url;

    // Run the GET (and its TLS handshake) on a large heap stack, like the FTP/
    // archive sessions — the 4 KB core stack can't hold the mbedTLS handshake.
    NetArenaLease lease;   // borrow the dormant Gigascreen prevFB if available
    size_t stksz = 12 * 1024;
    uint8_t* stk = netAltStackAlloc(stksz);
    if (!stk) { OSD::osdCenteredMsg(MSG_NET_CONN_ERR, LEVEL_WARN, 2500); return; }
    void* top = (void*)(((uintptr_t)stk + stksz) & ~(uintptr_t)7);
    net_call_on_stack(top, httpTestRun, &ctx);
    Buffer::pfree(stk);
}

#endif // ZIFI_NET_CLIENT

// // Cursor to OSD first row,col
void OSD::osdHome() { VIDEO::vga.setCursor(osdInsideX(), osdInsideY()); }

// // Cursor positioning
void OSD::osdAt(uint8_t row, uint8_t col) {
    if (row > osdMaxRows() - 1)
        row = 0;
    if (col > osdMaxCols() - 1)
        col = 0;
    unsigned short y = (row * OSD_FONT_H) + osdInsideY();
    unsigned short x = (col * OSD_FONT_W) + osdInsideX();
    VIDEO::vga.setCursor(x, y);
}

void OSD::drawOSD(bool bottom_info) {
    unsigned short x = scrAlignCenterX(OSD_W);
    unsigned short y = scrAlignCenterY(OSD_H);
    VIDEO::vga.fillRect(x, y, OSD_W, OSD_H, zxColor(1, 0));
    VIDEO::vga.rect(x, y, OSD_W, OSD_H, zxColor(0, 0));
    VIDEO::vga.rect(x + 1, y + 1, OSD_W - 2, OSD_H - 2, zxColor(7, 0));
    VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(5, 1));
    VIDEO::vga.setFont(Font6x8);
    osdHome();
    VIDEO::vga.print(OSD_TITLE);
    osdAt(23, 0);
    if (bottom_info) {
        string bottom_line;
#ifdef VGA_HDMI
    {
        uint8_t vm = SELECT_VGA ? Config::vga_video_mode : Config::hdmi_video_mode;
        const char* vmname;
        switch (vm) {
            case Config::VM_640x480_60: vmname = "640x480@60Hz"; break;
            case Config::VM_640x480_50: vmname = "640x480@50Hz"; break;
            case Config::VM_720x480_60: vmname = "720x480@60Hz"; break;
            case Config::VM_720x576_50: vmname = "720x576@50Hz"; break;
            default:                    vmname = "unknown";      break;
        }
        char buf2[41];
        snprintf(buf2, sizeof(buf2), " Video: %s %s  ",
                 (SELECT_VGA ? "VGA" : "HDMI"), vmname);
        bottom_line = buf2;
    }
#else
#ifdef TV
        bottom_line = " Video mode: TV RGBI PAL   ";
#endif
#ifdef SOFTTV
        bottom_line = " Video mode: TV-composite  ";
#endif
#ifdef TFT
#ifdef ILI9341
        bottom_line = TFT_INVERSION ? " Video mode: ILI9341I      " : " Video mode: ILI9341       ";
#else 
        bottom_line = TFT_INVERSION ? " Video mode: ST7789I       " : " Video mode: ST7789        ";
#endif
#endif
#endif
        VIDEO::vga.print(bottom_line.append(EMU_VERSION).c_str());
    } else VIDEO::vga.print(OSD_BOTTOM);
    osdHome();
}

void OSD::drawStats() {

    unsigned short x,y;

    if (VIDEO::isFullBorder288()) {
        x = 188;
        y = 268;
    } else if (VIDEO::isFullBorder240()) {
        x = 188;
        y = 220;
    } else {
        x = 168;
        y = 220;
    }

    // New-skin stats: same geometry and 6x8 face, the UI palette's colours. The
    // background still encodes the speed state (turbo steps / max speed). DS80
    // keeps the classic colours — the UI block would recolour the guest screen.
    if (nm::available() && !profi_ds80_active) {
        nm::gfxComputeSurface();
        nm::gfxInstallPalette();          // applyPalette may have rewritten our block
        const int base = nm::uiPaletteBase();
        nm::UiColor bg;
        if (ESPectrum::maxSpeed)                bg = nm::C_ICON_C;
        else switch (ESPectrum::multiplicator) {
            case 1:  bg = nm::C_SEL_BG;  break;
            case 2:  bg = nm::C_ICON_R;  break;
            case 3:  bg = nm::C_ICON_Y;  break;
            default: bg = nm::C_FOOT_BG; break;
        }
        VIDEO::vga.setTextColor((uint8_t)(base + nm::C_TEXT), (uint8_t)(base + bg));
        VIDEO::vga.setFont(Font6x8);
        VIDEO::vga.setCursor(x, y);
        VIDEO::vga.print(stats_lin1);
        VIDEO::vga.setCursor(x, y + 8);
        VIDEO::vga.print(stats_lin2);
        return;
    }
    VIDEO::vga.setTextColor(zxColor(7, 0), zxColor( ESPectrum::maxSpeed ? 5 : ESPectrum::multiplicator + 1, 0));
    VIDEO::vga.setFont(Font6x8);
    VIDEO::vga.setCursor(x,y);
    VIDEO::vga.print(stats_lin1);
    VIDEO::vga.setCursor(x,y+8);
    VIDEO::vga.print(stats_lin2);
}

void OSD::clearStats() {

    uint16_t brd16 = (uint16_t)VIDEO::brd;

    if (VIDEO::isFullBorder288()) {
        // full border 360x288: stats at x=188, y=268, 144x16px, uint16_t framebuffer
        for (int line = 268; line < 284; line++) {
            uint16_t *ptr = (uint16_t *)(VIDEO::vga.frameBuffer[line]);
            for (int col = 188; col < 332; col++)
                ptr[col ^ 1] = brd16;
        }
    } else if (VIDEO::isFullBorder240()) {
        // half border 360x240: stats at x=188, y=220, 144x16px, uint16_t framebuffer
        for (int line = 220; line < 236; line++) {
            uint16_t *ptr = (uint16_t *)(VIDEO::vga.frameBuffer[line]);
            for (int col = 188; col < 332; col++)
                ptr[col ^ 1] = brd16;
        }
    } else if ((Z80Ops::isPentagon || Z80Ops::isProfi)) {
        for (int line = 220; line < 236; line++) {
            uint16_t *ptr = (uint16_t *)(VIDEO::vga.frameBuffer[line]);
            for (int col = 84; col < 156; col++)
                ptr[col ^ 1] = brd16;
        }
    } else {
        uint32_t brdColor = VIDEO::brd;
        for (int line = 220; line < 236; line++) {
            uint32_t *ptr = (uint32_t *)(VIDEO::vga.frameBuffer[line]);
            for (int col = 21; col < 39; col++) {
                ptr[col * 2] = brdColor;
                ptr[col * 2 + 1] = brdColor;
            }
        }
    }
}


void OSD::drawVolumeBox() {

    unsigned short x, y;
    if (VIDEO::isFullBorder288()) {
        x = 188;
        y = 272;
    } else if (VIDEO::isFullBorder240()) {
        x = 188;
        y = 224;
    } else {
        x = 168;
        y = 224;
    }
    // New-skin styling of the same border band — the palette trick uiPausedBadge
    // uses: UI colours live at 224..239, so the running game keeps its own 16.
    // DS80 stays classic: installing the UI palette would steal the guest's
    // entries while the game is still drawing.
    if (nm::available() && !nm::Sf.ds80) {
        nm::gfxComputeSurface();
        nm::gfxInstallPalette();
        const int base = nm::uiPaletteBase();
        VIDEO::vga.fillRect(x, y - 4, 24 * 6, 16, nm::uiPaletteSlot(nm::C_PANEL));
        VIDEO::vga.setTextColor((uint8_t)(base + nm::C_TEXT_DIM),
                                (uint8_t)(base + nm::C_PANEL));
        VIDEO::vga.setFont(Font6x8);
        VIDEO::vga.setCursor(x + 4, y + 1);
        VIDEO::vga.print(Config::tape_player ? "TAP" : "VOL");
        // Full track: lit cells up to the volume, dim ticks for the rest.
        for (int i = 0; i < 16; i++) {
            const bool on = i < ESPectrum::aud_volume + 16;
            VIDEO::vga.fillRect(x + 26 + (i * 7), y + 1, 6, 7,
                                nm::uiPaletteSlot(on ? nm::C_ACCENT : nm::C_SEP));
        }
        return;
    }
    VIDEO::vga.fillRect(x, y - 4, 24 * 6, 16, zxColor(1, 0));
    VIDEO::vga.setTextColor(zxColor(7, 0), zxColor(1, 0));
    VIDEO::vga.setFont(Font6x8);
    VIDEO::vga.setCursor(x + 4, y + 1);
    VIDEO::vga.print(Config::tape_player ? "TAP" : "VOL");
    for (int i = 0; i < ESPectrum::aud_volume + 16; i++) {
        VIDEO::vga.fillRect(x + 26 + (i * 7), y + 1, 6, 7, zxColor(7, 0));
    }
}

// Forward-declare the local f_gets wrapper (defined further below)
static void f_gets(char* b, size_t sz, FIL& f);


// Get the base name (no extension) of the currently loaded tape or disk
string getDefaultSnapshotName() {
    // Try tape first
    if (Tape::tapeFileName != "none" && !Tape::tapeFileName.empty()) {
        string name = Tape::tapeFileName;
        // Strip directory
        size_t sl = name.rfind('/');
        if (sl != string::npos) name = name.substr(sl + 1);
        // Strip extension
        size_t dot = name.rfind('.');
        if (dot != string::npos) name = name.substr(0, dot);
        if (!name.empty()) return name;
    }
    // Try disk drive 0
    if (ESPectrum::fdd.disk[0] && !ESPectrum::fdd.disk[0]->fname.empty()) {
        string name = ESPectrum::fdd.disk[0]->fname;
        size_t sl = name.rfind('/');
        if (sl != string::npos) name = name.substr(sl + 1);
        size_t dot = name.rfind('.');
        if (dot != string::npos) name = name.substr(0, dot);
        if (!name.empty()) return name;
    }
    return "";
}

// Read slot name (3rd line) from .esp info file.
// Returns "" if slot file doesn't exist, "\x01" if file exists but has no name.
string getSlotName(uint8_t slotnumber) {
    char persistfinfo[sizeof(DISK_PSNA_FILE) + 7];
    sprintf(persistfinfo, DISK_PSNA_FILE "%u.esp", slotnumber);
    string finfo = string(DISK_PSNA_DIR) + "/" + persistfinfo;
    FIL* f = fopen2(finfo.c_str(), FA_READ);
    if (!f) return "";
    char buf[64];
    // Skip arch line
    f_gets(buf, sizeof(buf), *f);
    // Skip romset line
    f_gets(buf, sizeof(buf), *f);
    // Read name line
    buf[0] = 0;
    f_gets(buf, sizeof(buf), *f);
    fclose2(f);
    if (buf[0] == 0) return "\x01";  // file exists, no name stored
    return string(buf);
}

// Delete both .sna and .esp files for a slot
void persistDelete(uint8_t slotnumber) {
    char persistfname[sizeof(DISK_PSNA_FILE) + 7];
    char persistfinfo[sizeof(DISK_PSNA_FILE) + 7];
    sprintf(persistfname, DISK_PSNA_FILE "%u.sna", slotnumber);
    sprintf(persistfinfo, DISK_PSNA_FILE "%u.esp", slotnumber);
    string fsna  = string(DISK_PSNA_DIR) + "/" + persistfname;
    string finfo = string(DISK_PSNA_DIR) + "/" + persistfinfo;
    f_unlink(fsna.c_str());
    f_unlink(finfo.c_str());
}



// UI-free rename core (shared with the new UI): rewrite the .esp keeping arch/romset.
void persistSetName(uint8_t slotnumber, const string& newName) {
    char persistfinfo[sizeof(DISK_PSNA_FILE) + 7];
    sprintf(persistfinfo, DISK_PSNA_FILE "%u.esp", slotnumber);
    string finfo = string(DISK_PSNA_DIR) + "/" + persistfinfo;
    FIL* f = fopen2(finfo.c_str(), FA_READ);
    if (!f) return;
    char arch[64], romset[64];
    f_gets(arch, sizeof(arch), *f);
    f_gets(romset, sizeof(romset), *f);
    fclose2(f);
    f = fopen2(finfo.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) return;
    fputs((string(arch) + "\n" + string(romset) + "\n" + newName + "\n").c_str(), *f);
    fclose2(f);
}

// UI-free save core (shared with the new UI): the caller has already resolved
// the name and any overwrite question.
bool persistSaveNamed(uint8_t slotnumber, const string& slotName) {
    char persistfname[sizeof(DISK_PSNA_FILE) + 7];
    char persistfinfo[sizeof(DISK_PSNA_FILE) + 7];
    sprintf(persistfname, DISK_PSNA_FILE "%u.sna", slotnumber);
    sprintf(persistfinfo, DISK_PSNA_FILE "%u.esp", slotnumber);
    string finfo = string(DISK_PSNA_DIR) + "/" + persistfinfo;
    FIL* f = fopen2(finfo.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) return false;
    fputs((string(archToStr(Config::arch)) + "\n" + romsetToStr(Config::romSet) + "\n" + slotName + "\n").c_str(), *f);
    fclose2(f);
    return FileSNA::save(string(DISK_PSNA_DIR) + "/" + persistfname);
}




// Throttled progress overlay for IDE::createImage (zero-fill can take seconds).
static void ide_create_progress(uint32_t done, uint32_t total) {
    static int lastpct = -1;
    int pct = total ? (int)((uint64_t)done * 100 / total) : 100;
    if (pct == lastpct) return;
    lastpct = pct;
    // In the new UI this lands in the browser/menu footer status line (the
    // progressOverride hook), which is where every other long operation reports.
    if (OSD::progressOverride) {
        OSD::progressDialog("Creating HDD", "", pct, 1);
        return;
    }
    char msg[32];
    snprintf(msg, sizeof(msg), "Creating HDD... %d%%", pct);
    OSD::osdCenteredMsg(msg, LEVEL_INFO, 0);
}


// Quick Save (the hot key): save into `slotnumber`, keeping the slot's name when it
// already holds a snapshot. The named/interactive save is persistSaveNamed, driven by
// the menu's slot level.
static bool persistSave(uint8_t slotnumber)
{
    FILINFO stat_buf;
    char persistfname[sizeof(DISK_PSNA_FILE) + 7];
    char persistfinfo[sizeof(DISK_PSNA_FILE) + 7];
    sprintf(persistfname, DISK_PSNA_FILE "%u.sna", slotnumber);
    sprintf(persistfinfo, DISK_PSNA_FILE "%u.esp", slotnumber);
    string finfo = string(DISK_PSNA_DIR) + "/" + persistfinfo;

    string slotName;

    // Slot isn't void
    if (f_stat(finfo.c_str(), &stat_buf) == FR_OK) {
        slotName = getSlotName(slotnumber);
        if (slotName == "\x01") slotName = "";
    } else {
        slotName = getDefaultSnapshotName();
    }

    OSD::osdCenteredMsg(OSD_PSNA_SAVING, LEVEL_INFO, 500);

    // Save info file
    FIL* f = fopen2(finfo.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) {
        OSD::osdCenteredMsg(finfo + " - unable to open", LEVEL_ERROR, 5000);
        return false;
    }
    fputs((string(archToStr(Config::arch)) + "\n" + romsetToStr(Config::romSet) + "\n" + slotName + "\n").c_str(), *f);
    fclose2(f);

    string fsna = string(DISK_PSNA_DIR) + "/" + persistfname;
    if (!FileSNA::save(fsna)) {
        OSD::osdCenteredMsg(OSD_PSNA_SAVE_ERR, LEVEL_ERROR, 5000);
    }
    return true;
}

static void f_gets(char* b, size_t sz, FIL& f) {
    UINT br;
    char* bi = b;
    for (size_t i = 0; i < sz; ++i, ++bi) {
        if ( f_read(&f, bi, 1, &br) != FR_OK || br != 1 ) {
            *bi = 0;
            return;
        }
        if (*bi == '\r') {
            --bi;
            continue;
        }
        if (*bi == '\n') {
            *bi = 0;
            return;
        }
        if (*bi == 0) return;
    }
    b[sz - 1] = 0;
}

bool persistLoad(uint8_t slotnumber)
{
    char persistfname[sizeof(DISK_PSNA_FILE) + 7];
    char persistfinfo[sizeof(DISK_PSNA_FILE) + 7];

    sprintf(persistfname, DISK_PSNA_FILE "%u.sna", slotnumber);
    sprintf(persistfinfo, DISK_PSNA_FILE "%u.esp", slotnumber);

    if (!FileSNA::isPersistAvailable(string(DISK_PSNA_DIR) + "/" + persistfname)) {
        OSD::osdCenteredMsg(OSD_PSNA_NOT_AVAIL, LEVEL_INFO);
        return false;
    } else {
        // Read info file
        string finfo = string(DISK_PSNA_DIR) + "/" + persistfinfo;
        FIL* f = fopen2(finfo.c_str(), FA_READ);
        if (!f) {
            OSD::osdCenteredMsg(OSD_PSNA_LOAD_ERR, LEVEL_WARN);
            // printf("Error opening %s\n",persistfinfo);
            return false;
        }
        char buf[256];
        // f_gets keeps the trailing newline — strip it before the table lookup.
        // Unknown/corrupt text degrades to "nothing forced" (the snapshot's own
        // arch detection applies).
        f_gets(buf, sizeof(buf), *f);
        buf[strcspn(buf, "\r\n")] = 0;
        ArchIdx persist_arch = archCanon(archFromStr(buf, A_NONE));
        f_gets(buf, sizeof(buf), *f);
        buf[strcspn(buf, "\r\n")] = 0;
        RomsetIdx persist_romset = romsetFromStr(buf, R_NONE);
        fclose2(f);

        if (!LoadSnapshot(string(DISK_PSNA_DIR) + "/" + persistfname, persist_arch, persist_romset)) {
            OSD::osdCenteredMsg(OSD_PSNA_LOAD_ERR, LEVEL_WARN);
            return false;
        }
        else
        {
            Config::ram_file = string(DISK_PSNA_DIR) + "/" + persistfname;
            Config::last_ram_file = Config::ram_file;
        }
    }

    return true;

}

#define MADCTL_BGR_PIXEL_ORDER (1<<3)
#define MADCTL_ROW_COLUMN_EXCHANGE (1<<5)
#define MADCTL_COLUMN_ADDRESS_ORDER_SWAP (1<<6)
#define MADCTL_MY  (1 << 7) // Row Address Order (Y flip)
#define MADCTL_MX  (1 << 6) // Column Address Order (X flip)

string getMenuPrefix() {
    if (MEM_PG_CNT <= 64) return "ESPectrum ";
    if (MEM_PG_CNT <= 256) return "Murmuzavr 4M/";
    if (MEM_PG_CNT <= 512) return "Murmuzavr 8M/";
    if (MEM_PG_CNT <= 1024) return "Murmuzavr 16M/";
    return "Murmuzavr 32M/";
}

// Forward declarations for hotkey helpers (defined later in file)
static string hkBindingText(int idx);
extern const char* const hkDescEN[];

// Karabas-Pro "Menu"-key (Win/GUI) combos. Hard-wired in ESPectrum::processKeyboard —
// they are not entries of the remappable hotkey table, so the Help page (hotkeysText)
// lists them from here.
static const char* const kProfiHkKeys[] = {
    "Menu+F1-F4", "Menu+F5", "Menu+F7", "Menu+F11",
    "Menu+F12", "Menu+Tab", "Menu+J", "Menu+Esc",
};
static const char* const kProfiHkDescEN[] = {
    "ROMSET 0-3 select", "Turbo FDC", "AY stereo mode", "CPU speed",
    "NMI", "Swap drives A/B", "Joystick type", "Main menu",
};
static constexpr int kProfiHkCount = (int)(sizeof(kProfiHkKeys) / sizeof(kProfiHkKeys[0]));

// Cold-boot into TR-DOS for the current architecture. Mirrors the per-arch
// "Reset to… → TR-DOS" logic (HK_RESET_TO): Profi boots bank1 + SYSEN-style
// TR-DOS, Pentagon/Profi (incl. Gluk 128Kpg) boots the TR-DOS ROM (bank4) with
// romLatch + trdos asserted. 48K/128K have no dedicated TR-DOS cold-boot bank
// (entry is only via the BASIC trap), so they get a plain reset with the disk
// left mounted. Used by the Alt+Enter "download a disk to /tmp and run" path.
void OSD::bootTrdos() {
    Config::ram_file = NO_RAM_FILE;
    Config::last_ram_file = NO_RAM_FILE;
    if (Config::arch == A_PROFI) {
        ESPectrum::reset(1);
        MemESP::romLatch = 1;
        ESPectrum::trdos = true;
    } else if (Z80Ops::isPentagon || Z80Ops::isProfi) {
        ESPectrum::reset(4); // TR-DOS ROM
        MemESP::romLatch = 1;
        ESPectrum::trdos = true;
    } else {
        ESPectrum::reset(0); // 48K/128K: no TR-DOS cold bank — restart, disk stays mounted
    }
}

// OSD Main Loop
// Chooser for the small hotkey menus (NMI, Reset-to): takes a "Title\nRow\nRow\n"
// menu string and returns the 1-based row (0 = Esc), drawn as a pick list. One
// helper so every variant of these menus ports at once.
static uint8_t hotkeyChooser(const string& menu) {
    {
        string  rows[8];
        const char* items[8];
        string  title;
        int n = 0;
        size_t prev = 0, pos;
        bool first = true;
        while ((pos = menu.find('\n', prev)) != string::npos && n < 8) {
            string line = menu.substr(prev, pos - prev);
            if (first) { title = line; first = false; }
            else if (!line.empty()) { rows[n] = line; items[n] = rows[n].c_str(); n++; }
            prev = pos + 1;
        }
        if (!n) return 0;
        nm::gfxBegin();
        const int sel = nm::uiPickList(title.c_str(), items, n);
        nm::gfxEnd();
        return sel < 0 ? 0 : (uint8_t)(sel + 1);
    }
}

void OSD::nmiAction() {
    if (DivMMC::enabled) {
        // DivMMC NMI: automap at 0x0066 handled by preOpcFetch/postOpcFetch
        Z80::triggerNMI();
    } else
    if (Z80Ops::isByte) {
        // ZX Byte: NMI menu with COBMECT mode toggle
        string nmi_menu = MENU_NMI_TITLE;
        nmi_menu += "NMI\n";
        nmi_menu += MENU_BYTE_COBMECT_MODE;
        uint8_t opt = hotkeyChooser(nmi_menu);
        if (opt == 1) {
            Z80::triggerNMI();
        } else if (opt == 2) {
            Config::byte_cobmect_mode = !Config::byte_cobmect_mode;
            Config::save();
            // BYTE and BYTE-compat are both overlays over the Sinclair 48K base.
            MemESP::rom[0].assign_rom(gb_rom_0_sinclair_48k);
            MemESP::registerOverlay(gb_rom_0_sinclair_48k,
                Config::byte_cobmect_mode ? gb_overlay_48k_byte_sovmest : gb_overlay_48k_byte);
            MemESP::recoverPage0();
            osdCenteredMsg(Config::byte_cobmect_mode ? OSD_COBMECT_ON : OSD_COBMECT_OFF, LEVEL_INFO, 500);
        }
    } else if ((Z80Ops::isPentagon || Z80Ops::isProfi)) {
        uint8_t opt = hotkeyChooser(string(MENU_NMI_TITLE) + MENU_NMI_SEL);
        if (opt == 1)
            Z80::triggerNMI();
        else if (opt == 2)
            Z80::triggerNMIDOS();
    } else {
        Z80::triggerNMI();
    }
}

void OSD::do_OSD(fabgl::VirtualKey KeytoESP, bool ALT, bool CTRL) {

    struct AYGuard {
        AYGuard()  { if (Config::audio_driver == 3) send_to_595(LOW(AY_Enable)); }
        ~AYGuard() { if (Config::audio_driver == 3) send_to_595(HIGH(AY_Enable)); }
    } ayGuard;

    // DS80 OSD palette guard.
    //
    // While OSD is open, the Z80/main loop is paused (suspended inside do_OSD()),
    // so EndFrame() never fires — the deferred profi_ds80_*_pending flags cannot
    // be applied here.  The HDMI ISR on core1 keeps scanning the (now frozen) DS80
    // framebuffer, so the Profi screen stays visible behind the dialog.
    //
    // Problem: OSD draws standard ZX colour bytes (indices 0..16) into the
    // framebuffer, but in DS80 mode a framebuffer byte indexes the DS80 packed-pair
    // conv_color table, not the standard ZX palette — so OSD bytes render as garbled
    // striped colours (the reported bug).
    //
    // Fix: the Graphics-layer remap (Graphics8BitPalette::ds80_active + ds80_color_lut,
    // armed for the whole DS80 session) maps each ZX index to profi_pair_lookup[c][c] —
    // a solid Profi colour — so the classic OSD draws in the running app's palette and
    // the background behind it stays fully correct. HDMI never leaves DS80 mode.
    //
    // Edge case: if a machine reset fires during OSD, ESPectrum::reset() clears DS80
    // state directly.  The dtor re-arms activation only if the machine is still DS80.
    struct DS80Guard {
        bool was_active;
        DS80Guard() : was_active(profi_ds80_active) {
            // Cancel any pending activation that arrived just before OSD opened
            // (avoids a spurious framebuffer-zeroing + geometry reset under the dialog).
            VIDEO::profi_ds80_activate_pending = false;
            if (was_active) VIDEO::profi_ds80_osd_active = true;
        }
        ~DS80Guard() {
            if (VIDEO::profi_ds80_osd_active) {
                VIDEO::profi_ds80_osd_active = false;
                // Only touch the DS80 framebuffer if HDMI is STILL in DS80 mode: a machine
                // reset during OSD (switching to 48K/128K from a menu) already called
                // hdmi_set_profi_ds80_mode(false), and anything that re-arms DS80 over a
                // standard framebuffer gives a shifted/garbled screen.
                if (profi_ds80_active) {
                    // DS80 renderer only rewrites the 256 content bytes per row, so any
                    // side-padding the dialog drew over stays dirty — re-blacken it.
                    VIDEO::clearDS80Padding();
                }
            }
            // Cancel any stray deferred flags from transitions during OSD.
            VIDEO::profi_ds80_activate_pending   = false;
            VIDEO::profi_ds80_deactivate_pending = false;
            // Reset-during-OSD: profi_ds80_active was cleared by ESPectrum::reset()
            // directly.  Re-arm activation only if the machine is still DS80.
            if (was_active && !profi_ds80_active && VIDEO::isProfiDS80()) {
                VIDEO::profi_ds80_activate_pending = true;
            }
        }
    } ds80Guard;

    static uint8_t last_sna_row = 0;
    fabgl::VirtualKeyItem Nextkey;

    // Find matching configurable hotkey
    int hkIdx = -1;
    for (int i = 0; i < Config::HK_COUNT; i++) {
        if (Config::hotkeys[i].vk != (uint16_t)fabgl::VK_NONE &&
            Config::hotkeys[i].vk == (uint16_t)KeytoESP &&
            Config::hotkeys[i].alt  == ALT &&
            Config::hotkeys[i].ctrl == CTRL) {
            hkIdx = i;
            break;
        }
    }

    // Alt+` (grave/tilde) or plain PrtScr (the Karabas-Pro hardware combo) —
    // toggle Profi extended keyboard mode (only in Profi arch)
    if (Config::arch == A_PROFI && !CTRL &&
        ((ALT && (KeytoESP == fabgl::VK_GRAVEACCENT || KeytoESP == fabgl::VK_TILDE)) ||
         (!ALT && KeytoESP == fabgl::VK_PRINTSCREEN))) {
        Config::profi_ext_keys = !Config::profi_ext_keys;
        Config::save();
        osdCenteredMsg(Config::profi_ext_keys ? " XT keyboard ON  " : " XT keyboard OFF ", LEVEL_INFO, 500);
        return;
    }

#ifdef VGA_HDMI
    // Mode switches require a hard reset — heap fragmentation breaks runtime
    // framebuffer grow. Save the *old* vm to pending (loaded after reboot for
    // the rollback confirmation), write the new vm to main config, then reset.
    if (hkIdx == Config::HK_VIDMODE_60) { // HDMI 60Hz
        uint8_t &vm = SELECT_VGA ? Config::vga_video_mode : Config::hdmi_video_mode;
        if (vm == Config::VM_640x480_60) return;
        Config::savePendingVideoMode(); // captures old vm
        vm = Config::VM_640x480_60;
        Config::save();
        esp_hard_reset();
    } else
    if (hkIdx == Config::HK_VIDMODE_50) { // HDMI 50Hz
        uint8_t &vm = SELECT_VGA ? Config::vga_video_mode : Config::hdmi_video_mode;
        if (vm == Config::VM_640x480_50) return;
        Config::savePendingVideoMode(); // captures old vm
        vm = Config::VM_640x480_50;
        Config::save();
        esp_hard_reset();
    } else
#endif
    if (hkIdx == Config::HK_HW_INFO) { // Show mem info (Alt+F1)
            OSD::HWInfo();
            if (VIDEO::OSD) OSD::drawStats(); // Redraw stats for 16:9 modes
        } else
        if (hkIdx == Config::HK_TURBO) { // Turbo mode
            ESPectrum::multiplicator += 1;
            if (ESPectrum::multiplicator > 3) {
                ESPectrum::multiplicator = 0;
            }
            CPU::updateStatesInFrame();
        } else
        if (hkIdx == Config::HK_DEBUG) {
            osdDebug();
        }
        else if (hkIdx == Config::HK_LED_TOGGLE) { // Toggle LED indicators
            Config::ledIndicators = !Config::ledIndicators;
            if (!Config::ledIndicators) LED::clear();
            Config::save();
            osdCenteredMsg(Config::ledIndicators ? " LED indicators ON  " : " LED indicators OFF ", LEVEL_INFO, 500);
        } else
        if (hkIdx == Config::HK_POKE) { // Input Poke
            // Standalone hotkey context: install the UI palette for the duration
            // (DS80 swaps the guest palette, gfxEnd puts it back; standard mode is
            // additive).
            nm::gfxBegin();
            nm::act_debugPoke();
            nm::gfxEnd();
        } else
        if (hkIdx == Config::HK_NMI) { // NMI
            nmiAction();
        }
        else
        if (hkIdx == Config::HK_RESET_TO) { // Reset to...
            if (DivMMC::enabled) {
                uint8_t opt = hotkeyChooser(MENU_RESETTO_DIVMMC);
                if (opt == 1) {
                    // Soft Reset: keep DivMMC RAM (ESXDOS sees 0xAA flag, goes to file browser)
                    if (Config::ram_file != NO_RAM_FILE) Config::ram_file = NO_RAM_FILE;
                    Config::last_ram_file = NO_RAM_FILE;
                    DivMMC::reset();
                    ESPectrum::reset();
                } else if (opt == 2) {
                    // Hard Reset: clear DivMMC RAM (ESXDOS re-initializes from scratch)
                    if (Config::ram_file != NO_RAM_FILE) Config::ram_file = NO_RAM_FILE;
                    Config::last_ram_file = NO_RAM_FILE;
                    DivMMC::init();
                    ESPectrum::reset();
                }
            } else
            if (Z80Ops::is48) {
                // 48K - just reset directly
                if (Config::ram_file != NO_RAM_FILE) Config::ram_file = NO_RAM_FILE;
                Config::last_ram_file = NO_RAM_FILE;
                ESPectrum::reset(0);
            } else {
                // Build machine-dependent menu
                string reset_menu;
                if (Config::arch == A_PROFI) {
                    reset_menu = MENU_RESETTO_PROFI;
                } else if ((Z80Ops::isPentagon || Z80Ops::isProfi)) {
                    if (Config::romSet == R_PENT_GLUK)
                        reset_menu = MENU_RESETTO_PENTGLUK;
                    else
                        reset_menu = MENU_RESETTO_PENT;
                } else {
                    reset_menu = MENU_RESETTO_128;
                }

                uint8_t opt = hotkeyChooser(reset_menu);

                if (opt > 0) {
                    if (Config::ram_file != NO_RAM_FILE) Config::ram_file = NO_RAM_FILE;
                    Config::last_ram_file = NO_RAM_FILE;

                    if (Config::arch == A_PROFI) {
                        // Service ROM=1, TR-DOS=2, 128K=3, 48K=4
                        if (opt == 1) {
                            // Service ROM: boot SYS ROM (bank0), SYSEN=true (set by reset(0) for Profi).
                            ESPectrum::reset(0);
                        } else if (opt == 2) {
                            // TR-DOS: per UnrealSpeccy memory.cpp set_mode(RM_DOS) —
                            //   comp.flags |= CF_TRDOS;  comp.p7FFD |= 0x10;
                            // set_banks() then maps bank0 = (CF_TRDOS && p7FFD&0x10)
                            // ? DOS_ROM : ... → the TR-DOS ROM (bank1). The CPU is
                            // reset to PC=0 and TR-DOS cold-starts from its own ROM
                            // vector. Mirror exactly: reset into bank1, then assert
                            // trdos (CF_TRDOS) + romLatch=1 (p7FFD bit4).
                            ESPectrum::reset(1);
                            MemESP::romLatch = 1;
                            ESPectrum::trdos = true;
                        } else if (opt == 3) {
                            // 128K ROM: trdos=false, romLatch=0 → bank2
                            ESPectrum::reset(2);
                        } else if (opt == 4) {
                            // 48K/SOS ROM: trdos=false, romLatch=1 → bank3
                            ESPectrum::reset(3);
                            MemESP::romLatch = 1;
                        }
                    } else if ((Z80Ops::isPentagon || Z80Ops::isProfi) && Config::romSet == R_PENT_GLUK) {
                        // Service (Gluk)=1, TR-DOS=2, 128K=3, 48K=4
                        if (opt == 1) {
                            ESPectrum::reset(3); // Gluk ROM
                        } else if (opt == 2) {
                            ESPectrum::reset(4); // TR-DOS ROM
                            MemESP::romLatch = 1;
                            ESPectrum::trdos = true;
                        } else if (opt == 3) {
                            ESPectrum::reset(0); // 128K
                        } else if (opt == 4) {
                            ESPectrum::reset(1); // 48K BASIC ROM
                            MemESP::pagingLock = 1;
                        }
                    } else if ((Z80Ops::isPentagon || Z80Ops::isProfi)) {
                        // TR-DOS=1, 128K=2, 48K=3
                        if (opt == 1) {
                            ESPectrum::reset(4); // TR-DOS ROM
                            MemESP::romLatch = 1;
                            ESPectrum::trdos = true;
                        } else if (opt == 2) {
                            ESPectrum::reset(0); // 128K
                        } else if (opt == 3) {
                            ESPectrum::reset(1); // 48K BASIC ROM
                            MemESP::pagingLock = 1;
                        }
                    } else { // 128K
                        // 128K=1, 48K=2
                        if (opt == 1) {
                            ESPectrum::reset(0); // 128K
                        } else if (opt == 2) {
                            ESPectrum::reset(1); // 48K BASIC ROM
                            MemESP::pagingLock = 1;
                        }
                    }
                }
            }
        }
        else if (FileUtils::fsMount && hkIdx == Config::HK_DISK) {
            if (DivMMC::enabled) {
                string mFile = nm::browseFile(FileUtils::IMG_Path, MENU_IMG_TITLE, DISK_IMGFILE);
                if (mFile != "") {
                    string fname = FileUtils::IMG_Path + mFile.substr(1);
                    if (FileUtils::getLCaseExt(fname) == "zip") {
                        string zipFname = ZipExtract::extract(fname, DISK_IMGFILE);
                        if (zipFname.empty()) OSD::osdCenteredMsg(OSD_ZIP_ERR, LEVEL_WARN);
                        else if (zipFname != "\x1b") fname = zipFname;
                        else fname.clear();
                    }
                    if (!fname.empty()) {
                        // The slot chooser is a level of the menu.
                        nm::runDiskSlots(IFACE_ESX, fname.c_str());
                        Config::save();
                        ESPectrum::reset();
                        return;
                    }
                }
                if (VIDEO::OSD) OSD::drawStats();
            } else
            while (1) {
                string mFile = nm::browseFile(FileUtils::DSK_Path, MENU_DSK_TITLE, DISK_DSKFILE);
                if (mFile != "") {
                    string fname = FileUtils::DSK_Path + mFile.substr(1);
                    string fprefix = mFile.substr(0,1);
                    if ( fprefix == "1" || fprefix == "2" || fprefix == "3" || fprefix == "4") {

                        // Create empty trd
                        //Debug::log("Create empty trd. Prefix: %s\n",fprefix.c_str());
                        // FIL *fd = fopen2(fname.c_str(), FA_WRITE);
                        // if (!fd) {
                        //     Debug::led_blink();
                        //     break;
                        // }

                        // // TRD info for 40 tracks 2 sides -> Offset 2274, positions 1 - 4 contains disk type + number of files (0) + number of free sectors
                        // unsigned char trdheader[] = { 0x01, 0x17, 0x00, 0xf0, 0x04, 0x10, 0x00, 0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                        // 0x20, 0x20, 0x20, 0x00, 0x00, 0x42, 0x4c, 0x41, 0x4e, 0x4b }; //, 0x20, 0x20, 0x20 };

                        // char buffer[1024] = {0}; // Bloque de 1 KB lleno de ceros

                        // size_t to_write = 655360; // 640 KB

                        // if (fprefix == "1") {
                        //     // 80/2
                        //     trdheader[1] = 0x16;
                        //     trdheader[4] = 0x09;
                        // } else if (fprefix == "2") {
                        //     // 40/2
                        //     to_write >>= 1; // 320 KB
                        // } else if (fprefix == "3") {
                        //     // 80/1
                        //     to_write >>= 1; // 320 KB
                        //     trdheader[1] = 0x18;
                        // } else if (fprefix == "4") {
                        //     // 40/1
                        //     to_write >>= 2; // 160 KB
                        //     trdheader[1] = 0x19;
                        //     trdheader[3] = 0x70;
                        //     trdheader[4] = 0x02;
                        // }

                        // while (to_write > 0) {
                        //     size_t chunk = (to_write < sizeof(buffer)) ? to_write : sizeof(buffer);
                        //     fwrite(buffer, 1, chunk, fd);
                        //     to_write -= chunk;
                        // }

                        // // Write TRD header
                        // f_lseek(fd, 2274);
                        // fwrite(trdheader, 1, sizeof(trdheader), fd);

                        //  f_close(fd);

                        // continue;

                    }

                    string ext = FileUtils::getLCaseExt(fname);
                    if (ext == "zip") {
                        string zipFname = ZipExtract::extract(fname, DISK_DSKFILE);
                        if (zipFname.empty()) { OSD::osdCenteredMsg(OSD_ZIP_ERR, LEVEL_WARN); continue; }
                        if (zipFname == "\x1b") continue;
                        fname = zipFname;
                        ext = FileUtils::getLCaseExt(fname);
                    }
                    if (ext == "trd" || ext == "scl" || ext == "udi" || ext == "fdi" || ext == "td0" || ext == "pro") {
                        printf("Insert disk %s\n",fname.c_str());
                        rvmWD1793InsertDisk(&ESPectrum::fdd, 0, fname);
                    }
                    else if (ext == "mbd") {
                        printf("Insert MB-02 disk %s\n",fname.c_str());
                        if (MB02::enabled) {
                            rvmWD1793InsertDisk(&ESPectrum::mb02_fdd, 0, fname);
                            ESPectrum::mb02_fdd.diskLoadedCyl = -1;
                            ESPectrum::mb02_fdd.diskLoadedSide = -1;
                            MB02::signalDiskChange();
                        } else {
                            OSD::osdCenteredMsg("Enable MB-02+ first", LEVEL_WARN);
                        }
                    }
                    else
                    {
                        Debug::led_blink();
                    }

                    // string fname = FileUtils::DSK_Path + "/" + mFile;
                    // rvmWD1793InsertDisk(&ESPectrum::fdd, 0, fname);
                    Config::save();
                }
                break;
            }
            if (VIDEO::OSD) OSD::drawStats(); // Redraw stats for 16:9 modes
        }
        else if (hkIdx == Config::HK_USB_BOOT) {
            if (confirmReboot(OSD_DLG_USBBOOT)) {
                reset_usb_boot(0, 0);
                while(1);
            }
        }
        else if (hkIdx == Config::HK_GIGASCREEN) {
            // Profi is incompatible with Gigascreen (renderer geometry never
            // touches the prev-FB coherently) — same guard as the Video menu;
            // without it the Alt+PgUp toggle enabled it mid-Profi and the
            // render path SIGBUS-stormed (hw, PICO_DV).
            if (Z80Ops::isProfi) {
                osdCenteredMsg("Gigascreen: not available on Profi", LEVEL_WARN, 1500);
                return;
            }
            Config::gigascreen_onoff = (Config::gigascreen_onoff + 1) % 3; // Off -> On -> Auto -> Off
            bool want_on = (Config::gigascreen_onoff != 0);
            if (want_on && !Config::gigascreen_enabled &&
                !OSD::featureBudgetGate(Subsystems::FEAT_GIGASCREEN)) {
                // Denied / cancelled (a freeing reboot never returns) — stay Off.
                Config::gigascreen_onoff = 0;
                want_on = false;
            }
            if (want_on) {
                if (!Config::gigascreen_enabled) {
                    initGigascreenBlendLUT();
                    Config::gigascreen_enabled = true;
                    GsSubsys::request(true);
                    GsSubsys::apply();
                    if (!VIDEO::vga.prevFrameBuffer) {
                        // OOM — fall back to Off.
                        Config::gigascreen_enabled = false;
                        Config::gigascreen_onoff = 0;
                        VIDEO::gigascreen_enabled = false;
                        VIDEO::gigascreen_auto_countdown = 0;
                    } else {
                        VIDEO::InitPrevBuffer();
                    }
                }
                if (Config::gigascreen_enabled) {
                    VIDEO::gigascreen_enabled = (Config::gigascreen_onoff == 1);
                    VIDEO::gigascreen_auto_countdown = 0;
                }
            } else {
                // Off — release the 52 KB prev framebuffer.
                Config::gigascreen_enabled = false;
                VIDEO::gigascreen_enabled = false;
                VIDEO::gigascreen_auto_countdown = 0;
                GsSubsys::request(false);
            }
            std::string menu = Config::gigascreen_onoff == 1 ? OSD_GIGASCREEN_ON
                             : Config::gigascreen_onoff == 2 ? OSD_GIGASCREEN_AUTO
                             : OSD_GIGASCREEN_OFF;
            osdCenteredMsg(menu, LEVEL_INFO, 500);
            Config::save();
        } else if (hkIdx == Config::HK_MAX_SPEED || KeytoESP == fabgl::VK_NUMLOCK) {
        ESPectrum::maxSpeed = !ESPectrum::maxSpeed;
        std::string menu = ESPectrum::maxSpeed ? OSD_MAXSPEED_ON : OSD_MAXSPEED_OFF;
        osdCenteredMsg(menu, LEVEL_INFO, 500);
        click();
        } else if (hkIdx == Config::HK_PAUSE) {
        CPU::paused = !CPU::paused;
        click();
        } else if (FileUtils::fsMount && hkIdx == Config::HK_LOAD_SNA) {
            string mFile = nm::browseFile(FileUtils::SNA_Path, MENU_SNA_TITLE, DISK_SNAFILE);
            if (mFile != "") {
                Config::save();
                mFile.erase(0, 1);
                string fname = FileUtils::SNA_Path + mFile;
                if (FileUtils::getLCaseExt(fname) == "zip") {
                    string zipFname = ZipExtract::extract(fname, DISK_SNAFILE);
                    if (zipFname.empty()) { OSD::osdCenteredMsg(OSD_ZIP_ERR, LEVEL_WARN); }
                    else if (zipFname != "\x1b") fname = zipFname;
                    else fname.clear();
                }
                if (!fname.empty()) {
                    if(!LoadSnapshot(fname, A_NONE, R_NONE)) {
                        OSD::osdCenteredMsg(OSD_PSNA_LOAD_ERR, LEVEL_WARN);
                    }
                    Config::ram_file = fname;
                    Config::last_ram_file = fname;
                }
            }
            if (VIDEO::OSD) OSD::drawStats(); // Redraw stats for 16:9 modes
        } else if (FileUtils::fsMount && hkIdx == Config::HK_PERSIST_LOAD) {
            // The menu's native slot level (same rows F1 shows), like runDiskSlots.
            nm::runPersist(false);
            return;
        } else if (FileUtils::fsMount && hkIdx == Config::HK_PERSIST_SAVE) {
            nm::runPersist(true);
            return;
        } else if (FileUtils::fsMount && hkIdx == Config::HK_QUICK_LOAD) {
            // Quick Load — load current persist slot without dialog (same as F3 + Enter)
            persistLoad(Config::persist_slot);
        } else if (FileUtils::fsMount && hkIdx == Config::HK_QUICK_SAVE) {
            // Quick Save — save to current persist slot without dialog (same as F4 + F4)
            persistSave(Config::persist_slot);
        } else if (FileUtils::fsMount && hkIdx == Config::HK_LOAD_ANY) {
#if ZIFI_NET_CLIENT
            // When networking is available, F5 first offers a location picker IN the
            // browser window (Local / Remote / Web Archives / Add Remote). The gate is
            // cheap (no blocking ESP round-trip); the network actions check the WiFi link
            // themselves. With networking off, fall straight through to the SD browser.
            // The label is the "root": backing out of the SD browser returns here (below)
            // rather than closing the OSD, so the locations chooser is always reachable.
            // On a fresh F5 press, restore the last browse location across all sources
            // (g_f5_restore); the goto re-entry skips it so back-out lands on the chooser.
            if (f5HasChooser()) g_f5_restore = true;
            f5_locations:
            if (f5HasChooser()) {
                if (!f5Locations()) {                // chose a non-Local action or cancelled
                    if (OSD::net_launch_close) OSD::net_launch_close = false;
                    OSD::net_close_all = false;      // Esc-close consumed → reset
                    if (VIDEO::OSD) OSD::drawStats();
                    return;
                }
                lastLocSet("L");                     // Local (SD) chosen → record as last location
                // fall through to the SD browser below.
            }
#endif
            string mFile;
            string fname;
            string ext;
            bool fromZip = false;

            // Loop to allow re-opening the browser after ZIP cancel
            bool forcePopup = false;
            f5_retry:
#if ZIFI_NET_CLIENT
            // From locations: show a ".." even at the SD root → returns "" → locations.
            OSD::fd_root_parent = f5HasChooser();
#endif
            mFile = nm::browseFile(FileUtils::ALL_Path, MENU_ALL_TITLE, DISK_ALLFILE);
#if ZIFI_NET_CLIENT
            OSD::fd_root_parent = false;             // don't leak into other browser uses
            // ".." at the SD root → locations chooser. Esc ("") just closes the browser
            // (as before) — it does NOT climb a level.
            if (mFile == "\x02UP") goto f5_locations;
#endif
            if (mFile != "") {
                // X prefix = extract ZIP to current folder
                if (mFile[0] == 'X') {
                    fname = FileUtils::ALL_Path + mFile.substr(1);
                    OSD::osdCenteredMsg(OSD_ZIP_EXTRACTING, LEVEL_INFO, 0);
                    int count = ZipExtract::extractAll(fname, FileUtils::ALL_Path);
                    if (count > 0) {
                        char msg[40];
                        snprintf(msg, sizeof(msg), " Extracted %d file(s) ", count);
                        OSD::osdCenteredMsg(msg, LEVEL_INFO, 1000);
                    } else {
                        OSD::osdCenteredMsg(OSD_ZIP_ERR, LEVEL_WARN);
                    }
                    goto f5_retry;
                }
                // P prefix = F5 pressed on a disk/image — force the slot picker.
                if (mFile[0] == 'P') forcePopup = true;
                fname = FileUtils::ALL_Path + mFile.substr(1);
                ext = FileUtils::getLCaseExt(fname);
                fromZip = false;

                // ZIP archive — extract and replace fname/ext/mFile
                if (ext == "zip") {
                    // Lend the dormant Gigascreen prevFB to the extract's inflate
                    // buffers on butter-less boards (no-op on butter — palloc routes
                    // them to XIP PSRAM). Released right after the extract. The lease
                    // helper only exists where Gigascreen + the net arena do (RP2350).
#if ZIFI_NET_CLIENT
                    NetArenaLease zipLease;
#endif
                    string zipFname = ZipExtract::extract(fname, DISK_ALLFILE);
                    if (zipFname.empty()) {
                        OSD::osdCenteredMsg(OSD_ZIP_ERR, LEVEL_WARN);
                        if (VIDEO::OSD) OSD::drawStats();
                        return;
                    }
                    if (zipFname == "\x1b") {
                        // User cancelled ZIP selection — reopen file dialog
                        goto f5_retry;
                    }
                    fname = zipFname;
                    ext = FileUtils::getLCaseExt(fname);
                    // Reconstruct mFile with prefix for Tape::LoadTape compatibility
                    string zipBase = fname.substr(fname.rfind('/') + 1);
                    mFile = mFile.substr(0, 1) + zipBase;
                    fromZip = true;
                }

                if (ext == "tap" || ext == "tzx" || ext == "pzx" || ext == "wav" || ext == "mp3") {
                    // Tape — sync TAP_Path: /tmp/ for ZIP, ALL_Path for normal files
                    FileUtils::TAP_Path = fromZip ? "/tmp/" : FileUtils::ALL_Path;
                    Config::save();
                    // Respect the Auto-start toggle: with auto-start off, force the
                    // "L" (load-only) key so flashload never runs the program — the
                    // machine lands at BASIC. WAV/MP3 always play inside LoadTape.
                    // Don't press Play here: leaving the tape STOPPED until the guest
                    // polls keeps F8 stats out of tape mode while it's merely mounted.
                    if (!Config::tape_autostart && mFile.size() > 0)
                        mFile[0] = 'L';
                    Tape::LoadTape(mFile);
                }
                else if (FileUtils::ifaceForExt(ext) == IFACE_BETA) {
                    // TR-DOS disk. Enter in the browser mounts into Drive A;
                    // F5 in the browser opens the slot picker, where Enter mounts
                    // into the focused slot and the popup stays open for more
                    // operations until Esc closes it back to the file dialog.
                    if (!fromZip) FileUtils::DSK_Path = FileUtils::ALL_Path;
                    if (forcePopup) {
                        Config::driveWP[0] = true;
                        // The slot chooser is a level of the menu.
                        nm::runDiskSlots(IFACE_BETA, fname.c_str());
                        forcePopup = false;
                        goto f5_retry;
                    }
                    rvmWD1793InsertDisk(&ESPectrum::fdd, 0, fname);
                    if (ESPectrum::fdd.disk[0])
                        // TD0 is read-only (no write-back) → always WP regardless of the slot flag.
                        ESPectrum::fdd.disk[0]->writeprotect =
                            Config::driveWP[0]
                            || ESPectrum::fdd.disk[0]->IsTD0File
                            ;
                    Config::save();
                }
                else if (ext == "mbd") {
                    // MB-02+ disk — Enter mounts into Drive 1, F5 opens the popup.
                    if (MB02::enabled) {
                        if (!fromZip) FileUtils::DSK_Path = FileUtils::ALL_Path;
                        if (forcePopup) {
                            Config::mb02WP[0] = true;
                            // The slot chooser is a level of the menu.
                            nm::runDiskSlots(IFACE_MB02, fname.c_str());
                            forcePopup = false;
                            goto f5_retry;
                        }
                        rvmWD1793InsertDisk(&ESPectrum::mb02_fdd, 0, fname);
                        if (ESPectrum::mb02_fdd.disk[0])
                            ESPectrum::mb02_fdd.disk[0]->writeprotect = Config::mb02WP[0];
                        ESPectrum::mb02_fdd.diskLoadedCyl = -1;
                        ESPectrum::mb02_fdd.diskLoadedSide = -1;
                        MB02::signalDiskChange();
                        Config::save();
                    } else {
                        OSD::osdCenteredMsg("Enable MB-02+ first", LEVEL_WARN);
                    }
                }
                else if (ext == "sna" || ext == "z80" || ext == "p") {
                    // Snapshot
                    if (!fromZip) FileUtils::SNA_Path = FileUtils::ALL_Path;
                    Config::save();
                    if (!LoadSnapshot(fname, A_NONE, R_NONE)) {
                        OSD::osdCenteredMsg(OSD_PSNA_LOAD_ERR, LEVEL_WARN);
                    } else if (!fromZip) {
                        Config::ram_file = fname;
                        Config::last_ram_file = fname;
                    }
                }
                else if (ext == "rom" || ext == "bin") {
                    // ALF cartridge — lazy-mount from SD (no flash) and switch into ALF in place.
                    if (loadAlfCart(fname)) return;   // clean exit into the running machine
                }
                else if (ext == "mmc" || ext == "hdf") {
                    // DivMMC/DivIDE image — Enter loads into hd0 (slot 0); F5 opens
                    // the slot popup which mounts in-place and keeps the popup open.
                    // A full ESPectrum::reset() runs after everything is settled.
                    if (DivMMC::enabled) {
                        FileUtils::IMG_Path = FileUtils::ALL_Path;
                        if (forcePopup) {
                            // The slot chooser is a level of the menu.
                            nm::runDiskSlots(IFACE_ESX, fname.c_str());
                            Config::save();
                            ESPectrum::reset();
                            forcePopup = false;
                            return;
                        }
                        Config::esxdos_hdf_image[0] = fname;
                        DivMMC::init();
                        Config::save();
                        ESPectrum::reset();
                    } else {
                        OSD::osdCenteredMsg(OSD_IMG_NEEDS_ESXDOS, LEVEL_WARN);
                    }
                }
                else if (ext == "dls") {
                    // GM.DLS soundbank — convert to a GMWB bank in CONFIG_DIR and flash
                    // it, the same pipeline as Audio→MIDI→GM.DLS→"Convert a .dls".
                    if (!fromZip) FileUtils::DLS_Path = FileUtils::ALL_Path;
                    string outBin = osdConvertDlsToBank(fname);
                    if (!outBin.empty()) {
                        // Enabling GM.DLS (mode 4) is the RAM-heavy engine → gate it
                        // through the SRAM budget manager, like the MIDI menu does.
                        if (Config::midi == 4 || OSD::featureBudgetGate(Subsystems::FEAT_MIDI)) {
                            uint8_t prev_midi = Config::midi;
                            Config::midi = 4;
                            Config::midi_bank = outBin;
                            Midi::enabled = prev_midi; Midi::deinit();
                            Midi::enabled = 4; Midi::init();
                            MidiSubsys::request(true);
                            Config::save();
                            // applyBankLive() loads THIS freshly-converted bank: live on PSRAM
                            // (no reboot), else false → it must be written to flash at early boot.
                            if (MidiSynth::applyBankLive()) {
                                osdCenteredMsg(MSG_MIDI_BANK_OK, LEVEL_OK, 2000);
                            } else if (OSD::msgDialog("DLS Wavetable", MSG_MIDI_BANK_INSTALL_Q) == DLG_YES) {
                                osdCenteredMsg(MSG_MIDI_BANK_FLASHING, LEVEL_INFO, 3000);
                                OSD::esp_hard_reset();   // no PSRAM: provisionAtBoot writes it pre-video
                            }
                        }
                    }
                }
            }
            if (VIDEO::OSD) OSD::drawStats(); // Redraw stats for 16:9 modes
        } else if (hkIdx == Config::HK_TAPE_PLAY) {
            // Start / Stop .tap reproduction
            if (Tape::tapeStatus == TAPE_STOPPED) {
                Tape::Play();
            } else {
                Tape::Stop();
            }
            click();
        } else if (hkIdx == Config::HK_TAPE_BROWSER) {
            // Tape Browser — the menu's native block list (it toasts "No tape
            // inserted" itself). Standalone hotkey context: install the UI palette
            // for the duration.
            nm::gfxBegin();
            nm::act_tapeBrowser();
            nm::gfxEnd();
        } else if (hkIdx == Config::HK_STATS) {
            // Show / hide OnScreen Stats
            {
                uint8_t mode = VIDEO::OSD & 0x03;
                bool hasFdd = ((Z80Ops::isPentagon || Z80Ops::isProfi) || (Z80Ops::is128 && Z80Ops::isByte)
                                || ((Z80Ops::is48 || Z80Ops::is128) && MB02::enabled))
                        && Tape::tapeStatus != TAPE_LOADING
                    && !DivMMC::enabled
                    ;
                uint8_t maxMode = hasFdd ? 3 : 2;

                if (mode == 0)
                    mode = Tape::tapeStatus == TAPE_LOADING ? 1 : 2;
                else
                    mode++;

                if (mode > maxMode) {
                    if ((VIDEO::OSD & 0x04) == 0) {
                        OSD::clearStats();
                        VIDEO::Draw_OSD43 = VIDEO::BottomBorder;
                        VIDEO::brdnextframe = true;
                    }
                    VIDEO::OSD &= 0xfc;
                } else {
                    VIDEO::OSD = (VIDEO::OSD & 0xfc) | mode;
                    if ((VIDEO::OSD & 0x04) == 0) {
                        VIDEO::Draw_OSD43 = VIDEO::BottomBorder_OSD;

                        OSD::drawStats();
                    }
                    ESPectrum::TapeNameScroller = 0;
                }
            }
        } else if (hkIdx == Config::HK_VOL_DOWN) {
            if (VIDEO::OSD == 0) {
                VIDEO::Draw_OSD43 = VIDEO::BottomBorder_OSD;
                VIDEO::OSD = 0x04;
            } else
                VIDEO::OSD |= 0x04;
            ESPectrum::totalseconds = 0;
            ESPectrum::totalsecondsnodelay = 0;
            VIDEO::framecnt = 0;
            if (ESPectrum::aud_volume>ESP_VOLUME_MIN) {
                ESPectrum::aud_volume--;
                pwm_audio_set_volume(ESPectrum::aud_volume);
                Config::aud_volume = ESPectrum::aud_volume;
                ESPectrum::vol_changed = true;
            }
            OSD::drawVolumeBox();
        } else if (hkIdx == Config::HK_VOL_UP) {
            if (VIDEO::OSD == 0) {
                VIDEO::Draw_OSD43  = VIDEO::BottomBorder_OSD;
                VIDEO::OSD = 0x04;
            } else
                VIDEO::OSD |= 0x04;
            ESPectrum::totalseconds = 0;
            ESPectrum::totalsecondsnodelay = 0;
            VIDEO::framecnt = 0;
            if (ESPectrum::aud_volume<ESP_VOLUME_MAX) {
                ESPectrum::aud_volume++;
                pwm_audio_set_volume(ESPectrum::aud_volume);
                Config::aud_volume = ESPectrum::aud_volume;
                ESPectrum::vol_changed = true;
            }
            OSD::drawVolumeBox();
        } else if (hkIdx == Config::HK_HARD_RESET) { // Hard reset
            if (Config::ram_file != NO_RAM_FILE) {
                Config::ram_file = NO_RAM_FILE;
            }
            Config::last_ram_file = NO_RAM_FILE;
            if (Config::arch == A_PROFI) {
                // Profi hard reset = boot Service ROM (bank0, SYSEN), same as
                // Alt-F11 → "Service ROM". reset(0) sets trdos=true to hold SYSEN.
                ESPectrum::reset(0);
            } else
            ESPectrum::reset();
        } else if (hkIdx == Config::HK_REBOOT) { // ESP32 reset
            if (confirmReboot(OSD_DLG_REBOOT)) {
                Config::ram_file = NO_RAM_FILE;
                Config::save();
                esp_hard_reset();
            }
        } else if (hkIdx == Config::HK_MAIN_MENU) {
            // The new fullscreen menu owns this key. AYGuard and DS80Guard at the top
            // of do_OSD already cover this path, and the frame repaint on close is done
            // by ESPectrum::processKeyboard as usual.
            nm::run();
            if (VIDEO::OSD) OSD::drawStats();
            return;
        }
}


// Shows a red panel with error text
void OSD::errorPanel(const string& errormsg) {
    unsigned short x = scrAlignCenterX(OSD_W);
    unsigned short y = scrAlignCenterY(OSD_H);

    if (Config::slog_on)
        printf((errormsg + "\n").c_str());

    VIDEO::vga.fillRect(x, y, OSD_W, OSD_H, zxColor(0, 0));
    VIDEO::vga.rect(x, y, OSD_W, OSD_H, zxColor(7, 0));
    VIDEO::vga.rect(x + 1, y + 1, OSD_W - 2, OSD_H - 2, zxColor(2, 1));
    VIDEO::vga.setFont(Font6x8);
    osdHome();
    VIDEO::vga.setTextColor(zxColor(7, 1), zxColor(2, 1));
    VIDEO::vga.print(ERROR_TITLE);
    osdAt(2, 0);
    VIDEO::vga.setTextColor(zxColor(7, 1), zxColor(0, 0));
    VIDEO::vga.println(errormsg.c_str());
    osdAt(17, 0);
    VIDEO::vga.setTextColor(zxColor(7, 1), zxColor(2, 1));
    VIDEO::vga.print(ERROR_BOTTOM);
}

// Error panel and infinite loop
void OSD::errorHalt(const string& errormsg) {
    errorPanel(errormsg);
    while (1) {
        sleep_ms(5);
    }
}

// W/A
extern "C" void osd_printf(const char* msg, ...) {
    OSD::osdCenteredMsg(msg, LEVEL_WARN, 1000);
}


// Centered message
void OSD::osdCenteredMsg(const string& msg, uint8_t warn_level) {
    osdCenteredMsg(msg,warn_level,1000);
}

void OSD::osdCenteredMsg(const string& msg, uint8_t warn_level, uint16_t millispause) {
    // New-skin toasts. The persistent (millispause == 0) form leaves the UI
    // palette installed, which recolours a DS80 guest screen — keep the classic
    // renderer there; every timed toast is self-contained and safe everywhere.
    if (nm::available() && (millispause > 0 || !profi_ds80_active)) {
        nm::uiOsdMsg(msg.c_str(), warn_level, millispause);
        return;
    }

    // Count lines and find the longest line for proper sizing
    unsigned short nlines = 1;
    size_t maxlen = 0;
    size_t pos = 0, prev = 0;
    while ((pos = msg.find('\n', prev)) != string::npos) {
        size_t len = pos - prev;
        if (len > maxlen) maxlen = len;
        nlines++;
        prev = pos + 1;
    }
    size_t len = msg.length() - prev;
    if (len > maxlen) maxlen = len;

    size_t maxchars = (scrW / 6) - 10;
    if (maxlen > maxchars) maxlen = maxchars;

    const unsigned short h = OSD_FONT_H * (nlines + 2);
    const unsigned short y = scrAlignCenterY(h);
    unsigned short paper;
    unsigned short ink;
    unsigned int j;

    const unsigned short w = (maxlen + 2) * OSD_FONT_W;
    const unsigned short x = scrAlignCenterX(w);

    switch (warn_level) {
    case LEVEL_OK:
        ink = zxColor(7, 1);
        paper = zxColor(4, 0);
        break;
    case LEVEL_ERROR:
        ink = zxColor(7, 1);
        paper = zxColor(2, 0);
        break;
    case LEVEL_WARN:
        ink = zxColor(0, 0);
        paper = zxColor(6, 0);
        break;
    default:
        ink = zxColor(7, 0);
        paper = zxColor(1, 0);
    }

    if (millispause > 0) {
        // Save backbuffer data
        VIDEO::SaveRect.save(x, y, w, h);
    }

    VIDEO::vga.fillRect(x, y, w, h, paper);
    // VIDEO::vga.rect(x - 1, y - 1, w + 2, h + 2, ink);
    VIDEO::vga.setTextColor(ink, paper);
    VIDEO::vga.setFont(Font6x8);
    VIDEO::vga.setCursor(x + OSD_FONT_W, y + OSD_FONT_H);
    VIDEO::vga.print(msg.c_str());

    if (millispause > 0) {
        sleep_ms(millispause); // Pause if needed
        VIDEO::SaveRect.restore_last();
    }
}

const static char* mnem[256] {
    "NOP", // 00
    "LD BC,nn", // 01
    "LD (BC),A", // 02
    "INC BC", // 03
    "INC B", // 04
    "DEC B", // 05
    "LD B,n", // 06
    "RLCA", // 07
    "EX AF,AF'", // 08
    "ADD HL,BC", // 09
    "LD A,(BC)", // 0A
    "DEC BC", // 0B
    "INC C", // 0C
    "DEC C", // 0D
    "LD C,n", // 0E
    "RRCA", // 0F

    "DJNZ d", // 10
    "LD DE,nn", // 11
    "LD (DE),A", // 12
    "INC DE", // 13
    "INC D", // 14
    "DEC D", // 15
    "LD D,n", // 16
    "RLA", // 17
    "JR d", // 18
    "ADD HL,DE", // 19
    "LD A,(DE)", // 1A
    "DEC DE", // 1B
    "INC E", // 1C
    "DEC E", // 1D
    "LD E,n", // 1E
    "RRA", // 1F

    "JR NZ d", // 20
    "LD HL,nn", // 21
    "LD (nn),HL", // 22
    "INC HL", // 23
    "INC H", // 24
    "DEC H", // 25
    "LD H,n", // 26
    "DAA", // 27
    "JR Z,d", // 28
    "ADD HL,HL", // 29
    "LD HL,(nn)", // 2A
    "DEC HL", // 2B
    "INC L", // 2C
    "DEC L", // 2D
    "LD L,n", // 2E
    "CPL", // 2F

    "JR NC d", // 30
    "LD SP,nn", // 31
    "LD (nn),A", // 32
    "INC SP", // 33
    "INC (HL)", // 34
    "DEC (HL)", // 35
    "LD (HL),n", // 36
    "SCF", // 37
    "JR C,d", // 38
    "ADD HL,SP", // 39
    "LD A,(nn)", // 3A
    "DEC SP", // 3B
    "INC A", // 3C
    "DEC A", // 3D
    "LD A,n", // 3E
    "CCF", // 3F

    "LD B,B" , // 40
    "LD B,C", // 41
    "LD B,D", // 42
    "LD B,E", // 43
    "LD B,H", // 44
    "LD B,L", // 45
    "LD B,(HL)", // 46
    "LD B,A", // 47
    "LD C,B", // 48
    "LD C,C", // 49
    "LD C,D", // 4A
    "LD C,E", // 4B
    "LD C,H", // 4C
    "LD C,L", // 4D
    "LD C,(HL)", // 4E
    "LD C,A", // 4F

    "LD D,B", // 50
    "LD D,C", // 51
    "LD D,D", // 52
    "LD D,E", // 53
    "LD D,H", // 54
    "LD D,L", // 55
    "LD D,(HL)", // 56
    "LD D,A", // 57
    "LD E,B", // 58
    "LD E,C", // 59
    "LD E,D", // 5A
    "LD E,E", // 5B
    "LD E,H", // 5C
    "LD E,L", // 5D
    "LD E,(HL)", // 5E
    "LD E,A", // 5F

    "LD H,B", // 60
    "LD H,C", // 61
    "LD H,D", // 62
    "LD H,E", // 63
    "LD H,H", // 64
    "LD H,L", // 65
    "LD H,(HL)", // 66
    "LD H,A", // 67
    "LD L,B", // 68
    "LD L,C", // 69
    "LD L,D", // 6A
    "LD L,E", // 6B
    "LD L,H", // 6C
    "LD L,L", // 6D
    "LD L,(HL)", // 6E
    "LD L,A", // 6F

    "LD (HL),B", // 70
    "LD (HL),C", // 71
    "LD (HL),D", // 72
    "LD (HL),E", // 73
    "LD (HL),H", // 74
    "LD (HL),L", // 75
    "HALT", // 76
    "LD (HL),A", // 77
    "LD A,B", // 78
    "LD A,C", // 79
    "LD A,D", // 7A
    "LD A,E", // 7B
    "LD A,H", // 7C
    "LD A,L", // 7D
    "LD A,(HL)", // 7E
    "LD A,A", // 7F

    "ADD A,B", // 80
    "ADD A,C", // 81
    "ADD A,D", // 82
    "ADD A,E", // 83
    "ADD A,H", // 84
    "ADD A,L", // 85
    "ADD A,(HL)", // 86
    "ADD A,A", // 87
    "ADC A,B", // 88
    "ADC A,C", // 89
    "ADC A,D", // 8A
    "ADC A,E", // 8B
    "ADC A,H", // 8C
    "ADC A,L", // 8D
    "ADC A,(HL)", // 8E
    "ADC A,A", // 8F

    "SUB A,B", // 90
    "SUB A,C", // 91
    "SUB A,D", // 92
    "SUB A,E", // 93
    "SUB A,H", // 94
    "SUB A,L", // 95
    "SUB A,(HL)", // 96
    "SUB A,A", // 97
    "SBC A,B", // 98
    "SBC A,C", // 99
    "SBC A,D", // 9A
    "SBC A,E", // 9B
    "SBC A,H", // 9C
    "SBC A,L", // 9D
    "SBC A,(HL)", // 9E
    "SBC A,A", // 9F

    "AND A,B", // A0
    "AND A,C", // A1
    "AND A,D", // A2
    "AND A,E", // A3
    "AND A,H", // A4
    "AND A,L", // A5
    "AND A,(HL)", // A6
    "AND A,A", // A7
    "XOR A,B", // A8
    "XOR A,C", // A9
    "XOR A,D", // AA
    "XOR A,E", // AB
    "XOR A,H", // AC
    "XOR A,L", // AD
    "XOR A,(HL)", // AE
    "XOR A,A", // AF

    "OR A,B", // B0
    "OR A,C", // B1
    "OR A,D", // B2
    "OR A,E", // B3
    "OR A,H", // B4
    "OR A,L", // B5
    "OR A,(HL)", // B6
    "OR A,A", // B7
    "CP A,B", // B8
    "CP A,C", // B9
    "CP A,D", // BA
    "CP A,E", // BB
    "CP A,H", // BC
    "CP A,L", // BD
    "CP A,(HL)", // BE
    "CP A,A", // BF

    "RET NZ", // C0
    "POP BC", // C1
    "JP NZ,(nn)", // C2
    "JP (nn)", // C3
    "CALL NZ,(nn)", // C4
    "PUSH BC", // C5
    "ADD A,n", // C6
    "RST 0H", // C7
    "RET Z", // C8
    "RET", // C9
    "JP Z,(nn)", // CA
    "bo", // CB
    "CALL Z,(nn)", // CC
    "CALL (nn)", // CD
    "ADC A,n", // CE
    "RST 8H", // CF

    "RET NC", // D0
    "POP DE", // D1
    "JP NC,(nn)", // D2
    "OUT (n),A", // D3
    "CALL NC,(nn)", // D4
    "PUSH DE", // D5
    "SUB A,n", // D6
    "RST 10H", // D7
    "RET C", // D8
    "EXX", // D9
    "JP C,(nn)", // DA
    "IN A,(n)", // DB
    "CALL C,(nn)", // DC
    "op IX:", // DD
    "SBC A,n", // DE
    "RST 18H", // DF

    "RET PO", // E0
    "POP HL", // E1
    "JP PO,(nn)", // E2
    "EX (SP),HL", // E3
    "CALL PO,(nn)", // E4
    "PUSH HL", // E5
    "AND A,n", // E6
    "RST 20H", // E7
    "RET PE", // E8
    "JP (HL)", // E9
    "JP PE,(nn)", // EA
    "EX DE,HL", // EB
    "CALL PE,(nn)", // EC
    "ext:", // ED
    "XOR A,n", // EE
    "RST 28H", // EF

    "RET P", // F0
    "POP AF", // F1
    "JP P,(nn)", // F2
    "DI", // F3
    "CALL P,(nn)", // F4
    "PUSH AF", // F5
    "OR A,n", // F6
    "RST 30H", // F7
    "RET M", // F8
    "LD SP,HL", // F9
    "JP M,(nn)", // FA
    "EI", // FB
    "CALL M,(nn)", // FC
    "op IY:", // FD
    "CP A,n", // FE
    "RST 38H", // FF
};

const char* mnemIX(uint8_t b) {
    switch(b) {
        case 0x09: return "ADD IX,BC";

        case 0x19: return "ADD IX,DE";

        case 0x21: return "LD IX,nn";
        case 0x22: return "LD (nn),IX";
        case 0x23: return "INC IX";
        case 0x24: return "INC IXh";
        case 0x25: return "DEC IXh";
        case 0x26: return "LD IXh,n";

        case 0x29: return "ADD IX,IX";
        case 0x2A: return "LD IX,(nn)";
        case 0x2B: return "DEC IX";
        case 0x2C: return "INC IXl";
        case 0x2D: return "DEC IXl";
        case 0x2E: return "LD IXl,n";

        case 0x34: return "INC (IX+d)";
        case 0x35: return "DEC (IX+d)";
        case 0x36: return "LD (IX+d),n";

        case 0x39: return "ADD IX,SP";

        case 0x44: return "LD B,IXh";
        case 0x45: return "LD B,IXl";
        case 0x46: return "LD B,(IX+d)";

        case 0x4C: return "LD C,IXh";
        case 0x4D: return "LD C,IXl";
        case 0x4E: return "LD C,(IX+d)";

        case 0x54: return "LD D,IXh";
        case 0x55: return "LD D,IXl";
        case 0x56: return "LD D,(IX+d)";

        case 0x5C: return "LD E,IXh";
        case 0x5D: return "LD E,IXl";
        case 0x5E: return "LD E,(IX+d)";

        case 0x60: return "LD IXh,B";
        case 0x61: return "LD IXh,C";
        case 0x62: return "LD IXh,D";
        case 0x63: return "LD IXh,E";
        case 0x64: return "LD IXh,IXh";
        case 0x65: return "LD IXh,IXl";
        case 0x66: return "LD H,(IX+n)";
        case 0x67: return "LD IXh,A";
        case 0x68: return "LD IXl,B";
        case 0x69: return "LD IXl,C";
        case 0x6A: return "LD IXl,D";
        case 0x6B: return "LD IXl,E";
        case 0x6C: return "LD IXl,IXh";
        case 0x6D: return "LD IXl,IXl";
        case 0x6E: return "LD L,(IX+n)";
        case 0x6F: return "LD IXl,A";

        case 0x70: return "LD (IX+d),B";
        case 0x71: return "LD (IX+d),C";
        case 0x72: return "LD (IX+d),D";
        case 0x73: return "LD (IX+d),E";
        case 0x74: return "LD (IX+d),H";
        case 0x75: return "LD (IX+d),L";

        case 0x77: return "LD (IX+d),A";

        case 0x7C: return "LD A,IXh";
        case 0x7D: return "LD A,IXl";
        case 0x7E: return "LD A,(IX+d)";

        case 0x84: return "ADD A,IXh";
        case 0x85: return "ADD A,IXl";
        case 0x86: return "ADD A,(IX+d)";

        case 0x8C: return "ADC A,IXh";
        case 0x8D: return "ADC A,IXl";
        case 0x8E: return "ADC A,(IX+d)";

        case 0x94: return "SUB A,IXh";
        case 0x95: return "SUB A,IXl";
        case 0x96: return "SUB A,(IX+d)";

        case 0x9C: return "SBC A,IXh";
        case 0x9D: return "SBC A,IXl";
        case 0x9E: return "SBC A,(IX+d)";

        case 0xA4: return "AND A,IXh";
        case 0xA5: return "AND A,IXl";
        case 0xA6: return "AND A,(IX+d)";

        case 0xAC: return "XOR A,IXh";
        case 0xAD: return "XOR A,IXl";
        case 0xAE: return "XOR A,(IX+d)";

        case 0xB4: return "OR A,IXh";
        case 0xB5: return "OR A,IXl";
        case 0xB6: return "OR A,(IX+d)";

        case 0xBC: return "CP A,IXh";
        case 0xBD: return "CP A,IXl";
        case 0xBE: return "CP A,(IX+d)";

        case 0xCB: return "IX bits:";

        case 0xE1: return "POP IX";

        case 0xE3: return "EX (SP),IX";

        case 0xE5: return "PUSH IX";

        case 0xE9: return "JP (IX)";

        case 0xF9: return "LD SP,IX";
    }
    return "???";
}

const char* mnemED(uint8_t b) {
    switch(b) {
        case 0x40: return "IN B,(C)";
        case 0x41: return "OUT (C),B";
        case 0x42: return "SBC HL,BC";
        case 0x43: return "LD (nn),BC";
        case 0x44: return "NEG";
        case 0x45: return "RETN";
        case 0x46: return "IM 0";
        case 0x47: return "LD I,A";
        case 0x48: return "IN C,(C)";
        case 0x49: return "OUT (C),C";
        case 0x4A: return "ADC HL,BC";
        case 0x4B: return "LD BC,(nn)";
        case 0x4C: return "NEG";
        case 0x4D: return "RETI";
        case 0x4E: return "IM 0/1";
        case 0x4F: return "LD R,A";

        case 0x50: return "IN D,(C)";
        case 0x51: return "OUT (C),D";
        case 0x52: return "SBC HL,DE";
        case 0x53: return "LD (nn),DE";
        case 0x54: return "NEG";
        case 0x55: return "RETN";
        case 0x56: return "IM 1";
        case 0x57: return "LD A,I";
        case 0x58: return "IN E,(C)";
        case 0x59: return "OUT (C),E";
        case 0x5A: return "ADC HL,DE";
        case 0x5B: return "LD DE,(nn)";
        case 0x5C: return "NEG";
        case 0x5D: return "RETN";
        case 0x5E: return "IM 2";
        case 0x5F: return "LD A,R";

        case 0x60: return "IN H,(C)";
        case 0x61: return "OUT (C),H";
        case 0x62: return "SBC HL,HL";
        case 0x63: return "LD (nn),HL";
        case 0x64: return "NEG";
        case 0x65: return "RETN";
        case 0x66: return "IM 0";
        case 0x67: return "RRD";
        case 0x68: return "IN L,(C)";
        case 0x69: return "OUT (C),L";
        case 0x6A: return "ADC HL,HL";
        case 0x6B: return "LD HL,(nn)";
        case 0x6C: return "NEG";
        case 0x6D: return "RETN";
        case 0x6E: return "IM 0/1";
        case 0x6F: return "RLD";

        case 0x70: return "IN F,(C)";
        case 0x71: return "OUT (C),0";
        case 0x72: return "SBC HL,SP";
        case 0x73: return "LD (nn),SP";
        case 0x74: return "NEG";
        case 0x75: return "RETN";
        case 0x76: return "IM 1";

        case 0x78: return "IN A,(C)";
        case 0x79: return "OUT (C),A";
        case 0x7A: return "ADC HL,SP";
        case 0x7B: return "LD SP,(nn)";
        case 0x7C: return "NEG";
        case 0x7D: return "RETN";
        case 0x7E: return "IM 2";

        case 0xA0: return "LDI";
        case 0xA1: return "CPI";
        case 0xA2: return "INI";
        case 0xA3: return "OUTI";

        case 0xA8: return "LDD";
        case 0xA9: return "CPD";
        case 0xAA: return "IND";
        case 0xAB: return "OUTD";

        case 0xB0: return "LDIR";
        case 0xB1: return "CPIR";
        case 0xB2: return "INIR";
        case 0xB3: return "OUTIR";

        case 0xB8: return "LDDR";
        case 0xB9: return "CPDR";
        case 0xBA: return "INDR";
        case 0xBB: return "OUTDR";
    }
    return "???";
}

const char* const mnemCB[256] = {
    "RLC B", // 00
    "RLC C", // 01
    "RLC D", // 02
    "RLC E", // 03
    "RLC H", // 04
    "RLC L", // 05
    "RLC (HL)", // 06
    "RLC A", // 07
    "RRC B", // 08
    "RRC C", // 09
    "RRC D", // 0A
    "RRC E", // 0B
    "RRC H", // 0C
    "RRC L", // 0D
    "RRC (HL)", // 0E
    "RRC A", // 0F

    "RL B", // 10
    "RL C", // 11
    "RL D", // 12
    "RL E", // 13
    "RL H", // 14
    "RL L", // 15
    "RL (HL)", // 16
    "RL A", // 17
    "RR B", // 18
    "RR C", // 19
    "RR D", // 1A
    "RR E", // 1B
    "RR H", // 1C
    "RR L", // 1D
    "RR (HL)", // 1E
    "RR A", // 1F

    "SLA B", // 20
    "SLA C", // 21
    "SLA D", // 22
    "SLA E", // 23
    "SLA H", // 24
    "SLA L", // 25
    "SLA (HL)", // 26
    "SLA A", // 27
    "SRA B", // 28
    "SRA C", // 29
    "SRA D", // 2A
    "SRA E", // 2B
    "SRA H", // 2C
    "SRA L", // 2D
    "SRA (HL)", // 2E
    "SRA A", // 2F

    "SLL B", // 30
    "SLL C", // 31
    "SLL D", // 32
    "SLL E", // 33
    "SLL H", // 34
    "SLL L", // 35
    "SLL (HL)", // 36
    "SLL A", // 37
    "SRL B", // 38
    "SRL C", // 39
    "SRL D", // 3A
    "SRL E", // 3B
    "SRL H", // 3C
    "SRL L", // 3D
    "SRL (HL)", // 3E
    "SRL A", // 3F

    "BIT 0,B", // 40
    "BIT 0,C", // 41
    "BIT 0,D", // 42
    "BIT 0,E", // 43
    "BIT 0,H", // 44
    "BIT 0,L", // 45
    "BIT 0,(HL)", // 46
    "BIT 0,A", // 47
    "BIT 1,B", // 48
    "BIT 1,C", // 49
    "BIT 1,D", // 4A
    "BIT 1,E", // 4B
    "BIT 1,H", // 4C
    "BIT 1,L", // 4D
    "BIT 1,(HL)", // 4E
    "BIT 1,A", // 4F

    "BIT 2,B", // 50
    "BIT 2,C", // 51
    "BIT 2,D", // 52
    "BIT 2,E", // 53
    "BIT 2,H", // 54
    "BIT 2,L", // 55
    "BIT 2,(HL)", // 56
    "BIT 2,A", // 57
    "BIT 3,B", // 58
    "BIT 3,C", // 59
    "BIT 3,D", // 5A
    "BIT 3,E", // 5B
    "BIT 3,H", // 5C
    "BIT 3,L", // 5D
    "BIT 3,(HL)", // 5E
    "BIT 3,A", // 5F

    "BIT 4,B", // 60
    "BIT 4,C", // 61
    "BIT 4,D", // 62
    "BIT 4,E", // 63
    "BIT 4,H", // 64
    "BIT 4,L", // 65
    "BIT 4,(HL)", // 66
    "BIT 4,A", // 67
    "BIT 5,B", // 68
    "BIT 5,C", // 69
    "BIT 5,D", // 6A
    "BIT 5,E", // 6B
    "BIT 5,H", // 6C
    "BIT 5,L", // 6D
    "BIT 5,(HL)", // 6E
    "BIT 5,A", // 6F

    "BIT 6,B", // 70
    "BIT 6,C", // 71
    "BIT 6,D", // 72
    "BIT 6,E", // 73
    "BIT 6,H", // 74
    "BIT 6,L", // 75
    "BIT 6,(HL)", // 76
    "BIT 6,A", // 77
    "BIT 7,B", // 78
    "BIT 7,C", // 79
    "BIT 7,D", // 7A
    "BIT 7,E", // 7B
    "BIT 7,H", // 7C
    "BIT 7,L", // 7D
    "BIT 7,(HL)", // 7E
    "BIT 7,A", // 7F

    "RES 0,B", // 80
    "RES 0,C", // 81
    "RES 0,D", // 82
    "RES 0,E", // 83
    "RES 0,H", // 84
    "RES 0,L", // 85
    "RES 0,(HL)", // 86
    "RES 0,A", // 87
    "RES 1,B", // 88
    "RES 1,C", // 89
    "RES 1,D", // 8A
    "RES 1,E", // 8B
    "RES 1,H", // 8C
    "RES 1,L", // 8D
    "RES 1,(HL)", // 8E
    "RES 1,A", // 8F

    "RES 2,B", // 90
    "RES 2,C", // 91
    "RES 2,D", // 92
    "RES 2,E", // 93
    "RES 2,H", // 94
    "RES 2,L", // 95
    "RES 2,(HL)", // 96
    "RES 2,A", // 97
    "RES 3,B", // 98
    "RES 3,C", // 99
    "RES 3,D", // 9A
    "RES 3,E", // 9B
    "RES 3,H", // 9C
    "RES 3,L", // 9D
    "RES 3,(HL)", // 9E
    "RES 3,A", // 9F

    "RES 4,B", // A0
    "RES 4,C", // A1
    "RES 4,D", // A2
    "RES 4,E", // A3
    "RES 4,H", // A4
    "RES 4,L", // A5
    "RES 4,(HL)", // A6
    "RES 4,A", // A7
    "RES 5,B", // A8
    "RES 5,C", // A9
    "RES 5,D", // AA
    "RES 5,E", // AB
    "RES 5,H", // AC
    "RES 5,L", // AD
    "RES 5,(HL)", // AE
    "RES 5,A", // AF

    "RES 6,B", // B0
    "RES 6,C", // B1
    "RES 6,D", // B2
    "RES 6,E", // B3
    "RES 6,H", // B4
    "RES 6,L", // B5
    "RES 6,(HL)", // B6
    "RES 6,A", // B7
    "RES 7,B", // B8
    "RES 7,C", // B9
    "RES 7,D", // BA
    "RES 7,E", // BB
    "RES 7,H", // BC
    "RES 7,L", // BD
    "RES 7,(HL)", // BE
    "RES 7,A", // BF

    "SET 0,B", // C0
    "SET 0,C", // C1
    "SET 0,D", // C2
    "SET 0,E", // C3
    "SET 0,H", // C4
    "SET 0,L", // C5
    "SET 0,(HL)", // C6
    "SET 0,A", // C7
    "SET 1,B", // C8
    "SET 1,C", // C9
    "SET 1,D", // CA
    "SET 1,E", // CB
    "SET 1,H", // CC
    "SET 1,L", // CD
    "SET 1,(HL)", // CE
    "SET 1,A", // CF

    "SET 2,B", // 90
    "SET 2,C", // D1
    "SET 2,D", // D2
    "SET 2,E", // D3
    "SET 2,H", // D4
    "SET 2,L", // D5
    "SET 2,(HL)", // D6
    "SET 2,A", // D7
    "SET 3,B", // D8
    "SET 3,C", // D9
    "SET 3,D", // DA
    "SET 3,E", // DB
    "SET 3,H", // DC
    "SET 3,L", // DD
    "SET 3,(HL)", // DE
    "SET 3,A", // DF

    "SET 4,B", // E0
    "SET 4,C", // E1
    "SET 4,D", // E2
    "SET 4,E", // E3
    "SET 4,H", // E4
    "SET 4,L", // E5
    "SET 4,(HL)", // E6
    "SET 4,A", // E7
    "SET 5,B", // E8
    "SET 5,C", // E9
    "SET 5,D", // EA
    "SET 5,E", // EB
    "SET 5,H", // EC
    "SET 5,L", // ED
    "SET 5,(HL)", // EE
    "SET 5,A", // EF

    "SET 6,B", // F0
    "SET 6,C", // F1
    "SET 6,D", // F2
    "SET 6,E", // F3
    "SET 6,H", // F4
    "SET 6,L", // F5
    "SET 6,(HL)", // F6
    "SET 6,A", // F7
    "SET 7,B", // F8
    "SET 7,C", // F9
    "SET 7,D", // FA
    "SET 7,E", // FB
    "SET 7,H", // FC
    "SET 7,L", // FD
    "SET 7,(HL)", // FE
    "SET 7,A" // FF
};

#define BNc(x, b) ((x >> b) & 1 ? '1' : '0')

/// TODO:
static uint16_t dump_pc = 0;

static void saveDumpToFile(uint16_t addr_from, uint16_t addr_to) {
    static const char* fname = DUMP_LOG_PATH;
    FIL* f = fopen2(fname, FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) {
        FileUtils::mkdirParents(CONFIG_DIR);
        f = fopen2(fname, FA_WRITE | FA_CREATE_ALWAYS);
        if (!f) return;
    }

    char line[128];
    UINT bw;

    // Separator
    f_write(f, "========================================\n", 41, &bw);

    // Timestamp
    snprintf(line, sizeof(line), "Dump: #%04X - #%04X\n", addr_from, addr_to);
    f_write(f, line, strlen(line), &bw);

    // Machine info
    snprintf(line, sizeof(line), "Arch: %s  RomSet: %s\n", archToStr(Config::arch), romsetToStr(Config::romSet));
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "ROM in use: %d  romLatch: %d  bankLatch: %d  videoLatch: %d\n",
        MemESP::romInUse, MemESP::romLatch, MemESP::bankLatch, MemESP::videoLatch);
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "pagingLock: %d  page0ram: %d  newSRAM: %d  divmmc: %d\n",
        MemESP::pagingLock, MemESP::page0ram, MemESP::newSRAM,
        MemESP::divmmc_mapped
    );
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "TR-DOS: %s  TR-DOS BIOS: %d\n",
        ESPectrum::trdos ? "on" : "off", Config::trdosBios);
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "portDFFD: %02X  (CPM=%d ROM14=%d DS80=%d NOROM=%d)\n",
        Ports::portDFFD, (Ports::portDFFD & 0x20) != 0, MemESP::romLatch,
        (Ports::portDFFD & 0x80) != 0, (Ports::portDFFD & 0x10) != 0);
    f_write(f, line, strlen(line), &bw);

    // Registers
    f_write(f, "--- Registers ---\n", 18, &bw);

    snprintf(line, sizeof(line), "AF=%04X  BC=%04X  DE=%04X  HL=%04X\n",
        Z80::getRegAF(), Z80::getRegBC(), Z80::getRegDE(), Z80::getRegHL());
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "AF'=%04X BC'=%04X DE'=%04X HL'=%04X\n",
        Z80::getRegAFx(), Z80::getRegBCx(), Z80::getRegDEx(), Z80::getRegHLx());
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "IX=%04X  IY=%04X  SP=%04X  PC=%04X\n",
        Z80::getRegIX(), Z80::getRegIY(), Z80::getRegSP(), Z80::getRegPC());
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "I=%02X R=%02X  IM=%d  IFF1=%d IFF2=%d  Halted=%d\n",
        Z80::getRegI(), Z80::getRegR(), Z80::getIntMode(),
        Z80::isIFF1(), Z80::isIFF2(), Z80::isHalted());
    f_write(f, line, strlen(line), &bw);

    // Flags
    uint8_t fl = Z80::getRegAF() & 0xFF;
    snprintf(line, sizeof(line), "Flags: S=%d Z=%d H=%d P=%d N=%d C=%d\n",
        (fl >> 7) & 1, (fl >> 6) & 1, (fl >> 4) & 1,
        (fl >> 2) & 1, (fl >> 1) & 1, fl & 1);
    f_write(f, line, strlen(line), &bw);

    // Stack top 8 words
    f_write(f, "--- Stack (top 8) ---\n", 22, &bw);
    uint16_t sp = Z80::getRegSP();
    for (int i = 0; i < 8; i++) {
        uint16_t addr = sp + i * 2;
        uint16_t val = MemESP::readbyte(addr) | (MemESP::readbyte(addr + 1) << 8);
        snprintf(line, sizeof(line), "  SP+%02X [%04X] = %04X\n", i * 2, addr, val);
        f_write(f, line, strlen(line), &bw);
    }

    snprintf(line, sizeof(line), "CPU T-states: %u  statesInFrame: %u\n",
        CPU::tstates, CPU::statesInFrame);
    f_write(f, line, strlen(line), &bw);

    // TR-DOS / WD1793 state
    f_write(f, "--- TR-DOS / WD1793 ---\n", 24, &bw);
    rvmWD1793 &wd = ESPectrum::fdd;
    snprintf(line, sizeof(line), "TR-DOS: %s  BIOS: %d  fastmode: %d  WP: A=%d B=%d C=%d D=%d\n",
        ESPectrum::trdos ? "on" : "off", Config::trdosBios,
        Config::trdosFastMode,
        Config::driveWP[0], Config::driveWP[1], Config::driveWP[2], Config::driveWP[3]);
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "WD1793: cmd=%02X status=%04X track=%d sector=%d data=%02X\n",
        wd.command, wd.status, wd.track, wd.sector, wd.data);
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "  drive=%d side=%d dsr=%d retry=%d\n",
        wd.diskS, wd.side, wd.dsr, wd.retry);
    f_write(f, line, strlen(line), &bw);

    snprintf(line, sizeof(line), "  state=%u stepState=%u control=%04X\n",
        wd.state, wd.stepState, wd.control);
    f_write(f, line, strlen(line), &bw);

    for (int d = 0; d < 4; d++) {
        if (wd.disk[d]) {
            snprintf(line, sizeof(line), "  Disk%d: trk=%u sides=%d wp=%d %s%s\n",
                d, wd.disk[d]->tracks, wd.disk[d]->sides,
                wd.disk[d]->writeprotect,
                wd.disk[d]->IsSCLFile ? "[SCL] " : "",
                wd.disk[d]->fname.c_str());
            f_write(f, line, strlen(line), &bw);
        }
    }

    // Memory dump
    f_write(f, "--- Memory dump ---\n", 20, &bw);

    uint32_t from = addr_from;
    uint32_t to = addr_to;
    if (to < from) to += 0x10000; // wrap around

    for (uint32_t a = from & 0xFFF0; a <= to; a += 16) {
        uint16_t addr = a & 0xFFFF;
        int n = snprintf(line, sizeof(line),
            "%04X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X  |",
            addr,
            MemESP::readbyte(addr+0),  MemESP::readbyte(addr+1),
            MemESP::readbyte(addr+2),  MemESP::readbyte(addr+3),
            MemESP::readbyte(addr+4),  MemESP::readbyte(addr+5),
            MemESP::readbyte(addr+6),  MemESP::readbyte(addr+7),
            MemESP::readbyte(addr+8),  MemESP::readbyte(addr+9),
            MemESP::readbyte(addr+10), MemESP::readbyte(addr+11),
            MemESP::readbyte(addr+12), MemESP::readbyte(addr+13),
            MemESP::readbyte(addr+14), MemESP::readbyte(addr+15));
        // ASCII representation
        for (int j = 0; j < 16; j++) {
            uint8_t ch = MemESP::readbyte((addr + j) & 0xFFFF);
            line[n++] = (ch >= 0x20 && ch < 0x7F) ? ch : '.';
        }
        line[n++] = '|';
        line[n++] = '\n';
        line[n] = 0;
        f_write(f, line, n, &bw);
    }

    // NeoGS side. Without it a two-CPU deadlock is undebuggable: the ZX half
    // only ever shows "waiting on #BB", and the code the card is stuck in lives
    // in card RAM, which nothing else can read (hw 2026-08-07, TheLink).
    {
        GS::Snapshot gs;
        if (GS::ngsSnapshot(gs)) {
            f_write(f, "\n--- NeoGS GS-Z80 ---\n", 22, &bw);
            snprintf(line, sizeof(line),
                "PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X\n",
                gs.pc, gs.sp, gs.af, gs.bc, gs.de, gs.hl, gs.ix, gs.iy);
            f_write(f, line, strlen(line), &bw);
            snprintf(line, sizeof(line),
                "GSCFG0=%02X MPAG=%02X MPAGEX=%02X status=%02X INTENA=%02X INTREQ=%02X "
                "clk=%luHz  ZXDMA=%u@%06lX\n",
                gs.cfg0, gs.mpag, gs.mpagex, gs.status, gs.intena, gs.intreq,
                (unsigned long)gs.clock_hz, (unsigned)gs.zxdma,
                (unsigned long)gs.dma_addr);
            f_write(f, line, strlen(line), &bw);
            // Whole GS-Z80 address space as it is mapped right now. Banked
            // slots read 0xFF on SPI-PSRAM boards (see ngsCpuPeek).
            f_write(f, "--- NeoGS memory (GS-Z80 view) ---\n", 35, &bw);
            for (uint32_t a = 0; a < 0x10000; a += 16) {
                uint8_t b[16];
                GS::ngsCpuPeek((uint16_t)a, b, 16);
                int n = snprintf(line, sizeof(line),
                    "%04X: %02X %02X %02X %02X %02X %02X %02X %02X  "
                    "%02X %02X %02X %02X %02X %02X %02X %02X  |",
                    (unsigned)a, b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
                    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                for (int j = 0; j < 16; j++)
                    line[n++] = (b[j] >= 0x20 && b[j] < 0x7F) ? b[j] : '.';
                line[n++] = '|';
                line[n++] = '\n';
                line[n] = 0;
                f_write(f, line, n, &bw);
            }
        }
    }

    f_write(f, "\n", 1, &bw);
    fclose2(f);
}


static uint32_t memSearchResultAddr = 0x10000; // >0xFFFF = no result
static string memSearchHex;
static uint16_t memSearchLastFound = 0;
static uint32_t memDoSearch(uint16_t startAddr);

// Disassemble instruction at addr into out buffer (max maxlen chars)
static void disasmAt(uint16_t addr, char* out, int maxlen) {
    uint8_t b = MemESP::readbyte(addr);
    std::string m;
    int off = 1; // offset to first operand byte
    bool isIY = false;
    if (b == 0xDD || b == 0xFD) {
        isIY = (b == 0xFD);
        uint8_t b1 = MemESP::readbyte(addr + 1);
        if (b1 == 0xCB) {
            m = mnemCB[MemESP::readbyte(addr + 3)];
            auto sp = m.find(" ");
            if (sp != std::string::npos) m.replace(sp, 1, isIY ? " (IY+d)," : " (IX+d),");
            off = 2;
        } else {
            m = mnemIX(b1);
            if (isIY) { auto p = m.find("IX"); if (p != std::string::npos) m.replace(p, 2, "IY"); }
            off = 2;
        }
    } else if (b == 0xED) {
        m = mnemED(MemESP::readbyte(addr + 1));
        off = 2;
    } else if (b == 0xCB) {
        m = mnemCB[MemESP::readbyte(addr + 1)];
        off = 2;
    } else {
        m = mnem[b];
    }
    // Substitute operands
    auto pnn = m.find("nn");
    if (pnn != std::string::npos) {
        uint16_t val = MemESP::readbyte(addr + off) | (MemESP::readbyte(addr + off + 1) << 8);
        char tmp[5]; snprintf(tmp, 5, "%04X", val);
        m.replace(pnn, 2, tmp);
    } else {
        auto pd = m.find("+d");
        if (pd != std::string::npos) {
            int8_t disp = (int8_t)MemESP::readbyte(addr + off);
            char tmp[8]; snprintf(tmp, 8, "%+d", disp);
            m.replace(pd, 2, tmp);
        } else {
            auto pn = m.find("n");
            if (pn != std::string::npos) {
                uint8_t val = MemESP::readbyte(addr + off);
                char tmp[3]; snprintf(tmp, 3, "%02X", val);
                m.replace(pn, 1, tmp);
            } else {
                auto pe = m.find("d");
                if (pe != std::string::npos) {
                    int8_t disp = (int8_t)MemESP::readbyte(addr + off);
                    uint16_t target = addr + 2 + disp; // JR/DJNZ are always 2 bytes
                    char tmp[5]; snprintf(tmp, 5, "%04X", target);
                    m.replace(pe, 1, tmp);
                }
            }
        }
    }
    strncpy(out, m.c_str(), maxlen - 1);
    out[maxlen - 1] = 0;
}

static int instrLen(uint16_t addr) {
    uint8_t b = MemESP::readbyte(addr);
    if (b == 0xDD || b == 0xFD) {
        uint8_t b1 = MemESP::readbyte(addr + 1);
        if (b1 == 0xCB) return 4;
        const char* m = mnemIX(b1);
        if (strstr(m, "nn")) return 4;
        bool has_d = strstr(m, "d") != nullptr;
        bool has_n = strstr(m, "n") != nullptr;
        if (has_d && has_n) return 4;
        if (has_d || has_n) return 3;
        return 2;
    }
    if (b == 0xED) {
        const char* m = mnemED(MemESP::readbyte(addr + 1));
        if (strstr(m, "nn")) return 4;
        return 2;
    }
    if (b == 0xCB) return 2;
    const char* m = mnem[b];
    if (strstr(m, "nn")) return 3;
    if (strstr(m, "n") || strstr(m, "d")) return 2;
    return 1;
}

// ── Debugger chrome ─────────────────────────────────────────────────────────────
// The debugger draws in the UI's full-screen chrome (6x10 menu font, header +
// footer bands, menu palette). Its logic addresses CHARACTER CELLS of a content
// grid whose row 0 is the first disassembly line; the facade below maps cells to
// pixels.
namespace {

enum DbgInk : uint8_t {
    DBG_NORM,       // plain text
    DBG_CUR,        // cursor row of the active section
    DBG_PC,         // the PC line
    DBG_PC_CUR,     // PC line under the cursor
    DBG_HDR,        // "-Pages----" section rules
    DBG_SEL_BYTE,   // memory byte under the column cursor
    DBG_EDIT,       // inline hex editor, idle digits
    DBG_EDIT_CUR,   // inline hex editor, current digit
    DBG_TITLE,      // title bar text
};

struct DbgSkin {
    int  cx, cy;        // pixel origin of content cell (0,0)
    int  code_lines;    // disassembly rows
    int  mem_hdr_row;   // content row of the "-Memory-" rule (Flags aligns to it)
    int  regs_hdr_row;  // content row of the "-Regs-" rule
};
static DbgSkin s_dbg;

static const nm::UiColor kDbgInkNu[] = {
    nm::C_TEXT, nm::C_WHITE, nm::C_ICON_R, nm::C_ICON_R, nm::C_TEXT_DIM,
    nm::C_WHITE, nm::C_WHITE, nm::C_WHITE, nm::C_WHITE,
};
static const nm::UiColor kDbgPaperNu[] = {
    nm::C_PANEL, nm::C_SEL_BG, nm::C_PANEL, nm::C_SEL_BG, nm::C_PANEL,
    nm::C_ICON_R, nm::C_ICON_R, nm::C_SEL_BG, nm::C_PANEL,
};

// Cell width: the glyphs scale horizontally in DS80 (glyphScale 2).
static int dbgCellW() { return UI_FONT_W * nm::Sf.glyphScale; }
static int dbgPX(int col) { return s_dbg.cx + col * dbgCellW(); }
static int dbgPY(int row) { return s_dbg.cy + row * UI_FONT_H; }

static void dbgText(int col, int row, const char* s, DbgInk k) {
    const int x = dbgPX(col), y = dbgPY(row);
    nm::fill(x, y, (int)strlen(s) * dbgCellW(), UI_FONT_H, kDbgPaperNu[k]);
    nm::text(x, y, s, kDbgInkNu[k]);
}

// Row band fill (the code/cursor line background, wider than its text).
static void dbgFillRow(int col, int row, int ncols, DbgInk k) {
    nm::fill(dbgPX(col), dbgPY(row), ncols * dbgCellW(), UI_FONT_H, kDbgPaperNu[k]);
}

// Breakpoint dot on a code row.
static void dbgBpDot(int row) {
    nm::fill(dbgPX(0) + 1, dbgPY(row) + 3, 4 * nm::Sf.glyphScale, 4, nm::C_ICON_R);
}

// Full-screen chrome with an arbitrary title and footer — the new skin's frame,
// shared by the debugger itself and its Dump page. Repaints the whole body.
static void dbgFrameNu(const char* title, const char* footer) {
    using namespace nm;
    gfxComputeSurface();
    gfxInstallPalette();
    const int sc = Sf.glyphScale;             // 1 — DS80 is excluded in dbgBegin
    const int margin = 2 * sc;
    const int ix = margin + sc, iy = 3;
    const int iw = Sf.w - 2 * ix, ih = Sf.h - iy - 3;
    const int hdr_h = UI_FONT_H + 6, foot_h = UI_FONT_H + 4;
    const int pad = 2 * sc;
    fill(0, 0, Sf.w, Sf.h, C_BG);
    roundRect(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, C_SEP, C_PANEL);
    fill(ix, iy, iw, hdr_h, C_PANEL);
    rainbow(ix + pad, iy + 3);
    textClip(ix + pad + rainbowW() + 2 * pad, iy + 4, iw - 4 * pad - rainbowW(),
             title, C_WHITE);
    hline(ix, iy + hdr_h - 1, iw, C_SEP);
    const int fy = iy + ih - foot_h;
    fill(ix, fy, iw, foot_h, C_FOOT_BG);
    hline(ix, fy, iw, C_SEP);
    text(ix + pad, fy + 3, footer, C_TEXT_DIM);
    roundRectBorder(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, C_SEP, C_BG);
    s_dbg.cx = ix + 2 * pad;
    s_dbg.cy = iy + hdr_h + 2;
}

// Frame, title bar and footer. Repaints the whole body background.
static void dbgFrame(const char* section) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Debugger  [%s]", section);
    dbgFrameNu(buf, "Tab Sect  Spc Step  Ent Edit  F5 BP  F1 Help");
}
// Grid geometry for this session. The 20-row content grid needs ~50 columns, which
// every supported mode has — DS80 included, since the surface covers the whole
// framebuffer (640 px / 12-px doubled glyphs = 53 columns).
static void dbgBegin() {
    nm::gfxComputeSurface();
    // 20 content rows: 15 code + memory rule + 4 dump; the right column packs
    // Pages / Regs / Flags into the same 20 (T-states and BPs move under Flags).
    s_dbg.code_lines   = 15;
    s_dbg.mem_hdr_row  = 15;
    s_dbg.regs_hdr_row = 5;
}

// ── Debugger modals ────────────────────────────────────────────────────────────
// The address / breakpoint / memory-search / dump-range forms, built on the
// uiPrompt/uiPickList primitives.

// One-line hex prompt. Returns 0x10000 on Esc (the "cancelled" address).
static uint32_t dbgHexPromptNu(uint16_t init, const char* title) {
    char b[8];
    snprintf(b, sizeof(b), "%04X", init);
    std::string io = b;
    while (1) {
        if (!nm::uiPrompt(title, io, 4)) return 0x10000;
        char* end = nullptr;
        const uint32_t v = strtoul(io.c_str(), &end, 16);
        if (end && !*end && v <= 0xFFFF) return v;
        flushKbd();
        nm::uiToast("Hex address 0000-FFFF", true, 0);
    }
}

// F7: pick the type, then the address.
static void dbgBpAddNu() {
    static const char* const items[] =
        { "PC address", "Port read", "Port write", "Mem write", "Mem read" };
    static const Config::BPType types[] =
        { Config::BP_PC, Config::BP_PORT_READ, Config::BP_PORT_WRITE,
          Config::BP_MEM_WRITE, Config::BP_MEM_READ };
    static const char* const titles[] =
        { "PC breakpoint (hex)", "Port read BP (hex)", "Port write BP (hex)",
          "Mem write BP (hex)", "Mem read BP (hex)" };
    const int sel = nm::uiPickList("Breakpoint type", items, 5);
    if (sel < 0) return;
    const uint32_t a = dbgHexPromptNu(Z80::getRegPC(), titles[sel]);
    if (a > 0xFFFF) return;
    Config::addBreakPoint((uint16_t)a, types[sel]);
    Config::save();
}

// Alt+F7: the breakpoint list. Enter on a PC breakpoint returns its address (the
// code view lands there), F8/Del removes in place, Esc closes. 0xFFFF = no pick.
static int s_bpIdxNu[Config::MAX_BREAKPOINTS];
static int s_bpCountNu = 0;
static void dbgBpRebuildNu() {
    s_bpCountNu = 0;
    for (int i = 0; i < Config::MAX_BREAKPOINTS; i++)
        if (Config::breakPoints[i].type != Config::BP_NONE)
            s_bpIdxNu[s_bpCountNu++] = i;
    for (int i = 0; i < s_bpCountNu - 1; i++)          // by type, then address
        for (int j = i + 1; j < s_bpCountNu; j++) {
            auto &a = Config::breakPoints[s_bpIdxNu[i]];
            auto &b = Config::breakPoints[s_bpIdxNu[j]];
            if (a.type > b.type || (a.type == b.type && a.addr > b.addr)) {
                int t = s_bpIdxNu[i]; s_bpIdxNu[i] = s_bpIdxNu[j]; s_bpIdxNu[j] = t;
            }
        }
}
static void dbgBpRowNu(int i, char* out, size_t outsz) {
    auto &bp = Config::breakPoints[s_bpIdxNu[i]];
    if (bp.type == Config::BP_PC) {
        char mn[13];
        disasmAt(bp.addr, mn, sizeof(mn));
        snprintf(out, outsz, "%s %04X  %s", Config::bpTypeName(bp.type), bp.addr, mn);
    } else {
        snprintf(out, outsz, "%s %04X", Config::bpTypeName(bp.type), bp.addr);
    }
}
static uint16_t dbgBpListNu() {
    bool changed = false;
    int sel = 0;
    uint16_t ret = 0xFFFF;
    while (1) {
        dbgBpRebuildNu();
        if (!s_bpCountNu) {
            // Classic returned silently here; an "Alt+F7 does nothing" report came
            // from exactly that. Say why instead.
            if (!changed) { flushKbd(); nm::uiToast("No breakpoints (F5/F7 to add)", true, 0); }
            break;
        }
        if (sel >= s_bpCountNu) sel = s_bpCountNu - 1;
        uint8_t fkey = 0;
        sel = nm::uiPickListCb("Breakpoints   Enter Go  F8 Remove",
                               s_bpCountNu, dbgBpRowNu, sel, 36, &fkey);
        if (sel < 0) break;
        if (fkey == 8) {
            Config::removeBreakPointAt(s_bpIdxNu[sel]);
            changed = true;
            continue;
        }
        auto &bp = Config::breakPoints[s_bpIdxNu[sel]];
        if (bp.type == Config::BP_PC) ret = bp.addr;
        break;
    }
    if (changed) Config::save();
    return ret;
}

// Alt+F1: hex-bytes search. Feeds the same memSearchHex / memDoSearch state the
// F3 "search next" key reuses. Returning with a hit recenters the code view.
static void dbgMemSearchNu() {
    std::string io = memSearchHex;
    while (1) {
        if (!nm::uiPrompt("Search hex bytes (e.g. 3E05)", io, 16)) return;
        std::string h;
        for (char c : io) {
            if (c == ' ') continue;
            c = (char)toupper((unsigned char)c);
            if (!isxdigit((unsigned char)c)) { h.clear(); break; }
            h += c;
        }
        if (h.length() < 2 || (h.length() & 1)) {
            flushKbd();
            nm::uiToast("Need an even count of hex digits", true, 0);
            continue;
        }
        memSearchHex = h;
        memSearchResultAddr = memDoSearch(0);
        if (memSearchResultAddr <= 0xFFFF) return;   // hit: the view jump shows it
        flushKbd();
        nm::uiToast("Not found", true, 0);
    }
}

// Alt+F2: dump range, two prompts.
static bool dbgDumpRangeNu(uint16_t& from, uint16_t& to) {
    const uint32_t f = dbgHexPromptNu(from, "Dump from (hex)");
    if (f > 0xFFFF) return false;
    const uint32_t t = dbgHexPromptNu(to, "Dump to (hex)");
    if (t > 0xFFFF) return false;
    from = (uint16_t)f;
    to   = (uint16_t)t;
    return true;
}

} // namespace

// F2 memory dump: the whole facade grid (20 rows x 16 bytes). Up/Down +-16,
// PgUp/PgDn a page, 0 top, F8 go, Alt+F2 save range, Esc closes.
void OSD::osdDump() {
    const int rows = s_dbg.mem_hdr_row + 5;      // full content grid (20 rows)
    char buf[52];
    bool alt = false;
    bool redraw_frame = true;
    fabgl::VirtualKeyItem k;
    auto Kbd = ESPectrum::PS2Controller.keyboard();
    while (1) {
        if (redraw_frame) {
            dbgFrameNu("Debugger  [Dump]",
                       "Up/Dn PgUp/Dn Scroll  0 Top  F8 Go  Alt+F2 Save");
            redraw_frame = false;
        }
        for (int i = 0; i < rows; i++) {
            const uint16_t pci = (uint16_t)((dump_pc + i * 16) & 0xFFF0);
            int n = snprintf(buf, sizeof(buf), "%04X  ", pci);
            for (int b = 0; b < 16; b++)
                n += snprintf(buf + n, sizeof(buf) - n, (b & 1) ? "%02X " : "%02X",
                              MemESP::readbyte((uint16_t)(pci + b)));
            dbgText(0, i, buf, DBG_NORM);
        }
        while (!Kbd->virtualKeyAvailable()) sleep_ms(5);
        Kbd->getNextVirtualKey(&k);
        if (k.vk == fabgl::VK_LALT || k.vk == fabgl::VK_RALT) alt = k.down;
        if (!k.down) continue;
        switch (k.vk) {
            case fabgl::VK_ESCAPE:
                return;
            case fabgl::VK_KP_MINUS:
            case fabgl::VK_UP:       dump_pc -= 16;        break;
            case fabgl::VK_KP_PLUS:
            case fabgl::VK_DOWN:     dump_pc += 16;        break;
            case fabgl::VK_PAGEUP:   dump_pc -= rows * 16; break;
            case fabgl::VK_PAGEDOWN: dump_pc += rows * 16; break;
            case fabgl::VK_0:        dump_pc = 0;          break;
            case fabgl::VK_F8: {
                const uint32_t a = dbgHexPromptNu(dump_pc, "Go to (hex)");
                if (a <= 0xFFFF) dump_pc = (uint16_t)a;
                alt = false;
                redraw_frame = true;
                break;
            }
            case fabgl::VK_F2:
                if (alt && FileUtils::fsMount) {
                    if (!FileUtils::checkSDCard()) FileUtils::remountSD();
                    const uint32_t f = dbgHexPromptNu(dump_pc, "Dump from (hex)");
                    if (f <= 0xFFFF) {
                        const uint32_t t =
                            dbgHexPromptNu((uint16_t)((f + 0xFF) & 0xFFFF), "Dump to (hex)");
                        if (t <= 0xFFFF) {
                            saveDumpToFile((uint16_t)f, (uint16_t)t);
                            flushKbd();
                            nm::uiToast("Dump saved", false, 1200);
                        }
                    }
                    alt = false;
                    redraw_frame = true;
                }
                break;
            default: break;
        }
    }
}

void OSD::osdDebug(uint16_t gotoAddr) {
    dbgBegin();
    // Drain the key that opened the debugger: the menu's Enter may still be in the
    // queue (or auto-repeating), and Enter here starts the inline address editor.
    flushKbd();
    const int CODE_LINES = s_dbg.code_lines;

    char buf[40];
    int ii = 3;
    int cursor_row = 3; // cursor starts at PC line
    uint32_t t1 = 0;
    uint32_t t2 = 0;
    uint32_t T1 = 0;
    uint32_t T2 = 0;
    int activeSection = 0; // 0=Code, 1=Pages, 2=Memory, 3=Registers
    int regCursorRow = 0;
    bool regAltSet = false;
    int memCursorRow = 0;
    int memCursorCol = 0; // 0-3 = byte index
    uint16_t memViewAddr = Z80::getRegPC();
    const int XR = 32;      // content column of the right panel
    int pagesCursorRow = 0; // 0=PAGE0, 1=PAGE3, 2=VIDEO, 3=PAGING LOCK
    bool memAsciiMode = false;
    bool gotoApplied = false;
    bool redrawCode = true;
    bool redrawMem = true;
    bool redrawRight = true;
    bool redrawTitle = true;

c:
    sleep_ms(5);

    if (redrawTitle) {
        // Border + title bar (+ footer on the new chrome); repaints the body bg.
        const char* secNames[] = {"Code", "Memory", "Pages", "Regs"};
        dbgFrame(secNames[activeSection]);
    } // redrawTitle

    uint16_t pc = Z80::getRegPC();
    // Apply gotoAddr: set ii so that gotoAddr appears on current cursor_row
    if (gotoAddr != 0xFFFF && !gotoApplied) {
        ii = (int16_t)(pc - gotoAddr) + cursor_row;
        gotoApplied = true;
    }
    const int MEM_LINES = 4;
    int i = 0;
    uint16_t line_addr[18];             // max code_lines across skins
    int regStartRow = 0;
  if (redrawCode) {
    uint16_t pci = pc - ii; // starting address for line 0
    std::string mem;
    for (; i < CODE_LINES; ++i) {
        line_addr[i] = pci;
        int len = instrLen(pci);
        uint8_t bytes[4];
        for (int b = 0; b < len && b < 4; b++)
            bytes[b] = MemESP::readbyte(pci + b);
        // Highlight: red ink for PC, cursor-bar bg for the cursor row
        bool isCursor = (i == cursor_row && activeSection == 0);
        const DbgInk rowInk = (pci == pc && isCursor) ? DBG_PC_CUR
                            : (pci == pc)             ? DBG_PC
                            : isCursor                ? DBG_CUR : DBG_NORM;
        dbgFillRow(0, i, XR - 1, rowInk);   // band spans to the right panel

        // Build hex bytes string (up to 4 bytes, 8 chars + trailing space)
        char hexbytes[10];
        int hpos = 0;
        for (int b = 0; b < len && b < 4; b++) {
            snprintf(hexbytes + hpos, 3, "%02X", bytes[b]);
            hpos += 2;
        }
        while (hpos < 9) hexbytes[hpos++] = ' ';
        hexbytes[9] = 0;

        // Get mnemonic with resolved operands
        uint8_t bi = bytes[0];
        bool isED = (bi == 0xED);
        bool isCB = (bi == 0xCB);
        bool isIX = (bi == 0xDD);
        bool isIY = (bi == 0xFD);
        if (isIX || isIY) {
            uint8_t b1 = bytes[1];
            if (b1 == 0xCB) {
                mem = mnemCB[bytes[3]];
                auto sp = mem.find(" ");
                if (sp != string::npos) mem.replace(sp, 1, isIY ? " (IY+d)," : " (IX+d),");
            } else {
                mem = mnemIX(b1);
            }
            auto pos = mem.find(",(HL)");
            if (pos != string::npos) mem.replace(pos, 5, " ");
            if (isIY) {
                auto ixp = mem.find("IX");
                if (ixp != string::npos) mem.replace(ixp, 2, "IY");
            }
        } else if (isCB) {
            mem = mnemCB[bytes[1]];
        } else if (isED) {
            mem = mnemED(bytes[1]);
        } else {
            mem = mnem[bi];
        }
        // Replace operand placeholders
        const char* memc = mem.c_str();
        if (strstr(memc, "nn") != 0) {
            int off = (isED || isIX || isIY) ? 2 : 1;
            uint16_t addr = MemESP::readbyte(pci + off) | (MemESP::readbyte(pci + off + 1) << 8);
            char tmp[5]; snprintf(tmp, sizeof(tmp), "%04X", addr);
            auto p = mem.find("nn");
            if (p != string::npos) mem.replace(p, 2, tmp);
        } else if (strstr(memc, "n") != 0) {
            int off = (isED || isIX || isIY) ? 2 : 1;
            uint8_t val = MemESP::readbyte(pci + off);
            char tmp[3]; snprintf(tmp, sizeof(tmp), "%02X", val);
            auto p = mem.find("n");
            if (p != string::npos) mem.replace(p, 1, tmp);
        } else if (strstr(memc, "d") != 0) {
            bool ixiy = isIX || isIY;
            int off = ixiy ? 2 : 1;
            int8_t disp = (int8_t)MemESP::readbyte(pci + off);
            char tmp[8];
            if (ixiy) {
                snprintf(tmp, sizeof(tmp), "%+d", disp);
                auto p = mem.find("+d");
                if (p != string::npos) mem.replace(p, 2, tmp);
            } else {
                uint16_t target = pci + 2 + disp;
                snprintf(tmp, sizeof(tmp), "%04X", target);
                auto p = mem.find("d");
                if (p != string::npos) mem.replace(p, 1, tmp);
            }
        }
        if (mem.length() > 15) mem = mem.substr(0, 15);
        snprintf(buf, 40, "%c%04X %s%-15s", pci == pc ? '*' : ' ', pci, hexbytes, mem.c_str());
        dbgText(0, i, buf, rowInk);
        if (Config::numPcBP > 0 && Config::hasBreakPoint(pci, Config::BP_PC))
            dbgBpDot(i);
        pci += len;
    }

  } // redrawCode

  if (redrawMem) {
    // === MEMORY DUMP (bottom of left panel, 4 rows) ===
    int memBytesPerRow = memAsciiMode ? 20 : 8;
    {
        dbgText(0, s_dbg.mem_hdr_row,
                memAsciiMode ? "-Memory (ASCII)---------------"
                             : "-Memory (HEX)-----------------", DBG_HDR);
        for (int row = 0; row < MEM_LINES; row++) {
            const int r = s_dbg.mem_hdr_row + 1 + row;
            uint16_t addr = (memViewAddr + row * memBytesPerRow) & 0xFFFF;
            bool isRow = (row == memCursorRow && activeSection == 1);
            const DbgInk rowInk = isRow ? DBG_CUR : DBG_NORM;
            snprintf(buf, 32, "%04X ", addr);
            dbgText(1, r, buf, rowInk);
            if (memAsciiMode) {
                // ASCII: 20 chars per row (4+1+20 = 25 cols)
                for (int col = 0; col < memBytesPerRow; col++) {
                    uint8_t val = MemESP::readbyte((addr + col) & 0xFFFF);
                    char s[2] = {(char)((val >= 32 && val < 127) ? val : '.'), 0};
                    dbgText(6 + col, r, s,
                            (isRow && col == memCursorCol) ? DBG_SEL_BYTE : rowInk);
                }
                dbgText(6 + memBytesPerRow, r, "  ", DBG_NORM);   // clear rest
            } else {
                // HEX: 8 bytes per row as BBBB BBBB BBBB BBBB, cursor byte marked
                for (int col = 0; col < 8; col++) {
                    uint8_t val = MemESP::readbyte((addr + col) & 0xFFFF);
                    char s[3]; snprintf(s, 3, "%02X", val);
                    const int c = 6 + col * 2 + (col / 2);
                    dbgText(c, r, s, (isRow && col == memCursorCol) ? DBG_SEL_BYTE : rowInk);
                    if ((col & 1) && col < 7) dbgText(c + 2, r, " ", rowInk);
                }
                dbgText(25, r, "  ", DBG_NORM);                   // clear trailing
            }
        }
    }
  } // redrawMem

  if (redrawRight) {
    // === RIGHT PANEL ===
    i = 0;
    // regStartRow set for Enter handler

    // --- PAGES header (row 0) + 4 data rows (1-4) ---
    dbgText(XR, i++, "-Pages-----------", DBG_HDR);
    {
        char pb0[20], pb1[20], pb2[20], pb3[20];
        if (MemESP::ramCurrent[0] && MemESP::ramCurrent[0] < (uint8_t*)0x11000000)
            snprintf(pb0, 20, "PAGE0 -> ROM#%d", MemESP::romInUse);
        else if (MemESP::newSRAM)
            snprintf(pb0, 20, "PAGE0 -> SRAM#%d", MemESP::romLatch);
        else
            snprintf(pb0, 20, "PAGE0 -> RAM#0");
        snprintf(pb1, 20, "PAGE3 -> RAM#%d", MemESP::bankLatch);
        snprintf(pb2, 20, "VIDEO -> RAM#%d", MemESP::videoLatch ? 7 : 5);
        snprintf(pb3, 20, "LOCK %s", MemESP::pagingLock ? "true" : "false");
        const char* pageLabels[] = { pb0, pb1, pb2, pb3 };
        for (int r = 0; r < 4; r++) {
            bool isCur = (r == pagesCursorRow && activeSection == 2);
            snprintf(buf, 32, "%-17s", pageLabels[r]);
            buf[17] = 0;
            dbgText(XR, i, buf, isCur ? DBG_CUR : DBG_NORM);
            i++;
        }
    }

    // --- REGS header --- fixed row
    i = s_dbg.regs_hdr_row;
    dbgText(XR, i++, "-Regs------------", DBG_HDR);
    regStartRow = i; // first data row index
    {
        // Paired registers: show main and alt together
        // Format: "AF 5F88 AF'C011" = 15 chars, fits in 18
        struct PairedReg {
            const char* name; const char* altName;
            uint16_t (*get)(); uint16_t (*getAlt)();
        };
        PairedReg paired[] = {
            {"AF", "AF'", Z80::getRegAF, Z80::getRegAFx},
            {"BC", "BC'", Z80::getRegBC, Z80::getRegBCx},
            {"HL", "HL'", Z80::getRegHL, Z80::getRegHLx},
            {"DE", "DE'", Z80::getRegDE, Z80::getRegDEx},
        };
        for (int r = 0; r < 4; r++) {
            bool isCur = (regCursorRow == r && activeSection == 3);
            snprintf(buf, 10, "%-2s %04X ", paired[r].name, paired[r].get());
            dbgText(XR, i, buf, (isCur && !regAltSet) ? DBG_CUR : DBG_NORM);
            snprintf(buf, 10, "%-3s %04X ", paired[r].altName, paired[r].getAlt());
            dbgText(XR + 8, i, buf, (isCur && regAltSet) ? DBG_CUR : DBG_NORM);
            i++;
        }

        // Single registers: IX, IY, SP, PC
        struct SingleReg {
            const char* name;
            uint16_t (*get)();
        };
        SingleReg singles[] = {
            {"IX", Z80::getRegIX},
            {"IY", Z80::getRegIY},
            {"SP", Z80::getRegSP},
            {"PC", (uint16_t(*)())Z80::getRegPC},
        };
        for (int r = 0; r < 4; r++) {
            int regIdx = 4 + r;
            bool isCur = (regCursorRow == regIdx && activeSection == 3);
            snprintf(buf, 32, "%-2s %04X         ", singles[r].name, singles[r].get());
            buf[17] = 0;
            dbgText(XR, i, buf, isCur ? DBG_CUR : DBG_NORM);
            i++;
        }

        // I, R, IM row (regCursorRow == 8)
        {
            bool isCur = (regCursorRow == 8 && activeSection == 3);
            snprintf(buf, 32, "IR %02X%02X IM %d    ", Z80::getRegI(), Z80::getRegR(), Z80::getIntMode());
            buf[17] = 0;
            dbgText(XR, i, buf, isCur ? DBG_CUR : DBG_NORM);
            i++;
        }

    }

    // --- FLAGS block, aligned with the Memory rule ---
    i = s_dbg.mem_hdr_row;
    dbgText(XR, i++, "-Flags-----------", DBG_HDR);
    {
        uint8_t f = Z80::getRegAF() & 0xFF;
        uint8_t fx = Z80::getRegAFx() & 0xFF;
        snprintf(buf, 32, "%c%c%c%c%c%c%c%c %c%c%c%c%c%c%c%c ",
           BNc(f,7), BNc(f,6), BNc(f,5), BNc(f,4), BNc(f,3), BNc(f,2), BNc(f,1), BNc(f,0),
           BNc(fx,7), BNc(fx,6), BNc(fx,5), BNc(fx,4), BNc(fx,3), BNc(fx,2), BNc(fx,1), BNc(fx,0));
        buf[17] = 0;
        dbgText(XR, i++, buf, DBG_NORM);
    }
    dbgText(XR, i++, "SZ-H-PNC SZ-H-PNC", DBG_NORM);

    // T-states and BP count sit under the Flags block.
    snprintf(buf, 32, "%dT %dus         ", T2 - T1, t2 - t1);
    buf[17] = 0;
    dbgText(XR, i++, buf, DBG_NORM);
    if (Config::numBreakPoints > 0)
        snprintf(buf, 32, "BP(s):%d          ", Config::numBreakPoints);
    else
        snprintf(buf, 32, "                 ");
    buf[17] = 0;
    dbgText(XR, i++, buf, DBG_NORM);
  } // redrawRight

    // Reset redraw flags (default: redraw all on next goto c)
    redrawCode = true;
    redrawMem = true;
    redrawRight = true;
    redrawTitle = false; // title/border only on first draw

    // Wait for a key
    fabgl::VirtualKeyItem Nextkey;
    auto Kbd = ESPectrum::PS2Controller.keyboard();
    bool alt = false;
    while (1) {
        sleep_ms(5);
        if (Kbd->virtualKeyAvailable()) {
            Kbd->getNextVirtualKey(&Nextkey);
            if (Config::joystick == JOY_KEMPSTON) {
                Ports::port[Config::kempstonPort] = 0;
                for (int i = fabgl::VK_JOY_RIGHT; i <= fabgl::VK_JOY_C; i++) {
                    if (Kbd->isVKDown((fabgl::VirtualKey) i)) {
                        bitWrite(Ports::port[Config::kempstonPort], i - fabgl::VK_JOY_RIGHT, 1);
                    }
                }
            }

            if (Nextkey.vk == fabgl::VK_LALT || Nextkey.vk == fabgl::VK_RALT) {
                alt = Nextkey.down;
            }

            if (!Nextkey.down) continue;

            if (Nextkey.vk == fabgl::VK_ESCAPE) {
                // Remount SD if card was removed and reinserted during debug session
                if (!FileUtils::checkSDCard()) {
                    FileUtils::remountSD();
                }
                break;
            } else
            if (Nextkey.vk == fabgl::VK_F7) {
                if (alt) {
                    const uint16_t bpAddr = dbgBpListNu();
                    if (bpAddr != 0xFFFF) { gotoAddr = bpAddr; gotoApplied = false; }
                } else {
                    dbgBpAddNu();
                }
                // The modal consumed the Alt key-up: without this, the NEXT F-key
                // would still read as an Alt-chord.
                alt = false;
                redrawTitle = true;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_F8) {
                {
                    const uint32_t a = dbgHexPromptNu(Z80::getRegPC(), "Set PC to (hex)");
                    if (a <= 0xFFFF && a != Z80::getRegPC()) Z80::setRegPC((uint16_t)a);
                }
                alt = false;
                redrawTitle = true;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_F9 && !alt) {
                nm::act_debugPoke();   // palette already installed
                redrawTitle = true;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_F9 && alt) {
                // Fullscreen debug: show Spectrum screen, step with Space/ALT+Space, ESC to return
                {
                    // Chrome is full-screen: render one guest frame instead. The
                    // paper repaints every frame, the border only on demand — ask
                    // for it or the chrome stays in the border area. DS80: hand the
                    // palette back so the guest frame shows its own colours
                    // (dbgFrame re-installs on return).
                    nm::gfxSuspendPalette();
                    VIDEO::brdnextframe = true;
                    CPU::loop();
                }
                bool fs_alt = false;
                while (1) {
                    sleep_ms(5);
                    if (Kbd->virtualKeyAvailable()) {
                        Kbd->getNextVirtualKey(&Nextkey);
                        if (Nextkey.vk == fabgl::VK_LALT || Nextkey.vk == fabgl::VK_RALT) {
                            fs_alt = Nextkey.down;
                            continue;
                        }
                        if (!Nextkey.down) continue;
                        if (Nextkey.vk == fabgl::VK_ESCAPE) break;
                        if (Nextkey.vk == fabgl::VK_SPACE) {
                            // Step (or step over with ALT)
                            int si = 0;
                            T1 = CPU::tstates;
                            t1 = time_us_32();
                            uint16_t pcs = Z80::getRegPC();
                            pc = pcs;
                            while (si++ < 64*1024 &&
                                (pc == Z80::getRegPC() ||
                                 (fs_alt && pc + 3 != Z80::getRegPC()))
                            ) {
                                CPU::step();
                            }
                            t2 = time_us_32();
                            T2 = CPU::tstates;
                            pc = Z80::getRegPC();
                            ii = 3;
                            // Redraw Spectrum screen after step
                            CPU::loop();
                        }
                    }
                }
                alt = false;
                redrawTitle = true;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_TAB) {
                activeSection = (activeSection + 1) % 4;
                redrawTitle = true;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_UP) {
                if (activeSection == 0) {
                    if (cursor_row > 0) cursor_row--;
                    else {
                        uint16_t start = pc - ii;
                        int step = 1;
                        for (int k = 1; k <= 4; ++k) {
                            if (instrLen(start - k) == k) { step = k; break; }
                        }
                        ii += step;
                    }
                } else if (activeSection == 2) {
                    if (pagesCursorRow > 0) pagesCursorRow--;
                    redrawCode = false; redrawMem = false;
                } else if (activeSection == 1) {
                    if (memCursorRow > 0) memCursorRow--;
                    else memViewAddr = (memViewAddr - (memAsciiMode ? 20 : 8)) & 0xFFFF;
                    redrawCode = false; redrawRight = false;
                } else if (activeSection == 3) {
                    if (regCursorRow > 0) regCursorRow--;
                    redrawCode = false; redrawMem = false;
                }
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_DOWN) {
                if (activeSection == 0) {
                    if (cursor_row < CODE_LINES - 1) cursor_row++;
                    else ii -= instrLen(pc - ii);
                } else if (activeSection == 2) {
                    if (pagesCursorRow < 3) pagesCursorRow++;
                    redrawCode = false; redrawMem = false;
                } else if (activeSection == 1) {
                    if (memCursorRow < MEM_LINES - 1) memCursorRow++;
                    else memViewAddr = (memViewAddr + (memAsciiMode ? 20 : 8)) & 0xFFFF;
                    redrawCode = false; redrawRight = false;
                } else if (activeSection == 3) {
                    if (regCursorRow < 8) regCursorRow++;
                    redrawCode = false; redrawMem = false;
                }
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_LEFT) {
                if (activeSection == 2) {
                    // Pages: cycle value left — affects code view (ROM/bank switch)
                    if (pagesCursorRow == 0) { // PAGE0: ROM
                        if (MemESP::romInUse > 0) MemESP::romInUse--;
                        MemESP::ramCurrent[0] = MemESP::rom[MemESP::romInUse].direct();
                    } else if (pagesCursorRow == 1) { // PAGE3: RAM bank
                        MemESP::bankLatch = (MemESP::bankLatch - 1) & 7;
                        MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3); // slot 3, not bankLatch
                    } else if (pagesCursorRow == 2) { // VIDEO
                        MemESP::videoLatch = MemESP::videoLatch ? 0 : 1;
                    } else if (pagesCursorRow == 3) { // PAGING LOCK
                        MemESP::pagingLock = !MemESP::pagingLock;
                    }
                } else if (activeSection == 1) {
                    if (memCursorCol > 0) memCursorCol--;
                    redrawCode = false; redrawRight = false;
                } else if (activeSection == 3) {
                    regAltSet = !regAltSet;
                    redrawCode = false; redrawMem = false;
                }
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_RIGHT) {
                if (activeSection == 2) {
                    // Pages: cycle value right — affects code view (ROM/bank switch)
                    if (pagesCursorRow == 0) {
                        if (MemESP::romInUse < 3) MemESP::romInUse++;
                        MemESP::ramCurrent[0] = MemESP::rom[MemESP::romInUse].direct();
                    } else if (pagesCursorRow == 1) {
                        MemESP::bankLatch = (MemESP::bankLatch + 1) & 7;
                        MemESP::ramCurrent[3] = MemESP::ram[MemESP::bankLatch].sync(3); // slot 3, not bankLatch
                    } else if (pagesCursorRow == 2) {
                        MemESP::videoLatch = MemESP::videoLatch ? 0 : 1;
                    } else if (pagesCursorRow == 3) {
                        MemESP::pagingLock = !MemESP::pagingLock;
                    }
                } else if (activeSection == 1) {
                    if (memCursorCol < (memAsciiMode ? 19 : 7)) memCursorCol++;
                    redrawCode = false; redrawRight = false;
                } else if (activeSection == 3) {
                    regAltSet = !regAltSet;
                    redrawCode = false; redrawMem = false;
                }
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_0 && activeSection == 0) {
                ii = 3;
                cursor_row = 3;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_PAGEUP) {
                if (activeSection == 0) ii += CODE_LINES;
                else if (activeSection == 1) { memViewAddr = (memViewAddr - MEM_LINES * (memAsciiMode ? 20 : 8)) & 0xFFFF; redrawCode = false; redrawRight = false; }
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_PAGEDOWN) {
                if (activeSection == 0) ii -= CODE_LINES;
                else if (activeSection == 1) { memViewAddr = (memViewAddr + MEM_LINES * (memAsciiMode ? 20 : 8)) & 0xFFFF; redrawCode = false; redrawRight = false; }
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_T && alt) {
                memAsciiMode = !memAsciiMode;
                memCursorCol = 0; // reset col since byte count per row changes
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_F5) {
                uint16_t bp_addr = line_addr[cursor_row];
                if (Config::hasBreakPoint(bp_addr)) {
                    Config::removeBreakPoint(bp_addr);
                } else {
                    Config::addBreakPoint(bp_addr);
                }
                Config::save();
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_F2) {
                if (alt && FileUtils::fsMount) {
                    // Remount SD if needed (card may have been swapped)
                    if (!FileUtils::checkSDCard()) FileUtils::remountSD();
                    uint16_t from_addr = line_addr[cursor_row];
                    uint16_t to_addr = (from_addr + 0xFF) & 0xFFFF;
                    if (dbgDumpRangeNu(from_addr, to_addr)) {
                        saveDumpToFile(from_addr, to_addr);
                        flushKbd();
                        nm::uiToast("Dump saved", false, 1200);
                    }
                } else {
                    osdDump();
                }
                redrawTitle = true;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_F1 && alt) {
                // ALT+F1: Search memory for hex byte sequence
                dbgMemSearchNu();
                if (memSearchResultAddr <= 0xFFFF) {
                    ii = pc - (uint16_t)memSearchResultAddr + cursor_row;
                }
                redrawTitle = true;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_F3 && !alt) {
                // F3: Search next (continues from last ALT+F1 search)
                if (memSearchHex.length() >= 2) {
                    uint32_t result = memDoSearch((memSearchLastFound + 1) & 0xFFFF);
                    if (result <= 0xFFFF) {
                        ii = pc - (uint16_t)result + cursor_row;
                    }
                }
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_F1) {
                nm::uiTextPage("Debugger help", OSD_DBG_HELP_EN);
                redrawTitle = true;
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_RETURN || Nextkey.vk == fabgl::VK_KP_ENTER) {
                // Inline hex editing — shared helper lambda (content-cell coords)
                auto inlineHexEdit = [&](int ecol, int erow, char* hexbuf, int ndigits) -> bool {
                    int hpos = 0;
                    while (1) {
                        for (int p = 0; p < ndigits; p++) {
                            char ch[2] = {hexbuf[p], 0};
                            dbgText(ecol + p, erow, ch, p == hpos ? DBG_EDIT_CUR : DBG_EDIT);
                        }
                        while (!Kbd->virtualKeyAvailable()) sleep_ms(5);
                        fabgl::VirtualKeyItem ek;
                        Kbd->getNextVirtualKey(&ek);
                        if (!ek.down) continue;
                        if (ek.vk >= fabgl::VK_0 && ek.vk <= fabgl::VK_9) {
                            hexbuf[hpos] = '0' + (ek.vk - fabgl::VK_0);
                            if (hpos < ndigits - 1) hpos++;
                        } else if (ek.vk >= fabgl::VK_A && ek.vk <= fabgl::VK_F) {
                            hexbuf[hpos] = 'A' + (ek.vk - fabgl::VK_A);
                            if (hpos < ndigits - 1) hpos++;
                        } else if (ek.vk == fabgl::VK_LEFT) { if (hpos > 0) hpos--; }
                        else if (ek.vk == fabgl::VK_RIGHT) { if (hpos < ndigits - 1) hpos++; }
                        else if (ek.vk == fabgl::VK_BACKSPACE) { if (hpos > 0) { hpos--; hexbuf[hpos] = '0'; } }
                        else if (ek.vk == fabgl::VK_RETURN || ek.vk == fabgl::VK_KP_ENTER) return true;
                        else if (ek.vk == fabgl::VK_ESCAPE) return false;
                    }
                };
                auto parseHex = [](const char* h, int n) -> uint16_t {
                    uint16_t v = 0;
                    for (int i = 0; i < n; i++) {
                        v <<= 4;
                        if (h[i] >= '0' && h[i] <= '9') v += h[i] - '0';
                        else v += h[i] - 'A' + 10;
                    }
                    return v;
                };

                if (activeSection == 0) {
                    // Code: inline address edit — jump to entered address
                    char hexbuf[5];
                    snprintf(hexbuf, 5, "%04X", line_addr[cursor_row]);
                    if (inlineHexEdit(1, cursor_row, hexbuf, 4)) {
                        gotoAddr = parseHex(hexbuf, 4);
                        gotoApplied = false;
                    }
                } else if (activeSection == 1) {
                    // Memory: inline byte edit (left panel, below code)
                    int bpr = memAsciiMode ? 20 : 8;
                    uint16_t addr = (memViewAddr + memCursorRow * bpr + memCursorCol) & 0xFFFF;
                    char hexbuf[3];
                    snprintf(hexbuf, 3, "%02X", MemESP::readbyte(addr));
                    int ccx = memAsciiMode
                        ? (6 + memCursorCol)
                        : (6 + memCursorCol * 2 + (memCursorCol / 2));
                    if (inlineHexEdit(ccx, s_dbg.mem_hdr_row + 1 + memCursorRow, hexbuf, 2)) {
                        MemESP::writebyte(addr, (uint8_t)parseHex(hexbuf, 2));
                    }
                } else if (activeSection == 3) {
                    // Registers: inline edit
                    typedef uint16_t (*RegGetter)();
                    typedef void (*RegSetter)(uint16_t);
                    struct RI { RegGetter get; RegSetter set; bool is8bit; };
                    RI mainRegs[] = {
                        {Z80::getRegAF, Z80::setRegAF, false},
                        {Z80::getRegBC, Z80::setRegBC, false},
                        {Z80::getRegHL, Z80::setRegHL, false},
                        {Z80::getRegDE, Z80::setRegDE, false},
                        {Z80::getRegIX, Z80::setRegIX, false},
                        {Z80::getRegIY, Z80::setRegIY, false},
                        {Z80::getRegSP, Z80::setRegSP, false},
                        {(RegGetter)Z80::getRegPC, Z80::setRegPC, false},
                    };
                    RI altRegs[] = {
                        {Z80::getRegAFx, Z80::setRegAFx, false},
                        {Z80::getRegBCx, Z80::setRegBCx, false},
                        {Z80::getRegHLx, Z80::setRegHLx, false},
                        {Z80::getRegDEx, Z80::setRegDEx, false},
                        {Z80::getRegIX, Z80::setRegIX, false},
                        {Z80::getRegIY, Z80::setRegIY, false},
                        {Z80::getRegSP, Z80::setRegSP, false},
                        {(RegGetter)Z80::getRegPC, Z80::setRegPC, false},
                    };
                    if (regCursorRow < 8) {
                        RI& ri = (regAltSet && regCursorRow < 4) ? altRegs[regCursorRow] : mainRegs[regCursorRow];
                        char hexbuf[5];
                        snprintf(hexbuf, 5, "%04X", ri.get());
                        // For paired regs (0-3): main "%-2s %04X " (val at col 3), alt "%-3s %04X " (val at col 8+4=12)
                        // For singles (4-7): "%-2s %04X" (val at col 3)
                        const int ecol = (regCursorRow < 4 && regAltSet)
                                             ? XR + 8 + 4    // after main(8) + "AF' "(4)
                                             : XR + 3;       // after "AF "(3)
                        if (inlineHexEdit(ecol, regStartRow + regCursorRow, hexbuf, 4)) {
                            ri.set(parseHex(hexbuf, 4));
                        }
                    }
                    // IR row (regCursorRow == 8): skip for now, complex
                }
                goto c;
            } else
            if (Nextkey.vk == fabgl::VK_SPACE) {
                // Step CPU
                int i = 0;
                T1 = CPU::tstates;
                t1 = time_us_32();
                uint16_t pcs = Z80::getRegPC();
                while (i++ < 64*1024 &&
                    (
                        pc == Z80::getRegPC() ||
                        (alt && pc + 3 != Z80::getRegPC()) // CALL nn case
                    )
                ) {
                    CPU::step();
                }
                if (alt && pc + 3 != Z80::getRegPC() && i >= 64*1024) {
                    Config::addBreakPoint(pcs + 3); // CALL nn case - temp BP at return addr
                    break;
                }
                ii -= (int)pc - Z80::getRegPC();
                if (ii > 16) ii = 4;
                if (ii < 0) ii = 4;
                t2 = time_us_32();
                T2 = CPU::tstates;
                redrawTitle = true;
                goto c;
            }
        }
    }
    // Nothing to restore — the caller repaints its own screen (runModal redraws the
    // menu; a resumed emulation repaints the guest frame every frame). The border is
    // on-demand though: without the flag it keeps the chrome (visible when the
    // debugger was opened by hotkey, with no menu behind to repaint). DS80
    // additionally holds our palette in the guest's 16 entries — hand it back
    // (no-op in standard mode; runModal re-installs it for the menu behind us).
    nm::gfxSuspendPalette();
    VIDEO::brdnextframe = true;

}


// inline static uint32_t get_cpu_flash_size(void) {
//     uint8_t rx[4] = {0};
//     get_cpu_flash_jedec_id(rx);
//     return 1u << rx[3];
// }
extern "C" uint8_t linkVGA01;
extern "C" uint8_t link_i2s_code;
extern bool is_i2s_enabled;

extern char __HeapLimit;
extern "C" void *sbrk(intptr_t incr);

size_t getFreeHeap(void) {
    struct mallinfo mi = mallinfo();
    // fordblks = free blocks in free list + sbrk headroom (total really free memory)
    // Add remaining sbrk space that mallinfo doesn't account for
    char *brk = (char *)sbrk(0);
    size_t sbrk_free = (brk < &__HeapLimit) ? (size_t)(&__HeapLimit - brk) : 0;
    return mi.fordblks + sbrk_free;
}

// Upper bound on a single contiguous allocation that will succeed without
// tripping SDK's check_alloc panic. Ignores fordblks (may be fragmented);
// trusts only sbrk headroom, which is always contiguous.
size_t getContiguousHeap(void) {
    char *brk = (char *)sbrk(0);
    return (brk < &__HeapLimit) ? (size_t)(&__HeapLimit - brk) : 0;
}

// Largest single block that malloc() can actually satisfy RIGHT NOW, without
// triggering the SDK's panic-on-OOM. getContiguousHeap() only measures the sbrk
// top gap and is blind to freed blocks in the allocator's free-list (e.g. a 38 KB
// prevFB freed when Gigascreen was turned off), so it badly under-reports after a
// disable→re-enable. getFreeHeap() (total free) over-reports on a fragmented heap.
// This probes the real (non-panicking) allocator by binary search: the only metric
// that reflects what a single allocation can really get.
extern "C" void* __real_malloc(size_t);
extern "C" void  __real_free(void*);
extern "C" size_t getLargestAllocatable(void) {
    extern size_t getFreeHeap(void);
    size_t hi = getFreeHeap();          // a single block can't exceed total free
    if (hi == 0) return 0;
    size_t lo = 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        void* p = __real_malloc(mid);
        if (p) { __real_free(p); lo = mid; } // fits → search higher
        else   { hi = mid - 1; }             // too big → search lower
    }
    return lo;
}

// Generic read-only text dialog with vertical scroll
void (*OSD::textPageOverride)(const char* title, const char* text) = nullptr;

void OSD::showTextDialog(const char* title, const char* text, bool blocking, int* scroll_state) {
    // The fullscreen UI renders text pages itself (only the blocking, stateless
    // form — a live-updating window keeps the plain centered box).
    if (textPageOverride && blocking && !scroll_state) {
        textPageOverride(title, text);
        return;
    }
    if (blocking) {
        click();
        unsigned short sx = scrAlignCenterX(OSD_W);
        unsigned short sy = scrAlignCenterY(OSD_H);
        VIDEO::SaveRect.save(sx, sy, OSD_W, OSD_H);
    }

    // Parse text into line pointers (zero-copy: index into original text).
    // Matches the new UI's text page (UiDialog.cpp MAXL) so a page that fits
    // there is not silently cut short here.
    const int MAX_DLGLINES = 128;
    const char* lineStart[MAX_DLGLINES];
    uint8_t lineLen[MAX_DLGLINES];
    int nlines = 0;
    const char* p = text;
    while (*p && nlines < MAX_DLGLINES) {
        lineStart[nlines] = p;
        const char* eol = p;
        while (*eol && *eol != '\n') eol++;
        int len = eol - p;
        lineLen[nlines] = len > 255 ? 255 : len;
        nlines++;
        p = *eol ? eol + 1 : eol;
    }

    const int visCols = osdMaxCols();
    // rows: 0=OSD_TITLE, 1=title, 2=separator, ... last=OSD_BOTTOM → content rows = maxRows-4
    const int visRows = osdMaxRows() - 4;
    int scroll_local = 0;
    int& scroll = scroll_state ? *scroll_state : scroll_local;
    bool needRedraw = true;

    auto drawContent = [&]() {
        drawOSD(true);
        // Title (row 1)
        osdAt(1, 0);
        VIDEO::vga.setTextColor(zxColor(7, 1), zxColor(1, 0));
        char hdr[42];
        snprintf(hdr, sizeof(hdr), " %s", title);
        int hlen = strlen(hdr);
        while (hlen < visCols) hdr[hlen++] = ' ';
        hdr[visCols] = '\0';
        VIDEO::vga.print(hdr);

        // Separator (row 2)
        osdAt(2, 0);
        VIDEO::vga.setTextColor(zxColor(5, 0), zxColor(1, 0));
        char sep[42];
        memset(sep, '-', visCols);
        sep[visCols] = '\0';
        sep[0] = ' ';
        VIDEO::vga.print(sep);

        // Text lines (rows 3..3+visRows-1)
        char row[42];
        for (int r = 0; r < visRows; r++) {
            osdAt(3 + r, 0);
            int li = scroll + r;
            if (li < nlines) {
                VIDEO::vga.setTextColor(zxColor(7, 0), zxColor(1, 0));
                int len = lineLen[li];
                if (len > visCols) len = visCols;
                memcpy(row, lineStart[li], len);
                memset(row + len, ' ', visCols - len);
            } else {
                VIDEO::vga.setTextColor(zxColor(5, 0), zxColor(1, 0));
                memset(row, ' ', visCols);
            }
            row[visCols] = '\0';
            VIDEO::vga.print(row);
        }

        // Scrollbar on right edge
        if (nlines > visRows) {
            int sbx = osdInsideX() + (visCols - 1) * OSD_FONT_W;
            int sby = osdInsideY() + 3 * OSD_FONT_H;
            int barH = visRows * OSD_FONT_H;
            // Track
            VIDEO::vga.fillRect(sbx, sby, OSD_FONT_W, barH, zxColor(7, 0));
            // Thumb
            int thumbH = (visRows * barH) / nlines;
            if (thumbH < 3) thumbH = 3;
            int thumbY = (scroll * barH) / nlines;
            if (thumbY + thumbH > barH) thumbY = barH - thumbH;
            VIDEO::vga.fillRect(sbx + 1, sby + thumbY, OSD_FONT_W - 2, thumbH, zxColor(0, 0));
        }
    };

    fabgl::VirtualKeyItem Nextkey;
    do {
        if (needRedraw) {
            drawContent();
            needRedraw = false;
        }
        if (!ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
            if (blocking) { sleep_ms(5); continue; }
            else break;  // очередь пуста — выходим, caller сам перерисует позже
        }
        ESPectrum::PS2Controller.keyboard()->getNextVirtualKey(&Nextkey);
        if (!Nextkey.down) continue;
        if (is_enter(Nextkey.vk) || is_back(Nextkey.vk)) {
            click();
            break;
        }
        int maxScroll = nlines > visRows ? nlines - visRows : 0;
        if (Nextkey.vk == fabgl::VK_UP) {
            if (scroll > 0) { scroll--; needRedraw = true; }
        } else if (Nextkey.vk == fabgl::VK_DOWN) {
            if (scroll < maxScroll) { scroll++; needRedraw = true; }
        } else if (Nextkey.vk == fabgl::VK_PAGEUP) {
            scroll -= visRows; if (scroll < 0) scroll = 0; needRedraw = true;
        } else if (Nextkey.vk == fabgl::VK_PAGEDOWN) {
            scroll += visRows; if (scroll > maxScroll) scroll = maxScroll; needRedraw = true;
        }
    } while(blocking);
    if (blocking) VIDEO::SaveRect.restore_last();
}

// 0-based line index of the Uptime row in the info text — lets HWInfo's live
// loop repaint just that one row between full redraws.
static int s_hwinfo_uptime_line = -1;

extern "C" uint32_t uptime_seconds(void); // main.cpp — survives F12/watchdog reboots

static int formatUptimeLine(char* out, int outsz) {
    // Seconds since POWER-ON: the hardware timer resets on every watchdog
    // reboot (F12 / video-mode switch), so main.cpp accumulates the running
    // total across those in a watchdog scratch register.
    uint32_t up_s = uptime_seconds();
    return snprintf(out, outsz, " Uptime         : %dd %02d:%02d:%02d",
        (int)(up_s / 86400), (int)(up_s / 3600 % 24),
        (int)(up_s / 60 % 60), (int)(up_s % 60));
}

static void buildHWInfoText() {
    char (&hwtext)[OSD_INFO_BUF_SZ] = osd_info_buf;
    int pos = 0;

    uint32_t cpu_hz = clock_get_hz(clk_sys) / MHZ;
    uint32_t free_heap = getFreeHeap();

    {
        static const uint16_t vreg_mv[] = {
            550, 600, 650, 700, 750, 800, 850, 900, 950, 1000,
            1050, 1100, 1150, 1200, 1250, 1300, 1350, 1400, 1500,
            1600, 1650, 1700, 1800, 1900, 2000, 2350, 2500, 2650,
            2800, 3000, 3150, 3300
        };
        int vi = vreg_get_voltage();
        int mv = (vi >= 0 && vi < 32) ? vreg_mv[vi] : 0;
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos,
            " Chip model     : RP2350%s %d MHz\n"
            " Chip cores     : 2\n"
            " Chip VREG      : %d.%02d V\n"
            " Chip RAM       : 520 KB\n"
            " Free RAM       : %d KB\n",
            chip_is_rp2350a() ? "A" : "B", (int)cpu_hz,
            mv / 1000, (mv % 1000) / 10,
            (int)(free_heap / 1024));
    }

    {
        uint32_t flash_size = (1 << rx[3]);
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos,
            " Flash size     : %d MB\n"
            " Flash JEDEC ID : %02X-%02X-%02X-%02X\n",
            (int)(flash_size >> 20), rx[0], rx[1], rx[2], rx[3]);
        if (flash_qe) {
            pos += snprintf(hwtext + pos, sizeof(hwtext) - pos,
                " Flash QE bit   : %s\n", flash_qe_text());
            if (flash_qe >= 4)
                pos += snprintf(hwtext + pos, sizeof(hwtext) - pos,
                    " Flash QE diag  : %02X %02X %02X %02X %02X %02X\n",
                    flash_qe_diag[0], flash_qe_diag[1], flash_qe_diag[2],
                    flash_qe_diag[3], flash_qe_diag[4], flash_qe_diag[5]);
        }
    }

#ifndef MURM2
    {
        uint32_t psram32 = psram_size();
        if (psram32) {
            // ID is immutable — read once, not on every 1 Hz refresh (SPI
            // transaction under PSRAM_SPINLOCK, GS may be running on core1)
            static uint8_t rx8[8];
            static bool rx8_valid = false;
            if (!rx8_valid) { psram_id(rx8); rx8_valid = true; }
            pos += snprintf(hwtext + pos, sizeof(hwtext) - pos,
                " PSRAM size     : %d MB\n"
                " PSRAM MF ID/KGD: %02X/%02X\n"
                " PSRAM EID      : %02X%02X-%02X%02X-%02X%02X\n",
                (int)(psram32 >> 20), rx8[0], rx8[1], rx8[2], rx8[3], rx8[4], rx8[5], rx8[6], rx8[7]);
        }
    }
#endif
#ifdef BUTTER_PSRAM_GPIO
    if (butter_psram_size()) {
        uint32_t psram32 = butter_psram_size();
        if (psram32)
            pos += snprintf(hwtext + pos, sizeof(hwtext) - pos,
                "+PSRAM on GP%02d  : %d MB (QSPI)\n", psram_pin, (int)(psram32 >> 20));
        else
            pos += snprintf(hwtext + pos, sizeof(hwtext) - pos,
                " PSRAM on GP%02d  : Not found\n", psram_pin);
    }
#endif

    if (Config::audio_driver == 4)
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " Audio mode     : HDMI\n");
    else if (Config::audio_driver == 3)
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " Audio mode     : AY-3-8910\n");
    else
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " Audio mode     : %s [%02Xh] %s\n",
            (is_i2s_enabled ? "i2s" : "PWM"), link_i2s_code,
            (Config::audio_driver == 0 ? " (auto)" : "(overriden)"));

#ifdef VGA_HDMI
    pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " VGA/HDMI detect: %02Xh\n", linkVGA01);
#endif

    if (!psram_pages)
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " 16K RAM pages  : %d[s%d:b%d:v%d]\n",
            ram_pages + butter_pages + swap_pages, ram_pages, butter_pages, swap_pages);
    else if (!butter_pages)
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " 16K RAM pages  : %d[s%d:p%d:v%d]\n",
            ram_pages + psram_pages + swap_pages, ram_pages, psram_pages, swap_pages);
    else if (!swap_pages)
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " 16K RAM pages  : %d[s%d:b%d:p%d]\n",
            ram_pages + butter_pages + psram_pages, ram_pages, butter_pages, psram_pages);
    else
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " 16K RAM pages  : %d[s%d:b%d:p%d:v%d]\n",
            ram_pages + butter_pages + psram_pages + swap_pages, ram_pages, butter_pages, psram_pages, swap_pages);

    if (DivMMC::enabled) {
        const char* mode_names[] = { "OFF", "DivMMC", "DivIDE", "DivSD" };
        const char* mem_type = DivMMC::use_psram ? "PSRAM" : "swap";
        pos += snprintf(hwtext + pos, sizeof(hwtext) - pos, " %-15s: 128K+8K [%s]\n",
            mode_names[Config::esxdos], mem_type);
    }

    {
        int ln = 0;
        for (int i = 0; i < pos; i++) if (hwtext[i] == '\n') ln++;
        s_hwinfo_uptime_line = ln;
        pos += formatUptimeLine(hwtext + pos, sizeof(hwtext) - pos);
        if (pos < (int)sizeof(hwtext) - 1) { hwtext[pos++] = '\n'; hwtext[pos] = '\0'; }
    }

    pos += snprintf(hwtext + pos, sizeof(hwtext) - pos,
        "\n"
        " Built at %s %s\n"
        " branch '%s' commit [%s]\n"
        " %s\n",
        __DATE__, __TIME__, PICO_GIT_BRANCH, PICO_GIT_COMMIT, PICO_BUILD_NAME);

}

// Snapshot for the new UI's live page (Help > System status, also Alt+F1).
const char* hwInfoText() {
    buildHWInfoText();
    return osd_info_buf;
}

void OSD::HWInfo() {
    // Alt+F1 renders the same page the Help branch shows: 1 Hz live ticks.
    {
        // Current scanout mode, right-aligned in the header. The driver table is
        // the truth (VIDEO::video_mode tracks arch-dependent 50 Hz variants too);
        // output width is screen_width doubled, freq is the nominal 50/60.
        char vmode[24];
#ifdef VGA_HDMI
        const struct video_mode_t vm = graphics_get_video_mode(VIDEO::video_mode);
        snprintf(vmode, sizeof(vmode), "%dx%d @%dHz",
                 vm.screen_width * 2, vm.v_active, vm.freq);
#else
        // TFT/SOFTTV: no scanout-mode table (VIDEO::video_mode exists only on
        // VGA/HDMI) — show the framebuffer geometry instead.
        snprintf(vmode, sizeof(vmode), "%dx%d", VIDEO::vga.xres, VIDEO::vga.yres);
#endif
        nm::gfxBegin();
        nm::uiTextPageLive(TXT_INFO_SYSTEM, hwInfoText, 1000, vmode);
        nm::gfxEnd();
    }
}

void OSD::ChipInfo() {
    char (&buf)[OSD_INFO_BUF_SZ] = osd_info_buf;
    int pos = 0;
    uint32_t cpu_hz = clock_get_hz(clk_sys) / MHZ;
    uint32_t free_heap = getFreeHeap();

    {
        static const uint16_t vreg_mv[] = {
            550, 600, 650, 700, 750, 800, 850, 900, 950, 1000,
            1050, 1100, 1150, 1200, 1250, 1300, 1350, 1400, 1500,
            1600, 1650, 1700, 1800, 1900, 2000, 2350, 2500, 2650,
            2800, 3000, 3150, 3300
        };
        int vi = vreg_get_voltage();
        int mv = (vi >= 0 && vi < 32) ? vreg_mv[vi] : 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            " Chip model     : RP2350%s\n"
            " Chip cores     : 2\n"
            " CPU frequency  : %d MHz\n"
            " VREG voltage   : %d.%02d V\n"
            " Chip RAM       : 520 KB\n"
            " Free RAM       : %d KB\n",
            chip_is_rp2350a() ? "A" : "B",
            (int)cpu_hz,
            mv / 1000, (mv % 1000) / 10,
            (int)(free_heap / 1024));
    }

    {
        uint32_t flash_size = (1 << rx[3]);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            " Flash size     : %d MB\n"
            " Flash JEDEC ID : %02X-%02X-%02X-%02X\n",
            (int)(flash_size >> 20), rx[0], rx[1], rx[2], rx[3]);
        if (flash_qe) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                " Flash QE bit   : %s\n", flash_qe_text());
            if (flash_qe >= 4)
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    " Flash QE diag  : %02X %02X %02X %02X %02X %02X\n",
                    flash_qe_diag[0], flash_qe_diag[1], flash_qe_diag[2],
                    flash_qe_diag[3], flash_qe_diag[4], flash_qe_diag[5]);
        }
    }

#ifndef MURM2
    {
        uint32_t psram32 = psram_size();
        if (psram32) {
            uint8_t rx8[8];
            psram_id(rx8);
            size_t psram_used = (size_t)psram_pages * MEM_PG_SZ;
            size_t psram_free = psram32 > psram_used ? psram32 - psram_used : 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                " PSRAM size     : %d MB\n"
                " PSRAM MF ID/KGD: %02X/%02X\n"
                " PSRAM EID      : %02X%02X-%02X%02X-%02X%02X\n"
                " Free PSRAM     : %d KB\n",
                (int)(psram32 >> 20), rx8[0], rx8[1], rx8[2], rx8[3], rx8[4], rx8[5], rx8[6], rx8[7],
                (int)(psram_free / 1024));
        }
    }
#endif
#ifdef BUTTER_PSRAM_GPIO
    {
        uint32_t psram32 = butter_psram_size();
        if (psram32) {
            size_t butter_used = (size_t)butter_pages * MEM_PG_SZ;
            size_t butter_free = psram32 > butter_used ? psram32 - butter_used : 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "+PSRAM on GP%02d  : %d MB (QSPI)\n"
                " Free PSRAM     : %d KB\n", psram_pin, (int)(psram32 >> 20), (int)(butter_free / 1024));
        }
    }
#endif

    // On-chip temperature sensor via ADC
    {
        // Register bases differ between chips
        volatile uint32_t *resets     = (volatile uint32_t *)0x40020000;
        volatile uint32_t *adc_cs     = (volatile uint32_t *)0x400a0000;
        volatile uint32_t *adc_result = (volatile uint32_t *)0x400a0004;
        uint32_t ts_ch = chip_is_rp2350a() ? 4 : 8;

        // Unreset ADC block: clear bit 0 in RESETS_RESET, wait bit 0 in RESET_DONE
        resets[0] &= ~1u;                      // RESET: clear ADC bit
        while (!(resets[2] & 1u)) {}            // RESET_DONE: wait ADC ready

        *adc_cs = 1; // EN=1
        while (!(*adc_cs & (1 << 8))) {} // wait READY
        *adc_cs = (ts_ch << 12) | (1 << 1) | 1; // AINSEL=ch, TS_EN=1, EN=1
        sleep_ms(1); // let temp sensor stabilize
        *adc_cs = (ts_ch << 12) | (1 << 2) | (1 << 1) | 1; // + START_ONCE
        while (!(*adc_cs & (1 << 8))) {} // wait READY
        uint16_t raw = *adc_result & 0xFFF;
        *adc_cs = 0; // disable ADC

        // T = 27 - (V - 0.706) / 0.001721, V = raw * 3.3 / 4096
        // Integer: T*10 = 270 - (raw*33000/4096 - 7060) * 100 / 1721
        int uv10 = (int)raw * 33000 / 4096; // voltage * 10000 (0..33000)
        int temp_x10 = 270 - (uv10 - 7060) * 100 / 1721;
        int t_int = temp_x10 / 10;
        int t_frac = (temp_x10 < 0 ? -temp_x10 : temp_x10) % 10;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            " Temperature    : %d.%d C\n", t_int, t_frac);
    }

    showTextDialog("Chip Info", buf);
}

void OSD::BoardInfo() {
    char (&buf)[OSD_INFO_BUF_SZ] = osd_info_buf;
    int pos = 0;

    // SD Card
    if (FileUtils::fsMount) {
        FATFS* fsp;
        DWORD fre_clust;
        if (f_getfree("", &fre_clust, &fsp) == FR_OK) {
            uint32_t tot_mb = (uint32_t)((fsp->n_fatent - 2) * fsp->csize / 2048);
            uint32_t fre_mb = (uint32_t)(fre_clust * fsp->csize / 2048);
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                " SD Card        : %d/%d MB free\n", (int)fre_mb, (int)tot_mb);

            const char* fs_name = "?";
            switch (fsp->fs_type) {
                case FS_FAT12: fs_name = "FAT12"; break;
                case FS_FAT16: fs_name = "FAT16"; break;
                case FS_FAT32: fs_name = "FAT32"; break;
                case FS_EXFAT: fs_name = "exFAT"; break;
            }
            uint32_t cluster_kb = (uint32_t)fsp->csize / 2;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "  FS / cluster  : %s / %u KB\n", fs_name, (unsigned)cluster_kb);

            char label[34] = {0};
            DWORD vsn = 0;
            if (f_getlabel("", label, &vsn) == FR_OK) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "  Label / VSN   : '%s' / %04lX-%04lX\n",
                    label[0] ? label : "(none)",
                    (unsigned long)((vsn >> 16) & 0xFFFF),
                    (unsigned long)(vsn & 0xFFFF));
            }
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " SD Card        : mounted\n");
        }

        // Card type via MMC_GET_TYPE (CT_SD1=0x02, CT_SD2=0x04, CT_MMC=0x01, CT_BLOCK=0x08)
        BYTE ct = 0;
        if (disk_ioctl(0, MMC_GET_TYPE, &ct) == RES_OK) {
            const char* ct_name = "?";
            if (ct & 0x01) ct_name = "MMCv3";
            else if (ct & 0x02) ct_name = "SDv1";
            else if (ct & 0x04) ct_name = (ct & 0x08) ? "SDHC/SDXC" : "SDv2";
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "  Card type     : %s\n", ct_name);
        }

        // CID: manufacturer + product name (5 ASCII) + serial
        BYTE cid[16];
        if (disk_ioctl(0, MMC_GET_CID, cid) == RES_OK) {
            char pname[6];
            for (int i = 0; i < 5; i++) {
                char c = (char)cid[3 + i];
                pname[i] = (c >= 0x20 && c < 0x7F) ? c : ' ';
            }
            pname[5] = 0;
            uint32_t serial = ((uint32_t)cid[9] << 24) | ((uint32_t)cid[10] << 16)
                            | ((uint32_t)cid[11] << 8) | (uint32_t)cid[12];
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "  CID name/sn   : '%s' / %08lX\n", pname, (unsigned long)serial);
        }
    } else {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " SD Card        : not mounted\n");
    }

    // Audio
    if (Config::audio_driver == 4)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " Audio          : HDMI\n");
    else if (Config::audio_driver == 3)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " Audio          : AY-3-8910\n");
    else
        pos += snprintf(buf + pos, sizeof(buf) - pos, " Audio          : %s [%02Xh]%s\n",
            (is_i2s_enabled ? "i2s" : "PWM"), link_i2s_code,
            (Config::audio_driver == 0 ? " auto" : ""));

#ifdef VGA_HDMI
    pos += snprintf(buf + pos, sizeof(buf) - pos, " VGA/HDMI detect: %02Xh\n", linkVGA01);
#endif

    // RAM pages
    if (!psram_pages)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " 16K RAM pages  : %d[s%d:b%d:v%d]\n",
            ram_pages + butter_pages + swap_pages, ram_pages, butter_pages, swap_pages);
    else if (!butter_pages)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " 16K RAM pages  : %d[s%d:p%d:v%d]\n",
            ram_pages + psram_pages + swap_pages, ram_pages, psram_pages, swap_pages);
    else if (!swap_pages)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " 16K RAM pages  : %d[s%d:b%d:p%d]\n",
            ram_pages + butter_pages + psram_pages, ram_pages, butter_pages, psram_pages);
    else
        pos += snprintf(buf + pos, sizeof(buf) - pos, " 16K RAM pages  : %d[s%d:b%d:p%d:v%d]\n",
            ram_pages + butter_pages + psram_pages + swap_pages, ram_pages, butter_pages, psram_pages, swap_pages);

    if (DivMMC::enabled) {
        const char* mode_names[] = { "OFF", "DivMMC", "DivIDE", "DivSD" };
        const char* mem_type = DivMMC::use_psram ? "PSRAM" : "swap";
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %-15s: 128K+8K [%s]\n",
            mode_names[Config::esxdos], mem_type);
    }

    // GPIO pins (all labels 16 chars after "  " prefix, colon at col 18)
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n GPIO pins:\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  Kbd CLK/DATA  : %d/%d\n", KBD_CLOCK_PIN, KBD_DATA_PIN);
#ifdef VGA_BASE_PIN
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  VGA base      : %d\n", VGA_BASE_PIN);
#endif
#ifdef HDMI_BASE_PIN
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  HDMI base     : %d\n", HDMI_BASE_PIN);
#endif
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  PWM L/R       : %d/%d\n", PWM_PIN0, PWM_PIN1);
#ifdef BEEPER_PIN
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  Beeper        : %d\n", BEEPER_PIN);
#endif
#if defined(I2S_DATA_PIO) && defined(I2S_BCK_PIO) && defined(I2S_LCK_PIO)
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  I2S D/BCK/LCK : %d/%d/%d\n", I2S_DATA_PIO, I2S_BCK_PIO, I2S_LCK_PIO);
#endif
#ifdef PCM5122_I2S_DATA
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  PCM5122 I2S   : %d/%d/%d\n", PCM5122_I2S_DATA, PCM5122_I2S_BCK, PCM5122_I2S_LCK);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  PCM5122 I2C   : %d/%d\n", PCM5122_I2C_SDA, PCM5122_I2C_SCL);
#endif
#ifdef LATCH_595_PIN
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  AY 595 L/C/D  : %d/%d/%d\n", LATCH_595_PIN, CLK_595_PIN, DATA_595_PIN);
#endif
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  SD MI/MO/CK/CS: %d/%d/%d/%d\n",
        SDCARD_PIN_SPI0_MISO, SDCARD_PIN_SPI0_MOSI, SDCARD_PIN_SPI0_SCK, SDCARD_PIN_SPI0_CS);
#ifdef PSRAM_PIN_CS
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  PSRAM CS/CK/IO: %d/%d/%d/%d\n",
        PSRAM_PIN_CS, PSRAM_PIN_SCK, PSRAM_PIN_MOSI, PSRAM_PIN_MISO);
#endif
#ifdef BUTTER_PSRAM_GPIO
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  Butter PSRAM  : %d\n", BUTTER_PSRAM_GPIO);
#endif
#ifdef MIDI_TX_PIN
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  MIDI TX       : %d\n", MIDI_TX_PIN);
#endif
#ifdef LOAD_WAV_PIO
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  LOAD WAV      : %d\n", LOAD_WAV_PIO);
#endif
#ifdef USE_NESPAD
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  NES CLK/LAT/D : %d/%d/%d\n", NES_GPIO_CLK, NES_GPIO_LAT, NES_GPIO_DATA);
#endif
#if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "  LED           : %d\n", PICO_DEFAULT_LED_PIN);
#endif

    // Build info
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\n %s\n"
        " Built %s %s\n"
        " branch '%s'\n"
        " commit [%s]\n",
        PICO_BUILD_NAME, __DATE__, __TIME__, PICO_GIT_BRANCH, PICO_GIT_COMMIT);

    showTextDialog("Board Info", buf);
}

// Memory Info — overall FLASH/SRAM/PSRAM occupancy plus the Buffer tier pools and the
// SRAM cost of the features currently enabled via the Subsystem budget manager. Data
// sources: linker symbols (firmware/static/heap extents), getFreeHeap/getLargestAllocatable,
// butter_psram_size/psram_size + page counters, Buffer::poolStat() and Subsystems::feature*.
void OSD::MemoryInfo() {
    extern char __flash_binary_start, __flash_binary_end;  // pico-sdk linker symbols
    extern char end, __HeapLimit;                          // heap arena [end, __HeapLimit)

    char (&buf)[OSD_INFO_BUF_SZ] = osd_info_buf;
    int pos = 0;
    const int KB = 1024;

    // ── SRAM ───────────────────────────────────────────────────────────────────
    size_t sram_total  = (size_t)((uintptr_t)&__HeapLimit - SRAM_BASE);  // up to stack top
    size_t sram_static = (size_t)((uintptr_t)&end - SRAM_BASE);          // data + bss
    size_t heap_total  = (size_t)((uintptr_t)&__HeapLimit - (uintptr_t)&end);
    size_t heap_free   = getFreeHeap();
    size_t heap_used   = heap_total > heap_free ? heap_total - heap_free : 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, " SRAM (%d KB usable):\n", (int)(sram_total / KB));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  Static bss+data: %d KB\n", (int)(sram_static / KB));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  Heap used/total: %d/%d KB\n", (int)(heap_used / KB), (int)(heap_total / KB));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  Heap free      : %d KB\n", (int)(heap_free / KB));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  Largest block  : %d KB\n", (int)(getLargestAllocatable() / KB));

    // ── FLASH ──────────────────────────────────────────────────────────────────
    size_t fw = (size_t)((uintptr_t)&__flash_binary_end - (uintptr_t)&__flash_binary_start);
    uint32_t flash_total = (1u << rx[3]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, " FLASH (%d MB):\n", (int)(flash_total >> 20));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  Firmware       : %d KB\n", (int)(fw / KB));
    Buffer::PoolStat fp = Buffer::poolStat(Buffer::TIER_FLASH);
    if (fp.total)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  Buffer pool    : %d/%d KB\n", (int)(fp.used / KB), (int)(fp.total / KB));

    // ── PSRAM ──────────────────────────────────────────────────────────────────
    // A chip that was found but is switched off (Debug > PSRAM) would otherwise just
    // be missing from this page, reading as a hardware fault. Say so instead.
    if (!Config::psram_enabled && (butter_psram_probed() || psram_probed_size()))
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        " PSRAM          : off (Debug menu)\n");
#ifdef BUTTER_PSRAM_GPIO
    if (butter_psram_size()) {
        uint32_t bsz = butter_psram_size();
        size_t emu = (size_t)butter_pages * MEM_PG_SZ;
        Buffer::PoolStat bp = Buffer::poolStat(Buffer::TIER_BUTTER);
        pos += snprintf(buf + pos, sizeof(buf) - pos, " Butter PSRAM (%d.%d MB):\n",
            (int)(bsz >> 20), (int)(((bsz & 0xFFFFF) * 10) >> 20));
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  Emu RAM pages  : %d KB\n", (int)(emu / KB));
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  Buffer arena   : %d/%d KB\n", (int)(bp.used / KB), (int)(bp.total / KB));
    }
#endif
    if (psram_size()) {
        uint32_t psz = psram_size();
        size_t emu = (size_t)psram_pages * MEM_PG_SZ;
        Buffer::PoolStat sp = Buffer::poolStat(Buffer::TIER_SPI);
        pos += snprintf(buf + pos, sizeof(buf) - pos, " SPI PSRAM (%d.%d MB):\n",
            (int)(psz >> 20), (int)(((psz & 0xFFFFF) * 10) >> 20));
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  Emu RAM pages  : %d KB\n", (int)(emu / KB));
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  Buffer arena   : %d/%d KB\n", (int)(sp.used / KB), (int)(sp.total / KB));
    }
    Buffer::PoolStat swp = Buffer::poolStat(Buffer::TIER_SWAP);
    if (swp.total)
        pos += snprintf(buf + pos, sizeof(buf) - pos, " SD swap pool   : %d/%d KB\n", (int)(swp.used / KB), (int)(swp.total / KB));

    // ── Enabled features (Subsystem SRAM budget) ────────────────────────────────
    using namespace Subsystems;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n Enabled features (SRAM):\n");
    size_t feat_total = 0;
    for (int i = 0; i < FEAT_COUNT; i++) {
        FeatureId f = (FeatureId)i;
        if (!featureEnabled(f)) continue;
        size_t c = featureCost(f);
        feat_total += c;
        // Round up so a sub-KB feature (e.g. 512 B Z-Controller) isn't shown as 0 KB;
        // a genuinely-zero cost (Gigascreen on butter, ULA+/Timex) stays 0.
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  %-14s : %d KB\n", featureName(f), (int)((c + KB - 1) / KB));
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  %-14s : %d KB\n", "TOTAL", (int)((feat_total + KB - 1) / KB));

    // ── PSRAM by feature ────────────────────────────────────────────────────────
    // The big tiered buffers (GM.DLS bank, GS sample RAM, prevFB, DivMMC banks) live
    // in PSRAM, not the heap — invisible in the SRAM list above. List the PSRAM users.
    size_t psram_feat_total = 0;
    int psram_feat_n = 0;
    for (int i = 0; i < FEAT_COUNT; i++) {
        FeatureId f = (FeatureId)i;
        if (!featureEnabled(f)) continue;
        size_t pc = featurePsramCost(f);
        if (!pc) continue;
        if (!psram_feat_n++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\n PSRAM by feature:\n");
        psram_feat_total += pc;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  %-14s : %d KB\n", featureName(f), (int)((pc + KB - 1) / KB));
    }
    if (psram_feat_n)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "  %-14s : %d KB\n", "TOTAL", (int)((psram_feat_total + KB - 1) / KB));

    showTextDialog("Memory Info", buf);
}

// snprintf-style append that returns what was ACTUALLY written, never the
// would-be length. The info builders accumulate `pos += ...` across dozens of
// calls; with raw snprintf a page that outgrows its buffer pushes `pos` past the
// end, and the next call then gets an out-of-bounds `buf + pos` together with an
// underflowed (huge) size_t size — a write past the buffer instead of the
// intended truncation. Clamping here makes overflow degrade into a cut-off page.
static int infoAppend(char* buf, int pos, int bufsize, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int infoAppend(char* buf, int pos, int bufsize, const char* fmt, ...) {
    if (pos < 0 || pos >= bufsize - 1) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, bufsize - pos, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    return (pos + n >= bufsize) ? bufsize - 1 - pos : n;
}

// Helper: append just the filename part of a path, truncated to maxlen chars
static int appendFilename(char* buf, int pos, int bufsize, const string& path, int maxlen) {
    if (path.empty()) return infoAppend(buf, pos, bufsize, "(none)");
    size_t slash = path.find_last_of("/");
    const char* fn = (slash != string::npos) ? path.c_str() + slash + 1 : path.c_str();
    int len = strlen(fn);
    if (len <= maxlen)
        return infoAppend(buf, pos, bufsize, "%s", fn);
    else
        return infoAppend(buf, pos, bufsize, "..%s", fn + len - (maxlen - 2));
}

// Emulator Info's text buffer: heap while the page is open, else the shared info
// buffer (the page then truncates at the tail — infoAppend keeps that safe).
// Allocated/freed by OSD::EmulatorInfo, so the emulator never carries the cost.
#define EMU_INFO_BUF_SZ 2560
static char* emu_buf    = nullptr;
static int   emu_buf_sz = 0;
static inline char* emuBuf()   { return emu_buf ? emu_buf : osd_info_buf; }
static inline int   emuBufSz() { return emu_buf ? emu_buf_sz : OSD_INFO_BUF_SZ; }

static void buildEmulatorInfoText() {
    char* buf = emuBuf();
    const int bufsz = emuBufSz();
    int pos = 0;

    // --- Machine ---
    pos += infoAppend(buf, pos, bufsz,
        " --- Machine ---\n"
        " Architecture   : %s\n"
        " ROM set        : %s\n"
        " Issue 2        : %s\n"
        " ALU Timing     : %s\n",
        // archDisplay: Config only ever holds A_PROFI, so a Karabas romset would
        // otherwise report "Profi" here while the Machine menu says "Karabas".
        archToStr(archDisplay(Config::arch, Config::romSet)),
        romsetDisplay(Config::romSet),
        Config::Issue2 ? "On" : "Off",
        Config::AluTiming == 0 ? "Early" : "Late");

    // Rows that only exist in some configurations — printed only when they do,
    // so the common case stays short.
    if (Config::throtling)
        pos += infoAppend(buf, pos, bufsz,
            " Frameskip      : %d us\n", Config::throtling * 1000);
    if (Z80Ops::isProfi && Config::profi_ext_keys)
        pos += infoAppend(buf, pos, bufsz, " XT keyboard    : On\n");
    if (Config::byte_cobmect_mode)
        pos += infoAppend(buf, pos, bufsz, " COBMECT mode   : On\n");
    if (MEM_PG_CNT != 64)   // Murmuzavr SD-swap: 64 pages = no swap
        pos += infoAppend(buf, pos, bufsz,
            " SD swap        : %u MB\n", (unsigned)(MEM_PG_CNT / 64));

    // RTC: the live reading plus the register format the guest picked. Both are
    // here because a wrong-looking clock is nearly always one of the two.
    if (!Config::rtc_enabled) {
        pos += infoAppend(buf, pos, bufsz, " RTC + NVRAM    : Off\n");
    } else {
        int ry, rmo, rd, rh, rmi, rs;
        if (RTC::now(ry, rmo, rd, rh, rmi, rs))
            pos += infoAppend(buf, pos, bufsz,
                " RTC + NVRAM    : On (%s)\n"
                "  time          : %02d.%02d.%04d %02d:%02d\n",
                RTC::formatStr(), rd, rmo, ry, rh, rmi);
        else
            pos += infoAppend(buf, pos, bufsz,
                " RTC + NVRAM    : On (not set)\n");
    }

    // --- Video ---
    {
        const char* vmname;
        uint8_t vm = VIDEO::activeVideoMode();
        switch (vm) {
            case Config::VM_640x480_60: vmname = "640x480@60"; break;
            case Config::VM_640x480_50: vmname = "640x480@50"; break;
            case Config::VM_720x480_60: vmname = "720x480@60"; break;
            case Config::VM_720x576_50: vmname = "720x576@50"; break;
            default:                    vmname = "unknown";    break;
        }
        pos += infoAppend(buf, pos, bufsz, "\n --- Video ---\n");
#ifdef VGA_HDMI
        extern bool SELECT_VGA;
        pos += infoAppend(buf, pos, bufsz,
            " Video output   : %s %s\n",
            SELECT_VGA ? "VGA" : "HDMI", vmname);
#else
        pos += infoAppend(buf, pos, bufsz,
            " Video mode     : %s\n", vmname);
#endif
        pos += infoAppend(buf, pos, bufsz,
            " Scanlines      : %s\n"
            " CRT filter     : %s\n"
            " Render         : %s\n"
            " Palette        : %s\n",
            Config::scanlines ? (Config::scanlines == 1 ? "Lvl 1" :
                                 Config::scanlines == 2 ? "Lvl 2" :
                                 Config::scanlines == 3 ? "Lvl 3" : "Lvl 4") : "Off",
            Config::crt_filter == 1 ? "Soft" :
            Config::crt_filter == 2 ? "Medium" :
            Config::crt_filter == 3 ? "Strong" :
            Config::crt_filter == 4 ? "Grille soft" :
            Config::crt_filter == 5 ? "Grille med" :
            Config::crt_filter == 6 ? "Grille hard" : "Off",
            Config::render ? "Snow effect" : "Standard",
            VIDEO::paletteName(Config::palette));
        {
            const char* gs;
            if (!Config::gigascreen_enabled || Config::gigascreen_onoff == 0) gs = "Off";
            else if (Config::gigascreen_onoff == 1) gs = "On";
            else gs = "Auto";
            pos += infoAppend(buf, pos, bufsz,
                " Gigascreen     : %s\n"
                " ULA+           : %s\n"
                " Timex video    : %s\n"
                " V-Sync         : %s\n",
                gs,
                Config::ulaplus ? "On" : "Off",
                Config::timex_video ? "On (#FF)" : "Off",
                Config::v_sync_enabled ? "On" : "Off");
        }
        // 16col is a Pentagon-only port (#EFF7 D0); dither only exists on HDMI.
        if (Z80Ops::isPentagon)
            pos += infoAppend(buf, pos, bufsz,
                " 16col mode     : %s\n",
                Config::mode16col_onoff ? "On (#EFF7)" : "Off");
#ifdef VGA_HDMI
        {
            extern bool SELECT_VGA;
            if (!SELECT_VGA)
                pos += infoAppend(buf, pos, bufsz,
                    " HDMI dither    : %s\n", Config::hdmi_dither ? "On" : "Off");
        }
#endif
    }

    // --- Sound ---
    {
        pos += infoAppend(buf, pos, bufsz, "\n --- Sound ---\n");

        // AY chip
        if (Config::AY48) {
            static const char* stereo[] = { "ABC", "ACB", "Mono" };
            int si = Config::ayConfig;
            if (si > 2) si = 0;
            pos += infoAppend(buf, pos, bufsz,
                " AY-3-8912      : On (%s) #FFFD\n", stereo[si]);
        } else {
            pos += infoAppend(buf, pos, bufsz,
                " AY-3-8912      : Off\n");
        }

        // TurboSound
        pos += infoAppend(buf, pos, bufsz,
            " TurboSound     : %s\n", Config::turbosound ? "On" : "Off");

        // Covox
        if (Config::covox == 1)
            pos += infoAppend(buf, pos, bufsz, " Covox          : On (#FB)\n");
        else if (Config::covox == 2)
            pos += infoAppend(buf, pos, bufsz, " Covox          : On (#DD)\n");
        else
            pos += infoAppend(buf, pos, bufsz, " Covox          : Off\n");

        // SounDrive
        pos += infoAppend(buf, pos, bufsz, " SounDrive      : %s\n",
            Config::soundrive == 2 ? (Config::soundriveEnabled() ? "Auto (On)" : "Auto (Off)")
                                   : (Config::soundrive == 1 ? "On" : "Off"));

        // SAA1099
        pos += infoAppend(buf, pos, bufsz,
            " SAA1099        : %s\n",
            Config::SAA1099 ? "On (#FF)" : "Off");

        // General Sound — configured vs actually running: GS::init needs PSRAM
        // for its sample RAM, so "enabled but not up" is a real state (same
        // distinction MB-02+ makes below).
        if (!Config::gs_enabled) {
            pos += infoAppend(buf, pos, bufsz, " General Sound  : Off\n");
        } else if (!GS::enabled) {
            pos += infoAppend(buf, pos, bufsz, " General Sound  : No PSRAM\n");
        } else {
            uint32_t kb = GS::configuredRamBytes() >> 10;
            char ram[8];
            if (kb >= 1024) snprintf(ram, sizeof(ram), "%uM", (unsigned)(kb >> 10));
            else            snprintf(ram, sizeof(ram), "%uK", (unsigned)kb);
            // Live clock, not the menu pick: NeoGS may be on Auto and the
            // firmware re-picks via GSCFG0 CKSEL while running.
            pos += infoAppend(buf, pos, bufsz,
                " %s : On %s @%u MHz%s\n",
                GS::neogs ? "NeoGS         " : "General Sound ",
                ram, (unsigned)(GS::clockHz() / 1000000u),
                (GS::neogs && Config::ngs_clock == 0) ? " (auto)" : "");
        }

        // MIDI
        if (Config::midi == 0) {
            pos += infoAppend(buf, pos, bufsz, " MIDI           : Off\n");
        } else if (Config::midi == 4) {
            // Where the bank actually landed matters (Instrument storage is a request,
            // not a guarantee: a full PSRAM arena falls back to the flash partition).
            pos += infoAppend(buf, pos, bufsz,
                " MIDI           : DLS (%s)\n",
                MidiSynth::bankReady() ? MidiSynth::bankLocation() : "no bank");
        } else {
            pos += infoAppend(buf, pos, bufsz,
                " MIDI           : %s\n",
                Config::midi == 1 ? "AY bitbang" : "ShamaZX");
        }

        // Audio driver
        if (Config::audio_driver == 4)
            pos += infoAppend(buf, pos, bufsz, " Audio driver   : HDMI\n");
        else if (Config::audio_driver == 3)
            pos += infoAppend(buf, pos, bufsz, " Audio driver   : AY-3-8910\n");
        else
            pos += infoAppend(buf, pos, bufsz, " Audio driver   : %s%s\n",
                (is_i2s_enabled ? "i2s" : "PWM"),
                (Config::audio_driver == 0 ? " (auto)" : ""));

        if (Config::audio_boost)
            pos += infoAppend(buf, pos, bufsz,
                " Volume boost   : +%d\n", Config::audio_boost);
    }

    // --- Input ---
    {
        static const char* jnames[] = {
            "Cursor", "Kempston", "Sinclair 1",
            "Sinclair 2", "Fuller", "Custom", "None"
        };
        int ji = Config::joystick;
        if (ji > 6) ji = 6;
        pos += infoAppend(buf, pos, bufsz,
            "\n --- Input ---\n");

        // Joystick with port number
        if (ji == JOY_KEMPSTON)
            pos += infoAppend(buf, pos, bufsz,
                " Joystick       : Kempston (#%02X)\n", Config::kempstonPort);
        else if (ji == JOY_FULLER)
            pos += infoAppend(buf, pos, bufsz,
                " Joystick       : Fuller (#7F)\n");
        else
            pos += infoAppend(buf, pos, bufsz,
                " Joystick       : %s\n", jnames[ji]);

        {
            static const char* sjnames[] = { "Off", "DPAD #1", "DPAD #2", "NUMPAD" };
            int sji = Config::secondJoy <= 3 ? Config::secondJoy : 0;
            pos += infoAppend(buf, pos, bufsz,
                " Second joystick: %s\n", sjnames[sji]);
        }

        pos += infoAppend(buf, pos, bufsz,
            " TAB as fire    : %s\n"
            " Cursor as joy  : %s\n"
            " Joy to cursor  : %s\n"
            " WASD as Kempst.: %s\n"
            " R.Enter as Spc : %s\n",
            Config::TABasfire1 ? "On" : "Off",
            Config::CursorAsJoy ? "On" : "Off",
            Config::joy2cursor ? "On" : "Off",
            Config::wasd ? "On" : "Off",
            Config::rightSpace ? "On" : "Off");
    }

    // --- Storage ---
    {
        pos += infoAppend(buf, pos, bufsz, "\n --- Storage ---\n");

        if (UsbMsc::ready())
            pos += infoAppend(buf, pos, bufsz, " USB drive      : %s\n",
                FileUtils::usbRoot ? "Ready (as root)" : "Ready");

        // esxDOS
        {
            static const char* esx[] = { "Off", "DivMMC", "DivIDE", "DivSD" };
            int ei = Config::esxdos;
            if (ei > 3) ei = 0;
            pos += infoAppend(buf, pos, bufsz,
                " esxDOS         : %s\n", esx[ei]);

            // Show image names depending on mode (hd0/hd1 are shared slots).
            if (ei == 1) {
                // DivMMC — shows hd0 only.
                pos += infoAppend(buf, pos, bufsz, "  hd0           : ");
                pos += appendFilename(buf, pos, bufsz, Config::esxdos_hdf_image[0], 19);
                pos += infoAppend(buf, pos, bufsz, "\n");
            } else if (ei == 2) {
                // DivIDE — both slots.
                pos += infoAppend(buf, pos, bufsz, "  hd0           : ");
                pos += appendFilename(buf, pos, bufsz, Config::esxdos_hdf_image[0], 19);
                pos += infoAppend(buf, pos, bufsz, "\n");
                pos += infoAppend(buf, pos, bufsz, "  hd1           : ");
                pos += appendFilename(buf, pos, bufsz, Config::esxdos_hdf_image[1], 19);
                pos += infoAppend(buf, pos, bufsz, "\n");
            }
        }

        // Z-Controller (mutually exclusive with esxDOS/MB-02+, hence its own row)
        if (Config::zcontroller)
            pos += infoAppend(buf, pos, bufsz,
                " Z-Controller   : On (#77/#57)\n");

        // IDE/HDD (NEMO/PROFI)
        if (Config::ide_scheme != 0) {
            static const char* idesc[] = { "Off", "NEMO", "PROFI" };
            int si = Config::ide_scheme; if (si > 2) si = 0;
            pos += infoAppend(buf, pos, bufsz, " IDE/HDD        : %s\n", idesc[si]);
            pos += infoAppend(buf, pos, bufsz, "  hd0           : ");
            pos += appendFilename(buf, pos, bufsz, Config::ide_image[0], 19);
            pos += infoAppend(buf, pos, bufsz, "\n");
            pos += infoAppend(buf, pos, bufsz, "  hd1           : ");
            pos += appendFilename(buf, pos, bufsz, Config::ide_image[1], 19);
            pos += infoAppend(buf, pos, bufsz, "\n");
        }

        // TR-DOS — available for Pentagon or Byte 128K
        {
            bool trdos_available = (Z80Ops::isPentagon || Z80Ops::isProfi) || (Z80Ops::is128 && Z80Ops::isByte);
            if (trdos_available) {
                pos += infoAppend(buf, pos, bufsz, " TR-DOS         : On");
                if (Config::trdosFastMode) {
                    pos += infoAppend(buf, pos, bufsz, " (fast)");
                }
                pos += infoAppend(buf, pos, bufsz, "\n");

                {
                    static const char* trbios[] = { "5.03", "5.04TM", "5.05D", "Custom" };
                    pos += infoAppend(buf, pos, bufsz,
                        "  ROM / autoboot: %s / %s\n",
                        trbios[Config::trdosBios < 4 ? Config::trdosBios : 2],
                        Config::trdosAutoBoot ? "On" : "Off");
                }

                // Show disk drives A:-D:
                static const char drive_letter[] = "ABCD";
                for (int i = 0; i < 4; i++) {
                    pos += infoAppend(buf, pos, bufsz, "  %c:            : ", drive_letter[i]);
                    if (ESPectrum::fdd.disk[i] && !ESPectrum::fdd.disk[i]->fname.empty()) {
                        pos += appendFilename(buf, pos, bufsz, ESPectrum::fdd.disk[i]->fname, 19);
                        if (Config::driveWP[i])
                            pos += infoAppend(buf, pos, bufsz, " WP");
                    } else
                        pos += infoAppend(buf, pos, bufsz, "(empty)");
                    pos += infoAppend(buf, pos, bufsz, "\n");
                }
            } else {
                pos += infoAppend(buf, pos, bufsz, " TR-DOS         : Off\n");
            }
        }

        // MB-02+
        if (MB02::enabled) {
            pos += infoAppend(buf, pos, bufsz, " MB-02+         : On\n");
            for (int i = 0; i < 4; i++) {
                pos += infoAppend(buf, pos, bufsz, "  %02d            : ", i + 1);
                if (ESPectrum::mb02_fdd.disk[i] && !ESPectrum::mb02_fdd.disk[i]->fname.empty()) {
                    pos += appendFilename(buf, pos, bufsz, ESPectrum::mb02_fdd.disk[i]->fname, 19);
                    if (Config::mb02WP[i])
                        pos += infoAppend(buf, pos, bufsz, " WP");
                } else
                    pos += infoAppend(buf, pos, bufsz, "(empty)");
                pos += infoAppend(buf, pos, bufsz, "\n");
            }
        } else if (Config::mb02) {
            pos += infoAppend(buf, pos, bufsz, " MB-02+         : No PSRAM\n");
        }

        // DMA
        if (Config::dma_mode == 1)
            pos += infoAppend(buf, pos, bufsz, " DMA            : Z80 DMA (#0B)\n");
        else if (Config::dma_mode == 2)
            pos += infoAppend(buf, pos, bufsz, " DMA            : zxnDMA (#6B)\n");
        else
            pos += infoAppend(buf, pos, bufsz, " DMA            : Off\n");

        // ALF cartridge (ALF TV GAME): built-in Elf-1 unless a cart was flashed.
        if (Z80Ops::isALF) {
            pos += infoAppend(buf, pos, bufsz, " ALF cartridge  : ");
            if (Config::alfCartBanks) {
                pos += appendFilename(buf, pos, bufsz, Config::alfCartPath, 14);
                pos += infoAppend(buf, pos, bufsz, " %dx16K\n",
                    Config::alfCartBanks);
            } else
                pos += infoAppend(buf, pos, bufsz, "built-in Elf-1\n");
        }

        // Loaded snapshot (the .sna/.z80 that Config remembers across reboots)
        if (Config::ram_file != NO_RAM_FILE) {
            pos += infoAppend(buf, pos, bufsz, " Snapshot       : ");
            pos += appendFilename(buf, pos, bufsz, Config::ram_file, 19);
            pos += infoAppend(buf, pos, bufsz, "\n");
        }

        // Tape
        pos += infoAppend(buf, pos, bufsz, " Tape           : ");
        pos += appendFilename(buf, pos, bufsz, Tape::tapeFileName, 19);
        pos += infoAppend(buf, pos, bufsz, "\n");

        pos += infoAppend(buf, pos, bufsz,
            " Flash load     : %s\n",
            Config::flashload ? "On" : "Off");

        // Tape options as one row — four booleans each on their own line would
        // dominate the section.
        {
            char opts[40]; int op = 0;
            if (Config::tape_player)    op += snprintf(opts + op, sizeof(opts) - op, "Player, ");
            if (Config::tape_timing_rg) op += snprintf(opts + op, sizeof(opts) - op, "R.G., ");
            if (Config::tape_autostart) op += snprintf(opts + op, sizeof(opts) - op, "Auto, ");
            if (op >= 2) opts[op - 2] = '\0';    // drop the trailing ", "
            else         snprintf(opts, sizeof(opts), "Default");
            pos += infoAppend(buf, pos, bufsz, " Tape options   : %s\n", opts);
        }
    }

    // --- Network ---
    {
        pos += infoAppend(buf, pos, bufsz, "\n --- Network ---\n");

        if (!Config::zifi_enabled) {
            pos += infoAppend(buf, pos, bufsz, " ZiFi NIC       : Off\n");
        } else if (Config::zifi_transport == 1) {
            pos += infoAppend(buf, pos, bufsz, " ZiFi NIC       : On (USB-CDC)\n");
        } else {
            // Pins are resolved at runtime from the board's UART pinmux, so show
            // what the NIC actually claimed rather than the stored sentinel.
            uint8_t tx = BoardPins::PIN_OFF, rx = BoardPins::PIN_OFF;
            if (BoardPins::resolveZifiPins(Config::zifi_tx_pin, Config::zifi_rx_pin, tx, rx))
                pos += infoAppend(buf, pos, bufsz,
                    " ZiFi NIC       : On (UART %d/%d)\n", tx, rx);
            else
                pos += infoAppend(buf, pos, bufsz,
                    " ZiFi NIC       : On (FIFO only)\n");
        }
        if (Config::zifi_enabled)
            pos += infoAppend(buf, pos, bufsz,
                " Baud           : %u\n", (unsigned)Config::zifi_baud);

        // Cached status only — ZiFiAT::getStatus() blocks on the ESP.
        if (!Config::wifi_enabled)
            pos += infoAppend(buf, pos, bufsz, " WiFi           : Off\n");
        else if (ZiFiAT::connected)
            // An SSID is not a path — clip it, don't run it through appendFilename
            // (which would keep only the part after a '/').
            pos += infoAppend(buf, pos, bufsz,
                " WiFi           : %.19s\n"
                "  IP            : %s\n",
                ZiFiAT::current_ssid.c_str(), ZiFiAT::current_ip.c_str());
        else
            pos += infoAppend(buf, pos, bufsz,
                " WiFi           : On (not connected)\n");
    }

}

// Snapshot for the new UI's live page (Help > Emulator info). Live rather than a
// one-shot snapshot because the page now carries state that moves while it is
// open — the RTC clock above all: a frozen reading reads as a stopped clock.
const char* emuInfoText() {
    buildEmulatorInfoText();
    return emuBuf();
}

void OSD::EmulatorInfo() {
    // Borrow the roomy buffer for as long as the page is on screen (the live page
    // rebuilds into it every second, so it must outlive the call below). Keep a
    // working reserve so opening an info page can never be the allocation that
    // starves the heap; on refusal the page just truncates.
    if (getLargestAllocatable() >= EMU_INFO_BUF_SZ + 8 * 1024) {
        emu_buf = (char*)malloc(EMU_INFO_BUF_SZ);
        emu_buf_sz = emu_buf ? EMU_INFO_BUF_SZ : 0;
    }

    nm::uiTextPageLive(TXT_INFO_EMU, emuInfoText, 1000);

    free(emu_buf);
    emu_buf = nullptr;
    emu_buf_sz = 0;
}

extern "C" int hid_app_format_devices_info(char* buf, int bufsz);
extern "C" int xinput_app_format_devices_info(char* buf, int bufsz);

// Snapshot for the new UI's live page (Help > HID devices).
const char* hidInfoText() {
    extern int hid_app_format_devices_info(char* buf, int bufsz);
    extern int xinput_app_format_devices_info(char* buf, int bufsz);
    char (&buf)[OSD_INFO_BUF_SZ] = osd_info_buf;
    buf[0] = '\0';
    int xpos = xinput_app_format_devices_info(buf, sizeof(buf));
    if (xpos < 0) xpos = 0;
    buf[xpos] = '\0';
    int hpos = hid_app_format_devices_info(buf + xpos, sizeof(buf) - xpos);
    if (hpos < 0) hpos = 0;
    buf[xpos + hpos] = '\0';
    return buf;
}

void OSD::HIDDevices() {
    nm::uiTextPageLive(TXT_INFO_HID, hidInfoText, 1000);
}

extern "C" uint8_t __gm_bank_start[];   // shared flash region (RP2350) — ALF cart load target

static void __not_in_flash_func(flash_block)(const uint8_t* buffer, size_t flash_target_offset) {
    // ensure it is required to write block (may be, it is already the same)
    for (size_t i = 0; i < 512; ++i) {
        if (buffer[i] != *(uint8_t*)(XIP_BASE + flash_target_offset + i)) {
            goto flash_it;
        }
    }
    return;
flash_it:
    #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    gpio_put(PICO_DEFAULT_LED_PIN, flash_target_offset % (FLASH_SECTOR_SIZE << 2) == 0);
    #endif
    multicore_lockout_start_blocking();
    const uint32_t ints = save_and_disable_interrupts();
    if (flash_target_offset % FLASH_SECTOR_SIZE == 0) { // cleanup_block
        flash_range_erase(flash_target_offset, FLASH_SECTOR_SIZE);
    }
    flash_range_program(flash_target_offset, buffer, 512);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    #if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    gpio_put(PICO_DEFAULT_LED_PIN, false);
    #endif
}

bool OSD::loadAlfCart(const string& fname) {
    FIL* f = fopen2(fname.c_str(), FA_READ);
    if (!f) { OSD::osdCenteredMsg(OSD_NOROMFILE_ERR, LEVEL_WARN, 2000); return false; }
    size_t size = (size_t)f_size(f);
    fclose2(f);
    if (size == 0) {
        OSD::osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
        return false;
    }
    // Some distributions append a small text footer (e.g. "filename=.../size=.../crc32=...")
    // after the 1MB cart image. Clamp to 1MB; only the cart data is served, the tail ignored.
    if (size > (1ul << 20)) size = (1ul << 20);
    // Lazy load: the cart is served from SD on demand (like a wd1793 disk) — NO 1MB
    // flash write; banks fault in via #5F as the guest pages them (see AlfCart / Ports).
    // Switch into ALF IN PLACE (no reboot), the same way the Hardware menu and snapshot
    // loads switch machines: mount the cart, requestMachine + reset, then return so the
    // OSD closes cleanly into the running machine. GM.DLS no longer conflicts (the cart
    // does not use the shared flash region), so it is left untouched.
    Config::alfCartBanks = (uint8_t)((size + (16ul << 10) - 1) >> 14);   // ceil to 16K banks
    Config::alfCartPath  = fname;
    Config::arch = A_ALF; Config::romSet = R_ALF1; Config::pref_arch = A_ALF;
    Config::save();
    // Mount FRESH every load — open a new FIL on the file as it is NOW. ZIP carts always
    // extract to the same temp path (/tmp/.zip_extract.rom), so reloading would otherwise
    // keep a stale handle open on a file that was unlinked+rewritten underneath it
    // (progressively empty catalog until a reboot). mount() closes any previous handle.
    if (!AlfCart::mount(fname)) {
        OSD::osdCenteredMsg("ALF cart mount failed", LEVEL_WARN, 2000);
        return false;
    }
    Config::alfCartBanks = (uint8_t)AlfCart::bankCount();
    // ALF uses neither General Sound nor Gigascreen — free their SRAM so the cart +
    // machine have headroom. Gigascreen frees live (prevFB via GsSubsys); General
    // Sound only frees across a reboot (no live deinit), so if it was on we reboot
    // into ALF clean (the cart path is persisted → boot re-mounts it).
    bool needGsReboot = (Config::gs_enabled != 0);
    Config::gs_enabled = 0;
    Config::gigascreen_enabled = false;
    Config::gigascreen_onoff   = 0;
    GsSubsys::request(false);     // release prevFB (applied at reset/reboot below)
    Config::save();
    // Reboot when GS was on (to actually reclaim it) OR when leaving Profi's forced-
    // SRAM layout (no butter PSRAM) — an in-place reset() frees neither the ~96 KB
    // Profi pages nor GS, and ALF would OOM. Boot re-mounts the cart from SD.
    if (needGsReboot ||
        (butter_psram_size() == 0 &&
         MemESP::ram[56].memType() == mem_type_t::POINTER)) {
        OSD::esp_hard_reset();   // never returns
        return true;
    }
    Config::requestMachine(A_ALF, R_ALF1);   // in-place machine switch (no reboot)
    ESPectrum::reset();
    return true;
}

bool OSD::updateROM(const string& fname, uint8_t arch) {
    FIL* f = fopen2(fname.c_str(), FA_READ);
    if (!f) {
        osdCenteredMsg(OSD_NOROMFILE_ERR, LEVEL_WARN, 2000);
        return false;
    }
    FSIZE_t bytesfirmware = f_size(f);
    const uint8_t* rom;
    FSIZE_t max_rom_size = 0;
    string dlgTitle = OSD_ROM;
    // Flash custom ROM 48K
    if ( arch == 1 ) {
#if !CARTRIDGE_AS_CUSTOM
        if( bytesfirmware > 0x4000 ) {
            osdCenteredMsg("Too long file", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
#if NO_SEPARATE_48K_CUSTOM
        rom = gb_rom_0_128k_custom;
#else
        rom = gb_rom_0_48k_custom;
#endif
        max_rom_size = 16 << 10;
#else
        if( bytesfirmware > (1ul << 20) ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
        rom = gb_rom_Alf_cart;
        max_rom_size = 1ul << 20;
#endif
        dlgTitle += " 48K   ";
        Config::arch = A_48K;
        Config::romSet = R_48K_CS;
        Config::romSet48 = R_48K_CS;
        Config::pref_arch = A_48K;
        Config::pref_romSet_48 = R_48K_CS;
    }
    // Flash custom ROM 128K
    else if ( arch == 2 ) {
#if !CARTRIDGE_AS_CUSTOM
        if( bytesfirmware > 0x8000 ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
        rom = gb_rom_0_128k_custom;
        if (bytesfirmware <= (16 << 10)) {
            max_rom_size = 16 << 10;
        } else {
            max_rom_size = 32 << 10;
        }
#else
        if( bytesfirmware > (1ul << 20) ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
        rom = gb_rom_Alf_cart;
        if (bytesfirmware <= (16 << 10)) {
            max_rom_size = 16 << 10;
        } else if (bytesfirmware <= (32 << 10)) {
            max_rom_size = 32 << 10;
        } else {
            max_rom_size = 1ul << 20;
        }
#endif
        dlgTitle += " 128K  ";
        Config::arch = A_128K;
        Config::romSet = R_128K_CS;
        Config::romSet128 = R_128K_CS;
        Config::pref_arch = A_128K;
        Config::pref_romSet_128 = R_128K_CS;
    }
    else if ( arch == 3 ) {
#if !CARTRIDGE_AS_CUSTOM
        if( bytesfirmware > 0x8000 ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
        rom = gb_rom_0_128k_custom;
        if (bytesfirmware <= (16 << 10)) {
            max_rom_size = 16 << 10;
        } else {
            max_rom_size = 32 << 10;
        }
#else
        if( bytesfirmware > (1ul << 20) ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
        rom = gb_rom_Alf_cart;
        if (bytesfirmware <= (16 << 10)) {
            max_rom_size = 16 << 10;
        } else if (bytesfirmware <= (32 << 10)) {
            max_rom_size = 32 << 10;
        } else {
            max_rom_size = 1ul << 20;
        }
#endif
        dlgTitle += " Pentagon ";
        Config::arch = A_PENT;
        Config::romSet = R_128K_CS;
        Config::romSetPent = R_128K_CS;
        Config::pref_arch = A_PENT;
        Config::pref_romSetPent = R_128K_CS;
    }
    else if ( arch == 4 ) {
        if( bytesfirmware > (256ul << 10) ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
        rom = gb_rom_Alf;
        max_rom_size = 256ul << 10;
        dlgTitle += " ALF ROM ";
        Config::arch = A_ALF;
        Config::romSet = R_ALF1;
        Config::pref_arch = A_ALF;
    }
    else if ( arch == 5 ) {
        // Load an ALF cartridge (up to 1MB) served lazily from SD on demand (like a
        // wd1793 disk — see AlfCart). No 1MB flash write; the file stays on SD and 16K
        // banks fault in as the guest pages them. Switches into ALF in place (no reboot).
        fclose2(f);
        return loadAlfCart(fname);   // lazy-mount from SD + switch into ALF in place
    }
    else if ( arch == 6 ) {
        if( bytesfirmware > (16ul << 10) ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
        // Always flash into the dedicated custom slot and select it, so the
        // factory 5.03/5.04TM/5.05D images are never overwritten and the new
        // ROM is active after reboot (mirrors 48K -> "48Kcs").
        rom = gb_rom_4_trdos_custom;
        Config::trdosBios = 3;
        max_rom_size = 16ul << 10;
        dlgTitle += " TRDOS ";
        // TR-DOS ROM (rom[4]) is independent of the active machine and now
        // works with any of them, so we must NOT force Config::arch here —
        // doing so would yank the user out of their current machine into
        // Pentagon after the post-flash reboot. The custom slot is re-bound
        // unconditionally by Config::requestMachine() regardless of arch.
    }
    else if ( arch == 7 ) {
        if( bytesfirmware > (32ul << 10) ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
        // Custom Pentagon ROM: the factory Pentagon is now a 101-byte overlay over the
        // Sinclair 128K base (no 32K blob), so a user ROM flashes into the shared 128K
        // custom slot and the machine switches to the "128Kcs" romset.
#if !CARTRIDGE_AS_CUSTOM
        rom = gb_rom_0_128k_custom;
#else
        rom = gb_rom_Alf_cart;
#endif
        max_rom_size = bytesfirmware > (16ul << 10) ? (32ul << 10) : (16ul << 10);
        dlgTitle += " Pentagon#0 ";
        Config::arch = A_PENT;
        Config::romSet = R_128K_CS;
        Config::romSetPent = R_128K_CS;
        Config::pref_arch = A_PENT;
        Config::pref_romSetPent = R_128K_CS;
    }
    else if ( arch == 8 ) {
        if( bytesfirmware > (16 << 10) ) {
            osdCenteredMsg("Unsupported file (by size)", LEVEL_WARN, 2000);
            fclose2(f);
            return false;
        }
#if !CARTRIDGE_AS_CUSTOM
        rom = gb_rom_0_128k_custom;
#else
        rom = gb_rom_Alf_cart;
#endif
        max_rom_size = 16 << 10;
        dlgTitle += " Pentagon#1 ";
        Config::arch = A_PENT;
        Config::romSet = R_128K_CS;
        Config::romSetPent = R_128K_CS;
        Config::pref_arch = A_PENT;
        Config::pref_romSetPent = R_128K_CS;
    }
    else {
        osdCenteredMsg("Unexpected ROM type: " + to_string(arch), LEVEL_WARN, 2000);
        fclose2(f);
        return false;
    }
    size_t flash_target_offset = (size_t)rom - XIP_BASE;
    UINT br;
    const size_t sz = 512;
    uint8_t* buffer = (uint8_t*)malloc(sz);
    FSIZE_t i = 0;
    for (; i < bytesfirmware && i < max_rom_size; i += sz) {
        memset(buffer, 0, sz);
        if ( f_read(f, buffer, sz, &br) != FR_OK) {
            osdCenteredMsg(fname + " - unable to read", LEVEL_ERROR, 5000);
            free(buffer);
            fclose2(f);
            return false;
        }
        flash_block(buffer, flash_target_offset + (size_t)(i & 0xFFFFFFFF));
    }
    fclose2(f);
    memset(buffer, 0, sz);
    for (; i < max_rom_size; i += sz) {
        flash_block(buffer, flash_target_offset + (size_t)(i & 0xFFFFFFFF));
    }
    free(buffer);
    Config::save();
///    Config::requestMachine(Config::arch, Config::romSet);
    // Firmware written: reboot
///    ESPectrum::reset();
    OSD::esp_hard_reset();
    return true;
}


// ---------------------------------------------------------------------------
// Speed Test
// ---------------------------------------------------------------------------

// Sequential file R/W benchmark on one FatFs volume — measures the same
// FatFs+diskio path the emulator's own file I/O uses. benchPath selects the
// volume ("/bench.tmp" = SD, "USB:/..." = stick).
//
// NOTE on USB: block size does NOT change USB throughput. The RP2350 native
// USB host transfers bulk data at ~1 packet (64 B) per 1 ms SOF frame, so USB
// MSC is hard-capped near ~64 KB/s regardless of transfer size — measured a
// single 32 KB (64-sector) read10 at 558 ms, identical to 64 single-sector
// reads. It's a host-controller limit, not this code. See CLAUDE.md.
static bool benchFsSpeed(const char* benchPath, const char* volName,
                         const char* title, float& rd, float& wr) {
    static uint8_t io_buf[512];
    static FIL f;    // 2KB core stack — never put a FIL on it
    UINT bw, br;
    bool ok = false;
    char msg[24];

    snprintf(msg, sizeof(msg), "%s write...", volName);
    OSD::progressDialog(title, msg, 0, 0);
    memset(io_buf, 0x55, sizeof(io_buf));
    if (f_open(&f, benchPath, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        uint64_t t0 = time_us_64();
        uint32_t total = 0;
        while (time_us_64() - t0 < 2000000ULL && total < 512u * 1024u) {
            if (f_write(&f, io_buf, sizeof(io_buf), &bw) != FR_OK) break;
            total += bw;
        }
        uint64_t elapsed = time_us_64() - t0;
        f_close(&f);
        if (elapsed > 0 && total > 0) {
            wr = (float)total / (float)elapsed;
            ok = true;
        }
    }

    snprintf(msg, sizeof(msg), "%s read...", volName);
    OSD::progressDialog(title, msg, 50, 1);
    if (f_open(&f, benchPath, FA_READ) == FR_OK) {
        uint64_t t0 = time_us_64();
        uint32_t total = 0;
        while (f_read(&f, io_buf, sizeof(io_buf), &br) == FR_OK && br > 0)
            total += br;
        uint64_t elapsed = time_us_64() - t0;
        f_close(&f);
        if (elapsed > 0 && total > 0)
            rd = (float)total / (float)elapsed;
    }
    f_unlink(benchPath);

    OSD::progressDialog(title, "", 100, 1);
    OSD::progressDialog("", "", 0, 2);
    return ok;
}

#if ZIFI_NET_CLIENT
// NET download benchmark — GET the catalog's speedtest blob (512 KB of
// incompressible bytes published by gen_static.py) and count body bytes.
// Always the built-in Pages URL: a Config::catalog_host override may point at
// a dynamic /v1 proxy that doesn't serve the blob, and the point is to measure
// the device's real HTTPS-download path, not a LAN server.
// TLS handshake time and body throughput are reported separately — on a
// ~100 KB/s link the multi-second mbedTLS handshake would swamp the KB/s
// figure otherwise. Nothing is written to SD. ZiFiSock::sock_open already
// boostBaud()s the link, same as FTP/archive sessions.
#define SPEEDTEST_NET_URL "https://drewpo28.github.io/pico-spec-catalog/speedtest.bin"

struct NetBenchCtx {
    uint64_t t_start;      // just before HttpsGet::get
    uint64_t t_first;      // first body byte
    uint64_t t_end;        // last body byte
    uint64_t ui_last;      // progress-redraw rate limit
    uint32_t bytes;
    HttpsGet::Result r;
};

// Body cap: stop after this much measured time — a slow link (115200 baud)
// would otherwise take ~45 s for the full 512 KB. Aborting via the sink is
// fine: speed is computed from what arrived.
static const uint64_t NET_BENCH_BODY_US = 8 * 1000000ULL;

static bool netBenchSink(void* ctx, const uint8_t* data, size_t len) {
    (void)data;
    NetBenchCtx* c = (NetBenchCtx*)ctx;
    uint64_t now = time_us_64();
    if (!c->bytes) c->t_first = now;
    c->bytes += len;
    c->t_end = now;
    return now - c->t_first < NET_BENCH_BODY_US;
}

// Progress by bytes, rate-limited: the redraw runs between recv chunks and
// would otherwise eat into the measured throughput.
static bool netBenchProgress(void* ctx, uint32_t done, uint32_t total) {
    NetBenchCtx* c = (NetBenchCtx*)ctx;
    uint64_t now = time_us_64();
    if (total > 0 && now - c->ui_last > 250000ULL) {
        c->ui_last = now;
        OSD::progressDialog("", "", (int)((uint64_t)done * 100 / total), 1);
    }
    return true;
}

static void netBenchRun(void* p) {
    NetBenchCtx* c = (NetBenchCtx*)p;
    c->t_start = time_us_64();
    c->r = HttpsGet::get(SPEEDTEST_NET_URL, netBenchSink, c,
                         CONFIG_DIR "/cacert.pem", netBenchProgress, c);
    if (!c->t_end) c->t_end = time_us_64();
}

// Runs the GET on the big heap alt-stack (mbedTLS handshake doesn't fit the
// core stack). Returns false when no alt-stack could be allocated; success of
// the transfer itself is judged from ctx (2xx + bytes received — an abort by
// the time cap clears r.ok, but the sample is still valid).
static bool benchNetSpeed(NetBenchCtx& ctx, const char* title) {
    memset(&ctx, 0, sizeof(ctx));
    OSD::progressDialog(title, "NET download...", 0, 0);
    NetArenaLease lease;   // borrow the dormant Gigascreen prevFB if available
    size_t stksz = 12 * 1024;
    uint8_t* stk = netAltStackAlloc(stksz);
    if (!stk) {
        OSD::progressDialog("", "", 0, 2);
        return false;
    }
    void* top = (void*)(((uintptr_t)stk + stksz) & ~(uintptr_t)7);
    net_call_on_stack(top, netBenchRun, &ctx);
    Buffer::pfree(stk);
    OSD::progressDialog(title, "", 100, 1);
    OSD::progressDialog("", "", 0, 2);
    Debug::log("SpeedTest NET: status=%d bytes=%lu hs_us=%lu body_us=%lu",
               ctx.r.status, (unsigned long)ctx.bytes,
               (unsigned long)(ctx.bytes ? ctx.t_first - ctx.t_start : 0),
               (unsigned long)(ctx.bytes ? ctx.t_end - ctx.t_first : 0));
    return true;
}
#endif // ZIFI_NET_CLIENT

// One benchmark run for a picked row (1=CPU 2=SRAM 3=PSRAM 4=SD 5=USB [6=NET]
// 6/7=All) — one row of the menu's Speed test submenu.
void OSD::SpeedTestRun(uint8_t st_opt) {
    {
        // With the net client built in, row 6 is NET and "All tests" shifts to 7.
#if ZIFI_NET_CLIENT
        const uint8_t all_opt = 7;
        const bool do_net   = (st_opt == 6 || st_opt == all_opt);
#else
        const uint8_t all_opt = 6;
#endif
        const bool do_cpu   = (st_opt == 1 || st_opt == all_opt);
        const bool do_sram  = (st_opt == 2 || st_opt == all_opt);
        const bool do_psram = (st_opt == 3 || st_opt == all_opt);
        const bool do_sd    = (st_opt == 4 || st_opt == all_opt);
        const bool do_usb   = (st_opt == 5 || st_opt == all_opt);

        const char* title = "Speed Test";

        float cpu_mips = 0.0f;
        float sram_rd = 0.0f, sram_wr = 0.0f;
        float spi_rd = 0.0f, spi_wr = 0.0f;
        float qspi_rd = 0.0f, qspi_wr = 0.0f;
        float sd_rd = 0.0f, sd_wr = 0.0f;
        float usb_rd = 0.0f, usb_wr = 0.0f;
        bool sd_ok = false, usb_ok = false;
        // usbRoot: there is no SD — the unprefixed volume IS the stick, so the
        // SD test would silently benchmark USB; report "No card" instead.
        const bool sd_present  = FileUtils::fsMount && !FileUtils::usbRoot;
        const bool usb_present = UsbMsc::ready();

        const bool has_spi  = psram_size() > 0;
        const bool has_qspi = butter_psram_size() > 0;

        // --- CPU MIPS ---
        if (do_cpu) {
            progressDialog(title, "CPU MIPS...", 0, 0);
            const int BATCH = 10000;
            uint32_t acc = 1;
            uint32_t iters = 0;
            uint64_t t0 = time_us_64();
            do {
                for (int i = 0; i < BATCH; i++)
                    acc = acc * 1664525u + 1013904223u;
                iters += BATCH;
            } while (time_us_64() - t0 < 500000ULL);
            volatile uint32_t _sink = acc; (void)_sink;
            uint64_t elapsed = time_us_64() - t0;
            // 2 integer ops per iteration (mul + add)
            cpu_mips = (float)iters * 2.0f / (float)elapsed;
            progressDialog(title, "", 100, 1);
            progressDialog("", "", 0, 2);
        }

        // --- SRAM R/W ---
        // osd_info_buf used as scratch; results saved in floats before we refill it
        if (do_sram) {
            char (&scratch)[OSD_INFO_BUF_SZ] = osd_info_buf;
            uint64_t t0, elapsed;
            uint32_t total;

            typedef uint32_t __attribute__((may_alias)) u32a;

            progressDialog(title, "SRAM write...", 0, 0);
            total = 0;
            t0 = time_us_64();
            do {
                char *p = scratch;
                for (uint32_t a = 0; a < OSD_INFO_BUF_SZ; a += 4)
                    *(u32a *)(p + a) = a;
                total += OSD_INFO_BUF_SZ;
            } while (time_us_64() - t0 < 300000ULL);
            elapsed = time_us_64() - t0;
            sram_wr = (float)total / (float)elapsed;

            progressDialog(title, "SRAM read...", 50, 1);
            uint32_t acc = 0;
            total = 0;
            t0 = time_us_64();
            do {
                const char *p = scratch;
                for (uint32_t a = 0; a < OSD_INFO_BUF_SZ; a += 4)
                    acc ^= *(const u32a *)(p + a);
                total += OSD_INFO_BUF_SZ;
            } while (time_us_64() - t0 < 300000ULL);
            volatile uint32_t _sink = acc; (void)_sink;
            elapsed = time_us_64() - t0;
            sram_rd = (float)total / (float)elapsed;

            progressDialog(title, "", 100, 1);
            progressDialog("", "", 0, 2);
        }

        // --- PSRAM ---
        if (do_psram) {
            // SPI PSRAM — burst via psram_write_range / psram_read_range
            if (has_spi) {
                char (&burst)[OSD_INFO_BUF_SZ] = osd_info_buf;
                uint64_t t0, elapsed;
                uint32_t total;

                progressDialog(title, "SPI PSRAM wr...", 0, 0);
                total = 0;
                t0 = time_us_64();
                do {
                    psram_write_range(0, (const uint8_t*)burst, OSD_INFO_BUF_SZ);
                    total += OSD_INFO_BUF_SZ;
                } while (time_us_64() - t0 < 500000ULL);
                elapsed = time_us_64() - t0;
                spi_wr = (float)total / (float)elapsed;

                progressDialog(title, "SPI PSRAM rd...", 50, 1);
                total = 0;
                t0 = time_us_64();
                do {
                    psram_read_range(0, (uint8_t*)burst, OSD_INFO_BUF_SZ);
                    total += OSD_INFO_BUF_SZ;
                } while (time_us_64() - t0 < 500000ULL);
                elapsed = time_us_64() - t0;
                spi_rd = (float)total / (float)elapsed;

                progressDialog(title, "", 100, 1);
                progressDialog("", "", 0, 2);
            }
            // QSPI/butter PSRAM — memory-mapped, use memset + sum loop
            if (has_qspi) {
                extern uint8_t* PSRAM_DATA;
                const uint32_t TEST_SZ = 0x10000u; // 64KB
                uint32_t sz = (uint32_t)butter_psram_size() < TEST_SZ
                            ? (uint32_t)butter_psram_size() : TEST_SZ;
                uint64_t t0, elapsed;
                uint32_t total;

                progressDialog(title, "QSPI PSRAM wr...", 0, 0);
                total = 0;
                t0 = time_us_64();
                do {
                    memset(PSRAM_DATA, 0xAA, sz);
                    total += sz;
                } while (time_us_64() - t0 < 300000ULL);
                elapsed = time_us_64() - t0;
                qspi_wr = (float)total / (float)elapsed;

                progressDialog(title, "QSPI PSRAM rd...", 50, 1);
                total = 0;
                t0 = time_us_64();
                do {
                    const uint32_t *p32 = (const uint32_t *)PSRAM_DATA;
                    uint32_t a;
                    for (a = 0; a < sz / 4; a++) {
                        if (p32[a] != 0xAAAAAAAAu) break;
                    }
                    total += a * 4;
                } while (time_us_64() - t0 < 300000ULL);
                elapsed = time_us_64() - t0;
                qspi_rd = (float)total / (float)elapsed;

                progressDialog(title, "", 100, 1);
                progressDialog("", "", 0, 2);
            }
        }

        // --- SD Card ---
        if (do_sd && sd_present)
            sd_ok = benchFsSpeed("/bench.tmp", "SD", title, sd_rd, sd_wr);

        // --- USB flash stick ---
        if (do_usb && usb_present)
            usb_ok = benchFsSpeed("USB:/bench.tmp", "USB", title, usb_rd, usb_wr);

        // --- NET (HTTPS download from the catalog Pages) ---
#if ZIFI_NET_CLIENT
        NetBenchCtx net_ctx = {};
        bool net_wifi = false, net_ran = false;
        if (do_net) {
            string ssid, ip;
            net_wifi = ZiFiAT::getStatus(ssid, ip);
            if (net_wifi)
                net_ran = benchNetSpeed(net_ctx, title);
        }
#endif

        // --- Build result text ---
        char (&buf)[OSD_INFO_BUF_SZ] = osd_info_buf;
        int pos = 0;

        if (do_cpu) {
            uint32_t cpu_mhz = clock_get_hz(clk_sys) / 1000000u;
            pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                " CPU     : %u MHz\n"
                " MIPS    : %.1f\n\n",
                (unsigned)cpu_mhz, cpu_mips);
        }
        if (do_sram) {
            pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                " SRAM rd : %.1f MB/s\n"
                " SRAM wr : %.1f MB/s\n\n",
                sram_rd, sram_wr);
        }
        if (do_psram) {
            if (!has_spi && !has_qspi) {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " PSRAM   : N/A\n\n");
            }
            if (has_spi) {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " SPI PSRAM rd: %.1f MB/s\n"
                    " SPI PSRAM wr: %.1f MB/s\n\n",
                    spi_rd, spi_wr);
            }
            if (has_qspi) {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " QSPI PSRAM rd: %.2f MB/s\n"
                    " QSPI PSRAM wr: %.2f MB/s\n\n",
                    qspi_rd, qspi_wr);
            }
        }
        if (do_sd) {
            if (!sd_present) {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " SD      : No card\n\n");
            } else if (sd_ok) {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " SD rd   : %.2f MB/s\n"
                    " SD wr   : %.2f MB/s\n\n",
                    sd_rd, sd_wr);
            } else {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " SD      : Error\n\n");
            }
        }
        if (do_usb) {
            if (!usb_present) {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " USB     : No stick\n\n");
            } else if (usb_ok) {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " USB rd  : %.2f MB/s\n"
                    " USB wr  : %.2f MB/s\n\n",
                    usb_rd, usb_wr);
            } else {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " USB     : Error\n\n");
            }
        }
#if ZIFI_NET_CLIENT
        if (do_net) {
            // 2xx + bytes counts as success even when r.ok was cleared by the
            // sink's time cap — the sample is what we measure.
            if (!net_wifi) {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " NET     : No WiFi\n\n");
            } else if (net_ran && net_ctx.r.status >= 200 && net_ctx.r.status < 300 &&
                       net_ctx.bytes > 0 && net_ctx.t_end > net_ctx.t_first) {
                float kbs = (float)net_ctx.bytes * (1000000.0f / 1024.0f)
                          / (float)(net_ctx.t_end - net_ctx.t_first);
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " NET rd  : %.1f KB/s\n"
                    " TLS hshk: %lu ms\n\n",
                    kbs, (unsigned long)((net_ctx.t_first - net_ctx.t_start) / 1000));
            } else {
                pos += snprintf(buf + pos, OSD_INFO_BUF_SZ - pos,
                    " NET     : Error (%d)\n\n", net_ctx.r.status);
            }
        }
#endif

        showTextDialog(title, buf);
    }
}


void (*OSD::progressOverride)(const char* title, const char* msg, int percent,
                              int action, bool cyrillic) = nullptr;

void OSD::progressDialog(const string& title, const string& msg, int percent, int action, bool cyrillic) {
    if (progressOverride) {
        progressOverride(title.c_str(), msg.c_str(), percent, action, cyrillic);
        return;
    }

    static unsigned short h;
    static unsigned short y;
    static unsigned short w;
    static unsigned short x;
    static unsigned short progress_x;
    static unsigned short progress_y;
    static unsigned int j;
    static bool cyr;   // remembered from SHOW so UPDATE/CLOSE keep the same face

    if (action == 0 ) { // SHOW

        cyr = cyrillic;

        h = (OSD_FONT_H * 6) + 2;
        y = scrAlignCenterY(h);

        // Transcode Cyrillic (online-catalog names) to CP1251 before measuring/truncating,
        // so widths are right and we never cut a multibyte UTF-8 sequence.
        string cmsg = cyr ? FileUtils::utf8ToCp1251(msg) : msg;
        string ctitle = cyr ? FileUtils::utf8ToCp1251(title) : title;

        size_t maxchars = (scrW / 6) - 4;
        string tmsg = cmsg.length() > maxchars ? cmsg.substr(0, maxchars) : cmsg;
        string ttitle = ctitle.length() > maxchars ? ctitle.substr(0, maxchars) : ctitle;

        w = (((tmsg.length() > ttitle.length() + 6 ? tmsg.length(): ttitle.length() + 6) + 2) * OSD_FONT_W) + 2;
        x = scrAlignCenterX(w);

        // Save backbuffer data
        VIDEO::SaveRect.save(x, y, w, h);

        // Set font
        VIDEO::vga.setFont(cyr ? Font6x8Cyr : Font6x8);

        // Menu border
        VIDEO::vga.rect(x, y, w, h, zxColor(0, 0));

        VIDEO::vga.fillRect(x + 1, y + 1, w - 2, OSD_FONT_H, zxColor(0,0));
        VIDEO::vga.fillRect(x + 1, y + 1 + OSD_FONT_H, w - 2, h - OSD_FONT_H - 2, zxColor(7,1));

        // Title
        VIDEO::vga.setTextColor(zxColor(7, 1), zxColor(0, 0));
        VIDEO::vga.setCursor(x + OSD_FONT_W + 1, y + 1);
        VIDEO::vga.print(ttitle.c_str());

        // Msg
        VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(7, 1));
        VIDEO::vga.setCursor(scrAlignCenterX(tmsg.length() * OSD_FONT_W), y + 1 + (OSD_FONT_H * 2));
        VIDEO::vga.print(tmsg.c_str());

        // Rainbow
        unsigned short rb_y = y + 8;
        unsigned short rb_paint_x = x + w - 30;
        uint8_t rb_colors[] = {2, 6, 4, 5};
        for (uint8_t c = 0; c < 4; c++) {
            for (uint8_t i = 0; i < 5; i++) {
                VIDEO::vga.line(rb_paint_x + i, rb_y, rb_paint_x + 8 + i, rb_y - 8, zxColor(rb_colors[c], 1));
            }
            rb_paint_x += 5;
        }

        // Progress bar frame
        progress_x = scrAlignCenterX(72);
        progress_y = y + (OSD_FONT_H * 4);
        VIDEO::vga.rect(progress_x, progress_y, 72, OSD_FONT_H + 2, zxColor(0, 0));
        progress_x++;
        progress_y++;

    } else if (action == 1 ) { // UPDATE

        // Msg
        VIDEO::vga.setFont(cyr ? Font6x8Cyr : Font6x8);
        string umsg = cyr ? FileUtils::utf8ToCp1251(msg) : msg;
        VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(7, 1));
        VIDEO::vga.setCursor(scrAlignCenterX(umsg.length() * OSD_FONT_W), y + 1 + (OSD_FONT_H * 2));
        VIDEO::vga.print(umsg.c_str());

        // Progress bar
        int barsize = (70 * percent) / 100;
        VIDEO::vga.fillRect(progress_x, progress_y, barsize, OSD_FONT_H, zxColor(5,1));
        VIDEO::vga.fillRect(progress_x + barsize, progress_y, 70 - barsize, OSD_FONT_H, zxColor(7,1));
    } else if (action == 2) { // CLOSE
        // Restore backbuffer data
        VIDEO::SaveRect.restore_last();
    }
}

uint8_t OSD::msgDialog(const string& title_, const string& msg_) {

    const unsigned short h = (OSD_FONT_H * 6) + 2;
    const unsigned short y = scrAlignCenterY(h);
    uint8_t res = DLG_NO;

    string msg = msg_, title = title_;
    if (msg.length() > (scrW / 6) - 4) msg = msg.substr(0,(scrW / 6) - 4);
    if (title.length() > (scrW / 6) - 4) title = title.substr(0,(scrW / 6) - 4);

    const unsigned short w = (((msg.length() > title.length() + 6 ? msg.length() : title.length() + 6) + 2) * OSD_FONT_W) + 2;
    const unsigned short x = scrAlignCenterX(w);

    // Save backbuffer data
    VIDEO::SaveRect.save(x, y, w, h);

    // Set font
    VIDEO::vga.setFont(Font6x8);

    // Menu border
    VIDEO::vga.rect(x, y, w, h, zxColor(0, 0));

    VIDEO::vga.fillRect(x + 1, y + 1, w - 2, OSD_FONT_H, zxColor(0,0));
    VIDEO::vga.fillRect(x + 1, y + 1 + OSD_FONT_H, w - 2, h - OSD_FONT_H - 2, zxColor(7,1));

    // Title
    VIDEO::vga.setTextColor(zxColor(7, 1), zxColor(0, 0));
    VIDEO::vga.setCursor(x + OSD_FONT_W + 1, y + 1);
    VIDEO::vga.print(title.c_str());

    // Msg
    VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(7, 1));
    VIDEO::vga.setCursor(scrAlignCenterX(msg.length() * OSD_FONT_W), y + 1 + (OSD_FONT_H * 2));
    VIDEO::vga.print(msg.c_str());

    // Yes
    VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(7, 1));
    VIDEO::vga.setCursor(scrAlignCenterX(6 * OSD_FONT_W) - (w >> 2), y + 1 + (OSD_FONT_H * 4));
    VIDEO::vga.print(" Yes  ");

    // // Ruler
    // VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(7, 1));
    // VIDEO::vga.setCursor(x + 1, y + 1 + (OSD_FONT_H * 3));
    // VIDEO::vga.print("123456789012345678901234567");

    // No
    VIDEO::vga.setTextColor(zxColor(0, 1), zxColor(5, 1));
    VIDEO::vga.setCursor(scrAlignCenterX(6 * OSD_FONT_W) + (w >> 2), y + 1 + (OSD_FONT_H * 4));
    VIDEO::vga.print("  No  ");

    // Rainbow
    unsigned short rb_y = y + 8;
    unsigned short rb_paint_x = x + w - 30;
    uint8_t rb_colors[] = {2, 6, 4, 5};
    for (uint8_t c = 0; c < 4; c++) {
        for (uint8_t i = 0; i < 5; i++) {
            VIDEO::vga.line(rb_paint_x + i, rb_y, rb_paint_x + 8 + i, rb_y - 8, zxColor(rb_colors[c], 1));
        }
        rb_paint_x += 5;
    }

    // Keyboard loop
    fabgl::VirtualKeyItem Menukey;
    while (1) {
        // Process external keyboard
        if (ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
            if (ESPectrum::readKbd(&Menukey)) {
                if (!Menukey.down) continue;
                if (is_left(Menukey.vk)) {
                    // Yes
                    VIDEO::vga.setTextColor(zxColor(0, 1), zxColor(5, 1));
                    VIDEO::vga.setCursor(scrAlignCenterX(6 * OSD_FONT_W) - (w >> 2), y + 1 + (OSD_FONT_H * 4));
                    VIDEO::vga.print(" Yes  ");
                    // No
                    VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(7, 1));
                    VIDEO::vga.setCursor(scrAlignCenterX(6 * OSD_FONT_W) + (w >> 2), y + 1 + (OSD_FONT_H * 4));
                    VIDEO::vga.print("  No  ");
                    click();
                    res = DLG_YES;
                } else if (is_right(Menukey.vk)) {
                    // Yes
                    VIDEO::vga.setTextColor(zxColor(0, 0), zxColor(7, 1));
                    VIDEO::vga.setCursor(scrAlignCenterX(6 * OSD_FONT_W) - (w >> 2), y + 1 + (OSD_FONT_H * 4));
                    VIDEO::vga.print(" Yes  ");
                    // No
                    VIDEO::vga.setTextColor(zxColor(0, 1), zxColor(5, 1));
                    VIDEO::vga.setCursor(scrAlignCenterX(6 * OSD_FONT_W) + (w >> 2), y + 1 + (OSD_FONT_H * 4));
                    VIDEO::vga.print("  No  ");
                    click();
                    res = DLG_NO;
                } else if (is_enter(Menukey.vk)) {
                    break;
                } else if (Menukey.vk == fabgl::VK_ESCAPE) {
                    res = DLG_CANCEL;
                    break;
                }
            }
        }
        sleep_ms(5);
    }

    click();

    // Restore backbuffer data
    VIDEO::SaveRect.restore_last();

    return res;

}

bool OSD::videoModeConfirm(int timeout_sec) {
    const bool keep = nm::uiConfirmTimeout("Keep this video mode?", "Video Mode", timeout_sec);
    // The chrome never touches SaveRect: the paper repaints every frame, the border
    // only on demand — ask for it.
    VIDEO::brdnextframe = true;
    return keep;
}


#define MENU_JOYSELKEY_EN "Key      \n"\
    "None     \n"\
    "A-Z      \n"\
    "0-9      \n"\
    "Special  \n"\
    "Joystick \n"
#define MENU_JOYSELKEY MENU_JOYSELKEY_EN

#define MENU_JOY_AZ "A-Z\n"\
    "A\n"\
    "B\n"\
    "C\n"\
    "D\n"\
    "E\n"\
    "F\n"\
    "G\n"\
	"H\n"\
	"I\n"\
	"J\n"\
	"K\n"\
    "L\n"\
    "M\n"\
    "N\n"\
    "O\n"\
    "P\n"\
    "Q\n"\
    "R\n"\
	"S\n"\
	"T\n"\
	"U\n"\
	"V\n"\
    "W\n"\
    "X\n"\
    "Y\n"\
    "Z\n"

#define MENU_JOY_09 "0-9\n"\
	"0\n"\
    "1\n"\
    "2\n"\
    "3\n"\
    "4\n"\
    "5\n"\
    "6\n"\
    "7\n"\
	"8\n"\
	"9\n"

#define MENU_JOY_SPECIAL\
    "F1\n"\
    "F2\n"\
    "F3\n"\
    "F4\n"\
    "F5\n"\
	"F6\n"\
	"F7\n"\
    "F8\n"\
    "F9\n"\
    "F10\n"\
    "F11\n"\
    "F12\n"\
    "Pause\n"\
    "PrtScr\n"\
    "Left\n"\
    "Right\n"\
    "Up\n"\
    "Down\n"\
    "Enter\n"\
    "Caps\n"\
    "SymbShift\n"\
    "Brk/Space\n"\
    "Backspace\n"\
    "KP 0/Ins\n"\
    "KP ./Del\n"

#define MENU_JOY_KEMPSTON "DPAD\n"\
    "Left\n"\
    "Right\n"\
    "Up\n"\
    "Down\n"\
    "A\n"\
    "B\n"\
    "C\n"\
    "X\n"\
    "Y\n"\
    "Z\n"\
    "Start\n"\
    "Select\n"

string vkToText(int key) {

fabgl::VirtualKey vk = (fabgl::VirtualKey) key;

switch (vk)
{
case fabgl::VK_0:
    return "    0    ";
case fabgl::VK_1:
    return "    1    ";
case fabgl::VK_2:
    return "    2    ";
case fabgl::VK_3:
    return "    3    ";
case fabgl::VK_4:
    return "    4    ";
case fabgl::VK_5:
    return "    5    ";
case fabgl::VK_6:
    return "    6    ";
case fabgl::VK_7:
    return "    7    ";
case fabgl::VK_8:
    return "    8    ";
case fabgl::VK_9:
    return "    9    ";
case fabgl::VK_A:
    return "    A    ";
case fabgl::VK_B:
    return "    B    ";
case fabgl::VK_C:
    return "    C    ";
case fabgl::VK_D:
    return "    D    ";
case fabgl::VK_E:
    return "    E    ";
case fabgl::VK_F:
    return "    F    ";
case fabgl::VK_G:
    return "    G    ";
case fabgl::VK_H:
    return "    H    ";
case fabgl::VK_I:
    return "    I    ";
case fabgl::VK_J:
    return "    J    ";
case fabgl::VK_K:
    return "    K    ";
case fabgl::VK_L:
    return "    L    ";
case fabgl::VK_M:
    return "    M    ";
case fabgl::VK_N:
    return "    N    ";
case fabgl::VK_O:
    return "    O    ";
case fabgl::VK_P:
    return "    P    ";
case fabgl::VK_Q:
    return "    Q    ";
case fabgl::VK_R:
    return "    R    ";
case fabgl::VK_S:
    return "    S    ";
case fabgl::VK_T:
    return "    T    ";
case fabgl::VK_U:
    return "    U    ";
case fabgl::VK_V:
    return "    V    ";
case fabgl::VK_W:
    return "    W    ";
case fabgl::VK_X:
    return "    X    ";
case fabgl::VK_Y:
    return "    Y    ";
case fabgl::VK_Z:
    return "    Z    ";
case fabgl::VK_RETURN:
    return "  Enter  ";
case fabgl::VK_BACKSPACE:
    return "Backspace";
case fabgl::VK_KP_0:
    return "KP 0/Ins ";
case fabgl::VK_KP_PERIOD:
    return "KP ./Del ";
case fabgl::VK_SPACE:
    return "Brk/Space";
case fabgl::VK_LSHIFT:
    return "  Caps   ";
case fabgl::VK_RSHIFT:
    return " RShift  ";
case fabgl::VK_LCTRL:
    return "  LCtrl  ";
case fabgl::VK_RCTRL:
    return "  RCtrl  ";
case fabgl::VK_F1:
    return "   F1    ";
case fabgl::VK_F2:
    return "   F2    ";
case fabgl::VK_F3:
    return "   F3    ";
case fabgl::VK_F4:
    return "   F4    ";
case fabgl::VK_F5:
    return "   F5    ";
case fabgl::VK_F6:
    return "   F6    ";
case fabgl::VK_F7:
    return "   F7    ";
case fabgl::VK_F8:
    return "   F8    ";
case fabgl::VK_F9:
    return "   F9    ";
case fabgl::VK_F10:
    return "   F10   ";
case fabgl::VK_F11:
    return "   F11   ";
case fabgl::VK_F12:
    return "   F12   ";
case fabgl::VK_PAUSE:
    return "  Pause  ";
case fabgl::VK_PRINTSCREEN:
    return " PrtScr  ";
case fabgl::VK_LEFT:
    return "  Left   ";
case fabgl::VK_RIGHT:
    return "  Right  ";
case fabgl::VK_UP:
    return "   Up    ";
case fabgl::VK_DOWN:
    return "  Down   ";
case fabgl::VK_DPAD_LEFT:
    return "Joy.Left ";
case fabgl::VK_DPAD_RIGHT:
    return "Joy.Right";
case fabgl::VK_DPAD_UP:
    return " Joy.Up  ";
case fabgl::VK_DPAD_DOWN:
    return "Joy.Down ";
case fabgl::VK_DPAD_FIRE:
    return "  Joy.A  ";
case fabgl::VK_DPAD_ALTFIRE:
    return "  Joy.B  ";
case fabgl::VK_DPAD_SELECT:
    return " Joy.Sel ";
case fabgl::VK_DPAD_START:
    return "Joy.Start";
case fabgl::VK_JOY_C:
    return "  Joy.C  ";
case fabgl::VK_JOY_X:
    return "  Joy.X  ";
case fabgl::VK_JOY_Y:
    return "  Joy.Y  ";
case fabgl::VK_JOY_Z:
    return "  Joy.Z  ";
case fabgl::VK_JOY_L2:
    return "  Joy.L2 ";
case fabgl::VK_JOY_R2:
    return "  Joy.R2 ";
case fabgl::VK_HOME:
    return "  Home   ";
case fabgl::VK_END:
    return "   End   ";
case fabgl::VK_PAGEUP:
    return "  PgUp   ";
case fabgl::VK_PAGEDOWN:
    return "  PgDn   ";
case fabgl::VK_INSERT:
    return " Insert  ";
case fabgl::VK_DELETE:
    return " Delete  ";
case fabgl::VK_NUMLOCK:
    return " NumLock ";
case fabgl::VK_TAB:
    return "   Tab   ";
case fabgl::VK_TILDE:
    return "    ~    ";
case fabgl::VK_GRAVEACCENT:
    return "    `    ";
case fabgl::VK_SLASH:
    return "    /    ";
case fabgl::VK_BACKSLASH:
    return "    \\    ";
case fabgl::VK_SEMICOLON:
    return "    ;    ";
case fabgl::VK_QUOTE:
    return "    '    ";
case fabgl::VK_COMMA:
    return "    ,    ";
case fabgl::VK_PERIOD:
    return "    .    ";
case fabgl::VK_MINUS:
    return "    -    ";
case fabgl::VK_EQUALS:
    return "    =    ";
case fabgl::VK_LEFTBRACKET:
    return "    [    ";
case fabgl::VK_RIGHTBRACKET:
    return "    ]    ";
case fabgl::VK_VOLUMEUP:
    return "  Vol+   ";
case fabgl::VK_VOLUMEDOWN:
    return "  Vol-   ";
case fabgl::VK_VOLUMEMUTE:
    return "  Mute   ";
default:
    return "  None   ";
}

}

unsigned int joyControl[14][3]={
    {34,55,zxColor(0,0)},   // Left
    {87,55,zxColor(0,0)},   // Right
    {63,30,zxColor(0,0)},   // Up
    {63,78,zxColor(0,0)},   // Down
    {49,109,zxColor(0,0)},  // Start
    {130,109,zxColor(0,0)}, // Select
    {142,17,zxColor(0,0)},  // A (circle left of A cell)
    {222,17,zxColor(0,0)},  // B
    {142,65,zxColor(0,0)},  // C
    {142,41,zxColor(0,0)},  // X
    {222,41,zxColor(0,0)},  // Y
    {222,65,zxColor(0,0)},  // Z
    {142,89,zxColor(0,0)},  // L2 (circle left of L2 cell)
    {222,89,zxColor(0,0)}   // R2
};

// Also reachable as OSD::vkToText for the new UI's joystick page (defined after
// the free function it forwards to).
string OSD::vkToText(int key) { return ::vkToText(key); }

void DrawjoyControls(unsigned short x, unsigned short y) {

    // Draw joy controls

    // Left arrow
    for (int i = 0; i <= 5; i++) {
        VIDEO::vga.line(x + joyControl[0][0] + i, y + joyControl[0][1] - i, x + joyControl[0][0] + i, y + joyControl[0][1] + i, joyControl[0][2]);
    }

    // Right arrow
    for (int i = 0; i <= 5; i++) {
        VIDEO::vga.line(x + joyControl[1][0] + i, y + joyControl[1][1] - ( 5 - i), x + joyControl[1][0] + i, y + joyControl[1][1] + ( 5 - i), joyControl[1][2]);
    }

    // Up arrow
    for (int i = 0; i <= 6; i++) {
        VIDEO::vga.line(x + joyControl[2][0] - i, y + joyControl[2][1] + i, x + joyControl[2][0] + i, y + joyControl[2][1] + i, joyControl[2][2]);
    }

    // Down arrow
    for (int i = 0; i <= 6; i++) {
        VIDEO::vga.line(x + joyControl[3][0] - (6 - i), y + joyControl[3][1] + i, x + joyControl[3][0] + ( 6 - i), y + joyControl[3][1] + i, joyControl[3][2]);
    }

    // START text
    VIDEO::vga.setTextColor(joyControl[4][2], zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[4][0], y + joyControl[4][1]);
    VIDEO::vga.print("START");

    // SELECT text
    VIDEO::vga.setTextColor(joyControl[5][2], zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[5][0], y + joyControl[5][1]);
    VIDEO::vga.print("SELECT");

    // Text A
    VIDEO::vga.setTextColor( joyControl[6][2],zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[6][0], y + joyControl[6][1]);
    VIDEO::vga.circle(x + joyControl[6][0] + 3, y + joyControl[6][1] + 3, 8,  joyControl[6][2]);
    VIDEO::vga.print("A");

    // Text B
    VIDEO::vga.setTextColor(joyControl[7][2],zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[7][0], y + joyControl[7][1]);
    VIDEO::vga.circle(x + joyControl[7][0] + 3, y + joyControl[7][1] + 3, 8,  joyControl[7][2]);
    VIDEO::vga.print("B");

    // Text C
    VIDEO::vga.setTextColor(joyControl[8][2],zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[8][0], y + joyControl[8][1]);
    VIDEO::vga.circle(x + joyControl[8][0] + 3, y + joyControl[8][1] + 3, 8, joyControl[8][2]);
    VIDEO::vga.print("C");

    // Text X
    VIDEO::vga.setTextColor(joyControl[9][2],zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[9][0], y + joyControl[9][1]);
    VIDEO::vga.circle(x + joyControl[9][0] + 3, y + joyControl[9][1] + 3, 8, joyControl[9][2]);
    VIDEO::vga.print("X");

    // Text Y
    VIDEO::vga.setTextColor(joyControl[10][2],zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[10][0], y + joyControl[10][1]);
    VIDEO::vga.circle(x + joyControl[10][0] + 3, y + joyControl[10][1] + 3, 8, joyControl[10][2]);
    VIDEO::vga.print("Y");

    // Text Z
    VIDEO::vga.setTextColor(joyControl[11][2],zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[11][0], y + joyControl[11][1]);
    VIDEO::vga.circle(x + joyControl[11][0] + 3, y + joyControl[11][1] + 3, 8, joyControl[11][2]);
    VIDEO::vga.print("Z");

    // Text L2
    VIDEO::vga.setTextColor(joyControl[12][2], zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[12][0] - 3, y + joyControl[12][1]);
    VIDEO::vga.circle(x + joyControl[12][0] + 3, y + joyControl[12][1] + 3, 8, joyControl[12][2]);
    VIDEO::vga.print("L2");

    // Text R2
    VIDEO::vga.setTextColor(joyControl[13][2], zxColor(7, 1));
    VIDEO::vga.setCursor(x + joyControl[13][0] - 3, y + joyControl[13][1]);
    VIDEO::vga.circle(x + joyControl[13][0] + 3, y + joyControl[13][1] + 3, 8, joyControl[13][2]);
    VIDEO::vga.print("R2");

}

// Returns a short display string for a hotkey binding, left-aligned, e.g. "ALT+F1", "F2", "None"
static string hkBindingText(int idx) {
    const Config::HotkeyBinding &b = Config::hotkeys[idx];
    if (b.vk == (uint16_t)fabgl::VK_NONE) return "None";
    // vkToText returns a 9-char padded string; trim it
    string kname = vkToText(b.vk);
    size_t s = kname.find_first_not_of(' ');
    size_t e = kname.find_last_not_of(' ');
    string trimmed = (s == string::npos) ? "None" : kname.substr(s, e - s + 1);
    string text;
    if (b.ctrl && b.alt) text = "C+A+" + trimmed;
    else if (b.ctrl)      text = "C+" + trimmed;
    else if (b.alt)      text = "A+" + trimmed;
    else                 text = trimmed;
    return text;
}



// EN
const char* const hkDescEN[Config::HK_COUNT] = {
    "Main menu",            // HK_MAIN_MENU
    "Load (SNA,Z80,P)",     // HK_LOAD_SNA
    "Load snapshot",        // HK_PERSIST_LOAD
    "Save snapshot",        // HK_PERSIST_SAVE
    "Open file",            // HK_LOAD_ANY
    "Play/Stop tape",       // HK_TAPE_PLAY
    "Tape browser",         // HK_TAPE_BROWSER
    "CPU/Tape stats",       // HK_STATS
    "Volume down",          // HK_VOL_DOWN
    "Volume up",            // HK_VOL_UP
    "Hard reset",           // HK_HARD_RESET
    "Reboot RP2350",        // HK_REBOOT
    "Max speed toggle",     // HK_MAX_SPEED
    "Pause",                // HK_PAUSE
    "Hardware info",        // HK_HW_INFO
    "Turbo mode",           // HK_TURBO
    "Debug",                // HK_DEBUG
    "Insert disk",          // HK_DISK
    "NMI",                  // HK_NMI
    "Reset to...",          // HK_RESET_TO
    "USB Boot mode",        // HK_USB_BOOT
    "Gigascreen toggle",    // HK_GIGASCREEN
    "LED indicators",       // HK_LED_TOGGLE
    "Input poke",           // HK_POKE
    "HDMI 60Hz mode",       // HK_VIDMODE_60
    "HDMI 50Hz mode",       // HK_VIDMODE_50
    "Quick Load snapshot",  // HK_QUICK_LOAD
    "Quick Save snapshot",  // HK_QUICK_SAVE
};

// The Help > Hot keys page of the new UI: description + current binding.
const char* hotkeysText() {
    char (&buf)[OSD_INFO_BUF_SZ] = osd_info_buf;
    int pos = 0;
    for (int i = 0; i < Config::HK_COUNT; i++) {
        if (Config::hotkeys[i].vk == (uint16_t)fabgl::VK_NONE) continue;  // unbound: as the classic page
        const string b = hkBindingText(i);
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %-20s %s\n",
                        hkDescEN[i], b.c_str());
        if (pos >= (int)sizeof(buf) - 1) break;
    }
    // Everything below is hard-wired in ESPectrum::processKeyboard — not entries of the
    // table above, so not remappable and previously listed only on the classic page.
    const bool profi = Z80Ops::isProfi;
    if (profi) {
        // On Profi plain PrtScr is the Karabas XT-keyboard toggle, so BMP capture moves
        // to Alt+PrtScr. The XT toggle is now the ONLY way to reach that setting — its
        // Machine-menu row is gone, which makes this line its documentation.
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %-20s %s\n",
                        "XT keyboard", "Alt+~ or PrtScr");
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %-20s %s\n",
                        "BMP capture", "Alt+PrtScr");
    } else {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %-20s %s\n",
                        "BMP capture", "PrtScr");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, " %-20s %s\n",
                    "Cursor = Joystick", "ScrollLk");
    if (profi) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n Karabas (Menu = Win)\n");
        for (int p = 0; p < kProfiHkCount; p++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %-20s %s\n",
                            kProfiHkDescEN[p], kProfiHkKeys[p]);
            if (pos >= (int)sizeof(buf) - 1) break;
        }
    }
    return buf;
}


// ── IDE slot editor / image creator ────────────────────────────────────────────
// IDE/HDD (NEMO / PROFI): the two image slots and image creation. Insert / eject /
// CHS override / create — a wizard rather than a setting, so the Devices rows reach
// it as one modal call.
void ideSlotEdit(uint8_t slot) {
    while (1) {
        char title[16];
        snprintf(title, sizeof(title), "hd%u", (unsigned)slot);
        const bool has = !Config::ide_image[slot].empty();
        const bool cd  = has && IDE::isCD(slot);

        // Rows: Insert / Eject / CHS (not for ATAPI) — geometry shown as the value.
        char geo[40] = "";
        if (has && !cd) {
            const uint16_t C = IDE::geomC(slot), H = IDE::geomH(slot), S = IDE::geomS(slot);
            const bool over = Config::ide_chs[slot][0] && Config::ide_chs[slot][1] &&
                              Config::ide_chs[slot][2];
            if (C && H && S)
                snprintf(geo, sizeof(geo), "CHS %u/%u/%u%s", C, H, S,
                         IDE::scheme == IDE::PROFI ? " (C only)" : (over ? "" : " auto"));
        }
        char sz[40] = "";
        if (has) {
            if (cd) snprintf(sz, sizeof(sz), "CD-ROM, %u MB",
                             (unsigned)(((uint64_t)IDE::sizeBytes(slot)) / (1024 * 1024)));
            else    snprintf(sz, sizeof(sz), "LBA %u (%u MB)", (unsigned)IDE::geomLBA(slot),
                             (unsigned)(((uint64_t)IDE::geomLBA(slot) * 512) / (1024 * 1024)));
        }

        const char* rows[4];
        int n = 0;
        rows[n++] = has ? "Replace image" : "Insert image";
        if (has) rows[n++] = "Eject";
        if (has && !cd) rows[n++] = geo[0] ? geo : "CHS (auto)";
        if (has) rows[n++] = sz;

        char sub[80];
        string shown = "<empty>";
        if (has) {
            shown = Config::ide_image[slot];
            const size_t sl = shown.rfind('/');
            if (sl != string::npos) shown = shown.substr(sl + 1);
        }
        snprintf(sub, sizeof(sub), "%s  %s", title, shown.c_str());
        const int sel = nm::uiPickList(sub, rows, n, 0);
        if (sel < 0) return;

        if (sel == 0) {                                  // insert / replace
            string mFile = nm::browseFile(FileUtils::IMG_Path, MENU_IDE_IMG_TITLE, DISK_IMGFILE);
            if (mFile.empty()) continue;
            string fname = FileUtils::IMG_Path + mFile.substr(1);
            if (FileUtils::getLCaseExt(fname) == "zip") {
                const string zf = ZipExtract::extract(fname, DISK_IMGFILE);
                if (zf.empty()) { nm::uiToast(OSD_ZIP_ERR, true, 1500); continue; }
                if (zf == "\x1b") continue;
                fname = zf;
            }
            Config::ide_image[slot] = fname;
            IDE::init();
            Config::save();
            continue;
        }
        if (has && sel == 1) {                           // eject
            Config::ide_image[slot].clear();
            IDE::init();
            Config::save();
            continue;
        }
        if (has && !cd && sel == 2) {                    // CHS override
            if (IDE::scheme == IDE::PROFI) {
                // Profi locks H=16/S=16 (BIOS CHS addressing) — only C is editable.
                char cur[8]; snprintf(cur, sizeof(cur), "%u", IDE::geomC(slot));
                string in = cur;
                if (!nm::uiPrompt("Cylinders (empty = auto)", in, 6, false, true)) continue;
                unsigned c = 0;
                if (!in.empty() && sscanf(in.c_str(), "%u", &c) != 1) {
                    nm::uiToast("Cylinders: number or empty", true, 2000);
                    continue;
                }
                Config::ide_chs[slot][0] = (uint16_t)c;
                Config::ide_chs[slot][1] = c ? 16 : 0;
                Config::ide_chs[slot][2] = c ? 16 : 0;
            } else {
                char cur[20];
                if (Config::ide_chs[slot][0] && Config::ide_chs[slot][1] && Config::ide_chs[slot][2])
                    snprintf(cur, sizeof(cur), "%u/%u/%u", Config::ide_chs[slot][0],
                             Config::ide_chs[slot][1], Config::ide_chs[slot][2]);
                else
                    snprintf(cur, sizeof(cur), "%u/%u/%u", IDE::geomC(slot),
                             IDE::geomH(slot), IDE::geomS(slot));
                string in = cur;
                if (!nm::uiPrompt("C/H/S (empty = auto)", in, 14, false, true)) continue;
                unsigned c = 0, h = 0, s2 = 0;
                if (!in.empty() && sscanf(in.c_str(), "%u/%u/%u", &c, &h, &s2) != 3) {
                    nm::uiToast("Format: C/H/S", true, 2000);
                    continue;
                }
                if (!((c == 0 && h == 0 && s2 == 0) ||
                      (h >= 1 && h <= 16 && s2 >= 1 && s2 <= 63 && c >= 1))) {
                    nm::uiToast("Invalid CHS (H<=16 S<=63)", true, 2000);
                    continue;
                }
                Config::ide_chs[slot][0] = (uint16_t)c;
                Config::ide_chs[slot][1] = (uint16_t)h;
                Config::ide_chs[slot][2] = (uint16_t)s2;
            }
            IDE::init();
            Config::save();
            continue;
        }
        // The size/LBA row is informational.
    }
}

void ideCreateImage() {
    static const char* const slots[] = { "hd0", "hd1" };
    const int slot = nm::uiPickList("Create image in", slots, 2, 0);
    if (slot < 0) return;

    static const char* const sizes[] = { "10 MB", "32 MB", "64 MB", "128 MB" };
    static const uint32_t mbs[] = { 10, 32, 64, 128 };
    const int si = nm::uiPickList(MENU_IDE_CREATE_SIZE, sizes, 4, 1);
    if (si < 0) return;

    string name = "new";
    if (!nm::uiPrompt("Image name", name, 18)) return;
    while (!name.empty() && name.back()  == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) return;
    if (FileUtils::getLCaseExt(name) != "hdd") name += ".hdd";

    const string path = FileUtils::IMG_Path + name;
    FILINFO fno;
    if (f_stat(path.c_str(), &fno) == FR_OK) {
        nm::uiToast("File already exists", true, 2000);
        return;
    }
    // ide_create_progress() reports through progressDialog, which the new UI has
    // pointed at its footer status line for the whole menu session.
    OSD::progressDialog("Creating HDD", name, 0, 0);
    const bool ok = IDE::createImage(path.c_str(), mbs[si], ide_create_progress);
    OSD::progressDialog("", "", 100, 1);
    OSD::progressDialog("", "", 0, 2);
    if (!ok) { nm::uiToast("Create failed", true, 2000); return; }
    Config::ide_image[slot] = path;
    IDE::init();
    Config::save();
    nm::uiToast("HDD image created", false, 1500);
}


// Capture a new binding for hotkey `idx`, exactly as the classic dialog does:
// modifiers tracked separately, Spectrum keys refused, duplicates refused, Esc
// cancels. Returns true when the binding changed. The caller owns the prompt on
// screen and the repaint afterwards.
bool hotkeyCapture(int idx) {
    auto Kbd = ESPectrum::PS2Controller.keyboard();
    fabgl::VirtualKeyItem Nextkey;

    // Joystick mapping off for the duration: LALT/cursors must not arrive as DPAD.
    const bool savedCursorAsJoy = Config::CursorAsJoy;
    Config::CursorAsJoy = false;

    while (Kbd->virtualKeyAvailable()) Kbd->getNextVirtualKey(&Nextkey);

    bool alt = false, ctrl = false, changed = false;
    while (1) {
        sleep_ms(5);
        if (!Kbd->virtualKeyAvailable()) continue;
        Kbd->getNextVirtualKey(&Nextkey);
        if (Nextkey.vk == fabgl::VK_LALT || Nextkey.vk == fabgl::VK_RALT)
            alt = Nextkey.down;
        if (Nextkey.vk == fabgl::VK_LCTRL || Nextkey.vk == fabgl::VK_RCTRL)
            ctrl = Nextkey.down;
        if (!Nextkey.down) continue;
        const fabgl::VirtualKey vk = Nextkey.vk;
        // Ignore modifier, joystick and menu-navigation keys
        if (vk == fabgl::VK_LALT || vk == fabgl::VK_RALT ||
            vk == fabgl::VK_LCTRL || vk == fabgl::VK_RCTRL ||
            vk == fabgl::VK_LSHIFT || vk == fabgl::VK_RSHIFT ||
            vk == fabgl::VK_DPAD_FIRE || vk == fabgl::VK_DPAD_ALTFIRE ||
            vk == fabgl::VK_DPAD_LEFT || vk == fabgl::VK_DPAD_RIGHT ||
            vk == fabgl::VK_DPAD_UP || vk == fabgl::VK_DPAD_DOWN ||
            vk == fabgl::VK_DPAD_SELECT || vk == fabgl::VK_DPAD_START ||
            vk == fabgl::VK_MENU_LEFT || vk == fabgl::VK_MENU_RIGHT ||
            vk == fabgl::VK_MENU_UP || vk == fabgl::VK_MENU_DOWN ||
            vk == fabgl::VK_MENU_ENTER || vk == fabgl::VK_MENU_BS ||
            vk == fabgl::VK_MENU_HOME)
            continue;
        if (vk == fabgl::VK_ESCAPE) break;

        // Only non-Spectrum keys (F-keys, navigation, special)
        if (!((vk >= fabgl::VK_F1 && vk <= fabgl::VK_F12) ||
              vk == fabgl::VK_PAUSE || vk == fabgl::VK_PRINTSCREEN ||
              vk == fabgl::VK_SCROLLLOCK || vk == fabgl::VK_NUMLOCK ||
              vk == fabgl::VK_INSERT ||
              vk == fabgl::VK_HOME || vk == fabgl::VK_END ||
              vk == fabgl::VK_PAGEUP || vk == fabgl::VK_PAGEDOWN ||
              vk == fabgl::VK_TILDE || vk == fabgl::VK_GRAVEACCENT ||
              vk == fabgl::VK_VOLUMEUP || vk == fabgl::VK_VOLUMEDOWN ||
              vk == fabgl::VK_VOLUMEMUTE ||
              vk == fabgl::VK_DELETE || vk == fabgl::VK_BACKSPACE)) {
            nm::uiToast("Key not allowed", true, 800);
            continue;
        }

        bool conflict = false;
        for (int i = 0; i < Config::HK_COUNT; i++) {
            if (i == idx) continue;
            if (Config::hotkeys[i].vk == (uint16_t)vk &&
                Config::hotkeys[i].alt == alt && Config::hotkeys[i].ctrl == ctrl) {
                conflict = true;
                break;
            }
        }
        if (conflict) {
            nm::uiToast("Already assigned", true, 1000);
            continue;
        }

        Config::hotkeys[idx] = { (uint16_t)vk, alt, ctrl };
        changed = true;
        break;
    }

    Config::CursorAsJoy = savedCursorAsJoy;
    return changed;
}

// Row text for the new UI's hotkey level: description + current binding.
const char* hotkeyRowDesc(int idx) {
    return (idx >= 0 && idx < Config::HK_COUNT) ? hkDescEN[idx] : "";
}
const char* hotkeyRowBinding(int idx) {
    static char buf[24];
    snprintf(buf, sizeof(buf), "%s", hkBindingText(idx).c_str());
    return buf;
}
bool hotkeyReadonly(int idx) {
    return idx >= 0 && idx < Config::HK_COUNT && Config::hotkeys[idx].readonly;
}


// Pick a ZX key for a pad control, reusing the classic submenu tables so the
// option -> VirtualKey mapping lives in one place. Returns the new VK, or -1 when
// the user backed out. Drawn with the new UI's list picker.
int OSD::joyPickKey(int currentVk) {
    static const char* const groups[] = { "None", "A-Z", "0-9", "Special", "Joystick" };
    const int g = nm::uiPickList("Assign key", groups, 5, 0);
    if (g < 0) return -1;
    if (g == 0) return fabgl::VK_NONE;

    // Rows of each group, in the same order the classic MENU_JOY_* strings had —
    // the arithmetic below depends on it.
    static const char* const az[] = {
        "A","B","C","D","E","F","G","H","I","J","K","L","M",
        "N","O","P","Q","R","S","T","U","V","W","X","Y","Z" };
    static const char* const d09[] = { "0","1","2","3","4","5","6","7","8","9" };
    static const char* const spc[] = {
        "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
        "Pause","PrtScr","Left","Right","Up","Down","Enter","Caps","SymbShift",
        "Brk/Space","Backspace","KP 0/Ins","KP ./Del" };
    static const char* const kmp[] = {
        "Left","Right","Up","Down","A","B","C","X","Y","Z","Start","Select" };

    if (g == 1) {
        const int i = nm::uiPickList("A-Z", az, 26, 0);
        return i < 0 ? -1 : (int)(47 + i + 1);          // classic: VK = 47 + opt2
    }
    if (g == 2) {
        const int i = nm::uiPickList("0-9", d09, 10, 0);
        return i < 0 ? -1 : (int)(1 + i + 1);           // classic: VK = 1 + opt2
    }
    if (g == 3) {
        const int i = nm::uiPickList("Special", spc, 25, 0);
        if (i < 0) return -1;
        const int opt2 = i + 1;
        if (opt2 < 13) return 158 + opt2;               // F1..F12
        switch (opt2) {
            case 13: return fabgl::VK_PAUSE;
            case 14: return fabgl::VK_PRINTSCREEN;
            case 15: return fabgl::VK_LEFT;
            case 16: return fabgl::VK_RIGHT;
            case 17: return fabgl::VK_UP;
            case 18: return fabgl::VK_DOWN;
            case 19: return fabgl::VK_RETURN;
            case 20: return fabgl::VK_LSHIFT;
            case 21: return fabgl::VK_LCTRL;
            case 22: return fabgl::VK_SPACE;
            case 23: return fabgl::VK_BACKSPACE;
            case 24: return fabgl::VK_KP_0;
            case 25: return fabgl::VK_KP_PERIOD;
            default: return -1;
        }
    }
    // Joystick (Kempston / Fuller): the classic handler shifts the first six by
    // 6 for Fuller.
    const int i = nm::uiPickList("Joystick", kmp, 12, 0);
    if (i < 0) return -1;
    const int opt2 = i + 1;
    int vk;
    switch (opt2) {
        case 1:  vk = fabgl::VK_DPAD_LEFT;    break;
        case 2:  vk = fabgl::VK_DPAD_RIGHT;   break;
        case 3:  vk = fabgl::VK_DPAD_UP;      break;
        case 4:  vk = fabgl::VK_DPAD_DOWN;    break;
        case 5:  vk = fabgl::VK_DPAD_FIRE;    break;
        case 6:  vk = fabgl::VK_DPAD_ALTFIRE; break;
        case 7:  vk = fabgl::VK_JOY_C;        break;
        case 8:  vk = fabgl::VK_JOY_X;        break;
        case 9:  vk = fabgl::VK_JOY_Y;        break;
        case 10: vk = fabgl::VK_JOY_Z;        break;
        case 11: vk = fabgl::VK_DPAD_START;   break;
        case 12: vk = fabgl::VK_DPAD_SELECT;  break;
        default: return -1;
    }
    if (Config::joystick == JOY_FULLER && opt2 <= 6) vk += 6;
    return vk;
}


// POKE DIALOG

#define DLG_OBJ_BUTTON 0
#define DLG_OBJ_INPUT 1
#define DLG_OBJ_COMBO 2

struct dlgObject {
    string Name;
    unsigned short int posx;
    unsigned short int posy;
    int objLeft;
    int objRight;
    int objTop;
    int objDown;
    unsigned char objType;
    string Label;
};

const dlgObject dlg_Objects[5] = {
    {"Bank",70,16,-1,-1, 4, 1, DLG_OBJ_COMBO , "RAM Bank  "},
    {"Address",70,32,-1,-1, 0, 2, DLG_OBJ_INPUT , "Address   "},
    {"Value",70,48,-1,-1, 1, 4, DLG_OBJ_INPUT , "Value     "},
    {"Ok",7,65,-1, 4, 2, 0, DLG_OBJ_BUTTON,  "  Ok  "},
    {"Cancel",52,65, 3,-1, 2, 0, DLG_OBJ_BUTTON, "  Cancel  "}
};

const string BankCombo[9] = { "   -   ", "   0   ", "   1   ", "   2   ", "   3   ", "   4   ", "   5   ", "   6   ", "   7   " };



void flushKbd() {
    auto kbd = ESPectrum::PS2Controller.keyboard();
    while (kbd->virtualKeyAvailable()) {
        fabgl::VirtualKeyItem dummy;
        kbd->getNextVirtualKey(&dummy);
    }
}

const dlgObject dlg_Objects2[3] = {
    {"Address",70,32,-1,-1, 0, 1, DLG_OBJ_INPUT , "Address   "},
    {"Ok",     7, 65, 2, 2, 0, 2, DLG_OBJ_BUTTON, "  Ok  "},
    {"Cancel", 52,65, 2, 2, 1, 0, DLG_OBJ_BUTTON, "  Cancel  "}
};





// Search memory for memSearchHex pattern starting at startAddr, return found address or 0x10000
static uint32_t memDoSearch(uint16_t startAddr) {
    int len = memSearchHex.length();
    if (len < 2 || (len & 1)) return 0x10000;
    int nBytes = len / 2;
    uint8_t pattern[8];
    if (nBytes > 8) nBytes = 8;
    for (int i = 0; i < nBytes; i++) {
        char hi = memSearchHex[i * 2], lo = memSearchHex[i * 2 + 1];
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hv = hexVal(hi), lv = hexVal(lo);
        if (hv < 0 || lv < 0) return 0x10000;
        pattern[i] = (hv << 4) | lv;
    }
    for (uint32_t off = 0; off < 0x10000; off++) {
        uint16_t addr = (startAddr + off) & 0xFFFF;
        bool match = true;
        for (int i = 0; i < nBytes; i++) {
            if (MemESP::readbyte((addr + i) & 0xFFFF) != pattern[i]) { match = false; break; }
        }
        if (match) {
            memSearchLastFound = addr;
            return addr;
        }
    }
    return 0x10000;
}


