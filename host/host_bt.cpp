// Host build: the on-chip Bluetooth (src/net/BtHost.cpp, BTstack over the CYW43
// bus) has no radio on the PC. Every call answers "nothing there", so the menus
// show an idle, empty Bluetooth page.

#include "net/BtHost.h"
#include <stdint.h>

uint8_t audio_driver_running = 0;   // pwm_audio.cpp on the board (src/drivers is not built here)

namespace BtHost {
void setLog(LogCb) {}
bool scanBegin(int, bool) { return false; }
bool scanBusy() { return false; }
void scanStop() {}
int  found() { return 0; }
int  devCount() { return 0; }
const Dev* dev(int) { return nullptr; }
bool isAudio(const Dev&) { return false; }
bool isHid(const Dev&) { return false; }
int  known(bool, Known*, int) { return 0; }
bool isKnown(bool, const uint8_t*) { return false; }
bool connectedTo(bool, const uint8_t*) { return false; }
AudioState audioState() { return A_IDLE; }
const char* audioName() { return ""; }
bool audioConnect(const uint8_t*, const char*) { return false; }
void audioDisconnect() {}
bool remembered(uint8_t*, char*, int) { return false; }
void forget(const uint8_t*) {}
InputState inputState() { return I_IDLE; }
const char* inputName() { return ""; }
bool inputConnect(const Dev&) { return false; }
void inputDisconnect() {}
const InputInfo* inputInfo() { return nullptr; }
bool audioInfo(AudioInfo&) { return false; }
bool inputRemembered(uint8_t*, char*, int) { return false; }
void bootTick() {}
bool streaming() { return false; }
void pump() {}
const char* stateStr() { return "off (no radio on the host)"; }
}
