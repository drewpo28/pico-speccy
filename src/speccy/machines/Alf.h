// pico-speccy — ALF TV Game: the port handling that used to sit inline in the
// RAM-resident Ports::input/output, and the lazy SD cartridge loader (formerly
// AlfCart). FLASH-resident: ALF is one machine of ~20 and its ROM-bank latch is
// written once per bank switch. See CLAUDE.md "Machine code must not live in the
// SHARED RAM hot paths".
#pragma once
#include <stdint.h>
#include <string>

namespace Alf {

// OUT D3 last written to #FE, read back as bit 3 of the floating-bus answer.
extern uint8_t newBit;

// OUT handler. Latches newBit on #FE; returns true when the write was the ROM-bank
// select (A7=0, A0=1), which the caller then takes exclusively.
bool portWrite(uint16_t address, uint8_t a8, uint8_t data);

// IN with A7=0: #1D maps the cart's hidden RAM in, #1F maps it out.
void portRead(uint8_t p8);

// .pss: the last ROM-bank select (bank | 0x80 for the cart) and its restore.
// snapRestore runs after MemESP::newSRAM / romLatch are set (it maps page 0).
uint8_t snapSelector();
void    snapRestore(uint8_t selector, uint8_t newBit);

// Bind/refresh the cart from Config::alfCartPath (at boot after Config::load and
// whenever a cartridge is loaded/unloaded). A missing SD file = empty drive
// (open bus, the system ROM runs), never a hang.
void bindCart();

// Lazy ALF cartridge loader: serves a .rom/.bin cart from the SD card on demand,
// the same way the WD1793 driver serves disk sectors. ALF cart ROM is only ever
// visible in Z80 page 0 (one 16K window at a time) and is rebound only on a #5F
// OUT, so a single 16K window faulted from SD is sufficient. The ALF system/cart
// code copies the data into the 128K RAM itself (exactly like a wd1793 program
// reads sectors into RAM); switching game/cartridge re-reads from SD. No flash
// write, no reboot.
namespace Cart {
    // Open the cart file on SD, allocate the 16K window and prefault bank 0 (the
    // catalog/front-end). Returns false on open / size / OOM failure.
    bool mount(const std::string& path);
    // Close the file and free the window.
    void unmount();
    // True when a lazy SD cart is currently mounted.
    bool active();
    // Cart size in 16K banks.
    int  bankCount();
    // Fault `bank` into the window (no-op when already resident) and return the
    // window pointer. Caller must ensure bank < bankCount().
    uint8_t* residentBank(int bank);
    // Path of the mounted cart (empty when none).
    const std::string& path();
}

}

// The 16K cart window buffer (nullptr when unmounted). Global so MemESP's inline
// writebyte() can drop guest writes to it: ALF page 0 is always ROM, but the window
// is heap SRAM (> 0x11000000) so the generic ROM-write drop would otherwise miss it.
extern uint8_t* g_alfWindow;
