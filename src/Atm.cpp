// pico-speccy — MicroART ATM-Turbo 1 / 2+ memory manager and system ports.
// See Atm.h for the hardware description and the references.
#include "Atm.h"

#include <string.h>
#include "Buffer.h"
#include "CPU.h"
#include "Config.h"
#include "Debug.h"
#include "ESPectrum.h"
#include "IDE.h"
#include "LEDIndicators.h"
#include "MemESP.h"
#include "OSDMain.h"
#include "Ports.h"
#include "RomOverlay.h"
#include "Video.h"
#include "Z80_JLS/z80.h"

uint8_t g_atm_ro = 0;

namespace Atm {

bool     atm1 = false;
uint8_t  p7ffd = 0;
uint8_t  aFE = 0x80, aFB = 0x80, pFDFD = 0;
uint16_t a77 = 0;
uint8_t  p77 = 0;
uint8_t  pF7[8] = { 0 };
uint8_t  pal[16] = { 0 };
bool     beta = false;
bool     palDirty = false;

static const atm_rom_page_t* s_tbl = nullptr;   // bound romset's page table
static uint8_t  s_npages = 0;
static const uint8_t* s_rom[8] = { nullptr };  // resolved pages (flash or PSRAM)
static uint8_t* s_flat = nullptr;              // PSRAM block holding the flattened pages
static RomsetIdx s_flat_rs = R_NONE;           // romset s_flat currently holds
static bool     s_flat_warned = false;

// Filler for a page that could not be resolved (no PSRAM): reads 0xFF like an
// empty ROM socket. Aligned so the pointer is a valid flash address (writes are
// dropped by g_atm_ro anyway).
static const uint8_t kFF[16] __attribute__((aligned(4))) = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

uint8_t romPageCount() { return s_npages ? s_npages : 4; }

void bindRoms(RomsetIdx rs, const atm_rom_page_t* pages, uint8_t n) {
    atm1 = isAtm1Romset(rs);
    if (s_tbl != pages) {
        s_tbl = pages;
        s_npages = n;
        for (int i = 0; i < 8; i++) s_rom[i] = nullptr;
    }
    (void)rs;
}

// Resolve every page of the bound table. A page that is a raw flash array is used
// in place; one that is an overlay (or the all-0xFF page) is flattened into one
// butter-PSRAM block, because an ATM ROM page may sit in any CPU window and
// MemESP resolves overlays for window 0 only. Runs on the first reset after the
// Buffer pools exist (requestMachine binds before Buffer::initPools on a boot).
static void resolveRoms() {
    if (!s_tbl) return;
    const RomsetIdx rs = Config::romSetAtm;
    bool need = false;
    for (int i = 0; i < s_npages; i++)
        if (s_tbl[i].overlay || !s_tbl[i].base) need = true;
    if (need && (s_flat_rs != rs || !s_flat)) {
        if (s_flat) { Buffer::pfree(s_flat); s_flat = nullptr; }
        s_flat_rs = R_NONE;
        if (Buffer::butterPoolReady()) {
            void* p = Buffer::palloc((size_t)s_npages * MEM_PG_SZ,
                                     Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
            if (p && (uintptr_t)p < 0x11000000u) {   // landed on the heap: not for 128 KB
                Buffer::pfree(p);
                p = nullptr;
            }
            s_flat = (uint8_t*)p;
        }
        if (s_flat) {
            for (int i = 0; i < s_npages; i++) {
                uint8_t* dst = s_flat + (size_t)i * MEM_PG_SZ;
                const atm_rom_page_t& pg = s_tbl[i];
                if (!pg.base) {
                    memset(dst, 0xFF, MEM_PG_SZ);
                    if (pg.overlay) rom_overlay_flatten(pg.overlay, dst, dst);
                } else if (pg.overlay) {
                    rom_overlay_flatten(pg.overlay, pg.base, dst);
                }
            }
            s_flat_rs = rs;
            Debug::log("[ATM] ROM pages flattened into PSRAM @%p (%u pages)", s_flat, (unsigned)s_npages);
        } else if (!s_flat_warned) {
            s_flat_warned = true;
            Debug::log("[ATM] no butter PSRAM for the ROM pages - overlays unapplied");
        }
    }
    for (int i = 0; i < s_npages; i++) {
        const atm_rom_page_t& pg = s_tbl[i];
        if (pg.base && !pg.overlay)      s_rom[i] = pg.base;
        else if (s_flat)                 s_rom[i] = s_flat + (size_t)i * MEM_PG_SZ;
        else                             s_rom[i] = pg.base ? pg.base : kFF;
    }
}

// The DOS signal is raised by /CPM on the 2+ only. On the ATM1 the CP/M mode
// (#FE A7=0) does NOT open the TR-DOS ports — "в этом режиме недоступны порты
// TR-DOS, так что для работы с дисководом нужно прыгать в обычный режим"
// (atmdscr.htm, ATM-turbo 1): its BIOS drives the FDC from the SYSTEM ROM after a
// CALL into #3Dxx (see trdosTrap).
static inline bool cpmOn() { return !atm1 && !(a77 & 0x200); }

static void dosRecalc() {
    ESPectrum::trdos = beta || cpmOn();
}

// ---------------------------------------------------------------------- paging --

static uint8_t s_ro;

static inline void mapRom(int w, uint8_t page) {
    const uint8_t n = romPageCount();
    const uint8_t* p = s_rom[page % n];
    if (!p) p = kFF;
    MemESP::ramCurrent[w] = (uint8_t*)p;
    mem_desc_t::bank_dirty[w] = &mem_desc_t::dirty_sink;
    s_ro |= (uint8_t)(1u << w);
}

static inline void mapRam(int w, uint32_t page) {
    MemESP::ramCurrent[w] = MemESP::ram[page & (MEM_PG_CNT - 1)].sync(w);
}

void remap() {
    s_ro = 0;
    const uint8_t n = romPageCount();
    if (atm1) {
        // #FDFD D1..D0 extend the #C000 page to 512 KB; D2 is the ROM-disk select
        // (upper 64 KB of a 27010 — this set is a 27512, so it has nothing to show).
        const uint32_t pg3 = (p7ffd & 7) | ((pFDFD & 3) << 3);
        if (!(aFE & 0x80)) {
            // CP/M mode: RAM page 0 at #0000 and page 4 at #4000.
            mapRam(0, 0);
            mapRam(1, 4);
        } else {
            if (p7ffd & 0x20) aFB &= 0x7F;                   // the 48 lock drops CPSYS
            if (beta && (pFDFD & 0x08)) aFB |= 0x80;         // DOS + #FDFD D3 raises it
            uint8_t r;
            if (aFB & 0x80) r = 0;                           // SYSTEM ROM
            else if (beta)  r = 1;                           // TR-DOS
            else            r = (p7ffd & 0x10) ? 3 : 2;      // 48 / 128
            mapRom(0, r);
            mapRam(1, 5);
        }
        mapRam(2, 2);
        mapRam(3, pg3);
        MemESP::bankLatch = pg3 & (MEM_PG_CNT - 1);
    } else if (!(a77 & 0x100)) {
        // PEN = 0: the last ROM page (the BIOS) in every window.
        for (int w = 0; w < 4; w++) mapRom(w, (uint8_t)(n - 1));
        MemESP::bankLatch = p7ffd & 7;
    } else {
        const int set = (p7ffd & 0x10) ? 4 : 0;
        const uint8_t dos = ESPectrum::trdos ? 1 : 0;
        for (int w = 0; w < 4; w++) {
            const uint8_t v = pF7[set + w];
            const uint8_t page = (uint8_t)(~v & 0x3F);
            switch (v & 0xC0) {
                case 0xC0: mapRam(w, (page & ~7u) | (p7ffd & 7)); break;   // RAM, low bits from #7FFD
                case 0x40: mapRam(w, page); break;                         // RAM from the register
                case 0x80: mapRom(w, (uint8_t)((page & 0xFE) | dos)); break; // ROM, bit 0 = DOS
                default:   mapRom(w, page); break;                         // ROM from the register
            }
        }
        MemESP::bankLatch = p7ffd & 7;
    }
    g_atm_ro = s_ro;
    MemESP::videoLatch = (p7ffd >> 3) & 1;
    MemESP::romLatch = (p7ffd >> 4) & 1;
    MemESP::romInUse = 0;
    // recoverPage0() must not touch window 0 on ATM (it would map rom[romInUse]
    // over the memory manager's choice): the +3's "someone else owns page 0" flag.
    MemESP::p3special = 2;
    MemESP::pagingLock = (p7ffd >> 5) & 1;
    for (int w = 0; w < 4; w++) MemESP::ramContended[w] = false;
    VIDEO::grmem = MemESP::ram[MemESP::videoLatch ? 7 : 5].direct();
}

void reset() {
    resolveRoms();
    p7ffd = 0;
    beta = false;
    if (atm1) {
        // BIOS 1.03/1.04 keeps the CP/M body XOR-ed at #22B9 with a key read off the
        // #FE bit-7 "Z" signal (#2024: 16 samples after a delay). It first counts the
        // frame (#2005 -> (#5F74)) and, when that says turbo, takes a second delay
        // and adds #82 — a Z pattern for 7 MHz that the Unreal model (atm1FeBit7)
        // does not reproduce, so the key came out #0081 instead of #FFF4 and CP/M
        // decrypted into garbage (hw 2026-09-26: hang before the first disk read,
        // gone without turbo). The ATM-Turbo 1 has no guest turbo register here,
        // so every reset starts it at 3.5 MHz; Alt+F2 after the boot still works.
        if (ESPectrum::multiplicator) {
            ESPectrum::multiplicator = 0;
            CPU::updateStatesInFrame();
        }
        // Unreal reset(): aFE = 0x80 (no CP/M, EGA), aFB = 0x80 (CPSYS -> SYSTEM ROM).
        aFE = 0x80;
        aFB = 0x80;
        pFDFD = 0;
        a77 = 0x0200;
        p77 = 0x20;
    } else {
        // Unreal/MAME reset: #77 written with address 0 and data 0 — PEN = 0 (the BIOS
        // in all four windows), /CPM = 0 (DOS signal up), /PEN2 = 0, EGA, 3.5 MHz,
        // frame INT off until the BIOS enables it.
        a77 = 0;
        p77 = 0;
        aFE = 0xFE;
        aFB = 0;
        pFDFD = 0;
    }
    memset(pF7, 0, sizeof pF7);
    dosRecalc();
    remap();
}

// ------------------------------------------------------------------ palette --

// ATM-Turbo 2+: data bits (inverted) grbG--RB -> 2 bits per channel.
// ATM-Turbo 1:  --grbGRB. Unreal atm_zc_tables(); MAME atm_port_ff_w agrees for the 2+.
uint32_t palRgb(uint8_t i) {
    const uint8_t v = (uint8_t)~pal[i & 15];
    uint8_t R, G, B;
    if (atm1) {
        R = (uint8_t)(((v >> 1) & 1) * 0xAA + ((v >> 4) & 1) * 0x55);
        G = (uint8_t)(((v >> 2) & 1) * 0xAA + ((v >> 5) & 1) * 0x55);
        B = (uint8_t)(((v >> 0) & 1) * 0xAA + ((v >> 3) & 1) * 0x55);
    } else {
        R = (uint8_t)(((v >> 1) & 1) * 0xAA + ((v >> 6) & 1) * 0x55);
        G = (uint8_t)(((v >> 4) & 1) * 0xAA + ((v >> 7) & 1) * 0x55);
        B = (uint8_t)(((v >> 0) & 1) * 0xAA + ((v >> 5) & 1) * 0x55);
    }
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}

static void palWrite(uint8_t data) {
    const uint8_t idx = VIDEO::borderColor & 15;
    if (pal[idx] != data) {
        pal[idx] = data;
        palDirty = true;
        VIDEO::atmPaletteChanged();
    }
}

uint8_t borderBright() { return VIDEO::borderColor & 8; }

// ---------------------------------------------------------------- video mode --

VMode videoMode() {
    if (atm1) {
        switch ((aFE >> 5) & 3) {
            case 0:  return VM_EGA;
            case 1:  return VM_HIRES;
            default: return VM_ZX;
        }
    }
    switch (p77 & 7) {
        case 0:  return VM_EGA;
        case 2:  return VM_HIRES;
        case 6:  return VM_TEXT;
        default: return VM_ZX;       // 3 = ZX; the undefined codes render as ZX (MAME)
    }
}

// --------------------------------------------------------------------- ports --

static void write7ffd(uint8_t data) {
    if (p7ffd & 0x20) return;                  // 48 lock
    const uint8_t old = p7ffd;
    p7ffd = data;
    if ((old ^ data) & 0x08) VIDEO::gigascreenAutoFlip();
    remap();
    LED::touchW(LED::RAM);
}

static void write77(uint16_t address, uint8_t data) {
    const uint8_t oldMode = p77 & 7;
    const uint8_t oldTurbo = p77 & 0x08;
    a77 = address;
    p77 = data;
    dosRecalc();
    remap();
    // D3: the machine's own clock (MAME atm_update_cpu: 3.5 / 7 MHz). Applied on the
    // EDGE only, so an Alt+F2 override survives the BIOS's many #77 rewrites with an
    // unchanged D3. A clock change is a change of UNITS for CPU::tstates (the TS-Conf
    // lesson): rescale it, or a 7 -> 3.5 dip late in the frame ends the frame at once.
    if ((data & 0x08) != oldTurbo) {
        const uint8_t want = (data & 0x08) ? 1 : 0;
        const uint8_t old = ESPectrum::multiplicator;
        if (want != old) {
            CPU::tstates = (want > old) ? (CPU::tstates << (want - old)) : (CPU::tstates >> (old - want));
            ESPectrum::multiplicator = want;
            CPU::updateStatesInFrame();
            OSD::notifyClock(want ? " CPU: 7 MHz " : " CPU: 3.5 MHz ");
        }
    }
    if ((data & 7) != oldMode) VIDEO::atmVideoModeChanged();
}

void feWrite(uint16_t address) {
    if (!atm1) return;
    const uint8_t a = (uint8_t)address;
    const uint8_t old = aFE;
    aFE = a;
    if ((old ^ a) & 0x80) {
        dosRecalc();
        remap();
    }
    if ((old ^ a) & 0x60) VIDEO::atmVideoModeChanged();
}

// IDE (ATM-Turbo 2+, xx0F family, DOS ports only): A7..A5 = register, A8 = the
// high-byte latch of the 16-bit data register (Unreal IDE_ATM, MAME ata_r/ata_w).
static inline bool ideLive(uint16_t address) {
    return !atm1 && IDE::portScheme == IDE::ATM && (address & 0x1F) == 0x0F;
}

bool portWrite(uint16_t address, uint8_t data) {
    const uint8_t lo = (uint8_t)address;
    if (atm1) {
        if (address & 2) return false;
        if ((address & 0x8202) == 0x0000) { palWrite(data); return true; }        // #7DFD
        if ((address & 0x8202) == 0x0200) { write7ffd(data); return true; }       // #7FFD
        if ((address & 0x8202) == 0x8000) {                                       // #FDFD
            pFDFD = data;
            remap();
            LED::touchW(LED::RAM);
            return true;
        }
        return false;
    }
    // ATM-Turbo 2+
    if (!(address & 0x8002)) { write7ffd(data); return true; }                    // #7FFD
    if (!ESPectrum::trdos) return false;
    // %nXnnnnXX 0nn101n1 / 1nn101n1: A6, A5, A1 are not decoded.
    if ((lo & 0x9D) == 0x15) { write77(address, data); return true; }
    if ((lo & 0x9D) == 0x95) {
        pF7[((p7ffd & 0x10) >> 2) | (address >> 14)] = data;
        remap();
        LED::touchW(LED::RAM);
        return true;
    }
    if (ideLive(address)) {
        LED::touchW(LED::IDE);
        if (address & 0x100) IDE::write_latch(data);
        else {
            const uint8_t reg = (address >> 5) & 7;
            if (reg == 0) IDE::write_data_low(data); else IDE::write8(reg, data);
        }
        return true;
    }
    // #FF family with /PEN2 = 0: palette. NOT consumed — Unreal also hands the byte
    // to the Beta-128 system register ("don't return").
    if ((lo & 0x9F) == 0x9F && !(a77 & 0x4000)) palWrite(data);
    return false;
}

// PAL-detect quirk of the ATM-Turbo 1 (Unreal atm450_z): #FE bit 7 reads 0 in three
// short windows of the frame, 1 everywhere else.
static inline uint8_t atm1FeBit7() {
    const uint32_t t = CPU::tstates >> ESPectrum::multiplicator;
    if ((t - 7200u) < 40u || (t - 7284u) < 40u || (t - 7326u) < 40u) return 0;
    return 0x80;
}

bool portRead(uint16_t address, uint8_t& v) {
    if (atm1) {
        // IN #FB (%nnnnnnnn Xnnnn0n1): the Centronics status, and the low address
        // byte is latched — A7 is CPSYS. D7 = BUSY (0 = free), D6 = ULINE, D5..D0 = 1;
        // a printer that always reads busy would hang LPRINT.
        if ((address & 0x05) == 0x01) {
            const uint8_t old = aFB;
            aFB = (uint8_t)address;
            if ((old ^ aFB) & 0x80) remap();
            v = 0x7F;
            return true;
        }
        // #FA (%nnnnnnnn nnnnn0n0): the external system bus — nothing attached.
        if ((address & 0x05) == 0x00) { v = 0xFF; return true; }
        return false;
    }
    if (ESPectrum::trdos && ideLive(address)) {
        LED::touchR(LED::IDE);
        if (address & 0x100) v = IDE::read_latch();
        else {
            const uint8_t reg = (address >> 5) & 7;
            v = (reg == 0) ? IDE::read_data_low() : IDE::read8(reg);
        }
        return true;
    }
    // Printer status #FB (%nnnnn011), as on the ATM1 but without the CPSYS latch.
    // (#FA, the external bus, is left to the rest of the decode: the VGM cards
    // answer on even ports there.)
    if ((address & 0x07) == 0x03) { v = 0x7F; return true; }
    // #FF outside DOS: the attribute port the ATM1 lacked ("порт атрибутов"), i.e.
    // what the video controller is fetching — the 48K floating bus (same frame).
    if ((address & 0xFF) == 0xFF && !ESPectrum::trdos) { v = Ports::getFloatBusData48(); return true; }
    // A15 = 0, A9 = 1, A1 = 0: the IDE INTRQ / DAC status port — D6 = INTRQ.
    // Below the DOS ports, as in Unreal (in(): the CF_DOSPORTS block returns first):
    // `IN A,(#FF)` puts A on A8-A15, so with DOS up a Beta register read can match
    // this decode and must still reach the WD1793 (the CP/M BIOS polls INTRQ/DRQ
    // that way).
    if (ESPectrum::trdos && (address & 0x1F) == 0x1F) return false;
    if ((address & 0x8202) == 0x0200) {
        v = 0x3F | 0x40;
        return true;
    }
    return false;
}

// Keyboard read post-processing for the ATM-Turbo 1 (called by Ports::input).
uint8_t feRead(uint8_t v) { return atm1 ? (uint8_t)((v & 0x7F) | atm1FeBit7()) : v; }

// ------------------------------------------------------------------- TR-DOS --

void trdosTrap(uint8_t pcH) {
    if (!Config::betadisk) return;
    if (!beta) {
        // Enter at #3Dxx with the 48 ROM selected (#7FFD D4) and ROM actually in
        // window 0 — "for Scorp, ATM-1/2 and KAY, TR-DOS not started on executing
        // RAM 3Dxx" (Unreal memory.cpp).
        // ATM1 CPSYS (the system ROM paged by an IN from #FB with A7=1) opens the
        // TR-DOS ports the same way, the ROM staying where it is: "сохраняется
        // возможность доступа к портам TR-DOS (без включения ПЗУ TR-DOS). Для этого
        // просто надо сделать CALL в промежуток от 15616 до 15871" (atmdscr.htm).
        // The CP/M BIOS does exactly that (ROM 0: CALL #3DFD = RET, then OUT (#FF)).
        const bool romOk = (p7ffd & 0x10) || (atm1 && (aFB & 0x80));
        if (pcH == 0x3D && romOk && (g_atm_ro & 1)) {
            beta = true;
            dosRecalc();
            remap();
        }
    } else {
        // Leave on executing RAM (CF_LEAVEDOSRAM).
        const uint8_t w = pcH >> 6;
        if (!(g_atm_ro & (1u << w))) {
            beta = false;
            dosRecalc();
            remap();
        }
    }
}

} // namespace Atm
