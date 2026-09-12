#include "FlashRoms.h"
#include "Config.h"
#include "MemESP.h"          // butter_psram_size()
#include "MidiSynth.h"       // selectedBankBytes() — what decides the trade
#include "Debug.h"
#include <string.h>

extern "C" uint8_t __psramrom_start[];
extern "C" uint8_t __psramrom_end[];
extern "C" uint8_t __gm_bank_start[];
extern "C" uint8_t __gm_bank_end[];

// The magic as the linker laid it down. Compared against flash through a volatile
// pointer, never against this array's initialiser — reading the symbol directly would
// let the compiler fold the comparison to "true" and the whole check would vanish.
static const unsigned char kMagic[16] = {
    'P','S','R','O','M','v','1', 0x00,
    0x5A, 0xA5, 0x3C, 0xC3, 0x0F, 0xF0, 0x69, 0x96
};

// -1 = not decided yet. Latched rather than recomputed: the answer reads the SD card
// and, more importantly, must not change under a running machine — Config::
// requestMachine() steers TS-Conf and GMX away from a window that is about to move,
// and provisionAtBoot() then writes into it.
static int s_extended = -1;

namespace FlashRoms {

const uint8_t* regionStart() { return (const uint8_t*)__psramrom_start; }
size_t         regionSize()  { return (size_t)(__psramrom_end - __psramrom_start); }

size_t bankSizePlain()    { return (size_t)(__gm_bank_end - __gm_bank_start); }
size_t bankSizeExtended() { return (size_t)(__gm_bank_end - __psramrom_start); }

bool intact() {
    if (regionSize() < sizeof(kMagic)) return false;   // nothing linked here
    const volatile uint8_t* p = (const volatile uint8_t*)__psramrom_start;
    for (size_t i = 0; i < sizeof(kMagic); i++)
        if (p[i] != kMagic[i]) return false;
    return true;
}

bool canTrade() {
    if (regionSize() < sizeof(kMagic)) return false;   // no ROMs to trade
    return butter_psram_size() == 0;                   // ...and they are unreachable
}

size_t bankCapacity() { return canTrade() ? bankSizeExtended() : bankSizePlain(); }

bool extendedLive() {
    if (regionSize() < sizeof(kMagic)) return false;
    // Already traded: the window IS the big one, whatever anyone would have preferred.
    // Reading the bank at __gm_bank_start would parse the middle of its own body as a
    // header, so this case is a fact and is checked before the decision.
    if (!intact()) return true;
    if (s_extended < 0) {
        // Spend the ROMs only when a bank actually needs the room — never merely
        // because this board cannot use them. That is what keeps a later PSRAM-module
        // swap harmless for everyone running an ordinary bank.
        if (!canTrade() || Config::midi != 4) { s_extended = 0; return false; }
        const size_t want = MidiSynth::selectedBankBytes();
        // 0 means "no bank visible yet" — an unmounted card, or a query that arrived
        // before the filesystem. Answer no WITHOUT latching, so the real decision is
        // still open when someone asks again with the card up; latching a guess here
        // would pin the window small and the big bank could never be placed.
        if (!want) return false;
        s_extended = (want > bankSizePlain()) ? 1 : 0;
        if (s_extended)
            Debug::log("[FlashRoms] bank %uKB > %uKB partition - trading the "
                       "GMX + TS-Conf ROM overlay for the room",
                       (unsigned)(want >> 10), (unsigned)(bankSizePlain() >> 10));
    }
    return s_extended > 0;
}

bool romsUsable() { return intact() && !extendedLive(); }

const uint8_t* bankStart() {
    return extendedLive() ? (const uint8_t*)__psramrom_start
                          : (const uint8_t*)__gm_bank_start;
}

size_t bankSize() { return (size_t)((const uint8_t*)__gm_bank_end - bankStart()); }

}  // namespace FlashRoms
