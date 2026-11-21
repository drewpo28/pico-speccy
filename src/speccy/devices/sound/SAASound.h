/*
  SAA1099 Sound Chip Emulation for pico-speccy ZX Spectrum emulator

  Based on stripwax/SAASound by Dave Hooper — verified against real SAA1099P.
  https://github.com/stripwax/SAASound

  Flattened from stripwax's CSAAFreq, CSAANoise, CSAAEnv, CSAAAmp, CSAADevice
  into a single class for RP2350 efficiency.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifndef SAASound_h
#define SAASound_h

#include "app/hardconfig.h"
#include "app/ESPectrum.h"
#include <inttypes.h>
#include <stddef.h>
#include <string.h>

class SAASound {
public:
    SAASound();

    void init();
    void reset();
    void selectRegister(uint8_t reg);
    void setRegisterData(uint8_t data);
    uint8_t getRegisterData();
    void set_sound_format(int freq, int chans, int bits);
    // Master clock in Hz. The generator was written for the ZX/SAM convention
    // of 8 MHz with one tick per 31250 Hz output sample; other clocks (the
    // Creative Music System runs its two SAA1099s at 7.159090 MHz) scale every
    // counter increment by clock/8MHz in Q16. Default 8000000 = bit-identical
    // to the pre-clock-aware code.
    void set_clock(int hz);
    // ADDS (saturating at 255) into sinkL/sinkR[bufpos ..], gated by mix (0xFF
    // heard / 0 not; the generators step either way). The emulator's chips
    // (saaChip, cmsChip) add straight into the frame mix, ESPectrum::audioBuffer_
    // L/R — no buffers of their own since 2026-11-21; the players use SAASoundBuf.
    void gen_sound(int bufsize, int bufpos);
    void setSink(uint8_t* L, uint8_t* R) { sinkL = L; sinkR = R; }
    uint8_t* sinkL = ESPectrum::audioBuffer_L;
    uint8_t* sinkR = ESPectrum::audioBuffer_R;
    uint8_t  mix = 0xFF;

private:
    uint32_t tick_q16 = 1u << 16;   // clock / 8 MHz, Q16 (see set_clock)
    uint32_t rem_scale_q16 = 1u << 16;  // 65536 / tick_q16, Q16 (see set_clock)
    uint8_t regs[32];
    uint8_t selectedRegister;
    bool outputEnabled;
    bool syncState;

    // Per-channel state (CSAAFreq + CSAAAmp flattened)
    struct Channel {
        // Tone generator (from CSAAFreq)
        uint8_t freq_offset;    // current frequency offset
        uint8_t octave;         // current octave
        uint8_t next_offset;    // buffered offset
        uint8_t next_octave;    // buffered octave
        bool new_data;          // buffered freq data pending
        bool ignore_offset;     // Philips quirk: defer offset after same-cycle octave
        uint32_t counter;       // tone accumulator
        uint32_t period;        // = max(511 - freq_offset, 1)
        uint8_t level;          // square wave output (0 or 1)

        // Amplifier/mixer (from CSAAAmp)
        uint8_t amp_left;       // 0-15
        uint8_t amp_right;      // 0-15
        uint8_t mix_mode;       // bit0=tone, bit1=noise
    };
    Channel channels[6];

    // Noise generator (from CSAANoise)
    struct NoiseGen {
        uint32_t counter;
        uint8_t source;         // 0-3
        uint32_t rand;          // 18-bit LFSR state
    };
    NoiseGen noise[2];

    // Envelope generator (from CSAAEnv)
    struct EnvelopeGen {
        bool enabled;
        bool invert_right;
        bool clock_externally;
        bool envelope_ended;
        bool looping;
        uint8_t num_phases;
        uint8_t phase;
        uint8_t phase_position;
        uint8_t resolution;     // 1=4-bit (step by 1), 2=3-bit (step by 2)
        uint8_t shape;          // 0-7 (index into env_shapes)
        bool new_data;
        uint8_t next_data;      // buffered register byte
        int left_level;
        int right_level;
    };
    EnvelopeGen envs[2];

    // Tone: update buffered octave/offset on half-cycle
    void toneUpdateData(int ch);

    // Envelope control
    void envSetControl(int env, uint8_t data);
    void envTick(int env);
    void envSetLevels(int env);
    void envSetNewData(int env, uint8_t data);

    // Envelope shape data table (from stripwax cs_EnvData)
    struct EnvShape {
        uint8_t num_phases;
        bool looping;
        uint8_t levels[2][2][16]; // [resolution 0=4bit 1=3bit][phase][position]
    };
    static const EnvShape env_shapes[8];

    // PDM effective amplitude table (from stripwax SAAAmp)
    // pdm_x4[amp/2][env_level] = EffectiveAmplitude * 4
    static const uint16_t pdm_x4[8][16];
};

// A chip with buffers of its own, for the players: gen_sound clears the range
// first, so the frame reads as it always did from SamplebufSAA_L/R.
struct SAASoundBuf : public SAASound {
    uint8_t SamplebufSAA_L[ESP_AUDIO_SAMPLES_PENTAGON];
    uint8_t SamplebufSAA_R[ESP_AUDIO_SAMPLES_PENTAGON];
    SAASoundBuf() { setSink(SamplebufSAA_L, SamplebufSAA_R); }
    SAASoundBuf(const SAASoundBuf&) = delete;           // the sink points into *this
    SAASoundBuf& operator=(const SAASoundBuf&) = delete;
    void gen_sound(int bufsize, int bufpos) {
        if (bufsize > 0 && bufpos >= 0 && bufpos + bufsize <= ESP_AUDIO_SAMPLES_PENTAGON) {
            memset(SamplebufSAA_L + bufpos, 0, (size_t)bufsize);
            memset(SamplebufSAA_R + bufpos, 0, (size_t)bufsize);
        }
        SAASound::gen_sound(bufsize, bufpos);
    }
};

// saaChip is heap-allocated by SaaSubsys when Config::SAA1099 is true; nullptr otherwise.
extern SAASound* saaChip;

// The Creative Music System / Game Blaster pair (CmsSubsys, Config::cms):
// two more SAA1099s at the CMS clock, on Z80 ports #D4-#D7 (see Ports.cpp).
#define CMS_SAA_CLOCK 7159090
extern SAASound* cmsChip[2];

#endif // SAASound_h
