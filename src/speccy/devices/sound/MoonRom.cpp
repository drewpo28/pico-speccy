// MoonRom.cpp — where the ZXM-MoonSound YRW801 ROM comes from (see MoonRom.h).

#include "MoonRom.h"
#include "MoonPcm.h"
#include "app/Buffer.h"
#include "app/Debug.h"
#include "fatfs/ff.h"
#include "fs/FileUtils.h"
#include "app/TryAlloc.h"
#include "speccy/core/MemESP.h"     // PSRAM_DATA
#include <stdlib.h>
#include <string.h>

#if YRW801_BUILTIN
extern "C" const uint8_t gb_rom_yrw801[];   // src/speccy/roms/moonsound/yrw801.S
#endif

namespace MoonRom {

#if YRW801_BUILTIN
bool builtin() { return true; }
const uint8_t* get() { return gb_rom_yrw801; }
bool loadForCard(const char**) { return true; }
uint32_t psramBytes() { return 0; }
#else
static uint8_t* s_rom = nullptr;    // the card's copy, kept for the session
bool builtin() { return false; }
uint32_t psramBytes() { return s_rom ? MoonPcm::ROM_SIZE : 0; }
const uint8_t* get() { return s_rom; }
bool loadForCard(const char** why) {
    if (!s_rom) s_rom = loadFile(why);
    return s_rom != nullptr;
}
#endif

bool filePresent() {
    if (!FileUtils::fsMount) return false;
    FILINFO fi;
    return f_stat(PATH, &fi) == FR_OK && fi.fsize == MoonPcm::ROM_SIZE;
}

uint8_t* loadFile(const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    if (!filePresent()) { *why = "no /pico-speccy/roms/yrw801.rom"; return nullptr; }
    uint8_t* p = (uint8_t*)Buffer::palloc(MoonPcm::ROM_SIZE, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!p || (uintptr_t)p < (uintptr_t)PSRAM_DATA) {        // never 2 MB of heap
        if (p) Buffer::pfree(p);
        *why = "no 2 MB of PSRAM for its ROM";
        return nullptr;
    }
    // Through an SRAM bounce: the SD driver may DMA into the destination, and a DMA
    // write into the cached PSRAM alias leaves stale lines behind.
    uint8_t small[512];
    uint8_t* bounce = (uint8_t*)tryMalloc(4096);
    const UINT chunk = bounce ? 4096 : sizeof(small);
    if (!bounce) bounce = small;
    FIL f;
    bool ok = false;
    if (f_open(&f, PATH, FA_READ) == FR_OK) {
        ok = true;
        for (uint32_t off = 0; ok && off < MoonPcm::ROM_SIZE; off += chunk) {
            UINT got = 0;
            ok = f_read(&f, bounce, chunk, &got) == FR_OK && got == chunk;
            if (ok) memcpy(p + off, bounce, chunk);
        }
        f_close(&f);
    }
    if (bounce != small) free(bounce);
    if (!ok) {
        Buffer::pfree(p);
        *why = "ROM read error";
        return nullptr;
    }
    Debug::log("[MOON] YRW801 loaded from %s to %08X", PATH, (unsigned)(uintptr_t)p);
    return p;
}

} // namespace MoonRom
