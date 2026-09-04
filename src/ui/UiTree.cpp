// pico-speccy — the declarative menu tree. DATA ONLY.
//
// Row order here is presentation only: a value row stores Option::value, never its
// index, and a conditional row is hidden by visible() instead of being spliced out of a
// string. That is what makes this tree safe to reorder — unlike the classic menu, where
// the row order *is* the stored enum (Config::x = opt2 - 1, ~200 sites).
//
// Leaves reach the existing modal dialogs through UiActions.cpp.

#include "OSDNewMenu.h"


#include "UiModel.h"
#include "UiStage.h"
#include "UiActions.h"
#include "UiStrings.h"
#include "UiRender.h"   // SYM_* glyphs for the persist verb lists
#include "Config.h"
#include "FileUtils.h"
#include "MemESP.h"          // butter_psram_size() for the Profi / ext-RAM predicates
#include "psram_spi.h"       // psram_size()
#include "Buffer.h"          // Buffer::gsPsramAvailable() for the General Sound gate
#include "BoardPins.h"       // the ESP-link predicate of the Network rows
#include <hardware/vreg.h>   // VREG_VOLTAGE_* values used by the option table
#include <stdio.h>           // snprintf (murmuzavrTag)

#if defined(VGA_HDMI)
// Defined in drivers/vga-nextgen/vga.c; file scope, or inside namespace nm it would
// resolve to nm::SELECT_VGA and fail to link (same note as UiStage.cpp).
extern bool SELECT_VGA;
#endif

namespace nm {

// Two-space label indent for rows that belong to the row above (a machine's own
// options). Pure presentation — macro concatenation, the engine knows nothing.
#define NM_IND "  "

// ── shared predicates ──────────────────────────────────────────────────────────

static bool p_hasSD() { return FileUtils::fsMount; }

// ── option tables ──────────────────────────────────────────────────────────────
// Values are the ones actually stored in Config; display order is independent.

static const Option opt_alu[] = {
    { "Early", 0 },
    { "Late",  1 },
};
static const Option opt_frameskip[] = {          // Config::throtling, microseconds
    { "Off",  0 },
    { "1000", 1 },
    { "2000", 2 },
    { "3000", 3 },
};
static const Option opt_palette[] = {
    { "Pulsar",    0 },
    { "Alone",     1 },
    { "Grayscale", 2 },
    { "Mars",      3 },
    { "Ocean",     4 },
};
static const Option opt_scanlines[] = {
    { "Off",          0 },
    { "1 Darkest",    1 },
    { "2 Dark",       2 },
    { "3 Light",      3 },
    { "4 Lightest",   4 },
};
// CRT filter: gamma + phosphor tint + black lift, plus a vertical mask.
// 1..3 use a soft 4-pixel-pitch profile (smooth phosphor stripes); 4..6 a hard
// 2-pixel grille, which beats harder against a display's own upscaler — that moire
// is the slow left-to-right envelope a real tube's phosphor pitch produces, so on a
// non-integer-scaling monitor it often looks more natural than the softer profile.
// Default Off. With Scanlines on, either completes a full dot mask.
static const Option opt_crt[] = {
    { "Off",          0 },
    { "Soft",         1 },
    { "Medium",       2 },
    { "Strong",       3 },
    { "Grille soft",  4 },
    { "Grille med",   5 },
    { "Grille hard",  6 },
};
static const Option opt_secondjoy[] = {          // 1-based in Config, do not shift
    { "DPAD #1", 1 },
    { "DPAD #2", 2 },
    { "NUMPAD",  3 },
};
static const Option opt_kport[] = {              // port numbers, not indices
    { "0x1F", 0x1F },
    { "0x37", 0x37 },
    { "0x5F", 0x5F },
};

// ── Audio ──────────────────────────────────────────────────────────────────────
static const Option opt_ay_stereo[] = {
    { "ABC",  0 },
    { "ACB",  1 },
    { "Mono", 2 },
};
// Not NM_BOOL: "on" is 3 here (both chip-select schemes at once), not 1.
static const Option opt_turbosound[] = {
    { "Off",  0 },
    { "On",   3 },
};
static const Option opt_covox[] = {
    { "Off",   0 },
    { "#FB",   1 },
    { "#DD",   2 },
};
static const Option opt_soundrive[] = {          // classic row order was Auto/On/Off
    { "Auto (Profi only)", 2, "Auto" },
    { "On",                1 },
    { "Off",               0 },
};
static const Option opt_boost[] = {              // stored value is the gain itself
    { "+0",  0 }, { "+4",  4 }, { "+8",  8 }, { "+12", 12 },
    { "+16", 16 }, { "+32", 32 }, { "+64", 64 },
};
static const Option opt_audio_driver[] = {
    { "Auto",     0 },
    { "PWM",      1 },
    { "I2S",      2 },
    { "AY 595",   3 },
#if defined(VGA_HDMI)
    { "HDMI",     4 },
#endif
#ifdef PCM5122_I2S_DATA
    { "PCM5122",  5 },              // ZERO2's I2S DAC board (I2C-configured)
#endif
};
static const Option opt_video_mode[] = {      // values are the VM_* enum
    { "640x480 @60", 0 },
    { "640x480 @50", 1 },
    { "720x480 @60", 2 },
    { "720x576 @50", 3 },
};
static const Option opt_render[] = {
    { "Standard",    0 },
    { "Snow effect", 1 },
};
static const Option opt_gigascreen[] = {
    { "Off",  0 },
    { "On",   1 },
    { "Auto", 2 },
};
static const Option opt_dma[] = {
    { "Off",           0 },
    { "#0B MB-02+",    1 },
    { "#6B DATA-GEAR", 2 },
};

// ── Overclock: every value here is read once at boot ───────────────────────────
static const Option opt_cpu_mhz[] = {
    { "252 MHz", 252 },
    { "378 MHz", 378 },
    { "504 MHz", 504 },
};
static const Option opt_vreg[] = {               // VREG_VOLTAGE_* enum values
    { "1.15 V", VREG_VOLTAGE_1_15 }, { "1.20 V", VREG_VOLTAGE_1_20 },
    { "1.25 V", VREG_VOLTAGE_1_25 }, { "1.30 V", VREG_VOLTAGE_1_30 },
    { "1.35 V", VREG_VOLTAGE_1_35 }, { "1.40 V", VREG_VOLTAGE_1_40 },
    { "1.50 V", VREG_VOLTAGE_1_50 }, { "1.60 V", VREG_VOLTAGE_1_60 },
    { "1.65 V", VREG_VOLTAGE_1_65 }, { "1.70 V", VREG_VOLTAGE_1_70 },
    { "1.80 V", VREG_VOLTAGE_1_80 },
};
static const Option opt_flash_freq[] = {
    { "33 MHz", 33 }, { "66 MHz", 66 }, { "84 MHz", 84 },
    { "100 MHz", 100 }, { "133 MHz", 133 }, { "166 MHz", 166 },
};
static const Option opt_psram_freq[] = {
    { "66 MHz", 66 }, { "84 MHz", 84 }, { "100 MHz", 100 },
    { "133 MHz", 133 }, { "166 MHz", 166 },
};

// ── Machine ────────────────────────────────────────────────────────────────────
// Every machine row is a radio over ITS OWN ROM sets, all sharing SET_MACHINE. Since a
// machine and its ROM together are one value, only the running machine's row shows a
// marked option — the others show none, which is exactly the "one of these is active"
// reading we want. NM_MACH() spells the arch out in every entry, so a ROM cannot silently
// end up under the wrong machine.
//
// The classic menu reached the same result through two nested menus plus a "Byte" entry
// that quietly rewrites arch to 48K or 128K; here that is visible in the table.

static bool p_extRam()    { return butter_psram_size() || psram_size() > 0 || FileUtils::fsMount; }
static bool p_hasPsram()  { return butter_psram_size() || psram_size() > 0; }
// The Debug > PSRAM row asks about the CHIP, not the usable size: with the switch off
// butter_psram_size()/psram_size() report 0 all session, and a p_hasPsram() row would
// disappear as soon as it was used — with no way back short of a reflash.
static bool p_psramChip() { return butter_psram_probed() || psram_probed_size() > 0; }

// Profi needs PSRAM for the DS80 hires framebuffer, and DS80 itself only exists in the
// VGA/HDMI drivers (set_profi_ds80_mode is a stub on TFT/SOFTTV/TV), so on those builds
// the machine is pointless. Same two conditions as the classic menu's show_profi.
static bool p_showProfi() {
#if !defined(VGA_HDMI)
    return false;
#else
    return p_hasPsram();
#endif
}

// Rows that only make sense for the machine that is about to be running: the staged pick
// if the user just chose one, otherwise the live machine.
static bool p_profiActive() {
    const int32_t m = Stage::get(SET_MACHINE);
    if (m < 0) return Config::arch == A_PROFI;   // Config never holds A_KARABAS
    const int a = (m >> 8) & 0xFF;
    return a == A_PROFI || a == A_KARABAS;       // Karabas = Profi hardware
}
// Murmuzavr is a Pentagon extension (the #AFF7 plane latch on top of #7FFD paging), so
// its submenu is offered for the Pentagon family only — staged pick first, else the live
// machine, like the other *Active predicates.
static bool p_pentActive() {
    const int32_t m = Stage::get(SET_MACHINE);
    const int a = (m < 0) ? (int)Config::arch : ((m >> 8) & 0xFF);
    return a == A_PENT || a == A_P512 || a == A_P1024;
}
// Strictly Pentagon: a count cannot survive on another machine, so there is nothing to
// come back and switch off here. ESPectrum::setup clamps it at boot and the commit's
// resolveConstraints() forces it to Off whenever the staged machine is not Pentagon.
static bool p_murmAvail() { return FileUtils::fsMount && p_pentActive(); }

static bool p_byteActive() {
    const int32_t m = Stage::get(SET_MACHINE);
    if (m < 0) return Config::romSet == R_48K_BY || Config::romSet == R_128K_BY ||
                      Config::romSet == R_128K_BY_GLUK;
    const int r = m & 0xFF;
    return r == R_48K_BY || r == R_128K_BY || r == R_128K_BY_GLUK;
}

static const Option opt_mach_48[] = {
    { TXT_ROM_48K,        NM_MACH(A_48K, R_48K)    },
#if !NO_SPAIN_ROM_48k
    { TXT_ROM_48K_ES,     NM_MACH(A_48K, R_48K_ES),     TXT_ROM_48K_ES_S },
#endif
    { TXT_ROM_CUSTOM,     NM_MACH(A_48K, R_48K_CS) },
};
static const Option opt_mach_128[] = {
    { TXT_ROM_128K,       NM_MACH(A_128K, R_128K)     },
#if !NO_SPAIN_ROM_128k
    { TXT_ROM_128K_ES,    NM_MACH(A_128K, R_128K_ES),   TXT_ROM_128K_ES_S },
    { TXT_ROM_PLUS2,      NM_MACH(A_128K, R_PLUS2)    },
    { TXT_ROM_PLUS2_ES,   NM_MACH(A_128K, R_PLUS2_ES),  TXT_ROM_PLUS2_ES_S },
    { TXT_ROM_ZX81P,      NM_MACH(A_128K, R_ZX81P)    },
#endif
    // The +2A/+3 is a romset of the 128K machine, like the +2 (ArchRom.h): the
    // English v4.0 four-bank image. Everything +3-specific keys on this romset.
    { TXT_ROM_P3,         NM_MACH(A_128K, R_P3)       },
    // ...and the +3e is a romset of that: the same machine with Garry Lancaster's
    // replacement ROM, which carries IDEDOS and an 8-bit IDE interface on #xxEF.
    { TXT_ROM_P3E,        NM_MACH(A_128K, R_P3E), TXT_ROM_P3E_S },
    { TXT_ROM_CUSTOM,     NM_MACH(A_128K, R_128K_CS)  },
};
static const Option opt_mach_pent[] = {
    { TXT_ROM_PENT,       NM_MACH(A_PENT, R_PENT)      },
    { TXT_ROM_PENT_GLUK,  NM_MACH(A_PENT, R_PENT_GLUK), TXT_ROM_PENT_GLUK_S },
    { TXT_ROM_CUSTOM,     NM_MACH(A_PENT, R_128K_CS)   },
};
static const Option opt_mach_p512[] = {
    { TXT_ROM_PENT,       NM_MACH(A_P512, R_PENT)      },
    { TXT_ROM_PENT_GLUK,  NM_MACH(A_P512, R_PENT_GLUK), TXT_ROM_PENT_GLUK_S },
    { TXT_ROM_CUSTOM,     NM_MACH(A_P512, R_128K_CS)   },
};
static const Option opt_mach_p1024[] = {
    { TXT_ROM_PENT,       NM_MACH(A_P1024, R_PENT)      },
    { TXT_ROM_PENT_GLUK,  NM_MACH(A_P1024, R_PENT_GLUK), TXT_ROM_PENT_GLUK_S },
    { TXT_ROM_CUSTOM,     NM_MACH(A_P1024, R_128K_CS)   },
};
// Byte is not an arch of its own: it is a ROM set over 48K or 128K, which is why the
// entries below switch arch as well.
static const Option opt_mach_byte[] = {
    { TXT_ROM_BYTE_48,    NM_MACH(A_48K,  R_48K_BY)  },
    { TXT_ROM_BYTE_128,   NM_MACH(A_128K, R_128K_BY) },
    { TXT_ROM_BYTE_GLUK,  NM_MACH(A_128K, R_128K_BY_GLUK), TXT_ROM_BYTE_GLUK_S },
};
// Profi and Karabas are two Machine rows over the same Profi hardware (like Byte over
// 48K/128K): Profi = the stock ROM, Karabas = the real board's four ROMSET slots
// (ROMain / PQDOS BIOS / Flash Tool / FDImage). The Karabas entries spell A_KARABAS so
// its row shows the mark; MachineSwitch folds it back to A_PROFI on commit (ArchRom.h).
static const Option opt_mach_profi[] = {
    { TXT_ROM_PROFI_ORIG, NM_MACH(A_PROFI, R_PROFI)       },
};
static const Option opt_mach_karabas[] = {
    { TXT_ROM_KAR_MAIN,   NM_MACH(A_KARABAS, R_PROFI_KAR) },
    { TXT_ROM_KAR_PQ,     NM_MACH(A_KARABAS, R_PROFI_PQ)  },
    { TXT_ROM_KAR_FT,     NM_MACH(A_KARABAS, R_PROFI_FT)  },
    { TXT_ROM_KAR_FDI,    NM_MACH(A_KARABAS, R_PROFI_FDI) },
};
static const Option opt_mach_scorp[] = {
    { TXT_ROM_SCORP,      NM_MACH(A_SCORP, R_SCORP),      TXT_ROM_SCORP_S      },
    { TXT_ROM_SCORP_GR,   NM_MACH(A_SCORP, R_SCORP_GR),   TXT_ROM_SCORP_GR_S   },
#if GMX_IN_FLASH   // escape-hatch builds carry no GMX ROM (CMakeLists); with the ROM
                   // in, a butter-less module reverts the pick in resolveConstraints
    { TXT_ROM_SCORP_GMX,  NM_MACH(A_SCORP, R_SCORP_GMX),  TXT_ROM_SCORP_GMX_S  },
#endif
    { TXT_ROM_SCORP_1024, NM_MACH(A_SCORP, R_SCORP_1024), TXT_ROM_SCORP_1024_S },
#if PROFROM_IN_FLASH
    { TXT_ROM_SCORP_PROF, NM_MACH(A_SCORP, R_SCORP_PROF), TXT_ROM_SCORP_PROF_S },
#endif
};
static const Option opt_mach_alf[] = {
    { TXT_ROM_ALF,        NM_MACH(A_ALF, R_ALF1) },
};

// Murmuzavr mode is the extended page count, not a machine — values are page counts, and
// MEM_PG_CNT == 64 is the "no extra RAM" state. The pages live in PSRAM as far as the
// budget reaches (Buffer::pageBudgetButter) and in the SD swap file beyond it.
static const Option opt_murmuzavr[] = {
    { "Off",   64   },
    { "4 MB",  256  },
    { "8 MB",  512  },
    { "16 MB", 1024 },
    { "32 MB", 2048 },
};

const char* murmuzavrTag() {
    // Only where the mode actually applies: the persisted pick survives a switch to
    // another machine, but ESPectrum::setup clamps the live count off there, so showing
    // the tag would advertise RAM the machine does not have.
    if (!p_pentActive()) return nullptr;
    // The STAGED count, so the subheader tracks the pick right away (the live MEM_PG_CNT
    // only follows on the reboot the commit asks for).
    const int32_t pg = Stage::get(SET_MEM_PG_CNT);
    if (pg <= 64) return nullptr;
    static char buf[12];
    snprintf(buf, sizeof(buf), "MZ[%dMB]", (int)(pg / 64));   // 64 pages of 16 KB = 1 MB
    return buf;
}

// Its own level rather than a radio row wedged between the machine rows: the page count
// is not a machine, and the extra depth is where a Murmuzavr-only option belongs.
static const Node kMurmuzavr[] = {
    NM_RADIO(TXT_MACH_MURM_SIZE, SET_MEM_PG_CNT, opt_murmuzavr, nullptr),
};

static const Node kMachine[] = {
    NM_RADIO(TXT_MACH_48K,   SET_MACHINE, opt_mach_48,    nullptr),
    NM_RADIO(TXT_MACH_128K,  SET_MACHINE, opt_mach_128,   nullptr),
    NM_RADIO(TXT_MACH_PENT,  SET_MACHINE, opt_mach_pent,  nullptr),
    NM_RADIO(TXT_MACH_P512,  SET_MACHINE, opt_mach_p512,  p_extRam),
    NM_RADIO(TXT_MACH_P1024, SET_MACHINE, opt_mach_p1024, p_extRam),
    // Machine-dependent options sit right under their machine, indented so the
    // grouping reads at a glance (they also only show while that machine is
    // running or staged) — Murmuzavr belongs to the three Pentagon rows above it.
    NM_SUB  (NM_IND TXT_MACH_MURM, kMurmuzavr, p_murmAvail),
    // Scorpion sits with the Soviet-clone block, right after the Pentagons.
    // Its pages above the base 128K need extended-RAM backing, same gate as P512.
    NM_RADIO(TXT_MACH_SCORP, SET_MACHINE, opt_mach_scorp, p_extRam),
    NM_RADIO(TXT_MACH_BYTE,  SET_MACHINE, opt_mach_byte,  p_extRam),
    NM_BOOL (NM_IND TXT_MACH_COBMECT, SET_BYTE_COBMECT, p_byteActive),
    NM_RADIO(TXT_MACH_PROFI,   SET_MACHINE, opt_mach_profi,   p_showProfi),
    NM_RADIO(TXT_MACH_KARABAS, SET_MACHINE, opt_mach_karabas, p_showProfi),
    NM_RADIO(TXT_MACH_ALF,   SET_MACHINE, opt_mach_alf,   nullptr),
    // Not a machine, but it lives with them by request: the built-in game — the
    // one "machine" that needs neither ROM nor SD card. Also reachable by
    // holding S during the boot R/M probe window.
    NM_PAGE (TXT_GAME,       act_gameScwong, nullptr),
};

// ── Speed test ─────────────────────────────────────────────────────────────────
// A level of the menu, not a popup. Row order is presentation only — the arg is
// the classic st_opt (1=CPU 2=SRAM 3=PSRAM 4=SD 5=USB [6=NET] 6/7=All), so "All
// tests" leads without renumbering anything.
static const Node kSpeedTest[] = {
#if ZIFI_NET_CLIENT
    NM_ACTION_ARG("All tests", act_speedTestOne, 7, nullptr),
#else
    NM_ACTION_ARG("All tests", act_speedTestOne, 6, nullptr),
#endif
    NM_ACTION_ARG("CPU MIPS",  act_speedTestOne, 1, nullptr),
    NM_ACTION_ARG("SRAM R/W",  act_speedTestOne, 2, nullptr),
    NM_ACTION_ARG("PSRAM",     act_speedTestOne, 3, nullptr),
    NM_ACTION_ARG("SD card",   act_speedTestOne, 4, nullptr),
    NM_ACTION_ARG("USB drive", act_speedTestOne, 5, nullptr),
#if ZIFI_NET_CLIENT
    NM_ACTION_ARG("Network",   act_speedTestOne, 6, nullptr),
#endif
};

// ── Help ───────────────────────────────────────────────────────────────────────
// The hardware-info pages used to be a top-level item of their own. They are read-only
// pages you visit to answer a question, which is what the rest of this branch is, so they
// live here now — the four help pages first, the six diagnostics after. Speed test is the
// odd one out (it actually writes to the card), but it belongs with the memory and board
// numbers it exists to explain.
static const Node kHelp[] = {
    NM_PAGE  (TXT_HELP_KEYS,  act_helpHotkeys,    nullptr),
    NM_PAGE  (TXT_HELP_REMAP, act_helpRemapInfo,  nullptr),
    NM_PAGE  (TXT_HELP_ZXKBD, act_helpZxKeyboard, nullptr),
    NM_PAGE  (TXT_HELP_ABOUT, act_helpAbout,      nullptr),
    NM_PAGE  (TXT_INFO_CHIP,   act_chipInfo,     nullptr),
    NM_PAGE  (TXT_INFO_BOARD,  act_boardInfo,    nullptr),
    NM_PAGE  (TXT_INFO_MEMORY, act_memoryInfo,   nullptr),
    NM_PAGE  (TXT_INFO_EMU,    act_emulatorInfo, nullptr),
    NM_PAGE  (TXT_INFO_HID,    act_hidDevices,   nullptr),
    NM_SUB   (TXT_INFO_SPEED,  kSpeedTest,       nullptr),
};

// ── Storage ────────────────────────────────────────────────────────────────────
static const Option opt_player[] = {
    { "Emulated", 0 },
    { "Player",   1 },
};
static const Option opt_sndled[] = {             // shared by Betadisk and MB-02+
    { "Off",          0 },
    { "LED",          1 },
    { "Sound",        2 },
    { "Sound + LED",  3 },
};
static const Option opt_trdos_rom[] = {
    { "5.03",        0 },
    { "5.04TM",      1 },
    { "5.05D",       2 },
    { "Custom",      3 },
};

static const Node kTape[] = {
    NM_ACTION(TXT_TAPE_SELECT,    act_tapeSelect,   p_hasSD),
    NM_ACTION(TXT_TAPE_PLAYSTOP,  act_tapePlayStop, nullptr),
    NM_ACTION(TXT_TAPE_BROWSER,   act_tapeBrowser,  nullptr),
    NM_RADIO (TXT_TAPE_PLAYER,    SET_TAPE_PLAYER,  opt_player, nullptr),
    NM_BOOL  (TXT_TAPE_FASTLOAD,  SET_FLASHLOAD,    nullptr),
    NM_BOOL  (TXT_TAPE_RG,        SET_TAPE_RG,      nullptr),
    NM_BOOL  (TXT_TAPE_AUTOSTART, SET_TAPE_ASTART,  nullptr),
};

// Hot keys level verbs.
static const Option opt_hotkey_hints[] = {
    { SYM_ENTER " Assign a key", 0 },
    { "F6  All defaults", 0 },
    { "F8  Clear", 0 },
};

// IDE slots carry their own verbs: a HDD has no write-protect, F2 edits the CHS
// override instead (see UiActions ideEditChs).
static const Option opt_ide_slot_hints[] = {
    { SYM_ENTER " Mount an image", 0 },
    { "F2  Edit CHS", 0 },
    { "F8  Eject", 0 },
};

// The storage interfaces are flat rows of the Devices level: the feature row
// itself carries the on/off value, and its extra settings sit indented right
// below it — ALWAYS visible, but greyed out and inert until the feature is on
// (running or staged). The generic pattern for feature-owned options; the drive
// rows keep their own F2/F8 verbs and persistence.
static bool p_betaOn()  { return Stage::get(SET_BETADISK) != 0; }
// Images make sense for DivMMC (hd0) and DivIDE (hd0+hd1) only — DivSD talks to
// the real SD card, so the row stays greyed there just like when esxDOS is off.
static bool p_esxImages() {
    const int32_t v = Stage::get(SET_ESXDOS);
    return v == 1 || v == 2;
}
static bool p_mb02On()  { return Stage::get(SET_MB02) != 0; }
// The +3 disk rows follow the machine, not a toggle: the interface is part of the
// machine and cannot be turned off, so they appear whenever a +3 is staged or running.
// The +3 is the R_P3 romset of the 128K arch, so this keys on the romset (as
// p_byteActive does).
static bool p_plus3On() {
    const int32_t m = Stage::get(SET_MACHINE);
    if (m >= 0) return isPlus3Romset((RomsetIdx)(m & 0xFF));
    return Config::isPlus3();
}
// The +3e carries an IDE interface whether or not the scheme row has caught up yet
// (resolveConstraints only forces it at commit), so the image rows follow the machine
// there — the same rule the +3 disk rows use.
static bool p_ideOn()   { return Stage::get(SET_IDE_SCHEME) != 0; }

// IDE/HDD scheme: the value IS Config::ide_scheme. "IDEDOS" is not a card the user
// plugs in — it is the interface the +3e ROM drives, so the romset forces that value
// and forces it away again on any other machine (UiStage resolveConstraints). It is
// listed here so the row can display it rather than showing a blank radio.
static const Option opt_ide_scheme[] = {
    { "Off",   0 },
    { "NEMO",  1 },
    { "PROFI", 2 },
    { "IDEDOS",   3 },
    { "SMUC",  4 },
};


// ── Additional hardware ────────────────────────────────────────────────────────
static const Node kOverclock[] = {
    NM_RADIO(TXT_OC_CPU,   SET_CPU_MHZ,    opt_cpu_mhz,    nullptr),
    NM_RADIO(TXT_OC_VREG,  SET_VREG,       opt_vreg,       nullptr),
    NM_RADIO(TXT_OC_FLASH, SET_FLASH_FREQ, opt_flash_freq, nullptr),
    NM_RADIO(TXT_OC_PSRAM, SET_PSRAM_FREQ, opt_psram_freq, nullptr),
};
// esxDOS variants: the value IS Config::esxdos, so the row order is free.
static const Option opt_esxdos[] = {
    { "Off",    0 },
    { "DivMMC", 1 },
    { "DivIDE", 2 },
    { "DivSD",  3 },
};

// Storage and Devices used to be two top-level items, which split one question ("what is
// this machine plugged into?") across two menus — Betadisk sat in one and the Z-Controller
// that also drives disks in the other. Merged, storage first in the order the classic
// Storage menu used (MENU_STORAGE_MAIN), then everything else.
static const Node kHardware[] = {
    NM_SUB     (TXT_TAPE,          kTape,     nullptr),
    NM_BOOL    (TXT_BETA,          SET_BETADISK, nullptr),
    NM_DYN_EN  (NM_IND TXT_BETA_DRIVES,   slots_buildBeta, slots_keyBeta, p_hasSD, p_betaOn),
    NM_BOOL_EN (NM_IND TXT_BETA_FASTMODE, SET_TRDOS_FAST,     nullptr, p_betaOn),
    NM_RADIO_EN(NM_IND TXT_BETA_SNDLED,   SET_TRDOS_LED,      opt_sndled, nullptr, p_betaOn),
    NM_RADIO_EN(NM_IND TXT_BETA_ROM,      SET_TRDOS_ROM,      opt_trdos_rom, nullptr, p_betaOn),
    NM_BOOL_EN (NM_IND TXT_BETA_AUTOBOOT, SET_TRDOS_AUTOBOOT, nullptr, p_betaOn),
    NM_RADIO   (TXT_HW_ESXDOS,     SET_ESXDOS, opt_esxdos, p_hasSD),
    NM_DYN_EN  (NM_IND TXT_HW_ESX_IMAGES, slots_buildEsx, slots_keyEsx, p_hasSD, p_esxImages),
    NM_BOOL    (TXT_MB02,          SET_MB02,  nullptr),
    NM_DYN_EN  (NM_IND TXT_MB02_DRIVES,   slots_buildMb02, slots_keyMb02, p_hasSD, p_mb02On),
    NM_RADIO_EN(NM_IND TXT_MB02_SNDLED,   SET_MB02_LED,   opt_sndled, nullptr, p_mb02On),
    NM_DYN_EN  (TXT_P3_DRIVES,     slots_buildP3, slots_keyP3, p_hasSD, p_plus3On),
    NM_BOOL_EN (NM_IND TXT_P3_FASTDISK,   SET_P3_FASTDISK,  nullptr, p_plus3On),
    NM_BOOL_EN (NM_IND TXT_P3_SPEEDLOCK,  SET_P3_SPEEDLOCK, nullptr, p_plus3On),
    NM_BOOL    (TXT_HW_ZC,         SET_ZCONTROLLER, p_hasSD),
    NM_RADIO   (TXT_HW_IDE,        SET_IDE_SCHEME, opt_ide_scheme, p_hasSD),
    NM_DYNH_EN (NM_IND TXT_IDE_IMAGES,   slots_buildIde, slots_keyIde,
                opt_ide_slot_hints, p_hasSD, p_ideOn),
    NM_ACTION_EN(NM_IND TXT_IDE_CREATE,  act_ideCreate, p_hasSD, p_ideOn),
    NM_BOOL  (TXT_HW_RTC,        SET_RTC,     nullptr),
};

// ── Video > TFT panel ──────────────────────────────────────────────────────────
// TFT builds only. All four values are read once by the display init, hence the single
// "Apply and reboot?" the commit asks for them; "Restore defaults" is the driver's own
// starting point (landscape, BGR, no flips), which is the way back from a panel that
// came up mirrored or with inverted colours.
#if TFT
static const Node kTft[] = {
    NM_BOOL  (TXT_TFT_INVERT,   SET_TFT_INVERT, nullptr),
    NM_BOOL  (TXT_TFT_BGR,      SET_TFT_BGR,    nullptr),
    NM_BOOL  (TXT_TFT_FLIPX,    SET_TFT_FLIP_X, nullptr),
    NM_BOOL  (TXT_TFT_FLIPY,    SET_TFT_FLIP_Y, nullptr),
    NM_ACTION(TXT_TFT_DEFAULTS, act_tftDefaults, nullptr),
};
#endif

// ── Video ──────────────────────────────────────────────────────────────────────
static const Node kVideo[] = {
    NM_RADIO(TXT_VID_MODE,       SET_VIDEO_MODE, opt_video_mode, nullptr),
    NM_RADIO(TXT_VID_PALETTE,    SET_PALETTE,    opt_palette,    nullptr),
    NM_RADIO(TXT_VID_RENDER,     SET_RENDER,     opt_render,     nullptr),
    NM_RADIO(TXT_VID_SCANLINES,  SET_SCANLINES,  opt_scanlines,  nullptr),
    NM_RADIO(TXT_VID_CRT,        SET_CRT_FILTER, opt_crt,        nullptr),
    NM_BOOL (TXT_VID_VSYNC,      SET_VSYNC,      nullptr),
    NM_RADIO(TXT_VID_GIGASCREEN, SET_GIGASCREEN, opt_gigascreen, nullptr),
    NM_BOOL (TXT_VID_ULAPLUS,    SET_ULAPLUS,    nullptr),
    NM_BOOL (TXT_VID_TIMEX,      SET_TIMEX,      nullptr),
    NM_RADIO(TXT_VID_DMA,        SET_DMA,        opt_dma,        nullptr),
    NM_BOOL (TXT_VID_DITHER,     SET_HDMI_DITHER, nullptr),
    NM_BOOL (TXT_VID_16COL,      SET_16COL,      nullptr),
#if TFT
    NM_SUB  (TXT_VID_TFT,        kTft,           nullptr),
#endif
};

// ── Audio ──────────────────────────────────────────────────────────────────────
// Audio driver first, as it matters more than the rest; Volume comes next once the
// slider lands.
// General Sound needs somewhere to put its 2 MB of sample RAM: butter XIP PSRAM (fast) or,
// as a fallback, plain SPI PSRAM with room left over for the MemESP swap pool (slow path,
// ~30x, best-effort). One shared test with the classic menu's gs_avail.
static bool p_gsAvail() { return Buffer::gsPsramAvailable(); }
// The children read the STAGED mode so they light up as soon as the mode is switched.
// Clock is classic-GS only (NeoGS firmware picks its own clock via GSCFG0 CKSEL);
// the RAM pick is NeoGS only (classic GS stays on its 2 MB default).
static bool p_gsClassic() { return p_gsAvail() && Stage::get(SET_GS_MODE) == 1; }
static bool p_gsNeo()     { return p_gsAvail() && Stage::get(SET_GS_MODE) == 2; }
// Clock is two rows sharing one label — the classic table (12..24 MHz) and the
// NeoGS one (Auto + the four CKSEL rates) have no values in common. They are
// mutually exclusive by VISIBILITY rather than greyed like their neighbours,
// because both being visible would put two identical "Clock" lines on screen.
// Off still shows the classic row greyed, as before.
static bool p_gsClockCls() { return p_gsAvail() && Stage::get(SET_GS_MODE) != 2; }

// values ARE Config::gs_enabled. Built at runtime (NM_RADIO_D) because NeoGS is
// offered only where it can actually run: its 64 KB of physical pages 0+1 (the card
// firmware EXECUTES from there under NOROM) plus the 8 KB blank ROM page must be
// pointer-backed, and SPI PSRAM hands out no pointers — on a butter-less board those
// 72 KB, the DAC rings and the MP3 decoder all land on the heap, ~149 KB of it, and
// the framebuffer that VIDEO::Init allocates NEXT then fails. pico_malloc panics
// instead of returning NULL, so the board came up in an "Out of memory" reboot loop
// (m1p2, hw 2026-08-13). Classic GS costs 36 KB and stays offered everywhere.
static const Option* gs_modeOpts(uint8_t& cnt) {
    static Option opts[3];
    static uint8_t n = 0;
    if (!n) {
        opts[n++] = { "Off",           0, nullptr };
        opts[n++] = { "General Sound", 1, nullptr };
        if (butter_psram_size()) opts[n++] = { "NeoGS", 2, nullptr };
    }
    cnt = n;
    return opts;
}
// NeoGS RAM sizes fw 1.11 auto-detects; values ARE Config::gs_ram_size
// (1 = 1 MB is a classic-GS-only value, not offered here).
static const Option opt_gs_ram[] = {
    { "512 KB", 0 },
    { "2 MB",   2 },
    { "4 MB",   3 },
};

static const Option opt_sn_clock[] = {          // values are Config::sn_clock indices
    { "3.58 MHz", 0 },   // SMS / the VGM default
    { "2 MHz",    1 },   // Sega System 1/2 dual-SN arcades
    { "4 MHz",    2 },   // Super Locomotive etc.
};

static const Option opt_gs_clock[] = {          // values are Config::gs_clock indices
    { "12 MHz", 0 },
    { "13 MHz", 1 },
    { "14 MHz", 2 },
    { "20 MHz", 3 },
    { "24 MHz", 4 },
};

// NeoGS clock. "Auto" is the real behaviour — the card's firmware picks one of
// these four through GSCFG0 CKSEL. Forcing a lower one emulates a card clocked
// down: the 37.5 kHz DAC tick is a divider of the clock, so pitch and tempo do
// not move, only the firmware's T-state budget per sample. Worth doing because
// the emulated GS-Z80 needs the whole of core1 to sustain 24 MHz at 504 MHz and
// cannot reach it at 378 — a starved producer crackles, a slower card does not.
static const Option opt_ngs_clock[] = {         // values ARE Config::ngs_clock
    { "Auto (fw)", 0 },
    { "24 MHz",    1 },
    { "20 MHz",    2 },
    { "12 MHz",    3 },
    { "10 MHz",    4 },
};

// ── MIDI ───────────────────────────────────────────────────────────────────────
// The classic MIDI wizard (OSD::midiDialog) becomes flat rows: the mode is a plain
// staged radio, and its DLS follow-ups — where the bank lives and which bank it is —
// are indented rows greyed until the STAGED mode makes them meaningful, the same
// pattern as Betadisk's children in Hardware.
//
// Value 3 is missing on purpose: it was "Software MIDI" (the procedural SoftSynth),
// removed along with its preset row. The value is retired, never reused.
static const Option opt_midi_mode[] = {
    { "Off",           0 },
    { "AY",            1 },
    { "ShamaZX",       2 },
    { "DLS Wavetable", 4 },
};
static bool p_midiDls()  { return Stage::get(SET_MIDI_MODE) == 4; }
static const Option opt_midi_bank_hints[] = {
    { SYM_ENTER " Select / install", 0 },
};
// Where the ~1.6 MB bank lives. Only a butter-PSRAM board has a choice: SPI PSRAM is
// accessor-only and the engine reads samples through a raw pointer, so everywhere else
// the flash partition is the sole home and the row would be a lie. PSRAM reloads from
// SD each boot (a bank swap applies live); Flash survives reboots and a missing card
// but is written only at early boot, so switching to it costs one reboot.
static bool p_butterPsram() { return butter_psram_size() > 0; }
static const Option opt_midi_storage[] = {      // values ARE Config::midi_storage
    { "PSRAM", 0 },
    { "Flash", 1 },
};

// The chips only the DivMMC VGM-player plugin drives, grouped out of the
// native-Spectrum rows. "All" flips the whole card family at once.
static const Node kVgmChips[] = {
    NM_BOOL (TXT_VGM_ALL,        SET_VGM_ALL,      nullptr),
    NM_BOOL (TXT_AUD_OPL3,       SET_OPL3,         nullptr),
    NM_BOOL (TXT_AUD_OPLL,       SET_YM2413,       nullptr),
    NM_BOOL (TXT_AUD_CMS,        SET_CMS,          nullptr),
    NM_BOOL (TXT_AUD_SN,         SET_SN76489,      nullptr),
    NM_RADIO(NM_IND TXT_SN_CLOCK, SET_SN_CLOCK,    opt_sn_clock,   nullptr),
};

static const Node kAudio[] = {
    NM_RADIO(TXT_AUD_DRIVER,     SET_AUDIO_DRIVER, opt_audio_driver, nullptr),
    NM_BOOL (TXT_AUD_AY,         SET_AY48,         nullptr),
    NM_RADIO(TXT_AUD_AY_STEREO,  SET_AY_STEREO,    opt_ay_stereo,  nullptr),
    NM_RADIO(TXT_AUD_TURBOSOUND, SET_TURBOSOUND,   opt_turbosound, nullptr),
    NM_BOOL (TXT_AUD_TSFM,       SET_TSFM,         nullptr),
    // Chip-first naming ("YMF262 (OPL3)"), grouped by family: the SAA pair
    // together (single #FF chip vs the CMS/Game Blaster two-chip card), then
    // FM, then the SN pair, then the DACs.
    NM_BOOL (TXT_AUD_SAA,        SET_SAA1099,      nullptr),


    NM_SUB  (TXT_AUD_VGM,        kVgmChips,        nullptr),
    NM_RADIO(TXT_AUD_COVOX,      SET_COVOX,        opt_covox,      nullptr),
    NM_RADIO(TXT_AUD_SOUNDRIVE,  SET_SOUNDRIVE,    opt_soundrive,  nullptr),
    NM_RADIO   (TXT_AUD_MIDI,           SET_MIDI_MODE,   opt_midi_mode,   nullptr),
    NM_RADIO_EN(NM_IND TXT_MIDI_STORAGE, SET_MIDI_STORAGE, opt_midi_storage,
                p_butterPsram, p_midiDls),
    // The bank list + on-device .dls conversion act immediately (like the disk slots):
    // a bank pick can applyBankLive() or defer a flash write to the next boot.
    NM_DYNH_EN (NM_IND TXT_MIDI_BANK,   midi_buildBanks, midi_keyBanks,
                opt_midi_bank_hints, p_hasSD, p_midiDls),
    NM_RADIO_D (TXT_AUD_GS,          SET_GS_MODE,  gs_modeOpts,  p_gsAvail),
    NM_RADIO_EN(NM_IND TXT_GS_CLOCK, SET_GS_CLOCK,  opt_gs_clock,  p_gsClockCls, p_gsClassic),
    NM_RADIO   (NM_IND TXT_GS_CLOCK, SET_NGS_CLOCK, opt_ngs_clock, p_gsNeo),
    NM_RADIO_EN(NM_IND TXT_GS_RAM,   SET_GS_RAM,    opt_gs_ram,    p_gsAvail, p_gsNeo),
    NM_RADIO   (TXT_AUD_BOOST,       SET_AUDIO_BOOST, opt_boost,  nullptr),
};

// ── Joystick ───────────────────────────────────────────────────────────────────
static const Node kJoyPrefs[] = {
    NM_BOOL (TXT_JOY_CURSOR_AS,   SET_CURSOR_JOY,   nullptr),
    NM_BOOL (TXT_JOY_TO_CURSOR,   SET_JOY2CURSOR,   nullptr),
    NM_BOOL (TXT_JOY_TAB_FIRE,    SET_TAB_FIRE,     nullptr),
    NM_BOOL (TXT_JOY_RIGHT_ENTER, SET_RIGHT_SPACE,  nullptr),
    NM_BOOL (TXT_JOY_WASD,        SET_WASD,         nullptr),
    NM_RADIO(TXT_JOY_SECOND,      SET_SECOND_JOY,   opt_secondjoy, nullptr),
    NM_RADIO(TXT_JOY_KPORT,       SET_KEMPSTON_PORT, opt_kport,    nullptr),
};
// Values are the JOY_* defines from Config.h, so the display order is free.
static const Option opt_joy_type[] = {
    { "Cursor",     JOY_CURSOR    },
    { "Kempston",   JOY_KEMPSTON  },
    { "Sinclair 1", JOY_SINCLAIR1 },
    { "Sinclair 2", JOY_SINCLAIR2 },
    { "Fuller",     JOY_FULLER    },
};

static const Node kJoystick[] = {
    NM_RADIO (TXT_JOY_TYPE,    SET_JOY_TYPE,  opt_joy_type, nullptr),
    NM_ACTION(TXT_JOY_MAPPING, act_joyDialog, nullptr),
    NM_SUB   (TXT_JOY_PREFS,   kJoyPrefs,     nullptr),
};

// ── Options ────────────────────────────────────────────────────────────────────
// Preferred machine / ROM: what a cold boot loads. "Last used" defers to whatever was
// running, which is also the fallback for any value not in these tables.
// Index-aligned with kPrefArch[] in UiStage.cpp — keep the two in step.
static const Option opt_pref_arch[] = {
    { TXT_MACH_48K,   0 },
    { TXT_MACH_128K,  1 },
    { TXT_MACH_PENT,  2 },
    { TXT_MACH_P512,  3 },
    { TXT_MACH_P1024, 4 },
    { TXT_MACH_SCORP, 5 },
    { TXT_ROM_LAST,   6 },
};
static const Option opt_pref48[] = {
    { TXT_ROM_48K,     0 },
#if !NO_SPAIN_ROM_48k
    { TXT_ROM_48K_ES,  1 },
    { TXT_ROM_CUSTOM,  2 },
    { TXT_ROM_LAST,    3 },
#else
    { TXT_ROM_CUSTOM,  1 },
    { TXT_ROM_LAST,    2 },
#endif
};
static const Option opt_pref128[] = {
    { TXT_ROM_128K,     0 },
#if !NO_SPAIN_ROM_128k
    { TXT_ROM_128K_ES,  1 },
    { TXT_ROM_PLUS2,    2 },
    { TXT_ROM_PLUS2_ES, 3 },
    { TXT_ROM_ZX81P,    4 },
    { TXT_ROM_P3,       5 },
    { TXT_ROM_P3E,      6 },
    { TXT_ROM_CUSTOM,   7 },
    { TXT_ROM_LAST,     8 },
#else
    { TXT_ROM_P3,       1 },
    { TXT_ROM_P3E,      2 },
    { TXT_ROM_CUSTOM,   3 },
    { TXT_ROM_LAST,     4 },
#endif
};
static const Option opt_pref_pent[] = {
    { TXT_ROM_PENT_ORIG, 0 },
    { TXT_ROM_CUSTOM,    1 },
    { TXT_ROM_LAST,      2 },
};
// Values are indices into UiStage's kPrefScorp — 1024 sits BEFORE the
// conditional GMX entry so the indices are identical on both build variants.
static const Option opt_pref_scorp[] = {
    { TXT_ROM_SCORP,      0, TXT_ROM_SCORP_S      },
    { TXT_ROM_SCORP_GR,   1, TXT_ROM_SCORP_GR_S   },
    { TXT_ROM_SCORP_1024, 2, TXT_ROM_SCORP_1024_S },
    { TXT_ROM_SCORP_PROF, 3, TXT_ROM_SCORP_PROF_S },
#if GMX_IN_FLASH
    { TXT_ROM_SCORP_GMX,  4, TXT_ROM_SCORP_GMX_S  },
    { TXT_ROM_LAST,       5 },
#else
    { TXT_ROM_LAST,       4 },
#endif
};

static const Node kPrefRom[] = {
    NM_RADIO(TXT_MACH_48K,   SET_PREF_ROM_48,    opt_pref48,    nullptr),
    NM_RADIO(TXT_MACH_128K,  SET_PREF_ROM_128,   opt_pref128,   nullptr),
    NM_RADIO(TXT_MACH_PENT,  SET_PREF_ROM_PENT,  opt_pref_pent, nullptr),
    NM_RADIO(TXT_MACH_P512,  SET_PREF_ROM_P512,  opt_pref_pent, nullptr),
    NM_RADIO(TXT_MACH_P1024, SET_PREF_ROM_P1024, opt_pref_pent, nullptr),
    NM_RADIO(TXT_MACH_SCORP, SET_PREF_ROM_SCORP, opt_pref_scorp, nullptr),
};

// "Other" is gone: it held four unrelated rows behind one more keypress, so they sit at
// this level now. The LED pair came over from Devices for the same reason — they are
// preferences, not hardware you mount.
// The argument IS the arch index OSD::updateROM takes; in the classic menu it was the row
// position (opt2 - 1), so the list could not be reordered without flashing the wrong slot.
static const Node kReplaceRom[] = {
    NM_ACTION_ARG(TXT_ROM_SLOT_48,      act_replaceRom, 1, nullptr),
    NM_ACTION_ARG(TXT_ROM_SLOT_128,     act_replaceRom, 2, nullptr),
    NM_ACTION_ARG(TXT_ROM_SLOT_PENT,    act_replaceRom, 3, nullptr),
    NM_ACTION_ARG(TXT_ROM_SLOT_ALF,     act_replaceRom, 4, nullptr),
    NM_ACTION_ARG(TXT_ROM_SLOT_ALFCART, act_replaceRom, 5, nullptr),
    NM_ACTION_ARG(TXT_ROM_SLOT_TRDOS,   act_replaceRom, 6, nullptr),
    NM_ACTION_ARG(TXT_ROM_SLOT_PENT0,   act_replaceRom, 7, nullptr),
    NM_ACTION_ARG(TXT_ROM_SLOT_PENT1,   act_replaceRom, 8, nullptr),
};

// The VGA menu-palette row only means something while the VGA output is live —
// on HDMI (and the non-VGA builds) the menu always shows the full-depth palette.
static bool p_vgaOut() {
#if defined(VGA_HDMI)
    return SELECT_VGA;
#else
    return false;
#endif
}
static const Option opt_ui_theme[] = {
    { "Slate",       0 },     // the cool neutral scheme
    { "ZX Spectrum", 1 },     // the classic pico-spec menu colours (white/cyan/black)
};
// The solid/dithered pick only applies to the Slate theme — the ZX theme's colours
// sit on the VGA grid already, so it is solid on VGA whatever this says. Staged-first
// so the row greys out the moment the theme is switched.
static bool p_themeSlate() { return Stage::get(SET_UI_THEME) == 0; }
static const Option opt_ui_vga_pal[] = {
    { "Solid 2:2:2", 1 },     // on-grid twin: solid fills, coarser colours
    { "Dithered",    0 },     // full-depth scheme through the Bayer dither
};
static const Option opt_ui_corners[] = {
    { "Rounded", 1 },
    { "Square",  0 },
};

// ── Interface ──────────────────────────────────────────────────────────────────
// Everything about the firmware's own UI and indication, split out of Options
// (which stays machine preferences): menu look, hot keys, LED indication.
static const Node kInterface[] = {
    // Menu look: all three apply live (the corner switch redraws the chrome on the
    // spot, the theme and palette switches re-install the UI palette block, which
    // recolours the open menu instantly — the framebuffer stores palette indices).
    NM_RADIO   (TXT_OPT_THEME,        SET_UI_THEME,   opt_ui_theme,    nullptr),
    NM_RADIO_EN(NM_IND TXT_OPT_VGA_MENU_PAL, SET_UI_VGA_PAL, opt_ui_vga_pal, p_vgaOut, p_themeSlate),
    NM_RADIO (TXT_OPT_UI_CORNERS,   SET_UI_CORNERS, opt_ui_corners,  nullptr),
    NM_DYNH  (TXT_OTHER_HOTKEYS,    hotkeys_build, hotkeys_key, opt_hotkey_hints, nullptr),
    // LED indication is one group: the master toggle, its legend (a reference for
    // the indicators, so greyed while they are off) and the board's own SD LED,
    // which came over from Devices — it is indication, not an interface setting
    // of the machine.
    NM_BOOL     (TXT_HW_LED,           SET_LED_IND,   nullptr),
    // Always available: the legend is reference material, useful before you turn
    // the indicators on (to see what they will mean) as much as after.
    NM_ACTION   (NM_IND TXT_HW_LEGEND, act_ledLegend, nullptr),
    NM_BOOL     (NM_IND TXT_HW_SDLED,  SET_SD_LED,    nullptr),
};

static const Node kOptions[] = {
    NM_SUB   (TXT_HW_OVERCLOCK,     kOverclock,     nullptr),
    NM_RADIO (TXT_OPT_PREF_MACHINE, SET_PREF_ARCH,  opt_pref_arch, nullptr),
    NM_SUB   (TXT_OPT_PREF_ROM,     kPrefRom,       nullptr),
    NM_RADIO (TXT_OTHER_ALU,        SET_ALU_TIMING, opt_alu,       nullptr),
    NM_BOOL  (TXT_OTHER_ISSUE2,     SET_ISSUE2,     nullptr),
    NM_RADIO (TXT_OTHER_FRAMESKIP,  SET_FRAMESKIP,  opt_frameskip, nullptr),
    NM_SUB   (TXT_OPT_REPLACE_ROM,  kReplaceRom,    p_hasSD),
    NM_ACTION(TXT_OPT_UPDATE_FW,    act_updateFirmware, nullptr),
};

// ── Debug ──────────────────────────────────────────────────────────────────────
// Breakpoints, jump-to and NMI are NOT menu rows: they live inside the Debugger
// (F7/F8/Alt+F9 there), where they act on a visible code cursor instead of a blind
// address prompt.
// Per-chip calibration for the RP2350 temperature sensor: uncalibrated silicon,
// and per-chip offsets are real (one z0p2 unit reads ~55-60 C low with a
// verified-good 3.28 V reference — see chipTempX10 in OSDMain.cpp). The stored
// value IS the offset in whole °C, added to the reading at display time.
static const Option opt_tempOffset[] = {
    { "-60 C", -60 }, { "-55 C", -55 }, { "-50 C", -50 }, { "-45 C", -45 },
    { "-40 C", -40 }, { "-35 C", -35 }, { "-30 C", -30 }, { "-25 C", -25 },
    { "-20 C", -20 }, { "-15 C", -15 }, { "-10 C", -10 }, { "-5 C",   -5 },
    { "0 C",     0 }, { "+5 C",    5 }, { "+10 C",  10 }, { "+15 C",  15 },
    { "+20 C",  20 }, { "+25 C",  25 }, { "+30 C",  30 }, { "+35 C",  35 },
    { "+40 C",  40 }, { "+45 C",  45 }, { "+50 C",  50 }, { "+55 C",  55 },
    { "+60 C",  60 }, { "+65 C",  65 }, { "+70 C",  70 }, { "+75 C",  75 },
};

static const Node kDebug[] = {
    NM_ACTION(TXT_DBG_DIALOG, act_debugDialog, nullptr),
    NM_ACTION(TXT_DBG_POKE,   act_debugPoke,   nullptr),
    NM_BOOL  (TXT_DBG_LOG,    SET_DEBUG_LOG,   nullptr),
    // Testing aid: run the firmware as if the board had no PSRAM (see SET_PSRAM_ON).
    NM_BOOL  (TXT_DBG_PSRAM,  SET_PSRAM_ON,    p_psramChip),
    // Border-timing aid: No = the paper area is not rendered; the border state
    // machine paints through it, showing the border colour "under" the paper as
    // it would run on the raster (per-T-state — multicolour effects included).
    NM_BOOL  (TXT_DBG_PAPER,  SET_PAPER,       nullptr),
    NM_RADIO (TXT_DBG_TEMPOFF, SET_TEMP_OFFSET, opt_tempOffset, nullptr),
};

// ── Reset ──────────────────────────────────────────────────────────────────────
// All actions: they take effect immediately by definition, and three of them reboot.
// visible() replaces the classic index gymnastics around the MurmulatorOS row
// ((mos && opt2 == 5) || (!mos && opt2 == 4)).
static const Node kReset[] = {
    NM_ACTION(TXT_RESET_SOFT,     act_resetSoft,    nullptr),
    NM_ACTION(TXT_RESET_HARD,     act_resetHard,    nullptr),
    NM_ACTION(TXT_RESET_RPI,      act_resetBoard,   nullptr),
    NM_ACTION(TXT_RESET_MOS,      act_resetMOS,     p_mosPresent),
    NM_ACTION(TXT_RESET_FACTORY,  act_resetFactory, nullptr),
    NM_ACTION(TXT_RESET_SAVE_CFG, act_saveCustomCfg, nullptr),
    NM_ACTION(TXT_RESET_LOAD_CFG, act_loadCustomCfg, nullptr),
};

// ── Network ────────────────────────────────────────────────────────────────────
// UTC-12..UTC+14; the stored value IS the hour offset (Config::wifi_tz).
static const Option opt_tz[] = {
    { "UTC-12", -12 }, { "UTC-11", -11 }, { "UTC-10", -10 }, { "UTC-9", -9 },
    { "UTC-8",   -8 }, { "UTC-7",   -7 }, { "UTC-6",   -6 }, { "UTC-5", -5 },
    { "UTC-4",   -4 }, { "UTC-3",   -3 }, { "UTC-2",   -2 }, { "UTC-1", -1 },
    { "UTC+0",    0 }, { "UTC+1",    1 }, { "UTC+2",    2 }, { "UTC+3",  3 },
    { "UTC+4",    4 }, { "UTC+5",    5 }, { "UTC+6",    6 }, { "UTC+7",  7 },
    { "UTC+8",    8 }, { "UTC+9",    9 }, { "UTC+10",  10 }, { "UTC+11", 11 },
    { "UTC+12",  12 }, { "UTC+13",  13 }, { "UTC+14",  14 },
};
// The stored value is the baud rate itself, not an index.
static const Option opt_zifi_baud[] = {
    { "115200", 115200 },
    { "230400", 230400 },
    { "460800", 460800 },
    { "921600", 921600 },
};

// There is a path to the ESP at all: USB transport, or a resolvable GPIO pair.
// With Transport = Off scanning/connecting is meaningless — the WiFi row (and
// everything below it) greys out. Reads Config directly: the transport is
// F_PREVIEW, so the live value tracks the radio as it is edited.
static bool p_espLink() {
#if defined(KBDUSB)
    if (Config::zifi_transport == 1) return true;
#endif
    uint8_t tx, rx;
    return BoardPins::resolveZifiPins(Config::zifi_tx_pin, Config::zifi_rx_pin, tx, rx);
}
// Everything that needs the WiFi link is greyed while WiFi is off — the affordance
// says "turn WiFi on first" instead of a toast after the fact. Transport and baud
// stay editable regardless: the link is CONFIGURED before it is brought up.
static bool p_wifiOn() { return Config::wifi_enabled && p_espLink(); }

static const Node kNetwork[] = {
    NM_ACTIONV_EN(TXT_NET_WIFI, act_wifi, vl_wifi, nullptr, p_espLink),
    // The ESP link settings, shared by WiFi and the NIC — indented under WiFi.
    // Transport's option list is per-board (GPIO pairs), hence the runtime radio.
    NM_RADIO_D(NM_IND TXT_NET_TRANSPORT, SET_ZIFI_TRANSPORT, zifi_transportOpts, nullptr),
    NM_RADIO  (NM_IND TXT_NET_BAUD,      SET_ZIFI_BAUD,      opt_zifi_baud,     nullptr),
    NM_RADIO    (TXT_NET_TZ,   SET_WIFI_TZ, opt_tz, nullptr),
    NM_ACTION_EN(TXT_NET_SYNC, act_sntp, nullptr, p_wifiOn),
#if ZIFI_NET_CLIENT
    NM_ACTION_EN(TXT_NET_FTP,  act_ftpServer, nullptr, p_wifiOn),
    NM_ACTION_EN(TXT_NET_HTTP, act_httpTest,  nullptr, p_wifiOn),
#endif
    NM_BOOL_EN  (TXT_NET_NIC_SUB, SET_ZIFI_NIC, nullptr, p_wifiOn),
};

// Right-pane verb lists of the persist levels (NM_DYNH).
static const Option opt_persist_save_hints[] = {
    { SYM_ENTER " Save here", 0 },
    { "F6  Rename", 0 },
    { "F8  Remove", 0 },
    { "F4  = Enter", 0 },
};
static const Option opt_persist_load_hints[] = {
    { SYM_ENTER " Load", 0 },
    { "F6  Rename", 0 },
    { "F8  Remove", 0 },
    { "F3  = Enter", 0 },
};

// ── root ───────────────────────────────────────────────────────────────────────
// Spec order; Volume last so it is reachable by holding Down on a joystick.
static const Node kRoot[] = {
    NM_SUB   (TXT_HELP,      kHelp,     nullptr),
    NM_SUB   (TXT_MACHINE,   kMachine,  nullptr),
    NM_DYNH  (TXT_SNAP_SAVE, persist_build, persist_keySave, opt_persist_save_hints, p_hasSD),
    NM_DYNH  (TXT_SNAP_LOAD, persist_build, persist_keyLoad, opt_persist_load_hints, p_hasSD),
    NM_SUB   (TXT_HW,        kHardware, nullptr),
    NM_SUB   (TXT_VIDEO,     kVideo,    nullptr),
    NM_SUB   (TXT_AUDIO,     kAudio,    nullptr),
    NM_SUB   (TXT_JOYSTICK,  kJoystick, nullptr),
    NM_SUB   (TXT_OPTIONS,   kOptions,  nullptr),
    NM_SUB   (TXT_INTERFACE, kInterface, nullptr),
    NM_SUB   (TXT_NETWORK,   kNetwork,  nullptr),
    NM_SUB   (TXT_DEBUG,     kDebug,    nullptr),
    NM_SUB   (TXT_RESET,     kReset,    nullptr),
    NM_INT   (TXT_VOLUME,    SET_VOLUME, -16, 0, 1, nullptr),
};

// The disk hot key opens the menu straight on one of these, so the node has to be
// reachable by interface. Returning the node (not a hand-written path) means the row can
// move anywhere in the tree without this breaking — UiNav searches for it.
static const Node* findDyn(const Node* arr, uint8_t n, void (*bld)(DynRows&)) {
    for (uint8_t i = 0; i < n; i++)
        if (arr[i].kind == K_DYNAMIC && arr[i].build == bld) return &arr[i];
    return nullptr;
}

const Node* slotNodeFor(int iface) {
    // Matched by builder identity, not by row index: the row can move anywhere
    // (they are flat rows of the Devices level now).
    switch (iface) {
        case IFACE_BETA: return findDyn(kHardware, NM_COUNT(kHardware), slots_buildBeta);
        case IFACE_MB02: return findDyn(kHardware, NM_COUNT(kHardware), slots_buildMb02);
        case IFACE_ESX:  return findDyn(kHardware, NM_COUNT(kHardware), slots_buildEsx);
        case IFACE_PLUS3: return findDyn(kHardware, NM_COUNT(kHardware), slots_buildP3);
        default:         return nullptr;
    }
}

const Node* persistNodeFor(bool save) {
    // Both persist rows share persist_build as their builder, so identity has to
    // come from the rowkey (findDyn matches by builder and cannot tell them apart).
    void (*rk)(int32_t, uint8_t) = save ? persist_keySave : persist_keyLoad;
    for (uint8_t i = 0; i < NM_COUNT(kRoot); i++)
        if (kRoot[i].kind == K_DYNAMIC && kRoot[i].rowkey == rk) return &kRoot[i];
    return nullptr;
}

const Node* rootNodes()     { return kRoot; }
uint8_t     rootNodeCount() { return NM_COUNT(kRoot); }

} // namespace nm

