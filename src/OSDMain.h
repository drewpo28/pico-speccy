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

#ifndef ESPECTRUM_OSD_H
#define ESPECTRUM_OSD_H

#include <string>
#include <vector>
#include <algorithm>
#include "fabutils.h"
#include "ff.h"
#include "FileUtils.h"  // DiskIface enum

using namespace std;

// Defines

#define OSD_FONT_W 6
#define OSD_FONT_H 8

#define LEVEL_INFO 0
#define LEVEL_OK 1
#define LEVEL_WARN 2
#define LEVEL_ERROR 3

#define DLG_CANCEL 0
#define DLG_YES 1
#define DLG_NO 2

// OSD Interface
class OSD {

public:

    // Screen size to be set at initialization
    static unsigned short scrW;
    static unsigned short scrH;

    // Calc
    static unsigned short scrAlignCenterX(unsigned short pixel_width);
    static unsigned short scrAlignCenterY(unsigned short pixel_height);
    static uint8_t osdMaxRows();
    static uint8_t osdMaxCols();
    static unsigned short osdInsideX();
    static unsigned short osdInsideY();

    // OSD
    static void osdHome();
    static void osdAt(uint8_t row, uint8_t col);
    static void drawOSD(bool bottom_info);
    static void drawStats();
    static void clearStats();
    static void drawVolumeBox();
    static void do_OSD(fabgl::VirtualKey KeytoESP, bool ALT, bool CTRL);
    // NMI action shared by the configurable NMI hotkey and Karabas Menu+F12:
    // DivMMC → plain NMI; ZX Byte / Pentagon / Profi → chooser menu (NMI vs
    // NMI+DOS etc.); otherwise plain NMI.
    static void nmiAction();
    static void HWInfo();
    static void ChipInfo();
    static void BoardInfo();
    static void MemoryInfo();
    static void EmulatorInfo();
    static void HIDDevices();
    static void SpeedTestRun(uint8_t st_opt);   // one benchmark row of the Speed test
    // While the fullscreen UI is active it points showTextDialog at its own
    // renderer, so every text page (ChipInfo, BoardInfo, ...) shows in the menu
    // style without per-page changes. nullptr = the plain centered box.
    static void (*textPageOverride)(const char* title, const char* text);
    // Same idea for progressDialog: the F5 session points it at the browser's footer
    // loader (nm::uiProgressStatus), so net fetches show in the status line instead
    // of a centered box. nullptr = the plain centered box.
    static void (*progressOverride)(const char* title, const char* msg, int percent,
                                    int action, bool cyrillic);
    static void showTextDialog(const char* title, const char* text, bool blocking = true, int* scroll_state = nullptr);

    // Error
    static void errorPanel(const string& errormsg);
    static void errorHalt(const string& errormsg);
    static void osdCenteredMsg(const string& msg, uint8_t warn_level);
    static void osdCenteredMsg(const string& msg, uint8_t warn_level, uint16_t millispause);

    // Non-blocking status banner, centred in the TOP border with the F8 stats look
    // (6x8 face, UI palette). Unlike osdCenteredMsg it neither covers the guest
    // screen nor sleeps: notify() only records the text, drawNotify() repaints it
    // once per frame from VIDEO::EndFrame() and, when it expires, hands the band
    // back to the border renderer (same erase contract as the corner FDD lamp).
    // Use it for the transient toasts a hotkey fires while the machine runs;
    // anything the user must acknowledge, or anything raised while the menu owns
    // the screen, still belongs in osdCenteredMsg.
    static void notify(const string& msg, uint8_t warn_level = LEVEL_INFO,
                       uint16_t millis = 1200);
    static void drawNotify();     // per-frame repaint (VIDEO::EndFrame)
    static void cancelNotify();   // drop it now and schedule the border erase
    static bool notifyAvailable();// false when the mode has no usable top border

    // Boot notices: setup() runs long before video is up, so a feature that gives up
    // there (GS::init short on heap, Gigascreen's prev-FB decline) can only queue a
    // line here; the first loop() frame shows them all in one centered box. One-shot:
    // after that flush new calls are dropped — mid-session failures already report
    // through the menu/budget-gate toasts. GS.cpp reaches bootNotice through the
    // C-linkage forward osd_boot_notice() (it does not include this header).
    static void bootNotice(const char* msg);
    static void flushBootNotices();

    static void osdDump();
    static void osdDebug(uint16_t gotoAddr = 0xFFFF);

    // Remote (FTP/SFTP) file browser — bounded RAM via an SD index (see OSDFile.cpp).
    static void remoteFileDialog(class RemoteFs* fs);

    // Shared "file-browser chrome" list, reused for the F5 location level, the
    // saved-remotes list, and the remote/web file browser so they all look like the
    // SD browser. Rendered by the fullscreen browser (src/ui/UiBrowser.cpp).
    enum { FD_SIDE_LOCATIONS = 0, FD_SIDE_HOSTS, FD_SIDE_REMOTE, FD_SIDE_WEB };
    enum { FDK_ENTER = 0, FDK_ALT, FDK_F2, FDK_F8, FDK_F5, FDK_F7, FDK_BACK, FDK_ESC };
    // Render the already-populated `filenames` index; returns the selected row (0-based)
    // or -1, with *outKey = FDK_*. Caller fills `filenames` (streamed) first. ioFocus/
    // ioBegin (if given) carry the cursor position in and out, so the caller can remember
    // it across re-lists and folder navigation (like the SD browser).
    static int fdChromeNav(const string& title, const string& subtitle, int side,
                           bool utf8, int* outKey, int* ioFocus = nullptr, int* ioBegin = nullptr);
    // Convenience: render a small fixed `rows` list (DIR_MARKER prefix = navigable).
    static int fdChromeList(const vector<string>& rows, const string& title,
                            const string& subtitle, int side, bool utf8, int* outKey,
                            int* ioFocus = nullptr, int* ioBegin = nullptr);
    // When set, the SD browser shows a ".." row even at the root "/" and selecting it
    // returns "" (the F5 handler then re-opens the locations chooser). Set by the F5
    // handler only when it entered SD via the locations level.
    static bool fd_root_parent;
    // The cwd remoteFileDialog last displayed — read by the caller to record the global
    // "last F5 location" (Config::last_loc) so F5 reopens where you left off.
    static string net_last_path;
    // Set when Esc is pressed in any network browser → unwind all the menu loops and
    // close the OSD (Esc closes the browser, like the old SD dialog). ".."/Backspace
    // climb one level instead. Reset by the F5 handler.
    static bool net_close_all;
    static void click();
    static void clickNoPause();   // click() without the paused-PAUSE-box repaint

    static uint8_t msgDialog(const string& title, const string& msg);
    // mask=true → password field: shows '*' until revealed (TAB toggles).
    // viscols = visible width in chars; when < maxlen the field scrolls
    // horizontally so up to maxlen characters can be entered. 0 → viscols=maxlen.
    static string inlineTextEdit(int ex, int ey, int maxlen, const string& text, bool mask = false, int viscols = 0);
    static bool videoModeConfirm(int timeout_sec = 15);
    // Cold-boot the machine into TR-DOS for the current arch (Pentagon/Profi/Gluk);
    // other archs fall back to a plain reset. Used after Alt+Enter mounts a disk.
    static void bootTrdos();
    static void progressDialog(const string& title, const string& msg, int percent, int action, bool cyrillic = false);
    // Shared with the joystick page of the menu: the key picker and the VK -> label
    // helper.
    static int joyPickKey(int currentVk);
    static string vkToText(int key);
    // Convert a .dls (full SD path) to a <stem>.bin bank in CONFIG_DIR, with on-screen
    // progress. "" on failure. Shared by the MIDI rows and the F5 browser.
    static string convertDlsToBank(const string& dlsPath);

    static void esp_hard_reset();

    // SRAM budget gate for the 5 heavy features. Call BEFORE the enable path.
    // Returns true → caller may proceed to enable the feature (it fits, or the
    // user freed room — in which case this reboots and never returns). Returns
    // false → caller must NOT enable (denied, or user cancelled the popup).
    static bool featureBudgetGate(int featureId);

    static bool updateROM(const string& file, uint8_t arch);
    // Defer-flash an ALF cartridge from `fname` into the shared region and reboot
    // into ALF (does NOT return on success). Used by the F5 browser, the Update menu
    // and the Web-Archive download launcher.
    static bool loadAlfCart(const string& fname);

    static char stats_lin1[25]; // "CPU: 00000 / IDL: 00000 ";
    static char stats_lin2[25]; // "FPS:000.00 / FND:000.00 ";

    static bool net_launch_close;         // Set when an online file was downloaded+launched → unwind menus and close OSD

};

// trim from start (in place)
static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(std::string &s) {
    rtrim(s);
    ltrim(s);
}

// trim from start (copying)
static inline std::string ltrim_copy(std::string s) {
    ltrim(s);
    return s;
}

// trim from end (copying)
static inline std::string rtrim_copy(std::string s) {
    rtrim(s);
    return s;
}

// trim from both ends (copying)
static inline std::string trim_copy(std::string s) {
    trim(s);
    return s;
}

#define is_up(vk) (vk == fabgl::VK_MENU_UP)
#define is_down(vk) (vk == fabgl::VK_MENU_DOWN)
#define is_home(vk) (vk == fabgl::VK_MENU_HOME)
#define is_backspace(vk) (vk == fabgl::VK_MENU_BS)
#define is_right(vk) (vk == fabgl::VK_MENU_RIGHT)
#define is_left(vk) (vk == fabgl::VK_MENU_LEFT)
#define is_back(vk) (vk == fabgl::VK_ESCAPE || vk == fabgl::VK_F1 || vk == fabgl::VK_MENU_LEFT)
#define is_enter(vk) (vk == fabgl::VK_MENU_RIGHT || vk == fabgl::VK_MENU_ENTER)

void flushKbd();

// Persist-slot primitives shared with the fullscreen UI (src/ui/UiActions.cpp).
// Defined in OSDMain.cpp.
std::string getSlotName(uint8_t slotnumber);          // "" empty, "\x01" no name
std::string getDefaultSnapshotName();
void persistDelete(uint8_t slotnumber);
void persistSetName(uint8_t slotnumber, const std::string& newName);
bool persistSaveNamed(uint8_t slotnumber, const std::string& slotName);
bool persistLoad(uint8_t slotnumber);
// US-layout shifted form of a symbol/digit ('1'->'!', '-'->'_', ...), shared with
// the UI's line editor. map_key() gives only unshifted symbol VKs.
char shiftSymUS(char c);
// Read-only view of the browsers' shared row index (OSDFile.cpp), for the renderer
// of the remote/web lists.
size_t fdIndexSize();
std::string fdIndexGet(size_t i);
// Hot-key remapping primitives for the Hot keys level (the capture loop and the row
// texts). Defined in OSDMain.cpp.
bool hotkeyCapture(int idx);
const char* hotkeyRowDesc(int idx);
const char* hotkeyRowBinding(int idx);
bool hotkeyReadonly(int idx);
// IDE slot editor (insert / eject / CHS) and the create-image wizard, for the
// Devices rows. Defined in OSDMain.cpp.
void ideSlotEdit(uint8_t slot);
void ideCreateImage();
// Rebuilds and returns the live hardware-summary text (Help > System status).
const char* hwInfoText();
// Live HID/XInput device list text (Help > HID devices).
const char* hidInfoText();
// Live emulated-machine summary text (Help > Emulator info).
const char* emuInfoText();
// Hot-key descriptions + current bindings (Help > Hot keys).
const char* hotkeysText();
// One-shot info-page text builders (also the right-pane previews of Help > ...).
// All return the shared osd_info_buf.
const char* chipInfoText();
const char* boardInfoText();
const char* memoryInfoText();

#endif // ESPECTRUM_OSD_H
