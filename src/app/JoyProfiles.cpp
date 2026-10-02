// pico-speccy — joystick profile file format (see JoyProfiles.h).

#include "JoyProfiles.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "drivers/input/fabutils.h"

namespace JoyProf {

using namespace fabgl;

// Every target the mapping page can assign (OSD::joyPickKey), plus the codes a
// default map and an older build may hold. The names are fabgl's without the VK_
// prefix, so they read as what they are and never depend on the enum's order.
struct VkName { uint16_t vk; const char* name; };
static const VkName kVk[] = {
    { VK_NONE, "NONE" },
    { VK_0, "0" }, { VK_1, "1" }, { VK_2, "2" }, { VK_3, "3" }, { VK_4, "4" },
    { VK_5, "5" }, { VK_6, "6" }, { VK_7, "7" }, { VK_8, "8" }, { VK_9, "9" },
    { VK_A, "A" }, { VK_B, "B" }, { VK_C, "C" }, { VK_D, "D" }, { VK_E, "E" },
    { VK_F, "F" }, { VK_G, "G" }, { VK_H, "H" }, { VK_I, "I" }, { VK_J, "J" },
    { VK_K, "K" }, { VK_L, "L" }, { VK_M, "M" }, { VK_N, "N" }, { VK_O, "O" },
    { VK_P, "P" }, { VK_Q, "Q" }, { VK_R, "R" }, { VK_S, "S" }, { VK_T, "T" },
    { VK_U, "U" }, { VK_V, "V" }, { VK_W, "W" }, { VK_X, "X" }, { VK_Y, "Y" },
    { VK_Z, "Z" },
    { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" },
    { VK_F5, "F5" }, { VK_F6, "F6" }, { VK_F7, "F7" }, { VK_F8, "F8" },
    { VK_F9, "F9" }, { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },
    { VK_PAUSE, "PAUSE" }, { VK_PRINTSCREEN, "PRINTSCREEN" },
    { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" }, { VK_UP, "UP" }, { VK_DOWN, "DOWN" },
    { VK_RETURN, "RETURN" }, { VK_LSHIFT, "LSHIFT" }, { VK_LCTRL, "LCTRL" },
    { VK_SPACE, "SPACE" }, { VK_BACKSPACE, "BACKSPACE" },
    { VK_KP_0, "KP_0" }, { VK_KP_PERIOD, "KP_PERIOD" },
    { VK_DPAD_LEFT, "DPAD_LEFT" }, { VK_DPAD_RIGHT, "DPAD_RIGHT" },
    { VK_DPAD_UP, "DPAD_UP" }, { VK_DPAD_DOWN, "DPAD_DOWN" },
    { VK_DPAD_FIRE, "DPAD_FIRE" }, { VK_DPAD_ALTFIRE, "DPAD_ALTFIRE" },
    { VK_DPAD_START, "DPAD_START" }, { VK_DPAD_SELECT, "DPAD_SELECT" },
    { VK_JOY_C, "JOY_C" }, { VK_JOY_X, "JOY_X" }, { VK_JOY_Y, "JOY_Y" },
    { VK_JOY_Z, "JOY_Z" }, { VK_JOY_L2, "JOY_L2" }, { VK_JOY_R2, "JOY_R2" },
};

const char* vkName(uint16_t vk, char* tmp) {
    for (const VkName& e : kVk) if (e.vk == vk) return e.name;
    snprintf(tmp, 8, "#%u", (unsigned)vk);
    return tmp;
}

bool vkFromName(const char* s, uint16_t& vk) {
    if (s[0] == '#') {
        char* end;
        const unsigned long v = strtoul(s + 1, &end, 10);
        if (end == s + 1 || *end || v > 0xFFFF) return false;
        vk = (uint16_t)v;
        return true;
    }
    for (const VkName& e : kVk) if (!strcmp(e.name, s)) { vk = e.vk; return true; }
    return false;
}

// Index = the JOY_* value (Config.h, static_assert'ed in Config.cpp).
static const char* const kType[] = { "Cursor", "Kempston", "Sinclair1", "Sinclair2", "Fuller" };
constexpr int kTypeN = (int)(sizeof(kType) / sizeof(kType[0]));

const char* typeName(uint8_t t) { return t < kTypeN ? kType[t] : nullptr; }

bool typeFromName(const char* s, uint8_t& t) {
    for (int i = 0; i < kTypeN; i++) if (!strcmp(kType[i], s)) { t = (uint8_t)i; return true; }
    return false;
}

bool setName(Profile& p, const char* name) {
    char tmp[NAME_LEN];
    size_t n = 0;
    for (const char* c = name; *c && n < NAME_LEN - 1; c++)
        if (*c != '\t' && *c != '\r' && *c != '\n') tmp[n++] = *c;
    while (n && tmp[n - 1] == ' ') n--;
    size_t b = 0;
    while (b < n && tmp[b] == ' ') b++;
    if (b == n) return false;
    memcpy(p.name, tmp + b, n - b);
    p.name[n - b] = 0;
    return true;
}

bool parseLine(const char* line, size_t len, Profile& out) {
    char buf[NAME_LEN + 16 + SLOTS * 16];
    if (len >= sizeof(buf)) return false;
    memcpy(buf, line, len);
    buf[len] = 0;
    if (len && buf[len - 1] == '\r') buf[--len] = 0;
    if (!len || buf[0] == '#') return false;

    char* tab1 = strchr(buf, '\t');
    if (!tab1) return false;
    *tab1 = 0;
    char* tab2 = strchr(tab1 + 1, '\t');
    if (!tab2) return false;
    *tab2 = 0;

    Profile p;
    memset(&p, 0, sizeof(p));
    if (!setName(p, buf)) return false;
    if (!typeFromName(tab1 + 1, p.type)) return false;

    // Targets: a missing or unknown one is NONE, so a shorter (or longer) list from
    // another build still yields a usable map.
    for (int i = 0; i < SLOTS; i++) p.map[i] = VK_NONE;
    char* s = tab2 + 1;
    for (int i = 0; i < SLOTS && *s; i++) {
        char* comma = strchr(s, ',');
        if (comma) *comma = 0;
        uint16_t vk;
        if (vkFromName(s, vk)) p.map[i] = vk;
        if (!comma) break;
        s = comma + 1;
    }
    out = p;
    return true;
}

size_t formatLine(const Profile& p, char* out, size_t cap) {
    const char* tn = typeName(p.type);
    if (!tn) tn = kType[1];
    int n = snprintf(out, cap, "%s\t%s\t", p.name, tn);
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t pos = (size_t)n;
    for (int i = 0; i < SLOTS; i++) {
        char tmp[8];
        n = snprintf(out + pos, cap - pos, "%s%s", i ? "," : "", vkName(p.map[i], tmp));
        if (n < 0 || (size_t)n >= cap - pos) return 0;
        pos += (size_t)n;
    }
    if (pos + 1 >= cap) return 0;
    out[pos++] = '\n';
    out[pos] = 0;
    return pos;
}

int find(const Profile* list, int n, const char* name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < n; i++) if (!strcmp(list[i].name, name)) return i;
    return -1;
}

const char kEmptySlotLine[] = "-\n";

bool isEmptySlotLine(const char* line, size_t len) {
    if (len && line[len - 1] == '\r') len--;
    return len == 1 && line[0] == '-';
}

} // namespace JoyProf
