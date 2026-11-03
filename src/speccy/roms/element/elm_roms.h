#ifndef ELM_ROMS_H
#define ELM_ROMS_H
// The eLeMeNt ZX's own firmware pieces, embedded so a fresh flash image comes with the
// SetUp (block 7) and the MB03+SD esxDOS (block 4) already installed — the two flasher
// packages (FlashSetUp_V2.tap, FLASHESX) are then only needed for updates. 16 MB images
// only: the 4 MB ones sit at the GM.DLS floor (memory-sram-flash.md).
#include <stdint.h>
#if defined(HOST_BUILD) || (defined(PICO_FLASH_SIZE_BYTES) && PICO_FLASH_SIZE_BYTES >= (16u * 1024 * 1024))
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
