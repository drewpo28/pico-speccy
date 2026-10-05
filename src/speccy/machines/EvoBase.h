// pico-speccy — ZX Evolution BaseConf (R_EVO_BASE on the ATM arch).
//
// The board's ATM-compatible FPGA configuration: the ATM-Turbo 2+ memory manager
// (Atm.cpp: pF7 / a77 / remap) plus everything below, which lives here so Atm.cpp
// keeps the ATM boards only. Atm::evo (set by Atm::bindRoms) says the romset is live.
//
// Sources: svn.zxevo.ru pentevo fpga/base_trdemu (the build EVO Reset Service is made
// for; zports.v, atm_pager.v, zdos.v, znmi.v, zint.v, zclock.v, video_fontrom.v) and
// docs/zxevo_base_configuration_eng.pdf. Decode, port for port:
//   #xFF7 (A11:A10 = 11) / #x7F7 (= 01) at full low-byte decode, shadow ports only;
//   #EFF7 noshad: D2 128K mode (else Pentagon-1024), D3 RAM 0 at #0000, D4 turbo off,
//     D7 the Gluk ports; #DEF7/#BEF7 Gluk in shadow, #DFF7/#BFF7 outside it;
//   #BF (always): D0 shadow, D1 ROM write, D2 font RAM write, D3 1->0 NMI, D4 break,
//     D5 4096-colour palette; #BE = leave NMI / the FDD emulator; #BD = the config
//     read-back (A12..A8 index), #13BD write = the FDD emulator's drive mask;
//   NMI puts RAM page #FF in window 0 until OUT (#BE); base_trdemu maps #FE there
//   while EVO Reset Service emulates a drive; no INT gate; 14 / 7 / 3.5 MHz.
#pragma once

#include <stdint.h>

// #BF D2, the font RAM write enable (mirrored as g_atm_ro bit 7).
extern uint8_t g_atm_fnt;

namespace EvoBase {

extern uint8_t  pEFF7;
extern bool     inNmi;          // NMI state: RAM page #FF in window 0 until OUT (#BE)
extern bool     nmiClrPending;  // OUT (#BE) seen; in_nmi drops at the next control transfer
extern uint8_t  fddMask;        // #13BD: drives served by EVO Reset Service's FDD emulator
extern uint8_t  vgSys;          // last #FF (FDC system register) write
extern bool     inTrdemu;       // RAM page #FE in window 0 (base_trdemu in_trdemu)
extern uint8_t* font;           // text-mode character generator (char*8+line), 2 KB; null = the ATM ROM font
extern uint64_t intAckFrame;    // id of the INT window that was acknowledged (intWindowId)

// The id of the frame INT window CPU::tstates is sampling: the frame's own
// global_tstates, or the NEXT frame's when the sample sits in the frame-tail
// overshoot (Z80Ops::isActiveINT wraps tstates there — that is where a HALTed CPU
// takes the next frame's INT). Keying the ack on global_tstates alone made the
// tail sample look "already acknowledged" (it still carried the frame whose own INT
// had been taken at its start), so every INT came one NOP late, frame after frame:
// intT 4..7 against Pentagon's 0..3 (Across the Edge, hw 2026-10-04).
inline uint64_t intWindowId(uint32_t tstates, uint32_t statesInFrame, uint64_t globalT) {
    return globalT + (tstates >= statesInFrame ? statesInFrame : 0);
}

void reset();                   // from Atm::reset: the BaseConf register file (not the pages)
void bootPlain();               // Atm::bootRom: 128K mode, 3.5 MHz
void clockApply();              // { #xx77 D3, ~#EFF7 D4 } -> 14 / 7 / 3.5 MHz
void nmiEnter();                // Z80::doNMI
void nmiClrApply();             // the deferred OUT (#BE)

// Port hooks behind Atm::portWrite/portRead. true = consumed.
// .pss: EvoBase's part of the PSAT block (Atm::snapSave). Returns bytes written.
uint32_t snapSave(uint8_t* out);
void     snapLoad(const uint8_t* in, uint32_t n);

bool portWrite(uint16_t address, uint8_t data);
bool portRead(uint16_t address, uint8_t& v);

// NEMO IDE (Unreal IDE_NEMO_DIVIDE): #10 is a toggle — two reads give low then high
// byte of a word, so INIR/INIR moves a whole sector. Called from Ports.cpp's NEMO
// block with a NEMO image mounted; false = not an IDE port.
bool nemoRead(uint16_t address, uint8_t& v);
bool nemoWrite(uint16_t address, uint8_t data);

// The manager hooks Atm::remap / trdosTrap consult on a BaseConf.
inline bool mode1M() { return !(pEFF7 & 0x04); }    // #EFF7 D2 = 0: Pentagon-1024 paging
// Window 0 overrides ahead of the manager: NMI -> RAM #FF, base_trdemu -> RAM #FE,
// #EFF7 D3 -> RAM 0. true = override, page in `page`.
inline bool window0(uint32_t& page) {
    if (!(inNmi || inTrdemu || (pEFF7 & 0x08))) return false;
    page = (inNmi || inTrdemu) ? (0xFEu | (inNmi ? 1u : 0u)) : 0u;
    return true;
}
bool dosEnterOk();              // atm_pager.v dos_exec_stb: set-1 window 0 is ROM in #7FFD/DOS mode
bool ramExec(uint8_t w);        // atm_pager.v ram_exec_stb: window w's REGISTER is RAM

}
