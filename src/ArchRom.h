// pico-speccy — the machine identity as two enums.
//
// Arch and romset used to live in Config as free-form std::strings compared with
// literals at ~400 sites; the strings survive only at the edges (NVS key=value file,
// .esp snapshot sidecars, OSD text). Both index spaces come from an X-macro, so an
// index can never drift from its on-disk spelling.
//
// Sentinels:
//  * A_LAST / R_LAST  — the "Last used" preference. Placed at COUNT so a table whose
//    final row is "Last used" can be indexed by the enum directly (the new-UI pref
//    tables and the classic menus both end with that row).
//  * A_NONE / R_NONE  — "not specified": requestMachine(arch, R_NONE) derives the
//    arch's default romset (the old empty-string argument), LoadSnapshot(.., A_NONE,
//    R_NONE) forces nothing. Never persisted.

#pragma once

#include <stdint.h>
#include <string>

#define NM_ARCH_TABLE(X) \
    X(A_48K,        "48K")    \
    X(A_128K,       "128K")   \
    X(A_PENT,       "Pentagon") \
    X(A_P512,       "P512")   \
    X(A_P1024,      "P1024")  \
    X(A_PROFI,      "Profi")  \
    X(A_KARABAS,    "Karabas")  \
    X(A_SCORP,      "Scorpion") \
    X(A_ALF,        "ALF")

// Third column = the human label. The second column is an on-disk spelling that
// must never change (NVS, .esp sidecars); the third is what a user reads, kept
// here rather than in UiStrings.h so the classic menus and the info pages get it
// too, and so a new romset cannot be added without one.
#define NM_ROMSET_TABLE(X)                                        \
    X(R_48K,            "48K",              "48K")                \
    X(R_48K_ES,         "48Kes",            "48K Spanish")        \
    X(R_48K_CS,         "48Kcs",            "Custom 48K")         \
    X(R_48K_BY,         "48Kby",            "Byte 48K")           \
    X(R_128K,           "128K",             "128K")               \
    X(R_128K_ES,        "128Kes",           "128K Spanish")       \
    X(R_PLUS2,          "+2",               "+2")                 \
    X(R_PLUS2_ES,       "+2es",             "+2 Spanish")         \
    X(R_ZX81P,          "ZX81+",            "ZX81+")              \
    X(R_128K_CS,        "128Kcs",           "Custom 128K")        \
    X(R_128K_BY,        "128Kby",           "Byte 128K")          \
    X(R_128K_BY_GLUK,   "128Kbg",           "Byte 128K+Gluk")     \
    X(R_PENT,           "128Kp",            "128K")               \
    X(R_PENT_GLUK,      "128Kpg",           "128K + Mr Gluk")     \
    X(R_PROFI,          "Profi",            "Original")           \
    X(R_PROFI_KAR,      "ProfiKarabas",     "ROMain")             \
    X(R_PROFI_PQ,       "ProfiPQ",          "PQDOS")              \
    X(R_PROFI_FT,       "ProfiKarabasFT",   "Flash Tool")         \
    X(R_PROFI_FDI,      "ProfiKarabasFDI",  "FDImage")            \
    X(R_SCORP,          "Scorp",            "ZS-256 Turbo (Yellow)")      \
    X(R_SCORP_GR,       "ScorpGr",          "ZS-256 Turbo+ (Green)")      \
    X(R_SCORP_GMX,      "ScorpGMX",         "ZS-256 Turbo+ & GMX")        \
    X(R_SCORP_1024,     "Scorp1024",        "ZS-1024 Turbo+")             \
    X(R_SCORP_PROF,     "ScorpProf",        "ZS-1024 + ProfROM")          \
    X(R_ALF1,           "ALF1",             "ALF cartridge")

#define NM_X_IDX(id, str) id,
#define NM_XR_IDX(id, str, ui) id,
enum ArchIdx   : uint8_t { NM_ARCH_TABLE(NM_X_IDX)    ARCH_COUNT,
                           A_LAST = ARCH_COUNT,
                           A_NONE = 0xFF };
enum RomsetIdx : uint8_t { NM_ROMSET_TABLE(NM_XR_IDX) ROMSET_COUNT,
                           R_LAST = ROMSET_COUNT,
                           R_NONE = 0xFF };
#undef NM_X_IDX
#undef NM_XR_IDX

#define NM_X_STR(id, str) str,
#define NM_XR_STR(id, str, ui) str,
#define NM_XR_UI(id, str, ui) ui,
inline constexpr const char* kArchName    [ARCH_COUNT]   = { NM_ARCH_TABLE(NM_X_STR)    };
inline constexpr const char* kRomsetName  [ROMSET_COUNT] = { NM_ROMSET_TABLE(NM_XR_STR) };
inline constexpr const char* kRomsetUiName[ROMSET_COUNT] = { NM_ROMSET_TABLE(NM_XR_UI)  };
#undef NM_X_STR
#undef NM_XR_STR
#undef NM_XR_UI

// *_LAST serializes as "Last" (the NVS pref value); *_NONE has no spelling — it is an
// argument sentinel, never persisted.
inline const char* archToStr(ArchIdx a) {
    if (a < ARCH_COUNT) return kArchName[a];
    return a == A_LAST ? "Last" : "";
}
inline const char* romsetToStr(RomsetIdx r) {
    if (r < ROMSET_COUNT) return kRomsetName[r];
    return r == R_LAST ? "Last" : "";
}

// For anything a user reads. romsetToStr() is the serialization spelling and
// leaks internals ("ProfiKarabasFDI", "128Kbg") — never show it in the UI.
inline const char* romsetDisplay(RomsetIdx r) {
    if (r < ROMSET_COUNT) return kRomsetUiName[r];
    return r == R_LAST ? "Last used" : "";
}

// Unknown/garbage input returns `def`, so a hand-edited NVS line can never put a
// non-table value into Config.
inline ArchIdx archFromStr(const std::string& s, ArchIdx def) {
    for (int i = 0; i < ARCH_COUNT; i++)
        if (s == kArchName[i]) return (ArchIdx)i;
    if (s == "Last") return A_LAST;
    return def;
}
inline RomsetIdx romsetFromStr(const std::string& s, RomsetIdx def) {
    for (int i = 0; i < ROMSET_COUNT; i++)
        if (s == kRomsetName[i]) return (RomsetIdx)i;
    if (s == "Last") return R_LAST;
    if (s == "ALF")  return R_ALF1;  // legacy spelling written by older firmware
    return def;
}

inline RomsetIdx defaultRomsetFor(ArchIdx a) {
    switch (a) {
        case A_48K:     return R_48K;
        case A_128K:    return R_128K;
        case A_PROFI:   return R_PROFI;
        case A_KARABAS: return R_PROFI_KAR;
        case A_SCORP:   return R_SCORP;
        case A_ALF:     return R_ALF1;
        default:        return R_PENT;   // Pentagon / P512 / P1024
    }
}

// Karabas is Profi hardware — it exists as a separate arch only so the Machine menu
// can offer "Profi" (stock ROM) and "Karabas" (the real board's four ROMSET slots) as
// two rows, the same way Byte is a romset over 48K/128K. The emulator core and Config
// only ever see A_PROFI: archCanon() folds the alias at every commit boundary, and
// archDisplay() recovers the UI-facing arch from the running romset.
inline ArchIdx archCanon(ArchIdx a) {
    return a == A_KARABAS ? A_PROFI : a;
}
inline bool isKarabasRomset(RomsetIdx r) {
    return r == R_PROFI_KAR || r == R_PROFI_PQ || r == R_PROFI_FT || r == R_PROFI_FDI;
}
inline ArchIdx archDisplay(ArchIdx a, RomsetIdx r) {
    return (a == A_PROFI && isKarabasRomset(r)) ? A_KARABAS : a;
}
