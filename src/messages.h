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

#ifndef ESPECTRUM_MESSAGES_h
#define ESPECTRUM_MESSAGES_h

// Stringify helper for embedding compile-time constants in menu strings
#define _PIN_STR(x) #x
#define _PIN_XSTR(x) _PIN_STR(x)

// General
#if PORT_VERSION_LEN <= 5
#define EMU_VERSION " v1.2/" PORT_VERSION "  "
#else
#define EMU_VERSION " v1.2/" PORT_VERSION " "
#endif

// Error
#define ERROR_TITLE "  !!!   ERROR - CLIVE MEDITATION   !!!  "
#define ERROR_BOTTOM "  Sir Clive is smoking in the Rolls...  "
// OSD
  #if PORT_VERSION_LEN <= 5
    #define OSD_TITLE  " pico-speccy v." PORT_VERSION " (ESPectrum 1.2)    "
  #else
    #define OSD_TITLE  " pico-speccy v." PORT_VERSION " (ESPectrum 1.2)   "
  #endif
// #define OSD_BOTTOM " SCIENCE LEADS TO PROGRESS              "
#define OSD_BOTTOM " Murmulator port by MikeV73" EMU_VERSION

#define OSD_PAUSE "--=[ PAUSED ]=--"

#define OSD_MAXSPEED_ON "--=[ MAX SPEED ON ]=--"

#define OSD_MAXSPEED_OFF "--=[ MAX SPEED OFF ]=--"

#define OSD_GIGASCREEN_ON "--=[ GIGASCREEN ON ]=--"

#define OSD_GIGASCREEN_OFF "--=[ GIGASCREEN OFF ]=--"

#define OSD_GIGASCREEN_AUTO "--=[ GIGASCREEN AUTO ]=--"

#define OSD_COBMECT_ON  "--=[ COBMECT. MODE ON ]=--"

#define OSD_COBMECT_OFF "--=[ COBMECT. MODE OFF ]=--"

#define OSD_PSNA_NOT_AVAIL "No Persist Snapshot Available"
#define OSD_PSNA_SAVING  "Saving Persist Snapshot"
#define OSD_PSNA_SAVE_WARN "Disk error. Trying slow mode, be patient"
#define OSD_PSNA_SAVE_ERR "ERROR Saving Persist Snapshot"
#define OSD_PSNA_LOAD_ERR "ERROR Loading Persist Snapshot"
#define OSD_TAPE_FLASHLOAD "Fast loading tape file"
#define OSD_TAPE_LOAD_ERR "ERROR Loading tape file"
#define OSD_TAPE_SAVE_ERR "ERROR Saving tape file"
#define OSD_TAPE_SELECT_ERR "No tape file selected"

#define OSD_DLG_REBOOT "Reboot the board?"

#define OSD_DLG_USBBOOT "Reboot to USB mode?"

#define OSD_DLG_APPLYREBOOT "Apply and reboot?"

#define OSD_DLG_SETJOYMAPDEFAULTS "Load joy type default map?"

// Factory reset: hold R at boot → confirm → wipe storage.nvs → reboot to defaults.
#define MSG_FACTORY_RESET_TITLE "Factory reset"
#define MSG_FACTORY_RESET_Q "Reset all settings to defaults?"
// "My Default" reset: hold M at boot → confirm → wipe storage.nvs (keeps
// default.nvs) → reboot, which then falls back to the user's saved default.
#define MSG_MYDEFAULT_RESET_TITLE "Reset to my Default"
#define MSG_MYDEFAULT_RESET_Q "Reset settings to your saved Default?"
// Guided boot prompt shown while the "hold R / hold M" reset window is open.
#define MSG_FACTORY_RESET_HOLD "Hold R: Factory Reset\nHold M: My Default"

#define OSD_NOROMFILE_ERR "No custom ROM file found."

#define OSD_ROM "Flash Custom ROM"

#define MENU_SNA_TITLE "Select Snapshot"

#define MENU_DSK_TITLE "Select disk"

#define MENU_ALL_TITLE "Open File"

#define OSD_IMG_NEEDS_ESXDOS "  Enable esxDOS first  "

#define OSD_DSK_WRITE_PROTECT "  Disk is write protected  "

#define OSD_ZIP_ERR " No supported file in ZIP "

#define OSD_ZIP_EXTRACTING "    Extracting...    "

#define OSD_ZIP_NOMEM " ZIP: not enough memory "

#define MENU_IMG_TITLE "esxDOS Image\n"
#define MENU_IDE_IMG_TITLE "IDE Image"
#define MENU_IDE_CREATE_SIZE "New HDD size\n"
#define MENU_NMI_TITLE "NMI\n"
#define MENU_NMI_SEL "NMI\n" "Magic Button\n"
// Extra NMI-menu row, appended at runtime on Byte.
#define MENU_BYTE_COBMECT_MODE "COBMECT. Mode\n"

#define MENU_RESETTO_128 "Reset to\n" "128K\n" "48K\n"

#define MENU_RESETTO_DIVMMC "Reset\n" "Soft Reset\n" "Hard Reset\n"

#define MENU_RESETTO_PENT "Reset to\n" "TR-DOS\n" "128K\n" "48K\n"

#define MENU_RESETTO_PENTGLUK "Reset to\n" "Mr Gluk Reset Srvs\n" "TR-DOS\n" "128K\n" "48K\n"

// Profi reset menu: Service ROM=1, TR-DOS=2, 128K=3, 48K=4
#define MENU_RESETTO_PROFI "Reset to\n" "Service ROM\n" "TR-DOS\n" "128K\n" "48K\n"

// Scorpion reset menu: Service monitor=1, TR-DOS=2, 128K=3, 48K=4
#define MENU_RESETTO_SCORP "Reset to\n" "Service monitor\n" "TR-DOS\n" "128K\n" "48K\n"

#define MOS_FILE "/.firmware"

// DLS wavetable mode (4): a user-supplied bank (gm_bank.bin) lives either in butter
// PSRAM (reloaded from SD each boot) or, provisioned once from SD, in a flash
// partition read via XIP (persistent, the only option without QSPI PSRAM).
// See Config::midi_storage.
#define MSG_MIDI_BANK_OK "DLS wavetable bank loaded."
// msgDialog sizes its width to the message length and is single-line only — keep
// this to ONE short line (a multi-line string makes the box span the whole screen).
// Shown when a newly picked bank differs from flash: confirm the (reboot-to-)flash
// so the user can decline and keep the current bank. Single short line.
#define MSG_MIDI_BANK_INSTALL_Q "Install this bank? (reboots)"
#define MSG_MIDI_BANK_FLASHING "Restarting to install DLS bank...\nBoot takes ~20-30s (LED blinks). Do\nNOT power off until it comes back."
// On-device .dls -> gm_bank.bin conversion (RP2350): progress and result.
#define MSG_MIDI_CONVERTING "Converting .dls to bank..."
#define MSG_MIDI_CONVERT_OK "Soundbank created."
#define MSG_MIDI_CONVERT_FAIL "Conversion failed (bad .dls or low\nspace). See debug log."
// A converted bank can exceed the fixed flash partition (~1.6 MB) — it is then
// written to SD but cannot be installed, so the picker hides it. Tell the user
// (the actual KB sizes are appended at runtime). Single short line for msgDialog.
#define MSG_MIDI_BANK_TOOBIG "Bank too big for flash"

#define OSD_DBG_HELP_EN \
    " [Space]      Step CPU\n"\
    " [ALT+Space]  Step over CALL\n"\
    " [Enter]      Go to address (view)\n"\
    " [Esc]        Exit\n"\
    " [ALT+F1]     Search memory\n"\
    " [F3]         Search next\n"\
    " [F1]         This Help\n"\
    " [F2]         Show memory dump\n"\
    " [ALT+F2]     Save dump to file\n"\
    " [F5]         Toggle PC breakpoint\n"\
    " [F7]         Add breakpoint (type)\n"\
    " [ALT+F7]     Breakpoint list\n"\
    " [Tab]        Code/Memory/Regs\n"\
    " [F8]         Set PC to address\n"\
    " [ALT+F9]     Show full screen\n"\
    " [0]          Default position\n"\
    " + PageUp/Down and cursor keys\n"


// Runtime SD automount toast (the probe runs on every board, so
// this must live OUTSIDE the RP2350-only network block below).
#define MSG_SD_AUTOMOUNT "SD card mounted"

// ─── ZiFi / Network menu strings ─────────────────────────────────────────────

#define MSG_WIFI_CONNECTING "Connecting..."
#define MSG_WIFI_CONNECTED "Connected"
#define MSG_WIFI_DISCONNECTED "Disconnected"
#define MSG_WIFI_CONNECT_ERR "Connect failed"
#define MSG_RTC_SYNCING "Syncing time..."
#define MSG_RTC_SYNCED "Time set:"
#define MSG_RTC_SYNC_ERR "Time sync failed"
#define MENU_ZIFI_USB_LABEL "USB (CH340)"
// ─── SRAM budget manager strings ────────────────────────────────────────────
// Heavy features (Gigascreen / General Sound / DivMMC / Profi / ZiFi) don't all
// fit in SRAM on butter-less boards. When enabling one would overflow, the OSD
// offers to free room or refuses.
#define MSG_BUDGET_DENY "not enough free SRAM"
#define MSG_BUDGET_APPLY "Apply & reboot"
#define MSG_BUDGET_INSUFFICIENT "Not enough freed - pick more"

// ─── File transfer (FTP/SFTP) client strings ────────────────────────────────
#define MSG_NET_PROTO_TITLE "Protocol"
#define MSG_NET_FT_NOWIFI "Connect Wi-Fi first"
#define MSG_NET_HOST_LABEL "Host:"
#define MSG_NET_USER_LABEL "User:"
#define MSG_NET_PORT_LABEL "Port:"
#define MSG_NET_PASS_LABEL "Pass:"
#define MSG_NET_CONNECTING "Connecting..."
#define MSG_NET_CONN_ERR "Connection failed"
#define MSG_NET_TRUST_Q "Trust this host key?"
#define MSG_NET_HOSTKEY_BAD "HOST KEY CHANGED!\nPossible MITM"
#define MSG_NET_DOWNLOADING "Downloading..."
#define MSG_NET_UPLOADING "Uploading..."
#define MSG_NET_XFER_OK "Transfer complete"
#define MSG_NET_XFER_ERR "Transfer failed"
#define MSG_NET_DELETE_Q "Delete?"
#define MSG_NET_COPYING "Copying..."
#define MSG_NET_LAUNCHING "Launching..."
#define MSG_NET_UNSUPPORTED "Cannot run this type"
// ─── F5 location picker + saved-remotes manager ─────────────────────────────
#define MSG_F5_LOCAL "Local (SD)"
#define MSG_F5_USB "USB Drive"
#define MSG_F5_REMOTE "Remote (FTP/SFTP)"
#define MSG_F5_WEB "Web Archives"
#define MENU_REMOTE_TITLE "Remote connections"
#define MSG_REMOTE_ADD_ROW "[Add Remote]"
#define MSG_REMOTE_FORGET_Q "Forget connection?"
#define MSG_REMOTE_SAVEPASS_Q "Save password?"
#define MSG_REMOTE_ALIAS_LABEL "Alias:"
#define MSG_REMOTE_PATH_LABEL "Path:"
#define MSG_REMOTE_FULL "Too many remotes"

// ─── Archive download (catalog server) strings ──────────────────────────────
#define MENU_ARCH_SITE_TITLE "Archive source"
#define MSG_ARCH_SITES_ERR "No sources found"

// ─── HTTP test ("curl") strings ─────────────────────────────────────────────
#define MSG_HTTP_TESTING "Requesting..."
#define MSG_HTTP_TEST_TITLE "HTTP test"


#endif // ESPECTRUM_MESSAGES_h
