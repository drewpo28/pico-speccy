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

// Line type
#define IS_TITLE 0
#define IS_FOCUSED 1
#define IS_NORMAL 2
#define IS_INFO 3

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
    static void SpeedTest();
    static void SpeedTestRun(uint8_t st_opt);   // one benchmark row (new-UI submenu)
    // While the new fullscreen UI is active it points showTextDialog at its own
    // renderer, so every classic text page (ChipInfo, BoardInfo, ...) shows in the
    // new style without per-page changes. nullptr = classic rendering.
    static void (*textPageOverride)(const char* title, const char* text);
    // Same idea for progressDialog: the F5 session points it at the new browser's
    // footer loader (nm::uiProgressStatus), so net fetches show in the status line
    // instead of a classic centered box. nullptr = classic rendering.
    static void (*progressOverride)(const char* title, const char* msg, int percent,
                                    int action, bool cyrillic);
    static void showTextDialog(const char* title, const char* text, bool blocking = true, int* scroll_state = nullptr);

    // Error
    static void errorPanel(const string& errormsg);
    static void errorHalt(const string& errormsg);
    static void osdCenteredMsg(const string& msg, uint8_t warn_level);
    static void osdCenteredMsg(const string& msg, uint8_t warn_level, uint16_t millispause);
    static void showLedLegend();
    static void zxKeyboardOverlay();    // classic ZX keyboard bitmap (also DS80 fallback)

    static void osdDump();
    static void osdDebug(uint16_t gotoAddr = 0xFFFF);

    // Menu
    static unsigned short menuRealRowFor(uint8_t virtual_row_num);
    // static bool menuIsSub(uint8_t virtual_row_num);
    static void menuPrintRow(uint8_t virtual_row_num, uint8_t line_type);
    static void menuRedraw();
    static void WindowDraw();
    static unsigned short menuRun(const string& new_menu);
    static unsigned short simpleMenuRun(const string& new_menu, uint16_t posx, uint16_t posy, uint8_t max_rows, uint8_t max_cols);
    // F5 slot-picker popup. Interface selects the slot family (Betadisk / MB-02+ / esxDOS).
    // If `fname` is non-empty, Enter mounts it into the focused slot and keeps the
    // popup open so the user can see the new state and mount into other slots.
    // Returns 0 on Esc (popup was opened as a side-effect of F5 / HK_DISK — the
    // caller does not need to act on the return value for mounting).
    // F2 toggles Write Protect for Betadisk/MB-02+ (no effect for esxDOS).
    // F8/Del ejects the focused slot.
    // All side effects (mount/eject/WP/path) are persisted via Config::save() and
    // survive Esc.
    static int diskSlotDialog(DiskIface iface, uint8_t initialSlot, const string& fname = "");
    // Format "Label\t[fname, WP]" or "Label\t<empty>" row text for slot menus.
    static string formatSlotRow(const string& label, const string& fname,
                                bool wp, bool showWP);
    static string fileDialog(string &fdir, const string& title, uint8_t ftype, uint8_t mfcols, uint8_t mfrows);
    // Remote (FTP/SFTP) file browser — bounded RAM via an SD index (see OSDFile.cpp).
    static void remoteFileDialog(class RemoteFs* fs);

    // Shared "file-browser chrome" list (Open File window + sidebar), reused for the
    // F5 location level, the saved-remotes list, and the remote/web file browser so
    // they all look like the SD browser. See OSDFile.cpp.
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
    // When set, the SD fileDialog shows a ".." row even at the root "/" and selecting it
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
    static int menuTape(const string& title);
    static void menuScroll(bool up);
    static void fd_Redraw(const string& title, const string& fdir, uint8_t ftype, const vector<string>& filexts);
    static void fd_PrintRow(uint8_t virtual_row_num, uint8_t line_type, const vector<string>& filexts);
    static void tapemenuRedraw(const string& title);
    static void PrintRow(uint8_t virtual_row_num, uint8_t line_type);
    static void menuAt(short int row, short int col);
    static void menuScrollBar(unsigned short br);
    static void click();
    static void clickNoPause();   // click() without the paused-PAUSE-box repaint
    static uint8_t menu_level;
    static bool menu_saverect;    
    static unsigned short menu_curopt;    

    static int8_t fdScrollPos;
    static int timeStartScroll;
    static int timeScroll;
    static unsigned int elements;
    static unsigned int ndirs;

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
    string inputBox(int x, int y, const string& text);
    static void joyDialog(void);
#if NEW_UI
    // Shared with the new UI's joystick page: the key picker (classic submenu
    // tables) and the VK -> label helper.
    static int joyPickKey(int currentVk);
    static string vkToText(int key);
#endif
    static void pokeDialog();
    static void jumpToDialog();
    static void hotkeyDialog();
    static void midiDialog();       // MIDI mode / preset / GM.DLS bank wizard
    // Convert a .dls (full SD path) to a <stem>.bin bank in CONFIG_DIR, with on-screen
    // progress. "" on failure. Shared by midiDialog, the F5 browser and the new menu.
    static string convertDlsToBank(const string& dlsPath);
    static void ideDialog();        // IDE scheme / images / create image wizard
    // Fast-snapshot slot pickers. Return true when the caller must leave do_OSD.
    static bool persistLoadDialog();
    static bool persistSaveDialog();
    static void BPDialog();
    static uint16_t BPListDialog();
    static bool dumpRangeDialog(uint16_t &from, uint16_t &to);
    static void memSearchDialog();
    static uint32_t addressDialog(uint16_t addr, const char* title);


    // Rows
    static unsigned short rowCount(const string& menu);
    static string rowGet(const string& menu, unsigned short row_number);

    static void esp_hard_reset();

    // SRAM budget gate for the 5 heavy features. Call BEFORE the enable path.
    // Returns true → caller may proceed to enable the feature (it fits, or the
    // user freed room — in which case this reboots and never returns). Returns
    // false → caller must NOT enable (denied, or user cancelled the popup).
    static bool featureBudgetGate(int featureId);

    static bool updateFirmware(FIL *firmware);
    static bool updateROM(const string& file, uint8_t arch);
    // Defer-flash an ALF cartridge from `fname` into the shared region and reboot
    // into ALF (does NOT return on success). Used by the F5 browser, the Update menu
    // and the Web-Archive download launcher.
    static bool loadAlfCart(const string& fname);

    static char stats_lin1[25]; // "CPU: 00000 / IDL: 00000 ";
    static char stats_lin2[25]; // "FPS:000.00 / FND:000.00 ";
    
    static uint8_t cols;                     // Maximum columns
    static uint8_t tab_col;                  // Tab stop column (longest left part before \t)
    static uint8_t max_right;                // Longest right part after \t (hotkeys only)
    static uint8_t mf_rows;                  // File menu maximum rows
    static unsigned short real_rows;      // Real row count
    static uint8_t virtual_rows;             // Virtual maximum rows on screen
    static uint16_t w;                        // Width in pixels
    static uint16_t h;                        // Height in pixels
    static uint16_t x;                        // X vertical position
    static uint16_t y;                        // Y horizontal position
    static uint16_t prev_y[5];                // Y prev. position
    static unsigned short menu_prevopt;
    static bool menu_del_pressed;         // Set by menuRun when Del pressed on a row
    static bool menu_rename_pressed;      // Set by menuRun when R pressed on a row
    static bool menu_quicksave_pressed;   // Set by menuRun when F4 pressed on a row
    static bool menu_quickload_pressed;   // Set by menuRun when F3 pressed on a row
    static bool net_launch_close;         // Set when an online file was downloaded+launched → unwind menus and close OSD
    static string menu_footer;            // Optional hint line drawn below menu (cleared after each menuRun)
    static string menu;                   // Menu string
    static unsigned short begin_row;      // First real displayed row
    static uint8_t focus;                    // Focused virtual row
    static uint8_t last_focus;               // To check for changes
    static unsigned short last_begin_row; // To check for changes

    static uint8_t fdCursorFlash;    
    static bool fdSearchRefresh;    
    static unsigned int fdSearchElements;    

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
#define is_enter_fd(vk) (vk == fabgl::VK_MENU_ENTER)
#define is_return(vk) (vk == fabgl::VK_MENU_ENTER)

void flushKbd();

#if NEW_UI
// Persist-slot primitives shared with the new fullscreen UI (src/ui/UiActions.cpp).
// Defined in OSDMain.cpp next to the classic persist dialogs.
std::string getSlotName(uint8_t slotnumber);          // "" empty, "\x01" no name
std::string getDefaultSnapshotName();
void persistDelete(uint8_t slotnumber);
void persistSetName(uint8_t slotnumber, const std::string& newName);
bool persistSaveNamed(uint8_t slotnumber, const std::string& slotName);
bool persistLoad(uint8_t slotnumber);
// US-layout shifted form of a symbol/digit ('1'->'!', '-'->'_', ...), shared with
// the new UI's line editor. map_key() gives only unshifted symbol VKs.
char shiftSymUS(char c);
// Read-only view of the classic browsers' shared row index (OSDFile.cpp), for the
// new-chrome renderer of the remote/web lists.
size_t fdIndexSize();
std::string fdIndexGet(size_t i);
// Hot-key remapping primitives for the new UI's Hot keys level (the capture loop
// and the row texts). Defined in OSDMain.cpp next to the classic hotkeyDialog.
bool hotkeyCapture(int idx);
const char* hotkeyRowDesc(int idx);
const char* hotkeyRowBinding(int idx);
bool hotkeyReadonly(int idx);
// IDE slot editor (insert / eject / CHS) and the create-image wizard, for the new
// UI's Devices rows. Defined in OSDMain.cpp next to the classic ideDialog.
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
#endif

#endif // ESPECTRUM_OSD_H
