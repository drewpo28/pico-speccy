#ifndef ELM_ROMS_H
#define ELM_ROMS_H
// The eLeMeNt ZX's own firmware pieces, embedded so a fresh flash image comes with the
// SetUp (block 7) and the MB03+SD esxDOS (block 4) already installed — the two flasher
// packages (FlashSetUp_V2.tap, FLASHESX) are then only needed for updates. Every image
// with the eLeMeNt (4 MB too since 2026-11-27, when the GM.DLS flash floor went away).
#include <stdint.h>
#if defined(HOST_BUILD) || !defined(ELEMENT_ZX) || ELEMENT_ZX
#define ELM_ROMS_BUILTIN 1
#else
#define ELM_ROMS_BUILTIN 0
#endif
#if ELM_ROMS_BUILTIN
#ifdef __cplusplus
extern "C" {
#endif
extern const unsigned char gb_rom_elm_setup[15695];   // SetUp 20260406
extern const unsigned char gb_rom_esxdos_elm[8192];   // esxDOS MB03+SD (ESX-SD.ROM)
#ifdef __cplusplus
}
#endif
#endif
#endif // ELM_ROMS_H
