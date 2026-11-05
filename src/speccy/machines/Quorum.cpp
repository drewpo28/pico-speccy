// pico-speccy — Quorum 64 / 128 / 128+. See Quorum.h. FLASH-resident on purpose.
#include "Quorum.h"

#include "app/Config.h"
#include "app/ESPectrum.h"
#include "speccy/core/ArchRom.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/devices/disk/wd1793.h"
#include "speccy/devices/storage/RTC.h"
#include "speccy/video/Video.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/MemGates.h"
#include "ui/LEDIndicators.h"

uint8_t g_quorum = 0;

namespace Quorum {

uint8_t p00 = 0;
bool rasterPending = false;
uint8_t p80FD = 0;
uint8_t ext[8] = {};
bool fkeysToGuest = false;
static bool s_cpm = false;        // #80FD written since reset (write-once)
// #85 as last written. D5 = drive motor: on the Quorum it spins the disk by itself,
// head loaded or not — the SYS ROM's "Boot DOS" issues RESTORE with h = 0 and then
// waits for index pulses (QU7V42 #047E), which our WD1793 only produces while it is
// stepped. So a status read steps the controller whenever the motor is on.
static uint8_t s_p85 = 0;
// #7FFD as last written (the 256/512/1024 take page bits from D5-D7, which
// MemESP::bankLatch does not keep) and the RAM size of the romset, in 16 KB pages.
static uint8_t s_p7ffd = 0;
static uint8_t s_pages = 8;
// The original Quorum 64 (quorum48.rom): no TR-DOS ROM, no DOS signal — the trap at
// #3Dxx does nothing and the Beta ports are never the controller.
static bool s_orig = false;
// Pss::load restored #00 & co. (snapLoad): the afterSnapshot() that follows keeps them.
static bool s_snapRestored = false;

static inline bool is128() { return g_quorum >= 2; }
// The Quorum-1024+ (74xx board, ROM-MENU V5.0): #00 reads back, D2 = Pentagon raster,
// D4 = 128K-only. The Quorum 256 has neither (D2/D4 reserved, #00 write-only).
static inline bool is1024() { return g_quorum >= 2 && s_pages == 64; }
// /EXRAMEN enables DD22, which drives RAM17-19: in window 3 EX/E2/E3 from #7FFD,
// elsewhere RAM_8. Disabled = those lines low, i.e. a 128K map. #00 D4 ZX128 = 0 is
// 1024 mode (the schematic: DD24.3 OR -> /EXRAMEN). ROM-MENU V5.0's menu labelled it the
// other way ("128K-ONLY MODE: ON" at D4 = 0 — our copy is patched, rom_pack.py), but its
// own memtest settles it: the ROM
// test procedure (#0516) ends XOR A / OUT (0),A, unpatched, so the RAM test always runs
// with D4 = 0 — with D4 = 0 as 128K-only, page 0Dh aliases page 5 and the test
// overwrites its code at #5D1A ("DD1F 74 00" over a striped screen, hw 2026-10-30).
// #00 D5 is not in the gate: the ROM runs BASIC/TR-DOS with D5 = 1.
static inline bool extRamOn() { return !is1024() || !(p00 & ZX128); }
// #00 D2 (Pentagon raster) and D4 (1024) are the menu's settings: V5.0 keeps them in
// every #00 write, so a reset and the NMI acknowledge leave them (owner's call). Power-on
// and a romset change clear them (1024, "Quorum" raster). Quorum 1024 only.
static RomsetIdx s_cfgOwner = R_NONE;
static inline uint8_t keepBits() { return is1024() ? (uint8_t)(p00 & (PENT | ZX128)) : 0; }
// The shadow RAM at #0000: page 0 on the 128 (the same page #7FFD can show at
// #C000, as on the ZXMAK2/Unreal model), a page the 48K map never uses on the 64.
// #00 D3 RAM_8 puts page 8 there instead (Quorum 256+; folds to 0 on the 128).
static inline uint32_t shadowPage() {
    if (!is128()) return 1;
    return ((p00 & RAM_8) && extRamOn()) ? (8u & (s_pages - 1)) : 0;
}
// The #C000 page: #7FFD D6 EX_RAM, D7 E2_RAM, D5 E3_RAM -> page bits 3, 4, 5,
// masked by the RAM size — the Quorum memory map table ("Организация адресного
// пространства": banks 0..7 = pages 00h, 08h, 10h ... 38h by E3:E2:EX) and
// UnrealSpeccy MM_QUORUM agree.
// Write protect of the shadow RAM: #00 D6 BLK_WR on the 128/128+; on the 256..1024
// D6 only locks #7FFD and D5 B_ROM = 1 protects the shadow RAM (Quorum port map,
// "Порты компьютеров Кворум").
static inline bool shadowWp() {
    return (s_pages > 8) ? (p00 & B_ROM) != 0 : (p00 & BLK_WR) != 0;
}
static inline uint32_t topPage() {
    uint32_t pg = s_p7ffd & 7;
    if (extRamOn()) pg |= ((s_p7ffd & 0xC0) >> 3) | (s_p7ffd & 0x20);
    return pg & (s_pages - 1);
}

// Window 0: ROM index (rom[] slot) and whether RAM is mapped instead.
static void window0(uint8_t& rom, bool& ram) {
    const bool dos = ESPectrum::trdos;
    // Under the DOS signal #00 D7 picks TR-DOS from ROM (1) or the shadow RAM (0,
    // the "TR-DOS emulator" firmware); otherwise D0 maps the shadow RAM (ZXMAK2).
    ram = dos ? !(p00 & TR_DOS) : (p00 & F_RAM) != 0;
    // ZXMAK2 MemoryQuorum256: DOS first, then SYS only when B_ROM AND TR_DOS are
    // both 0, else BASIC by #7FFD D4.
    if (!is128())                  rom = dos ? 4 : 0;
    else if (dos)                  rom = 4;
    else if (!(p00 & (B_ROM | TR_DOS))) rom = 2;                     // SYS
    else                           rom = MemESP::romLatch ? 1 : 0;
}

static uint8_t* screenBase() {
    if (!is128())
        return MemESP::ram[(p00 & UP_S) ? 0 : 5].direct();
    // #80FD's screen base is not applied: ZXMAK2 and Unreal both show page 5/7
    // by #7FFD D3 only, and moving the screen with it broke the CP/M boot.
    return MemESP::ram[MemESP::videoLatch ? 7 : 5].direct();
}

void remap() {
    uint8_t r; bool ram;
    window0(r, ram);
    MemESP::romInUse = r;
    MemESP::wr_overlay_mask &= (uint8_t)~3;
    const uint32_t sp = shadowPage();
    if (ram) {
        MemESP::plugPtr16(0, MemESP::ram[sp].sync(0));
    } else {
        MemESP::plugPtr16(0, (uint8_t*)MemESP::rom[r].direct());
        mem_desc_t::bank_dirty[0] = &mem_desc_t::dirty_sink;
        mem_desc_t::bank_dirty[1] = &mem_desc_t::dirty_sink;
        // ROM visible: a write still lands in the shadow RAM under it (ports.md D6:
        // "запись в адреса 0000..3FFF разрешена"; ZXMAK2 MapWrite0000 = RAM page).
        if (!shadowWp()) {
            uint8_t* sh = MemESP::ram[sp].direct();
            MemESP::wrOverlay[0] = sh;
            MemESP::wrOverlay[1] = sh + 0x2000;
            MemESP::wr_overlay_mask |= 3;
        }
    }
    // Write protect drops every write into #0000-#3FFF (the ATM read-only window gate).
    g_atm_ro = shadowWp() ? 1 : 0;
    MemESP::ramContended[0] = MemESP::ramContended[1] = false;
    if (is128()) {
        MemESP::plug16(3, MemESP::ram[topPage()].sync(3), false);
    }
    MemESP::p3special = 3;     // recoverPage0() keeps out: window 0 is ours
    uint8_t* scr = screenBase();
    if (VIDEO::grmem != scr) {
        VIDEO::grmem = scr;
        VIDEO::gigascreenAutoFlip();
    }
}

void reset() {
    g_quorum = (Config::arch == A_48K && isQuorum64Romset(Config::romSet48)) ? 1
             : (Config::arch == A_PENT && isQuorum128Romset(Config::romSetPent))
                 ? (Config::romSetPent == R_QU128 ? 2 : 3)     // 128+ and up have the AY
                 : 0;
    {
        // Keep D2/D4 across a reset of the same 256..1024 romset (keepBits).
        const RomsetIdx rs = (g_quorum >= 2) ? Config::romSetPent : R_NONE;
        // Power-on / romset change: #00 = 0 = 1024 mode, "Quorum" raster.
        const uint8_t keep = (rs == s_cfgOwner && rs == R_QU1024)
                                 ? (uint8_t)(p00 & (PENT | ZX128)) : 0;
        s_cfgOwner = rs;
        p00 = keep;
    }
    p80FD = 0;
    s_p7ffd = 0;
    s_pages = (g_quorum >= 2) ? quorumRamPages(Config::romSetPent) : 4;
    s_orig = g_quorum == 1 && isQuorum64OrigRomset(Config::romSet48);
    s_cpm = false;
    s_p85 = 0;
    s_snapRestored = false;
    for (int i = 0; i < 8; i++) ext[i] = 0;
    if (!g_quorum) {
        MemESP::wr_overlay_mask &= (uint8_t)~3;
        return;
    }
    ESPectrum::trdos = false;
    remap();
}

void afterSnapshot() {
    if (!g_quorum) return;
    if (s_snapRestored) { s_snapRestored = false; return; }   // a .pss brought its own
    p00 = (uint8_t)(B_ROM | TR_DOS | keepBits());      // BASIC running, TR-DOS from ROM
    s_p7ffd = (uint8_t)((MemESP::bankLatch & 7) | (MemESP::videoLatch ? 0x08 : 0)
                      | (MemESP::romLatch ? 0x10 : 0));
    remap();
}

uint32_t ramPages() {
    if (!g_quorum) return 0;
    return is128() ? s_pages : 4;
}

uint32_t snapSave(uint8_t* b) {
    b[0] = 1;
    b[1] = p00;
    b[2] = p80FD;
    b[3] = s_p7ffd;
    b[4] = s_p85;
    b[5] = s_cpm ? 1 : 0;
    return SNAP_MAX;
}

bool snapLoad(const uint8_t* b, uint32_t n) {
    if (!g_quorum || n < 6 || b[0] < 1) return false;
    const uint8_t old = p00;
    p00     = b[1];
    p80FD   = b[2];
    s_p7ffd = b[3];
    s_p85   = b[4];
    s_cpm   = (b[5] & 1) != 0;
    // D2 picks the raster on the 1024: re-derive it at the frame boundary.
    if (is1024() && ((old ^ p00) & PENT)) rasterPending = true;
    remap();
    s_snapRestored = true;
    return true;
}

void nmiAck() {
    if (!g_quorum) return;
    p00 = keepBits();
    remap();
}

void bootRom(Boot b) {
    if (!g_quorum) return;
    // ESPectrum::reset() left #00 = 0 (SYS on the 128, BASIC-48 on the 64), #7FFD = 0.
    MemESP::bankLatch = 0;
    MemESP::romLatch = 0;
    MemESP::videoLatch = 0;
    s_p7ffd = 0;
    ESPectrum::trdos = false;
    const uint8_t keep = keepBits();
    switch (b) {
    case BOOT_SYS:
        p00 = 0;
        break;
    case BOOT_128:                                    // SYS #0083 (RUN_BASIC_128): #00 = #A8
        p00 = B_ROM | TR_DOS | RAM_8;                 // (RAM_8 folds to page 0 on the 128)
        break;
    case BOOT_48:                                     // SYS #0097: #7FFD = #17, #00 = #E0
        if (is128()) {
            MemESP::bankLatch = 7;
            MemESP::romLatch = 1;
            s_p7ffd = 0x17;
            p00 = B_ROM | BLK_WR | TR_DOS;            // D6 = the #7FFD lock (+ ROM write-protect)
        } else {
            p00 = 0;                                  // the 64's plain reset is BASIC-48
        }
        break;
    case BOOT_TRDOS:                                  // the TR-DOS ROM, BASIC-48 under it
        if (s_orig) { p00 = 0; break; }               // none on the original 64
        MemESP::romLatch = 1;
        s_p7ffd = 0x10;
        p00 = B_ROM | TR_DOS;
        ESPectrum::trdos = true;
        break;
    }
    p00 |= keep;
    remap();
}

void trdosEdge(uint8_t pch) {
    if (s_orig) return;                              // no TR-DOS ROM to trap into
    if (!ESPectrum::trdos) {
        if (pch != 0x3D) return;
        uint8_t r; bool ram;
        window0(r, ram);
        // DOS opens only from the 48 BASIC with ROM actually mapped (the generic
        // 48K/Pentagon rule, plus the shadow RAM and the 128's SYS page).
        if (ram || r != (is128() ? 1 : 0)) return;
        ESPectrum::trdos = true;
        remap();
    } else if (pch >= 0x40) {
        ESPectrum::trdos = false;
        remap();
    }
}

// #85: D0 drive A, D1 drive B, D4 side (0 = upper head), D5 motor. Beta SYS layout:
// D0-D1 drive, D2 /RESET, D3 HLT, D4 side (1 = side 0), D6 density (0 = MFM).
static uint8_t sysToBeta(uint8_t v) {
    static const uint8_t drv[4] = { 3, 0, 1, 3 };   // none / A / B / both (Unreal)
    return (uint8_t)(0x04 | drv[v & 3] | ((v & 0x20) ? 0x08 : 0) | ((v & 0x10) ? 0 : 0x10));
}

static uint8_t extRead(uint16_t address) {
    uint8_t v = 0;
    const uint8_t hi = (uint8_t)~(address >> 8);
    for (int i = 0; i < 8; i++)
        if (hi & (1u << i)) v |= ext[i];
    uint8_t hiBits = 0xC0;
    // "в Кворум-256-1024 (не во всех)": D6 reads RAM_8 (#00 D3), D7 reads SCR_P
    // (#7FFD D3); always 1 elsewhere.
    if (s_pages > 8)
        hiBits = (uint8_t)(((p00 & RAM_8) ? 0x40 : 0) | ((s_p7ffd & 0x08) ? 0x80 : 0));
    return (uint8_t)(hiBits | (~v & 0x3F));
}

// Quorum 64 disk board "disk5trd" (quorum-reborn resources/DiskDrive): DD19 (KP11)
// switches the WD1793 register select from A1/A0 (#80-#85) to A6/A5 (the Beta ports)
// while /TRD is active, and QU4V93T4's TR-DOS is the stock 5.04T on #1F/#3F/#5F/#7F/
// #FF. Under the DOS signal those ports go to the generic Beta-128 decode
// (Config::betadisk is forced on for the 64). The 128's QU7V42 TR-DOS is moved to #80.
// The Quorum-1024 (2024) port table says the same: #1F/#3F/#5F/#7F/#FF are the
// WD1793 with TRD = 0, #80-#85 with TRD = 1. #80-#85 stay decoded under DOS here
// too, because QU7V42's own TR-DOS is the moved-port one.
static inline bool betaPort64(uint8_t lo) {
    return ((g_quorum == 1 && !s_orig) || is1024()) && ESPectrum::trdos && (lo & 0x1F) == 0x1F;
}

// Gluk clock (ports.md "Порты энергонезависимых часов"): #EFF7 (D7 = clock open),
// #DFF7 register select, #BFF7 data — the Pentagon pair. An add-on, so it answers
// only while Devices > CMOS + NVRAM fits it; #EFF7 D7 is not gated (as on Pentagon).
static inline bool glukPort(uint8_t lo) { return lo == 0xF7 && Config::rtc_enabled; }

bool portIn(uint16_t address, uint8_t* out) {
    const uint8_t lo = (uint8_t)address;
    if (betaPort64(lo)) return false;
    if (lo == 0xF7) {
        *out = (glukPort(lo) && address == 0xBFF7) ? RTC::readData() : 0xFF;
        return true;
    }
    if ((lo & 0x9C) == 0x80) {                       // #80-#83 WD1793
        Ports::FDDStep((s_p85 & 0x20) != 0);
        *out = rvmWD1793Read(&ESPectrum::fdd, lo & 3);
        return true;
    }
    if ((lo & 0x9F) == 0x85) {                       // #85 status (INTRQ, DRQ)
        Ports::FDDStep(true);
        uint8_t v = 0;
        if (ESPectrum::fdd.control & kRVMWD177XDRQ) v |= 0x40;
        if (ESPectrum::fdd.control & (kRVMWD177XINTRQ | kRVMWD177XFINTRQ)) v |= 0x80;
        *out = v;
        return true;
    }
    // #00 reads back on the Quorum-1024+ (V5.0: IN A,(0) / AND #14 / OR x / OUT (0),A).
    if (is1024() && (lo & 0x99) == 0x00) { *out = p00; return true; }
    if ((lo & 0x99) == 0x18) { *out = extRead(address); return true; }   // #7E
    // Quorum-1024 (2024) port table: #7FFD reads back (0xxx xxxx xxx1 1x0x).
    if (is1024() && (address & 0x801A) == 0x0018) { *out = s_p7ffd; return true; }
    if ((lo & 0x99) == 0x19) {                       // #1F / #1B: Kempston + printer BUSY
        uint8_t v = 0;
        if (Config::joystick == JOY_KEMPSTON) {
            LED::touchR(LED::KEMPJOY);
            v = Ports::port[Config::kempstonPort] & 0x1F;
        }
        *out = v;                                    // D7 = 0: no printer, not busy
        return true;
    }
    if (!(lo & 1)) {
        if ((lo & 0x99) == 0x98) return false;       // #FE: the generic ULA read
        *out = 0xFF;
        return true;
    }
    if ((lo & 0x1F) == 0x1F) { *out = 0xFF; return true; }   // no Beta-128 here
    // Quorum 128: no AY (the 128+ has one).
    if (g_quorum == 2 && (address & 0xC002) == 0xC000) { *out = 0xFF; return true; }
    return false;
}

bool portOut(uint16_t address, uint8_t data) {
    const uint8_t lo = (uint8_t)address;
    if (betaPort64(lo)) return false;
    if ((lo & 0x99) == 0x00) {                       // #00 memory control
        if (is1024() && ((p00 ^ data) & PENT)) rasterPending = true;
        p00 = data;
        remap();
        return true;
    }
    if ((lo & 0x9C) == 0x80) {                       // #80-#83 WD1793
        Ports::FDDStep(false);
        rvmWD1793Write(&ESPectrum::fdd, lo & 3, data);
        return true;
    }
    if ((lo & 0x9F) == 0x85) {                       // #85 drive / side / motor
        Ports::FDDStep(true);
        s_p85 = data;
        Ports::fdcSysWrite(sysToBeta(data));
        return true;
    }
    if (is128()) {
        if ((address & 0x801A) == 0x0018) {          // #7FFD
            // The lock is #00 D6 (BLK_WR), not #7FFD D5 (ZXMAK2 MemoryQuorum256).
            if (p00 & BLK_WR) return true;
            s_p7ffd = data;
            MemESP::bankLatch = data & 7;
            MemESP::romLatch = (data >> 4) & 1;
            MemESP::videoLatch = (data >> 3) & 1;
            remap();
            return true;
        }
        if ((address & 0xA01A) == 0x8018) {          // #80FD, write-once
            if (!s_cpm) { s_cpm = true; p80FD = data; }
            return true;
        }
        if ((address & 0x8002) == 0) return true;    // the loose #7FFD family: not ours
        if (g_quorum == 2 && (address & 0x8002) == 0x8000) return true;   // no AY on the 128
    }
    if (!(lo & 1)) return (lo & 0x99) != 0x98;      // only #FE reaches the ULA
    if ((lo & 0x1F) == 0x1F) return true;            // no Beta-128
    if (lo == 0xFB || lo == 0x7B) return true;      // printer data / strobe
    if (lo == 0xF7) {                                // Gluk clock (glukPort)
        if (glukPort(lo)) {
            if (address == 0xDFF7) RTC::selectReg(data);
            else if (address == 0xBFF7) RTC::writeData(data);
        }
        return true;
    }
    return false;
}

// Config::isQuorumKvRaster: the "Quorum" raster unless #00 D2 asks for Pentagon's.
// D2 is honoured only once Quorum::reset has claimed it for the current romset — CPU::reset
// derives the frame before Quorum::reset runs, and a stale D2 of another romset must not
// leak into it.
bool pentRaster() {
    return s_cfgOwner == Config::romSetPent && (p00 & PENT) != 0;
}

}
