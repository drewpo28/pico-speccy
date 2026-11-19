// Tape edges out of decoded audio — the "BT tape recorder" (docs/bt-tape-plan.md).
//
// A phone plays a tape's sound over A2DP; BtHost decodes the SBC frames into PCM
// and pushes every sample (channel 0) through EdgeExtractor, which turns it into
// edge events: the moment the signal changed sign, interpolated between samples.
//
// Depends on nothing from the firmware: tools/sbc_tape_test.cpp runs THIS code on
// the same SBC-encoded/decoded tape signals as its reference receiver, so a change
// here is checked there first (it exits non-zero when bitpool >= 41 stops loading).
//
// The rules, each from the host test (2026-11-18/19):
// - Schmitt trigger: a new level needs the signal past +-hyst, so codec noise
//   around zero cannot flip it. hyst = 15 % of the running peak, never below
//   HYST_FLOOR (silence after SBC is a few LSB; -44 dBFS is far above that).
// - The edge TIME is the zero crossing that led into the new level, linearly
//   interpolated between the two samples around it: 15-30 T of error against
//   60-100 T for the whole-sample time (both far inside a 4x turbo's 214 T).
// - The peak decays slowly (time constant 2^14 samples, ~0.37 s at 44.1 kHz), so
//   a tape's 1 s pause cannot shrink the hysteresis into the noise, and a volume
//   change on the phone is followed within a second or two.
// - Settle: the decoder's synthesis filter rings on its way from zero to the first
//   level (2 false edges in the first ~2 ms). Nothing is emitted for SETTLE
//   samples after reset().
//
// Output: one uint32 per edge, the time in 1/256 sample units since reset() with
// bit 0 replaced by the level AFTER the edge (1 = high). It wraps after 2^24
// samples (380 s at 44.1 kHz): consumers work with modular differences.
#pragma once
#include <stdint.h>

namespace bttape {

class EdgeExtractor {
public:
    static const int32_t  HYST_FLOOR = 200;     // int16 units, ~ -44 dBFS
    static const uint32_t SETTLE     = 441;     // samples (10 ms at 44.1 kHz)

    void reset() {
        n = 0; peak = 0; level = 0; prev = 0; prevPos = false;
        zcUp = zcDn = 0;
    }

    // One sample in; true + *edge when it completes an edge.
    bool push(int16_t x, uint32_t* edge) {
        const int32_t ax = x < 0 ? -(int32_t)x : x;
        peak -= peak >> 14;                        // slow decay (Q8 below)
        if ((ax << 8) > peak) peak = ax << 8;
        const bool pos = x > 0;
        if (n > 0 && pos != prevPos) {
            // Zero crossing between sample n-1 (prev) and n (x): prev and x have
            // different signs (or one is 0), so prev - x != 0.
            const int32_t num = (int32_t)prev << 8;
            const int32_t den = (int32_t)prev - x;
            const uint32_t t = ((n - 1) << 8) + (uint32_t)(num / den);
            if (pos) zcUp = t; else zcDn = t;
        }
        prev = x; prevPos = pos;
        const uint32_t i = n++;
        int32_t hyst = (int32_t)(((int64_t)peak * 38) >> 16);     // 0.15 * peak (peak is Q8)
        if (hyst < HYST_FLOOR) hyst = HYST_FLOOR;
        int8_t want = level;
        if (x > hyst) want = 1;
        else if (x < -hyst) want = -1;
        if (want == level) return false;
        const bool first = level == 0;
        level = want;
        if (first || i < SETTLE) return false;     // the starting level, or still ringing
        const uint32_t t = level > 0 ? zcUp : zcDn;
        *edge = (t & ~1u) | (level > 0 ? 1u : 0u);
        return true;
    }

    int8_t   currentLevel() const { return level; }   // 0 = not known yet
    uint32_t samples() const { return n; }            // samples since reset()
    int32_t  peakLevel() const { return peak >> 8; }  // int16 units

private:
    uint32_t n = 0;
    int32_t  peak = 0;          // Q8
    int8_t   level = 0;
    int16_t  prev = 0;
    bool     prevPos = false;
    uint32_t zcUp = 0, zcDn = 0;
};

}  // namespace bttape
