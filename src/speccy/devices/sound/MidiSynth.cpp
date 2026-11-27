#include "MidiSynth.h"


#include <string.h>
#include <stdint.h>
#include "pico.h"
#include "pico/stdlib.h"         // to_ms_since_boot
#include "midi_wt.h"             // C wrapper over external/embeded-midi-synth
#include "gm_bank.h"             // gm_bank_view, gm_bank_header_t, GM_BANK_VERSION
#include "fs/FileUtils.h"           // fopen2/fclose2, FIL, f_read, f_size
#include "app/Config.h"              // Config::midi
#include "app/Buffer.h"              // the butter PSRAM arena the bank is loaded into
#include "speccy/core/MemESP.h"              // butter_psram_size()
#include "app/Debug.h"
#include "ui/OSDMain.h"        // bootNotice — a refused bank must be visible without a UART
#include <string>
#include <vector>

// The bank's homes: butter PSRAM, loaded from SD (each boot, and live on a pick), or on
// a 16 MB image the bank built into the firmware (played straight from XIP). The flash
// partition a bank used to be WRITTEN into on boards without butter PSRAM (1.6 MB above
// the firmware, provisioned at early boot) is gone since 2026-11-27 (owner: nobody kept
// a bank in flash; the reserve is now firmware room on every 4 MB image).
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

#if GM_BANK_BUILTIN
extern "C" const uint8_t gb_gm_bank_builtin[];
#endif
// A 16 MB image treats an empty pick (and a pick that is gone from the card) as the
// built-in bank; only an explicit SD path loads from the card.
static inline bool builtinPicked() {
#if GM_BANK_BUILTIN
    return Config::midi_bank.empty();
#else
    return false;
#endif
}

static inline const uint8_t* bankBase() {
#if GM_BANK_BUILTIN
    if (!g_bankBuf.ok()) return gb_gm_bank_builtin;
#endif
    return (const uint8_t*)g_bankBuf.data();
}

size_t MidiSynth::bankPsramBytes() {
    return g_bankBuf.ok() && g_bankBuf.tier() == Buffer::TIER_BUTTER ? g_bankBuf.size() : 0;
}

// Where the bank the engine is bound to sits — for the Memory Info MIDI line.
const char* MidiSynth::bankLocation() {
    if (!bank_ready) return "no bank";
    if (!g_bankBuf.ok()) return "built-in";
    return g_bankBuf.tier() == Buffer::TIER_BUTTER ? "PSRAM" : g_bankBuf.tierName();
}

// Defined below.
static FIL* openValidSdBank(size_t* outSize, gm_bank_header_t* outHdr);

// Bind the engine to the bank built into a 16 MB image. False on a 4 MB image.
bool MidiSynth::bindBuiltin() {
#if GM_BANK_BUILTIN
    gm_bank_view_t v;
    if (!gm_bank_view(gb_gm_bank_builtin, &v)) return false;
    midi_wt_bind(gb_gm_bank_builtin);
    bank_ready = true;
    return true;
#else
    return false;
#endif
}

// Buffer::LoadReader over the open SD bank file: fill `dst` with `n` bytes at `off`.
static bool bankFileReader(void* ctx, void* dst, uint32_t off, uint32_t n) {
    FIL* f = (FIL*)ctx;
    UINT br = 0;
    if (f_lseek(f, off) != FR_OK) return false;
    return f_read(f, dst, n, &br) == FR_OK && br == n;
}

// Load the SD bank into the butter PSRAM arena and bind. Butter only: the heap is far
// too small for a bank and a small one there would starve the emulator, and SPI PSRAM
// is not pointer-addressable. Returns true once bound.
bool MidiSynth::loadBank() {
    if (!Buffer::butterPoolReady()) return false;
    size_t size; gm_bank_header_t hdr;
    FIL* f = openValidSdBank(&size, &hdr);
    if (!f) return false;                                  // no usable SD bank
    g_bankBuf.free();
    if (!g_bankBuf.alloc(size, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM) ||
        g_bankBuf.tier() != Buffer::TIER_BUTTER) {
        g_bankBuf.free();
        fclose2(f);
        Debug::log("MidiSynth: bank alloc failed (%uKB)", (unsigned)(size >> 10));
        return false;
    }
    bool ok = g_bankBuf.load((uint32_t)size, bankFileReader, f);
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
    if (loadBank()) return;
    bindBuiltin();
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
    if (builtinPicked()) return nullptr;        // 16 MB: the built-in bank, not the card
    if (!Config::midi_bank.empty()) {
        FIL* f = tryOpenBank(Config::midi_bank.c_str(), outSize, outHdr);
        if (f) return f;                        // chosen bank still present & valid
    }
#if GM_BANK_BUILTIN
    return nullptr;                             // chosen bank gone -> the built-in one
#endif
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
    if (builtinPicked()) return 0;
    if (!Config::midi_bank.empty()) {
        const size_t n = bankFileBytes(Config::midi_bank.c_str());
        if (n) return n;
    }
#if GM_BANK_BUILTIN
    return 0;                                   // a vanished pick plays the built-in bank
#endif
    for (size_t i = 0; i < sizeof(kBankPaths) / sizeof(kBankPaths[0]); i++) {
        const size_t n = bankFileBytes(kBankPaths[i]);
        if (n) return n;
    }
    return 0;
}

// What a bank may weigh here: the butter arena's total (a CAPABILITY figure, not its
// free space right now — a live apply may still fail on occupancy, and the next boot,
// with an empty arena, places it). 0 on a board without butter PSRAM.
size_t MidiSynth::maxBankBytes() { return Buffer::butterArenaBytes(); }

// EARLY-BOOT bank setup, from setup() AFTER Buffer::initPools() (the butter arena must
// exist). Loads the selected SD bank into PSRAM; a 16 MB image falls back to its
// built-in bank.
void MidiSynth::provisionAtBoot() {
    if (Config::midi != 4) return;              // only GM.DLS mode provisions a bank
#if !GM_BANK_BUILTIN
    // No butter PSRAM (or switched off in the Debug menu) and no built-in bank: GM.DLS
    // cannot play here. Off for this session, as GS::init does for NeoGS; the menu does
    // not offer the mode on such a board (UiTree midi_modeOpts).
    if (!butter_psram_size()) {
        Debug::log("MidiSynth: GM.DLS needs butter PSRAM - MIDI off");
        OSD::bootNotice(" DLS Wavetable needs PSRAM - MIDI off ");
        Config::midi = 0;
        return;
    }
#endif
    Debug::log("MidiSynth: provisionAtBoot enter @%u ms",
               (unsigned)to_ms_since_boot(get_absolute_time()));
    if (loadBank()) return;

    // Nothing was installed. If a bank IS selected, say WHY, with the number — needs no
    // UART. Not worded as "too big": a failed read or a full arena lands here too.
    if (const size_t want = selectedBankBytes()) {
        const size_t cap = maxBankBytes();
        Debug::log("MidiSynth: bank %uKB NOT installed - %uKB PSRAM arena",
                   (unsigned)(want >> 10), (unsigned)(cap >> 10));
        OSD::bootNotice((std::string(" GM.DLS bank not installed: ") +
                         std::to_string(want >> 10) + " KB, " +
                         std::to_string(cap >> 10) + " KB PSRAM available ").c_str());
    }
    bindBuiltin();
}

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

// True if a valid bank is present on SD (the picker's "nothing to select" check).
bool MidiSynth::sdBankAvailable() {
    size_t size; gm_bank_header_t sdh;
    FIL* f = openValidSdBank(&size, &sdh);
    if (!f) return false;
    fclose2(f);
    return true;
}

// (Re)load + bind the selected bank without a reboot: an SD bank into PSRAM, or the
// built-in one on a 16 MB image. False = nothing bound (no PSRAM, no room, no bank).
bool MidiSynth::applyBankLive() {
    bank_ready = false;                   // tear down current; rebind below
    midi_wt_unbind();
    g_bankBuf.free();
    if (builtinPicked()) return bindBuiltin();
    if (loadBank()) return true;
#if GM_BANK_BUILTIN
    if (!sdBankAvailable()) return bindBuiltin();   // the SD pick is gone -> built-in
#endif
    return false;
}

void MidiSynth::deinit() {
    bank_ready = false;       // stop using the bank
    midi_status = midi_data_pos = midi_expected = 0;
    g_gate = 0;
    midi_wt_unbind();         // release the ~5 KB voice array (lazy; back to .bss-free)
    g_bankBuf.free();         // release the PSRAM copy (no-op for the built-in bank)
}

void MidiSynth::reset() {
    midi_status = midi_data_pos = midi_expected = 0;
    g_gate = 0;
    if (bank_ready) midi_wt_bind(bankBase());   // re-bind = all-notes-off
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
        vl = buf_L[i] + (vl < 0 ? 0 : (vl > 255 ? 255 : vl));
        vr = buf_R[i] + (vr < 0 ? 0 : (vr > 255 ? 255 : vr));
        buf_L[i] = (uint8_t)(vl > 255 ? 255 : vl);
        buf_R[i] = (uint8_t)(vr > 255 ? 255 : vr);
    }
}

