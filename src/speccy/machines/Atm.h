// pico-speccy — MicroART ATM-Turbo 1 and ATM-Turbo 2+.
//
// References (the owner's link, atmturbo.nedopc.com/atmshem.htm, is not reachable
// from the build environment, so everything here is read out of two emulators that
// agree with each other): UnrealSpeccy (tslabs/zx-evo pentevo/unreal — memory.cpp
// MM_ATM450 / MM_ATM710, io.cpp, atm.cpp, drawers.cpp) and MAME sinclair/atm.cpp
// (ATM-Turbo 2+ only).
//
// Both boards: the 48K frame (224 T x 312 lines = 69888 T at 3.5 MHz), no
// contention, AY on #FFFD/#BFFD, Beta-128 TR-DOS, and a 16-entry palette that
// colours EVERY video mode (index = the ZX ink/paper nibble with BRIGHT as bit 3;
// the border is 4 bits too — BRIGHT comes from address line A3 of the #FE write,
// inverted). Video modes (ZX 256x192 / EGA 320x200x16 / hires 640x200 with an
// attribute per byte / 80x25 text — the last one ATM-Turbo 2+ only) read the
// screen page (5 or 7, #7FFD D3) plus the page four below it (1 or 3).
//
// ATM-Turbo 1 (Unreal MM_ATM450), 1 MB (the 1024K upgrade):
//   OUT (#FE): the LOW BYTE OF THE ADDRESS is latched (aFE): A7=0 = CP/M mode (RAM
//     page 0 at #0000, page 4 at #4000), A6..A5 = video mode (0 EGA, 1 hires, 3 ZX).
//   #7DFD (A15=0, A9=0, A1=0) palette write; #7FFD needs A9=1.
//   #FDFD (A15=1, A9=0, A1=0): D2..D0 = #C000 page bits 5..3, D3 with DOS = CPSYS.
//     (D2 is the ROM-disk select on a stock 512K board; used as RAM here.)
//   IN from any port with A2=0 latches the low address byte (aFB); A7 = CPSYS,
//     which pages the SYSTEM ROM at #0000. #7FFD D5 clears it.
//   ROM page = CPSYS ? SYS : DOS ? TR-DOS : #7FFD D4 ? 48 : 128.
//
// ATM-Turbo 2+ (Unreal MM_ATM710, MAME atmtb2plus), 1 MB, ports in the DOS space only
// (TR-DOS active, or CP/M mode):
//   #xx77: data D2..D0 video mode (3 ZX, 0 EGA, 2 hires, 6 text), D3 turbo (7 MHz),
//     D5 frame INT enable; ADDRESS A8 = PEN (memory manager on), A9 = /CPM (0 keeps
//     the DOS signal and the DOS ports up), A14 = /PEN2 (0 = #FF writes the palette).
//   #xxF7: page register of window A15..A14 in set #7FFD D4. Data D7 = take the low
//     bits from #7FFD (RAM: D2..D0 of #7FFD; ROM: bit 0 = the DOS signal), D6 =
//     RAM (1) / ROM (0), D5..D0 = page number INVERTED.
//   PEN = 0 puts the LAST ROM page (the BIOS) in all four windows — the reset state.
//   IDE on the xx0F family (A7..A5 = register, A8 = the 16-bit high-byte latch).
//
// ATM-Turbo 3 v8.0 (NedoPC / Zorel + Maksagor, 2017), 4 MB — the full ATM-Turbo 2+
// (ATM IDE, printer, the lot) plus, per Maksagor's "Обзор нового компьютера
// ATM-turbo 3 версии 8.0" (Info Guide #12, zxpress.ru) and MSD888's test ROM:
//   #BF (open port, read = the written value, unused bits 0, reset 0): D0 = DOSEN2,
//     the shadow ports without the TR-DOS ROM; D1 = PGSN, #xxE7 works as #x7F7 (else
//     as #xFF7); D5 = EXT_PAL, 16 of 4096 colours. D2-D4, D6, D7 unused.
//   #x7F7 (the F7 family with A11 = 0, shadow ports): page register, 8-bit RAM page
//     inverted (4 MB); #xFF7 needs A11 = 1. The article has #x7F7 only behind #BF D0;
//     NedoOS pages through it from plain TR-DOS, so it is decoded whenever the shadow
//     ports are open (see Atm.cpp).
//   #xxE7 (#FFE7/#FEE7 — A8 ignored, window = A15..A14, shadow ports): a second page
//     port on the short decode, in the #xFF7 or the #x7F7 format by #BF D1.
//   EXT_PAL: #xxFF — data = the two high bits per channel (grbG--RB inverted, as on
//     the 2+), A15..A8 = the two low bits in the same layout.
// ROM: 256 KB, ATM3TEST_XBIOS137XT.020 — pages 0-7 the test (page 7), 8-15 xBIOS 1.37.
#pragma once

#include <stdint.h>
#include "speccy/core/ArchRom.h"

// One ROM page of a bound romset (tools/rom_pack.py pack_atm -> roms/atm/atm_banks.h):
// base == nullptr is the all-0xFF page; overlay == nullptr means "base as is".
struct atm_rom_page_t { const unsigned char* base; const unsigned char* overlay; };

// Bit n (0..3) set = CPU window n shows ROM: writes are dropped there (the pages may be
// flattened into butter PSRAM, which MemESP::writebyte's flash-pointer filter does
// not cover). Tested predicted-not-taken in the CPU write funnel (CPU.cpp
// gsDmaPoke8) — zero on every other machine.
extern uint8_t g_atm_ro;

namespace Atm {
    enum VMode : uint8_t { VM_ZX = 0, VM_EGA = 1, VM_HIRES = 2, VM_TEXT = 3, VM_TEXT1 = 4 };  // TEXT1: ZX-Evo 80x25 in RAM page 8

    extern bool     atm1;       // ATM-Turbo 1 board (else 2+), set by bindRoms
    extern bool     atm3;       // ATM-Turbo 3 (4 MB + #BF / #x7F7 / #xxE7), set by bindRoms
    // ZX Evolution BaseConf (R_EVO_BASE): this manager plus EvoBase.{h,cpp}, which owns
    // the Evo ports, NMI, FDD emulator, clock and font RAM.
    extern bool     evo;
    extern uint8_t  p7ffd;
    // ATM-Turbo 1
    extern uint8_t  aFE, aFB, pFDFD;
    // ATM-Turbo 2+
    extern uint16_t a77;
    extern uint8_t  p77;
    // Page registers in Unreal's pFFF7 form: bits 7..0 page (not inverted), bit 8 =
    // ROM, bit 9 = take the page from the register (0 = low bits from #7FFD).
    extern uint16_t pF7[8];
    // ATM-Turbo 3
    extern uint8_t  pBF;        // #BF: D0 DOSEN2, D1 PGSN, D5 EXT_PAL
    extern bool     shaden;     // pBF D0: shadow ports open outside TR-DOS (Ports.cpp FDC gate)
    extern bool     testBoot;   // PEN = 0 shows the lower 128 KB (the test ROM); bootTest()
    extern uint8_t  palHi[16];  // EXT_PAL: A15..A8 of each palette write
    extern uint8_t  pal[16];    // raw palette bytes as written (read back on ATM 2+ via #BF)
    extern bool     beta;       // the Beta-128 trap signal (DOSEN), separate from /CPM
    extern bool     palDirty;   // palette changed — applied at EndFrame

    // Record this romset's page table. The flattening into PSRAM happens on the first
    // reset() after Buffer::initPools (requestMachine runs before the pools exist).
    void bindRoms(RomsetIdx rs, const atm_rom_page_t* pages, uint8_t n);
    // Flatten the overlay pages into butter PSRAM (idempotent). setup() calls it right
    // after Buffer::initPools so the ROMs claim the arena before the GM.DLS bank does.
    void resolveRoms();
    void reset();               // machine reset: register file + remap
    void remap();               // the one writer of MemESP::ramCurrent[0..3] on ATM

    // After reset(): skip the BIOS and start a ROM directly, the way Unreal's
    // reset(RM_DOS/RM_128/RM_SOS) does for ATM (Alt+F11 "Reset to", the Web-catalog
    // TRD launch). Target: BOOT_TRDOS / BOOT_128 / BOOT_48 (48 BASIC, paging locked).
    enum BootTarget : uint8_t { BOOT_TRDOS = 1, BOOT_128 = 2, BOOT_48 = 3 };
    void bootRom(BootTarget t);

    // "Reset to CP/M" on the ATM-Turbo 2+ (BIOS 1.07.x, and xBIOS's page 7 which
    // carries the same code): after its hardware init the BIOS copies its boot menu
    // to #8000 and does CALL #8003 at #00E1; A = 0 on return is the menu's "CP/M"
    // answer (#0138 POP AF / OR A / JP NZ,#8000, else the CP/M cold boot). Armed
    // after a reset, the hook in Z80::check_trdos answers that CALL with A = 0
    // without running the menu. One-shot, cleared by every reset.
    // ATM-Turbo 1 (BIOS 1.04rs) has the same shape at other addresses: after
    // decrypting CP/M into #C000 it does CALL #F864 at #1727, and A = 0 on return
    // (#172A) is again CP/M (#1733 OR A / JR Z -> ... JP #F85C); 1 TR-DOS, 2 128, else 48.
    extern bool cpmBootArmed;
    // ATM-Turbo 2 (BIOS 1.06.02): the same CALL #8003, issued at #00C1 (A = 0 -> CP/M,
    // #00ED OR A / JP NZ,#8000), so only the return address differs.
    constexpr uint16_t kBiosMenuCall = 0x8003, kBiosMenuRet = 0x00E4;     // ATM-Turbo 2+
    constexpr uint16_t kBios106MenuRet = 0x00C4;                          // ATM-Turbo 2 1.06
    constexpr uint16_t kBios1MenuCall = 0xF864, kBios1MenuRet = 0x172A;   // ATM-Turbo 1

    // "Reset to TR-DOS" the way the 128 menu's TR-DOS entry does it: TR-DOS started
    // from under 128 BASIC (the handler at #2816 runs RANDOMIZE USR 15616 in the 128
    // editor), not cold from its own ROM — a cold start leaves 48 BASIC in charge and
    // 128K titles (Trashe) decide the machine is a 48K. Armed by bootRom(BOOT_TRDOS),
    // which boots the 128 ROM; the hook in Z80::check_trdos takes the first entry
    // into the menu loop (JP #2653, SP = #5BFF there) and goes to #2816 instead.
    // Same two addresses in the Pentagon 128 ROM (ATM1 / ATM2 page 2) and in xBIOS's
    // page 6, whose menu table differs but whose TR-DOS handler does not.
    extern bool trdosMenuArmed;
    constexpr uint16_t k128MenuLoop = 0x2653, k128MenuTrdos = 0x2816, k128MenuSp = 0x5BFF;

    // ...and a TR-DOS entered that way does not run "boot" by itself (its first-entry
    // autorun needs (#5B00) == #AA, a byte only its cold start sets; under 128 BASIC
    // #5B00 is the 128 ROM's SWAP routine, and forcing the byte ran a RUN "boot"
    // that skipped the prompt's own init at #02CB and did nothing). So the hook does
    // what the user does: at the first prompt it answers the line editor with RUN.
    // #02E9 CALL #2135 prints "A>" and reaches the 48 ROM editor through JP #1D90;
    // the hook writes RUN (token #F7) into E_LINE, the way the editor would leave it,
    // and returns from #2135 to #02EC, where #3032 / #02EF parse and run the line.
    // 0 = off, 1 = armed.
    extern uint8_t trdosBootState;
    constexpr uint16_t kTrdosEditor = 0x1D90, kTrdosEditorRet = 0x02EC;

    // Port hooks, called early from Ports::output/input. true = consumed.
    bool portWrite(uint16_t address, uint8_t data);
    bool portRead(uint16_t address, uint8_t& v);
    void feWrite(uint16_t address);   // ATM1 #FE address latch (the ULA branch still runs)
    uint8_t feRead(uint8_t v);        // ATM1 #FE bit 7 PAL-detect quirk

    // check_trdos replacement (Z80_JLS.cpp).
    void trdosTrap(uint8_t pcH);

    // ATM-Turbo 3: after reset(), start the test ROM (Alt+F11 "ATM3 test").
    void bootTest();

    // Frame INT gate: #xx77 D5 on the 2+; always open on the ATM1 AND on the plain
    // ATM-Turbo 2 — BIOS 1.06.02 never sets D5 (every #77 write it makes is 00/06/0E)
    // and waits in EI/HALT at #3DBC for the frame INT, so the gate is a 2+ addition.
    extern bool     intGated;   // set by bindRoms
    inline bool intEnabled() { return !intGated || (p77 & 0x20); }

    VMode videoMode();          // live mode from the latches
    uint8_t borderBright();     // 8 when the border is BRIGHT (A3 = 0 at the #FE write)
    uint32_t palRgb(uint8_t i); // palette entry i as RGB888 (2 bits per channel)
    uint8_t romPageCount();

    // ── .pss snapshot (src/speccy/core/Pss.cpp) ─────────────────────────────
    // The manager's register file, the palette RAM, the clock and — on the ZX-Evo —
    // EvoBase's latches: the PSAT block. The Evo font RAM is its own block (PSEF).
    constexpr uint32_t SNAP_MAX = 112;
    uint32_t snapSave(uint8_t* out);
    void     snapLoad(const uint8_t* in, uint32_t n);
    void     snapRemap();   // windows, DOS signal, video mode, palette, clock
}
