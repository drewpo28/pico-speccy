// pico-speccy — Description block content (see UiDesc.h).

#include "UiDesc.h"

#include <stdio.h>
#include <string.h>

#include "UiStage.h"
#include "app/Config.h"

extern uint32_t butter_psram_size();

namespace nm {

static int addLine(DescLine* out, int n, int max, uint8_t kind, const char* label,
                   const char* value, uint8_t state) {
    if (n >= max) return n;
    out[n].kind  = kind;
    out[n].state = state;
    snprintf(out[n].label, sizeof(out[n].label), "%s", label);
    snprintf(out[n].value, sizeof(out[n].value), "%s", value ? value : "");
    return n + 1;
}

// Subheader: the parameter and its current value ("now ..."), coloured by `st`.
static int sub(DescLine* out, int n, int max, const char* name, const char* now, uint8_t st) {
    return addLine(out, n, max, DESC_SUB, name, now, st);
}
// Rule: requirement + the condition it holds under ("" = always); `applies` greys
// the rules that are not the one in force.
static int rule(DescLine* out, int n, int max, const char* req, const char* cond,
                bool applies = true) {
    return addLine(out, n, max, DESC_RULE, req, cond, applies ? DESC_PLAIN : DESC_DIM);
}

static void mhzNow(char (&buf)[20], unsigned mhz) { snprintf(buf, sizeof(buf), "now %u MHz", mhz); }

// CPU >= minMhz, against the STAGED Overclock pick, so the block follows an edit made
// in this session before it is applied.
static int needCpu(DescLine* out, int n, int max, unsigned minMhz) {
    const unsigned mhz = (unsigned)Stage::get(SET_CPU_MHZ);
    char now[20], req[24];
    mhzNow(now, mhz);
    snprintf(req, sizeof(req), ">=%u MHz", minMhz);
    n = sub(out, n, max, "CPU", now, mhz >= minMhz ? DESC_OK : DESC_BAD);
    return rule(out, n, max, req, "");
}

// What picking a machine turns on and off by itself: the same rules as
// UiStage resolveConstraints / MachineSwitch / the CPU::reset backstops, written out
// for the user. A list, not a per-setting state: up to 2 + 3 short lines, comma-packed
// (a rule is at most 23 glyphs, 25 fit the narrowest pane with the indent).
struct MachFx { const char* on[2]; const char* off[3]; };

static const MachFx* machineFx(ArchIdx a, RomsetIdx r) {
    static const MachFx kTsconf = { { "Z-Controller, Betadisk" },
                                    { "Timex, esxDOS, MB-02+", "YM2413, SN76489", "16col, Murmuzavr" } };
    static const MachFx kEvo    = { { "Z-Controller, Betadisk" },
                                    { "Timex, esxDOS, MB-02+", "16col, Murmuzavr" } };
    static const MachFx kAtm    = { { "Betadisk" },
                                    { "Timex, esxDOS, MB-02+", "16col, Murmuzavr" } };
    static const MachFx kZcBeta = { { "Z-Controller, Betadisk" }, { "MB-02+" } };
    static const MachFx kBeta   = { { "Betadisk" }, { "MB-02+" } };
    static const MachFx kGmx    = { { "Betadisk" }, { "MB-02+, Timex" } };
    // ZiFi NIC goes off on Profi/Karabas only on boards without butter PSRAM
    // (MachineSwitch): the guest pages are SRAM there.
    static const MachFx kProfi  = { { "Betadisk" }, { "Timex, esxDOS, MB-02+" } };
    static const MachFx kKarab  = { { "Z-Controller, Betadisk" }, { "Timex, esxDOS, MB-02+" } };
    static const MachFx kProfiN = { { "Betadisk" },
                                    { "Timex, esxDOS, MB-02+", "ZiFi NIC" } };
    static const MachFx kKarabN = { { "Z-Controller, Betadisk" },
                                    { "Timex, esxDOS, MB-02+", "ZiFi NIC" } };
    const bool butter = butter_psram_size() != 0;
    static const MachFx kP3     = { { nullptr },
                                    { "Betadisk, Timex, MB-02+", "esxDOS, Z-Controller" } };
    static const MachFx kP3e    = { { "IDE: IDEDOS" },
                                    { "Betadisk, Timex, MB-02+", "esxDOS, Z-Controller", "ZiFi NIC" } };
    static const MachFx kP3div  = { { "IDE: DivIDE" },
                                    { "Betadisk, Timex, MB-02+", "esxDOS, Z-Controller" } };
    static const MachFx kTc2048 = { { "Timex Gfx mode" }, { "SAA1099" } };
    static const MachFx kTc2068 = { { "Timex Gfx mode" }, { "SAA1099, esxDOS" } };
    static const MachFx kByte48 = { { nullptr }, { "Betadisk, Timex, MB-02+" } };
    static const MachFx kByte   = { { nullptr }, { "Timex, MB-02+" } };

    if (a == A_TSCONF)                          return &kTsconf;
    if (a == A_ATM) return isEvoBaseRomset(r) ? &kEvo : &kAtm;
    if (a == A_KARABAS || (a == A_PROFI && isKarabasRomset(r)))
        return butter ? &kKarab : &kKarabN;
    if (a == A_PROFI) return butter ? &kProfi : &kProfiN;
    if (a == A_PENT || a == A_P512 || a == A_P1024) return &kBeta;
    if (a == A_SCORP) {
        if (isScorpEvoRomset(r)) return &kZcBeta;
        if (isScorpGmxRomset(r)) return &kGmx;
        return &kBeta;
    }
    if (isPlus3eRomset(r))   return &kP3e;
    if (isPlus3DivRomset(r)) return &kP3div;
    if (isPlus3Romset(r))    return &kP3;
    if (r == R_TC2068)       return &kTc2068;
    if (isTimexRomset(r))    return &kTc2048;
    if (r == R_48K_BY)       return &kByte48;
    if (r == R_128K_BY || r == R_128K_BY_GLUK) return &kByte;
    return nullptr;
}

static int machineEffects(DescLine* out, int n, int max, int32_t v) {
    const MachFx* fx = machineFx((ArchIdx)((v >> 8) & 0xFF), (RomsetIdx)(v & 0xFF));
    if (!fx) return n;
    if (fx->on[0]) {
        n = sub(out, n, max, "Turns on", "", DESC_OK);     // green
        for (const char* l : fx->on)  if (l) n = rule(out, n, max, l, "");
    }
    if (fx->off[0]) {
        n = sub(out, n, max, "Turns off", "", DESC_BAD);   // red
        for (const char* l : fx->off) if (l) n = rule(out, n, max, l, "");
    }
    return n;
}

static int describeMachine(int32_t v, DescLine* out, int max) {
    int n = 0;
    switch (v) {
        // ZX Evolution: TS-Conf (14 MHz Z80 + whole-line video) and BaseConf (14 MHz
        // ATM manager) both overrun the frame at the lower clocks.
        case NM_MACH(A_ATM,    R_EVO_BASE):
        case NM_MACH(A_TSCONF, R_TSCONF):
        case NM_MACH(A_TSCONF, R_TSCONF_GLUK):
            n = needCpu(out, n, max, 504);
            break;
        default: break;
    }
    return machineEffects(out, n, max, v);
}

// General Sound: the GS-Z80 runs on core1 beside the video ISR, so how fast it can be
// clocked without starving depends on the CPU clock. Both rules are listed, the one
// in force at the staged CPU clock judges the staged GS clock.
static int gsClockRules(DescLine* out, int n, int max) {
    static const uint8_t kGsMhz[] = { 12, 13, 14, 20, 24 };   // Config::gs_clock index
    const unsigned cpu = (unsigned)Stage::get(SET_CPU_MHZ);
    const unsigned gi  = (unsigned)Stage::get(SET_GS_CLOCK);
    const unsigned gs  = gi < sizeof(kGsMhz) ? kGsMhz[gi] : 0;
    const bool fast = cpu >= 504;
    const unsigned hi = fast ? 20 : 14;
    char now[20];
    mhzNow(now, gs);
    n = sub (out, n, max, "GS clock", now, (gs >= 12 && gs <= hi) ? DESC_OK : DESC_BAD);
    n = rule(out, n, max, "12-14 MHz", "CPU <504",  !fast);
    n = rule(out, n, max, "12-20 MHz", "CPU >=504",  fast);
    return n;
}

static int describeGs(int32_t v, DescLine* out, int max) {
    switch (v) {
        case 1:  return gsClockRules(out, 0, max);   // classic General Sound
        case 2:  return needCpu(out, 0, max, 504);   // NeoGS (clock is always Auto)
        default: return 0;
    }
}

// V-Sync paces one emulated frame per DISPLAY frame, so the emulation runs at the
// display's refresh: right on a 50 Hz mode, ~20% fast on 60 Hz.
#if HOST_BUILD
extern "C" unsigned host_display_hz(void);   // host_main.cpp: the monitor's refresh
#endif
static int describeVsync(int32_t v, DescLine* out, int max) {
    if (v != 1) return 0;
#if HOST_BUILD
    const unsigned hz = host_display_hz();       // the machine follows the monitor
    const char* what = "Monitor";
#else
    const unsigned hz = stagedVideoHz();
    const char* what = "Video mode";
#endif
    char now[20];
    snprintf(now, sizeof(now), "now %u Hz", hz);
    int n = sub (out, 0, max, what, now, hz == 50 ? DESC_OK : DESC_BAD);
    n = rule(out, n, max, "50 Hz", "normal speed", hz == 50);
    n = rule(out, n, max, "60 Hz", "runs faster",  hz != 50);
    return n;
}

// Video > Screen position: what the three numbers are (border rows above the paper,
// the paper, border rows below) and which raster lines of the Pentagon's 304 visible
// ones that keeps — lines 16..303 are visible on the real machine.
static int describeScreenTop(int32_t v, DescLine* out, int max) {
    if (v < 0 || v > 2) return 0;
    static const int kTop[3] = { 48, 60, 64 };
    const int top = kTop[v], bot = 96 - top;
    char a[24], b[24], c[24];
    int n = sub(out, 0, max, "Rows on screen", nullptr, DESC_PLAIN);
    snprintf(a, sizeof(a), "Top border %d", top);
    n = rule(out, n, max, a, "");
    n = rule(out, n, max, "Paper 192", "");
    snprintf(b, sizeof(b), "Bottom border %d", bot);
    n = rule(out, n, max, b, "");
    n = sub(out, n, max, "Pentagon lines", nullptr, DESC_PLAIN);
    snprintf(c, sizeof(c), "%d..%d of 16..303", 80 - top, 271 + bot);
    n = rule(out, n, max, c, "");
    snprintf(a, sizeof(a), "%d", 64 - top);
    n = rule(out, n, max, "Top lines cut", a, top != 64);
    snprintf(b, sizeof(b), "%d", 48 - bot);
    n = rule(out, n, max, "Bottom lines cut", b, bot != 48);
    return n;
}

// The machine the commit will end on: the staged pick, else the running one.
static void stagedMachine(ArchIdx& a, RomsetIdx& r) {
    const int32_t m = Stage::get(SET_MACHINE);
    if (m >= 0) { a = (ArchIdx)((m >> 8) & 0xFF); r = (RomsetIdx)(m & 0xFF); }
    else        { a = Config::arch; r = Config::romSet; }
}

// Subheader "Machine  now <short name>", red when `bad`.
static int machineSub(DescLine* out, int n, int max, ArchIdx a, RomsetIdx r, bool bad) {
    const char* fam = nullptr; const char* sh = nullptr;
    char now[20];
    if (machineMenuNameFor(a, r, fam, sh)) snprintf(now, sizeof(now), "now %s", sh);
    else                                    snprintf(now, sizeof(now), "now ?");
    return sub(out, n, max, "Machine", now, bad ? DESC_BAD : DESC_OK);
}

// Machines where Timex Gfx mode is forced off (UiStage resolveConstraints,
// MachineSwitch and the CPU::reset backstop): port #FF / the screen belong to the
// machine itself there.
static int timexMachines(DescLine* out, int max) {
    ArchIdx a; RomsetIdx r;
    stagedMachine(a, r);
    const bool tsAtm = (a == A_TSCONF || a == A_ATM);
    const bool profi = (a == A_PROFI || a == A_KARABAS);
    const bool other = isPlus3Romset(r) || isScorpGmxRomset(r) ||
                       r == R_48K_BY || r == R_128K_BY || r == R_128K_BY_GLUK;
    int n = machineSub(out, 0, max, a, r, tsAtm || profi || other);
    n = rule(out, n, max, "not TS-Conf, ATM",   "", tsAtm);
    n = rule(out, n, max, "not Profi, Karabas", "", profi);
    return rule(out, n, max, "not +3, Byte, GMX", "", other);
}

// SAA1099: off on the Timex machines (their ULA is the SCLD on #FF), and it does not
// work on TS-Conf (owner, 2026-10-14) — the menu does not enforce that one yet.
static int saaMachines(DescLine* out, int max) {
    ArchIdx a; RomsetIdx r;
    stagedMachine(a, r);
    const bool ts = (a == A_TSCONF), tx = isTimexRomset(r);
    int n = machineSub(out, 0, max, a, r, ts || tx);
    n = rule(out, n, max, "not TS-Conf",       "", ts);
    return rule(out, n, max, "not TC2048/TC2068", "", tx);
}

// Timex Gfx mode and the SAA1099 both decode port #FF, so only one of them can be on
// (resolveConstraints turns the other off). Each row names the other one.
static int portClash(DescLine* out, int n, int max, const char* other, uint16_t otherSetting) {
    const bool on = Stage::get(otherSetting) != 0;
    n = sub (out, n, max, other, on ? "now On" : "now Off", on ? DESC_BAD : DESC_OK);
    return rule(out, n, max, "Off", "port #FF");
}

// Overclock: every row there (CPU, VREG, flash, PSRAM) is applied at the next boot,
// and a pick the chip cannot hold leaves it unable to start.
static int overclockWarn(DescLine* out, int max) {
    int n = sub (out, 0, max, "Warning", "", DESC_BAD);    // no value: the name goes red
    n = rule(out, n, max, "chip may not start", "");
    return rule(out, n, max, "after reboot", "");
}

int describe(const Node& n, int32_t value, DescLine* out, int max) {
    if (n.kind != K_RADIO && n.kind != K_BOOL) return 0;
    switch (n.setting) {
        case SET_MACHINE: return describeMachine(value, out, max);
        case SET_GS_MODE: return describeGs(value, out, max);
        case SET_VSYNC:   return describeVsync(value, out, max);
        case SET_SCREEN_TOP: return describeScreenTop(value, out, max);
        case SET_CPU_MHZ:
        case SET_VREG:
        case SET_FLASH_FREQ:
        case SET_PSRAM_FREQ: return overclockWarn(out, max);
        case SET_TIMEX:
            return value ? portClash(out, timexMachines(out, max), max, "SAA1099", SET_SAA1099) : 0;
        case SET_SAA1099:
            return value ? portClash(out, saaMachines(out, max), max, "Timex Gfx mode", SET_TIMEX) : 0;
        // The Clock row: the same rules, judged against the staged clock.
        case SET_GS_CLOCK: return gsClockRules(out, 0, max);
        default:          return 0;
    }
}

} // namespace nm
