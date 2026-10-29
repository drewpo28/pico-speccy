// pico-speccy — Quorum (Kvorum; Kamil Karimov, Sverdlovsk/Yekaterinburg, 1989-1994).
// FLASH-resident: the hot paths test g_quorum (CPU.h) and call in here.
//
//   Quorum 64    romset R_48K_QU64 of A_48K — a 48K on discrete logic with 64 KB of
//                RAM (16 KB of it shadow RAM at #0000) and the disk board (WD1793 on
//                #80-#85; the "disk5trd" board answers the Beta ports while the DOS
//                signal is up). ROM QU4V93T4: page 0 TR-DOS 5.04T on the STOCK Beta
//                ports (22 bytes off 504t.bin), page 1 Quorum BASIC-48 (its loader at
//                #39C9 uses #80/#85).
//   Quorum 64 (original)  romset R_48K_QU64O of A_48K: quorum48.rom
//                (speccy4ever), BASIC-48 only. No TR-DOS ROM, no DOS signal: its
//                loader (#39D8) reads TR-DOS / CP/M from disk into the shadow RAM
//                through #80/#85, which are always decoded; the Beta ports never are.
//   Quorum 128   romset R_QU128 of A_PENT — uncontended 128K on the ULA-M gate array
//   Quorum 128+  (KB01VG1-2); the 128+ adds the AY and an internal drive. ROM
//                QU7V42 "ROM-MENU QUORUM V4.2": SYS (menu/tests/monitor/copier),
//                TR-DOS, BASIC-128, BASIC-48.
//   Quorum 256 / 512 / 1024  romsets R_QU256/512/1024 of A_PENT: the 128+ (AY)
//                with more RAM. #C000 page = #7FFD D0-D2 | D6 EX_RAM -> bit 3 | D7
//                E2_RAM -> bit 4 | D5 E3_RAM -> bit 5 (the Quorum memory-map table),
//                masked by the size; #00 D3 RAM_8 = page 8 at #0000. On these #00 D6
//                is only the #7FFD lock and D5 B_ROM = 1 write-protects the shadow
//                RAM; #7E D6/D7 read RAM_8 / SCR_P. The extended page bits (EX/E2/E3
//                and RAM_8) reach the RAM only while #00 D4 ZX128 = 0 (DD24.3 ->
//                /EXRAMEN -> DD22 /EO), so a reset is 1024 mode; the port table's
//                "ZX128 = 0 -> 128 mode" is inverted, the schematic wins. BROMEN on
//                the same OR is not #00 D5 (QU7V42 runs BASIC with D5 = 1). The 1024 runs the "Quorum" raster
//                of the Quorum-1024+ timing diagram (kKvRasterShift). Not modelled:
//                the "Pentagon" raster mode (#00 D2), #80FD D7 EN_RAM.
//
// Sources: the KiCad schematics in github.com/UncleRus/quorum-reborn (Quorum 64 drawn
// by its designer), its resources/Doc/ports.md, UnrealSpeccy MM_QUORUM, ZXMAK2
// Hardware/Quorum, SpeccyP quorum.c, and the Quorum 128+ user manual (old-hard.ru scan).
//
// TIMING. Quorum 64 — read off the schematic (DD13/DD14 line counter, DD16/DD17 frame
// counter, DD15/DD18/DD19 sync flip-flops, C7/R61 INT shaper):
//   14 MHz crystal, 7 MHz pixel clock, 3.5 MHz CPU; a line is 256 px of paper + a
//   192 px phase 2 (HSYNC = phase-2 chars 8..12) = 448 px = 224 T; the frame counter
//   runs 0..191 (paper), loads 136 at 192 and runs to 255: 192 + 120 = 312 lines =
//   69888 T, the 48K frame. The V counter steps at the end of HSYNC (phase-2 px 96).
//   VSYN (and with it INT, through C7) rises when V reaches 192 — i.e. at the START
//   OF THE BOTTOM BORDER, like the Leningrad-1 — and falls 64 px later at the next
//   H2 edge (DD18.1 /R spike via C10): INT = 32 T. The paper of line 0 starts 120
//   lines + the remaining 96 px of phase 2 (48 T) after the INT, plus 2 T of pipeline
//   (the IR24 shift register loads at DC = 3). No /WAIT, no contention (CPU and video
//   alternate slots), no floating bus.
// Quorum 128 / 128+ — NOT derivable: the raster lives inside the ULA-M gate array.
//   The schematic shows only that it is clocked at 8 MHz (a 16 MHz crystal halved, or
//   an 8 MHz one) and hands the Z80 its clock (CLC4). The manual quotes "700 thousand
//   op/s", i.e. 3.5 MHz. The frame is taken as the Pentagon preset — what
//   UnrealSpeccy and SpeccyP use — and is UNCONFIRMED; measure on hardware.
//
// PORTS (ports.md, decode masks from ZXMAK2/Unreal):
//   #00  (A7,A4,A3,A0 = 0) write: D0 F_RAM shadow RAM at #0000, D1 UP_S (Quorum 64) the
//        screen from the #C000 page, D5 B_ROM (128) 1 = BASIC ROMs, 0 = SYS ROM,
//        D6 BLK_WR write-protect #0000-#3FFF, D7 TR_DOS 1 = TR-DOS from ROM,
//        0 = from the shadow RAM. Cleared by reset and by the NMI acknowledge.
//   #7FFD (0xxxxxxx xxx11x0x, 128) standard; #80FD (1x0xxxxx xxx11x0x, 128) CP/M
//        screen base in 8 KB steps (D0-D2) — write-once after reset.
//   #FE  (1xx11xx0) ULA; #7E (0xx11xx0) read: the extended keyboard, 6 bits.
//   #80-#83 WD1793, #85 drive/side/motor; #1F/#1B Kempston + printer BUSY in D7.
//   #EFF7/#DFF7/#BFF7 Gluk clock (add-on: only with Devices > CMOS + NVRAM).
//   Every other even port and the Beta #1F-family are NOT decoded — except on the
//   64 under the DOS signal (Quorum.cpp betaPort64).
//
// 128: ZXMAK2 MemoryQuorum256 is the model where the sources disagree — the #7FFD
// lock is #00 D6 (not #7FFD D5), SYS ROM only with #00 D5 = D7 = 0, the screen is
// page 5/7 by #7FFD D3. Deliberate deviations: #80FD is latched but moves nothing
// (as in ZXMAK2 / Unreal; the ULA-M's CP/M screen base is unknown), its D7 EN_RAM is
// not modelled; a snapshot load comes
// back with #00 = BASIC-running (#A0) — no snapshot format carries the register.
#pragma once
#include <stdint.h>

namespace Quorum {

// Quorum 64, relative to the 48K's paper anchor (TS_SCREEN_48 = 64 lines after INT).
constexpr int kPaperDelta64 = 120 * 224 + 48 + 2 - 64 * 224;   // 12594
constexpr int kIntLen64 = 32;
// Quorum-1024+ timing diagram ("Временные диаграммы формирования экрана"): a line is
// 448 px = 224 T (paper 256, right border 64, blanking 80 with HSYNC 32, left border
// 48). "Quorum" mode: 312 lines, bottom border 56, top 44, INT at line 248;
// "Pentagon" mode: 320 lines, bottom 48, top 64, INT at line 240 — the same point in
// the line in both. So the Quorum paper comes 16 lines earlier after the INT.
constexpr int kKvRasterShift = 16 * 224;

// #00 bits
constexpr uint8_t F_RAM = 0x01, UP_S = 0x02, RAM_8 = 0x08, ZX128 = 0x10, B_ROM = 0x20, BLK_WR = 0x40, TR_DOS = 0x80;

extern uint8_t p00;
extern uint8_t p80FD;
// Extended keyboard (#7E): ext[i] = keys pressed in row A(8+i), bits 0..5 active HIGH.
extern uint8_t ext[8];
// Alt+~ / PrtScr: F1-F6 go to the #7E keyboard (NC, CP/M) instead of our hot keys.
// Session state; a machine reset keeps it.
extern bool fkeysToGuest;

// CPU::reset: set g_quorum, clear the registers, take window 0 (and on the 128 the
// #7FFD windows) — the memory manager owns them from here on.
void reset();
// Window 0 (+ #C000 and the screen on the 128) from p00, #7FFD and the DOS state.
void remap();
// Every snapshot loader leaves the #7FFD latches; rebuild around them.
void afterSnapshot();
// Port dispatch ahead of the generic decode. true = handled.
bool portIn(uint16_t address, uint8_t* out);
bool portOut(uint16_t address, uint8_t data);
// Z80::check_trdos edges: PC at #3Dxx with DOS down, or PC >= #4000 with DOS up.
void trdosEdge(uint8_t pch);
// Z80::doNMI: the NMI acknowledge clears #00 (the SYS ROM appears at #0066 on the 128).
void nmiAck();
// Alt+F11 "Reset to" / web-catalog disk launch, called right after ESPectrum::reset():
// set #00 / #7FFD the way the SYS ROM's own menu does (QU7V42 #0083 Basic128:
// #7FFD = 0, #00 = #A8; #0097 Basic48: #7FFD = #17, #00 = #E0), TR-DOS = its ROM
// cold-started at 0 with the DOS signal up. PC is already 0.
enum Boot : uint8_t { BOOT_SYS, BOOT_TRDOS, BOOT_128, BOOT_48 };
void bootRom(Boot b);

}
