// pico-speccy — named joystick profiles (type + pad mapping), stored as ONE text file
// beside wifi.cfg: CONFIG_DIR "/joystick.cfg".
//
// A profile is what the Joystick > Mapping page edits: the joystick TYPE (Cursor,
// Kempston, Sinclair 1/2, Fuller) and the 14 pad-control targets (Config::joydef).
// The two belong together — the same map means different things on different
// types — so they are saved and loaded as one.
//
// File format, one SLOT per line, '#' starts a comment:
//
//     name <TAB> type <TAB> t0,t1,...,t13          a profile
//     -                                             an empty slot
//
// The type is a WORD ("Kempston"), the targets are key NAMES ("SPACE", "DPAD_FIRE",
// "NONE"), never enum numbers: fabgl::VirtualKey is an enum, and a file of raw
// numbers would silently mean other keys once its order moves. A code with no name
// in the table is written as "#<n>" and read back as such, so nothing is lost.
// An unknown name reads as NONE and a malformed line is skipped, so a file written
// by another firmware never breaks the load.
//
// This file depends on nothing from the firmware but the VirtualKey enum and the
// JOY_* type values, so tools/joyprofiles_test.cpp drives it on a host. The SD I/O
// lives in Config (Config::joyProfilesLoad / joyProfilesSave).
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace JoyProf {

constexpr int MAX      = 16;    // numbered slots (#01..#16); a slot may be empty
constexpr int NAME_LEN = 24;    // display name incl. NUL
constexpr int SLOTS    = 14;    // pad controls, = Config::joydef

struct Profile {
    char     name[NAME_LEN];
    uint8_t  type;              // JOY_* (Config.h)
    uint16_t map[SLOTS];        // fabgl::VirtualKey per pad control
};

// Key target <-> name. vkName writes into tmp (>= 8 bytes) when the code has no name.
const char* vkName(uint16_t vk, char* tmp);
bool        vkFromName(const char* s, uint16_t& vk);

// Joystick type <-> word. typeName returns nullptr for a value outside JOY_*.
const char* typeName(uint8_t t);
bool        typeFromName(const char* s, uint8_t& t);

// One line (no terminator). Returns false for a comment, a blank or a bad line.
bool parseLine(const char* line, size_t len, Profile& out);

// Formats one line WITH its '\n'. Returns the length, or 0 if it did not fit.
size_t formatLine(const Profile& p, char* out, size_t cap);

// Copies a user-typed name into dst, dropping the characters the format reserves
// (TAB, CR, LF) and trimming spaces. Returns false when nothing is left.
bool setName(Profile& p, const char* name);

// Index of the profile named `name` (exact match), or -1. An empty name matches
// nothing: an empty slot is a Profile whose name is "".
int find(const Profile* list, int n, const char* name);

// The profiles are numbered SLOTS, and the slot is the line's position among the
// profile lines. An empty slot below a used one is kept as a line holding just "-".
// (A file with no such lines — written by hand, or by an older build — fills slots
// 1..n in order.)
bool isEmptySlotLine(const char* line, size_t len);
extern const char kEmptySlotLine[];     // "-\n"

} // namespace JoyProf
