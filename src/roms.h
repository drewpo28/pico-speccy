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

#ifndef ROMS_H
#define ROMS_H

#include "roms/48k/romSinclair48K.h"
#include "roms/48k/rom48Kcustom.h"
#include "roms/128k/romSinclair128K.h"
#include "roms/128k/rom128Kspanish.h"
#include "roms/128k/romPlus2.h"
#include "roms/128k/RomPlus2spanish.h"
#include "roms/128k/rom128Kcustom.h"
#include "roms/128k/S128_ZX81+_ROM.h"
#include "roms/plus3/romPlus3.h"
#include "roms/plus3e/romPlus3e.h"

#include "roms/48k/byte/romByte48k.h"
#include "romGluk.h"
#include "roms/romSTS75.h"

// gb_rom_Alf_cart (built-in "Elf-1") removed — ALF carts are served lazily from SD
// (see AlfCart). gb_rom_Alf = ALF system ROM; gb_rom_Alf_ep = open-bus filler.
extern "C" const unsigned char gb_rom_Alf[];
extern "C" const unsigned char gb_rom_Alf_ep[];
// TR-DOS: ONE raw base, 5.04T, and 5.03 / 5.04TM / 5.05D as small read-only
// overlays over it (src/roms/trdos/, tools/rom_pack.py), applied on the fly by
// MemESP (see RomOverlay.h) — no RAM copy, no flash write, no reboot. The base is
// 5.04T rather than 5.05D because TS-BIOS ROM page 1 IS 5.04T and TS-Conf reads its
// ROM window as a RAW POINTER (TsConf::romPtr), i.e. it cannot see an overlay: what
// that machine needs has to be the base. Costs 1046 B of overlays instead of 855+454
// and saves the 16 KB copy that used to sit inside the TS-BIOS blob.
#include "roms/trdos/trdos_base.h"
#include "roms/trdos/trdos_overlays.h"
#include "roms/48k/48k_overlays.h"
#include "roms/128k/128k_overlays.h"
// rom[0] of the 128K family: the raw base is the PENTAGON ROM0 and the stock
// Sinclair 128K first half is the 101-byte overlay (same reason as TR-DOS above —
// this ROM is TS-BIOS page 2). rom[1] is shared by both with no overlay at all.
#include "roms/pentagon/pentagon_base.h"
#include "roms/pentagon/pentagon_overlays.h"
#include "roms/plus3/plus3_overlays.h"
#include "roms/plus3e/plus3e_overlays.h"
extern "C" const unsigned char gb_rom_4_trdos_custom[];
// gb_rom_pentagon_128k (32K blob) removed: Pentagon is a 101-byte overlay over the
// Sinclair 128K base now (roms/pentagon/). Custom Pentagon uses the 128K custom slot.
extern "C" unsigned char gb_rom_esxdos[];
extern "C" unsigned char gb_rom_esxide[];
#include "roms/profi/romProfi.h"
#include "roms/scorpion/romScorpion.h"
#include "roms/tsconf/tsconf_roms.h"
#endif
