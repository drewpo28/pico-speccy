#pragma once


#include <inttypes.h>
#include <string>
#include <vector>

// MIDI wavetable synth facade.
//
// Wraps the xrip embeded-midi-synth fixed-point GM wavetable engine
// (external/embeded-midi-synth, hosted by src/speccy/devices/sound/midi_wt.c). It plays a
// user-provided General MIDI sound bank ("gm_bank.bin", packed off-device at
// 31250 Hz from a gm.dls / GUS set). No bank ships in the repo/firmware.
//
// The bank needs a directly-addressable home (the engine reads samples through a raw
// pointer): butter QSPI PSRAM, loaded from SD at boot and live on a pick, or on a 16 MB
// image the bank built into the firmware. A board without butter PSRAM plays GM.DLS
// only from that built-in bank. (A bank written into a flash partition on such boards
// existed until 2026-11-27.)
//
// This keeps the public API the rest of the emulator already calls
// (Midi.cpp / Subsystem.cpp / ESPectrum.cpp / OSDMain.cpp). The render output is
// signed (stored as int8 in the uint8 L/R buffers, silence = 0) and added into
// the audio bus by the mixer in ESPectrum::loop.
class MidiSynth {
public:
    static void init();    // load the selected SD bank into PSRAM, else bind the built-in one
    static void deinit();  // stop using the bank, free the PSRAM copy
    static void reset();   // reset parser + silence all voices
    // EARLY BOOT, after Buffer::initPools(): load the selected bank (PSRAM / built-in);
    // a selected bank that cannot be placed raises a boot notice.
    static void provisionAtBoot();
    // True if a valid bank exists on SD.
    static bool sdBankAvailable();
    // The biggest bank this board can bind: the butter PSRAM arena (0 without butter).
    // The picker and the .dls converter measure against it.
    static size_t maxBankBytes();
    // File size of the bank the picker has selected (Config::midi_bank, else the
    // default gm_bank.bin locations), 0 if none is valid.
    static size_t selectedBankBytes();
    // Enumerate selectable banks on SD (*.bin with a valid GMWB v5 header) in
    // CONFIG_DIR + card root. Fills index-aligned full paths + display names;
    // returns the count. Used by the OSD "instrument set" picker.
    static size_t scanBanks(std::vector<std::string>& paths,
                            std::vector<std::string>& names);
    // (Re)load + bind the selected bank WITHOUT a reboot. False = nothing bound (no
    // butter PSRAM, the arena is full, or no valid bank).
    static bool applyBankLive();

    // Feed one raw MIDI byte (running-status stream from the Z80 ShamaZX port).
    static void feedByte(uint8_t b);

    // Render `count` frames into the L/R buffers (called once per audio frame).
    // ADDS (saturating) into buf_L/buf_R — the frame mix, ESPectrum::audioBuffer_
    // L/R, since 2026-11-21 (no MIDI buffers of its own any more).
    static void gen_sound(uint8_t *buf_L, uint8_t *buf_R, int count);

    static bool bankReady() { return bank_ready; }

    // GM.DLS bank size if it sits in PSRAM, else 0 (built-in or no bank). For the
    // Memory Info per-feature PSRAM breakdown.
    static size_t bankPsramBytes();

    // Where the bound bank lives ("PSRAM" / "built-in" / "no bank"). Memory Info.
    static const char* bankLocation();

private:
    // MIDI byte-stream parser state (reconstructs full messages, incl. running status)
    static uint8_t midi_status;
    static uint8_t midi_data[2];
    static uint8_t midi_data_pos;
    static uint8_t midi_expected;

    static bool  bank_ready;   // a valid bank (PSRAM or built-in) is bound

    static bool bindBuiltin();  // bind the 16 MB image's built-in bank; false on 4 MB
    static bool loadBank();     // the SD bank into the butter arena + bind
    static void processMessage(uint8_t status, uint8_t d0, uint8_t d1);
};

