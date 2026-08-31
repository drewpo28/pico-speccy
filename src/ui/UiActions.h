// pico-speccy — action leaves of the new fullscreen menu.
//
// Thin wrappers so UiTree.cpp stays pure data and UiNav.cpp stays pure engine. Each
// wrapper calls a long-lived OSD:: dialog; the nav loop repaints the fullscreen
// around the call.

#pragma once


#include <stdint.h>
#include <string>

#include "UiModel.h"   // DynRows

namespace nm {

void act_todo();                // placeholder for branches not yet wired up

// Help
void act_helpHotkeys();
void act_helpRemapInfo();
void act_helpZxKeyboard();
void act_helpAbout();
// Right-pane preview text of the Help/Info pages (NM_PAGE_PV): each returns the
// same text its full-screen page shows, from OSD's shared info buffer.
const char* pv_helpHotkeys();
const char* pv_helpRemap();
const char* pv_helpAbout();
const char* pv_chipInfo();
const char* pv_boardInfo();
const char* pv_memoryInfo();
const char* pv_emuInfo();
const char* pv_hidInfo();

// Storage — actions, because they mount media or restart the machine
void act_tapeSelect();
void act_tapePlayStop();
void act_tapeBrowser();
// The slot editors are LEVELS of the new menu, not modals: build fills the row pool,
// rowkey runs Enter / F2 / F8 on the focused slot.
void slots_buildBeta(DynRows& d);
void slots_buildMb02(DynRows& d);
void slots_buildEsx(DynRows& d);
void slots_buildIde(DynRows& d);
void slots_keyBeta(int32_t slot, uint8_t key);
void slots_keyMb02(int32_t slot, uint8_t key);
void slots_keyEsx(int32_t slot, uint8_t key);
void slots_keyIde(int32_t slot, uint8_t key);

// Pre-armed file for the "load to which slot?" flow (the disk hot key). Consumed by the
// next Enter on a slot row.
void slotsArmFile(const std::string& fname);


// Devices
void act_ledLegend();
// LED legend preview: label lines + the real LED sprite per row (NM_ACTION_PV).
const char* pv_ledLegend();
int         pvicon_ledLegend(int idx, int x, int y);

// Joystick / hot keys
void act_joyDialog();
// Hot keys are a LEVEL of the new menu: Enter re-binds the focused action, F6
// restores every default, F8 clears the focused binding.
void hotkeys_build(DynRows& d);
void hotkeys_key(int32_t idx, uint8_t key);
// MIDI instrument set (GM.DLS banks) is a LEVEL too: row 0 converts a .dls from the
// card, the rest are the banks scanBanks() finds; Enter selects / installs.
void midi_buildBanks(DynRows& d);
void midi_keyBanks(int32_t tag, uint8_t key);
void act_ideCreate();
// The 40 persist slots are LEVELS of the new menu (like the disk slots): build
// fills the pool, rowkey runs Enter / F6 rename / F8 remove on the focused slot.
void persist_build(DynRows& d);
void persist_keySave(int32_t slot, uint8_t key);
void persist_keyLoad(int32_t slot, uint8_t key);
void act_updateFirmware();
#if TFT
// Video > TFT panel > Restore defaults: stages the driver's own default MADCTL /
// inversion (landscape, BGR, no flips) — the classic TFT menu's "Defaults" row.
void act_tftDefaults();
#endif
void act_replaceRom(int32_t slot);

// Network
void act_wifi();
void act_sntp();
#if ZIFI_NET_CLIENT
void act_ftpServer();
void act_httpTest();
#endif
// Runtime option table for the Transport radio (NM_RADIO_D — per-board GPIO pairs).
const Option* zifi_transportOpts(uint8_t& cnt);
// Live label of the WiFi row (cached; one blocking AT status query per
// invalidation). Invalidate whenever an action changed the link state.
const char* vl_wifi();
void netStatusInvalidate();

// Debug
void act_debugDialog();
void act_debugPoke();

// Pico-Scwong — the built-in native paddle game (UiGame.cpp).
// The standalone twin is the boot-time entrance (held S in the R/M probe).
void act_gameScwong();
void gameScwongStandalone();

// Reset (all immediate by definition; three of them reboot)
void act_resetSoft();
void act_resetHard();
void act_resetBoard();
void act_resetMOS();
void act_resetFactory();
void act_saveCustomCfg();
void act_loadCustomCfg();
bool p_mosPresent();

// Hardware info
void act_chipInfo();
void act_boardInfo();
void act_memoryInfo();
void act_emulatorInfo();
void act_hidDevices();
void act_speedTestOne(int32_t opt);   // rows of the Speed test submenu

} // namespace nm

