// pico-speccy — ZX Evolution BaseConf ports, NMI, FDD emulator trap, clock and font
// RAM. The memory manager underneath is Atm.cpp's; see EvoBase.h.
#include "EvoBase.h"
#include "AtmInternal.h"

#include <string.h>
#include "app/Buffer.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/PerfFdc.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/devices/disk/wd1793.h"
#include "speccy/devices/storage/IDE.h"
#include "speccy/devices/storage/RTC.h"
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"
#include "ui/LEDIndicators.h"
#include "ui/OSDMain.h"

uint8_t g_atm_fnt = 0;
bool    g_evo_contend = false;
extern "C" const unsigned char gb_rom_atm_font[];

namespace EvoBase {

using namespace Atm;

uint8_t  pEFF7 = 0;
bool     inNmi = false;
bool     nmiClrPending = false;
uint64_t intAckFrame = ~0ull;
uint8_t  fddMask = 0;
uint8_t  vgSys = 0;
bool     inTrdemu = false;
uint8_t* font = nullptr;   // 2 KB, allocated on the first Evo reset

static uint8_t s_nemoRt = 0, s_nemoWt = 0, s_nemoW1 = 0, s_nemoWlo = 0;   // NEMO #10 triggers
// #2F/#4F/#6F/#8F in shadow: four plain read/write registers (the "savelij" ports).
// EvoProfROM keeps its per-drive real/virtual flags in #2F: its driver init writes
// #FF (all real), "Mount on A/B" clears bit 7/6 (page 17 #15DC: OUT (#2F),D), and
// its TR-DOS (page 13 #0856...) reads the bits back to route drive A/B through the
// virtual-disk path. With #2F answering #FF a mounted virtual disk on A/B was sent
// to the WD1793 and TR-DOS said "Disc error"; with it aliased to the WD1793 track
// register every real disk went through the slow ProfROM driver. Power-up value #FF;
// survives a machine reset (see reset()).
static uint8_t s_sav[4] = {0xFF, 0xFF, 0xFF, 0xFF};
static inline int savIdx(uint8_t lo) {
    return (lo == 0x2F || lo == 0x4F || lo == 0x6F || lo == 0x8F) ? (lo >> 5) - 1 : -1;
}
#if EVO_CFG_TRACE
static uint16_t s_evo_tr_n = 0;   // evoTrace budget, re-armed by reset()
static int s_nmi_tr = 0;          // port accesses still to log after an NMI
#endif

// ------------------------------------------------------------------ reset --

void reset() {
    s_nemoRt = s_nemoWt = s_nemoW1 = 0;
    // s_sav is NOT reset: a warm reset (F11) keeps EvoProfROM's mount state in RAM and
    // its warm path does not rewrite #2F, so clearing the latch here made a mounted A:/B:
    // "real" again until a power cycle. #FF only at power-up (static init).
#if EVO_CFG_TRACE
    s_evo_tr_n = 0;
    Debug::log("[EVOP] reset");
#endif
    // zports.v reset: scr_mode 3 (ZX), turbo off, atm_pen = 1 (no manager: the last
    // ROM page in every window), atm_cpm_n = 0 (DOS signal held up), atm_pen2 = 0
    // (no palette through #FF) — i.e. #77 as if written with A14 = 1, A9 = A8 = 0.
    a77 = 0x4000;
    p77 = 3;
    aFE = 0xFE;
    aFB = 0;
    pFDFD = 0;
    pEFF7 = 0;
    inNmi = false;
    nmiClrPending = false;
    inTrdemu = false;
    fddMask = 0;
    vgSys = 0;
    intAckFrame = ~0ull;
    // "When you reset the computer installs the ROM palette in compatibility mode
    // with the ZX-Spectrum" (BaseConf reference, 6.2).
    loadSpecPalette();
    palDirty = false;
    VIDEO::atmPaletteRestore();
    // The font RAM is the FPGA's, initialised from atm.fnt at configuration time
    // (video_fontrom.v) — i.e. once per power-up, NOT by a reset.
    if (!font) {
        font = (uint8_t*)Buffer::palloc(2048, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (font) memcpy(font, gb_rom_atm_font, 2048);
    }
    clockApply();
}

// The plain Spectrum the BASIC rows of Alt+F11 promise — #EFF7 D2 (128K paging, D5
// of #7FFD locks again) and D4 (turbo off, 3.5 MHz).
void bootPlain() {
    pEFF7 = 0x14;
    clockApply();
}

// CPU clock (top.v: turbo = { #xx77 D3, ~#EFF7 D4 }; zclock.v: 1x = 14 MHz,
// 01 = 7 MHz, 00 = 3.5 MHz) — so the machine comes out of reset at 7 MHz (#EFF7 = 0).
// A change is a change of UNITS for CPU::tstates (the TS-Conf lesson).
// ── .pss snapshot ────────────────────────────────────────────────────────────
uint32_t snapSave(uint8_t* out) {
    uint32_t n = 0;
    out[n++] = pEFF7;
    out[n++] = (uint8_t)((inNmi ? 1 : 0) | (nmiClrPending ? 2 : 0) | (inTrdemu ? 4 : 0));
    out[n++] = fddMask;
    out[n++] = vgSys;
    for (int i = 0; i < 4; i++) out[n++] = s_sav[i];
    out[n++] = s_nemoRt; out[n++] = s_nemoWt; out[n++] = s_nemoW1; out[n++] = s_nemoWlo;
    out[n++] = g_atm_fnt;
    return n;   // 13
}

void snapLoad(const uint8_t* in, uint32_t n) {
    if (n < 13) return;
    pEFF7 = in[0];
    inNmi = in[1] & 1; nmiClrPending = (in[1] & 2) != 0; inTrdemu = (in[1] & 4) != 0;
    fddMask = in[2];
    vgSys = in[3];
    for (int i = 0; i < 4; i++) s_sav[i] = in[4 + i];
    s_nemoRt = in[8]; s_nemoWt = in[9]; s_nemoW1 = in[10]; s_nemoWlo = in[11];
    g_atm_fnt = (in[12] && font) ? 1 : 0;
}

// 48K/128K raster contention (zclock.v): the 6,5,4,3,2,1,0,0 pattern on #4000-#7FFF,
// and in the 128K raster also on #C000-#FFFF while #7FFD D0 is set — by ADDRESS and
// #7FFD, whatever the memory manager has mapped there. Only at 3.5 MHz (!int_turbo).
// mode_contend_type is tied to 0 in top.v, so the +2A/+3 pattern never occurs.
void contendApply() {
    g_evo_contend = evo && (Config::isEvo48Raster() || Config::isEvo128Raster()) &&
                    ESPectrum::multiplicator == 0;
    MemESP::ramContended[1] = g_evo_contend;
    MemESP::ramContended[3] = g_evo_contend && Config::isEvo128Raster() && (p7ffd & 1);
}

// Scroll Lock = the AVR's raster switch (avr/baseconf zx.c: case 0x7E cycles the
// MODES_RASTER|MODE_VGA bits, stored in the PCF8583 NVRAM). We cycle the raster alone —
// there is no TV/VGA output choice here — and persist it like the menu row does. The
// raster sets the frame, audio and display timing, which are derived at the reset.
void scrollLockRaster() {
    static const char* const kName[4] = { " Raster: Pentagon ", " Raster: 60 Hz ",
                                          " Raster: 48K ", " Raster: 128K " };
    Config::evo_raster = (uint8_t)((Config::evo_raster + 1) & 3);
    Config::save();
    ESPectrum::reset();
    OSD::notify(kName[Config::evo_raster]);
}

void clockApply() {
    const uint8_t want = (p77 & 0x08) ? 2 : ((pEFF7 & 0x10) ? 0 : 1);
    const uint8_t old = ESPectrum::multiplicator;
    if (want == old) return;
    CPU::tstates = (want > old) ? (CPU::tstates << (want - old)) : (CPU::tstates >> (old - want));
    ESPectrum::multiplicator = want;
    CPU::updateStatesInFrame();
    OSD::notifyClock(want == 2 ? " CPU: 14 MHz " : want ? " CPU: 7 MHz " : " CPU: 3.5 MHz ");
}

// -------------------------------------------------------------------- NMI --
// NMI (Print Screen / #BF D3 / a breakpoint): znmi.v holds in_nmi from the NMI until
// OUT (#BE), and atm_pager.v maps RAM page #FF into #0000-#3FFF meanwhile — so the
// #0066 the CPU jumps to is ERS's resident handler, not the running ROM.
void nmiClrApply() {
    nmiClrPending = false;
    if (inNmi) { inNmi = false; remap(); }
}

void nmiEnter() {
    if (!evo) return;
#if EVO_CFG_TRACE
    Debug::log("[NMI] RAM #FF in window 0, pc=%04X", Z80::getRegPC());
    s_nmi_tr = 300;
#endif
    inNmi = true;
    remap();
}

// ----------------------------------------------------------------- TR-DOS --

bool dosEnterOk() { return (pF7[4] & 0x300) == 0x100; }

// By the window's REGISTER, not by what is mapped — RAM #FE/#FF of the FDD emulator
// / NMI and #EFF7 RAM 0 override window 0 without dropping DOS.
bool ramExec(uint8_t w) {
    return (a77 & 0x100) && !(pF7[((p7ffd & 0x10) >> 2) + w] & 0x100);
}

// ------------------------------------------------------------------ ports --
// zports.v, port for port. `shadow` = dos || #BF D0. What is not taken here falls
// through to the generic decode (ULA #FE, AY, Kempston #1F noshad, Z-Controller
// #77/#57 noshad, NEMO IDE, GS, covox) — Ports.cpp keeps the Z-Controller #57
// with A15 = 1 in shadow mode for the card's CS (atmPortWriteEarly).

static void write7ffd(uint8_t data) {
    // The 48 lock only in the 128K mode (zports.v block7ffd = p7ffd[5] & block1m);
    // in the Pentagon-1024 mode D5 is a page bit.
    if ((p7ffd & 0x20) && !mode1M()) return;
    set7ffd(data);
}

static void write77(uint16_t address, uint8_t data) {
    const uint8_t oldMode = p77 & 7;
    a77 = address;
    p77 = data;
    // zdos.v: DOS drops on ANY M1 from a RAM window while /CPM = 1, not only on a
    // jump into one. An OUT that raises /CPM from RAM therefore drops DOS at the very
    // next fetch, before control leaves RAM — check_trdos only sees jump TARGETS, so
    // it would miss it. EVO Reset Service enters ProfROM exactly so: JP #BF5A (in
    // RAM, /CPM still 0) / OUT (#FF77),#A3 / RET to #0000 — with DOS left up the ROM's
    // DOS bit picked the service page 15 instead of page 14 (hw 2026-10-02).
    if (Atm::beta && !cpmOn() && (a77 & 0x100) && ramExec((uint8_t)(Z80::getRegPC() >> 14)))
        Atm::beta = false;
    dosRecalc();
    remap();
    clockApply();
    if ((data & 7) != oldMode) VIDEO::atmVideoModeChanged();
}

// #xxF7 with A8 = 1 outside shadow (#EFF7/#DFF7/#BFF7), A8 = 0 inside it (#EEF7 —
// abandoned in shadow mode — /#DEF7/#BEF7): "F7 ports are accessible in shadow mode
// but at addresses like EEF7, DEF7, BEF7 so that there are no conflicts with ATM xFF7
// and x7F7". Gluk access: #EFF7 D7 outside shadow, always inside it.
static void f7Write(uint16_t address, uint8_t data, bool sh) {
    if (!sh && !(address & 0x1000)) {                       // #EFF7 (noshad only)
        const uint8_t old = pEFF7;
        pEFF7 = data;
        if ((old ^ data) & 0x0C) remap();                    // D2 128K/1M mode, D3 RAM0
        if ((old ^ data) & 0x10) clockApply();               // D4 turbo off
        return;
    }
    if (!(sh || (pEFF7 & 0x80))) return;                     // Gluk ports off
    if (!(address & 0x2000))      RTC::selectReg(data);      // #DFF7 / #DEF7
    else if (!(address & 0x4000)) RTC::writeData(data);      // #BFF7 / #BEF7
}

// base_trdemu FDD emulation (zdos.v trdemu_on): any FDC port access (#1F/#3F/#5F/
// #7F/#FF) in shadow mode, with the drive #FF selects masked in #13BD, DOS up, ROM
// in the access window (window 0 here) and /PEN2 = 1 (#77 A14 = 1), maps RAM page
// #FE into window 0 — EVO Reset Service's resident emulator, which serves the disk
// image from the SD card and leaves with OUT (#BE). The WD1793 is not selected for
// a masked drive (zports.v vg_cs_n |= fdd_mask[vg_a]). DEVIATION: the hardware
// switches at the end of the access; we switch inside it, like TS-Conf's VDOS.
static bool trdemuTrap(uint8_t lo) {
    if (lo != 0x1F && lo != 0x3F && lo != 0x5F && lo != 0x7F && lo != 0xFF) return false;
    if (!(fddMask & (1u << (vgSys & 3)))) return false;
    if (!ESPectrum::trdos || !(a77 & 0x4000) || !(g_atm_ro & 1) || inTrdemu || inNmi) return false;
    inTrdemu = true;
#if EVO_CFG_TRACE
    Debug::log("[TRDEMU] on port %02X drv %d mask %X pc=%04X", lo, vgSys & 3, fddMask, Z80::getRegPC());
#endif
    remap();
    return true;
}

#if EVO_CFG_TRACE
// Configuration writes with the guest PC: #7FFD (A15 = 0, FD/FC), #xx77 and #EFF7 —
// the same filter and line shape as tools/evo_sim.c ONLYCFG=1, so a capture diffs
// against the RTL model. Runs of one (port, value, pc) collapse; the budget is
// re-armed by every reset so a post-F11 capture is not empty.
static void evoTrace(uint16_t address, uint8_t data) {
    static uint16_t la = 0, lpc = 0; static uint8_t lv = 0; static uint32_t rep = 0;
    const uint16_t pc = Z80::getRegPC();
    if (s_evo_tr_n >= 1500) return;
    if (address == la && data == lv && pc == lpc) { rep++; return; }
    if (rep) { Debug::log("[EVOP] ... x%lu", (unsigned long)rep); rep = 0; }
    la = address; lv = data; lpc = pc; s_evo_tr_n++;
    Debug::log("[EVOP] pc=%04X %04X=%02X", pc, address, data);
}
#endif

static bool portWriteImpl(uint16_t address, uint8_t data) {
    const uint8_t lo = (uint8_t)address;
    const bool sh = dosPorts();
#if EVO_CFG_TRACE
    // The ProfROM far-call thunks (#BF = 1 with SET 0, window-0 page via #3FF7) run
    // hundreds of times a second in its 128 menu and drown everything else: skipped.
    const bool farCall = (lo == 0xBF && (data & ~1u) == (pBF & ~1u)) || (lo == 0xF7 && address == 0x3FF7);
    if (!farCall &&
        (((lo == 0xFD || lo == 0xFC) && !(address & 0x8000)) || lo == 0x77 ||
         lo == 0xBF || lo == 0xBE || lo == 0xBD ||
         (lo == 0xF7 && (address & 0x100) && (sh || !(address & 0x1000)))))
        evoTrace(address, data);
    if (lo == 0xBF && ((pBF | data) & 0x18)) {   // D3 NMI / D4 breakpoint: always, own budget
        static uint16_t n = 0;
        if (n < 300) { n++;
            const uint16_t sp = Z80::getRegSP();
            auto rd = [](uint16_t a) -> uint8_t { return MemESP::ramCurrent[a >> 14][a & 0x3FFF]; };
            Debug::log("[BF] pc=%04X %02X->%02X sp=%04X ret=%02X%02X %02X%02X 7ffd=%02X eff7=%02X w2=%03X/%03X%s", Z80::getRegPC(), pBF, data, sp,
                       rd((uint16_t)(sp + 1)), rd(sp), rd((uint16_t)(sp + 3)), rd((uint16_t)(sp + 2)),
                       p7ffd, pEFF7, pF7[2], pF7[6],
                       ((pBF & 0x08) && !(data & 0x08)) ? "  => NMI" : ""); }
    }
#endif
    switch (lo) {
    case 0xBF: {                                              // always
        const uint8_t old = pBF;
        // D3 1 -> 0 requests an NMI (znmi.v set_nmi_now, taken at the next frame INT
        // start; EVO Reset Service pulses it and HALTs). Taken at once here — the CPU
        // is halted waiting for it.
        if ((pBF & 0x08) && !(data & 0x08)) Z80::triggerNMI();
        pBF = data & 0x3F;   // D0 shadow, D1 ROM write, D2 font write, D3 NMI, D4 break,
                             // D5 4096-colour palette (base_trdemu pal444_ena)
        if ((old ^ pBF) & 0x20) { palDirty = true; VIDEO::atmPaletteChanged(); }
        shaden = (pBF & 1) != 0;
        g_atm_fnt = ((pBF & 0x04) && font) ? 1 : 0;
        g_atm_ro = (uint8_t)((g_atm_ro & 0x0F) | (g_atm_fnt ? 0x80 : 0));
        return true;
    }
    case 0xBE:                                               // OUT = leave NMI / the FDD emulator
        // zdos.v: in_trdemu clears on clr_nmi only when NOT in NMI.
        // znmi.v: in_nmi does NOT drop at the OUT — clr_nmi loads clr_count = 3 and
        // in_nmi clears after two more M1 refreshes, so the RETN that follows is still
        // fetched from RAM page #FF (EVO Reset Service's OUT_NMI: OUT (#BE),A / RETN at
        // #001B). Dropping it here fetched RETN from the ROM page under it and the
        // Magic menu ran into the 48K cold start. Deferred to the next control transfer
        // (check_trdos -> nmiClrApply). in_trdemu has no such delay (zdos.v).
        if (inNmi) nmiClrPending = true;
        else if (inTrdemu) { inTrdemu = false; remap(); }
        return true;
    case 0xBD:                                               // A12..A8 = #13: the FDD mask
        if (((address >> 8) & 0x1F) == 0x13) fddMask = data & 0x0F;
        return true;                                         // (#10/#11: breakpoint, unmodelled)
    case 0xFD: case 0xFC:                                    // #7FFD: A15 = 0, low byte FD/FC
        if (!(address & 0x8000)) { write7ffd(data); return lo == 0xFD; }
        return false;
    case 0xF7: {
        const bool hiA8 = (address & 0x100) != 0;
        if (sh && hiA8) {                                    // the memory manager
            const uint8_t sel = (uint8_t)(((p7ffd & 0x10) >> 2) | (address >> 14));
            switch ((address >> 10) & 3) {                   // { A11, A10 }
            case 3: pF7[sel] = f7enc(data); break;           // #xFF7: flags + 6-bit page
            case 1: pF7[sel] = (uint16_t)((pF7[sel] & ~0x1FFu) | (uint8_t)(data ^ 0xFF)); break; // #x7F7
            default: return true;                            // #xBF7 write-protect: unmodelled
            }
            remap();
#if EVO_CFG_TRACE
            if ((address >> 14) == 2) {   // window 2: the monitor's work page lives here
                static uint16_t n = 0; static uint16_t la = 0, lpc = 0; static uint8_t lv = 0;
                const uint16_t pc = Z80::getRegPC();
                if (n < 400 && !(address == la && data == lv && pc == lpc)) { n++; la = address; lv = data; lpc = pc;
                    Debug::log("[W2] pc=%04X %04X=%02X -> reg %03X 7ffd=%02X eff7=%02X",
                               pc, address, data, pF7[sel], p7ffd, pEFF7); }
            }
#endif
            LED::touchW(LED::RAM);
            return true;
        }
        if (sh != hiA8) f7Write(address, data, sh);
        return true;
    }
    default: break;
    }
    // The NEMO IDE decode with no NEMO image: swallowed, not handed to the A0 = 0 ULA.
    if (IDE::portScheme != IDE::NEMO &&
        (((lo & 7) == 0 && (((lo >> 3) ^ (lo >> 4)) & 1)) || lo == 0x11))
        return true;
    if (!sh) return false;
    if (const int i = savIdx(lo); i >= 0) { s_sav[i] = data; return true; }
    if (lo == 0x77) { write77(address, data); return true; }
    // #FF: the FDC system register, and the palette while /PEN2 (#77 A14 = 0). The
    // drive number it carries decides the FDD-emulator trap below.
    if (lo == 0xFF) {
        PERF_FDC_PORT(lo, true);
        vgSys = data;
        if (!(a77 & 0x4000)) palWrite(data, (uint8_t)(address >> 8));
    }
    if (trdemuTrap(lo)) return lo != 0xFF;   // a masked drive: the WD1793 never sees it
    return false;
}

// #BD read: the configuration read-back, by A12..A8 (base_trdemu zports.v portbdmux;
// the baseconf trunk had it on #BE — EVO Reset Service reads #BD).
static uint8_t portBD(uint8_t idx) {
    // Page numbers come back INVERTED — top.v feeds zports `.pages(~{rd_pages...})`,
    // i.e. in the #xFF7/#x7F7 data format: EVO Reset Service's far-call trampoline
    // ANDs it with #3F and writes it straight back to #3FF7 (page 24 at #0EEF).
    // (docs/evonmi.txt says "not inverted"; the RTL wins.)
    if (idx < 8) return (uint8_t)~pF7[idx];
    switch (idx) {
    case 0x08: { uint8_t v = 0; for (int i = 0; i < 8; i++) if (!(pF7[i] & 0x100)) v |= (uint8_t)(1u << i); return v; }
    case 0x09: { uint8_t v = 0; for (int i = 0; i < 8; i++) if (!(pF7[i] & 0x200)) v |= (uint8_t)(1u << i); return v; }
    case 0x0A: return p7ffd;
    case 0x0B: return pEFF7;
    case 0x0C: return (uint8_t)(((a77 & 0x4000) ? 0x80 : 0) | ((a77 & 0x200) ? 0x40 : 0) |
                                ((a77 & 0x100) ? 0x20 : 0) | (ESPectrum::trdos ? 0x10 : 0) |
                                (p77 & 0x0F));
    case 0x0D: return pal[VIDEO::borderColor & 15];
    case 0x0E: return 0xFF;                                  // font read-back: unmodelled
    case 0x0F: return (uint8_t)(0xF0 | (VIDEO::borderColor & 15));
    // The RTL leaves D7..D4 as XXXX; EVO Reset Service's _VERSION writes #0A here and
    // demands it back exactly (rst8service.a80), else "Incorrect FPGA zxevo_fw.bin".
    case 0x13: return fddMask;
    default:   return 0xFF;
    }
}

static bool portReadImpl(uint16_t address, uint8_t& v) {
    const uint8_t lo = (uint8_t)address;
    const bool sh = dosPorts();
    switch (lo) {
    case 0xBF: v = pBF; return true;
    case 0xBD: v = portBD((uint8_t)((address >> 8) & 0x1F)); return true;
    // #BE read = the same read-back. base_trdemu moved it to #BD (EVO Reset Service
    // reads #BD), baseconf trunk had it on #BE — and the ROM's own ATM CP/M BIOS
    // 1.07.15pe still reads #BE: ED_LDIR (atm_cpm/source/ed_drv.a80, page 4 #1837)
    // saves window 2 with IN (#06BE) and restores it through #B7F7. Answering #FF
    // left set-1 window 2 on RAM page 0, so every RAM-disk access moved the BIOS's
    // copy window and a sector bound for #8000 landed on #0000 (GOB2: the CCP's
    // JP BDOS at #0005 overwritten, hang while loading). Nothing in the ROM reads #BE
    // expecting #FF.
    case 0xBE: v = portBD((uint8_t)((address >> 8) & 0x1F)); return true;
    case 0xF7:
        // #BFF7 (noshad, A8 = 1) / #BEF7 (shadow, A8 = 0) with the Gluk ports on; any
        // other #xxF7 reads #FF.
        if (!(address & 0x4000) && (((address & 0x100) != 0) != sh) && (sh || (pEFF7 & 0x80)))
            v = RTC::readData();
        else
            v = 0xFF;
        return true;
    default: break;
    }
    if (lo == 0x2F) PERF_FDC_PORT(lo, false);
    if (!sh) return false;
    if (const int i = savIdx(lo); i >= 0) { v = s_sav[i]; return true; }
    if (lo == 0xFF) PERF_FDC_PORT(lo, false);
    if (trdemuTrap(lo)) { v = 0xFF; return true; }
    // #FF in shadow: { INTRQ, DRQ, 1, the system register's D4..D0 } (base_trdemu).
    if (lo == 0xFF) {
        Ports::FDDStep(true);
        v = (uint8_t)(0x20 | (vgSys & 0x1F));
        if (ESPectrum::fdd.control & kRVMWD177XDRQ) v |= 0x40;
        if (ESPectrum::fdd.control & (kRVMWD177XINTRQ | kRVMWD177XFINTRQ)) v |= 0x80;
        return true;
    }
    return false;
}

bool portWrite(uint16_t address, uint8_t data) {
#if EVO_CFG_TRACE
    if (s_nmi_tr > 0) { s_nmi_tr--; Debug::log("[NMIP] OUT pc=%04X %04X=%02X sh=%d", Z80::getRegPC(), address, data, (int)dosPorts()); }
#endif
    return portWriteImpl(address, data);
}

bool portRead(uint16_t address, uint8_t& v) {
    const bool r = portReadImpl(address, v);
#if EVO_CFG_TRACE
    if (s_nmi_tr > 0) { s_nmi_tr--; Debug::log("[NMIP] IN  pc=%04X %04X=%s%02X sh=%d", Z80::getRegPC(), address, r ? "" : "gen:", r ? v : 0, (int)dosPorts()); }
#endif
    return r;
}

// ---------------------------------------------------- NEMO IDE (#10 toggle) --
// The Evo's on-board IDE is NEMO-decoded (loa[2:0] = 0, loa[3] != loa[4]; #11 the
// high byte), but #10 alone also moves whole words: each access to #10 flips a
// trigger, the first read fetches the word and returns its low byte, the second
// returns the high one (writes: the first is held, the second sends {it, this}).
// An access to #11 or to any other IDE register resets the trigger, so the classic
// #10 + #11 pair keeps working. Evo ProfROM / Shadow Monitor read a sector with
// LD BC,#0010 : INIR : INIR (page 17 #1758) — without the trigger they got the
// low halves only. Unreal IDE_NEMO_DIVIDE (io.cpp) is the same model.

#if IDE_PORT_TRACE
// Low-noise NEMO access trace: runs of one (dir, port) collapse into a count with the
// first/last value, so a 512-byte INIR burst is one line and the drive probe before
// the first ATA command (which IDE.cpp's own level-1 lines cannot show) is readable.
static void nemoTrace(char dir, uint8_t lo, uint8_t v) {
    static char lD = 0; static uint8_t lP = 0, lF = 0, lL = 0; static uint32_t n = 0;
    static uint16_t lines = 0; static uint16_t lpc = 0;
    if (dir == lD && lo == lP) { n++; lL = v; return; }
    if (n && lines < 1500) {
        lines++;
        if (n == 1) Debug::log("[NEMO] %c %02X=%02X pc=%04X", lD, lP, lF, lpc);
        else Debug::log("[NEMO] %c %02X x%lu %02X..%02X pc=%04X", lD, lP, (unsigned long)n, lF, lL, lpc);
    }
    lD = dir; lP = lo; lF = lL = v; n = 1; lpc = Z80::getRegPC();
}
#define NEMO_TR(d, lo, v) nemoTrace(d, lo, v)
#else
#define NEMO_TR(d, lo, v) ((void)0)
#endif

bool nemoRead(uint16_t address, uint8_t& v) {
    const uint8_t lo = (uint8_t)address;
    if (address & 1) {                                  // #11 (any odd: NEMO A0 = high latch)
        s_nemoRt = 0;
        v = IDE::read_latch();
    } else if ((address & 0x18) == 0x08 && (address & 0xE0) == 0xC0) {   // #C8
        v = IDE::read8(8);
    } else if ((address & 0x18) == 0x10) {
        const uint8_t reg = (address >> 5) & 7;
        if (reg == 0) {
            s_nemoRt ^= 1;
            v = s_nemoRt ? IDE::read_data_low() : IDE::read_latch();
        } else {
            s_nemoRt = 0;
            v = IDE::read8(reg);
        }
    } else return false;
    LED::touchR(LED::IDE);
    NEMO_TR('R', lo, v);
    return true;
}

bool nemoWrite(uint16_t address, uint8_t data) {
    const uint8_t lo = (uint8_t)address;
    if (address & 1) {                                  // #11: the high byte comes first
        IDE::write_latch(data);
        s_nemoWt = 0; s_nemoW1 = 1;
    } else if ((address & 0x18) == 0x08 && (address & 0xE0) == 0xC0) {
        IDE::write8(8, data);
    } else if ((address & 0x18) == 0x10) {
        const uint8_t reg = (address >> 5) & 7;
        if (reg == 0) {
            s_nemoWt ^= 1;
            if (s_nemoW1) { s_nemoW1 = 0; IDE::write_data_low(data); }      // #11 then #10
            else if (s_nemoWt) { s_nemoWlo = data; }                         // hold the low byte
            else { IDE::write_latch(data); IDE::write_data_low(s_nemoWlo); } // {held, this}
        } else {
            s_nemoWt = 0;
            IDE::write8(reg, data);
        }
    } else return false;
    LED::touchW(LED::IDE);
    NEMO_TR('W', lo, data);
    return true;
}

} // namespace EvoBase
