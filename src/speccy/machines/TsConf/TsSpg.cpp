/*

TS-Conf .spg ("SpectrumProg") loader — the distribution format of ZX-Evolution
TS-Configuration software (games, demos). Loaded the way UnrealSpeccy loads it
(snapshot.cpp readSPG, tslabs/zx-evo, GPL-2.0+): reset the machine, unpack the
blocks straight into the 16 KB pages, set the few CPU/TS registers the header
names, jump. Format: pentevo/docs/Formats/SPGv1_0.txt. The MegaLZ and Hrust1
depackers are ports of pentevo/unreal/Unreal/depack.cpp with output/input
bounds added — a corrupt file on the SD must not write past the page.

Not modelled (as in Unreal): the "pager" (<=32 bytes) and "resident" (16 bytes)
the header locates — those are stubs a disk-based launcher installs for the
program; nothing in the three test titles calls them. SPG v0.x (2 KB blocks,
15 descriptors) is not supported.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include "speccy/core/Snapshot.h"
#include "fs/FileUtils.h"
#include "app/Config.h"
#include "app/ESPectrum.h"
#include "speccy/core/MemESP.h"
#include "TsConf.h"
#include "speccy/video/Video.h"
#include "app/Buffer.h"
#include "speccy/devices/storage/DivMMC.h"
#include "ui/OSDMain.h"
#include "app/Debug.h"
#include "speccy/z80/z80.h"
#include <cstring>

#include "TsSpgDepack.h"
using tsspg::dehrust;
using tsspg::demlz;

// ---------------------------------------------------------------- loader ----

static bool spgFail(const string& fn, const char* why) {
    Debug::log("[SPG] %s: %s", fn.c_str(), why);
    OSD::osdCenteredMsg(string("SPG: ") + why + "\n" + fn, LEVEL_WARN, 3000);
    return false;
}

bool FileSPG::load(const string& fn) {
    FIL* f = fopen2(fn.c_str(), FA_READ);
    if (!f) return spgFail(fn, "cannot open");

    uint8_t hdr[0x400];
    UINT br = 0;
    if (f_read(f, hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr) ||
        memcmp(hdr + 0x20, "SpectrumProg", 12) != 0) {
        fclose2(f);
        return spgFail(fn, "not a SpectrumProg file");
    }
    if ((hdr[0x2C] >> 4) != 1) {
        fclose2(f);
        return spgFail(fn, "only SPG v1.x is supported");
    }
    const uint16_t start = (uint16_t)(hdr[0x30] | (hdr[0x31] << 8));
    const uint16_t sp    = (uint16_t)(hdr[0x32] | (hdr[0x33] << 8));
    const uint8_t  page3 = hdr[0x34];
    const uint8_t  clk   = hdr[0x35];
    unsigned nblk = hdr[0x3A] | (hdr[0x3B] << 8);
    if (nblk == 0 || nblk > 256) nblk = 256;

    // The machine: TS-Conf, its own page strip. Crossing the strip boundary
    // reboots inside requestMachine — LoadSnapshot has persisted this path as
    // ram_file, so setup() lands back here with the machine ready.
    if (Config::arch != A_TSCONF) {
#if !defined(VGA_HDMI)
        fclose2(f);
        return spgFail(fn, "TS-Conf needs a VGA/HDMI build");
#else
        if (butter_psram_size() < (1u << 20)) {
            fclose2(f);
            return spgFail(fn, "TS-Conf needs QSPI PSRAM");
        }
        // Same cascade MachineSwitch applies: DivMMC's #AF collides with the
        // TS register file.
        if (Config::esxdos) { Config::esxdos = 0; DivMMC::init(); }
        // R_NONE: keep whichever TS-BIOS set the user picked.
        Config::requestMachine(A_TSCONF, R_NONE);
#endif
    }
    // (No Gigascreen handling here: VIDEO::gigascreenModeGate() suspends it from
    // EndFrame the moment the program puts a whole-line mode up, and hands it back
    // if the program runs in the standard ZX mode.)
    ESPectrum::resetForLoad();

    // Every page the file does not carry must be BLANK, or the previous program
    // shows through. An .spg names only the blocks it needs; a reset does not
    // touch RAM (deliberately — see ESPectrum::reset), so launching demos one
    // after another left the earlier one's bitmap in every page the new one does
    // not write, which in 256c/TSU is simply displayed: garbage in the lower part
    // of the screen that survived F11 and cleared only on F12, where setup()'s
    // powerOnDramFill happens to lay down 0x00/0xFF stripes that usually map to
    // dark CRAM cells. The snapshot loaders already zero their void pages
    // (Snapshot.cpp cleanup() calls); this is the same rule for .spg.
    // cleanup() is the per-page zero and handles every backing; TS-Conf pages ARE
    // MemESP::ram[] (TsConf::pagePtr indexes it), and they are POINTER-backed
    // after the boot residency self-heal, so this is a straight memset of
    // MEM_PG_CNT x 16 KB (4 MB, Config::TSCONF_PAGES) into butter PSRAM.
    {
        const uint64_t t0 = esp_timer_get_time();
        for (int i = 0; i < MEM_PG_CNT; i++) MemESP::ram[i].cleanup();
        Debug::log("[SPG] %d RAM pages cleared in %u us",
                   (int)MEM_PG_CNT, (unsigned)(esp_timer_get_time() - t0));
    }
    // Same rule for the video register file, which leaks the same way: TsConf::reset
    // clears CRAM and SFILE only on a COLD reset (`if (cold)`, i.e. the first TS
    // reset of the session), so a warm one — F11, and this load — leaves the
    // previous program's 256 palette cells and its 85 sprite descriptors standing.
    // A demo that programs only part of either inherits the rest: stale CRAM cells
    // recolour whatever uses them, stale SFILE entries put the previous demo's
    // sprites on top of the new one's picture. The ZX bank right below is written
    // again immediately, so only 0x00..0xEF is cleared here.
    for (int i = 0; i <= 0xEF; i++) TsConf::cram[i] = 0;
    for (int i = 0; i < 256; i++)   TsConf::sfile[i] = 0;
    TsConf::sfileGen++;

    // Blocks: descriptor = {addr:5 (x512 in page) .. last:7, size:5 (x512 - 1)
    // .. comp:6-7, page}. Compressed input is at most 16 KB; output is bounded
    // by the page end.
    uint8_t* buf = (uint8_t*)Buffer::palloc(16384, Buffer::NEED_POINTER);
    if (!buf) { fclose2(f); return spgFail(fn, "no memory for the block buffer"); }
    unsigned loaded = 0, skipped = 0;
    for (unsigned i = 0; i < nblk; i++) {
        const uint8_t* d = hdr + 0x100 + i * 3;
        const uint32_t off  = (uint32_t)(d[0] & 0x1F) << 9;
        const uint32_t size = ((uint32_t)(d[1] & 0x1F) + 1) << 9;
        const uint8_t  comp = d[1] >> 6;
        const uint8_t  page = d[2];
        const bool     last = d[0] & 0x80;
        if (f_read(f, buf, size, &br) != FR_OK || br != size) {
            Debug::log("[SPG] block %u: short read (%u of %u)", i, (unsigned)br, (unsigned)size);
            break;
        }
        uint8_t* pg = TsConf::pagePtr(page);
        if (!pg) { skipped++; if (last) break; continue; }
        uint8_t* dst = pg + off;
        uint8_t* dst_end = pg + MEM_PG_SZ;
        switch (comp) {
            case 0: {
                size_t n = size;
                if (dst + n > dst_end) n = (size_t)(dst_end - dst);
                memcpy(dst, buf, n);
                break;
            }
            case 1: demlz(dst, dst_end, buf, size); break;
            case 2: dehrust(dst, dst_end, buf, size); break;
            default: skipped++; break;
        }
        loaded++;
        if (last) break;
    }
    Buffer::pfree(buf);
    fclose2(f);

    // Launch state (SPGv1_0.txt "defaults at launch" + Unreal readSPG): ROM
    // Basic48 at #0000 (#7FFD bit 4 set, DOS off), page 5 at #4000, page 2 at
    // #8000, Page3 from the header, ZX screen 0, IM 1, I = #3F; the standard
    // ZX palette in CRAM's ZX bank (load_spec_colors); ZCLK and EI from the
    // header's clock byte; IY/HL' as the reference sets them.
    ESPectrum::trdos = false;
    TsConf::r.p7ffd = 0x10;
    TsConf::r.memconf = 0;
    TsConf::r.page[0] = 0; TsConf::r.page[1] = 5; TsConf::r.page[2] = 2; TsConf::r.page[3] = page3;
    TsConf::r.vpage = TsConf::r.vpage_d = 5;
    // ZCLK from the header; CACHE stays ON — on hardware an .spg is launched
    // from a TS-BIOS that booted with its Setup's "CPU Cache [ON]" (RESET2 writes
    // SysConfig = cfrq | cach<<2), and Unreal's readSPG sets only zclk, leaving
    // cacheconf as TS-BIOS left it. The DRAM model (TsConf.cpp) charges 14 MHz
    // wait states per cache MISS, so a cold cache here would run every .spg
    // slower than the real machine does.
    TsConf::r.sysconf = (uint8_t)(0x04 | (clk & 0x03));
    TsConf::r.cacheconf = 0x0F;
    MemESP::pagingLock = 0;
    TsConf::setBanks();
    TsConf::applyZclk(true);   // the header's clock byte is the program's choice — show it
    {
        static const uint16_t zx555[16] = {
            0x0000, 0x0010, 0x4000, 0x4010, 0x0200, 0x0210, 0x4200, 0x4210,
            0x0000, 0x0018, 0x6000, 0x6018, 0x0300, 0x0318, 0x6300, 0x6318,
        };
        for (int i = 0; i < 16; i++) TsConf::cram[0xF0 + i] = zx555[i];
        VIDEO::tsCramDirty = true;
    }
    Z80::setRegI(0x3F);
    Z80::setIM(Z80::IntMode::IM1);
    Z80::setRegIY(0x5C3A);
    Z80::setRegHLx(0x2758);
    Z80::setRegSP(sp);
    Z80::setRegPC(start);
    Z80::setIFF1((clk & 0x04) != 0);
    Z80::setIFF2((clk & 0x04) != 0);

    Debug::log("[SPG] %s: start=%04X sp=%04X page3=%u clk=%u ei=%u blocks=%u loaded=%u skipped=%u",
               fn.c_str(), start, sp, page3, clk & 3, (clk >> 2) & 1, nblk, loaded, skipped);
    return true;
}
