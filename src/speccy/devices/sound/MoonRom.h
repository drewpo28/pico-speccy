#ifndef MoonRom_h
#define MoonRom_h
// The YRW801 sample ROM of ZXM-MoonSound (2 MB, CRC32 2A9D8D43).
// 16 MB firmware: embedded in flash (YRW801_BUILTIN, src/speccy/roms/moonsound/yrw801.S).
// 4 MB firmware: there is no room for it, so it is read from the SD card
// (MoonRom::PATH) into a 2 MB block of butter PSRAM when the card is switched on.

#include <stdint.h>

namespace MoonRom {

constexpr const char* PATH = "/pico-speccy/roms/yrw801.rom";

// True when no SD file is needed (the ROM is in flash).
bool builtin();

// The ROM the emulated card runs on: the flash image, or the one load() put in
// PSRAM; nullptr before that / when there is none.
const uint8_t* get();

// The SD file is there and is exactly 2 MB.
bool filePresent();

// Read PATH into a new PSRAM block (Buffer::palloc, caller frees with
// Buffer::pfree). nullptr = no file, wrong size, no PSRAM or a read error;
// *why (if given) says which.
uint8_t* loadFile(const char** why = nullptr);

// PSRAM held by the card's ROM copy (0 when built in or not loaded).
uint32_t psramBytes();

// ESPectrum::setup: load() once for the session and keep it as get().
// No-op on the 16 MB firmware. Returns get() != nullptr.
bool loadForCard(const char** why = nullptr);

} // namespace MoonRom

#endif
