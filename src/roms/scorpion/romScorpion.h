#pragma once
#include <stdint.h>
// Scorpion ZS-256 v2.94 64K ROM (page order 0=BASIC-128, 1=BASIC-48, 2=service
// monitor, 3=TR-DOS 5.03 variant). bank0/bank1 are tiny overlays over the Sinclair
// 128K halves (scorpion_overlays.h, 434+175 B); bank2 is unique code and bank3
// diffs ~30% from trdos_505d AND rom[4] already overlays that base pointer
// (MemESP::registerOverlay is keyed by base) — both stay raw (scorpion_banks.c).
extern "C" const unsigned char gb_rom_scorpion_bank2[];
extern "C" const unsigned char gb_rom_scorpion_bank3[];
// The Scorpion GMX 512 KB boot ROM (8 ProfROM planes x 4 x 16K banks) is NOT
// embedded: the GM.DLS flash partition leaves ~1.6 MB for firmware and the ROM
// alone would eat all remaining headroom. It is loaded from SD (/gmx.rom or
// /roms/gmx.rom, e.g. MAME's gmx13500.rom) into butter PSRAM once per session —
// Config::requestMachine, R_SCORP_GMX case. rom[] pages read it via XIP
// pointers exactly like flash. GMX is butter-PSRAM boards only.
#include "scorpion_overlays.h"
