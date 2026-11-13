// Game controllers on the PC (SDL GameController: Xbox, DualShock/DualSense, most USB
// and Bluetooth pads). The SDL thread samples the first two pads into bit masks;
// core0 turns the changes into the same virtual-key events the board's USB pad
// handler sends (hid_app.cpp process_ds4_gamepad), so joystick types, the 14-slot
// pad map, profiles and Joystick > Test behave exactly as on the board.
//   pad 1: D-pad/left stick, A = fire (+ menu Enter), B = second fire, X/Y/LB/RB/
//          LT/RT = the extra slots (VK_JOY_X/Y/Z/C/L2/R2), Start, Back = Select;
//   pad 2: the second joystick (gamepad2_bits), as the board's second NES pad.
#include <SDL.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "host_kbd_prelude.h"

extern input_bits_t gamepad1_bits;

namespace {

enum : uint32_t {
    P_UP = 1u << 0, P_DOWN = 1u << 1, P_LEFT = 1u << 2, P_RIGHT = 1u << 3,
    P_A = 1u << 4, P_B = 1u << 5, P_X = 1u << 6, P_Y = 1u << 7,
    P_L1 = 1u << 8, P_R1 = 1u << 9, P_L2 = 1u << 10, P_R2 = 1u << 11,
    P_START = 1u << 12, P_SELECT = 1u << 13,
};

SDL_GameController* s_pad[2] = {nullptr, nullptr};
std::atomic<uint32_t> s_state[2] = {{0}, {0}};
uint32_t s_applied[2] = {0, 0};

constexpr int AXIS_ON = 16000;

uint32_t sample(SDL_GameController* c) {
    if (!c) return 0;
    uint32_t m = 0;
    auto b = [&](SDL_GameControllerButton btn, uint32_t bit) { if (SDL_GameControllerGetButton(c, btn)) m |= bit; };
    b(SDL_CONTROLLER_BUTTON_DPAD_UP, P_UP);
    b(SDL_CONTROLLER_BUTTON_DPAD_DOWN, P_DOWN);
    b(SDL_CONTROLLER_BUTTON_DPAD_LEFT, P_LEFT);
    b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, P_RIGHT);
    b(SDL_CONTROLLER_BUTTON_A, P_A);
    b(SDL_CONTROLLER_BUTTON_B, P_B);
    b(SDL_CONTROLLER_BUTTON_X, P_X);
    b(SDL_CONTROLLER_BUTTON_Y, P_Y);
    b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, P_L1);
    b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, P_R1);
    b(SDL_CONTROLLER_BUTTON_START, P_START);
    b(SDL_CONTROLLER_BUTTON_BACK, P_SELECT);
    const int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    const int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx < -AXIS_ON) m |= P_LEFT;
    if (lx > AXIS_ON) m |= P_RIGHT;
    if (ly < -AXIS_ON) m |= P_UP;
    if (ly > AXIS_ON) m |= P_DOWN;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > AXIS_ON) m |= P_L2;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > AXIS_ON) m |= P_R2;
    return m;
}

void reopen() {
    for (int i = 0; i < 2; i++) { if (s_pad[i]) SDL_GameControllerClose(s_pad[i]); s_pad[i] = nullptr; }
    int n = 0;
    for (int j = 0; j < SDL_NumJoysticks() && n < 2; j++) {
        if (!SDL_IsGameController(j)) continue;
        if ((s_pad[n] = SDL_GameControllerOpen(j))) {
            fprintf(stderr, "host: pad %d = %s\n", n + 1, SDL_GameControllerName(s_pad[n]));
            n++;
        }
    }
}

// Pad 1: the board's USB-pad semantics (hid_app.cpp process_ds4_gamepad).
void apply1(uint32_t cur, uint32_t prev) {
    const uint32_t ch = cur ^ prev;
    if (!ch) return;
    const bool j1 = Config::secondJoy != 1;
    auto edge = [&](uint32_t bit, fabgl::VirtualKey menu, fabgl::VirtualKey dpad) {
        if (!(ch & bit)) return;
        const bool d = (cur & bit) != 0;
        if (menu != fabgl::VirtualKey::VK_NONE) joyPushData(menu, d);
        if (j1 && dpad != fabgl::VirtualKey::VK_NONE) joyPushData(dpad, d);
    };
    edge(P_UP, fabgl::VirtualKey::VK_MENU_UP, fabgl::VirtualKey::VK_DPAD_UP);
    edge(P_DOWN, fabgl::VirtualKey::VK_MENU_DOWN, fabgl::VirtualKey::VK_DPAD_DOWN);
    edge(P_LEFT, fabgl::VirtualKey::VK_MENU_LEFT, fabgl::VirtualKey::VK_DPAD_LEFT);
    edge(P_RIGHT, fabgl::VirtualKey::VK_MENU_RIGHT, fabgl::VirtualKey::VK_DPAD_RIGHT);
    edge(P_A, fabgl::VirtualKey::VK_MENU_ENTER, fabgl::VirtualKey::VK_DPAD_FIRE);
    edge(P_B, fabgl::VirtualKey::VK_NONE, fabgl::VirtualKey::VK_DPAD_ALTFIRE);
    edge(P_START, fabgl::VirtualKey::VK_MENU_HOME, fabgl::VirtualKey::VK_DPAD_START);
    edge(P_SELECT, fabgl::VirtualKey::VK_MENU_BS, fabgl::VirtualKey::VK_DPAD_SELECT);
    auto raw = [&](uint32_t bit, fabgl::VirtualKey vk) { if (ch & bit) kbdPushData(vk, (cur & bit) != 0); };
    raw(P_X, fabgl::VirtualKey::VK_JOY_X);
    raw(P_Y, fabgl::VirtualKey::VK_JOY_Y);
    raw(P_L1, fabgl::VirtualKey::VK_JOY_Z);
    raw(P_R1, fabgl::VirtualKey::VK_JOY_C);
    raw(P_L2, fabgl::VirtualKey::VK_JOY_L2);
    raw(P_R2, fabgl::VirtualKey::VK_JOY_R2);
    gamepad1_bits.up = cur & P_UP;
    gamepad1_bits.down = cur & P_DOWN;
    gamepad1_bits.left = cur & P_LEFT;
    gamepad1_bits.right = cur & P_RIGHT;
    gamepad1_bits.a = cur & P_B;        // the board stores Circle in .a, Cross in .b
    gamepad1_bits.b = cur & P_A;
    gamepad1_bits.start = cur & P_START;
    gamepad1_bits.select = cur & P_SELECT;
}

// Pad 2: the second joystick's state, as the board's second NES pad (nespad_tick2).
void apply2(uint32_t cur) {
    if (Config::secondJoy == 3) return;
    gamepad2_bits.a = cur & P_A;
    gamepad2_bits.b = cur & P_B;
    gamepad2_bits.select = cur & P_SELECT;
    gamepad2_bits.start = cur & P_START;
    gamepad2_bits.up = cur & P_UP;
    gamepad2_bits.down = cur & P_DOWN;
    gamepad2_bits.left = cur & P_LEFT;
    gamepad2_bits.right = cur & P_RIGHT;
}

// PICOSPECCY_PAD="frame:Btn[:N],..." holds pad 1's button Btn (Up Down Left Right A B
// X Y L1 R1 L2 R2 Start Select) from that presented frame for N frames (default 6):
// pad tests without a controller, on the same clock as PICOSPECCY_KEYS / _SHOTS.
struct ScriptedPress { uint32_t at, hold, bit; };
std::vector<ScriptedPress> s_script;

void parse_script() {
    const char* spec = getenv("PICOSPECCY_PAD");
    if (!spec) return;
    static const struct { const char* name; uint32_t bit; } kNames[] = {
        {"Up", P_UP}, {"Down", P_DOWN}, {"Left", P_LEFT}, {"Right", P_RIGHT},
        {"A", P_A}, {"B", P_B}, {"X", P_X}, {"Y", P_Y}, {"L1", P_L1}, {"R1", P_R1},
        {"L2", P_L2}, {"R2", P_R2}, {"Start", P_START}, {"Select", P_SELECT},
    };
    std::string all(spec);
    size_t p = 0;
    while (p < all.size()) {
        size_t e = all.find(',', p);
        if (e == std::string::npos) e = all.size();
        const std::string item = all.substr(p, e - p);
        p = e + 1;
        const size_t c = item.find(':');
        if (c == std::string::npos) continue;
        const size_t c2 = item.find(':', c + 1);
        const std::string name = item.substr(c + 1, c2 == std::string::npos ? std::string::npos : c2 - c - 1);
        uint32_t bit = 0;
        for (const auto& n : kNames) if (name == n.name) bit = n.bit;
        if (!bit) { fprintf(stderr, "host: PICOSPECCY_PAD: unknown button '%s'\n", name.c_str()); continue; }
        const uint32_t hold = c2 != std::string::npos ? (uint32_t)atoi(item.c_str() + c2 + 1) : 6u;
        s_script.push_back({(uint32_t)atoi(item.c_str()), hold, bit});
    }
}

}  // namespace

// SDL thread: hot-plug events and one sample per presented frame.
void host_pads_event(const SDL_Event& e) {
    if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) reopen();
}
void host_pads_sample() {
    static bool parsed = false;
    if (!parsed) { parsed = true; parse_script(); }
    extern uint32_t host_presented;   // host_main.cpp
    const uint32_t f = host_presented;
    uint32_t scripted = 0;
    for (const ScriptedPress& s : s_script)
        if (f >= s.at && f < s.at + s.hold) scripted |= s.bit;
    for (int i = 0; i < 2; i++)
        s_state[i].store(sample(s_pad[i]) | (i == 0 ? scripted : 0u), std::memory_order_relaxed);
}

// core0 (repeat_me_for_input): turn changes into the firmware's pad events.
void host_pads_apply() {
    const uint32_t c1 = s_state[0].load(std::memory_order_relaxed);
    if (c1 != s_applied[0]) { apply1(c1, s_applied[0]); s_applied[0] = c1; }
    const uint32_t c2 = s_state[1].load(std::memory_order_relaxed);
    if (c2 != s_applied[1]) { apply2(c2); s_applied[1] = c2; }
}
