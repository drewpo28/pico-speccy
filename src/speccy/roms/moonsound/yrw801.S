// The Yamaha YRW801-M GM sample ROM of the ZXM-MoonSound card (2 MB, CRC32 2A9D8D43,
// the same image MAME ships as yrw801.rom). Embedded only in the 16 MB firmware
// (YRW801_BUILTIN=1, cmake/board.cmake): the 4 MB image loads it from SD (MoonRom.h). MoonPcm reads it
// straight from flash through XIP; only the card's 1 MB SRAM goes to PSRAM.
#if YRW801_BUILTIN
    .section .rodata.yrw801, "a"
    .balign 4
    .global gb_rom_yrw801
    .type   gb_rom_yrw801, %object
gb_rom_yrw801:
    .incbin YRW801_ROM_PATH
    .size   gb_rom_yrw801, . - gb_rom_yrw801
#endif
