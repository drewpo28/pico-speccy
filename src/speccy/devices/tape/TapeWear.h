/*

pico-speccy — worn tape model ("the recorder is chewing the tape")

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#ifndef TapeWear_h
#define TapeWear_h

#include <inttypes.h>

// The arithmetic of a stretched, creased, oxide-shedding cassette played on a
// deck whose capstan no longer holds speed. It depends on NOTHING from the
// firmware — that is the only reason tools/tapewear_test.cpp can drive it on a
// host. Tape.cpp owns the one instance, feeds it the pulse the tape state
// machine just chose, and does the two things the model cannot: freeze the ear
// bit and read Config.
//
// Three faults, because each one is a different thing the user remembers:
//
//   wow    the speed wanders, so every pulse comes out a little long or short.
//          The tape signal feeds the speaker, so this is AUDIBLE as the warble
//          everyone remembers — and on its own it is nearly harmless to the
//          loader, exactly as on real hardware.
//   lurch  the tape binds for a moment and the pulses stretch 1.5-3x. The ROM's
//          LD-EDGE classifier then reads a 0 bit as a 1, the block's parity byte
//          disagrees, and LD-BYTES returns with carry clear: "R Tape loading
//          error".
//   drop   a crease or a bald patch lifts the tape off the head: no signal at
//          all for a few milliseconds. Either LD-EDGE's own counter times out,
//          or the frozen level swallows whole bits. Same message either way.

namespace tapewear {

// T-states (3.5 MHz): 3500000 = one second of tape.
static const uint32_t WOW_STEP_T  = 3500;     // one wow step per ~1 ms of tape
static const uint32_t MAX_PULSE_T = 175000;   // 50 ms — longer is a pause, not signal

// A clean stretch: 400-1200 s of undamaged tape. The only property that matters is
// that it outlasts the longest thing anyone loads in one go — a 48K game is ~250 s
// of data — so that landing in one means the load gets through.
static const uint32_t CLEAN_MIN_T  = 1400000000u;   // 400 s
static const uint32_t CLEAN_SPAN_T = 2800000000u;   // ...plus up to 800 s

enum Fault : uint8_t { FAULT_NONE = 0, FAULT_DROP = 1, FAULT_LURCH = 2 };

struct Level {
    int32_t  wowMax;   // speed excursion, 1/256ths (26 = ~10%)
    int32_t  wowStep;  // per-step random walk, same units
    // T-states between faults, uniform over [evtMin, evtMin+evtSpan). evtMin is a
    // GUARANTEED clean run after every Play (the schedule is re-armed there), so it
    // has to stay a small fraction of the span: make it large and a block shorter
    // than it can never fail while a longer one always does, which is a cliff, not
    // a worn tape. 3500000 T = one second, and a side of tape is minutes, so these
    // do not fit 32 bits with room to spare — hence the 64-bit countdown below.
    uint32_t evtMin;
    uint32_t evtSpan;
    uint32_t durMin;   // fault length in T-states
    uint32_t durSpan;
    // Percent of intervals drawn from a CLEAN STRETCH instead (CLEAN_MIN_T below),
    // i.e. a part of the tape with no damage on it. Real wear is not spread evenly:
    // a cassette has bad patches and good runs, and whether a load survives is
    // mostly whether it started inside one. This is the knob that sets the chance a
    // WHOLE GAME gets through, and it has to be its own knob — one fault kills a
    // load, so with a single interval distribution "a 205 s game loads 10% of the
    // time" would force a mean of ~125 s, i.e. a level that is not heavy at all
    // (its screens would load 92% of the time). With the mixture, a damaged pass
    // still tears every few seconds and you still hear the tape being chewed.
    uint8_t  cleanPct;
};

// One second of tape is 3500000 T-states. The fault interval is what separates the
// three levels in USE, and it is tuned against the block sizes that exist rather
// than against round numbers — measured with tools/tapewear_test.cpp's own 48K ROM:
//
//              header 19 B   loader 200 B   code 1.5 KB   screen 6912 B   game 35 KB
//   Light         100%          100%           100%          100%            80%
//   Medium        100%          100%           100%           80%            40%
//   Heavy          95%           85%            14%           ~10%           ~10%
//
// (sampled over 120-400 fresh Plays each, so +-a few per cent; the suite asserts
// them with margins rather than pinning the exact figures. The GAME column is a
// requirement rather than an outcome — see cleanPct.)
//
// i.e. Light plays and loads (you only hear it), Medium usually stops a big load but
// gets a game in 4 times out of 10, and Heavy normally gets you the name off the
// header and then dies — the one everybody remembers — while still letting a game
// through 1 time in 10, so no level is a dead end. A fault in a PILOT tone is
// survivable (the ROM restarts its search, see the test), so the damaged-patch rate
// is effectively per second of DATA.
//
//  wow ±        step     one damaged patch per      lasting          clean
static const Level kLevels[3] = {
    {  10,  1,   280000000u, 2240000000u,   1750u,   7000u,   0 }, // Light:  ±4%, ~400 s, 0.5-2.5 ms
    {  20,  2,    42000000u,  336000000u,   7000u,  28000u,  40 }, // Medium: ±8%, ~60 s,  2-10 ms
    {  40,  3,     4200000u,   33600000u,  17500u,  87500u,  10 }, // Heavy: ±16%, ~6 s,   5-30 ms
};
struct State {
    uint8_t  level    = 0;      // 0 off, 1 Light, 2 Medium, 3 Heavy
    uint32_t rng      = 1;
    int32_t  wow      = 0;      // current speed offset, 1/256ths
    int32_t  wowNext  = 0;      // T-states until the next wow step
    int64_t  evtNext  = 0;      // T-states until the next fault
    uint32_t evtLeft  = 0;      // T-states remaining in the running fault
    uint8_t  evtKind  = FAULT_NONE;
    uint16_t evtMul   = 256;    // lurch stretch, 1/256ths
    uint32_t nominal  = 0;      // the length the caller last CHOSE...
    uint32_t delivered = 0;     // ...and what we returned for it (see pulseFedback)

    uint32_t rand() {           // xorshift32
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }

    // The interval to the next fault: a damaged patch, or a clean stretch.
    int64_t drawInterval() {
        const Level& L = kLevels[level - 1];
        if (L.cleanPct && (rand() % 100) < L.cleanPct)
            return (int64_t)CLEAN_MIN_T + (int64_t)(rand() % CLEAN_SPAN_T);
        return (int64_t)L.evtMin + (int64_t)(rand() % L.evtSpan);
    }

    void arm() {
        wow = 0;
        wowNext = (int32_t)WOW_STEP_T;
        evtLeft = 0;
        evtKind = FAULT_NONE;
        nominal = delivered = 0;
        evtNext = level ? drawInterval() : 0;
    }

    // Rewinding and pressing play again lands the head on a different part of the
    // damage, so every attempt is a new roll of the dice — which is the point.
    // "Try once more and it might load" is the memory being emulated.
    void reset(uint8_t lvl, uint32_t seed) {
        level = lvl > 3 ? 3 : lvl;
        rng   = seed | 1u;
        arm();
    }

    // The level is read live so a menu change reaches the tape already playing;
    // a change re-arms the schedule rather than leaving the old one running.
    bool sync(uint8_t lvl) {
        if (lvl > 3) lvl = 3;
        if (lvl != level) { level = lvl; arm(); }
        return level != 0;
    }

    // Advance the schedule by `t` T-states of tape travel and start a fault when
    // one comes due. `signalPhase` is false in a pause or tail, where a fault is
    // inaudible and invisible — the tape moves, but there is nothing recorded on
    // it to damage, and a single 1-second pause pulse would otherwise swallow the
    // whole event budget in one step.
    void advance(uint32_t t, bool signalPhase) {
        if (!level) return;
        if (evtKind) {
            evtLeft = (evtLeft > t) ? evtLeft - t : 0;
            if (!evtLeft) evtKind = FAULT_NONE;
            return;
        }
        if (!signalPhase) return;
        evtNext -= (int64_t)t;
        if (evtNext > 0) return;

        const Level& L = kLevels[level - 1];
        evtKind = (rand() & 1) ? FAULT_DROP : FAULT_LURCH;
        evtLeft = L.durMin + rand() % L.durSpan;
        evtMul  = (uint16_t)(384 + rand() % 385);   // 1.5x .. 3.0x
        evtNext = drawInterval();
    }

    // Takes the pulse length the tape machine chose and returns the one a worn
    // tape actually delivers. `freeze` comes back true while the head has no tape
    // against it: the caller holds the ear bit where it was, so the loader sees no
    // edge at all.
    uint32_t pulse(uint32_t next, bool signalPhase, bool& freeze) {
        freeze = false;
        if (!level || next == 0 || next > MAX_PULSE_T) return next;

        const Level& L = kLevels[level - 1];

        // Speed wander: a bounded random walk on a clock of its own, slower than
        // the pulse rate — real flutter is a few Hz, not per-pulse jitter.
        wowNext -= (int32_t)next;
        while (wowNext <= 0) {
            wow += (int32_t)(rand() % (uint32_t)(2 * L.wowStep + 1)) - L.wowStep;
            if (wow >  L.wowMax) wow =  L.wowMax;
            if (wow < -L.wowMax) wow = -L.wowMax;
            wowNext += (int32_t)WOW_STEP_T;
        }

        int32_t scaled = (int32_t)next + (int32_t)next * wow / 256;
        if (scaled < 1) scaled = 1;                       // never a zero-length pulse
        uint32_t out = (uint32_t)scaled;
        if (out > MAX_PULSE_T) out = MAX_PULSE_T;         // the wow may not uncap it

        advance(out, signalPhase);

        if (evtKind == FAULT_LURCH) {
            out = (uint32_t)(((uint64_t)out * evtMul) >> 8);
            if (out > MAX_PULSE_T) out = MAX_PULSE_T;
        } else if (evtKind == FAULT_DROP) {
            freeze = true;
        }
        return out ? out : 1;
    }

    // THE ENTRY POINT FOR A CALLER THAT STORES OUR RESULT IN THE VARIABLE IT PASSES
    // BACK IN, which is what Tape::Read does: its `tapeNext` is both the request and
    // the store, and the tape machine only writes it when the pulse LENGTH changes —
    // a pilot tone writes it once and then repeats it for thousands of pulses, and a
    // direct-recording block never writes it at all. So the value handed to us is
    // usually the one we returned last time, already worn, and wearing it again
    // COMPOUNDS: a bounded ±4% wow applied 8000 times running is not a warble but an
    // exponential runaway (measured: a 2168 T pilot collapses to 25 T at Light, so
    // the ROM's LD-LEADER can never lock on and nothing loads at any level). Undo our
    // own feedback and wear the nominal length instead.
    //
    // A caller that passes a freshly chosen length every time may call pulse() directly,
    // but it costs nothing to use this one and it cannot be got wrong.
    uint32_t pulseFedback(uint32_t next, bool signalPhase, bool& freeze) {
        if (next == delivered) next = nominal;
        const uint32_t out = pulse(next, signalPhase, freeze);
        nominal   = next;
        delivered = out;
        return out;
    }

    // WAV/MP3: driven by elapsed guest time rather than by pulses, and dropouts
    // only — a wow on a recorded waveform would mean resampling it, and those
    // files are recordings of a real tape anyway, so whatever wear is on them is
    // already there. Both fault kinds present as a dropout here.
    bool audio(uint32_t dt) {
        if (!level) return false;
        if (dt > MAX_PULSE_T) dt = 0;   // first call, or the menu held the machine
        if (dt) advance(dt, true);
        return evtKind != FAULT_NONE;
    }
};

} // namespace tapewear

#endif
