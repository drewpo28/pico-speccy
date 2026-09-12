#include "MidiSynth.h"


#include <string.h>
#include <stdint.h>
#include "pico.h"
#include "hardware/flash.h"      // flash_range_erase/program, FLASH_SECTOR_SIZE
#include "hardware/sync.h"       // save_and_disable_interrupts
#include "hardware/gpio.h"       // LED blink during the flash write
#include "hardware/xip_cache.h"  // xip_cache_invalidate_all (safe: single-core boot)
#include "hardware/watchdog.h"   // watchdog scratch reg carries the "force reflash" request across reboot
#include "hardware/clocks.h"     // clock_get_hz(clk_sys) — capture/restore boot clock around the flash write
#include "pico/stdlib.h"         // set_sys_clock_khz — flash the bank at a conservative 252 MHz

// Force-reflash request, carried through the warm reset in a free watchdog scratch
// register (SDK uses scratch[4..7]; [0..3] are free). No flash write at runtime —
// just set this and reboot; provisionAtBoot() consumes it.
#define MIDI_REFLASH_SCRATCH 2
#define MIDI_REFLASH_MAGIC   0x4D494449u  /* 'MIDI' */
#include "hardware/regs/addressmap.h"  // XIP_BASE
#include "midi_wt.h"             // C wrapper over external/embeded-midi-synth
#include "gm_bank.h"             // gm_bank_view, gm_bank_header_t, GM_BANK_VERSION
#include "FileUtils.h"           // fopen2/fclose2, FIL, f_read, f_size
#include "Config.h"              // Config::midi
#include "Buffer.h"              // tiered allocator: butter PSRAM, else flash partition
#include "MemESP.h"              // butter_psram_size()
#include "Debug.h"
#include <string>
#include <vector>

// The bank lives wherever Buffer places it, and there is nothing to configure: butter
// PSRAM when the board has it (reloaded from SD each boot, so a bank swap applies
// live), else the flash partition at the top of flash (a NOLOAD region in
// rp2350-memmap.ld — not in the UF2; provisioned once from SD, survives reflashes and
// a missing card). That is what PREFER_PSRAM | ALLOW_FLASH already means, so the ask
// is one constant. The flash WINDOW is a runtime value — on a board with no butter
// PSRAM it grows over the ROM overlay when a bank needs the room, see FlashRoms.h —
// which costs the engine nothing: both homes are XIP-addressable and the bank format
// is position-independent.
#include "FlashRoms.h"

// When the bank lives in PSRAM this holds the allocation; empty when it lives in
// flash (then bankBase() returns the XIP partition pointer directly).
static Buffer g_bankBuf;

// The audio bus is unsigned 0..255 (silence = 0), summed with beeper/AY/SAA then
// scaled by volume in pwm_audio_write(); the main mixer/driver needs no change.
//
// FIXED-CENTER + SOFT-SATURATION + ACTIVITY-GATE.
//
// Lessons from the earlier tries: a signed value half-wave-clips against the
// bus's 0 floor; a fixed 128 center is clean but its always-on DC steals headroom
// and clicks against AY/beeper; an envelope-following bias removes the idle DC
// but its *moving* bias bends the waveform (distortion), and pushing gain just
// flat-tops the 255 ceiling. This approach avoids all three:
//
//   ac = softsat127( sample * GAIN >> 16 )   // symmetric, |ac| <= 127 (no bus clip)
//   v  = (128 + ac) * gate >> 8              // gate 0..256 follows note activity
//
//  * Fixed 128 center -> the wave is only DC-shifted, never bent (no distortion).
//  * Loudness comes from gently driving the AC into a SOFT saturator (musical,
//    symmetric) instead of amplifying into the hard 255 wall. Because |ac|<=127,
//    v = 128+ac stays in [1,255] and the bus never clips.
//  * The gate makes silence -> 0 (no idle DC to steal headroom / click against
//    other sources), so the main mixer/driver still needs no change.
//
// MIDI_OUT_GAIN256 = how hard we hit the saturator = loudness (256 light,
// 768 loud/AY-ish, higher = louder + more saturated "drive").
#define MIDI_OUT_GAIN256 768

static int32_t g_gate = 0;   // smoothed note-activity gate, 0..256

// Symmetric soft saturation to +/-127: linear below 80, asymptotic above.
static inline int32_t softsat127(int32_t x) {
    int32_t s = x >> 31;                       // sign mask (0 or -1)
    int32_t a = (x ^ s) - s;                   // |x|
    if (a > 80) a = 80 + ((a - 80) * 47) / ((a - 80) + 47);   // -> 127 asymptote
    return (a ^ s) - s;                        // restore sign
}

// Candidate locations for the user-supplied packed bank (never shipped by us).
static const char *kBankPaths[] = {
    CONFIG_DIR "/gm_bank.bin",
    "/gm_bank.bin",
};

// ── statics ──────────────────────────────────────────────────────────────────
uint8_t MidiSynth::midi_status   = 0;
uint8_t MidiSynth::midi_data[2]  = {0, 0};
uint8_t MidiSynth::midi_data_pos = 0;
uint8_t MidiSynth::midi_expected = 0;
bool    MidiSynth::bank_ready    = false;

static uint8_t dataLenForStatus(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0:                                  return 1;
        default:                                               return 0;
    }
}

// GM System On (F0 7E 7F 09 01 F7): players send it to return the synth to the
// GM power-on state (bend range ±2, controllers, programs, sustain). The engine
// itself never sees SysEx, so it is recognized here in the byte parser; every
// other SysEx is still dropped. gm_match counts pattern bytes matched so far.
static const uint8_t GM_ON[6] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
static uint8_t gm_match = 0;

// The flash window for THIS boot. Runtime, not a linker constant: extended storage
// moves the base down over the ROM overlay. Everything else in this file — the pool
// registration, the bind, the size cap, the picker's gate — goes through these two.
static inline const uint8_t* bankFlashPtr()   { return FlashRoms::bankStart(); }
static inline size_t         bankRegionSize() { return FlashRoms::bankSize(); }

// The base the engine is bound to: the PSRAM copy when present, else the flash
// partition (XIP). Both are directly addressable; the format is position-independent.
static inline const uint8_t* bankBase() {
    return g_bankBuf.ok() ? (const uint8_t*)g_bankBuf.data() : bankFlashPtr();
}

size_t MidiSynth::bankPsramBytes() {
    if (!g_bankBuf.ok()) return 0;
    Buffer::Tier t = g_bankBuf.tier();
    return (t == Buffer::TIER_BUTTER || t == Buffer::TIER_SPI) ? g_bankBuf.size() : 0;
}

// Where the bank the engine is bound to actually sits. Not the same thing as
// what was asked for — PSRAM is only preferred, and falls back to flash when the arena
// cannot hold the bank — so the Memory Info screen reports THIS.
const char* MidiSynth::bankLocation() {
    // "flash+" is the EXTENDED window: the same partition grown down over the traded
    // ROM overlay. Worth distinguishing here, because it is the one state in which two
    // machines are missing from the Machine menu and this screen is where someone goes
    // looking for why.
    const char* flash = FlashRoms::extendedLive() ? "flash+" : "flash";
    if (!bank_ready) return "no bank";
    if (!g_bankBuf.ok()) return flash;          // bound straight to the XIP partition
    switch (g_bankBuf.tier()) {
        case Buffer::TIER_BUTTER: return "PSRAM";
        case Buffer::TIER_FLASH:  return flash;
        default:                  return g_bankBuf.tierName();
    }
}

// The bank's home, and it asks for no decision: butter PSRAM if there is any, the
// flash partition otherwise. Buffer's own ladder is exactly that rule.
static inline uint32_t bankAllocFlags() {
    return Buffer::NEED_POINTER | Buffer::PREFER_PSRAM | Buffer::ALLOW_FLASH;
}

// Defined below; needed by bindFromPsram() above its definition.
static FIL* openValidSdBank(size_t* outSize, gm_bank_header_t* outHdr);

// Bind the engine to the bank already resident in the flash partition. No SD, no
// write → safe anytime. Returns true if a valid v5 bank is present in flash.
bool MidiSynth::bindFromFlash() {
    gm_bank_view_t v;
    if (!gm_bank_view(bankFlashPtr(), &v)) return false;   // empty / invalid / old version
    midi_wt_bind(bankFlashPtr());
    bank_ready = true;
    return true;
}

// Buffer::LoadReader over the open SD bank file: fill `dst` with `n` bytes at `off`.
static bool bankFileReader(void* ctx, void* dst, uint32_t off, uint32_t n) {
    FIL* f = (FIL*)ctx;
    UINT br = 0;
    if (f_lseek(f, off) != FR_OK) return false;
    return f_read(f, dst, n, &br) == FR_OK && br == n;
}

// Load the SD bank via Buffer, which places it (PSRAM preferred, else the flash
// partition) and writes it accordingly — MidiSynth does not branch on memory type. mayWriteFlash=false forbids a flash erase (post-VIDEO::Init);
// a PSRAM load is always allowed, so PSRAM storage never needs a reboot. Returns true
// once bound.
bool MidiSynth::loadBank(bool force, bool mayWriteFlash) {
    size_t size; gm_bank_header_t hdr;
    FIL* f = openValidSdBank(&size, &hdr);
    if (!f) return false;                                  // no usable SD bank
    g_bankBuf.free();
    if (!g_bankBuf.alloc(size, bankAllocFlags())) {
        fclose2(f);
        Debug::log("MidiSynth: bank alloc failed (%uKB)", (unsigned)(size >> 10));
        return false;
    }
    bool ok = g_bankBuf.load((uint32_t)size, force, bankFileReader, f, mayWriteFlash);
    fclose2(f);
    gm_bank_view_t v;
    if (!ok || !gm_bank_view(g_bankBuf.data(), &v)) { g_bankBuf.free(); return false; }
    midi_wt_bind(g_bankBuf.data());
    bank_ready = true;
    Debug::log("MidiSynth: bank ready (%s, %uKB)", g_bankBuf.tierName(), (unsigned)(size >> 10));
    return true;
}

void MidiSynth::init() {
    midi_status = midi_data_pos = midi_expected = 0;
    if (bank_ready) return;   // idempotent (already bound at boot by provisionAtBoot)
    // Runtime / post-VIDEO::Init: a PSRAM load is safe (no reboot); a flash *write* is
    // not (mayWriteFlash=false). If neither applies, bind any already-persisted flash bank.
    if (loadBank(/*force=*/false, /*mayWriteFlash=*/false)) return;
    bindFromFlash();
}

// Open one candidate path and validate its GMWB v5 header. Returns the open FIL*
// (caller fcloses) + size/header on success, or nullptr.
static FIL* tryOpenBank(const char* path, size_t* outSize, gm_bank_header_t* outHdr) {
    FIL* f = fopen2(path, FA_READ);
    if (!f) return nullptr;
    size_t size = (size_t)f_size(f);
    UINT br = 0;
    if (size >= sizeof(*outHdr) && size <= MidiSynth::maxBankBytes() &&
        f_read(f, outHdr, sizeof(*outHdr), &br) == FR_OK && br == sizeof(*outHdr) &&
        outHdr->magic[0] == 'G' && outHdr->magic[1] == 'M' &&
        outHdr->magic[2] == 'W' && outHdr->magic[3] == 'B' &&
        outHdr->version == GM_BANK_VERSION) {
        *outSize = size;
        return f;
    }
    fclose2(f);
    return nullptr;
}

// Open the GM wavetable bank on SD and validate its header. Prefers the user-chosen
// bank (Config::midi_bank, set by the OSD picker) and falls back to the default
// gm_bank.bin locations. Returns the open FIL* (caller fcloses) + size/header.
static FIL* openValidSdBank(size_t* outSize, gm_bank_header_t* outHdr) {
    if (!Config::midi_bank.empty()) {
        FIL* f = tryOpenBank(Config::midi_bank.c_str(), outSize, outHdr);
        if (f) return f;                        // chosen bank still present & valid
    }
    for (size_t i = 0; i < sizeof(kBankPaths) / sizeof(kBankPaths[0]); i++) {
        FIL* f = tryOpenBank(kBankPaths[i], outSize, outHdr);
        if (f) return f;
    }
    return nullptr;
}

// Header-valid size of one candidate, IGNORING any placement cap — the question
// "how big is the selected bank" must not depend on where it could go.
static size_t bankFileBytes(const char* path) {
    FIL* f = fopen2(path, FA_READ);
    if (!f) return 0;
    gm_bank_header_t h;
    UINT br = 0;
    const size_t size = (size_t)f_size(f);
    const bool ok = size >= sizeof(h) &&
                    f_read(f, &h, sizeof(h), &br) == FR_OK && br == sizeof(h) &&
                    h.magic[0] == 'G' && h.magic[1] == 'M' &&
                    h.magic[2] == 'W' && h.magic[3] == 'B' &&
                    h.version == GM_BANK_VERSION;
    fclose2(f);
    return ok ? size : 0;
}

size_t MidiSynth::selectedBankBytes() {
    if (!Config::midi_bank.empty()) {
        const size_t n = bankFileBytes(Config::midi_bank.c_str());
        if (n) return n;
    }
    for (size_t i = 0; i < sizeof(kBankPaths) / sizeof(kBankPaths[0]); i++) {
        const size_t n = bankFileBytes(kBankPaths[i]);
        if (n) return n;
    }
    return 0;
}

// What a bank may weigh here. The flash partition (1.6875 MB on a GMX build) is the
// FLOOR, not the limit: with PSRAM storage the bank is copied into the butter arena
// and never touches flash, so on a butter board the arena is the real ceiling. Gating
// on the partition alone made every bank above it invisible in the picker and made the
// on-device converter delete its own output (DLSbyXG.dls -> ~2.0 MB). Flash storage
// This is a CAPABILITY figure: the arena's total, not its current free space — a live
// apply may still fail on occupancy, and then the reboot path (provisionAtBoot, empty
// arena) places it. The flash half is FlashRoms::bankCapacity(), i.e. the EXTENDED
// window wherever a trade is possible: a bank has to be selectable before it can be
// the reason the window grows (the picker's gate and the converter both read this).
size_t MidiSynth::maxBankBytes() {
    const size_t flash = FlashRoms::bankCapacity();
    const size_t psram = Buffer::butterArenaBytes();
    return psram > flash ? psram : flash;
}

// True if there is a valid gm_bank.bin on SD that is NOT already identical in the
// flash region (flash empty/invalid, or a different bank) → a boot-time write is
// needed. Cheap (reads only the 44-byte headers).
bool MidiSynth::needsProvision() {
    size_t size; gm_bank_header_t sdh;
    FIL* f = openValidSdBank(&size, &sdh);
    if (!f) return false;                       // no usable SD bank → nothing to install
    fclose2(f);
    gm_bank_view_t fv;
    if (gm_bank_view(bankFlashPtr(), &fv) && memcmp(fv.header, &sdh, sizeof(sdh)) == 0)
        return false;                           // flash already holds this exact bank
    return true;
}

// EARLY-BOOT bank setup. MUST be called from setup() AFTER Buffer::initPools() (the
// butter arena must exist) and BEFORE VIDEO::Init() — still single core (core1 is
// launched later in main()), and a flash write needs XIP free of the live HDMI DMA.
// Buffer decides the placement: QSPI/butter PSRAM if present (copy from SD, no flash
// write, no reboot), else the flash partition (slow, LED blinks, commit-last,
// retry-safe). The "reinstall" watchdog magic forces a flash rewrite.
void MidiSynth::provisionAtBoot() {
    // Always register the flash partition (even when GM.DLS is off) so runtime apply
    // decisions (applyBankLive) can compare against / write the flash tier.
    Buffer::initFlashPool((uint8_t*)bankFlashPtr(), bankRegionSize());
    // Which window this boot uses, and whether the ROM overlay is still standing. The
    // one line that separates "my bank is too big" from "my Scorpion GMX vanished" in
    // a log, and both questions arrive without the user knowing they are the same one.
    Debug::log("MidiSynth: bank window @%p %uKB (overlay %s)",
               (const void*)bankFlashPtr(), (unsigned)(bankRegionSize() >> 10),
               FlashRoms::regionSize() < 16 ? "absent"
                   : (FlashRoms::intact() ? (FlashRoms::extendedLive() ? "spending" : "intact")
                                          : "traded"));
    if (Config::midi != 4) return;              // only GM.DLS mode provisions a bank

    bool force = (watchdog_hw->scratch[MIDI_REFLASH_SCRATCH] == MIDI_REFLASH_MAGIC);
    // Consume the request only when there is a volume to satisfy it from: this
    // function now runs on a card-less boot too (for initFlashPool above and the
    // bindFromFlash fallback below), and eating the flag there would silently
    // drop a reflash the user asked for.
    if (force && FileUtils::fsMount) watchdog_hw->scratch[MIDI_REFLASH_SCRATCH] = 0;
    Debug::log("MidiSynth: provisionAtBoot enter @%u ms (force=%d)",
                  (unsigned)to_ms_since_boot(get_absolute_time()), (int)force);

    // Pre-video, single core → a flash write is permitted here.
    if (loadBank(force, /*mayWriteFlash=*/true)) return;

    // No usable SD bank → bind whatever is already persisted in the flash partition.
    bindFromFlash();
}

// alfCartProvisionAtBoot() removed: ALF cartridges are no longer flashed into the
// shared region — they are served lazily from SD on demand (see src/AlfCart.*).
// The shared flash region is now GM.DLS-only.

// Scan the SD for selectable GM wavetable banks: any *.bin in CONFIG_DIR or the
// card root that carries a valid GMWB v5 header. Fills `paths` (full path, used as
// Config::midi_bank) and `names` (basename, shown in the OSD picker), index-aligned.
// Bounded (heap-light; RP2350-only path). Returns the count found.
size_t MidiSynth::scanBanks(std::vector<std::string>& paths,
                            std::vector<std::string>& names) {
    paths.clear();
    names.clear();
    static const char* kScanDirs[] = { CONFIG_DIR, "/" };
    const size_t kMaxBanks = 24;
    for (size_t d = 0; d < sizeof(kScanDirs) / sizeof(kScanDirs[0]); d++) {
        DIR dir;
        if (f_opendir(&dir, kScanDirs[d]) != FR_OK) continue;
        FILINFO fno;
        while (paths.size() < kMaxBanks &&
               f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
            if (fno.fattrib & AM_DIR) continue;
            const char* nm = fno.fname;
            size_t len = strlen(nm);
            if (len < 4 || strcasecmp(nm + len - 4, ".bin") != 0) continue;
            bool isRoot = (kScanDirs[d][0] == '/' && kScanDirs[d][1] == '\0');
            std::string full = isRoot ? (std::string("/") + nm)
                                      : (std::string(kScanDirs[d]) + "/" + nm);
            bool dup = false;                       // same path already listed
            for (auto& p : paths) if (p == full) { dup = true; break; }
            if (dup) continue;
            size_t size; gm_bank_header_t hdr;
            FIL* f = tryOpenBank(full.c_str(), &size, &hdr);
            if (!f) {
                // A valid bank rejected only by the size cap used to vanish here with
                // no trace at all ("the picker does not see my .bin"). Say so.
                const size_t vs = bankFileBytes(full.c_str());
                if (vs > MidiSynth::maxBankBytes())
                    Debug::log("MidiSynth: bank %s skipped (%uKB > %uKB max)",
                               nm, (unsigned)(vs >> 10),
                               (unsigned)(MidiSynth::maxBankBytes() >> 10));
                continue;
            }
            fclose2(f);
            paths.push_back(full);
            names.push_back(nm);
        }
        f_closedir(&dir);
    }
    return paths.size();
}

// True if a valid gm_bank.bin is present on SD (used to gate the "reinstall" offer
// so we never wipe a working flash bank when there is nothing to restore it from).
bool MidiSynth::sdBankAvailable() {
    size_t size; gm_bank_header_t sdh;
    FIL* f = openValidSdBank(&size, &sdh);
    if (!f) return false;
    fclose2(f);
    return true;
}

// The window this boot is using, which is NOT always the plain partition — callers
// that mean "what does the fixed partition hold" want FlashRoms::bankSizePlain().
size_t MidiSynth::flashBankCapacity() { return bankRegionSize(); }

// Force a re-provision on the NEXT boot. NO flash op here (that needs core1
// locked out vs the HDMI ISR → froze): just drop a magic in a watchdog scratch
// register that survives the warm reset. provisionAtBoot() sees it and rewrites
// the bank from SD even if the header looks current (recovers a broken body).
// Caller reboots (esp_hard_reset → watchdog reset → scratch preserved).
void MidiSynth::requestReflash() {
    watchdog_hw->scratch[MIDI_REFLASH_SCRATCH] = MIDI_REFLASH_MAGIC;
    bank_ready = false;
}

bool MidiSynth::applyBankLive() {
    size_t size; gm_bank_header_t h;
    FIL* f = openValidSdBank(&size, &h);
    if (!f) return bank_ready;            // no SD bank → keep whatever is bound
    fclose2(f);
    bank_ready = false;                   // tear down current; loadBank rebinds on success
    midi_wt_unbind();
    g_bankBuf.free();
    // No flash write here: lands in PSRAM (live), or a flash bank that is already
    // current (skip). Returns false only if a flash write is needed → caller reboots.
    return loadBank(/*force=*/false, /*mayWriteFlash=*/false);
}

void MidiSynth::deinit() {
    bank_ready = false;       // stop using the bank
    midi_status = midi_data_pos = midi_expected = 0;
    g_gate = 0;
    midi_wt_unbind();         // release the ~5 KB voice array (lazy; back to .bss-free)
    g_bankBuf.free();         // release the PSRAM copy (no-op if the bank was in flash)
}

void MidiSynth::reset() {
    midi_status = midi_data_pos = midi_expected = 0;
    g_gate = 0;
    if (bank_ready) midi_wt_bind(bankBase());   // re-bind = all-notes-off (PSRAM or flash)
}

void MidiSynth::feedByte(uint8_t b) {
    if (b & 0x80) {                  // status byte
        if (b >= 0xF8) return;       // realtime — ignore
        if (b >= 0xF0) {             // SysEx / system common — reset parser
            midi_status = midi_data_pos = midi_expected = 0;
            if (b == 0xF7 && gm_match == 5) reset();  // GM System On completed
            gm_match = (b == 0xF0);  // F0 starts a fresh match, anything else aborts
            return;
        }
        gm_match = 0;                // a channel status aborts any open SysEx
        midi_status   = b;
        midi_data_pos = 0;
        midi_expected = dataLenForStatus(b);
    } else {                         // data byte
        if (midi_expected == 0) {    // inside a SysEx (or stray): match GM On, drop the rest
            gm_match = (gm_match && gm_match < 5 && b == GM_ON[gm_match]) ? gm_match + 1 : 0;
            return;
        }
        midi_data[midi_data_pos++] = b;
        if (midi_data_pos >= midi_expected) {
            processMessage(midi_status, midi_data[0],
                           midi_expected > 1 ? midi_data[1] : 0);
            midi_data_pos = 0;       // running status
        }
    }
}

void MidiSynth::processMessage(uint8_t status, uint8_t d0, uint8_t d1) {
    if (!bank_ready) return;
    // The engine handles note on/off (vel 0 = off), CC, program change, pitch
    // bend and channel-9 percussion internally.
    midi_wt_message(status, d0, d1);
}

void __not_in_flash("midi") MidiSynth::gen_sound(uint8_t *buf_L, uint8_t *buf_R, int count) {
    if (!bank_ready) {                          // no bank: contribute nothing (bus silence = 0)
        memset(buf_L, 0, count);
        memset(buf_R, 0, count);
        g_gate = 0;
        return;
    }
    int32_t target = midi_wt_active() ? 256 : 0;   // open while any voice sounds
    for (int i = 0; i < count; i++) {
        if (g_gate < target) g_gate++;             // ~8 ms ramp in/out (no click)
        else if (g_gate > target) g_gate--;
        int16_t l, r;
        midi_wt_render(&l, &r);
        int32_t acl = softsat127(((int32_t)l * MIDI_OUT_GAIN256) >> 16);
        int32_t acr = softsat127(((int32_t)r * MIDI_OUT_GAIN256) >> 16);
        int32_t vl = ((128 + acl) * g_gate) >> 8;  // fixed center, gated; always in 0..255
        int32_t vr = ((128 + acr) * g_gate) >> 8;
        buf_L[i] = (uint8_t)(vl < 0 ? 0 : (vl > 255 ? 255 : vl));
        buf_R[i] = (uint8_t)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
    }
}

