// eLeMeNt ZX SoundDMA: 8 channels fetched from protected URAM (Programmer's Reference,
// "SoundDMA"). What the document leaves open comes from the programs: SDMAPLAY gives the
// register byte order and the rate, Lnx AmigaMOD Player the meaning of Length: it is the
// end of the loop counted from Start, so a loop runs from Loop to Start + Length.
// No firmware dependencies: tools/elm_sdma_test.cpp links this file.
#pragma once
#include <stdint.h>

namespace ElmSdma {

enum { CHANNELS = 8, REG_BYTES = CHANNELS * 16, MAX_SAMPLES = 640 };
enum { P_PLAY = 1, P_CONT = 2, P_LOOP = 4, P_SIGNED = 8, P_EXT = 0x10, P_LATE = 0x20,
       P_16BIT = 0x40, P_STEREO = 0x80 };

// Reads the 8K page of protected URAM, nullptr when it has no pointer.
typedef const uint8_t* (*PageFn)(uint32_t page);

bool init(PageFn fn, int outputRate);      // false = no heap
void reset();
void setRate(int outputRate);
void write(uint8_t off, uint8_t v);        // off 0..REG_BYTES-1
void renderTo(int sample);                 // catch up inside the frame
void frameEnd(int samples);                // the rest of the frame
uint8_t posHigh();                         // g_zxi_04A
bool active();                             // something was audible this frame
// Unipolar around a ramped +128, as the mixer wants it; nullptr = nothing to mix.
const int16_t* mixL();
const int16_t* mixR();

}
