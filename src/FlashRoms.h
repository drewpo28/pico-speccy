// The PSRAM-only ROM overlay: two mutually exclusive occupants of the top of flash,
// arbitrated at runtime — exactly what .gsovl/.tsovl do in RAM, and what the GM.DLS
// bank partition has always done in flash with a single occupant.
//
// Occupant A (default): the Scorpion GMX boot ROM + the two TS-BIOS pages, ~370 KB,
// linked into .psramroms (rp2350-memmap.ld) directly below __gm_bank_start.
// Occupant B: the GM.DLS bank, when it does not fit the plain partition on a board
// that cannot reach those ROMs anyway — it then starts at __psramrom_start and
// swallows them plus the alignment gap, lifting a 4 MB board's flash bank ceiling
// from 1.62 MB to ~2.0-2.1 MB.
//
// There is NO setting for this and deliberately so: on a board with no butter (QSPI)
// PSRAM both machines are already absent from the menu — mach_scorpOpts() drops the
// GMX romset when butter_psram_size() is 0 and p_showTsconf() wants >= 1 MB of it,
// and SPI PSRAM on a MURM1 carrier qualifies for neither — so the "choice" was
// between giving away bytes nothing on that board can use and not giving them away.
// The one real cost is that plugging a PSRAM module into the same carrier afterwards
// finds those two machines gone until the next UF2, which is why the trade happens
// ONLY when a bank actually needs the room, never merely because it could.
//
// Profi/Karabas are deliberately NOT in the overlay: p_showProfi() accepts SPI PSRAM,
// so they stay useful on MURM1 with no QSPI chip.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace FlashRoms {

// Base and size of the overlay region itself (0 size when the ROMs were compiled
// out, e.g. GMX_IN_FLASH=0 — then there is nothing to trade).
const uint8_t* regionStart();
size_t         regionSize();

// True while the .psramrom_magic record is still intact, i.e. no bank has been
// written over the region. Cheap (16-byte compare through XIP), needs no SD and no
// PSRAM, so it may be called as early as just after Config::load().
bool intact();

// True if this board could ever trade: the overlay exists and its machines are
// unreachable here (no butter PSRAM). Says nothing about whether it WILL.
bool canTrade();

// The largest bank the FLASH can hold on this board — the extended window where a
// trade is possible, the plain partition otherwise. A capability, not a promise: the
// trade itself only happens when a bank that big is actually installed.
size_t bankCapacity();

// True if the bank window IS the extended one this boot. Decided once, lazily, the
// first time anyone asks — which must be AFTER Config::load() and the SD mount, and
// is (Config::requestMachine, from ESPectrum::setup).
bool extendedLive();

// True if the ROMs may be BOUND this boot: still present, and not about to be spent.
// Consulted at the TOP of Config::requestMachine(), because that is what binds ROM
// pointers and populates the pointer-keyed overlay registry, and provisionAtBoot()
// erases the region ~120 lines later in ESPectrum::setup().
bool romsUsable();

// The GM.DLS bank window for this boot: {start, size}.
const uint8_t* bankStart();
size_t         bankSize();

// The two windows as sizes, independent of which one is live.
size_t bankSizePlain();
size_t bankSizeExtended();

}  // namespace FlashRoms
