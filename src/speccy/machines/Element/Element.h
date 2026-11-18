// eLeMeNt ZX (A_ELM): a real Z80 on an FPGA board, base personality Pentagon 1024,
// no FDC (esxDOS over DivSD). docs/element-zx-plan.md holds the sources, phases and
// decisions; the reference is the Programmer's Reference (Google Doc, core 20261012).
// There is no RTL: what the document leaves open is settled by the programs.
//
// The three ZXi register files, the identity and date the software checks, "no MB03+"
// and URAM with LCU1. Registers of what is not built yet are stored only.
#pragma once
#include <stdint.h>

namespace Elm {

void reset();                                 // from CPU::reset
bool portIn(uint16_t address, uint8_t* out);  // true = claimed
bool portOut(uint16_t address, uint8_t data);

void frameEnd();                              // EndFrame
void flush(bool force = false);               // flash image / FRAM write-back (main loop; force on reboot)
bool enterSetup();                            // next reset boots the SetUp ROM (block 7); false = not flashed
// The SetUp leaves by RST 0 (Shift+R, Shift+L): the board restarts, so the emulator
// does a machine reset too instead of letting the ROM run on into a warm esxDOS.
// rst0 is called from the Z80 core on every RST 0 (flash, rare); takeReset is
// polled at the frame end (ESPectrum.cpp) and clears the request.
void rst0(uint16_t pc);
int takeReset();                              // 0 = none, 1 = restart (esxDOS cold), 2 = warm (the EXTRA button: RAM and divSD kept)
// The SetUp's registers as the emulator's settings: joystick, Timex, ULAplus,
// GigaScreen, TSFM, SAA1099, Z-Controller. From ESPectrum::setup before the audio
// and video come up (the FRAM, so the boot allocates what the SetUp says); every
// later reset re-applies the live registers with the hooks and a quiet reconcile.
void bootSettings();
// The machine personality (e_zxi_000) as the menu sees it: the live register, or the
// FRAM's before the machine ran. setPersonality = Machine > eLeMeNt ZX: the register,
// the FRAM and the board's restart (the SetUp's own way).
uint8_t personality();
void    setPersonality(uint8_t p);
// Settings > registers: after the menu commits, every SetUp option this emulator
// shares (joystick, Timex, ULAplus, GigaScreen, TSFM, SAA, Covox, Z-Controller, esxDOS,
// AA-SRAM, AY stereo, scan lines, DMA, time zone, UART) is written into its e_zxi
// register and the FRAM, so the SetUp shows it and the power-up keeps it.
void    syncFromSettings();
// The emulator's turbo controls on this machine (Alt+F2, the fast menu's CPU speed, F11 in the
// Spectrum menu): e_zxi_030 itself, all eight SetUp clocks, FRAM included (owner 2026-11-18).
uint8_t     speed();                  // e_zxi_030 now (0-7)
const char* speedName(uint8_t sel);   // "20 MHz", "OverClock 28 MHz (RAM)", ...
void        setSpeed(uint8_t sel);    // apply + FRAM + Config::turbo + the clock banner
// The two buttons of the board on the PC keyboard. EXTRA = Win+F11 ("ZX + F11" in the
// User Manual): a short press does what e_zxi_020 says, holding it 1.5 s with
// e_zxi_021 = 01 opens the "Spectrum menu" (rainbow in the border) where 1-8 pick the
// CPU speed (5-8 with OverClock, e_zxi_031). RESET = the hard-reset hot key
// (`resetVk`, F11): released within 1.5 s = the reset, held longer = the SetUp
// firmware, as on the board. The CPU is held while either button is down.
// Returns what the caller has to do; EK_NONE = not ours, the key goes on.
enum { EK_NONE = 0, EK_TAKEN, EK_RESET, EK_SETUP };
int  extraKey(uint16_t vk, bool down, bool gui, bool plain, uint16_t resetVk);
bool rebootWanted();                          // the SetUp changed the URAM size (AA-SRAM): the strip is laid out at boot
// The personality e_zxi_000 names, applied at reset only (the SetUp restarts, FLASH6
// flips it and back without one): CPU::reset arms it before its timing chain, then
// cpuFlags() sets the Z80Ops machine flags, the floating bus and the frame target
// the way the stock 48K / 128K / +2A / Pentagon branches would.
void armPersonality();
void cpuFlags();
void remap();                                 // URAM back over the legacy paging
bool memWrite(uint16_t address, uint8_t v);   // true = the store is consumed
bool memWriteCold(uint16_t address, uint8_t v);   // memWrite's flash part (memWrite itself: ElmHgfx.cpp, RAM)
enum { WK_VRAM = 1, WK_CHUNKY = 6, WK_COW = 13 };   // wrKind of the HGFX VRAM / chunky windows; the pair frame's captured screen (Hgfx::cowWrite)
void masksRefresh();                          // the eLeMeNt loop's slot masks again after a wrKind change (Hgfx's COW)
extern const uint8_t* vramHi;                 // g_zxi_021: the VRAM window's high address byte
extern uint16_t slotPage[8];                  // chunky window: its 8K step per slot
void memRead(uint16_t address);               // a read of the HGFX VRAM window arms COPY
extern volatile uint8_t g_elm_vram_rd;        // non-zero while memRead is wanted (COPY mode)
// Chunky windows across the edge of the HGFX SRAM half (ElmHgfx.cpp): a bit per 8K slot.
// A read of such a slot goes byte by byte through splitRead (the generic read funnel,
// CPU.cpp gsDmaPeek8, and busRead); the slot's raw pointer serves a fetch only.
// Since 2026-11-18 the same gate also carries the ROM area (slots 0-1) under the OverClock ROM
// wait below: splitRead charges the wait T and reads the slot as the funnel would.
extern volatile uint8_t g_elm_split;
uint8_t splitRead(uint16_t address);
// e_zxi_030, the clocks that are not a power of two of 3.5 MHz (2026-11-18). All run on the
// 28 MHz frame (multiplicator 3: the beam, the AY and the master counter stay in real time):
//   03 / 04 (20 / 22 MHz): every T the Z80 runs is charged 28/20 or 28/22 (g_elm_stretchK,
//     extra T per T in 16.16), at the eLeMeNt loop's line checks and exit and per instruction
//     of the shared loop (Z80_JLS.cpp elmStretch); what would pass the frame end is carried.
//   05 / 07 (OverClock, "1T delay in ROM area"): +1 T per M1 and per read below #4000
//     (g_elm_romWait; the reads through g_elm_split). 07 runs at 28, not 30 (deviation).
// Both need the fastmem level 1 gate (g_elm_fastrd: the turbo ZX screen / HGFX); without it
// the clock is a plain 28 MHz.
extern uint8_t  g_elm_romWait;
extern uint32_t g_elm_stretchK, g_elm_stretchFrac, g_elm_stretchDebt, g_elm_shT;
void leave();                                 // CPU::reset of another machine: the eLeMeNt's gates off
bool fastReadOk();                            // the fast read path may bypass memRead
void busWrite(uint16_t address, uint8_t v);   // a DMA write, windows included
uint8_t busRead(uint16_t address);            // a DMA read: arms the HGFX COPY latch too
// A Z80 DMA memory-to-memory piece, both ends counting up: an HGFX COPY inside the VRAM
// window goes to the engine as a run. Returns the bytes done (time charged), 0 = none.
uint32_t dmaCopy(uint16_t src, uint16_t dst, uint32_t n, uint32_t cycles);
// A Z80 DMA piece from the SD data port #EB (fixed) into plain RAM (counting up): the
// block's data bytes straight from the sector buffer. Returns the bytes done (time
// charged), 0 = none (read the next byte through dmaInput).
uint32_t dmaFromCard(uint16_t port, uint16_t dst, uint32_t n, uint32_t cycles);
// The 48 ROM key wait (RET to #15E1, Z80::check_trdos): skip whole idle passes to the slice end.
void keyWaitSkip();
bool nmiBlocked();                            // SafeNMI: the button is locked
void nmiTaken(uint16_t pc);                   // SafeNMI: the return address
extern uint8_t* wrKind;                       // per high address byte: 0 = plain RAM
// Per 8K slot, for Z80::exec_nocheck_elm (elm_memory.h): a set bit sends the loop's
// reads / writes of that slot to the generic accessors. Bits 0-1 always (ROM, divSD,
// URAM zone 0); a slot with any window page in it (writes); the VRAM window's slots
// in COPY mode and a split chunky window (reads). elmMasksUpdate keeps them.
extern volatile uint8_t g_elm_rdmask, g_elm_wrmask;

// The ports the machine claims ahead of the generic decode, tested in the RAM path:
// #xx3B, #9EFD/#9FFD, the URAM ports #07 #27 #47 #67 #53 #73 and the DACs #0F-#7F
// (low byte only), #7FFD for the 48K personalities' ROM select and, with #1FFD/#DFFD/
// #FDFD, the Pentagon 4096 paging. portIn leaves the DAC ports and the xxFD ports to the
// generic decode.
static inline bool isPort(uint16_t address) {
    const uint8_t lo = (uint8_t)address;
    return lo == 0x3B || (lo & 0x9F) == 0x07 || (lo & 0xDF) == 0x53 ||
           (lo & 0x8F) == 0x0F || lo == 0x2B || (address & 0xF0FF) == 0x0003 ||
           (address & 0xFEFF) == 0x9EFD || address == 0xEFF7 ||
           (lo == 0xFD && ((address >> 8) == 0x7F || (address >> 8) == 0x1F || (address >> 8) == 0xDF || (address >> 8) == 0xFD));
}

}
