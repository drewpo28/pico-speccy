// First 16 bytes of the .psramroms flash overlay — see rp2350-memmap.ld and
// src/FlashRoms.h. A GM.DLS bank too big for the plain partition is written starting
// at __psramrom_start, so these bytes are the FIRST thing overwritten:
// reading them back is the whole "are the PSRAM-only ROMs still there" test, and
// it needs neither SD nor PSRAM, which is why it can run right after Config::load().
//
// Not const-folded away and not gc-able: the linker KEEPs .psramrom_magic, and
// FlashRoms reads it through a volatile pointer rather than this symbol, so the
// compiler can never answer the comparison from the initialiser.
#include <stdint.h>

__attribute__((section(".psramrom_magic"), aligned(4), used))
const unsigned char gb_psramrom_magic[16] = {
    'P','S','R','O','M','v','1', 0x00,
    0x5A, 0xA5, 0x3C, 0xC3, 0x0F, 0xF0, 0x69, 0x96
};
