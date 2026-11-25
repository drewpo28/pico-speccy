// pico-speccy — Opus Discovery (see Opus.h for the model and its source).
// Flash code except m1Late / nmiTick, which the Z80 loops call.

#include "Opus.h"

#include <string.h>

#include "app/Buffer.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/SnapPack.h"
#include "app/ESPectrum.h"
#include "app/MachineSwitch.h"
#include "fs/FileUtils.h"
#include "speccy/core/ArchRom.h"
#include "speccy/core/MemESP.h"
#include "speccy/devices/disk/DiskSlots.h"
#include "speccy/machines/Sam/SamFdc.h"
#include "speccy/roms/opus.h"
#include "speccy/z80/z80.h"

#define OPUS_HOT __attribute__((noinline)) __not_in_flash("opus")

uint8_t g_opus_io = 0;

namespace Opus {

bool    enabled = false;
bool    paged   = false;
uint8_t nmiCnt  = 0;

// 0x2000-0x3FFF while paged: 2K RAM, then #FF (the I/O goes through memRead/memWrite;
// these bytes are only what a fetch from there would see).
static uint8_t* s_hi = nullptr;
static uint8_t  s_drive = 0;
static uint8_t  s_dra, s_ddra, s_cra, s_drb, s_ddrb, s_crb;   // 6821 PIA

// Instructions from DRQ to its NMI. Longer than any of the ROM's byte handlers from
// their data access to their RETN (at most 8 instructions, #189F), so a second NMI
// never lands inside the first one's handler; a byte at DD is 32 us = 112 T anyway.
static constexpr uint8_t NMI_DELAY = 12;

static OPUS_HOT void pageIn() {
    if (!enabled) return;
    paged = true;
    g_opus_io = 1;
    MemESP::page0_lo = const_cast<uint8_t*>(gb_rom_opus);
    MemESP::page0_hi = s_hi;
    MemESP::divmmc_lo_rom   = true;
    MemESP::divmmc_lo_dirty = nullptr;
    MemESP::divmmc_hi_dirty = nullptr;
    MemESP::mb02_write_gate = true;
    MemESP::mb02_page_dirty = nullptr;
    MemESP::divmmc_mapped   = true;
}

static OPUS_HOT void pageOut() {
    paged = false;
    g_opus_io = 0;
    MemESP::divmmc_mapped = false;
}

OPUS_HOT void m1Late(uint16_t pc) {
    if (!enabled) return;
    if (paged) { if (pc == 0x1748) pageOut(); }
    else if (pc != 0x1748) pageIn();
}

OPUS_HOT void nmiTick() { Z80::doNMI(); }

static void armNmi() {
    if (!nmiCnt && SamFdc::drq(s_drive)) nmiCnt = NMI_DELAY;
}

// Fuse opus_6821_access.
static uint8_t pia(uint8_t reg, uint8_t v, bool wr) {
    switch (reg & 3) {
    case 0:
        if (wr) {
            if (s_cra & 0x04) {
                s_dra = v;
                const uint8_t d = (v & 0x02) ? 1 : 0;
                if (d != s_drive) { SamFdc::shareRegs(s_drive, d); s_drive = d; }
                SamFdc::setSide(0, (v >> 4) & 1);
                SamFdc::setSide(1, (v >> 4) & 1);
            } else s_ddra = v;
            return 0;
        }
        if (s_cra & 0x04) { s_dra &= (uint8_t)~0x40; return s_dra; }   // printer never busy
        return s_ddra;
    case 1:
        if (wr) { s_cra = v; return 0; }
        return (uint8_t)(s_cra | 0x40);                                // ACK the printer
    case 2:
        if (wr) { if (s_crb & 0x04) s_drb = v; else s_ddrb = v; return 0; }   // no printer
        return (s_crb & 0x04) ? s_drb : s_ddrb;
    default:
        if (wr) { s_crb = v; return 0; }
        return s_crb;
    }
}

uint8_t memRead(uint16_t a) {
    if (a >= 0x3800) return 0xFF;
    if (a >= 0x3000) return pia((uint8_t)a, 0, false);
    const uint8_t reg = a & 3;
    const uint8_t v = SamFdc::inReg(s_drive, reg);
    if (reg == 0 || reg == 3) armNmi();
    return v;
}

void memWrite(uint16_t a, uint8_t v) {
    if (a >= 0x3800) return;
    if (a >= 0x3000) { pia((uint8_t)a, v, true); return; }
    const uint8_t reg = a & 3;
    SamFdc::outReg(s_drive, reg, v);
    if (reg == 3) armNmi();
    else if (reg == 0) nmiCnt = 0;       // a new command (Force Interrupt included)
}

bool machineOk(uint8_t arch, uint8_t romset) {
    const RomsetIdx r = (RomsetIdx)romset;
    return arch == A_48K && !isTimexRomset(r) && !isHobbitRomset(r) && !isQuorum64Romset(r);
}

bool init() {
    const bool want = Config::opus != 0 && machineOk(Config::arch, Config::romSet);
    if (!want) {
        if (paged) pageOut();
        enabled = false;
        nmiCnt = 0;
        if (s_hi) { Buffer::pfree(s_hi); s_hi = nullptr; }
        return true;
    }
    if (!s_hi) {
        s_hi = (uint8_t*)Buffer::palloc(8192, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!s_hi) {
            Debug::log("[Opus] no memory for the 8K window: interface off");
            enabled = false;
            return false;
        }
        memset(s_hi, 0, 2048);
        memset(s_hi + 2048, 0xFF, 8192 - 2048);
    }
    enabled = true;
    Debug::log("[Opus] on");
    return true;
}

// A power-on (a machine switch, a launch that turns the interface on): the 2K RAM zeroed
// as init() leaves a fresh one. A reset (F11) keeps it, as on the real interface.
void powerOn() {
    if (s_hi) memset(s_hi, 0, 2048);
}

void reset() {
    nmiCnt = 0;
    if (paged) pageOut();
    if (!enabled) return;
    s_dra = s_ddra = s_cra = s_drb = s_ddrb = s_crb = 0;
    s_drive = 0;
    SamFdc::reset();
}

void frameEnd() { SamFdc::frameEnd(); }

// ---------------------------------------------------------------------------
// Disk launch, as on the real interface (owner 2026-11-25): the Opus already on this
// machine = a disk change (drive 1, no reset); otherwise a power-on: the interface on,
// a 48K, reset. No RUN typed: RUN loads "run" from drive 1 when the user types it.

bool launchDisk(const char* path) {
    DiskSlots::slotMount(IFACE_SAM, 0, path);
    if (!SamFdc::mounted(0)) return false;
    if (enabled && Config::opus && machineOk(Config::arch, Config::romSet)) return true;
    Config::ram_file = NO_RAM_FILE;
    Config::last_ram_file = NO_RAM_FILE;
    Config::opus = 1;                    // one disk interface at a time
    Config::plusd = 0;
    Config::esxdos = 0;
    Config::mb02 = 0;
    Config::betadisk = false;
    Config::save();
    if (!machineOk(Config::arch, Config::romSet)) {
        const RomsetIdx r = machineOk(A_48K, Config::romSet48) ? Config::romSet48 : R_48K;
        MachineSwitch::commit(A_48K, r);   // resets (Opus::init runs there)
    } else {                             // this 48K carries it: stays (owner 2026-11-25)
        init();
        powerOn();
        ESPectrum::resetForLoad();
    }
    return enabled;
}

// PSOP v1: u8 ver, paged, drive, nmiCnt, the PIA's six registers, 2048 RAM bytes.
uint32_t snapSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(1); o.u8(paged ? 1 : 0); o.u8(s_drive); o.u8(nmiCnt);
    o.u8(s_dra); o.u8(s_ddra); o.u8(s_cra); o.u8(s_drb); o.u8(s_ddrb); o.u8(s_crb);
    if (s_hi) o.raw(s_hi, 2048); else for (int i = 0; i < 2048; i++) o.u8(0);
    return o.n;
}

bool snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn i(in, n);
    if (i.u8() < 1) return false;
    const bool pg = i.u8() != 0;
    s_drive = i.u8() & 1;
    nmiCnt = i.u8();
    s_dra = i.u8(); s_ddra = i.u8(); s_cra = i.u8(); s_drb = i.u8(); s_ddrb = i.u8(); s_crb = i.u8();
    if (s_hi) i.raw(s_hi, 2048);
    if (!i.ok) return false;
    SamFdc::setSide(0, (s_dra >> 4) & 1);
    SamFdc::setSide(1, (s_dra >> 4) & 1);
    if (pg) pageIn(); else pageOut();
    return true;
}

} // namespace Opus
