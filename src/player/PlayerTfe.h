// pico-speccy — Pico-Zx-Player: TFM Music Maker (.tfe) playback engine.
//
// A C re-implementation of ZXTune's TFM Music Maker support (vitamin-caig/zxtune,
// LGPL-3.0: src/formats/chiptune/fm/tfmmusicmaker.cpp and
// src/module/players/tfm/tfmmusicmaker.cpp, tfm_base_track.cpp), reduced to what
// a player needs: the RLE-packed editor image is decoded as a STREAM that keeps
// only the fixed header and the patterns the position list uses (the full v1.3
// image is 4.3 MB), then the tracker is stepped once per 50 Hz frame and emits
// YM2203 register writes for two chips.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace pp {

struct Meta;

class TfeEngine {
public:
    typedef void (*WriteFn)(void* ctx, int chip, uint8_t reg, uint8_t val);

    ~TfeEngine();
    // Parses + unpacks `file` (any memory). Returns false with `err` set.
    bool load(const uint8_t* file, uint32_t size, const char*& err, Meta& meta);
    // One 50 Hz frame of register writes. Returns false once the song has
    // played through (its first wrap to the loop position).
    bool frame(WriteFn fn, void* ctx);
    uint32_t lengthFrames() const { return lenFrames_; }

    struct Impl;
private:
    Impl*    d_ = nullptr;
    uint32_t lenFrames_ = 0;
};

} // namespace pp
