#pragma once


#include <inttypes.h>
#include <string>
#include <vector>

// MIDI wavetable synth facade.
//
// Wraps the xrip embeded-midi-synth fixed-point GM wavetable engine
// (external/embeded-midi-synth, hosted by src/midi_wt.c). It plays a
// user-provided General MIDI sound bank ("gm_bank.bin", packed off-device at
// 31250 Hz from a gm.dls / GUS set). No bank ships in the repo/firmware.
//
// The bank needs a directly-addressable home (the engine reads samples through a raw
// pointer), so there are exactly two: butter QSPI PSRAM — reloaded from SD each boot,
// a bank change applies live — or a fixed flash partition (top of flash, see
// rp2350-memmap.ld), provisioned once from SD at early boot and read via XIP, which
// persists across reboots and needs no card. Config::midi_storage picks between them
// on a QSPI board; SPI PSRAM is accessor-only, so a board without QSPI always uses
// flash. Buffer does the placement — MidiSynth never branches on memory type.
//
// This keeps the public API the rest of the emulator already calls
// (Midi.cpp / Subsystem.cpp / ESPectrum.cpp / OSDMain.cpp). The render output is
// signed (stored as int8 in the uint8 L/R buffers, silence = 0) and added into
// the audio bus by the mixer in ESPectrum::loop.
class MidiSynth {
public:
    static void init();    // bind bank from the flash partition if present (safe at boot)
    static void deinit();  // stop using the bank (it stays in flash, persistent)
    static void reset();   // reset parser + silence all voices
    // EARLY-BOOT ONLY: write gm_bank.bin from SD into the flash region (single core,
    // before core1/video). Call once in main() between ESPectrum::setup() and the
    // core1 launch. Slow (~10 s, LED blinks); no-op unless a write is needed.
    static void provisionAtBoot();
    // True if a valid gm_bank.bin on SD differs from / is missing in flash (i.e. a
    // boot-time write is needed). Used by the OSD to decide whether to reboot.
    static bool needsProvision();
    // True if a valid gm_bank.bin exists on SD (gates the "reinstall" offer).
    static bool sdBankAvailable();
    // Size in bytes of the flash bank partition. A bank larger than this cannot be
    // installed (scanBanks/tryOpenBank reject it) — the OSD warns when a freshly
    // converted .dls overflows it.
    static size_t flashBankCapacity();
    // Enumerate selectable banks on SD (*.bin with a valid GMWB v5 header) in
    // CONFIG_DIR + card root. Fills index-aligned full paths + display names;
    // returns the count. Used by the OSD "instrument set" picker.
    static size_t scanBanks(std::vector<std::string>& paths,
                            std::vector<std::string>& names);
    // Force re-provision next boot: invalidate the flash header (1-sector erase).
    // Caller must reboot afterwards. Recovers a broken/partial flash bank.
    static void requestReflash();

    // Try to (re)load + bind the currently-selected bank WITHOUT a reboot. Returns
    // true if applied live (it fit PSRAM, or the flash partition already holds it);
    // false only when a flash *write* is required — the caller then reboots so
    // provisionAtBoot() can write it pre-video. With PSRAM storage this always
    // succeeds, so changing the bank never reboots. On false the bank is left unbound.
    static bool applyBankLive();

    // Feed one raw MIDI byte (running-status stream from the Z80 ShamaZX port).
    static void feedByte(uint8_t b);

    // Render `count` frames into the L/R buffers (called once per audio frame).
    static void gen_sound(uint8_t *buf_L, uint8_t *buf_R, int count);

    static bool bankReady() { return bank_ready; }

    // GM.DLS bank size if it landed in PSRAM (butter/SPI), else 0 (flash-resident or
    // no bank). For the Memory Info per-feature PSRAM breakdown.
    static size_t bankPsramBytes();

    // Where the bound bank actually lives ("PSRAM" / "flash" / "no bank") — the real
    // placement, not Config::midi_storage's request. For the Memory Info MIDI line.
    static const char* bankLocation();

private:
    // MIDI byte-stream parser state (reconstructs full messages, incl. running status)
    static uint8_t midi_status;
    static uint8_t midi_data[2];
    static uint8_t midi_data_pos;
    static uint8_t midi_expected;

    static bool  bank_ready;   // a valid bank (PSRAM or flash) is bound

    static bool bindFromFlash();  // validate + bind the persistent flash-partition bank (no write)
    // Load the SD bank into a Buffer (which places it in PSRAM or the flash partition —
    // MidiSynth stays oblivious) and bind. mayWriteFlash=false forbids a flash erase
    // (post-VIDEO::Init); a PSRAM load is always allowed (no reboot needed).
    static bool loadBank(bool force, bool mayWriteFlash);
    static void processMessage(uint8_t status, uint8_t d0, uint8_t d1);
};

