// Host test for src/app/JoyProfiles.{h,cpp} — the joystick profile file format.
//
//   g++ -O2 -Wall -Wextra -Isrc -Iexternal -Isrc/drivers/board
//       -o /tmp/joyprofiles_test tools/joyprofiles_test.cpp src/app/JoyProfiles.cpp
//   /tmp/joyprofiles_test
//
// Re-run after any change to JoyProfiles: a format bug does not misbehave by
// degrees, it silently swaps the user's keys on the next boot.

#include "app/JoyProfiles.h"
#include "drivers/input/fabutils.h"

#include <stdio.h>
#include <string.h>

using namespace fabgl;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static bool parse(const char* s, JoyProf::Profile& p) { return JoyProf::parseLine(s, strlen(s), p); }

int main() {
    // 1. Round trip of every VirtualKey value — named or not.
    for (unsigned v = 0; v < 400; v++) {
        char tmp[8];
        const char* n = JoyProf::vkName((uint16_t)v, tmp);
        uint16_t back = 0xFFFF;
        CHECK(JoyProf::vkFromName(n, back));
        CHECK(back == v);
    }
    // Named codes stay names (so the file survives an enum reorder).
    { char t[8]; CHECK(!strcmp(JoyProf::vkName(VK_SPACE, t), "SPACE")); }
    { char t[8]; CHECK(!strcmp(JoyProf::vkName(VK_DPAD_FIRE, t), "DPAD_FIRE")); }
    { uint16_t v; CHECK(!JoyProf::vkFromName("BOGUS", v)); CHECK(!JoyProf::vkFromName("#", v));
      CHECK(!JoyProf::vkFromName("#12x", v)); CHECK(!JoyProf::vkFromName("#70000", v)); }

    // 2. Types.
    for (uint8_t t = 0; t < 5; t++) {
        uint8_t b = 99;
        CHECK(JoyProf::typeName(t) && JoyProf::typeFromName(JoyProf::typeName(t), b) && b == t);
    }
    CHECK(JoyProf::typeName(5) == nullptr);

    // 3. Profile line round trip.
    JoyProf::Profile p;
    memset(&p, 0, sizeof(p));
    CHECK(JoyProf::setName(p, "  Elite  "));
    CHECK(!strcmp(p.name, "Elite"));
    p.type = 4;
    const uint16_t m[14] = { VK_DPAD_LEFT, VK_DPAD_RIGHT, VK_DPAD_UP, VK_DPAD_DOWN,
                             VK_DPAD_START, VK_NONE, VK_SPACE, VK_Q, VK_0, VK_JOY_X,
                             VK_F12, VK_LSHIFT, 377, VK_RETURN };
    memcpy(p.map, m, sizeof(m));
    char line[512];
    const size_t n = JoyProf::formatLine(p, line, sizeof(line));
    CHECK(n > 0 && line[n - 1] == '\n');
    printf("%s", line);
    JoyProf::Profile q;
    CHECK(JoyProf::parseLine(line, n - 1, q));
    CHECK(!strcmp(q.name, "Elite"));
    CHECK(q.type == 4);
    CHECK(!memcmp(q.map, m, sizeof(m)));
    // CRLF line.
    { char crlf[600]; snprintf(crlf, sizeof(crlf), "%.*s\r", (int)(n - 1), line);
      CHECK(parse(crlf, q) && !memcmp(q.map, m, sizeof(m))); }
    // Too small a buffer refuses rather than truncating.
    CHECK(JoyProf::formatLine(p, line, 20) == 0);

    // 4. Lenient reading.
    CHECK(!parse("", q));
    CHECK(!parse("# comment\tKempston\tA", q));
    CHECK(!parse("NoTabs", q));
    CHECK(!parse("Name\tKempston", q));            // no map field
    CHECK(!parse("Name\tJoystick\tA", q));         // unknown type
    CHECK(!parse("  \tKempston\tA", q));           // empty name
    CHECK(parse("Short\tCursor\tSPACE,WHAT,Q", q));
    CHECK(q.type == 0 && q.map[0] == VK_SPACE && q.map[1] == VK_NONE && q.map[2] == VK_Q);
    for (int i = 3; i < 14; i++) CHECK(q.map[i] == VK_NONE);
    CHECK(parse("Long\tKempston\tA,A,A,A,A,A,A,A,A,A,A,A,A,A,B,B,B", q));
    CHECK(q.map[13] == VK_A);

    // 5. Name rules.
    CHECK(JoyProf::setName(p, "a\tb\nc"));
    CHECK(!strcmp(p.name, "abc"));
    CHECK(!JoyProf::setName(p, "   "));
    CHECK(JoyProf::setName(p, "012345678901234567890123456789"));
    CHECK(strlen(p.name) == JoyProf::NAME_LEN - 1);

    // 6. find.
    JoyProf::Profile list[2];
    memset(list, 0, sizeof(list));
    JoyProf::setName(list[0], "One");
    JoyProf::setName(list[1], "Two");
    CHECK(JoyProf::find(list, 2, "Two") == 1);
    CHECK(JoyProf::find(list, 2, "two") == -1);
    // An empty slot (name "") is never found, whatever is asked for.
    list[0].name[0] = 0;
    CHECK(JoyProf::find(list, 2, "") == -1);
    CHECK(JoyProf::find(list, 2, "Two") == 1);

    // 7. The empty-slot line.
    CHECK(JoyProf::isEmptySlotLine("-", 1));
    CHECK(JoyProf::isEmptySlotLine("-\r", 2));
    CHECK(!JoyProf::isEmptySlotLine("", 0));
    CHECK(!JoyProf::isEmptySlotLine("--", 2));
    CHECK(!JoyProf::isEmptySlotLine("# -", 3));
    CHECK(!parse("-", q));                          // ...and it is not a profile
    CHECK(!strcmp(JoyProf::kEmptySlotLine, "-\n"));

    printf(fails ? "FAILED: %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
