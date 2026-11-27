// The PSRAM-only ROM overlay (.psramroms, rp2350-memmap.ld): the Scorpion GMX boot ROM,
// ProfROM, the TS-BIOS pages, SAM / Enterprise / ATM / Evo ROMs, the NeoGS firmware,
// esxDOS 0.8.9, the player code — everything that is reachable only with butter PSRAM
// (or, for ProfROM, is the owner's chosen place for it), collected at the top of the image.
//
// Until 2026-11-27 this range had a second, mutually exclusive occupant: on a board with
// no butter PSRAM a GM.DLS bank too big for the 1.6 MB flash partition above the firmware
// could be written over it ("traded"), and romsUsable() then steered the machines away.
// The flash bank and its partition are gone (owner, 2026-11-27: nobody kept a bank in
// flash, everybody loads it into PSRAM), so nothing writes here any more. The magic
// check stays: it is what a half-written image (an older firmware's bank write over a
// range this UF2 did not cover) would still be caught by.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace FlashRoms {

// Base and size of the overlay region itself.
const uint8_t* regionStart();
size_t         regionSize();

// True while the .psramrom_magic record is intact. Cheap (16-byte compare through XIP),
// needs no SD and no PSRAM, so it may be called as early as just after Config::load().
bool intact();

// True if the ROMs may be bound this boot (== intact()).
bool romsUsable();

// The overlay ROMs that also run WITHOUT butter PSRAM: Enterprise, ProfROM/ScorpEvo,
// esxDOS 0.8.9. On the 4 MB stock-W image (PSRAMROMS_ABSENT) they are linked into
// ordinary flash instead of the absent overlay (cmake/memory-layout.cmake), so they
// are always there; everywhere else they are in .psramroms (== intact()).
bool flashRomsUsable();

}  // namespace FlashRoms
