// pico-speccy — every user-visible string of the new fullscreen menu.
//
// Deliberately NOT in messages.h. Its macros were pre-rendered *rows* carrying
// layout markup (\t, trailing ">", "[X]" markers, {HK_*} tokens expanded by
// expandHotkeys, \x01 dim prefixes) and several were concatenated across #if
// variants, so they could not be reused as bare labels — keeping them apart is
// what let the classic menu's texts be deleted in one step once the cascade went.
// What is left of messages.h is dialog/error/status text (osdCenteredMsg,
// msgDialog, showTextDialog) plus the titles the file browsers take as arguments;
// bare menu labels belong here.
//
// English only (the Spanish strings and Config::lang are gone).

#pragma once


// ── top level ──────────────────────────────────────────────────────────────────
#define TXT_HELP            "Help"
#define TXT_MACHINE         "Machine"
#define TXT_SNAP_SAVE       "Save snapshot"
#define TXT_SNAP_LOAD       "Load snapshot"
#define TXT_HW              "Devices"
#define TXT_VIDEO           "Video"
#define TXT_AUDIO           "Audio"
#define TXT_JOYSTICK        "Joystick"
#define TXT_OPTIONS         "Options"
#define TXT_INTERFACE       "Interface"
#define TXT_NETWORK         "Network"
#define TXT_DEBUG           "Debug"
#define TXT_RESET           "Reset"
#define TXT_GAME            "Pico-Scwong"
#define TXT_VOLUME          "Volume"

// ── Help ───────────────────────────────────────────────────────────────────────
#define TXT_HELP_KEYS       "Hot keys"
#define TXT_HELP_REMAP      "Remap hot keys"
#define TXT_HELP_ZXKBD      "ZX keyboard"
#define TXT_HELP_ABOUT      "About"

// Authored here because the classic hotkeyDialog has no explanatory text at all —
// only a "F6:Defaults F8:Clear" footer and rules enforced silently in code.
#define TXT_HELP_REMAP_BODY \
    "Hot keys remapping\n" \
    "\n" \
    "Options > Other > Hot keys remapping lets you\n" \
    "rebind most emulator shortcuts.\n" \
    "\n" \
    "In the list:\n" \
    "  Enter    assign a new key to this action\n" \
    "  F6       restore all default bindings\n" \
    "  F8       clear the selected binding\n" \
    "  Esc      leave and keep the changes\n" \
    "\n" \
    "While assigning:\n" \
    "  press the key combination to bind, or\n" \
    "  Esc to cancel.\n" \
    "\n" \
    "Notes:\n" \
    "  - Greyed out rows are fixed and cannot be\n" \
    "    rebound (F1 always opens this menu).\n" \
    "  - Alt and Ctrl may be combined with a key.\n" \
    "  - A combination already used by another\n" \
    "    action is rejected; clear that one first.\n" \
    "  - Cleared actions stay available from the\n" \
    "    menu, they just have no shortcut.\n"

// ── Video ──────────────────────────────────────────────────────────────────────
#define TXT_VID_PALETTE     "Palette"
#define TXT_VID_SCANLINES   "Scanlines"
#define TXT_VID_CRT         "CRT filter"
#define TXT_VID_VSYNC       "V-Sync"
#define TXT_VID_DITHER      "HDMI dither"
#define TXT_VID_SNAP        "Capture-safe colours"

// ── Audio ──────────────────────────────────────────────────────────────────────
#define TXT_AUD_DRIVER      "Audio driver"
#define TXT_AUD_AY          "AY-3-8912"
#define TXT_AUD_AY_STEREO   "AY stereo mode"
#define TXT_AUD_TURBOSOUND  "TurboSound"
#define TXT_AUD_TSFM        "TurboSound FM"
#define TXT_AUD_VGM         "VGM chips"
#define TXT_VGM_ALL         "All"
#define TXT_AUD_OPL3        "YMF262 (OPL3)"
#define TXT_AUD_OPLL        "YM2413 (OPLL)"
#define TXT_AUD_CMS         "2x SAA1099 (CMS)"
#define TXT_AUD_SN          "2x SN76489"
#define TXT_SN_CLOCK        "Clock"
#define TXT_AUD_COVOX       "Covox port"
#define TXT_AUD_SOUNDRIVE   "SounDrive"
#define TXT_AUD_SAA         "SAA1099"
#define TXT_AUD_BOOST       "Volume boost"
#define TXT_VID_DMA         "DMA"
#define TXT_VID_MODE        "Mode"
#define TXT_VID_RENDER      "Render type"
#define TXT_VID_GIGASCREEN  "Gigascreen"
#define TXT_VID_ULAPLUS     "ULA+"
#define TXT_VID_TIMEX       "Timex Gfx mode"
#define TXT_VID_16COL       "16col (Pentagon)"
// TFT builds only (ST7789 / ILI9341): the panel's MADCTL orientation + inversion.
#define TXT_VID_TFT         "TFT panel"
#define TXT_TFT_INVERT      "Inversion"
#define TXT_TFT_BGR         "RGB / BGR order"
#define TXT_TFT_FLIPX       "Flip X"
#define TXT_TFT_FLIPY       "Flip Y"
#define TXT_TFT_DEFAULTS    "Restore defaults"

#define TXT_AUD_MIDI        "MIDI"
#define TXT_MIDI_BANK       "Bank set"
#define TXT_MIDI_STORAGE    "Bank storage"
#define TXT_MIDI_CONVERT    "[+] Convert a .dls..."
#define TXT_MIDI_DLS_PICK   "Select .dls soundbank"
#define TXT_AUD_GS          "General Sound"
#define TXT_GS_CLOCK        "Clock"
#define TXT_GS_RAM          "RAM"

// ── Overclock ──────────────────────────────────────────────────────────────────
#define TXT_OC_CPU          "CPU frequency"
#define TXT_OC_VREG         "Core voltage"
#define TXT_OC_FLASH        "Flash frequency"
#define TXT_OC_PSRAM        "PSRAM frequency"
#define TXT_OC_WARN         " Overclocking can stop the board from booting "

// ── Storage / Tape ─────────────────────────────────────────────────────────────
#define TXT_TAPE            "Tape"
#define TXT_TAPE_PICK       "Select tape"
#define TXT_TAPE_SELECT     "Select file"
#define TXT_TAPE_PLAYSTOP   "Play / Stop"
#define TXT_TAPE_BROWSER    "Tape browser"
#define TXT_TAPE_PLAYER     "Player mode"
#define TXT_TAPE_REALIN     "Real sound input"
#define TXT_TAPE_FASTLOAD   "Fast tape load"
#define TXT_TAPE_RG         "R.G. ROM timings"
#define TXT_TAPE_AUTOSTART  "Auto-start"

// ── Joystick / Additional preferences ──────────────────────────────────────────
#define TXT_JOY_CURSOR_AS   "Cursor as joy"
#define TXT_JOY_TO_CURSOR   "Joy to cursor"
#define TXT_JOY_TAB_FIRE    "TAB as Fire 1"
#define TXT_JOY_RIGHT_ENTER "R.Enter as Space"
#define TXT_JOY_WASD        "WASD as Kempston"
#define TXT_JOY_SECOND      "Second joystick"
#define TXT_JOY_KPORT       "Kempston port"

// ── Reset ──────────────────────────────────────────────────────────────────────
#define TXT_RESET_SOFT      "Soft ZX reset"
#define TXT_RESET_HARD      "Hard ZX reset"
#define TXT_RESET_RPI       "Hard RP2350 reset"
#define TXT_RESET_MOS       "MurmulatorOS"
#define TXT_RESET_FACTORY   "Reset to defaults"
#define TXT_RESET_SAVE_CFG  "Save my settings"
#define TXT_RESET_LOAD_CFG  "Load my settings"

// ── Additional hardware ────────────────────────────────────────────────────────
#define TXT_BETA            "Betadisk"
#define TXT_BETA_MODE       "Interface"
#define TXT_BETA_DRIVES     "Drives A-D"
#define TXT_BETA_FASTMODE   "Fast mode"
#define TXT_BETA_SNDLED     "Sound and LED"
#define TXT_BETA_ROM        "TR-DOS ROM"
#define TXT_BETA_AUTOBOOT   "Auto-boot"
#define TXT_SNAPSHOT        "Snapshot"
#define TXT_P3_DRIVES      "+3DOS"
#define TXT_P3_FASTDISK    "Fast disk"
#define TXT_P3_SPEEDLOCK   "Speedlock protection"
#define TXT_MB02            "MB-02+"
#define TXT_MB02_MODE       "Interface"
#define TXT_MB02_DRIVES     "Drives 1-4"
#define TXT_MB02_SNDLED     "Sound and LED"
#define TXT_ESX_INTERFACE   "Interface"
#define TXT_HW_ESX_IMAGES   "Images hd0 / hd1"
#define TXT_HW_ESXDOS       "esxDOS"
#define TXT_HW_ZC           "Z-Controller"
#define TXT_HW_IDE          "IDE / HDD"
#define TXT_IDE_IMAGES      "Images hd0 / hd1"
#define TXT_IDE_CREATE      "Create image"
#define TXT_HW_LED          "LED indicators"
#define TXT_HW_LEGEND       "LED legend"
// The board's own LED (GPIO 25), blinking on real SD traffic — internal /tmp
// files (swap, .idx) are deliberately not indicated. Not an on-screen indicator.
#define TXT_HW_SDLED        "Board LED on SD access"
#define TXT_HW_RTC          "RTC + NVRAM"
#define TXT_HW_OVERCLOCK    "Overclock"

// ── Joystick ───────────────────────────────────────────────────────────────────
#define TXT_JOY_TYPE        "Joystick type"
#define TXT_JOY_MAPPING     "Keyboard mapping"
#define TXT_JOY_PREFS       "Preferences"

// ── Machine ────────────────────────────────────────────────────────────────────
// Machine rows carry their ROM set in the right pane, so a machine and its ROM are
// chosen in one gesture instead of the classic two-level walk.
#define TXT_MACH_48K        "Spectrum 48K"
#define TXT_MACH_128K       "Spectrum 128K"
#define TXT_MACH_PENT       "Pentagon 128K"
#define TXT_MACH_P512       "Pentagon 512K"
#define TXT_MACH_P1024      "Pentagon 1024K"
#define TXT_MACH_BYTE       "Byte"
#define TXT_MACH_PROFI      "Profi"
#define TXT_MACH_KARABAS    "Karabas"
#define TXT_MACH_SCORP      "Scorpion"
#define TXT_MACH_ALF        "ALF TV GAME"
#define TXT_MACH_MURM       "Murmuzavr mode"
#define TXT_MACH_MURM_SIZE  "Extra RAM"
#define TXT_MACH_TSCONF      "TS-Conf (ZX-Evo)"
#define TXT_MACH_TSCONF_OPTS "Options"
#define TXT_MACH_TSCONF_RAM  "RAM"
#define TXT_ROM_TSBIOS       "TS-Conf"
#define TXT_MACH_COBMECT    "COBMECT. mode"

// ROM set labels, shared by the machine rows and the Preferred rom rows.
#define TXT_ROM_48K         "48K"
#define TXT_ROM_48K_ES      "48K Spanish"
#define TXT_ROM_48K_ES_S    "48K ESP"
#define TXT_ROM_128K        "128K"
#define TXT_ROM_128K_ES     "128K Spanish"
#define TXT_ROM_128K_ES_S   "128K ESP"
#define TXT_ROM_PLUS2       "+2"
#define TXT_ROM_PLUS2_ES    "+2 Spanish"
#define TXT_ROM_PLUS2_ES_S  "+2 ESP"
#define TXT_ROM_ZX81P       "ZX81+"
#define TXT_ROM_P3          "+3"
#define TXT_ROM_P3E         "+3 (IDEDOS)"
#define TXT_ROM_P3E_S       "+3 (IDE)"
#define TXT_ROM_PENT        "128K"
#define TXT_ROM_PENT_S      "128K"
#define TXT_ROM_PENT_GLUK   "128K + Mr Gluk"
#define TXT_ROM_PENT_GLUK_S "128K+Gluk"     // left-column short form (Option::slabel)
#define TXT_ROM_PENT_ORIG   "Original"
#define TXT_ROM_BYTE_48     "48K"
#define TXT_ROM_BYTE_128    "128K"
#define TXT_ROM_BYTE_GLUK   "128K + Mr Gluk"
#define TXT_ROM_BYTE_GLUK_S "128K+Gluk"  // left-column short form (Option::slabel)
#define TXT_ROM_CUSTOM      "Custom"
#define TXT_ROM_LAST        "Last used"
#define TXT_ROM_PROFI_ORIG  "Original"
// Karabas machine row — the real board's four ROMSET slots.
#define TXT_ROM_KAR_MAIN    "ROMain"
#define TXT_ROM_KAR_PQ      "PQDOS"
#define TXT_ROM_KAR_FT      "Flash Tool"
#define TXT_ROM_KAR_FDI     "FDImage"
// Scorpion PCB revisions (ZXMAK2's Yellow/Green): same v2.94 ROM, different
// frame timing (312 vs 316 lines).
#define TXT_ROM_SCORP        "ZS-256 Turbo (Yellow)"
#define TXT_ROM_SCORP_S      "ZS-256T"        // left-column short form (Option::slabel)
#define TXT_ROM_SCORP_GR     "ZS-256 Turbo+ (Green)"
#define TXT_ROM_SCORP_GR_S   "ZS-256T+"
#define TXT_ROM_SCORP_GMX    "ZS-256 Turbo+ & GMX"
#define TXT_ROM_SCORP_GMX_S  "GMX"
#define TXT_ROM_SCORP_1024   "ZS-1024 Turbo+"
#define TXT_ROM_SCORP_1024_S "ZS-1024T+"
#define TXT_ROM_SCORP_PROF   "ZS-1024 + ProfROM"
#define TXT_ROM_SCORP_PROF_S "ProfROM"
#define TXT_ROM_ALF         "ALF cartridge"

// ── Options ────────────────────────────────────────────────────────────────────
#define TXT_OPT_PREF_MACHINE "Preferred machine"
#define TXT_OPT_PREF_ROM     "Preferred rom"
// Inside the Interface menu the "menu" qualifier is redundant: the corners row
// sits right under Theme and its values (Rounded/Square) say the rest, and the
// VGA palette row is indented under Theme, which supplies the context.
#define TXT_OPT_THEME        "Theme"
#define TXT_OPT_VGA_MENU_PAL "VGA colors"
#define TXT_OPT_UI_CORNERS   "Corners"
#define TXT_OPT_REPLACE_ROM  "Replace ZX rom"
#define TXT_OPT_UPDATE_FW    "Update firmware"
#define TXT_ROM_PICK         "Select ROM file"
#define TXT_DLG_BOOTSEL      "Reboot into USB firmware update mode?"
#define TXT_ROM_SLOT_48      "Custom 48K"
#define TXT_ROM_SLOT_128     "Custom 128K"
#define TXT_ROM_SLOT_PENT    "Custom Pentagon"
#define TXT_ROM_SLOT_ALF     "Custom ALF"
#define TXT_ROM_SLOT_ALFCART "ALF cartridge"
#define TXT_ROM_SLOT_TRDOS   "TR-DOS"
#define TXT_ROM_SLOT_PENT0   "Pentagon bank #0"
#define TXT_ROM_SLOT_PENT1   "Pentagon bank #1"
#define TXT_OTHER_ALU        "ALU timing"
#define TXT_OTHER_ISSUE2     "48K Issue 2"
#define TXT_OTHER_FRAMESKIP  "Frameskip"
#define TXT_OTHER_HOTKEYS    "Remap hot keys"

// ── Debug ──────────────────────────────────────────────────────────────────────
#define TXT_DBG_DIALOG      "Debugger"
#define TXT_DBG_POKE        "Input poke"
#define TXT_DBG_LOG         "Debug log"
// Off = pretend the board has no PSRAM (testing the SRAM-only paths). Row shown only
// where a chip was actually found.
#define TXT_DBG_PSRAM       "PSRAM"
// No = hide the paper area and render the border colour through it instead
// (per-T-state, like top/bottom border) — shows border effects "under" the paper.
#define TXT_DBG_PAPER       "Paper"
#define TXT_DBG_TEMPOFF     "Temp offset"

// ── Hardware info ──────────────────────────────────────────────────────────────
// Alt+F1 live page (no menu row of its own — hotkey only, Esc closes).
#define TXT_INFO_SYSTEM     "Hardware Info"
#define TXT_INFO_CHIP       "Chip info"
#define TXT_INFO_BOARD      "Board info"
#define TXT_INFO_MEMORY     "Memory info"
#define TXT_INFO_EMU        "Emulator info"
#define TXT_INFO_HID        "HID devices"
#define TXT_INFO_SPEED      "Speed test"

// ── dialogs ────────────────────────────────────────────────────────────────────
#define TXT_DLG_REBOOT       "Reboot the board?"
#define TXT_DLG_FACTORY      "Reset all settings and reboot?"
#define TXT_DLG_SAVE_CFG     "Save the current settings as your custom configuration?"
#define TXT_DLG_LOAD_CFG     "Load your custom configuration and reboot?"
#define TXT_DLG_MOS          "Remove MurmulatorOS and reboot?"
#define TXT_DLG_APPLY_REBOOT "Some changes need a reboot. Apply and reboot now?"
#define TXT_MSG_CFG_SAVED    " Custom configuration saved "
#define TXT_MSG_SNAP_ERR     " Cannot load the snapshot "
#define TXT_MSG_SAVING       " Saving settings... "
#define TXT_MSG_ZIP_ERR      " Cannot extract the archive "
#define TXT_SNAP_PICK        "Select snapshot"
#define TXT_SLOT_PICK        "Select disk image"

// ── Network ────────────────────────────────────────────────────────────────────
#define TXT_NET_WIFI        "WiFi"
#define TXT_NET_SYNC        "Sync time (SNTP)"
#define TXT_NET_TZ          "Time zone"
#define TXT_NET_NIC_SUB     "ZiFi NIC"
#define TXT_NET_TRANSPORT   "Transport"
#define TXT_NET_URL_PROMPT  "URL  (https://host/path)"
#define TXT_NET_PINS_BACK   "Freed pins: peripheral returns after reboot"
#define TXT_NET_BAUD        "Baud rate"
#define TXT_NET_FTP         "FTP server"
#define TXT_NET_HTTP        "HTTP test (curl)"
#define TXT_MSG_WIFI_FIRST  "Enable WiFi first"
#define TXT_MSG_NO_NETS     "No networks found"
#define TXT_NET_PICK_TITLE  "Select network"

// ── About ──────────────────────────────────────────────────────────────────────
// "\x02<letter>" switches the ink (see UiDialog.cpp textPageRun): W white,
// M muted, G green, Y yellow, S sky, R red. Letters avoid hex digits on purpose.
#define TXT_HELP_ABOUT_BODY \
    "ZX Spectrum family emulator for RP2350\n" \
    "boards (Murmulator, Pico PC, Pico DV).\n" \
    "\n" \
    "\x02G(C) 2025-2026 Drew\n" \
    "New UI, features and ongoing\n" \
    "development since November 2025.\n" \
    "\n" \
    "Based on \x02WPico-Spec\x02T by \x02SMike V73\x02T -\n" \
    "the original port to the Raspberry\n" \
    "Pico / Pico 2 (Murmulator) platform.\n" \
    "\n" \
    "Pico-Spec builds on \x02WESPectrum\x02T:\n" \
    "(C) 2023-24 \x02YVictor Iborra \"Eremus\"\n" \
    "    2023    \x02YDavid Crespo \"dcrespo3d\"\n" \
    "Based on ZX-ESPectrum-Wiimote\n" \
    "(C) 2020-23 \x02YDavid Crespo\x02T, inspired by\n" \
    "projects from \x02YPete Todd\x02T, \x02YRampa & Queru\x02T.\n" \
    "\n" \
    "\x02WComponents:\n" \
    " \x02MZ80 core     \x02GJ.L. Sanchez\n" \
    " \x02MVGA/HDMI     \x02SMurmulator community\n" \
    " \x02MAY-3-8912    \x02YA. Sashnov\n" \
    " \x02MPS/2 driver  \x02GFabrizio di Vittorio\n" \
    " \x02MUSB host     \x02STinyUSB, Rumbledethumps\n" \
    "\n" \
    "Thanks to the authors of \x02WUnrealSpeccy\n" \
    "\x02Tand \x02WZXMAK2\x02T emulators, the collaborators,\n" \
    "testers and Patreons of the \x02WESPectrum\n" \
    "\x02Tproject, and to the \x02WMurmulator\x02T community.\n"

// ── misc ───────────────────────────────────────────────────────────────────────
#define TXT_TODO_TITLE      "Not implemented yet"
#define TXT_TODO_BODY       "This branch of the menu is not\nwired up yet."

