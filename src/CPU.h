/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or
visit https://zxespectrum.speccy.org/contacto

*/

#ifndef CPU_h
#define CPU_h

#include <inttypes.h>
#include "ESPectrum.h"

#define TSTATES_PER_FRAME_48 69888
#define TSTATES_PER_FRAME_128 70908
#define TSTATES_PER_FRAME_PENTAGON 71680
#define TSTATES_PER_FRAME_PROFI 69888
#define TSTATES_PER_FRAME_BYTE  69888
// Scorpion ZS-256: 224 T/line, uncontended, paper starting 14336 T after INT.
// Two PCB revisions differ ONLY in total frame length (ZXMAK2 UlaScorpion*/MAME):
// Yellow = 312 lines (the 48K frame), Green = 316 lines (MAME scorpiontb's +4).
#define TSTATES_PER_FRAME_SCORPION 69888
#define TSTATES_PER_FRAME_SCORPION_GR 70784

#define MICROS_PER_FRAME_48 19968
#define MICROS_PER_FRAME_128 19992
#define MICROS_PER_FRAME_PENTAGON 20480
#define MICROS_PER_FRAME_PROFI 19968
#define MICROS_PER_FRAME_BYTE  19968
#define MICROS_PER_FRAME_SCORPION 19968
#define MICROS_PER_FRAME_SCORPION_GR 20224   // 70784 T / 3.5 MHz

#define INT_START48 0
#define INT_END48 32
#define INT_END_BYTE48 33
#define INT_START128 0
#define INT_END128 36 // 35 in real +2 and Weiv's Spectramine. I'll have to check those numbers
#define INT_START_PENTAGON 0
// Pentagon INT pulse = 32 T, NOT the 36 T of a real 128K (which is where this
// value came from before). Settled from RTL: Karabas-Pro's pentagon_video.vhd
// re-evaluates int_sig only when `chr_col_cnt = 6 and hor_cnt(2 downto 0)="111"`
// — once every 8 character columns — and drives it low for the single window
// hor_cnt(5 downto 3)="100" (hor_cnt 32..39). One character column is 8 px = 4 T,
// so the pulse is exactly 8 * 4 = 32 T. Its TURBO branch narrows the window to
// 4 columns (hor_cnt 36..39), i.e. the CPU still sees 32 of ITS T-states at
// 7 MHz — a second, independent confirmation of the 32 figure.
// Cross-checks: ZXMAK2 UlaPentagon.cs c_ulaIntLength = 32, Unreal intlen = 32
// (unreal.ini). ZEsarUX alone says 36 (cpu.c "en spectrum, 32. en pentagon, 36")
// and the RTL does not support it.
// Why it matters: with 36 T a handler that returns between 33 and 36 T after the
// INT takes a SECOND interrupt, shifting every frame by ~19-33 T — cycle-exact
// border demos then tear. Same failure class as the Pentagon-1024 EFF7 D4 turbo
// window (see CLAUDE.md).
#define INT_END_PENTAGON 32
#define INT_START_PROFI 0
#define INT_END_PROFI 39
#define INT_START_SCORPION 0
#define INT_END_SCORPION 36   // libspectrum: 36 T INT pulse



/// TODO:
#define IRAM_ATTR

// Scorpion Yellow-PCB even-M1: the board holds /WAIT so every M1 cycle fetching
// from 0x4000+ starts on an even T-state (ZXMAK2 UlaScorpionYellow busRDM1,
// Unreal evenM1_C0=0xC0, MAME is_m1_even — the Green/Turbo+ boards dropped it).
// A plain global tested once per opcode fetch (the g_ngs_zxdma pattern) — set in
// CPU::reset only, so it can never be stale mid-frame.
extern bool g_scorp_even_m1;
// Scorpion GMX romset live (R_SCORP_GMX) — set in CPU::reset, gates the GMX port
// family, the 2 MB page composition and the ProfROM plane arithmetic.
extern bool g_scorp_gmx;
// Scorpion ZS-1024 (R_SCORP_1024): 1FFD D6,D7 extend the 0xC000 page to 64 pages
// (1 MB) — page = D7D6<<4 | D4<<3 | 7FFD 0-2 (MAME scorpion_update_memory, ZXMAK2
// MemoryScorpionProfRom1024 GetRamPage). Green/Turbo+ timing, no even-M1.
extern bool g_scorp_1024;
// GMX ProfROM 0x0100-0x010F read tap armed (service bank mapped at 0x0000, ROM
// visible). Recomputed by Ports::scorpionRomUpdate/check_trdos — one almost-
// always-false global test on the peek8/fetchOpcode hot paths.
extern bool g_gmx_tap;

class CPU
{
public:
    static void step();

    // call this for executing a frame's worth of instructions
    static void loop();

    static void updateStatesInFrame();

    // call this for resetting the CPU
    static void reset();

    // Flush screen
    static void FlushOnHalt();

    // CPU Tstates elapsed in current frame
    static uint32_t tstates;

    static int32_t prev_tstates;
    static uint32_t tstates_diff;

    // CPU Tstates elapsed since reset
    static uint64_t global_tstates;

    // CPU Tstates in frame
    static uint32_t statesInFrame;
    static uint32_t tstates_frame;  // tstates at end of last frame (before reset)
    static uint32_t tstates_active; // tstates of real code (excluding HALT idle)

    // Late timing
    static uint8_t latetiming;

    // INT signal lenght
    static int32_t IntStart;
    static int32_t IntEnd;

    // CPU Tstates in frame - IntEnd
    static uint32_t stFrame;

    static bool portBasedBP;

    static bool paused;
};

#endif // CPU_h
