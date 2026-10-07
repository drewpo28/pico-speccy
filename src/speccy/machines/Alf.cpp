// pico-speccy — ALF TV Game ports. See Alf.h. FLASH-resident on purpose.
#include "Alf.h"
#include "app/LastRun.h"

#include "app/Config.h"
#include "app/Debug.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/roms.h"
#include "fatfs/ff.h"        // fopen2/fclose2/FIL/f_read/f_lseek/f_size, UINT, FSIZE_t
#include <stdlib.h>
#include <string.h>

#ifndef bitRead
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#endif

uint8_t Alf::newBit = 0;

// The ROM-bank select (OUT with A7=0, A0=1): bits 0-6 the bank, bit 7 cart vs system
// ROM. `force` rebinds the system ROM even when its bank 0 already looks mapped —
// a snapshot restore cannot trust whatever the rom[] slots held before.
static void selectBank(uint8_t data, bool force) {
    bool cart = bitRead(data, 7);
    MemESP::romInUse = (data & 0b01111111);
    while (MemESP::romInUse >= 64)
      MemESP::romInUse -= 64; // rolling ROM
    if (cart && Alf::Cart::active()) {
      // Lazy SD cartridge: fault the selected 16K bank into the window on demand
      // (like wd1793 faults a sector). Cart ROM is only ever visible at page 0, so
      // binding just the selected bank suffices. Banks past the image = open bus.
      int b = MemESP::romInUse;
      MemESP::rom[b].assign_rom(b < Alf::Cart::bankCount()
                                  ? Alf::Cart::residentBank(b) : gb_rom_Alf_ep);
    } else if (cart) {
      // Cart selected but none mounted: empty drive (open bus), like TR-DOS w/o disk.
      MemESP::rom[MemESP::romInUse].assign_rom(gb_rom_Alf_ep);
    } else {
      // System ROM (gb_rom_Alf, 32KB = 2 banks in flash); banks 2+ → open-bus zeros.
      if (force || MemESP::ramCurrent[0] != gb_rom_Alf) {
        for (int i = 0; i < 64; ++i)
          MemESP::rom[i].assign_rom(i >= 2 ? gb_rom_Alf_ep
                                           : gb_rom_Alf + ((16 * i) << 10));
      }
    }
    MemESP::recoverPage0();
}

bool Alf::portWrite(uint16_t address, uint8_t a8, uint8_t data) {
  if (a8 == 0xFE) {
    newBit = (data >> 3) & 1;
  }
  if (bitRead(address, 7) == 0 &&
      (address & 1) == 1) { // ALF ROM selector A7=0, A0=1
    selectBank(data, false);
    return true;
  }
  return false;
}

// The select byte is not latched anywhere: the cart bit is recovered from what
// the live bank points at. A cart bank past the image and a system bank 2+ both
// read the open-bus page, so the ambiguity there is invisible to the guest.
uint8_t Alf::snapSelector() {
    const uint8_t b = MemESP::romInUse & 0x3F;
    const bool cart = Cart::active() && MemESP::rom[b].direct() == g_alfWindow;
    return b | (cart ? 0x80 : 0);
}

void Alf::snapRestore(uint8_t selector, uint8_t nb) {
    newBit = nb & 1;
    selectBank(selector, true);   // after newSRAM/romLatch: it ends in recoverPage0
}

void Alf::portRead(uint8_t p8) {
  if (bitRead(p8, 1) == 0) { // 1D
    MemESP::newSRAM = true;
    MemESP::recoverPage0();
  } else { // 1F
    MemESP::newSRAM = false;
    MemESP::recoverPage0();
  }
}

// ALF cartridges are served lazily from SD on demand, like a wd1793 disk: no
// built-in cart, no flash region. There is no default cart — the cart "drive" is
// empty (open bus) until the user mounts a .rom/.bin from SD, exactly like TR-DOS
// with no disk inserted. The system ROM (gb_rom_Alf) runs when nothing is mounted.
void Alf::bindCart() {
    if (!Config::alfCartPath.empty()) {
        if (!Cart::active() || Cart::path() != Config::alfCartPath) {
            if (!Cart::mount(Config::alfCartPath)) {
                Config::alfCartBanks = 0;   // SD file gone (card removed) → empty drive
                Config::alfCartPath  = "";
                return;
            }
        }
        Config::alfCartBanks = (uint8_t)Cart::bankCount();
    } else {
        Cart::unmount();
    }
}

// ---------------------------------------------------------------------------
// Lazy SD cartridge (formerly AlfCart.cpp)
// ---------------------------------------------------------------------------

#define ALF_BANK_SZ (16u << 10)   // 16 KB cartridge bank / window size
#define ALF_MAX_SZ  (1u << 20)    // clamp to 1 MB (ignore any trailing footer)

uint8_t* g_alfWindow = nullptr;   // visible to MemESP::writebyte (ROM-write guard)

namespace {
    FIL*        g_file    = nullptr;
    int         g_curBank = -1;    // bank currently held in g_alfWindow (-1 = none)
    int         g_banks   = 0;     // cart size in 16K banks
    std::string g_path;
}

bool Alf::Cart::mount(const std::string& p) {
    unmount();
    g_file = fopen2(p.c_str(), FA_READ);
    if (!g_file) { Debug::log("Alf::Cart: open failed %s", p.c_str()); return false; }
    size_t size = (size_t)f_size(g_file);
    if (size == 0) { fclose2(g_file); g_file = nullptr; return false; }
    if (size > ALF_MAX_SZ) size = ALF_MAX_SZ;
    g_banks = (int)((size + ALF_BANK_SZ - 1) / ALF_BANK_SZ);
    g_alfWindow = (uint8_t*)malloc(ALF_BANK_SZ);
    if (!g_alfWindow) {
        fclose2(g_file); g_file = nullptr; g_banks = 0;
        Debug::log("Alf::Cart: OOM"); return false;
    }
    g_curBank = -1;
    g_path = p;
    residentBank(0);   // prefault the catalog / front-end bank
    Debug::log("Alf::Cart: mounted %s (%d banks)", p.c_str(), g_banks);
    LastRun::note(p);
    return true;
}

void Alf::Cart::unmount() {
    if (g_file)      { fclose2(g_file);  g_file = nullptr; }
    if (g_alfWindow) { free(g_alfWindow); g_alfWindow = nullptr; }
    g_curBank = -1; g_banks = 0; g_path.clear();
}

bool Alf::Cart::active()   { return g_alfWindow != nullptr && g_file != nullptr; }
int  Alf::Cart::bankCount(){ return g_banks; }
const std::string& Alf::Cart::path() { return g_path; }

uint8_t* Alf::Cart::residentBank(int bank) {
    if (!g_alfWindow || !g_file) return g_alfWindow;
    if (bank == g_curBank)       return g_alfWindow;   // already in the window
    UINT br = 0;
    f_lseek(g_file, (FSIZE_t)bank << 14);              // bank * 16384
    if (f_read(g_file, g_alfWindow, ALF_BANK_SZ, &br) != FR_OK) br = 0;
    if (br < ALF_BANK_SZ)                              // short last bank / footer
        memset(g_alfWindow + br, 0xFF, ALF_BANK_SZ - br);  // open-bus pad
    g_curBank = bank;
    return g_alfWindow;
}

