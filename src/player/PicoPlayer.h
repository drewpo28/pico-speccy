// pico-speccy — Pico-Zx-Player: the native music player behind Menu > Pico-Zx-Player
// and the F5 browser.
//
// It plays music files straight off the SD/USB card WITHOUT the emulated
// machine: the menu owns core0 and the emulation is paused, so the player may
// use core0 and the butter-PSRAM arena freely. It must NOT disturb the paused
// machine — its RAM pages, overlays and the emulator's own sound chips (chip0,
// oplfm, ...) are left untouched; every chip here is a private instance.
//
// Offered only on boards with QSPI (butter) PSRAM: file images and decoder
// state live in the Buffer arena, never in the SRAM heap beyond small hot
// buffers.
//
// A Decoder turns one file into 31250 Hz stereo int16 (the audio timer's own
// rate, so nothing downstream resamples) and reports per-channel levels for the
// meter. The page (src/ui/UiPlayer.cpp) owns the playlist, the ring that feeds
// pcm_call_inner, and the controls.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

namespace pp {

constexpr int RATE = 31250;          // output rate = the audio timer's
constexpr int MAX_CH = 40;           // meter channels a decoder may report

struct Meta {
    char format[40];                 // "MP3 128 kbps 44.1 kHz stereo", "VGM 1.51: AY8910 x2"
    char title[64];
    char author[64];
    char album[64];                  // album / game
    char extra[64];                  // year / system / notes
};

class Decoder {
public:
    virtual ~Decoder() {}
    // Opens and prepares the file. On failure returns false with `err` set.
    virtual bool open(const char* path) = 0;
    // Renders up to `n` stereo frames (interleaved L,R) at RATE. Returns the
    // number produced; 0 means the track has ended.
    virtual int render(int16_t* lr, int n) = 0;
    // Meter: number of channels, their short names (<= 3 chars) and an optional
    // group label shown above the first channel of a group (or nullptr).
    virtual int         channels() const = 0;
    virtual const char* chanName(int i) const = 0;
    virtual const char* groupName(int i) const { (void)i; return nullptr; }
    // Current level per channel, 0..255. Called ~25 times a second.
    virtual void        levels(uint8_t* out) = 0;
    virtual uint32_t    posMs() const = 0;
    virtual uint32_t    lenMs() const { return 0; }   // 0 = unknown

    Meta        meta {};
    // Bumped by a decoder whose meta changes while it plays (sub-songs of an .ay
    // file); the page redraws the info rows when it moves.
    uint32_t    metaSeq = 0;
    const char* err = nullptr;
};

// Lower-case extension (no dot) → is it a format the player plays?
bool     playableExt(const std::string& lcext);
// Creates the decoder for a lower-case extension, or nullptr.
Decoder* createDecoder(const std::string& lcext);

// Is the player available on this board (QSPI PSRAM present and pooled)?
bool     available();

// ── helpers shared by the decoders ──────────────────────────────────────────
// Copies text into `dst` for the UI font: printable ASCII, Cyrillic (from UTF-8 /
// CP1251 / UTF-16) as CP1251 bytes — the font's own encoding — Latin-1 accents
// folded to their base letter, anything else unprintable → '?'.
enum TextEnc : uint8_t { TE_LATIN1, TE_UTF8, TE_CP1251, TE_UTF16LE, TE_UTF16BE };
void     textCopy(char* dst, size_t cap, const uint8_t* src, size_t len, TextEnc enc);
// Trims trailing blanks / NULs.
void     textTrim(char* s);

} // namespace pp
