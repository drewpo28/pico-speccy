#pragma once
#include <stdint.h>
// Scorpion ZS-256 v2.94 64K ROM (page order 0=BASIC-128, 1=BASIC-48, 2=service
// monitor, 3=TR-DOS 5.03 variant). bank0/bank1 are tiny overlays over the Sinclair
// 128K halves (scorpion_overlays.h, 434+175 B); bank2 is unique code and bank3
// diffs ~30% from trdos_505d AND rom[4] already overlays that base pointer
// (MemESP::registerOverlay is keyed by base) — both stay raw (scorpion_banks.c).
extern "C" const unsigned char gb_rom_scorpion_bank2[];
extern "C" const unsigned char gb_rom_scorpion_bank3[];
#include "scorpion_overlays.h"
