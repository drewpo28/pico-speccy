/*
  SAA1099 Sound Chip Emulation for pico-speccy ZX Spectrum emulator

  Based on stripwax/SAASound by Dave Hooper — verified against real SAA1099P.
  https://github.com/stripwax/SAASound

  Flattened from CSAAFreq, CSAANoise, CSAAEnv, CSAAAmp, CSAADevice into a
  single class. All logic ported faithfully:

  - Tone: integer step/period counter (mathematically equivalent to stripwax's
    fixed-point table). Octave/offset buffering with Philips documented quirk.
  - Noise: 18-bit Galois LFSR (x^18+x^11+x^1, mask 0x20400, seed 0xFFFFFFFF).
    Sources 0-2 counter-based, source 3 triggered by tone ch0/ch3.
  - Envelope: phase-based with resolution switching, buffered parameter updates
    applied only at natural phase boundaries (stripwax CSAAEnv::Tick logic).
  - Mixer: intermediate 0/1/2 model with PDM effective amplitude for envelope
    channels (stripwax SAAAmp).
  - Envelope only applies to ch2 (env0) and ch5 (env1).

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "SAASound.h"
#include "Config.h"
#include <string.h>


#ifndef IRAM_ATTR
#define IRAM_ATTR __not_in_flash("audio")
#endif

// Envelope shape data — from stripwax CSAAEnv::cs_EnvData.
// [resolution 0=4bit, 1=3bit][phase 0-1][position 0-15]
const SAASound::EnvShape SAASound::env_shapes[8] = {
    /* 0: zero */
    {1, false, {{{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                 {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
    /* 1: maximum */
    {1, true,  {{{15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15},
                  {15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15}},
                 {{14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
                  {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14}}}},
    /* 2: single decay */
    {1, false, {{{15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                 {{14,14,12,12,10,10,8,8,6,6,4,4,2,2,0,0},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
    /* 3: repetitive decay */
    {1, true,  {{{15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                 {{14,14,12,12,10,10,8,8,6,6,4,4,2,2,0,0},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
    /* 4: single triangular */
    {2, false, {{{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                  {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0}},
                 {{0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14},
                  {14,14,12,12,10,10,8,8,6,6,4,4,2,2,0,0}}}},
    /* 5: repetitive triangular */
    {2, true,  {{{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                  {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0}},
                 {{0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14},
                  {14,14,12,12,10,10,8,8,6,6,4,4,2,2,0,0}}}},
    /* 6: single attack */
    {1, false, {{{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                 {{0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}},
    /* 7: repetitive attack */
    {1, true,  {{{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
                 {{0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14},
                  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}}
};

// PDM effective amplitude table — from stripwax CSAAAmp::EffectiveAmplitude.
// Models analog PDM interaction of amplitude and envelope signals on real chip.
// Pre-multiplied by 4: pdm_x4[amp/2][env_level]
const uint16_t SAASound::pdm_x4[8][16] = {
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
    {  0,  4,  4,  8,  8, 12, 12, 16, 16, 20, 20, 24, 24, 28, 28, 32},
    {  0,  4,  8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60},
    {  0,  8, 12, 20, 24, 32, 36, 44, 48, 56, 60, 68, 72, 80, 84, 92},
    {  0,  8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96,104,112,120},
    {  0, 12, 20, 32, 40, 52, 60, 72, 80, 92,100,112,120,132,140,152},
    {  0, 12, 24, 36, 48, 60, 72, 84, 96,108,120,132,144,156,168,180},
    {  0, 16, 28, 44, 56, 72, 84,100,112,128,140,156,168,184,196,212}
};

// saaChip lives in heap; managed by SaaSubsys (see Subsystem.cpp).
SAASound* saaChip = nullptr;
SAASound* cmsChip[2] = { nullptr, nullptr };

SAASound::SAASound() {
    init();
}

void SAASound::init() {
    selectedRegister = 0;
    outputEnabled = false;
    syncState = false;
    memset(regs, 0, sizeof(regs));
    memset(channels, 0, sizeof(channels));
    memset(noise, 0, sizeof(noise));
    memset(envs, 0, sizeof(envs));

    for (int i = 0; i < 6; i++) {
        channels[i].period = 511;
        channels[i].level = 1; // INITIAL_LEVEL from stripwax
    }

    // LFSR seed 0xFFFFFFFF (stripwax CSAANoise constructor)
    noise[0].rand = 0xFFFFFFFF;
    noise[1].rand = 0xFFFFFFFF;

    for (int i = 0; i < 2; i++) {
        envs[i].envelope_ended = true;
        envs[i].resolution = 1;
        envs[i].left_level = 0;
        envs[i].right_level = 0;
    }

    memset(SamplebufSAA_L, 0, sizeof(SamplebufSAA_L));
    memset(SamplebufSAA_R, 0, sizeof(SamplebufSAA_R));
}

void SAASound::reset() {
    init();
}

void SAASound::set_sound_format(int freq, int chans, int bits) {
    (void)freq; (void)chans; (void)bits;
}

void SAASound::set_clock(int hz) {
    tick_q16 = (uint32_t)(((uint64_t)hz << 16) / 8000000u);
    // Reciprocal used by the sub-sample edge position in gen_sound: 65536 /
    // tick_q16 in Q16, i.e. exactly 65536 (1.0) at the stock 8 MHz clock.
    rem_scale_q16 = tick_q16 ? (uint32_t)((65536ull << 16) / tick_q16) : (1u << 16);
}

//////////////////////////////////////////////////////////////////////
// Register address select — also triggers external envelope clock
// (from stripwax CSAADevice::_WriteAddress)
//////////////////////////////////////////////////////////////////////
void SAASound::selectRegister(uint8_t reg) {
    selectedRegister = reg & 0x1F;
    // External envelope clock: only when selecting the corresponding
    // envelope register. Env0 on reg 24, Env1 on reg 25.
    if (selectedRegister == 24) {
        if (envs[0].clock_externally && envs[0].enabled) envTick(0);
    } else if (selectedRegister == 25) {
        if (envs[1].clock_externally && envs[1].enabled) envTick(1);
    }
}

uint8_t SAASound::getRegisterData() {
    if (selectedRegister > 0x1F) return 0xFF;
    return regs[selectedRegister];
}

//////////////////////////////////////////////////////////////////////
// Register data write (from stripwax CSAADevice::_WriteData)
//////////////////////////////////////////////////////////////////////
void SAASound::setRegisterData(uint8_t data) {
    if (selectedRegister > 0x1F) return;
    regs[selectedRegister] = data;

    switch (selectedRegister) {
    // Amplitude (=> CSAAAmp::SetAmpLevel)
    case 0: case 1: case 2: case 3: case 4: case 5:
        channels[selectedRegister].amp_left = data & 0x0F;
        channels[selectedRegister].amp_right = (data >> 4) & 0x0F;
        break;

    // Frequency offset (=> CSAAFreq::SetFreqOffset)
    case 8: case 9: case 10: case 11: case 12: case 13: {
        int ch = selectedRegister - 8;
        if (!syncState) {
            channels[ch].next_offset = data;
            channels[ch].new_data = true;
            // Philips quirk: if offset written without octave change,
            // defer offset to the half-cycle after next
            if (channels[ch].next_octave == channels[ch].octave)
                channels[ch].ignore_offset = true;
        } else {
            // During sync: apply immediately
            channels[ch].new_data = false;
            channels[ch].ignore_offset = false;
            channels[ch].freq_offset = data;
            channels[ch].next_offset = data;
            channels[ch].octave = channels[ch].next_octave;
            int p = 511 - (int)data;
            channels[ch].period = (p < 1) ? 1 : (uint32_t)p;
        }
        break;
    }

    // Octave (=> CSAAFreq::SetFreqOctave)
    case 16: case 17: case 18: {
        int base = (selectedRegister - 16) * 2;
        uint8_t oct_lo = data & 0x07;
        uint8_t oct_hi = (data >> 4) & 0x07;
        for (int i = 0; i < 2; i++) {
            int ch = base + i;
            uint8_t oct = (i == 0) ? oct_lo : oct_hi;
            if (!syncState) {
                channels[ch].next_octave = oct;
                channels[ch].new_data = true;
                channels[ch].ignore_offset = false;
            } else {
                channels[ch].new_data = false;
                channels[ch].ignore_offset = false;
                channels[ch].octave = oct;
                channels[ch].next_octave = oct;
                channels[ch].freq_offset = channels[ch].next_offset;
                int p = 511 - (int)channels[ch].freq_offset;
                channels[ch].period = (p < 1) ? 1 : (uint32_t)p;
            }
        }
        break;
    }

    // Tone mixer (=> CSAAAmp::SetToneMixer, bit 0 of mix_mode)
    case 20:
        for (int i = 0; i < 6; i++) {
            if (data & (1 << i))
                channels[i].mix_mode |= 1;
            else
                channels[i].mix_mode &= ~1;
        }
        break;

    // Noise mixer (=> CSAAAmp::SetNoiseMixer, bit 1 of mix_mode)
    case 21:
        for (int i = 0; i < 6; i++) {
            if (data & (1 << i))
                channels[i].mix_mode |= 2;
            else
                channels[i].mix_mode &= ~2;
        }
        break;

    // Noise source (=> CSAANoise::SetSource)
    case 22:
        noise[0].source = data & 0x03;
        noise[1].source = (data >> 4) & 0x03;
        break;

    // Envelope 0 (=> CSAAEnv::SetEnvControl)
    case 24:
        envSetControl(0, data);
        break;

    // Envelope 1 (=> CSAAEnv::SetEnvControl)
    case 25:
        envSetControl(1, data);
        break;

    // Global enable and sync (=> CSAADevice register 28)
    case 28: {
        bool newSync = (data & 0x02) != 0;
        if (newSync != syncState) {
            if (newSync) {
                // Sync ON: reset tone and noise counters
                for (int i = 0; i < 6; i++) {
                    channels[i].counter = 0;
                    channels[i].level = 1; // INITIAL_LEVEL
                    // Apply pending freq data immediately
                    channels[i].octave = channels[i].next_octave;
                    channels[i].freq_offset = channels[i].next_offset;
                    int p = 511 - (int)channels[i].freq_offset;
                    channels[i].period = (p < 1) ? 1 : (uint32_t)p;
                    channels[i].new_data = false;
                    channels[i].ignore_offset = false;
                }
                for (int i = 0; i < 2; i++) {
                    noise[i].counter = 0;
                }
                // Note: envelopes are NOT synced (per stripwax)
            }
            syncState = newSync;
        }

        bool newEnabled = (data & 0x01) != 0;
        outputEnabled = newEnabled;
        break;
    }

    default:
        break;
    }
}

//////////////////////////////////////////////////////////////////////
// Tone generator: update buffered octave/offset on half-cycle
// (from stripwax CSAAFreq::UpdateOctaveOffsetData)
//////////////////////////////////////////////////////////////////////
void SAASound::toneUpdateData(int ch) {
    Channel &c = channels[ch];
    if (!c.new_data) return;

    // Always apply octave
    c.octave = c.next_octave;
    // Only apply offset if not ignored (Philips quirk)
    if (!c.ignore_offset) {
        c.freq_offset = c.next_offset;
        c.new_data = false;
    }
    c.ignore_offset = false;

    // Recompute period
    int p = 511 - (int)c.freq_offset;
    c.period = (p < 1) ? 1 : (uint32_t)p;
}

//////////////////////////////////////////////////////////////////////
// Envelope: SetEnvControl (from stripwax CSAAEnv::SetEnvControl)
//////////////////////////////////////////////////////////////////////
void SAASound::envSetControl(int env, uint8_t data) {
    EnvelopeGen &e = envs[env];

    bool bEnabled = (data & 0x80) != 0;

    // If was disabled and still disabled, nothing to do
    if (!bEnabled && !e.enabled) return;

    e.enabled = bEnabled;
    if (!e.enabled) {
        // Disabling: mark as ended
        e.envelope_ended = true;
        return;
    }

    // Resolution is immediate (with phase position adjustment)
    uint8_t new_res = (data & 0x10) ? 2 : 1;
    if (e.resolution == 1 && new_res == 2) {
        e.phase_position &= 0x0E; // 4-bit→3-bit: clear LSB
    } else if (e.resolution == 2 && new_res == 1) {
        e.phase_position |= 0x01; // 3-bit→4-bit: set LSB
    }
    e.resolution = new_res;

    // Buffered parameters: apply immediately if envelope ended,
    // otherwise buffer until phase completion
    if (e.envelope_ended) {
        envSetNewData(env, data);
        e.new_data = false;
    } else {
        // Update levels for possible resolution change
        envSetLevels(env);
        // Buffer new data
        e.new_data = true;
        e.next_data = data;
    }
}

//////////////////////////////////////////////////////////////////////
// Envelope: Tick (from stripwax CSAAEnv::Tick)
//////////////////////////////////////////////////////////////////////
void SAASound::envTick(int env) {
    EnvelopeGen &e = envs[env];

    if (!e.enabled) {
        e.envelope_ended = true;
        e.phase = 0;
        e.phase_position = 0;
        return;
    }

    if (e.envelope_ended) return;

    // Advance position
    e.phase_position += e.resolution;

    bool bProcessNewData = false;
    if (e.phase_position >= 16) {
        e.phase++;
        if (e.phase == e.num_phases) {
            if (!e.looping) {
                // Non-looping: envelope ended at sustain level (0)
                e.envelope_ended = true;
                bProcessNewData = true;
            } else {
                // Looping: restart from phase 0
                e.envelope_ended = false;
                e.phase = 0;
                e.phase_position -= 16;
                bProcessNewData = true;
            }
        } else {
            // Middle of multi-phase envelope (triangular shapes)
            e.envelope_ended = false;
            e.phase_position -= 16;
        }
    } else {
        // Still within same phase
        e.envelope_ended = false;
    }

    // Apply buffered data at natural boundary
    if (e.new_data && bProcessNewData) {
        e.new_data = false;
        envSetNewData(env, e.next_data);
    } else {
        envSetLevels(env);
    }
}

//////////////////////////////////////////////////////////////////////
// Envelope: SetLevels (from stripwax CSAAEnv::SetLevels)
//////////////////////////////////////////////////////////////////////
void SAASound::envSetLevels(int env) {
    EnvelopeGen &e = envs[env];
    const EnvShape &shape = env_shapes[e.shape];

    if (e.resolution == 1) {
        // 4-bit resolution
        if (e.envelope_ended && !e.looping)
            e.left_level = 0;
        else
            e.left_level = shape.levels[0][e.phase][e.phase_position];
        if (e.invert_right)
            e.right_level = 15 - e.left_level;
        else
            e.right_level = e.left_level;
    } else {
        // 3-bit resolution
        if (e.envelope_ended && !e.looping)
            e.left_level = 0;
        else
            e.left_level = shape.levels[1][e.phase][e.phase_position];
        if (e.invert_right)
            e.right_level = 14 - e.left_level;
        else
            e.right_level = e.left_level;
    }
}

//////////////////////////////////////////////////////////////////////
// Envelope: SetNewEnvData (from stripwax CSAAEnv::SetNewEnvData)
//////////////////////////////////////////////////////////////////////
void SAASound::envSetNewData(int env, uint8_t data) {
    EnvelopeGen &e = envs[env];

    e.phase = 0;
    e.phase_position = 0;
    e.shape = (data >> 1) & 0x07;
    e.invert_right = (data & 0x01) != 0;
    e.clock_externally = (data & 0x20) != 0;
    e.num_phases = env_shapes[e.shape].num_phases;
    e.looping = env_shapes[e.shape].looping;
    e.resolution = (data & 0x10) ? 2 : 1;
    e.enabled = (data & 0x80) != 0;

    if (e.enabled) {
        e.envelope_ended = false;
    } else {
        e.envelope_ended = true;
        e.phase = 0;
        e.phase_position = 0;
    }

    envSetLevels(env);
}

//////////////////////////////////////////////////////////////////////
// Main audio generation — runs in RAM for speed on RP2350
// (from stripwax CSAADevice::_TickAndOutputStereo)
//
// Tick order matches stripwax:
//   1. Noise generators (sources 0-2)
//   2. For each channel 0-5: tone tick → mix → accumulate
//      Tone tick may trigger noise (source 3) or envelope (internal clock)
//////////////////////////////////////////////////////////////////////
IRAM_ATTR void SAASound::gen_sound(int bufsize, int bufpos) {
    uint8_t *buf_L = SamplebufSAA_L + bufpos;
    uint8_t *buf_R = SamplebufSAA_R + bufpos;

    // Muted fast path (reg 28 bit0 = 0, the power-on state): the per-sample
    // loop below ticks 2 noise LFSRs + 6 channel counters only to accumulate
    // nothing (outputEnabled gates every contribution) — ~624 iterations per
    // frame wasted on machines that never enable the SAA (Profi CP/M).
    // Generator phases freeze while muted and resume on re-enable; the real
    // chip keeps counting, but the difference is a phase offset of silent
    // oscillators — inaudible (same trade the syncState path makes).
    if (!outputEnabled) {
        memset(buf_L, 0, (size_t)bufsize);
        memset(buf_R, 0, (size_t)bufsize);
        return;
    }

    while (bufsize-- > 0) {
        // During sync: output silence, don't advance generators
        if (syncState) {
            *buf_L++ = 0;
            *buf_R++ = 0;
            continue;
        }

        // 1. Tick noise generators (sources 0-2 only)
        for (int ng = 0; ng < 2; ng++) {
            if (noise[ng].source < 3) {
                uint32_t period;
                switch (noise[ng].source) {
                case 0: period = 1; break;  // 31250 Hz
                case 1: period = 2; break;  // 15625 Hz
                default: period = 4; break; // 7812.5 Hz
                }
                noise[ng].counter += tick_q16;
                while (noise[ng].counter >= (period << 16)) {
                    noise[ng].counter -= (period << 16);
                    // 18-bit Galois LFSR (x^18+x^11+x^1, verified against SAA1099P)
                    if (noise[ng].rand & 1)
                        noise[ng].rand = (noise[ng].rand >> 1) ^ 0x20400;
                    else
                        noise[ng].rand >>= 1;
                }
            }
        }

        // 2. Process channels 0-5
        int output_l = 0, output_r = 0;

        for (int ch = 0; ch < 6; ch++) {
            Channel &c = channels[ch];
            int ng = ch / 3;

            // --- Tone tick (CSAAFreq::Tick) ---
            // Q16: increment scaled by clock/8MHz; period compared in Q16 too,
            // so at the default clock this is the old integer math times 65536.
            //
            // The mixer below takes the TIME AVERAGE of the square over this
            // output sample (lvl_acc), not the level at the sample point.
            // Taking the level alone snaps every edge to the 31250 Hz grid
            // (32 us), and that jitter is broadband noise only ~19 dB below a
            // 500 Hz tone — 8-10 dB dirtier than AySound at every pitch, which
            // oversamples 7x per output sample (ChipTacts_per_outcount) and
            // box-averages, so its edges land at 1/7-sample resolution. On a
            // real SAA1099 there is no such jitter at all: the output is an
            // analogue square whose edges fall wherever they fall. Measured on
            // the host with tools/saa_clock_test.cpp's harness (single channel,
            // full amplitude, harmonic energy vs everything else):
            //
            //     f0        AY 7x    SAA level-only    SAA time-average
            //     122 Hz    34.7      26.0              34.5
            //     500 Hz    28.9      19.0              29.1
            //    1215 Hz    23.4      14.6              23.9
            //    3216 Hz    17.2       9.6              17.5
            //
            // i.e. this puts the SAA exactly on the AY, and unlike 7x
            // oversampling it costs work per EDGE, not per sub-tick: inc is at
            // most half of period<<16 by construction (2^octave <= 128, period
            // >= 256), so there is never more than one edge per sample and
            // usually one per 10-30. In a sample with no edge lvl_acc is
            // level<<16 and the arithmetic is bit-identical to the old code.
            const uint8_t oct = c.octave;         // the octave THIS sample runs at
            const uint32_t inc = (tick_q16 << oct);
            uint32_t lvl_acc = 0;                 // sum of level * duration, Q16 time
            uint32_t t_prev = 0;                  // position in the sample, Q16 (0..65536)
            c.counter += inc;
            while (c.counter >= (c.period << 16)) {
                c.counter -= (c.period << 16);
                {
                    // What is left in the counter after the subtraction is how
                    // far past the threshold the sample already carried us,
                    // i.e. how much of the sample remains AFTER this edge:
                    // rem = c.counter / inc, in Q16 of one sample. Shifting
                    // by the octave first turns that into c.counter>>oct
                    // divided by tick_q16, and set_clock keeps the reciprocal
                    // (rem_scale_q16 = 65536/tick_q16, exactly 1.0 at the stock
                    // 8 MHz clock) so this is one multiply rather than a divide
                    // in a loop the compiler unrolls six times. Resolution is
                    // 1/65536 of a sample either way.
                    uint32_t rem = (uint32_t)
                        (((uint64_t)(c.counter >> oct) * rem_scale_q16) >> 16);
                    uint32_t t_flip = rem >= 65536u ? 0u : 65536u - rem;
                    if (t_flip < t_prev) t_flip = t_prev;   // keep it monotonic
                    lvl_acc += (uint32_t)c.level * (t_flip - t_prev);
                    t_prev = t_flip;
                }
                c.level ^= 1;

                // Trigger connected devices (from CSAAFreq constructor wiring):
                // ch0 → noise[0] source 3, ch3 → noise[1] source 3
                if (ch == 0 && noise[0].source == 3) {
                    if (noise[0].rand & 1)
                        noise[0].rand = (noise[0].rand >> 1) ^ 0x20400;
                    else
                        noise[0].rand >>= 1;
                }
                if (ch == 3 && noise[1].source == 3) {
                    if (noise[1].rand & 1)
                        noise[1].rand = (noise[1].rand >> 1) ^ 0x20400;
                    else
                        noise[1].rand >>= 1;
                }
                // ch1 → env[0] internal clock, ch4 → env[1] internal clock
                if (ch == 1 && envs[0].enabled && !envs[0].clock_externally)
                    envTick(0);
                if (ch == 4 && envs[1].enabled && !envs[1].clock_externally)
                    envTick(1);

                // Update buffered octave/offset on half-cycle completion
                toneUpdateData(ch);
            }

            lvl_acc += (uint32_t)c.level * (65536u - t_prev);

            // --- Mixer (CSAAAmp::Tick + TickAndOutputStereo) ---
            // tone_level and intermediate are Q16 now (65536 == the old 1):
            // the noise sources tick at most once per output sample (source 0
            // is clock/256 = the sample rate itself) and are advanced before
            // this loop, so noise_level is constant across the sample and only
            // the tone needs the time average.
            uint32_t tone_level = lvl_acc;
            int noise_level = noise[ng].rand & 1;
            uint32_t intermediate;   // Q16, 0..131072 (= the old 0..2)

            switch (c.mix_mode) {
            case 0: intermediate = 0; break;
            case 1: intermediate = tone_level * 2; break;
            case 2: intermediate = (uint32_t)noise_level * (2u << 16); break;
            case 3: intermediate = tone_level * (uint32_t)(2 - noise_level); break;
            default: intermediate = 0; break;
            }

            // Output amplitude
            if (!outputEnabled) {
                // Global mute — no output
            } else if ((ch == 2 && envs[0].enabled) || (ch == 5 && envs[1].enabled)) {
                // Envelope channel with active envelope: use PDM table
                int env_idx = ch / 3;
                int el = envs[env_idx].left_level;
                int er = envs[env_idx].right_level;
                int al_div2 = c.amp_left >> 1;
                int ar_div2 = c.amp_right >> 1;
                // 32-bit throughout: the largest product here is
                // pdm_x4[7][15] (212) * 131072 = 27.8M, well inside uint32_t.
                uint32_t inv = (2u << 16) - intermediate;   // (2 - intermediate), Q16
                output_l += (int)((pdm_x4[al_div2][el] * inv) >> 16);
                output_r += (int)((pdm_x4[ar_div2][er] * inv) >> 16);
            } else {
                // Non-envelope channel: simple amplitude * intermediate
                // (240 * 131072 = 31.5M, also inside uint32_t)
                output_l += (int)((c.amp_left  * 16u * intermediate) >> 16);
                output_r += (int)((c.amp_right * 16u * intermediate) >> 16);
            }
        }

        // 3. Scale to uint8_t
        // Max per channel (non-env): 15 * 2 * 16 = 480
        // Max per channel (env): pdm_x4[7][15] * 2 = 212 * 2 = 424
        // 6 channels max: ~2880. >>4 = 180.
        int out_l = (output_l + 8) >> 4;
        int out_r = (output_r + 8) >> 4;
        *buf_L++ = (uint8_t)(out_l > 255 ? 255 : out_l);
        *buf_R++ = (uint8_t)(out_r > 255 ? 255 : out_r);
    }
}
