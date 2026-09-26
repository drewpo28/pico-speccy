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
    X(A_ALF,        "ALF")      \
    X(A_TSCONF,     "TSconf")   \
    X(A_ATM,        "ATM")

// Third column = the human label. The second column is an on-disk spelling that
// must never change (NVS, .esp sidecars); the third is what a user reads, kept
// here rather than in UiStrings.h so the classic menus and the info pages get it
// too, and so a new romset cannot be added without one.
#define NM_ROMSET_TABLE(X)                                        \
    X(R_48K,            "48K",              "48K")                \
    X(R_48K_ES,         "48Kes",            "48K Spanish")        \
    X(R_48K_CS,         "48Kcs",            "Custom 48K")         \
    X(R_48K_BY,         "48Kby",            "Byte 48K")           \
    X(R_TC2048,         "TC2048",           "TC2048")             \
    X(R_TC2068,         "TC2068",           "TC2068")             \
    X(R_48K_DG89,       "48Kdg89",          "48K (Gama 89)")      \
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
    X(R_ALF1,           "ALF1",             "ALF cartridge")      \
    X(R_P3,             "P3",               "+3 v4.0")             \
    X(R_P3E,            "P3e",              "+3 (IDEDOS)")           \
    X(R_P3DIV,          "P3div",            "+3 (DivIDE)")      \
    X(R_TSCONF,         "TS-Conf",          "TS-BIOS + 128")              \
    X(R_TSCONF_GLUK,    "TS-Gluk",          "TS-BIOS + Mr Gluk")          \
    X(R_ATM1,           "ATM1",             "ATM-Turbo 1 (BIOS 1.04rs)")  \
    X(R_ATM2,           "ATM2",             "ATM-Turbo 2+ (BIOS 1.07.13)") \
    X(R_ATM2X,          "ATM2x",            "ATM-Turbo 2+ (xBIOS 1.37)")

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
    if (s == "P3")   return A_128K;  // the +3 was briefly an arch of its own (never released)
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
        case A_TSCONF:  return R_TSCONF;
        case A_ATM:     return R_ATM2;
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

// The +2A/+3 is a 128K-family machine and is offered exactly the way the +2 is: a
// romset under A_128K, not an arch. Everything that differs (four ROMs, #1FFD, the
// uPD765, the contention pattern, no floating bus) hangs off this romset — see
// Config::isPlus3() / Z80Ops::isP3.
// Timex TC2048: a ROMSET of the 48K machine, not an arch. MAME's tc2048_io is
// only #FE and #FF and its memory map is plain ROM 0000-3FFF / RAM 4000-FFFF at
// spec48 timing, so the only hardware difference from a 48K is the SCLD — which
// is why the whole machine costs a 37-byte ROM overlay and one forced flag.
inline bool isTc2048Romset(RomsetIdx r) { return r == R_TC2048; }
// Timex TC2068: the SAME 48K frame timing (224 T/line, 312 lines, 3.5 MHz — the
// TS2068's 60 Hz/262-line/3.528 MHz set is the only thing that separates the two
// machines, libspectrum timings.c), the same SCLD video, and on top of it the
// machine's own 16 KB HOME ROM, an 8 KB EX-ROM, the eight-slot horizontal MMU on
// port #F4 (src/Timex.cpp) and an AY-3-8912 on #F5/#F6. Still a romset of A_48K,
// because none of that changes the arch's timing or its RAM layout.
inline bool isTc2068Romset(RomsetIdx r) { return r == R_TC2068; }
// "Is the SCLD the machine's own ULA?" — both Timex romsets force Config::timex_video
// on and the SAA1099 off, and both take the SCLD #FF register unconditionally.
inline bool isTimexRomset(RomsetIdx r) { return r == R_TC2048 || r == R_TC2068; }

// Didaktik Gama 89 (Czechoslovak clone): a 48K machine whose ROM is the Sinclair one
// with a Czech character set and a Centronics driver written into its 0xFF tail — so
// it is a ROMSET of A_48K and nothing here keys on it yet. It has a predicate all the
// same, because "is this the Didaktik" is the question any future difference (its
// printer ports #1F/#5F, or the Gama's 80 KB of RAM) would have to ask, and a literal
// spread over five files is how the +3e's IDE scheme went wrong.
inline bool isDidaktikRomset(RomsetIdx r) { return r == R_48K_DG89; }

// "Is the GMX firmware live?" — one romset today (ProfRom GMX v5.44), but the question
// is asked from a dozen places (CPU::reset's g_scorp_gmx, the butter/traded fallbacks
// and the 128-page boundary in requestMachine, wantedPages, the boot check in
// ESPectrum::setup, resolveConstraints, Snapshot), so it stays a predicate: a second
// image of this family is a romset beside it, not a change to any of them. The v6.44
// build — the same tools drawn on the GMX extended 640x200x16 screen — shipped as
// R_SCORP_GMX6 for a day (2026-09-20) and was removed again; only requestMachine and
// gmxLiveBankTable() ever cared WHICH.
inline bool isScorpGmxRomset(RomsetIdx r) {
    return r == R_SCORP_GMX;
}

inline bool isPlus3Romset(RomsetIdx r) {
    return r == R_P3 || r == R_P3E || r == R_P3DIV;
}
// The +3e (Garry Lancaster's replacement ROM) is the same +3 hardware with IDEDOS in
// ROM, so it is a romset of the romset: everything above stays true, and on top of it
// the machine carries the "simple 8-bit" IDE interface on #xxEF (see Ports.cpp).
inline bool isPlus3eRomset(RomsetIdx r) {
    return r == R_P3E;
}
// The SAME IDEDOS ROM built for a divIDE card instead (the `div` build of p3eroms):
// still a +3 in every other respect, but the disk is reached over divIDE's #A3..#BF
// taskfile (DivideIde.h) on a 16-BIT bus — so its .hdf images are full-sector ones,
// where the +3e's 8-bit interface wants half-sector images. Kept separate from
// isPlus3eRomset() because the two differ in exactly that: which interface is fitted,
// and therefore which ports are claimed (the +3e's window collides with ZiFi, divIDE's
// with General Sound and the Profi CP/M FDC).
inline bool isPlus3DivRomset(RomsetIdx r) {
    return r == R_P3DIV;
}

// The ZX-Evo BIOS images are one romset family over the same machine: pages 0 and 1
// are byte-identical and only the 128 service ROM at page 2 changes (stock 128.rom
// or Mr Gluk). Anything that asks "is this TS-Conf?" by romset must accept both —
// the machine is identical in every other respect.
inline bool isTsconfRomset(RomsetIdx r) {
    return r == R_TSCONF || r == R_TSCONF_GLUK;
}

// MicroART ATM-Turbo. One arch, three romsets: the ATM-Turbo 1 (a different memory
// manager — #FE address-line latch, #FDFD, #7DFD palette) and two BIOS images of the
// ATM-Turbo 2+ (#xx77 system port, eight #xxF7 page registers, #FF palette). Both
// boards share the 48K frame (224 T x 312 lines, uncontended) and the video modes;
// see src/Atm.h. isAtm1Romset() is the one question that separates the two boards.
inline bool isAtmRomset(RomsetIdx r)  { return r == R_ATM1 || r == R_ATM2 || r == R_ATM2X; }
inline bool isAtm1Romset(RomsetIdx r) { return r == R_ATM1; }
