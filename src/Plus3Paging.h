// ZX Spectrum +2A/+3 memory map — the arithmetic, with no firmware dependencies.
//
// It lives in its own header so tools/plus3_paging_test.cpp builds against the SHIPPED
// table rather than a copy of it (same reasoning as drivers/hdmi/tmds_pair.h). The only
// consumer in the firmware is MemESP::plus3Remap().
//
// Reference: Fuse machines/specplus3.c (special_memory_map / select_special_map /
// specplus3_memory_map) and specplus3_plus2a_common_reset for the contention rule.
//
//   #7FFD  D0-2 RAM bank at 0xC000, D3 screen (RAM 5 / RAM 7),
//          D4 ROM select LOW bit, D5 paging lock
//   #1FFD  D0 special (all-RAM) paging enable,
//          D1-2 which configuration when D0=1 / D2 ROM select HIGH bit when D0=0,
//          D3 disk motor (both drives), D4 printer strobe

#pragma once

#include <stdint.h>

// The four all-RAM configurations selected by #1FFD D1-2 when D0 is set.
static const uint8_t kPlus3SpecialCfg[4][4] = {
    { 0, 1, 2, 3 },
    { 4, 5, 6, 7 },
    { 4, 5, 6, 3 },
    { 4, 7, 6, 3 },
};

// True while an all-RAM configuration is mapped (no ROM anywhere in the address space).
static inline bool plus3IsSpecial(uint8_t p1ffd) { return (p1ffd & 0x01) != 0; }

// The four RAM pages of the active all-RAM configuration. Only valid when
// plus3IsSpecial(p1ffd).
static inline const uint8_t* plus3SpecialPages(uint8_t p1ffd) {
    return kPlus3SpecialCfg[(p1ffd >> 1) & 0x03];
}

// ROM bank at 0x0000 in normal mode: 0 editor/menu, 1 syntax, 2 +3DOS, 3 48 BASIC.
// D2 of #1FFD is the ROM high bit ONLY while D0 is clear — with D0 set the same bit is
// part of the configuration number.
static inline uint8_t plus3RomIndex(uint8_t p1ffd, uint8_t romLatch) {
    return (uint8_t)(((p1ffd & 0x04) >> 1) | (romLatch & 0x01));
}

// Contended RAM pages on a +2A/+3 are 4,5,6,7 — NOT the odd ones as on a 128K.
static inline bool plus3PageContended(uint8_t page) { return page >= 4; }
