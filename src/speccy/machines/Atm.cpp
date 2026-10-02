// pico-speccy — MicroART ATM-Turbo 1 / 2+ memory manager and system ports.
// See Atm.h for the hardware description and the references.
#include "Atm.h"

#include <string.h>
#include "app/Buffer.h"
#include "speccy/devices/storage/RTC.h"
#include "speccy/devices/disk/wd1793.h"
#include "speccy/z80/CPU.h"
#include "app/Config.h"
#include "app/Debug.h"
#include "app/ESPectrum.h"
#include "speccy/devices/storage/IDE.h"
#include "ui/LEDIndicators.h"
#include "speccy/core/MemESP.h"
#include "ui/OSDMain.h"
#include "speccy/core/Ports.h"
#include "speccy/core/RomOverlay.h"
#include "speccy/video/Video.h"
#include "speccy/z80/z80.h"
#if EVO_CFG_TRACE
static uint16_t s_evo_tr_n = 0;   // evoTrace budget, re-armed by reset()
#endif


uint8_t g_atm_ro = 0;
uint8_t g_atm_fnt = 0;
#if ATM_PAGE_TRACE
extern bool g_atm_trace_armed;   // Ports.cpp
#endif

namespace Atm {

bool     atm1 = false;
bool     atm3 = false;
bool     evo = false;
uint8_t  pEFF7 = 0;
bool     inNmi = false;
bool     nmiClrPending = false;
uint64_t intAckFrame = ~0ull;
uint8_t  fddMask = 0;
uint8_t  vgSys = 0;
bool     inTrdemu = false;
uint8_t* font = nullptr;   // ZX-Evo font RAM, 2 KB, allocated on the first Evo reset
bool     intGated = false;
uint8_t  p7ffd = 0;
uint8_t  aFE = 0x80, aFB = 0x80, pFDFD = 0;
uint16_t a77 = 0;
uint8_t  p77 = 0;
uint16_t pF7[8] = { 0 };
uint8_t  pBF = 0;
bool     shaden = false;
bool     testBoot = false;
// ATM-Turbo 3 with MicroART BIOS 1.07.13EC: the 4 MB ports (#x7F7 = #xxF7 with A11 = 0,
// and #xxE7) are only taken from code running in RAM or with #BF D0 set; see the
// #xxE7 / #x7F7 decode below.
static bool s_atm3Bios107 = false;
uint8_t  pal[16] = { 0 };
uint8_t  palHi[16] = { 0 };
bool     beta = false;
bool     palDirty = false;

static const atm_rom_page_t* s_tbl = nullptr;   // bound romset's page table
static uint8_t  s_npages = 0;
static const uint8_t* s_rom[32] = { nullptr }; // resolved pages (flash or PSRAM); 32 = ZX-Evo
static uint8_t* s_flat = nullptr;              // PSRAM block holding the flattened pages
static RomsetIdx s_flat_rs = R_NONE;           // romset s_flat currently holds
static bool     s_flat_warned = false;

// Filler for a page that could not be resolved (no PSRAM): reads 0xFF like an
// empty ROM socket. Aligned so the pointer is a valid flash address (writes are
// dropped by g_atm_ro anyway).
static const uint8_t kFF[16] __attribute__((aligned(4))) = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

uint8_t romPageCount() { return s_npages ? s_npages : 4; }

// #xFF7 data byte -> the pFFF7 form (Unreal io.cpp): D7 = low bits from #7FFD,
// D6 = RAM, D5..D0 = page inverted.
static inline uint16_t f7enc(uint8_t v) {
    return (uint16_t)((((v & 0xC0) << 2) | (v & 0x3F)) ^ 0x33F);
}

void bindRoms(RomsetIdx rs, const atm_rom_page_t* pages, uint8_t n) {
    if (n > sizeof s_rom / sizeof s_rom[0]) n = sizeof s_rom / sizeof s_rom[0];
    atm1 = isAtm1Romset(rs);
    atm3 = isAtm3Romset(rs);
    evo  = isEvoBaseRomset(rs);
    // ZX-Evo BaseConf: zint.v raises INT at every frame start with no gate at all —
    // #xx77 D5 means nothing there (Dune II for the Evo writes #77 with D5 clear).
    intGated = (rs == R_ATM2 || rs == R_ATM2X || isAtm3Romset(rs));
    s_atm3Bios107 = (rs == R_ATM3_107);
    if (s_tbl != pages) {
        s_tbl = pages;
        s_npages = n;
        for (int i = 0; i < 32; i++) s_rom[i] = nullptr;
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
// The shadow (DOS) PORTS, not the DOS ROM: TR-DOS / CP/M, or the ATM3's #BF D0.
static inline bool dosPorts() { return ESPectrum::trdos || shaden; }

static void evoClockApply();
static void loadSpecPalette();
extern "C" const unsigned char gb_rom_atm_font[];

static void dosRecalc() {
    // ZX-Evo (zdos.v): DOS is a LATCH — set while /CPM = 0, and it stays set after
    // /CPM goes back to 1 until code runs from RAM (trdosTrap). EVO Reset Service
    // drops /CPM (#FF77) early in its ROM start and relies on the shadow ports.
    if (evo && cpmOn()) beta = true;
    ESPectrum::trdos = beta || cpmOn();
}

// ---------------------------------------------------------------------- paging --

static uint8_t s_ro;

// BIOS 1.07.13 is a 2+ BIOS: it writes the #xxF7 page registers with A11 = 0 (SYS page
// #02B6, the reset page-table set-up: LD BC,#00F7 / OUT (C),A, B = #00/#40/#80/#C0 —
// the EC build fixed only the OUTI loops) and its boot menu, copied to RAM at #8000,
// floods #xxE7 with #FF (#80AD: LD BC,#00E7 / OUT (C),A / DJNZ). Taken as the ATM3's
// 4 MB ports that remapped every window to RAM under the menu (hw dump 2026-09-29).
// Two rules, one per port. #xxE7: #BF D0 only (Maksagor's rule) — the flood comes from
// RAM, so a "runs from RAM" test cannot catch it. #x7F7: #BF D0, or a writer running
// from RAM — BIOS ROM code (#02B6) gets the 2+ #xxF7, while NedoOS's kernel (pages 4 MB
// through #37F7 with #BF = #20) and the menu's RAM copy (which only uses A11 = 1) work.
static inline bool atm3E7Port()  { return !s_atm3Bios107 || (pBF & 1); }
static inline bool atm3X7F7Port() {
    if (!s_atm3Bios107 || (pBF & 1)) return true;
    return !(s_ro & (1u << (Z80::getRegPC() >> 14)));
}

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
        // #FDFD D2..D0 extend the #C000 page to 1 MB (Unreal MM_ATM450 with the
        // 1024K option: `bank += (pFDFD & 7) << 3`). On a stock v4.50 board D2 is
        // the ROM-disk select (upper 64 KB of a 27010); this set is a 27512 with
        // nothing up there, so D2 is free to be the 1 MB upgrade UMT tests.
        const uint32_t pg3 = (p7ffd & 7) | ((pFDFD & 7) << 3);
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
        // PEN = 0: the last ROM page (the BIOS) in every window. The ATM3 test boot
        // takes the last page of the LOWER 128 KB instead (the test's own page 7),
        // as a board with the ROM's A17 held low would.
        const uint8_t last = (atm3 && testBoot && n > 8) ? (uint8_t)(n / 2 - 1) : (uint8_t)(n - 1);
        for (int w = 0; w < 4; w++) mapRom(w, last);
        MemESP::bankLatch = p7ffd & 7;
    } else {
        const int set = (p7ffd & 0x10) ? 4 : 0;
        const uint8_t dos = ESPectrum::trdos ? 1 : 0;
        // ZX-Evo (atm_pager.v): #EFF7 D2 = 0 is the Pentagon-1024 mode, where a RAM
        // window in "#7FFD mode" takes SIX page bits from #7FFD (D7..D5, D2..D0) under
        // the register's top two; D2 = 1 is the 128K mode (three bits, as on the 2+).
        const bool evo1m = evo && !(pEFF7 & 0x04);
        const uint8_t pent1m = (uint8_t)(((p7ffd >> 2) & 0x38) | (p7ffd & 7));
        for (int w = 0; w < 4; w++) {
            // ZX-Evo window 0 overrides, ahead of the manager: the NMI state puts RAM
            // page #FF there (znmi.v in_nmi), #EFF7 D3 RAM page 0 (pent1m_ram0_0).
            // base_trdemu also maps RAM page #FE there while the FDD emulator runs
            // (zdos.v in_trdemu; atm_pager.v page = { 7'h7F, in_nmi }).
            if (evo && w == 0 && (inNmi || inTrdemu || (pEFF7 & 0x08))) {
                mapRam(0, (inNmi || inTrdemu) ? (0xFE | (inNmi ? 1 : 0)) : 0);
                continue;
            }
            const uint16_t v = pF7[set + w];
            const uint8_t page = (uint8_t)v;
            switch (v & 0x300) {
                case 0x000:
                    if (evo1m) mapRam(w, (page & 0xC0u) | pent1m);
                    else       mapRam(w, (page & 0xF8u) | (p7ffd & 7));   // RAM, low bits from #7FFD
                    break;
                case 0x200: mapRam(w, page); break;                          // RAM from the register
                case 0x100: mapRom(w, (uint8_t)((page & 0xFE) | dos)); break; // ROM, bit 0 = DOS
                default:    mapRom(w, page); break;                          // ROM from the register
            }
        }
        MemESP::bankLatch = p7ffd & 7;
    }
    // Bit 7 = the ZX-Evo font RAM write (#BF D2): every CPU memory write also lands
    // in the 2 KB character generator, at A10..A0 (zports.v fnt_wr). Folded into
    // g_atm_ro so the write funnel keeps its single test.
    g_atm_ro = (uint8_t)(s_ro | (g_atm_fnt ? 0x80 : 0));
    MemESP::videoLatch = (p7ffd >> 3) & 1;
    MemESP::romLatch = (p7ffd >> 4) & 1;
    MemESP::romInUse = 0;
    // recoverPage0() must not touch window 0 on ATM (it would map rom[romInUse]
    // over the memory manager's choice): the +3's "someone else owns page 0" flag.
    MemESP::p3special = 2;
    // ZX-Evo: #7FFD D5 locks the port in the 128K mode only (zports.v block7ffd =
    // p7ffd[5] & block1m); in the Pentagon-1024 mode D5 is a page bit.
    MemESP::pagingLock = (evo && !(pEFF7 & 0x04)) ? 0 : ((p7ffd >> 5) & 1);
    for (int w = 0; w < 4; w++) MemESP::ramContended[w] = false;
    VIDEO::grmem = MemESP::ram[MemESP::videoLatch ? 7 : 5].direct();
}

bool cpmBootArmed = false;
bool trdosMenuArmed = false;
uint8_t trdosBootState = 0;

void reset() {
    cpmBootArmed = false;
    trdosMenuArmed = false;
    trdosBootState = 0;
    resolveRoms();
    p7ffd = 0;
    beta = false;
    // ATM3 #BF: every bit 0 after a reset (no priority shadow ports, #xxE7 = #xFF7,
    // standard 64-colour palette).
    pBF = 0;
    shaden = false;
    g_atm_fnt = 0;
    testBoot = false;
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
    } else if (evo) {
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
        evoClockApply();
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

// Skip the BIOS: the memory state its menu leaves behind for "TR-DOS" / "128" /
// "48". ATM-Turbo 2+ follows Unreal reset(RM_DOS): memory manager on (PEN), /CPM
// off, /PEN2 = 1 (no palette through #FF), ZX mode, frame INT on, and a page table
// of ROM-by-#7FFD in window 0 — set 0 (D4=0) the 128 ROM, set 1 the 48 ROM with
// bit 0 = the DOS signal, so the #3Dxx trap and the 128 menu's own paging keep
// working — RAM 5 / RAM 2 / RAM-by-#7FFD above. (Unreal pins RAM 0 in window 3;
// taking the low bits from #7FFD is what the BIOS itself sets and keeps 128K
// paging alive.) ATM-Turbo 1: Unreal's RM_DOS aFE = #E0 (no CP/M, ZX), aFB = 0
// (no CPSYS), then the ordinary #7FFD D4 / DOS ROM select.
void bootRom(BootTarget t) {
    const uint8_t n = romPageCount();
    const uint8_t base = (n >= 8) ? (uint8_t)(n - 4) : 0;   // xBIOS: the standard set
    if (atm1) {
        aFE = 0xE0;
        aFB = 0;
    } else {
        a77 = 0x4000 | 0x0200 | 0x0100;
        p77 = 0x20 | 3;
        const uint8_t w0set0 = (uint8_t)(~(base + 2) & 0x3F);          // ROM: 128
        const uint8_t w0set1 = (uint8_t)(0x80 | (~base & 0x3F));        // ROM: 48 | DOS
        const uint8_t tbl[4] = { 0, (uint8_t)(0x40 | (~5 & 0x3F)),
                                    (uint8_t)(0x40 | (~2 & 0x3F)), 0xFF };
        for (int s = 0; s < 2; s++)
            for (int w = 0; w < 4; w++)
                pF7[s * 4 + w] = f7enc(w ? tbl[w] : (s ? w0set1 : w0set0));
    }
    // ZX-Evo: the plain Spectrum the BASIC rows promise — #EFF7 D2 (128K paging,
    // D5 of #7FFD locks again) and D4 (turbo off, 3.5 MHz).
    if (evo) { pEFF7 = 0x14; evoClockApply(); }
    switch (t) {
        case BOOT_TRDOS: p7ffd = 0x00; beta = false; trdosMenuArmed = true;
                         Debug::log("[ATM] boot TR-DOS via the 128 menu"); break;
        case BOOT_128:   p7ffd = 0x00; beta = false; break;
        case BOOT_48:    p7ffd = 0x30; beta = false; break;
    }
    // The BIOS has already programmed its own palette by the time a reset reaches
    // here (it runs before the machine is handed over), and nothing on the direct
    // path rewrites it: load the standard ZX colours into the palette RAM, as
    // Unreal's reset() does (load_spec_colors), and hand the 16 hardware slots back
    // to the emulator's own ZX palette until the guest programs one.
    loadSpecPalette();
    palDirty = false;
    VIDEO::atmPaletteRestore();
    dosRecalc();
    remap();
    VIDEO::atmVideoModeChanged();
}

// ATM-Turbo 3 test ROM: the reset state with the lower 128 KB of the ROM chip in
// place of xBIOS — PEN = 0 then maps page 7 (MSD888's test) into every window and
// the test starts at #0000 by itself (DI / OUT #7FFD / #xFF7 set-up / #xx77).
// Cleared by the next machine reset, which brings xBIOS back.
// ZX-Evo NMI (Print Screen / #BF D3 / a breakpoint): znmi.v holds in_nmi from the
// NMI until OUT (#BE), and atm_pager.v maps RAM page #FF into #0000-#3FFF meanwhile —
// so the #0066 the CPU jumps to is ERS's resident handler, not the running ROM.
void nmiClrApply() {
    nmiClrPending = false;
    if (inNmi) { inNmi = false; remap(); }
}

#if EVO_CFG_TRACE
static int s_nmi_tr = 0;   // port accesses still to log after an NMI
#endif
void nmiEnter() {
    if (!evo) return;
#if EVO_CFG_TRACE
    Debug::log("[NMI] RAM #FF in window 0, pc=%04X", Z80::getRegPC());
    s_nmi_tr = 300;
#endif
    inNmi = true;
    remap();
}

void bootTest() {
    if (!atm3) return;
    testBoot = true;
    remap();
}

// ------------------------------------------------------------------ palette --

// ATM-Turbo 2+: data bits (inverted) grbG--RB -> 2 bits per channel.
// ATM-Turbo 1:  --grbGRB. Unreal atm_zc_tables(); MAME atm_port_ff_w agrees for the 2+.
// ATM-Turbo 3 with #BF D5: 16 of 4096 colours. The data byte keeps the two HIGH bits
// of each channel exactly as above and the address byte A15..A8 of the same #xxFF
// write carries the two LOW bits in the same layout — the test ROM's own ramps
// (page 7 #0120/#0140/#0160: data/high pairs DE/DE, DE/FE, DE/DF, DE/FF, FE/DE ...)
// only descend monotonically read that way.
uint32_t palRgb(uint8_t i) {
    const uint8_t v = (uint8_t)~pal[i & 15];
    uint8_t R, G, B;
    if ((atm3 || evo) && (pBF & 0x20)) {
        const uint8_t h = (uint8_t)~palHi[i & 15];
        auto ch = [v, h](int hi, int lo) -> uint8_t {
            const int c = (((v >> hi) & 1) << 3) | (((v >> lo) & 1) << 2) |
                          (((h >> hi) & 1) << 1) |  ((h >> lo) & 1);
            return (uint8_t)(c * 0x11);
        };
        R = ch(1, 6); G = ch(4, 7); B = ch(0, 5);
    } else if (atm1) {
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

static void palWrite(uint8_t data, uint8_t hi = 0xFF) {
    const uint8_t idx = VIDEO::borderColor & 15;
    if (pal[idx] != data || palHi[idx] != hi) {
        pal[idx] = data;
        palHi[idx] = hi;
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
        // ZX-Evo BaseConf (video_modedecode.v mode_a_txt_1page): the 80x25 text mode
        // with symbols, attributes and both halves in ONE page, #08 (EVO Reset
        // Service's Magic menu). Elsewhere code 7 is undefined and renders as ZX.
        case 7:  return evo ? VM_TEXT1 : VM_ZX;
        default: return VM_ZX;       // 3 = ZX; the undefined codes render as ZX (MAME)
    }
}

// --------------------------------------------------------------------- ports --

// ZX-Evo CPU clock (top.v: turbo = { #xx77 D3, ~#EFF7 D4 }; zclock.v: 1x = 14 MHz,
// 01 = 7 MHz, 00 = 3.5 MHz) — so the machine comes out of reset at 7 MHz (#EFF7 = 0).
// A change is a change of UNITS for CPU::tstates (the TS-Conf lesson).
static void evoClockApply() {
    const uint8_t want = (p77 & 0x08) ? 2 : ((pEFF7 & 0x10) ? 0 : 1);
    const uint8_t old = ESPectrum::multiplicator;
    if (want == old) return;
    CPU::tstates = (want > old) ? (CPU::tstates << (want - old)) : (CPU::tstates >> (old - want));
    ESPectrum::multiplicator = want;
    CPU::updateStatesInFrame();
    OSD::notifyClock(want == 2 ? " CPU: 14 MHz " : want ? " CPU: 7 MHz " : " CPU: 3.5 MHz ");
}

// The standard ZX colours in the palette RAM format (data byte, inverted).
static void loadSpecPalette() {
    for (int i = 0; i < 16; i++) {
        const bool b = i & 1, r = i & 2, g = i & 4, br = i & 8;
        uint8_t v;
        if (atm1) v = (uint8_t)((b ? 0x01 : 0) | (r ? 0x02 : 0) | (g ? 0x04 : 0) |
                                (br ? ((b ? 0x08 : 0) | (r ? 0x10 : 0) | (g ? 0x20 : 0)) : 0));
        else      v = (uint8_t)((b ? 0x01 : 0) | (r ? 0x02 : 0) | (g ? 0x10 : 0) |
                                (br ? ((b ? 0x20 : 0) | (r ? 0x40 : 0) | (g ? 0x80 : 0)) : 0));
        pal[i] = (uint8_t)~v;
    }
}

static void write7ffd(uint8_t data) {
    // 48 lock; on the ZX-Evo only in the 128K mode (#EFF7 D2 = 1).
    if ((p7ffd & 0x20) && (!evo || (pEFF7 & 0x04))) return;
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
    // ZX-Evo (zdos.v): DOS drops on ANY M1 from a RAM window while /CPM = 1, not only
    // on a jump into one. An OUT that raises /CPM from RAM therefore drops DOS at the
    // very next fetch, before control leaves RAM — check_trdos only sees jump TARGETS,
    // so it would miss it. EVO Reset Service enters ProfROM exactly so: JP #BF5A (in
    // RAM, /CPM still 0) / OUT (#FF77),#A3 / RET to #0000 — with DOS left up the ROM's
    // DOS bit picked the service page 15 instead of page 14 (hw 2026-10-02).
    if (evo && beta && !cpmOn() && (a77 & 0x100)) {
        const uint8_t w = (uint8_t)(Z80::getRegPC() >> 14);
        if (!(pF7[((p7ffd & 0x10) >> 2) + w] & 0x100)) beta = false;
    }
    dosRecalc();
    remap();
    if (evo) {
        evoClockApply();
        if ((data & 7) != oldMode) VIDEO::atmVideoModeChanged();
        return;
    }
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

// ------------------------------------------------------------ ZX-Evo BaseConf --
// zports.v, port for port. `shadow` = dos || #BF D0. What is not taken here falls
// through to the generic decode (ULA #FE, AY, Kempston #1F noshad, Z-Controller
// #77/#57 noshad, NEMO IDE, GS, covox) — Ports.cpp keeps the Z-Controller #57
// with A15 = 1 in shadow mode for the card's CS (atmPortWriteEarly).

// #xxF7 with A8 = 1 outside shadow (#EFF7/#DFF7/#BFF7), A8 = 0 inside it (#EEF7 —
// abandoned in shadow mode — /#DEF7/#BEF7): "F7 ports are accessible in shadow mode
// but at addresses like EEF7, DEF7, BEF7 so that there are no conflicts with ATM xFF7
// and x7F7". Gluk access: #EFF7 D7 outside shadow, always inside it.
static void evoF7Write(uint16_t address, uint8_t data, bool sh) {
    if (!sh && !(address & 0x1000)) {                       // #EFF7 (noshad only)
        const uint8_t old = pEFF7;
        pEFF7 = data;
        if ((old ^ data) & 0x0C) remap();                    // D2 128K/1M mode, D3 RAM0
        if ((old ^ data) & 0x10) evoClockApply();            // D4 turbo off
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
    if (!ESPectrum::trdos || !(a77 & 0x4000) || !(s_ro & 1) || inTrdemu || inNmi) return false;
    inTrdemu = true;
    remap();
    return true;
}

#if EVO_CFG_TRACE
// ZX-Evo configuration writes with the guest PC: #7FFD (A15 = 0, FD/FC), #xx77 and
// #EFF7 — the same filter and line shape as tools/evo_sim.c ONLYCFG=1, so a capture
// diffs against the RTL model. Runs of one (port, value, pc) collapse; the budget is
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

static bool evoPortWrite(uint16_t address, uint8_t data) {
    const uint8_t lo = (uint8_t)address;
    const bool sh = dosPorts();
#if EVO_CFG_TRACE
    if (((lo == 0xFD || lo == 0xFC) && !(address & 0x8000)) || lo == 0x77 ||
        lo == 0xBF || lo == 0xBE || lo == 0xBD ||
        (lo == 0xF7 && (address & 0x100) && (sh || !(address & 0x1000))))
        evoTrace(address, data);
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
            LED::touchW(LED::RAM);
            return true;
        }
        if (sh != hiA8) evoF7Write(address, data, sh);
        return true;
    }
    default: break;
    }
    // The NEMO IDE decode with no NEMO image: swallowed, not handed to the A0 = 0 ULA.
    if (IDE::portScheme != IDE::NEMO &&
        (((lo & 7) == 0 && (((lo >> 3) ^ (lo >> 4)) & 1)) || lo == 0x11))
        return true;
    if (!sh) return false;
    if (lo == 0x77) { write77(address, data); return true; }
    // #FF: the FDC system register, and the palette while /PEN2 (#77 A14 = 0). The
    // drive number it carries decides the FDD-emulator trap below.
    if (lo == 0xFF) {
        vgSys = data;
        if (!(a77 & 0x4000)) palWrite(data, (uint8_t)(address >> 8));
    }
    if (trdemuTrap(lo)) return lo != 0xFF;   // a masked drive: the WD1793 never sees it
    return false;
}

// #BD read: the configuration read-back, by A12..A8 (base_trdemu zports.v portbdmux;
// the baseconf trunk had it on #BE — EVO Reset Service reads #BD).
static uint8_t evoPortBD(uint8_t idx) {
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

static bool evoPortRead(uint16_t address, uint8_t& v) {
    const uint8_t lo = (uint8_t)address;
    const bool sh = dosPorts();
    switch (lo) {
    case 0xBF: v = pBF; return true;
    case 0xBD: v = evoPortBD((uint8_t)((address >> 8) & 0x1F)); return true;
    case 0xBE: v = 0xFF; return true;
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
    if (!sh) return false;
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
    const uint8_t lo = (uint8_t)address;
    if (evo) {
#if EVO_CFG_TRACE
        if (s_nmi_tr > 0) { s_nmi_tr--; Debug::log("[NMIP] OUT pc=%04X %04X=%02X sh=%d", Z80::getRegPC(), address, data, (int)dosPorts()); }
#endif
        return evoPortWrite(address, data);
    }
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
    // ATM-Turbo 3 #BF (open port, any mode — "доступный как на чтение, так и на
    // запись"): D0 = priority shadow ports without the TR-DOS ROM, D1 = #xxE7 works as
    // #x7F7 (else as #xFF7), D5 = the 4096-colour palette. D2-D4, D6, D7 unused.
    // Taken ahead of the #FF palette family, which #BF would otherwise match.
    if (atm3 && lo == 0xBF) {
        const uint8_t old = pBF;
        pBF = data & 0x23;
        shaden = (pBF & 1) != 0;
        if ((old ^ pBF) & 0x20) { palDirty = true; VIDEO::atmPaletteChanged(); }
        return true;
    }
    // ZX-128 #7FFD. ATM-Turbo 2+: the loose decode (A15 = 0, A1 = 0). ATM-Turbo 3:
    // low byte #FD and A15 = 0, so a NEMO IDE card fits beside it — NedoOS for ATM3 is
    // built with NEMOIDE=1 (src.r2698 _sdk/config/atm3*), and its driver writes #E0
    // (LBA, master) to the head register with LD BC,#00D0 : OUT (C),A, which the loose
    // decode took as #7FFD: paging locked (D5), register set flipped (D4) under the
    // kernel, FatFs never ran (hw 2026-09-28, p7ffd=E0; again 2026-09-29 once the loose
    // decode came back with the port rework).
    if (atm3 ? (lo == 0xFD && !(address & 0x8000)) : !(address & 0x8002)) {
        write7ffd(data);
        return true;
    }
    // ATM-Turbo 3: the NEMO IDE decode (loa[2:0] = 0 and loa[3] != loa[4]; plus #11,
    // the high-byte latch). With a NEMO image mounted Ports.cpp answers it; without one
    // the writes must still not fall through to the A0 = 0 ULA decode and repaint the
    // border / click the beeper.
    if (atm3 && IDE::portScheme != IDE::NEMO &&
        (((lo & 7) == 0 && (((lo >> 3) ^ (lo >> 4)) & 1)) || lo == 0x11))
        return true;
    if (!dosPorts()) return false;
    const uint8_t sel = (uint8_t)(((p7ffd & 0x10) >> 2) | (address >> 14));
    // ATM-Turbo 3 #xxE7 (#FFE7/#FEE7, A8 ignored, window = A15..A14): a second page
    // port on the short decode — #xFF7 while #BF D1 = 0, #x7F7 (8-bit RAM page,
    // inverted) while D1 = 1.
    if (atm3 && lo == 0xE7 && atm3E7Port()) {
        pF7[sel] = (pBF & 2) ? (uint16_t)((pF7[sel] & ~0x1FFu) | (uint8_t)(data ^ 0xFF))
                             : f7enc(data);
        remap();
        LED::touchW(LED::RAM);
        return true;
    }
    // %nXnnnnXX 0nn101n1 / 1nn101n1: A6, A5, A1 are not decoded.
    if ((lo & 0x9D) == 0x15) { write77(address, data); return true; }
    if ((lo & 0x9D) == 0x95) {
        // ATM-Turbo 3: A11 = 0 is #x7F7 (the 4 MB manager: 8-bit RAM page, inverted),
        // #xFF7 needs A11 = 1 — whenever the shadow ports are open, TR-DOS included.
        // DEVIATION from Maksagor's 2017 article, which has #x7F7 visible only through
        // #BF D0: NedoOS for ATM3 (osatm3sd.trd) enters DOS with JP #3D2F, never sets
        // #BF D0 (it writes #20) and pages 4 MB through #37F7/#B7F7/#F7F7 — with the
        // article's rule it hung at start (hw 2026-09-29). xBIOS 1.37 and the ATM3 test
        // ROM address #xFF7 with A11 = 1 throughout (#3FF7/#7FFx/#BFF7/#FFF7/#0EF7), so
        // they do not notice; only software doing OUT (#F7) with A11 = 0 in A would.
        if (atm3 && !(address & 0x0800) && atm3X7F7Port()) {
            pF7[sel] = (uint16_t)((pF7[sel] & ~0x1FFu) | (uint8_t)(data ^ 0xFF));
#if ATM_PAGE_TRACE
            // x7F7 into a register still in "low bits from #7FFD" mode: the page the
            // guest asked for is not the page it gets.
            if (!(pF7[sel] & 0x200)) {
                static uint16_t n = 0;
                if (n < 40) { n++;
                    Debug::log("[ATMX] x7F7 %04X=%02X set%d w%d in 7FFD mode -> pg %02X pc=%04X",
                               address, data, sel >> 2, sel & 3,
                               (unsigned)(((data ^ 0xFF) & 0xF8) | (p7ffd & 7)), Z80::getRegPC()); }
            }
#endif
        } else {
            pF7[sel] = f7enc(data);
#if ATM_PAGE_TRACE
            if (atm3) {
                static uint16_t n = 0;
                if (n < 80) { n++;
                    Debug::log("[ATMX] xFF7 %04X=%02X set%d w%d -> %03X pc=%04X",
                               address, data, sel >> 2, sel & 3, pF7[sel], Z80::getRegPC()); }
            }
#endif
        }
        remap();
#if ATM_PAGE_TRACE
        {   // NedoOS init_resident: the #C000 page load at #0055 is followed by
            // POP DE / POP BC / POP AF / RET — log where that RET goes.
            static uint16_t n = 0;
            const uint16_t pc = Z80::getRegPC();
            if (::g_atm_trace_armed && (pc == 0x0055 || pc == 0x004F || pc == 0x0049) && n < 60) {
                n++;
                const uint16_t sp = Z80::getRegSP();
                auto rd = [](uint16_t a) -> uint8_t { return MemESP::ramCurrent[a >> 14][a & 0x3FFF]; };
                const uint8_t* z = MemESP::ramCurrent[0];
                Debug::log("[ATMINIT]   w0[00..0F]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                           z[0],z[1],z[2],z[3],z[4],z[5],z[6],z[7],z[8],z[9],z[10],z[11],z[12],z[13],z[14],z[15]);
                Debug::log("[ATMINIT] %04X=%02X pc=%04X sp=%04X ret=%02X%02X w=%p %p %p %p",
                           address, data, pc, sp, rd((uint16_t)(sp + 7)), rd((uint16_t)(sp + 6)),
                           (void*)MemESP::ramCurrent[0], (void*)MemESP::ramCurrent[1],
                           (void*)MemESP::ramCurrent[2], (void*)MemESP::ramCurrent[3]);
            }
        }
#endif
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
    if ((lo & 0x9F) == 0x9F && !(a77 & 0x4000)) palWrite(data, (uint8_t)(address >> 8));
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
    if (evo) {
        const bool r = evoPortRead(address, v);
#if EVO_CFG_TRACE
        if (s_nmi_tr > 0) { s_nmi_tr--; Debug::log("[NMIP] IN  pc=%04X %04X=%s%02X sh=%d", Z80::getRegPC(), address, r ? "" : "gen:", r ? v : 0, (int)dosPorts()); }
#endif
        return r;
    }
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
    if (atm3 && (uint8_t)address == 0xBF) { v = pBF; return true; }   // unused bits read 0
    if (dosPorts() && ideLive(address)) {
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
    if ((address & 0xFF) == 0xFF && !dosPorts()) { v = Ports::getFloatBusData48(); return true; }
    // A15 = 0, A9 = 1, A1 = 0: the IDE INTRQ / DAC status port — D6 = INTRQ.
    // Below the DOS ports, as in Unreal (in(): the CF_DOSPORTS block returns first):
    // `IN A,(#FF)` puts A on A8-A15, so with DOS up a Beta register read can match
    // this decode and must still reach the WD1793 (the CP/M BIOS polls INTRQ/DRQ
    // that way).
    if (dosPorts() && (address & 0x1F) == 0x1F) return false;
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
        // ZX-Evo (atm_pager.v dos_exec_stb): window 0's set-1 register must be ROM in
        // "#7FFD/DOS" mode — the BIOS's own 48/TR-DOS pair — for the trap to fire.
        const bool romOk = ((p7ffd & 0x10) && (!evo || (pF7[4] & 0x300) == 0x100)) ||
                           (atm1 && (aFB & 0x80));
        if (pcH == 0x3D && romOk && (g_atm_ro & 1)) {
            beta = true;
            dosRecalc();
            remap();
        }
    } else {
        // Leave on executing RAM (CF_LEAVEDOSRAM). ZX-Evo (atm_pager.v ram_exec_stb):
        // by the window's REGISTER, not by what is mapped — RAM #FE/#FF of the FDD
        // emulator / NMI and #EFF7 RAM 0 override window 0 without dropping DOS.
        const uint8_t w = pcH >> 6;
        const bool ramExec = evo ? ((a77 & 0x100) && !(pF7[((p7ffd & 0x10) >> 2) + w] & 0x100))
                                 : !(g_atm_ro & (1u << w));
        if (ramExec) {
            beta = false;
            dosRecalc();
            remap();
        }
    }
}

} // namespace Atm
