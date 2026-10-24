// pico-speccy — PI-CARD MUSICA emulation (HRDY / demyanenko-d, PICARD-MUSICA).
//
// The card is a music coprocessor on two ports: the host (an esxDOS NMI-browser
// plugin such as AlexZor's "Tracker player", or Wild Commander's) only hands the
// FILE over the bus; the card parses and plays it. Protocol = docs/protocol.md
// of that firmware (summarised in PiMusica.cpp): the BOARD leads — it exposes a
// command code on #63 and a byte-serial argument frame on #67, the host serves
// read windows of the file (≤ 4096 bytes into #67) and acknowledges with an
// 8-byte command into #63.
//
// Here the card's decoders are the Pico-Zx-Player's (libxmp for MOD/S3M/XM/IT,
// the wavetable synth for .mid, src/player/), so this is the protocol, a file
// buffer in butter PSRAM and a ring the audio tick ADDS to the ZX mix
// (pcm_card_ring, pwm_audio.cpp). Tracker modules render on core1 (core1Pump,
// render_core), with a top-up from core0 at the frame end; a .mid renders on
// core0 only — midi_wt is one engine shared with the emulator's own live MIDI
// (MidiSynth), which steps aside while a card .mid plays.
//
// Reboot-class (Config::pimusica): the ring and the lock are taken once in
// setup(). Only on boards with QSPI (butter) PSRAM, like the player itself.
#pragma once
#include <stdint.h>

namespace PiMusica {
    // setup(): claims the ring / lock, arms the ports. nullptr = fitted, else
    // the reason it is not (for a bootNotice).
    const char* init();
    // Machine reset (F11): the plugin is gone with the program — end the session.
    void reset();
    // Port dispatch, low address byte only (0x63 / 0x67). Returns false when the
    // byte is not the card's (never, once fitted — kept for the caller's shape).
    bool portRead(uint8_t lo, uint8_t& v);
    void portWrite(uint8_t lo, uint8_t v);
    // core1 (render_core): one bounded render chunk into the ring when it has room.
    void core1Pump();
    // core0, once per emulated frame: open a received file, telemetry, seeks,
    // the .mid render and the tracker top-up.
    void frameTick();
    // A card .mid is playing: MidiSynth::gen_sound must not render midi_wt too.
    bool ownsMidiSynth();
    // Hardware Info.
    const char* status();
}

extern uint8_t g_pimus_on;        // card fitted — the one byte the RAM port paths test
extern bool    g_pimus_c1_live;   // core1 pump armed (a tracker module is open)
