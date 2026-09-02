/*

pico-speccy — 2 x SN76489 PSG (Sega arcade pair), for the DivMMC VGM-player
sound card. VGM commands 0x50 (chip 1) / 0x30 (chip 2) are single-byte
register writes; the card maps them to Z80 ports #CC (chip 1) / #CD (chip 2),
write-only — see Ports.cpp.

Implements the Sega convention, which is what VGM players assume by default
(header fields "SN76489 feedback" = 0x0009, "shift register width" = 16):
3 tone channels + 1 noise channel per chip, 10-bit tone periods, 2 dB/step
attenuation, 16-bit noise LFSR reset to 0x8000 on every noise-register write,
white-noise feedback = bit0 XOR bit3, output = bit0. A tone period of 0 or 1
outputs a constant +1 — that is the chip's PCM mode (games stream samples by
hammering the volume register), and at our 31250 Hz mixing rate a 55-111 kHz
square is DC anyway.

The internal tick is clock/16 (~223.7 kHz at the standard 3.579545 MHz);
gen() steps both chips tick-by-tick with a Q16 accumulator and box-averages
the ticks inside each output sample, so PCM playback and high tones fold
down cleanly instead of aliasing on raw undersampling.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef SnSound_h
#define SnSound_h

#include <inttypes.h>

// Standard NTSC colorburst clock (SMS / the VGM default). The dual-chip
// arcade rips actually run 2 MHz (Sega System 1/2) or 4 MHz — selectable via
// Config::sn_clock, resolved by sn_clock_hz() in Subsystem.cpp.
#define SN76489_CLOCK 3579545

class SnSound {
public:
    SnSound();

    // clock = SN76489 master clock in Hz, rate = our output sample rate.
    void setRates(int clock, int rate);
    void reset();

    // One register-byte write to chip 0 or 1 (the chips share nothing).
    void write(int chip, uint8_t v);

    // Accumulates `count` mono samples (sum of both chips, unipolar,
    // saturated to 255) into buf[bufpos..]; the caller clears the range.
    void gen(uint8_t* buf, int count, int bufpos);

private:
    struct Chip {
        uint16_t period[4];   // tone 0-2 (10 bit); [3] = raw noise control reg
        uint8_t  att[4];      // attenuation 0..15 (15 = off)
        uint8_t  latch;       // last latched register (channel*2 + type)
        int32_t  cnt[4];      // tick countdowns (noise uses its own, below)
        uint8_t  level[4];    // square outputs 0/1; [3] = noise flip-flop
        uint16_t lfsr;        // 16-bit shift register
        uint8_t  lfsr_out;    // current noise output bit
    };

    void     writeChip(Chip& c, uint8_t v);
    void     noisePeriodReload(Chip& c);
    int32_t  noisePeriod(const Chip& c) const;
    void     tick(Chip& c);       // one clock/16 step
    int32_t  chipOut(const Chip& c) const;

    Chip     m_chip[2];
    uint32_t m_step_q16;          // chip ticks per output sample, Q16
    uint32_t m_acc_q16;           // fractional tick accumulator (chips share it)
    uint8_t  m_last_out;          // held when a sample spans no whole tick
};

// Allocated by SnSubsys while Config::sn76489 is on.
extern SnSound* snChip;

// Config::sn_clock resolved to Hz (Subsystem.cpp).
int sn_clock_hz();

#endif // SnSound_h
