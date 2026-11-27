#include "FlashRoms.h"

extern "C" uint8_t __psramrom_start[];
extern "C" uint8_t __psramrom_end[];

// The magic as the linker laid it down. Compared against flash through a volatile
// pointer, never against this array's initialiser — reading the symbol directly would
// let the compiler fold the comparison to "true" and the whole check would vanish.
static const unsigned char kMagic[16] = {
    'P','S','R','O','M','v','1', 0x00,
    0x5A, 0xA5, 0x3C, 0xC3, 0x0F, 0xF0, 0x69, 0x96
};

namespace FlashRoms {

const uint8_t* regionStart() { return (const uint8_t*)__psramrom_start; }
size_t         regionSize()  { return (size_t)(__psramrom_end - __psramrom_start); }

bool intact() {
#if HOST_BUILD
    return true;   // a PC: these ROMs are ordinary data in the executable
#endif
#if PSRAMROMS_ABSENT
    // 4 MB stock-W image (Pico 2 W): .psramroms is not in the UF2 at all
    // (cmake/memory-layout.cmake), so this board is "traded" from the first boot.
    // Never trust bytes at __psramrom_start: an older image may have left a magic there.
    return false;
#endif
    if (regionSize() < sizeof(kMagic)) return false;   // nothing linked here
    const volatile uint8_t* p = (const volatile uint8_t*)__psramrom_start;
    for (size_t i = 0; i < sizeof(kMagic); i++)
        if (p[i] != kMagic[i]) return false;
    return true;
}

bool romsUsable() { return intact(); }

}  // namespace FlashRoms
