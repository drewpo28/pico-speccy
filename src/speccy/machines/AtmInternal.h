// pico-speccy — the ATM memory manager's internals shared with the machines that
// sit on it (EvoBase.cpp, the ZX Evolution BaseConf). Not for anything else: the
// rest of the firmware talks to Atm.h.
#pragma once

#include <stdint.h>
#include "Atm.h"
#include "app/ESPectrum.h"

namespace Atm {

// #xFF7 data byte -> the pFFF7 form (Unreal io.cpp): D7 = low bits from #7FFD,
// D6 = RAM, D5..D0 = page inverted.
inline uint16_t f7enc(uint8_t v) {
    return (uint16_t)((((v & 0xC0) << 2) | (v & 0x3F)) ^ 0x33F);
}

// The DOS signal is raised by /CPM on the 2+ only. On the ATM1 the CP/M mode
// (#FE A7=0) does NOT open the TR-DOS ports — "в этом режиме недоступны порты
// TR-DOS, так что для работы с дисководом нужно прыгать в обычный режим"
// (atmdscr.htm, ATM-turbo 1): its BIOS drives the FDC from the SYSTEM ROM after a
// CALL into #3Dxx (see trdosTrap).
inline bool cpmOn() { return !atm1 && !(a77 & 0x200); }
// The shadow (DOS) PORTS, not the DOS ROM: TR-DOS / CP/M, or the ATM3's #BF D0.
inline bool dosPorts() { return ESPectrum::trdos || shaden; }

void dosRecalc();                              // ESPectrum::trdos from beta / /CPM
void set7ffd(uint8_t data);                    // #7FFD store + remap, no lock test
void palWrite(uint8_t data, uint8_t hi = 0xFF); // palette cell of the border colour
void loadSpecPalette();                        // the ZX colours into the palette RAM

}
