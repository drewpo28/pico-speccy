/*

pico-speccy: .NEX file loader (ZX Spectrum Next executable format)

Format reference: https://wiki.specnext.dev/NEX_file_format
Header V1.0-V1.3, 512 bytes; optional palette + loading screens; 16K
banks stored in order 5,2,0,1,3,4,6,7,8...111.

*/

#include "speccy/core/Snapshot.h"
#include "app/Config.h"
#include "app/ESPectrum.h"
#include "speccy/core/MemESP.h"
#include "speccy/z80/CPU.h"
#include "speccy/video/Video.h"
#include "NextReg.h"
#include "NexLaunch.h"
#include "ui/OSDMain.h"
#include "fs/FileUtils.h"
#include "speccy/z80/z80.h"
#include "speccy/devices/storage/DivMMC.h"
#include "app/Debug.h"
#include "app/TryAlloc.h"
#include <string.h>

using namespace std;

static bool readExact(FIL* f, void* dst, UINT len) {
    UINT br = 0;
    return f_read(f, dst, len, &br) == FR_OK && br == len;
}

static bool skipBytes(FIL* f, FSIZE_t len) {
    return len <= f_size(f) - f_tell(f) &&
           f_lseek(f, f_tell(f) + len) == FR_OK;
}

// Read one 8K page. Banks 8+ live in butter PSRAM: the SD driver may DMA
// into the destination, bypassing the XIP cache — go through an SRAM bounce
// so the CPU write path keeps the cache coherent (banks 0-7 are plain SRAM
// and could read directly, but one path keeps this simple).
static bool readPage(FIL* f, uint8_t* dst) {
    // Heap for the read, not a static: the 2 KB used to be paid by every machine.
    constexpr int kBounce = 2048;
    ScopedHeap hb(kBounce);
    uint8_t* bounce = (uint8_t*)hb.p;
    if (!bounce) return false;
    for (int off = 0; off < 0x2000; off += kBounce) {
        if (!readExact(f, bounce, kBounce)) return false;
        memcpy(dst + off, bounce, kBounce);
    }
    return true;
}

// Read one 16K bank into Next RAM (two 8K pages)
static bool readBank(FIL* f, unsigned bank) {
    if (bank >= MemESP::NEXT_PAGES / 2) return false;
    if (!readPage(f, MemESP::nextPgPtr(bank * 2))) return false;
    return readPage(f, MemESP::nextPgPtr(bank * 2 + 1));
}

bool FileNEX::load(const string& nex_fn) {

    // .NEX needs the Next machine with its RAM carve-out in place. If we are
    // on another machine, save the file as the boot snapshot and reboot into
    // Next (setup() skips the pref_arch override when ram_file is set).
    if (!Z80Ops::isNext || !MemESP::nextRamReady) {
        if (butter_psram_size() < (2u << 20)) {
            OSD::osdCenteredMsg("Spectrum Next needs 2MB+ PSRAM", LEVEL_WARN, 2000);
            return false;
        }
        Config::arch = A_NEXT;
        Config::romSet = R_NEXT;
        Config::ram_file = nex_fn;
        Config::save();
        OSD::esp_settings_reboot();
        return true; // not reached
    }

    FIL* file = fopen2(nex_fn.c_str(), FA_READ);
    if (!file) {
        OSD::osdCenteredMsg("Error opening file:\n" + nex_fn + "\n", LEVEL_INFO, 5000);
        return false;
    }

    uint8_t hdr[512];
    if (!readExact(file, hdr, sizeof(hdr)) ||
        hdr[0] != 'N' || hdr[1] != 'e' || hdr[2] != 'x' || hdr[3] != 't') {
        OSD::osdCenteredMsg("Bad NEX file:\n" + nex_fn + "\n", LEVEL_INFO, 5000);
        fclose2(file);
        return false;
    }

    uint8_t numBanks = hdr[9];
    uint8_t screens = hdr[10];
    uint8_t border = hdr[11] & 7;
    uint16_t sp = hdr[12] | (hdr[13] << 8);
    uint16_t pc = hdr[14] | (hdr[15] << 8);
    uint8_t preserveRegs = hdr[134];
    uint8_t entryBank = hdr[139];
    (void)numBanks;

    // The OS loader establishes the application's working directory and keeps
    // its filesystem alive. Host-only bank injection cannot supply those APIs.
    const int native = NexLaunch::start(nex_fn);
    if (native) {
        fclose2(file);
        return native > 0;
    }

    // Reset alone leaves the BIOS entry state (ROM0, 3.5 MHz, sprites off).
    // NEX programs expect the application defaults installed by .nexload.
    if (!preserveRegs) {
        ESPectrum::reset();
    }
    // A .NEX runs on a BOOTED machine: the TBBLUE bootrom / config-mode
    // window must be gone even when the file preserves nextreg state. The
    // reboot-from-another-machine path (Config::ram_file) lands here right
    // after setup() armed the bootrom — without this the loaded PC fetches
    // the boot loader's bytes instead of the program's.
    MemESP::nextBootRomActive = false;
    MemESP::nextConfigMode = false;
    MemESP::applyNextConfigWindow();

    if (!preserveRegs) {
        // Select 48K BASIC for IM1/RST services, not the NextZXOS boot ROM.
        // Clear a staged Alt-ROM selection promoted by the soft reset too.
        NextReg::write(0x8C, 0);
        NextReg::write(0x08, (NextReg::reg[0x08] & 0x10) | 0xCE);
        NextReg::write1FFD(0x04);
        NextReg::write7FFD(0x10);
        NextReg::write(0x06, (NextReg::reg[0x06] & ~0x10) | 0x08);
        NextReg::write(0x07, 3); // .nexload starts applications at 28 MHz
        NextReg::write(0x12, 9);
        NextReg::write(0x13, 12);
        NextReg::write(0x15, 1); // sprites enabled
        NextReg::write(0x42, 15);
        NextReg::write(0x4A, 0);
        NextReg::contendRecalc();
    }

    // Loading via the host does not execute the dot-command's exit trampoline.
    // Release its divMMC overlay even when preserving the Next registers.
    DivMMC::automap = DivMMC::conmem = false;
    DivMMC::applyMapping();
    // .nexload establishes banks 5/2 independently of PreserveNextRegisters.
    for (uint8_t s = 2; s < 6; ++s)
        NextReg::write(0x50 + s, s < 4 ? s + 8 : s);

    auto readFailure = [&]() {
        fclose2(file);
        OSD::osdCenteredMsg("Error reading NEX file:\n" + nex_fn, LEVEL_WARN, 2000);
        return false;
    };

    // Optional palette block (for Layer2/LoRes screens, unless bit7 set)
    bool hasPalette = (screens & 0x05) && !(screens & 0x80);
    if (hasPalette) {
        // 256 entries x 2 bytes, same encoding as nextreg 0x44 two-write
        NextReg::write(0x43, 1 << 4); // select Layer 2 first palette for writes
        NextReg::write(0x40, 0);
        for (int i = 0; i < 256; i++) {
            uint8_t pair[2];
            if (!readExact(file, pair, 2)) return readFailure();
            NextReg::write(0x44, pair[0]);
            NextReg::write(0x44, pair[1]);
        }
        NextReg::write(0x43, 0);
    }

    // Loading screens
    if (screens & 0x01) { // Layer 2 (48K into banks 9,10,11)
        if (!readBank(file, 9) || !readBank(file, 10) || !readBank(file, 11))
            return readFailure();
        NextReg::write(0x12, 9);
        NextReg::writeLayer2Port(0x02);
    }
    if (screens & 0x02) { // ULA (6912 into bank 5)
        if (!readExact(file, MemESP::ram[5].direct(), 6912)) return readFailure();
    }
    if ((screens & 0x04) && !skipBytes(file, 12288)) return readFailure(); // LoRes
    if ((screens & 0x08) && !skipBytes(file, 12288)) return readFailure(); // Timex HiRes
    if ((screens & 0x10) && !skipBytes(file, 12288)) return readFailure(); // Timex HiCol

    // 16K banks in stored order
    static const uint8_t first8[] = { 5, 2, 0, 1, 3, 4, 6, 7 };
    for (int i = 0; i < 8; i++) {
        if (hdr[18 + first8[i]]) {
            if (!readBank(file, first8[i])) return readFailure();
        }
    }
    for (unsigned b = 8; b < 112; b++) {
        if (hdr[18 + b]) {
            if (!readBank(file, b)) return readFailure();
        }
    }

    fclose2(file);

    // Entry state
    NextReg::write(0x56, entryBank * 2);
    NextReg::write(0x57, entryBank * 2 + 1);

    VIDEO::borderColor = border;
    VIDEO::brd = VIDEO::border32[border];

    Z80::reset();
    Z80::setRegSP(sp);
    Z80::setRegBC(0); // no open esxDOS file handle
    Z80::setIM(Z80::IM1);
    Z80::setIFF1(false);
    Z80::setIFF2(false);
    if (pc != 0)
        Z80::setRegPC(pc);

    CPU::tstates = 0;
    CPU::global_tstates = 0;

    Debug::log("FileNEX: loaded '%s' PC=%04X SP=%04X entryBank=%d",
               nex_fn.c_str(), pc, sp, entryBank);
    return true;
}
