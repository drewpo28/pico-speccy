// eLeMeNt ZX SID (Programmer's Reference, "Sound interface device"): one MOS 6581/8580
// behind #6C3B (register select) / #6D3B (data). h00-h1C are the C64 registers, h1F the
// model (bit 0: 8580) and the clock (bit 1: NTSC 1022730 Hz, else PAL 985248 Hz).
// A lite model after reSID: exact ADSR counters (and the ADSR delay bug), the 23-bit noise
// LFSR, sync and ring modulation, the 6581 DC that makes volume-register digis audible.
// Deviations: combined waveforms are the AND of their parts, the filter is a ZDF SVF on
// the reSID cutoff curves, the oscillators step once per output sample.
// No firmware dependencies: tools/elm_sid_test.cpp links this file.
#pragma once
#include <stdint.h>

namespace ElmSid {

enum { MAX_SAMPLES = 640 };
enum { PAR_8580 = 1, PAR_NTSC = 2 };

bool init(int outputRate);                 // false = no heap
void reset();
void setParams(uint8_t par);               // h1F / e_zxi_019
void mute(bool m);                         // e_zxi_002 bit 3 clear
void write(uint8_t reg, uint8_t v);
uint8_t read(uint8_t reg);
void renderTo(int sample);                 // catch up inside the frame
void frameEnd(int samples);                // the rest of the frame
// Mono, unipolar around a ramped +128, as the mixer wants it; nullptr = nothing to mix.
const int16_t* mix();

}
