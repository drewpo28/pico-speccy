// The built-in General MIDI wavetable bank of the 16 MB firmware: gm.dls converted to
// the GMWB v5 format (1 668 026 B). Embedded only when GM_BANK_BUILTIN=1
// (cmake/board.cmake, the 16 MB images); the engine plays it straight from flash
// through XIP. A bank picked from the SD card still loads into PSRAM as before.
#if GM_BANK_BUILTIN
    .section .rodata.gmbank, "a"
    .balign 4
    .global gb_gm_bank_builtin
    .type   gb_gm_bank_builtin, %object
gb_gm_bank_builtin:
    .incbin GM_BANK_PATH
    .size   gb_gm_bank_builtin, . - gb_gm_bank_builtin
#endif
