// EpTapeRec.h — Enterprise tape SAVE: what EXOS writes to the cassette, recorded to a
// new ep128emu tape file (1 bit, 31250 Hz). See EpTapeRec.cpp.
#pragma once
#include <stdint.h>

namespace EpTapeRec {
    // One frame of Dave's left output (0..255). `remote`: a remote line is up;
    // `tapeRead`: the guest read #B6 this frame (a LOAD, not a SAVE).
    // Returns true on the frame recording starts (the caller quiets the deck).
    bool frame(const uint8_t* left, int n, bool remote, bool tapeRead);
    void close();        // finish the file now (reset, machine switch)
    bool active();
}
