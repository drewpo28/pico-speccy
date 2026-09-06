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

#include "Snapshot.h"
#include "FileUtils.h"
#include "Config.h"
#include "ESPectrum.h"
#include "MemESP.h"
#include "TsConf.h"
#include "Video.h"
#include "Buffer.h"
#include "DivMMC.h"
#include "OSDMain.h"
#include "Debug.h"
#include "Z80_JLS/z80.h"
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
        Config::requestMachine(A_TSCONF, R_TSCONF);
#endif
    }
    // Gigascreen is incompatible with the TS-Conf renderer (VIDEO::disableGigascreenForProfi).
    if (Config::gigascreen_enabled) VIDEO::disableGigascreenForProfi();
    ESPectrum::reset();

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
    TsConf::r.sysconf = (uint8_t)(clk & 0x03);
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
