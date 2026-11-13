// pico-speccy — PK Hobbit. See Hobbit.h. FLASH-resident on purpose.
#include "Hobbit.h"

#include <stdlib.h>
#include <string.h>

#include "app/Config.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include "miniz/miniz.h"
#include "app/ESPectrum.h"
#include "speccy/core/ArchRom.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/core/RomOverlay.h"
#include "speccy/devices/disk/MB02.h"
#include "speccy/devices/disk/wd1793.h"
#include "speccy/devices/storage/DivMMC.h"
#include "speccy/machines/Timex.h"
#include "speccy/roms/48k/romSinclair48K.h"
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"
#include "ui/LEDIndicators.h"

// hobbit_banks.h is Config.cpp's (internal-linkage bases); the row layout only.
struct hobbit_rom_bank_t { const uint8_t* data; const uint8_t* overlay; uint16_t zlen; };

// 0 = not a Hobbit; bit 0 = Hobbit; bit 1 = the DOS flip-flop is up (check_trdos
// looks for the leave edge only then).
uint8_t g_hobbit = 0;

#ifndef HOBBIT_TRACE
#define HOBBIT_TRACE 0
#endif

namespace Hobbit {

enum : uint8_t { ROLE_DOS, ROLE_LOGO, ROLE_MON };
// #0F bits
enum : uint8_t { HWMOD = 0x80, KSCAN_OFF = 0x40, NO_FE_NMI = 0x20, FRAME_NMI = 0x10, NO_KEY_NMI = 0x08 };

// RAM pages of the 48K map nothing else uses (the 48K runs on 5, 2, 0):
//   1  the shadow RAM: <a> at +0, <b> at +#2000
//   3  +#0000 the SINCLAIR image of #2000-#3FFF (a copy of the ROM's), +#2000 the interception bitmap (one bit per address below #4000)
//   4  SHADOW-B / SHADOW-A, 6  LOGO / FORTH: both are stored deflated
//      (hobbit_banks.h zlen) and inflated when the machine is selected
enum : uint8_t { PG_SHADOW = 1, PG_IMAGE = 3, PG_MON = 4, PG_LOGO = 6 };

static const hobbit_rom_bank_t* s_banks = nullptr;
static const hobbit_rom_bank_t* s_live = nullptr;    // the romset the RAM pages were built for
static uint8_t s_p0f = 0xFF;      // D25 port B; #FF = the pulled-up inputs after reset
uint8_t keysMode = KEYS_EDIT;
static uint8_t s_rowQ = 7;        // D46: the half-row of the last IN (#FE), encoded
static uint8_t s_symKey = 0;      // a punctuation key for the scanner only (frameKey)
static uint8_t s_fkey = 0;        // the framing key held (table A.5), 0 = none
static uint16_t s_fkeyLine = 0xFFFF;   // the line of the last key NMI
static uint16_t s_lastM1 = 0;     // the previous opcode fetch m1() saw
static uint8_t s_fkeyNmi = 0;     // ... the one the pending NMI is for
static uint8_t s_fkeyLatch = 0;   // what SHADOW-A's first IN (#0F) of that NMI reads
static bool    s_es = false;      // NMI taken, RETN #0063 not reached: configuration 7
static bool    s_dos = false;     // the #3Dxx flip-flop
// The 8251's control port (#35) as far as the screen needs it: after a reset it takes
// a mode byte, then the sync characters of a synchronous mode, then commands; command
// D6 is an internal reset. Its /DTR pin is the machine's "CP/M screen" line: command
// D1 = 0 puts the display on #E000 (shadow function #4F, SHADOW-A #018B: #94 with D1
// set for #4000 and clear for #E000; the boot sequence ends in #96).
static uint8_t s_uartNext = 1;    // 0 command, 1 mode, 2.. sync characters still to come + 1
static bool    s_scrE000 = false;
static uint8_t s_cfg = 0xFF;      // the configuration remap() last built
static bool s_snapRestored = false;   // Pss::load restored #0F & co.: afterSnapshot keeps them
static bool s_hadDiv = false, s_hadMb02 = false;   // the cards as the previous machine had them
// The hot key calls the shadow system's function #52, "выход в теневое меню"
// (guide, Appendix A): RST 8 with the number in I. Pending from the hot key to the
// NMI that delivers it; the caller's I comes back when that NMI's RETN is reached.
static bool s_apiCall = false;
static bool s_apiRestoreI = false;
static uint8_t s_apiSavedI = 0;
constexpr uint8_t kFnShadowMenu = 0x52;

// The M1 addresses that raise /INSHAD, as { address, instruction length }: the
// instruction completes and the NMI stacks address + length — the 28 return
// addresses SHADOW-A #0066 looks for (#1EE4 in V4.4, #1E1E in V2.9, #1EEB in the
// FORTH dump: the same set in all three). None of them is a jump.
static const struct { uint16_t at; uint8_t len; } kTrap[] = {
    {0x0008,3}, {0x089F,2}, {0x0976,1}, {0x09F7,2}, {0x0B80,1}, {0x0C0A,1}, {0x0CB2,1},
    {0x0EAD,2}, {0x0F8B,1}, {0x0FEB,3}, {0x1027,3}, {0x1041,1}, {0x10C5,2}, {0x1296,3},
    {0x12AC,2}, {0x17F5,2}, {0x1847,1}, {0x1869,2}, {0x18B4,1}, {0x1909,1}, {0x1B40,1},
    {0x2570,1}, {0x257C,1}, {0x2D30,1}, {0x3031,1}, {0x31FA,1}, {0x3221,2}, {0x34BA,1},
};
// An interception is pending: the NMI is taken at the fetch of this address (the
// core only looks at its NMI line between frames, and the service must see the
// machine exactly one instruction later). 0 = none.
static uint16_t s_trapRet = 0;
static bool s_nmiNext = false;    // IN (#FE) asked for an NMI: at the next opcode fetch

// PLM D9's line counter Y (0 = the first paper line; the INT is at Y 246, X 48).
static inline uint16_t scanY() { return (uint16_t)((246 + (CPU::tstates + 48 * 4) / 224) % 312); }

static inline uint8_t* shadow() { return MemESP::ram[PG_SHADOW].direct(); }
static inline uint8_t* image()  { return MemESP::ram[PG_IMAGE].direct(); }
static inline uint8_t* trapMap() { return image() + 0x2000; }

static inline uint8_t config() {
    const uint8_t c = (uint8_t)(s_p0f & 7);
    // ES up forces SHADOW-A only while #0F D7 ("the hardware may modify the
    // configuration") is 1. SHADOW-A's service reads SHADOW-B's texts with #3E and
    // the RAM with #3D, comes back with #B8, and MAGIC OPTION leaves for the DOS with
    // #38 — all with ES up.
    if (s_es && (s_p0f & HWMOD)) return 7;
    return (uint8_t)(c | (s_dos ? 2 : 0));
}

// SHADOW-B (half 0) / SHADOW-A (half 1).
static const uint8_t* monHalf(uint8_t half) {
    return MemESP::ram[PG_MON].direct() + (half ? 0x2000 : 0);
}

// A 16 KB ROM page at #0000 with the shadow RAM under it for writes — which is also
// what keeps an inflated page (it lives in RAM) from being written.
static void romWindow(const uint8_t* rom, const uint8_t* overlay, bool writeUnder) {
    MemESP::plugPtr16(0, (uint8_t*)rom);
    MemESP::registerOverlay(rom, overlay);
    if (!writeUnder) return;
    uint8_t* sh = shadow();
    MemESP::wrOverlay[0] = sh;
    MemESP::wrOverlay[1] = sh + 0x2000;
    MemESP::wr_overlay_mask |= 3;
}

// hobbit_banks.h zlen: raw deflate of a 16 KB page. The inflate state (~11 KB) is
// far too big for the 8 KB stack.
static bool inflatePage(const hobbit_rom_bank_t& b, uint8_t* dst) {
    tinfl_decompressor* d = (tinfl_decompressor*)tryMalloc(sizeof(tinfl_decompressor));
    if (!d) return false;
    tinfl_init(d);
    size_t in = b.zlen, out = 0x4000;
    const tinfl_status st = tinfl_decompress(d, b.data, &in, dst, dst, &out,
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(d);
    return st == TINFL_STATUS_DONE && out == 0x4000;
}

static void remap() {
    const uint8_t cfg = config();
    uint8_t* sh = shadow();
    bool dos = false;
    MemESP::wr_overlay_mask &= (uint8_t)~3;
    Timex::rd[0] = Timex::rd[1] = nullptr;
    Timex::wr[0] = Timex::wr[1] = nullptr;
    g_timex_mmu = 0;
    mem_desc_t::bank_dirty[0] = &mem_desc_t::dirty_sink;
    mem_desc_t::bank_dirty[1] = &mem_desc_t::dirty_sink;
    switch (cfg) {
    case 0: case 1: {
        // The Sinclair ROM, no writes (PLM D8). The SCLD slot hook serves both slots:
        // reads from rd[], writes dropped, and every opcode fetch comes to m1().
        uint8_t* img = image();
        MemESP::ramCurrent[0] = (uint8_t*)gb_rom_0_sinclair_48k;
        MemESP::ramCurrent[1] = img;
        MemESP::next_rom_mask = 0;
        Timex::rd[0] = (uint8_t*)gb_rom_0_sinclair_48k;
        Timex::rd[1] = img;
        g_timex_mmu = 1;
        break;
    }
    case 2:
        romWindow(s_banks[ROLE_DOS].data, s_banks[ROLE_DOS].overlay, false);
        dos = true;
        break;
    case 4:
        if (!s_banks[ROLE_LOGO].data) {
            // No LOGO ROM in this dump (V2.9: 16 KB of #FF): an empty socket, through
            // the SCLD hook's "nothing plugged in" (reads #FF, writes dropped).
            MemESP::ramCurrent[0] = (uint8_t*)gb_rom_0_sinclair_48k;
            MemESP::ramCurrent[1] = image();
            MemESP::next_rom_mask = 0;
            Timex::rd[0] = Timex::rd[1] = Timex::kOpen;
            g_timex_mmu = 1;
            break;
        }
        romWindow(MemESP::ram[PG_LOGO].direct(), nullptr, true);
        break;
    case 3: case 5:
        MemESP::plugPtr16(0, sh);
        break;
    default:   // 6 SHADOW-B, 7 SHADOW-A: 8 KB of ROM, <b> above, <a> under the ROM
        MemESP::ramCurrent[0] = (uint8_t*)monHalf(cfg & 1);
        MemESP::ramCurrent[1] = sh + 0x2000;
        MemESP::next_rom_mask = 0;
        MemESP::wrOverlay[0] = sh;
        MemESP::wr_overlay_mask |= 1;
        break;
    }
    MemESP::romInUse = dos ? 4 : 0;
    ESPectrum::trdos = dos;
    MemESP::ramContended[0] = MemESP::ramContended[1] = false;
    MemESP::p3special = 3;     // recoverPage0() keeps out: window 0 is ours
    // CP/M: the display reads #E000-#FAFF (Architectura, "Управление конфигурацией CP-M").
    uint8_t* scr = (cfg == 3 || s_scrE000) ? MemESP::ram[0].direct() + 0x2000 : MemESP::ram[5].direct();
    if (VIDEO::grmem != scr) {
        VIDEO::grmem = scr;
        VIDEO::gigascreenAutoFlip();
    }
    g_hobbit = (uint8_t)(1 | (s_dos ? 2 : 0));
    s_cfg = cfg;
#if HOBBIT_TRACE
    { static uint16_t n = 0;
      if (HOBBIT_TRACE > 1 || n < 400) { n++; Debug::log("[HOBBIT] cfg %u (0F=%02X es=%d dos=%d) pc=%04X sp=%04X", (unsigned)cfg,
                                     s_p0f, (int)s_es, (int)s_dos, (unsigned)Z80::getRegPC(), (unsigned)Z80::getRegSP()); } }
#endif
}

void bind(const hobbit_rom_bank_t* banks) { s_banks = banks; }

void reset() {
    const bool on = Config::arch == A_48K && isHobbitRomset(Config::romSet48) && s_banks;
    if (!on) {
        if (g_hobbit) {
            g_hobbit = 0;
            s_live = nullptr;
            MemESP::wr_overlay_mask &= (uint8_t)~3;
            DivMMC::enabled = s_hadDiv;
            MB02::enabled = s_hadMb02;
        }
        return;
    }
    // No expansion bus to speak of, and both cards live on the ROM's entry points
    // and in window 0, which is the configuration logic's here. Off for the session
    // (the IF2 rule, If2.cpp); the user's setting is not touched.
    if (!g_hobbit) { s_hadDiv = DivMMC::enabled; s_hadMb02 = MB02::enabled; }
    DivMMC::enabled = false;
    MB02::enabled = false;
    if (s_live != s_banks) {
        // Power-on: static RAM comes up with anything in it; SHADOW-A initialises
        // what it needs and deliberately keeps the rest across a RESET.
        s_live = s_banks;
        memset(shadow(), 0, 0x4000);
        uint8_t* img = image();
        memcpy(img, gb_rom_0_sinclair_48k + 0x2000, 0x2000);
        uint8_t* map = trapMap();
        memset(map, 0, 0x800);
        for (const auto& t : kTrap) map[t.at >> 3] |= (uint8_t)(1u << (t.at & 7));
        bool ok = inflatePage(s_banks[ROLE_MON], MemESP::ram[PG_MON].direct());
        if (s_banks[ROLE_LOGO].data) ok = inflatePage(s_banks[ROLE_LOGO], MemESP::ram[PG_LOGO].direct()) && ok;
        if (!ok) Debug::log("[HOBBIT] ROM page did not inflate (heap?) - the machine will not boot");
    }
    s_p0f = 0xFF;
    s_es = false;
    s_dos = false;
    s_uartNext = 1;
    s_scrE000 = false;
    s_snapRestored = false;
    s_apiCall = false;
    s_apiRestoreI = false;
    s_trapRet = 0;
    s_nmiNext = false;
    g_hobbit = 1;
    remap();
}

uint32_t snapSave(uint8_t* b) {
    b[0] = 1;
    b[1] = s_p0f;
    b[2] = (uint8_t)((s_es ? 1 : 0) | (s_dos ? 2 : 0) | (s_scrE000 ? 4 : 0) | (s_nmiNext ? 8 : 0));
    b[3] = s_uartNext;
    b[4] = (uint8_t)s_trapRet;
    b[5] = (uint8_t)(s_trapRet >> 8);
    return SNAP_MAX;
}

bool snapLoad(const uint8_t* b, uint32_t n) {
    if (!g_hobbit || n < 6 || b[0] < 1) return false;
    s_p0f = b[1];
    s_es = (b[2] & 1) != 0;
    s_dos = (b[2] & 2) != 0;
    s_scrE000 = (b[2] & 4) != 0;
    s_nmiNext = (b[2] & 8) != 0;
    s_uartNext = b[3] > 3 ? 0 : b[3];
    s_trapRet = (uint16_t)(b[4] | (b[5] << 8));
    s_apiCall = false;
    s_apiRestoreI = false;
    remap();
    s_snapRestored = true;
    return true;
}

void afterSnapshot() {
    if (!g_hobbit) return;
    if (s_snapRestored) { s_snapRestored = false; return; }   // a .pss brought its own
    s_p0f = (uint8_t)(HWMOD | NO_FE_NMI | FRAME_NMI | NO_KEY_NMI);   // #B8, SHADOW-A's own
    s_es = false;
    s_dos = false;
    s_scrE000 = false;
    s_nmiNext = false;
    s_trapRet = 0;
    remap();
}

void bootRom(Boot b) {
    if (!g_hobbit || b == BOOT_MENU) return;
    // The monitor has not run: its variables are whatever the last session left.
    s_p0f = (uint8_t)(HWMOD | NO_FE_NMI | FRAME_NMI | NO_KEY_NMI | (b == BOOT_DOS ? 2 : b == BOOT_LOGO ? 4 : 0));
    s_es = false;
    s_dos = false;
    s_trapRet = 0;
    remap();
}

// ── NMI ───────────────────────────────────────────────────────────────────────
void nmiAck() {
    s_trapRet = 0;
    s_nmiNext = false;
    if (s_apiCall) {
        // As if the program had executed RST 8 here: its return address goes on the
        // stack, and the NMI that follows stacks #000B — the address SHADOW-A takes
        // for an API call (it drops it again on the way out).
        s_apiCall = false;
        if (!s_es && config() <= 1) {
            const uint16_t pc = Z80::getRegPC();
            uint16_t sp = Z80::getRegSP();
            MemESP::writebyte(--sp, (uint8_t)(pc >> 8));
            MemESP::writebyte(--sp, (uint8_t)pc);
            Z80::setRegSP(sp);
            Z80::setRegPC(0x000B);
            s_apiSavedI = Z80::getRegI();
            s_apiRestoreI = true;
            Z80::setRegI(kFnShadowMenu);
        }
    }
#if HOBBIT_TRACE
    // HOBBIT_TRACE=2: no cap (bench runs that hunt one key press).
    { static uint16_t n = 0;
      if (HOBBIT_TRACE > 1 || n < 300) { n++; Debug::log("[HOBBIT] NMI ret=%04X I=%02X A=%02X key=%02X 0F=%02X ts=%u", (unsigned)Z80::getRegPC(),
                                (unsigned)Z80::getRegI(), (unsigned)Z80::getRegA(), s_fkeyLatch, s_p0f, (unsigned)CPU::tstates); } }
#endif
    s_es = true;
    remap();
}

void retn(uint16_t pcAfter) {
    // /OUTSHAD: the second byte of the RETN at #0063, fetched in configuration 7.
    if (!s_es || pcAfter != 0x0065 || s_cfg != 7) return;
    s_es = false;
    remap();
    if (s_apiRestoreI) { s_apiRestoreI = false; Z80::setRegI(s_apiSavedI); }
}

void shadowKey() {
    if (!g_hobbit) return;
    // Only where a program could issue the RST 8 itself: a SINCLAIR configuration,
    // no NMI in service.
    if (s_es || config() > 1) return;
    s_apiCall = true;
    Z80::triggerNMI();
}

// ── Opcode fetch, SINCLAIR configurations ─────────────────────────────────────
// This fetch is the second (or, for DD CB d op, the last) byte of a prefixed
// instruction: the Z80 takes no NMI there, and one stacked the middle of the
// instruction as the return address (hw 2026-11-13: ghost characters and lost keys —
// the NMI came between FD and CB of the ROM's BIT 5,(IY+1) at #10B0).
static inline bool midInstruction(uint16_t pc, uint16_t last) {
    auto rom = [](uint16_t a) -> uint8_t { return a < 0x4000 ? Timex::rd[a >> 13][a & 0x1FFF] : 0; };
    if (last == (uint16_t)(pc - 1)) {
        const uint8_t p = rom((uint16_t)(pc - 1));
        if (p == 0xDD || p == 0xFD || p == 0xED || p == 0xCB) return true;
    }
    if (last == (uint16_t)(pc - 2) && rom((uint16_t)(pc - 2)) == 0xCB) {
        const uint8_t p = rom((uint16_t)(pc - 3));
        if (p == 0xDD || p == 0xFD) return true;
    }
    return false;
}

uint8_t m1(uint16_t pc) {
    if (s_cfg > 1) return 0xFF;            // the empty LOGO socket
    const uint16_t last = s_lastM1;
    s_lastM1 = pc;
    if ((pc >> 8) == 0x3D && !(s_p0f & 7)) {
        // Not gated by D7: MAGIC OPTION (SHADOW-A #1165) stacks PUSH AF / LD A,#38 /
        // OUT (#0F),A / POP AF / RET, #3D2F and #0063, and expects the DOS ROM's
        // NOP / RET at #3D2F. The DOS ROM has NOP NOP NOP at #0063 in front of its
        // #0066 (which writes #B8): the fetch at #0064 is what drops ES (/OUTSHAD),
        // as SHADOW-A's RETN does. Dropped here, two instructions early — nothing
        // sees the difference, and the DOS configuration has no fetch hook.
        s_es = false;
        s_dos = true;
        remap();
        return MemESP::romPeek(pc >> 13, MemESP::ramCurrent[pc >> 13], pc);
    }
    // The scanner meets a framing key that is down: /INSHAD, every 128 lines while it
    // is held (the shadow system wants a cursor key on four passes running, SHADOW-B
    // #0F54, and does the auto-repeat). From ROM code only, like the IN (#FE) one.
    // There is ONE NMI latch (DD34:2): while an interception is waiting for its
    // instruction to end the key cannot ask again — it does on its next pass.
    if (s_fkey && !s_nmiNext && !s_trapRet && !s_es && (s_p0f & FRAME_NMI) && !midInstruction(pc, last)) {
        const uint16_t line = scanY();
        if ((line & 0x7F) == s_fkey && line != s_fkeyLine) {
            s_fkeyLine = line;
            s_fkeyNmi = s_fkey;
            s_nmiNext = true;
        }
    }
    if (s_nmiNext) {
        s_nmiNext = false;
        if (pc == s_trapRet) s_trapRet = 0;     // the same latch: one NMI serves both
        s_fkeyLatch = s_fkeyNmi ? (uint8_t)(s_fkeyNmi | 0x80) : 0;
        s_fkeyNmi = 0;
        Z80::doNMI();
        return MemESP::ramCurrent[0][0x66];
    }
    if (s_trapRet) {
        if (pc == s_trapRet) {
            // The NMI in place of this fetch: stack pc, ES up, SHADOW-A's #0066. The
            // core then counts the byte returned here as the opcode at REG_PC.
            s_trapRet = 0;
            Z80::doNMI();
            return MemESP::ramCurrent[0][0x66];
        }
        // An INT got in between: its handler returns to the address still pending.
    } else if (trapMap()[pc >> 3] & (1u << (pc & 7))) {
        for (const auto& t : kTrap)
            if (t.at == pc) { s_trapRet = (uint16_t)(pc + t.len); break; }
    }
    return Timex::rd[pc >> 13][pc & 0x1FFF];
}

void frameKey(uint8_t code, uint8_t sym) {
    if (code != s_fkey) s_fkeyLine = 0xFFFF;
    s_fkey = code;
    s_symKey = sym;
}

void dosLeave() {
    s_dos = false;
    remap();
}

// ── Ports ─────────────────────────────────────────────────────────────────────
static void unknownPort(char dir, uint16_t address, uint8_t data) {
    static uint8_t n = 0;
    if (n >= 48) return;
    n++;
    Debug::log("[HOBBIT] %s %04X %02X cfg=%u pc=%04X", dir == 'i' ? "IN" : "OUT", address, data,
               (unsigned)s_cfg, (unsigned)Z80::getRegPC());
}

static inline bool shadowCfg(uint8_t cfg) { return cfg == 3 || cfg >= 5; }

bool portIn(uint16_t address, uint8_t* out) {
    const uint8_t lo = (uint8_t)address;
    const uint8_t cfg = s_cfg;
    if (lo == 0xFE) {
        // D46: the half-row this read addressed, as PLM D7's priority encoder gives it
        // (Architectura p.7: A11, A12, A15, A14, A13, A10, A9 in that order, none = 7).
        // The shadow's keyboard emulation (SHADOW-B #0E8E) reads it back from #05.
        {
            const uint8_t h = (uint8_t)(address >> 8);
            s_rowQ = !(h & 0x08) ? 4 : !(h & 0x10) ? 3 : !(h & 0x80) ? 0 : !(h & 0x40) ? 1
                   : !(h & 0x20) ? 2 : !(h & 0x04) ? 5 : !(h & 0x02) ? 6 : 7;
        }
        // NMI on a read of #FE (the shadow system then answers for the keyboard).
        // It is asynchronous: taken at the end of this very IN, and the service
        // decodes the IN it interrupted. From ROM code the next opcode fetch is
        // m1()'s; from RAM the core's own NMI poll (the frame edge) is all there is.
        if (cfg <= 1 && !(s_p0f & NO_FE_NMI) && !s_es) {
            if (Z80::getRegPC() < 0x4000) s_nmiNext = true;
            else Z80::triggerNMI();
        }
        // D23 PA5 is the /INT line itself ("может быть считан для синхронизации"):
        // SHADOW-A's delay loop (#19A5) waits for it to go low. The generic read
        // ANDs this mask in (Leningrad's, 0xFF on every other 48K).
        g_len_kbmask = (CPU::tstates < (uint32_t)kIntLen) ? 0xDF : 0xFF;
        return false;                                 // the generic ULA read
    }
    if (lo == 0x0F) {                                 // D23.B: the key scanner
        // PB0-PB6 = the number of the key being scanned — the low seven bits of the
        // line counter (the scanner shares D34.1 / D40-41 with it, Architectura p.6) —
        // PB7 = that key is down. Programs wait for exact numbers: shadow function #4F
        // for a change (SHADOW-A #01CE), FORTH for #71 / #73 / #3D (its #0401, #0D5A,
        // #10C4) and reads the whole keyboard this way. The numbers are table A.5's:
        // a core key is row * 8 + (5 - bit), row 0 = the #7F half-row ... 7 = #FE.
        // A framing key's NMI: the scanner was on that key (SHADOW-A #007D).
        if (s_fkeyLatch) { *out = s_fkeyLatch; s_fkeyLatch = 0; return true; }
        const uint8_t n = (uint8_t)(scanY() & 0x7F);
        uint8_t v = n;
        if (s_fkey && n == s_fkey) v |= 0x80;
        if (s_symKey) { *out = (uint8_t)(n == s_symKey ? v | 0x80 : v); return true; }
        const uint8_t k = n & 7;
        if (n < 0x40 && k >= 1 && k <= 5 && !(Ports::port[7 - (n >> 3)] & (1u << (5 - k)))) v |= 0x80;
        *out = v;
        return true;
    }
    // #05: the configuration lines read back in D0-D2. The FORTH dump's boot stub
    // (#5B12) checks 0 after OUT (#0C),#80, then 5 and 7 after its own OUT (#0F).
    // It is the configuration bus BEHIND the ES gate: the same dump's NMI entry
    // (#0089) reads it with ES up to learn what was interrupted, keeps it in #3FFF
    // and puts it back before the RETN.
    // D5-D7 (D24 PB5-7) are register D46: the half-row of the last IN (#FE). Every
    // reader masks: #07 for the configuration, #E0 for the row.
    if (lo == 0x05) { *out = (uint8_t)((s_rowQ << 5) | 0x18 | (s_p0f & 7) | (s_dos ? 2 : 0)); return true; }
    if (!shadowCfg(cfg)) return false;                // SINCLAIR / DOS / LOGO: the Spectrum's ports
    // The VG93 (PLM D8, terms 7-13 and 16-18): its registers answer at #1F / #3F /
    // #5F / #7F in every configuration but SINCLAIR — except a READ in SHADOW-A,
    // which gets D24.C instead — and D24.C (DRQ, INTRQ) is also what #FF reads.
    // The CP/M BIOS drives the controller this way from configuration 3.
    if ((lo & 0x9F) == 0x1F && cfg != 7) {
        Ports::FDDStep(false);
        *out = rvmWD1793Read(&ESPectrum::fdd, (lo >> 5) & 3);
        return true;
    }
    if ((lo & 0x9F) == 0x1F || lo == 0xFF) {          // D24.C
        uint8_t v = 0;
        if (lo != 0xFF && Config::joystick == JOY_KEMPSTON) {
            LED::touchR(LED::KEMPJOY);
            v = Ports::port[Config::kempstonPort] & 0x1F;
        }
        Ports::FDDStep(true);
        if (ESPectrum::fdd.control & kRVMWD177XDRQ) v |= 0x40;
        if (ESPectrum::fdd.control & (kRVMWD177XINTRQ | kRVMWD177XFINTRQ)) v |= 0x80;
        *out = v;
        return true;
    }
    if (lo == 0x34 || lo == 0x35) { *out = 0; return true; }   // 8251: nothing on the wire
    // #06: D25.A read back — border, MIC, speaker as last written to #FE. The shadow
    // system saves it before it repaints the border (SHADOW-B #0FCB) and restores it.
    if (lo == 0x06) { *out = (uint8_t)(VIDEO::borderColor & 7); return true; }
    unknownPort('i', address, 0);
    *out = (lo & 1) ? 0xFF : 0x00;
    return true;
}

static void uartControl(uint8_t data) {
    if (s_uartNext == 1) {                            // mode
        s_uartNext = (data & 3) ? 0 : ((data & 0x80) ? 2 : 3);
        return;
    }
    if (s_uartNext > 1) {                             // a sync character
        if (--s_uartNext == 1) s_uartNext = 0;
        return;
    }
    if (data & 0x40) { s_uartNext = 1; return; }      // internal reset
    const bool e000 = !(data & 0x02);
    if (e000 != s_scrE000) { s_scrE000 = e000; remap(); }
}

bool portOut(uint16_t address, uint8_t data) {
    const uint8_t lo = (uint8_t)address;
    if (lo == 0x0F) {                                 // D25.B
        s_p0f = data;
        if (config() != s_cfg) remap();
        return true;
    }
    if (lo == 0xFE) return false;
    const uint8_t cfg = s_cfg;
    if (!shadowCfg(cfg)) return false;
    if (lo == 0x0C) {                                 // D25 control word
        if (data & 0x80) {                            // mode set: the output latches clear
            s_p0f = 0;
            if (config() != s_cfg) remap();
        }
        return true;
    }
    if (lo == 0x0D || lo == 0x0E) return true;        // D24 / D23 control words
    if ((lo & 0x9F) == 0x1F) {                        // VG93 registers (D8 terms 9, 10)
        Ports::FDDStep(false);
        rvmWD1793Write(&ESPectrum::fdd, (lo >> 5) & 3, data);
        return true;
    }
    if (lo == 0xFF) {                                 // D25.C: drive, /MR, HLT, side, /DDEN
        if (cfg == 3) { Ports::FDDStep(true); Ports::fdcSysWrite(data); }   // D8 term 22: DOS and CP/M only
        return true;
    }
    if (lo == 0x34) return true;                      // 8251 data: nothing on the wire
    if (lo == 0x35) { uartControl(data); return true; }
    unknownPort('o', address, data);
    return true;
}

}
