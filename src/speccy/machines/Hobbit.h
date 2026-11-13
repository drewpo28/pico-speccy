// pico-speccy — PK Hobbit (InterCompex / Severny Press, Leningrad, 1990-93).
// FLASH-resident: the hot paths test g_hobbit (CPU.h) and call in here.
//
// Romsets R_48K_HB* of A_48K: a 48K-compatible on discrete logic + 3 x KR580VV55A
// (8255) + 2 PLMs, 64 KB of static RAM, a 27512 ROM, a KR1818VG93 (WD1793) floppy
// controller and a KR580VV51A (8251) network port. No ULA: CPU and video take
// alternate memory slots (CLK/2), so no contention, no snow, no floating bus.
//
// Sources: github.com/bw2012/pk-hobbit — docs/Architectura.pdf (the designers' own
// description of the logic), docs/sys/*.gif (the system programmer's guide:
// Appendix A, tables A.1-A.5, "Память", "Система прерываний", "Ввод-вывод"), rom/
// (seven 64 KB dumps) — and the disassembly of SHADOW-A, which is what settles
// everything the two documents leave to the PLMs.
//
// MEMORY. 64 KB of RAM: <a> #0000-#1FFF and <b> #2000-#3FFF are the "shadow" RAM,
// <c> #4000-#FFFF the Spectrum's. The ROM is five pages picked by the 3-bit
// CONFIGURATION (table A.2, Architectura p.3):
//   0 SINCLAIR  / 1 SINCLAIR-X   ROM #0000 (the stock 48K ROM) at #0000-#3FFF
//   2 DOS                         ROM #4000 at #0000-#3FFF
//   4 LOGO (FORTH in one dump)    ROM #8000 at #0000-#3FFF
//   3 CP/M / 5 RAM                <a> <b> — 64 KB of RAM; 3 moves the screen to #E000
//   6 SHADOW-B                    ROM #C000 at #0000-#1FFF, <b> above
//   7 SHADOW-A                    ROM #E000 at #0000-#1FFF, <b> above
// The configuration is PB0-PB2 of the 8255 D25, i.e. port #0F:
//   D7 allow the hardware to modify the configuration (the ES gate below),
//   D6 keyboard scan off, D5 no NMI on IN (#FE), D4 NMI from the "framing" keys
//   (the 48 outside the Spectrum's 40), D3 no NMI from any key, D2-D0 configuration.
// Reset leaves the three 8255s in input mode and the configuration lines pulled
// high: SHADOW-A. Its first two OUTs are the 8255 mode words — in the shadow
// configurations #0C / #0D / #0E are the control registers of D25 / D24 / D23, and
// a mode word clears the output latches, so OUT (#0C),#80 is "configuration 0".
// (SHADOW-A's boot stub at #5B03 relies on exactly that to read the Sinclair font.)
//
// Writes below #4000 (PLM D8, see TIMING below): SINCLAIR and DOS write-protect the
// shadow RAM; under the LOGO and shadow ROMs a write lands in the RAM underneath.
// The helpers SHADOW-A installs at #3C3E-#3C7D switch the configuration with OUT
// (#0F) between SHADOW-A, SHADOW-B and the RAM only (ES up, D7 = 0): <b> is at
// #2000-#3FFF in all of them. (Two earlier beliefs, "#3C00-#3CFF reads as RAM in
// SINCLAIR" and "the DOS writes under its ROM", were props for a wrong ES rule.)
//
// NMI. Three sources: the 8251 (not modelled), IN (#FE) while D5 = 0, and /INSHAD
// in the SINCLAIR configurations — a key, or the opcode fetch of one of 28
// "interception" addresses of the Sinclair ROM. The request is latched at the end
// of that M1, the instruction completes, and the NMI's first stack write sets the
// ES flip-flop, which opens SHADOW-A (Architectura: "all three bits to 1") — but
// only while #0F D7 = 1: with ES up the service writes #3E to read SHADOW-B's texts,
// #3D for the RAM, #B8 to come back, and MAGIC OPTION writes #38 to reach the DOS
// (SINCLAIR, then the #3D2F trap). SHADOW-A #0066
// compares the stacked return address with its table (#1EE4 in V4.4) — RST 8 and
// CALL #12AC with a function number in I are the shadow system's API, #1299 is
// the copyright call, which is how the power-on menu appears — and leaves through
// the RETN at #0063: fetching its second byte in configuration 7 clears ES.
//
// DOS. An opcode fetch from #3D00-#3DFF in configuration 0 (whatever D7 is) sets a
// flip-flop that ORs 2 into the configuration; a fetch from #4000-#FFFF clears it.
// The DOS ROM has NOP NOP NOP at #0063 in front of its #0066 magic entry: a fetch of
// #0064 drops ES there as SHADOW-A's RETN does (modelled at the #3Dxx trap itself).
// In the DOS configuration the ports are the Beta-128's (#1F/#3F/#5F/#7F/#FF).
//
// PORTS, as far as SHADOW-A / SHADOW-B use them (shadow and RAM configurations):
//   #0C/#0D/#0E W  8255 mode words (D25, D24, D23)      #0F W  configuration (D25.B)
//   #05 R  the configuration lines in D0-D2, D5-D7 = register D46: the half-row the
//          last IN (#FE) addressed, priority-encoded (any configuration)
//   #06 R  D25.A read back: what #FE was last written
//   #0F R  D23.B: number of the key being scanned (table A.5; one key a line, the low
//          bits of the line counter), D7 = 1 while it is down
//   #35 W  8251 command D1 (/DTR) = 0: the display reads #E000 (the "CP/M screen")
//   #1F R  (SHADOW-A only) D24.C: Kempston D0-D4, D5 network MASTER, D6 FDC DRQ, D7 INTRQ
//   VG93 (PLM D8): registers #1F/#3F/#5F/#7F — write in configurations 2-7, read in 2-6;
//          #FF read = D24.C (DRQ, INTRQ) in 2-7; #FF write = D25.C (drive, /MR, HLT, side)
//          in 2 and 3 only. CP/M (3) drives the controller exactly as the DOS does.
//   #34/#35  8251 data / control (network): absent machine, reads 0
//   #FE  the Spectrum's
// Not modelled: the local network, the printer, the 60 Hz display PLM.
//
// KEYBOARD. 40 "core" keys on the Spectrum's matrix plus up to 48 "framing" ones that
// only the scanner sees (table A.5: Del #49, the cursors #5A-#5D, Edit #41, Break #55,
// Page #6D, the keyword keys Cat / Open# / Move / ...). With #0F D4 = 1 a framing key
// going past the scanner in a SINCLAIR configuration is an NMI; SHADOW-A #007D reads
// its number (D7 = 1) from #0F right there and SHADOW-B #0F6C turns it into the
// Spectrum's code. LOGO and BASIC edit with those keys — Caps Shift + 0 is a "0" to
// the shadow's decoder — so the PC's Backspace and arrows are framing keys here
// (keysMode, Alt+~ cycles it; the NMI hot key still uses function #52).
//
// TIMING, from the dump of PLM D9 (cpld/556RT2-9.TXT; inputs I5-I0 = the character
// counter X, 4 T a character, I14-I6 = the line counter Y; outputs latched by DD51):
//   /HRES  X = 56                  a line is 56 characters = 224 T
//   /VRES  Y = 312                 a frame is 312 lines = 69888 T, the 48K's
//   BORDER X = 0, X >= 33, Y >= 192    the paper is Y 0-191, X 1-32
//   BLANK  X 38-47, Y 220-244      border: 9 characters left, 5 right, 67 lines above
//                                  the paper, 28 below
//   /INT   Y = 246, X 48-55        32 T; also D23 PA5 (IN (#FE) D5)
//   /SS    Y6 = 0, Y & 7 = 5, X 4-15   the core keys' row goes to the keyboard RAM
// So the paper's first character starts (312 - 246) * 224 - 47 * 4 = 14596 T after the
// INT, 260 T later than a 48K's. Not measured on a real machine: hw 2026-11-13 owed.
// The key scanner shares Y: the number on #0F is Y's low seven bits, and Y restarts at
// 312, so numbers 0-55 come round three times a frame and the rest twice.
//
// The other two PLMs, read against pcb/1.pdf (pins) — they confirm the model:
//   D7 (address): the port decode, #3Dxx (/INBETA), #0064, the 28 interception
//     addresses (identical to SHADOW-A's table), the #FE half-row priority encoder.
//   D8 (control): RAM read = #4000+ | cfg 3 | cfg 5 | cfg 6/7 & A13; ROM otherwise
//     (so #3C00-#3CFF is plain ROM in SINCLAIR); writes below #4000 only in cfg 3-7
//     (SINCLAIR AND DOS are write-protected); ROM A13 = 0 for cfg 6 and for the low
//     half of cfg 0-5; /INSHAD = an interception address or a key in cfg 0/1 only;
//     /OUTSHAD = #0064 in cfg 7 only.
//   Gates: CF = CFG | (PB7 & ES) for all three bits (ENCFG, DD72:4), CF1 also | the DOS
//     flip-flop DD32:2. That flip-flop, while set, holds the NMI latch and ES cleared
//     (DD48:1 -> DD48:2 -> /S of DD34:2 and DD31:2): entering the DOS ends the shadow
//     state — MAGIC OPTION — and no NMI is taken inside the DOS. ES = the NMI latch
//     sampled at the end of every write (DD31:2 clocked by /WR).
//   Not followed: the schematic sets the DOS flip-flop on a fetch from #3Dxx in ANY
//     configuration (D7 decodes the address alone); the model keeps Architectura's
//     "SINCLAIR only" — LOGO and the monitors have bytes at #3Dxx. Unresolved.
#pragma once
#include <stdint.h>

struct hobbit_rom_bank_t;

namespace Hobbit {

// PLM D9: /INT is 8 characters long; the paper against the 48K's anchor (TS_SCREEN_48,
// 64 lines after the INT).
constexpr int kIntLen = 32;
constexpr int kPaperDelta = (312 - 246) * 224 - 47 * 4 - 64 * 224;   // +260

// Config::requestMachine: the romset's pages (hobbit_banks.h roles dos, logo, mon).
void bind(const hobbit_rom_bank_t* banks);
// CPU::reset: set g_hobbit, take windows 0/1. The shadow RAM is static and survives
// a RESET; it is cleared when the machine (or its romset) has just been selected.
void reset();
// Every snapshot loader: no foreign format carries #0F — BASIC running (#B8).
void afterSnapshot();
// .pss record PSHB: u8 version (1), #0F, flags (b0 ES, b1 DOS flip-flop, b2 the
// display on #E000, b3 an IN (#FE) NMI pending), the 8251 control port's state,
// u16 pending interception return address (0 = none). The shadow RAM is the file's
// fourth page (RAM page 1, as on the Quorum 64). snapLoad applies it and remaps;
// the afterSnapshot() that follows the load then keeps it.
constexpr uint32_t SNAP_MAX = 6;
uint32_t snapSave(uint8_t* b);
bool snapLoad(const uint8_t* b, uint32_t n);
// Port dispatch ahead of the generic decode. true = handled.
bool portIn(uint16_t address, uint8_t* out);
bool portOut(uint16_t address, uint8_t data);
// Opcode fetch below #4000 in the SINCLAIR configurations (through the Timex SCLD
// fetch hook, Timex.h): the DOS trap and the interception addresses.
uint8_t m1(uint16_t pc);
// Z80::check_trdos, DOS flip-flop up and PC >= #4000.
void dosLeave();
// Z80::doNMI: ES up. Z80 RETN: ES down when it is the one at #0063.
void nmiAck();
void retn(uint16_t pcAfter);
// The NMI hot key: the shadow system's menu, through its own function #52 (the
// framing keys, PAGE / SHADOW MODE among them, are not modelled).
void shadowKey();
// Which PC keys are the Hobbit's own (ESPectrum hobbitKeys): EDIT = Backspace, the
// arrows and Right Alt = Rus/Lat; ALL = also F1-F8 (Cat, Open#, Move, Lprint, Verify, Over, Bright, Paper —
// the eight macro keys, in WRITE MACROS' order), Ins Edit, Del Del, Home Rus/Lat,
// End Break, PgUp Register, PgDn Page; OFF = Caps Shift + 0 / 5..8 as on a 48K (games).
enum : uint8_t { KEYS_EDIT, KEYS_ALL, KEYS_OFF };
extern uint8_t keysMode;
// Every keyboard poll: the table A.5 number of the framing key held, 0 = none.
// sym: the Hobbit's own key for the PC punctuation key held ( . , / = - [ ] : ( ) * +
// < > @ # $ & ), 0 = none. The Spectrum combo stays in the matrix for whatever reads
// #FE, so this one raises no NMI: it is what the scanner (#0F) shows INSTEAD of the
// core keys — FORTH reads only the scanner and knows no Symbol Shift.
void frameKey(uint8_t code, uint8_t sym);
// For a "Reset to" chooser (not wired yet): after ESPectrum::reset() — BASIC without
// the power-on menu, the DOS, LOGO. The monitor has not run then.
enum Boot : uint8_t { BOOT_MENU, BOOT_BASIC, BOOT_DOS, BOOT_LOGO };
void bootRom(Boot b);

}
