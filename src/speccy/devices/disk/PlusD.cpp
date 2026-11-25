// pico-speccy — MGT Plus D (see PlusD.h for the model and its source).
// Flash code, except pageIn/pageOut: the fetch loop calls pageIn on every trap hit
// (each 48K-ROM interrupt runs through 0x003A and 0x028E with the Plus D on).

#include "PlusD.h"

#include <string.h>

#include "app/Buffer.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/SnapPack.h"
#include "speccy/core/ArchRom.h"
#include "speccy/core/MemESP.h"
#include "speccy/machines/Sam/SamFdc.h"
#include "speccy/roms/plusd.h"
#include "speccy/devices/disk/DiskSlots.h"
#include "app/ESPectrum.h"
#include "app/MachineSwitch.h"
#include "fs/FileUtils.h"
#include "fatfs/ff.h"

#define PLUSD_HOT __attribute__((noinline)) __not_in_flash("plusd")

namespace PlusD {

bool enabled = false;
bool paged   = false;

static uint8_t* s_ram = nullptr;     // 8K, heap or butter PSRAM
static uint8_t  s_ctrl = 0;          // #EF
static uint8_t  s_drive = 0;         // 0/1, the drive the controller talks to

PLUSD_HOT void pageIn() {
    if (!enabled) return;
    paged = true;
    MemESP::page0_lo = const_cast<uint8_t*>(gb_rom_plusd);
    MemESP::page0_hi = s_ram;
    MemESP::divmmc_lo_rom   = true;      // writes to the ROM half are dropped
    MemESP::divmmc_lo_dirty = nullptr;
    MemESP::divmmc_hi_dirty = nullptr;
    MemESP::mb02_write_gate = true;      // the RAM half is always writable
    MemESP::mb02_page_dirty = nullptr;
    MemESP::divmmc_mapped   = true;
}

PLUSD_HOT void pageOut() {
    paged = false;
    MemESP::divmmc_mapped = false;
}

bool machineOk(uint8_t arch, uint8_t romset) {
    const RomsetIdx r = (RomsetIdx)romset;
    if (arch != A_48K && arch != A_128K) return false;
    return !isPlus3Romset(r) && !isTimexRomset(r) && !isHobbitRomset(r) && !isQuorum64Romset(r);
}

bool init() {
    const bool want = Config::plusd != 0 && machineOk(Config::arch, Config::romSet);
    if (!want) {
        if (paged) pageOut();
        enabled = false;
        if (s_ram) { Buffer::pfree(s_ram); s_ram = nullptr; }
        return true;
    }
    if (!s_ram) {
        s_ram = (uint8_t*)Buffer::palloc(8192, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
        if (!s_ram) {
            Debug::log("[PlusD] no memory for the 8K RAM: interface off");
            enabled = false;
            return false;
        }
        memset(s_ram, 0, 8192);
    }
    enabled = true;
    Debug::log("[PlusD] on");
    return true;
}

// A power-on for the interface (a machine switch, a disk launch that turns it on): the
// 8K RAM zeroed, so the boot ROM's #3DE4 check initialises it and RUN loads +SYS again.
// A reset (F11) keeps it, as on the real interface.
void powerOn() {
    if (s_ram) memset(s_ram, 0, 8192);
}

void reset() {
    if (!enabled) { if (paged) pageOut(); return; }
    s_ctrl = 0;
    s_drive = 0;
    SamFdc::reset();
    // Fuse plusd_reset pages the interface in on every machine. G+DOS's reset code
    // (0x0000 -> 0x0533) then leaves through OUT (#E7) into 0x11CB, START/NEW of the
    // 48K ROM — on a 128K / +2 ROM 0 (the editor) is paged there and 0x11CB is the
    // middle of its tape-out loop: the machine crashed at power-on (host bench,
    // 2026-11-25). G+DOS never touches #7FFD, so on the 128K the interface stays out
    // at reset and the 128 ROM boots; the M1 traps bring G+DOS in from 48 BASIC's
    // ROM 1 (its IM1 handler at 0x003A, RST 8, KEY-SCAN) exactly as on the 48K.
    if (Config::arch == A_48K) pageIn(); else pageOut();
}

uint8_t portIn(uint8_t lo) {
    switch (lo) {
    case 0xE3: return SamFdc::inReg(s_drive, 0);
    case 0xEB: return SamFdc::inReg(s_drive, 1);
    case 0xF3: return SamFdc::inReg(s_drive, 2);
    case 0xFB: return SamFdc::inReg(s_drive, 3);
    case 0xE7: pageIn(); return 0;       // Fuse plusd_patch_read
    case 0xF7: return 0x7F;              // printer: never busy (bit 7 low)
    default:   return 0xFF;              // #EF is write-only
    }
}

void portOut(uint8_t lo, uint8_t v) {
    switch (lo) {
    case 0xE3: SamFdc::outReg(s_drive, 0, v); break;
    case 0xEB: SamFdc::outReg(s_drive, 1, v); break;
    case 0xF3: SamFdc::outReg(s_drive, 2, v); break;
    case 0xFB: SamFdc::outReg(s_drive, 3, v); break;
    case 0xE7: pageOut(); break;
    case 0xEF: {
        s_ctrl = v;
        const uint8_t d = ((v & 0x03) == 2) ? 1 : 0;   // Fuse plusd_cn_write
        if (d != s_drive) { SamFdc::shareRegs(s_drive, d); s_drive = d; }
        SamFdc::setSide(0, v >> 7);
        SamFdc::setSide(1, v >> 7);
        break;
    }
    default: break;                      // #F7 printer data: no printer
    }
}

void frameEnd() { SamFdc::frameEnd(); }

// G+DOS directory: tracks 0-3 of side 0, two 256-byte entries a sector, the file type
// in the low 5 bits of byte 0 (bits 6/7 = protected/hidden). Spectrum types are 1-11
// (BASIC ... EXECUTE), SAM ones start at 16. Flat MGT: track-interleaved sides.
bool looksPlusD(const char* path) {
    FIL* fp = fopen2(path, FA_READ);
    if (!fp) return false;
    const uint32_t size = (uint32_t)f_size(fp);
    if (size != 819200u) { fclose2(fp); return false; }
    int zx = 0, sam = 0;
    uint8_t sec[512];
    for (uint32_t cyl = 0; cyl < 4; cyl++) {
        for (uint32_t s = 0; s < 10; s++) {
            UINT br = 0;
            if (f_lseek(fp, cyl * 10240u + s * 512u) != FR_OK ||
                f_read(fp, sec, sizeof(sec), &br) != FR_OK || br != sizeof(sec)) { cyl = 4; break; }
            for (int e = 0; e < 2; e++) {
                const uint8_t t = sec[e * 256] & 0x1F;
                if (t >= 1 && t <= 11) zx++;
                else if (t >= 16) sam++;
            }
        }
    }
    fclose2(fp);
    return zx > 0 && zx > sam;
}

// ---------------------------------------------------------------------------
// Disk launch, as on the real interface (owner 2026-11-25):
// - the Plus D already on this machine: a disk change. Drive 1 only, no reset; a
//   resident G+DOS stays (CAT 1, LOAD d1"..."), F12 + RUN starts the disk afresh;
// - otherwise plugging the interface in = a power-on: the interface on, the 8K RAM
//   cleared, reset, on this machine when it carries the Plus D (48K / 128K / +2),
//   else on a 128K. No RUN typed: the user boots G+DOS with RUN.

bool launchDisk(const char* path) {
    DiskSlots::slotMount(IFACE_SAM, 0, path);
    if (!SamFdc::mounted(0)) return false;
    if (enabled && Config::plusd && machineOk(Config::arch, Config::romSet)) return true;
    Config::ram_file = NO_RAM_FILE;
    Config::last_ram_file = NO_RAM_FILE;
    // One disk interface at a time (the menu's rule, UiStage resolveConstraints).
    Config::plusd = 1;
    Config::esxdos = 0;
    Config::mb02 = 0;
    Config::betadisk = false;
    Config::save();
    // A launch that turns the interface on is a power-on: with G+DOS still resident in
    // the RAM, RUN only says "O.K. G+DOS" instead of loading +SYS and the disk's AUTO
    // (owner, hw 2026-11-25). MachineSwitch::commit clears it too; this covers the reset
    // on the same machine. init() zeroes a fresh allocation.
    powerOn();
    // The machine stays when it carries the interface (owner 2026-11-25: the user picks
    // it; a 128K-only title such as The Secret of Monkey Island, OutSoftware 2026,
    // resets a 48K after its intro). Otherwise a 128K: 48K titles run there too.
    if (!machineOk(Config::arch, Config::romSet)) {
        const RomsetIdx r = machineOk(A_128K, Config::romSet128) ? Config::romSet128 : R_128K;
        MachineSwitch::commit(A_128K, r);                    // resets (PlusD::init runs there)
    } else {
        init();
        ESPectrum::resetForLoad();
    }
    return enabled;
}

// PSPD v1: u8 ver, u8 paged, u8 ctrl, u8 drive, 8192 RAM bytes.
uint32_t snapSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(1); o.u8(paged ? 1 : 0); o.u8(s_ctrl); o.u8(s_drive);
    if (s_ram) o.raw(s_ram, 8192); else for (int i = 0; i < 8192; i++) o.u8(0);
    return o.n;
}

bool snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn i(in, n);
    if (i.u8() < 1) return false;
    const bool pg = i.u8() != 0;
    s_ctrl = i.u8();
    s_drive = i.u8() & 1;
    if (s_ram) i.raw(s_ram, 8192);
    if (!i.ok) return false;
    SamFdc::setSide(0, s_ctrl >> 7);
    SamFdc::setSide(1, s_ctrl >> 7);
    if (pg) pageIn(); else pageOut();
    return true;
}

} // namespace PlusD
