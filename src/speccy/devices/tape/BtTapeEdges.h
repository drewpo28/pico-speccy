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

    // A hole in the stream (lost RTP packets, placed by their timestamps): the
    // clock jumps over it so the edges after it keep their true times. The level
    // is kept; the next crossing interpolates against the last sample before it.
    void skip(uint32_t samples) { n += samples; }

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

// ── EdgePlayer: the edges, replayed on the emulator's tape clock ───────────────
// Bluetooth delivers audio in bursts and on the PHONE's clock; the loader reads
// EAR on the EMULATOR's T-state clock. The player keeps a position in sample time
// (Q24) that advances with the T-states at fs / 3.5 MHz, BUF_MS behind what the
// decoder has produced so far, and returns the level of the last edge it passed.
//
// - Rate trim: once a frame, the fill (produced - position) is compared with
//   BUF_MS and the rate is trimmed by 50 % of the error per second, at most
//   +-TRIM_MAX ppm. It absorbs the phone/emulator clock difference and an
//   emulator that does not run at exactly 3.5 MHz real time; within a pulse train
//   it scales pulses by at most TRIM_MAX, far inside any loader's tolerance.
// - Recentering: in a gap (no edge for GAP_MS behind and ahead of the position,
//   i.e. a tape pause or silence) the position jumps straight to the target.
//   Pulses are never cut: the jump stays GAP_MS after the last edge and 20 ms
//   before the next one known.
// - Underflow (the position caught up with the decoder): it waits there, the
//   level held; counted. A load that underflows mid-block will fail, like a
//   tape that stops.
// Pop: bool(uint32_t&) — the next edge from the producer's ring, false = none.
class EdgePlayer {
public:
    static const int32_t BUF_MS   = 250;
    static const int32_t GAP_MS   = 50;
    static const int32_t TRIM_MAX = 40000;     // ppm (4 %)

    void start(int sampleRate, uint64_t nowT, uint32_t producedQ8) {
        fs = sampleRate;
        k40 = ((uint64_t)fs << 40) / 3500000u;
        buf24 = ms24(BUF_MS);
        lastT = nowT;
        prodQ8 = producedQ8;
        prod24 = (int64_t)producedQ8 << 16;
        pos24 = prod24 - buf24;
        lastEdge24 = pos24;
        trim = 0;
        lvl = 0; haveLevel = false; havePeek = false;
        underflows = recenters = played = 0;
        blocks = 0; runEdges = 0;
        starving = false;
    }

    template <class Pop> void frame(uint64_t nowT, uint32_t producedQ8, Pop pop) {
        advance(nowT);
        prod24 += (int64_t)(int32_t)(producedQ8 - prodQ8) << 16;
        prodQ8 = producedQ8;
        const int64_t fill = prod24 - pos24;
        // A long stall (the menu was open, the emulator paused): start over.
        if (fill > ms24(2000) || fill < -ms24(2000)) {
            pos24 = prod24 - buf24; lastEdge24 = pos24; recenters++;
            return;
        }
        int64_t t = (fill - buf24) * 500000 / ((int64_t)fs << 24);
        if (t > TRIM_MAX) t = TRIM_MAX;
        if (t < -TRIM_MAX) t = -TRIM_MAX;
        trim = (int32_t)t;
        if (!havePeek) peek(pop);
        const bool gapBehind = pos24 - lastEdge24 > ms24(GAP_MS);
        const bool gapAhead  = !havePeek || next24 - pos24 > ms24(GAP_MS);
        if (gapBehind && gapAhead) {
            int64_t target = prod24 - buf24;
            const int64_t lo = lastEdge24 + ms24(GAP_MS);
            const int64_t hi = havePeek ? next24 - ms24(20) : target;
            if (lo <= hi) {
                if (target < lo) target = lo;
                if (target > hi) target = hi;
                const int64_t d = target - pos24;
                // Back only with no edge ahead: once one is known, a step back is
                // undone by the frame's advance and redone at the next frame, for
                // ever when the stream has ended (host test: the last edge never
                // played). Ahead of the target with an edge coming, the trim
                // alone slows down.
                if (d > ms24(5) || (d < -ms24(5) && !havePeek)) { pos24 = target; recenters++; }
            }
        }
    }

    // The EAR level now (0/1). Call on every EAR read.
    template <class Pop> uint8_t level(uint64_t nowT, Pop pop) {
        advance(nowT);
        for (;;) {
            if (!havePeek && !peek(pop)) break;
            if (next24 > pos24) break;
            // Block counter (the F8 line): a run of BLOCK_EDGES edges after at
            // least BLOCK_GAP_MS of silence is a new block — a pilot tone, like
            // a real deck's counter sees it. Noise bursts shorter than that
            // do not count.
            if (next24 - lastEdge24 > ms24(BLOCK_GAP_MS)) runEdges = 0;
            if (++runEdges == BLOCK_EDGES) blocks++;
            lvl = nextLvl;
            lastEdge24 = next24;
            havePeek = false;
            played++;
        }
        return lvl;
    }

    int32_t  trimPpm() const { return trim; }
    int32_t  fillMs() const { return (int32_t)((prod24 - pos24) * 1000 / ((int64_t)fs << 24)); }
    uint32_t underflows = 0, recenters = 0, played = 0;
    uint32_t blocks = 0;                       // pilot-led runs heard so far
    static const int32_t  BLOCK_GAP_MS = 200;
    static const uint32_t BLOCK_EDGES  = 256;  // the ROM locks on after 256 pilot pulses

private:
    int64_t ms24(int32_t ms) const { return ((int64_t)fs * ms / 1000) << 24; }

    void advance(uint64_t nowT) {
        const uint64_t dt = nowT > lastT ? nowT - lastT : 0;
        lastT = nowT;
        if (!dt) return;
        int64_t inc = (int64_t)((dt * k40) >> 16);
        inc += inc / 1000000 * trim + (inc % 1000000) * trim / 1000000;
        pos24 += inc;
        if (pos24 > prod24) {
            pos24 = prod24;
            if (!starving) { underflows++; starving = true; }
        } else {
            starving = false;
        }
    }

    template <class Pop> bool peek(Pop pop) {
        uint32_t e;
        if (!pop(e)) return false;
        const int64_t base = pos24 >> 16;                 // Q8, unwrapped
        const int64_t q8 = base + (int32_t)((e & ~1u) - (uint32_t)base);
        next24 = q8 << 16;
        nextLvl = (uint8_t)(e & 1u);
        if (!haveLevel) { lvl = nextLvl ^ 1u; haveLevel = true; }
        havePeek = true;
        return true;
    }

    int      fs = 44100;
    uint64_t k40 = 0;          // Q24 samples per T-state, << 16
    int64_t  buf24 = 0, pos24 = 0, prod24 = 0, lastEdge24 = 0, next24 = 0;
    uint32_t prodQ8 = 0;
    uint64_t lastT = 0;
    int32_t  trim = 0;
    uint8_t  lvl = 0, nextLvl = 0;
    bool     haveLevel = false, havePeek = false, starving = false;
    uint32_t runEdges = 0;
};

}  // namespace bttape
