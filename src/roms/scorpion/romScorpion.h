#pragma once
#include <stdint.h>
// Scorpion ZS-256 v2.94 64K ROM (page order 0=BASIC-128, 1=BASIC-48, 2=service
// monitor, 3=TR-DOS 5.03 variant). bank0/bank1 are tiny overlays over the Sinclair
// 128K halves (scorpion_overlays.h, 434+175 B); bank2 is unique code and bank3
// diffs ~30% from trdos_505d AND rom[4] already overlays that base pointer
// (MemESP::registerOverlay is keyed by base) — both stay raw (scorpion_banks.c).
extern "C" const unsigned char gb_rom_scorpion_bank2[];
extern "C" const unsigned char gb_rom_scorpion_bank3[];
// Scorpion GMX 512 KB boot ROM (8 ProfROM planes x 4 x 16K banks), embedded in
// flash on GMX-capable builds only — GMX_IN_FLASH is set by CMake for every
// board but MURM1 (whose SPI PSRAM cannot back the GMX page strip / 640x200
// pages), and the linker shrinks the GM.DLS partition on those builds to make
// room (rp2350-memmap.ld __gmx_rom_in_flash). scorpion_gmx_rom.c.
#if GMX_IN_FLASH
extern "C" const unsigned char gb_rom_scorpion_gmx[];
#endif
#include "scorpion_overlays.h"
