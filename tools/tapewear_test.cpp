// Host test for src/speccy/devices/tape/TapeWear.h — the worn-tape model behind
// Storage > Tape > Tape wear (Config::tape_wear).
//
//   g++ -O2 -Wall -Wextra -Isrc -o /tmp/tapewear_test tools/tapewear_test.cpp && /tmp/tapewear_test
//
// Re-run after ANY change to TapeWear.h. The model is the only part of the
// feature that can be checked without a Spectrum in front of you, and it fails
// by DEGREES — a tape that is a little too worn still looks like it works.
//
// The tape side below is a MIRROR OF Tape::Read's phase machine, including the
// thing that matters most: `tapeNext` is both the request and the store, and a
// pilot tone writes it once and then repeats it for thousands of pulses. The
// first version of this file handed the model a freshly chosen length on every
// call — which is only true of the DATA phases — so it passed while the shipped
// firmware wore its own output over and over and the pilot tone ran away
// exponentially (hw: nothing loaded at any level). Drive the model the way the
// caller really drives it, or the suite cannot see the bug that matters.
//
// The machine side is the real 48K ROM: LD-EDGE's 59 T sampling loop and its B
// counters, LD-LEADER's 256 pilot cycles, LD-SYNC, LD-8-BITS. Those counters are
// what decide whether a worn tape can be LOCKED ON TO at all, which no threshold
// approximation can tell you.

#include "speccy/devices/tape/TapeWear.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace tapewear;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

// ── Pulse lengths of a standard ROM block (Tape.h) ────────────────────────────
static const uint32_t PILOT = 2168, SYN1 = 667, SYN2 = 735, BIT0 = 855, BIT1 = 1710;
static const int PILOT_HDR = 8063, PILOT_DAT = 3223;

// ── The tape: Tape::Read's phase machine, driven through the model ────────────
enum { PH_SYNC, PH_SYNC1, PH_SYNC2, PH_DATA1, PH_DATA2, PH_TAIL, PH_END };
static const uint32_t TAIL = 7000, BLK_PAUSE = 3500000;

struct TapeSim {
    State w;
    std::vector<long long> edges;   // absolute T-state of every REAL level change
    long long t = 0;
    uint8_t level = 0, lastLevel = 0, frozen = 0;
    uint32_t tapeNext = 0;          // the shared slot: request AND store
    int phase = PH_END, hdrPulses = 0;
    const std::vector<uint8_t>* blk = nullptr;
    size_t byteIdx = 0; uint8_t curByte = 0, bitMask = 0x80;
    int faults = 0;
    bool pinSchedule = false;       // hold the fault off: wow in isolation
    uint32_t pilotMinOut = ~0u, pilotMaxOut = 0;

    // Exactly what Tape.cpp's wearPulse does around the model call.
    void emit(bool signalPhase, bool isPilot) {
        if (pinSchedule) w.evtNext = 0x40000000;
        const uint8_t was = w.evtKind;
        bool freeze = false;
        const uint32_t out = w.pulseFedback(tapeNext, signalPhase, freeze);
        if (w.evtKind && !was) faults++;
        if (freeze) { if (was == FAULT_NONE) frozen = level; level = frozen; }
        if (isPilot && !w.evtKind) {
            if (out < pilotMinOut) pilotMinOut = out;
            if (out > pilotMaxOut) pilotMaxOut = out;
        }
        if (level != lastLevel) { edges.push_back(t); lastLevel = level; }
        t += out;
    }

    void begin(const std::vector<uint8_t>& b, int pilot) {
        blk = &b; byteIdx = 0; curByte = b[0]; bitMask = 0x80;
        hdrPulses = pilot; phase = PH_SYNC; tapeNext = PILOT;
    }

    bool step() {
        if (phase == PH_END) return false;
        const bool isPilot = (phase == PH_SYNC) && hdrPulses > 1;
        // Tape.cpp's wearSignalPhase() excludes the tail and the pause.
        const bool signalPhase = (phase != PH_TAIL);
        switch (phase) {
            case PH_SYNC:
                level ^= 1;
                // Tape::Read reassigns tapeNext on the LAST pilot pulse only.
                if (--hdrPulses == 0) { phase = PH_SYNC1; tapeNext = SYN1; }
                break;
            case PH_SYNC1:
                level ^= 1; phase = PH_SYNC2; tapeNext = SYN2; break;
            case PH_SYNC2:
                level ^= 1; phase = PH_DATA1;
                tapeNext = (curByte & bitMask) ? BIT1 : BIT0; break;
            case PH_DATA1:
                level ^= 1; phase = PH_DATA2;
                tapeNext = (curByte & bitMask) ? BIT1 : BIT0; break;
            case PH_DATA2:
                level ^= 1;
                bitMask >>= 1;
                if (!bitMask) {
                    bitMask = 0x80;
                    // Block finished: this toggle still happens, and it is the edge
                    // that closes the last bit's second interval — the ROM cannot
                    // decode the final bit without it.
                    if (++byteIdx >= blk->size()) { phase = PH_TAIL; tapeNext = TAIL; break; }
                    curByte = (*blk)[byteIdx];
                }
                phase = PH_DATA1;
                tapeNext = (curByte & bitMask) ? BIT1 : BIT0;
                break;
            case PH_TAIL:
                level = 0;                 // the real TAIL case forces the level low
                phase = PH_END; tapeNext = BLK_PAUSE;
                break;
            default: return false;
        }
        emit(signalPhase, isPilot);
        return true;
    }

    void play(const std::vector<uint8_t>& b, int pilot) { begin(b, pilot); while (step()) {} }
};

// ── The machine: the 48K ROM's own LD-BYTES ───────────────────────────────────
//
// LD-EDGE-1 delays ~358 T and then samples port 0xFE every 59 T, incrementing B;
// B wrapping to 0 is the timeout that ABORTS the load. Every constant below is
// the ROM's: LD-LEADER needs 256 cycles each longer than 0xC6, LD-SYNC takes the
// first edge under 0xD4, LD-8-BITS calls it a 1 bit when the pair exceeds 0xCB.
struct Rom48 {
    const std::vector<long long>& e;
    size_t ei = 0;
    long long now = 0;
    bool timedOut = false;
    static const long long DELAY = 358, STEP = 59;

    explicit Rom48(const std::vector<long long>& edges) : e(edges) {}

    // LD-SAMPLE keeps polling while it counts, so a timeout costs real TIME as well
    // as the carry — which is what lets the retry below make progress.
    bool edge(int& B) {
        long long s = now + DELAY;
        for (;;) {
            B = (B + 1) & 0xFF;
            if (B == 0) { now = s; timedOut = true; return false; }
            if (ei < e.size() && e[ei] <= s) { now = s; ei++; return true; }
            s += STEP;
        }
    }
    bool edge2(int& B) { return edge(B) && edge(B); }
    bool exhausted() const { return ei >= e.size(); }

    // Returns true when `want` bytes arrived; false = "R Tape loading error".
    //
    // A TIMEOUT IN THE LEADER OR THE SYNC IS NOT FATAL, and getting that wrong makes
    // every level look far worse than the real machine: `JR NC,LD-BREAK` lands on
    // LD-BREAK's `RET NZ`, and LD-EDGE's timeout exit (`INC B / RET Z`) leaves Z SET,
    // so there is no return — control falls into LD-START and the whole pilot search
    // begins again. A header pilot is 5 s of tape and locking on needs only 512 clean
    // edges (~0.3 s), so the ROM rides out several dropouts there. Only LD-8-BITS
    // answers a timeout with `RET NC`, i.e. only a fault in the DATA fails the load.
    bool ldBytes(std::vector<uint8_t>& out, size_t want) {
        // A real user gives up eventually, and so must this: a broken model can emit
        // a pulse long enough that crossing it costs millions of timeout retries, and
        // a suite that hangs tells you less than one that fails.
        for (int tries = 0; tries < 20000; tries++) { // LD-START, retried
            if (exhausted()) return false;          // tape ran out: give up
            timedOut = false;
            int H = 0, B = 0;
            bool lost = false;
            for (;;) {                              // LD-LEADER
                B = 0x9C;
                if (!edge2(B)) { lost = true; break; }
                if (B <= 0xC6) { H = 0; continue; } // pulse too short -> restart
                if (++H == 256) break;
            }
            if (lost) continue;
            for (;;) {                              // LD-SYNC
                B = 0xC9;
                if (!edge(B)) { lost = true; break; }
                if (B < 0xD4) break;
            }
            if (lost) continue;
            if (!edge(B)) continue;                 // the second sync edge
            out.clear();
            while (out.size() < want) {             // LD-8-BITS
                int L = 1;
                for (int i = 0; i < 8; i++) {
                    int b = 0xB0;
                    if (!edge2(b)) return false;    // RET NC: the load has failed
                    L = ((L << 1) | (b > 0xCB ? 1 : 0)) & 0x1FF;
                }
                out.push_back((uint8_t)(L & 0xFF));
            }
            return true;
        }
        return false;
    }
};

static std::vector<uint8_t> makeBlock(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; v[i] = (uint8_t)(seed >> 16); }
    return v;
}

// Play one standard block through a worn tape into the ROM. True = loaded exactly.
static bool loadBlock(TapeSim& s, const std::vector<uint8_t>& blk, int pilot) {
    s.play(blk, pilot);
    Rom48 rom(s.edges);
    std::vector<uint8_t> got;
    if (!rom.ldBytes(got, blk.size())) return false;
    return got.size() == blk.size() && memcmp(got.data(), blk.data(), blk.size()) == 0;
}

// Success rate of one block over `n` fresh Plays (each is a new roll of the dice).
static int rate(uint8_t lvl, size_t bytes, int pilot, int n, uint32_t salt) {
    const std::vector<uint8_t> blk = makeBlock(bytes, 42);
    int ok = 0;
    for (uint32_t s = 1; s <= (uint32_t)n; s++) {
        TapeSim t;
        t.w.reset(lvl, s * 2654435761u + salt);
        if (loadBlock(t, blk, pilot)) ok++;
    }
    return ok * 100 / n;
}

int main() {
    // ── Off is a pure pass-through, through BOTH entry points ─────────────────
    {
        State w; w.reset(0, 12345);
        bool anyFreeze = false, anyChange = false;
        uint32_t rng = 7;
        for (int i = 0; i < 100000; i++) {
            rng = rng * 1103515245u + 12345u;
            const uint32_t in = 100 + (rng >> 20);
            bool f1 = false, f2 = false;
            if (w.pulse(in, true, f1) != in) anyChange = true;
            if (w.pulseFedback(in, true, f2) != in) anyChange = true;
            if (f1 || f2) anyFreeze = true;
        }
        check(!anyChange, "level 0 changes a pulse length");
        check(!anyFreeze, "level 0 freezes the ear bit");
        check(w.evtKind == FAULT_NONE, "level 0 starts a fault");
    }

    // ── A pulse longer than the cap is a pause, and passes through ────────────
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        State w; w.reset(lvl, 99 * lvl);
        bool freeze = false;
        const uint32_t big = MAX_PULSE_T + 1;
        check(w.pulse(big, true, freeze) == big, "an over-cap pulse is perturbed");
        check(!freeze, "an over-cap pulse freezes the ear bit");
        check(w.pulse(0, true, freeze) == 0, "a zero pulse is perturbed");
    }

    // ── A delivered pulse is never zero, and never over the cap ───────────────
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        State w; w.reset(lvl, 4242 + lvl);
        for (int i = 0; i < 400000; i++) {
            bool freeze = false;
            const uint32_t out = w.pulse(BIT0, true, freeze);
            check(out > 0, "a delivered pulse is zero");
            if (out > MAX_PULSE_T) { check(false, "a delivered pulse exceeds the cap"); break; }
        }
    }

    // ── A REPEATED pulse must not compound — the bug that shipped ─────────────
    //
    // Tape::Read hands the same slot back for every pulse of a pilot tone, so the
    // model has to undo its own feedback (pulseFedback). Without that, a bounded
    // wow applied 8000 times running is a geometric random walk with no bound: the
    // 2168 T pilot measured 25 T at LIGHT on hardware and nothing loaded at all.
    // The raw pulse() half of this test is what gives the assertion teeth — it
    // proves the harness can still see the fault it was written for.
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        const Level& L = kLevels[lvl - 1];
        const uint32_t band = PILOT * (uint32_t)L.wowMax / 256 + 1;
        State w; w.reset(lvl, 777 * lvl);
        uint32_t slot = PILOT, lo = ~0u, hi = 0;
        for (int i = 0; i < PILOT_HDR; i++) {
            w.evtNext = 0x40000000;                  // wow in isolation
            bool freeze = false;
            slot = w.pulseFedback(slot, true, freeze);
            if (slot < lo) lo = slot;
            if (slot > hi) hi = slot;
        }
        check(lo + band >= PILOT && hi <= PILOT + band,
              "a repeated pilot pulse drifts outside the wow band (compounding?)");

        State raw; raw.reset(lvl, 777 * lvl);
        uint32_t rslot = PILOT, rlo = ~0u, rhi = 0;
        for (int i = 0; i < PILOT_HDR; i++) {
            raw.evtNext = 0x40000000;
            bool freeze = false;
            rslot = raw.pulse(rslot, true, freeze);   // the WRONG entry point
            if (rslot < rlo) rlo = rslot;
            if (rslot > rhi) rhi = rslot;
        }
        check(rlo + band < PILOT || rhi > PILOT + band,
              "raw pulse() fed its own output did NOT run away - this test is blind");
    }

    // ── The speed wander stays inside its bound, both ways ────────────────────
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        const Level& L = kLevels[lvl - 1];
        State w; w.reset(lvl, 555 * lvl);
        int32_t lo = 0, hi = 0;
        for (int i = 0; i < 1000000; i++) {
            bool freeze = false;
            w.pulse(BIT1, true, freeze);
            if (w.wow < lo) lo = w.wow;
            if (w.wow > hi) hi = w.wow;
        }
        check(lo >= -L.wowMax && hi <= L.wowMax, "the wow walk left its bound");
        check(hi > L.wowMax / 2 && lo < -L.wowMax / 2, "the wow walk never moves");
    }

    // ── Faults come at about the configured rate, and last the right time ─────
    for (uint8_t lvl = 1; lvl <= 3; lvl++) {
        const Level& L = kLevels[lvl - 1];
        State w; w.reset(lvl, 31337 + lvl);
        // 40 mean intervals — of the MIXTURE, since a clean stretch is an interval
        // too and at Medium it is 40% of them.
        const double meanShort = (double)L.evtMin + L.evtSpan / 2.0;
        const double meanClean = (double)CLEAN_MIN_T + CLEAN_SPAN_T / 2.0;
        const double p = L.cleanPct / 100.0;
        const uint64_t tapeT = (uint64_t)((p * meanClean + (1 - p) * meanShort) * 40);
        uint64_t played = 0;
        int faults = 0;
        uint8_t was = FAULT_NONE;
        bool durOk = true, mulOk = true;
        while (played < tapeT) {
            bool freeze = false;
            played += w.pulse(BIT0, true, freeze);
            if (w.evtKind && !was) {
                faults++;
                if (w.evtLeft < L.durMin || w.evtLeft >= L.durMin + L.durSpan) durOk = false;
                // A scheduled lurch must actually stretch: 1.5x-3.0x. (The per-kind
                // test below sets evtMul by hand, so this is the only place the
                // scheduler's own choice is checked.)
                if (w.evtKind == FAULT_LURCH && (w.evtMul < 384 || w.evtMul > 768)) mulOk = false;
            }
            was = w.evtKind;
        }
        check(faults > 20 && faults < 80, "the fault rate is off");
        check(durOk, "a fault length left [durMin, durMin+durSpan)");
        check(mulOk, "a scheduled lurch does not stretch by 1.5x-3.0x");
    }

    // ── Nothing is damaged where nothing is recorded ──────────────────────────
    {
        State w; w.reset(3, 5150);
        for (int i = 0; i < 400000; i++) {
            bool freeze = false;
            w.pulse(BIT0, false, freeze);      // a pause/tail, every pulse
            if (w.evtKind) { check(false, "a fault started in a pause"); break; }
        }
    }

    // ── Off loads every real block, byte for byte, through the 48K ROM ────────
    // This is also what calibrates the ROM model: if it cannot load a CLEAN tape,
    // nothing it says about a worn one means anything.
    {
        check(rate(0,    19, PILOT_HDR, 20, 1) == 100, "level 0 fails to load a header");
        check(rate(0,   200, PILOT_DAT, 20, 2) == 100, "level 0 fails to load a loader block");
        check(rate(0,  1500, PILOT_DAT, 20, 3) == 100, "level 0 fails to load a code block");
        check(rate(0,  6912, PILOT_DAT, 10, 4) == 100, "level 0 fails to load a screen");
        check(rate(0, 35000, PILOT_DAT,  5, 5) == 100, "level 0 fails to load a whole game");
    }

    // ── The wow ALONE never corrupts a byte ───────────────────────────────────
    //
    // Heavy's ±16% keeps a 0 bit's pair at 2x855x1.16 = 1984 and a 1 bit's at
    // 2x1710x0.84 = 2873, against LD-8-BITS' ~2287 T threshold. That is the
    // property that makes the warble a SOUND and not a failure, so it is asserted
    // rather than assumed; the fault is held off so this is the wow by itself.
    {
        const std::vector<uint8_t> blk = makeBlock(400, 9);
        int bad = 0;
        for (uint32_t seed = 1; seed <= 40; seed++) {
            TapeSim t; t.w.reset(3, seed * 2654435761u);
            t.pinSchedule = true;
            if (!loadBlock(t, blk, PILOT_DAT)) bad++;
            check(t.faults == 0, "the wow-only run drew a fault after all");
        }
        check(bad == 0, "the wow alone corrupted a block");
    }

    // ── ...and EACH KIND of fault breaks the DATA on its own ──────────────────
    //
    // Per kind and injected by hand, because a run of Heavy draws both: with the two
    // mixed, neutering either one still leaves the other to fail the block — which
    // is a suite that cannot fail. The control run (same seed, no fault) has to
    // load, or the test would be proving nothing about the fault.
    {
        for (int kind = FAULT_DROP; kind <= FAULT_LURCH; kind++) {
            int broke = 0, controlOk = 0;
            for (uint32_t seed = 1; seed <= 40; seed++) {
                for (int inject = 0; inject < 2; inject++) {
                    const std::vector<uint8_t> blk = makeBlock(200, seed);
                    TapeSim t; t.w.reset(3, seed * 69069u + 1);
                    t.pinSchedule = true;
                    t.begin(blk, PILOT_DAT);
                    int pulses = 0;
                    while (t.phase != PH_END) {
                        // Well past the pilot, inside the data.
                        if (inject && pulses == PILOT_DAT + 400) {
                            t.w.evtKind = (uint8_t)kind;
                            t.w.evtLeft = 10000;        // ~2.9 ms
                            t.w.evtMul  = 512;          // 2x, for the lurch
                        }
                        pulses++;
                        if (!t.step()) break;
                    }
                    Rom48 rom(t.edges);
                    std::vector<uint8_t> got;
                    const bool ok = rom.ldBytes(got, blk.size()) &&
                                    got.size() == blk.size() &&
                                    memcmp(got.data(), blk.data(), blk.size()) == 0;
                    if (inject) { if (!ok) broke++; } else if (ok) controlOk++;
                }
            }
            check(controlOk == 40, "the control run of the per-kind test does not load");
            // A dropout always costs bits. A lurch only breaks a 0 bit (stretched
            // past the threshold it reads as a 1) — over a run of 1s it is
            // inaudible to the loader, which is right, so it is a handful of the
            // seeds and not none of them.
            if (kind == FAULT_DROP) check(broke == 40, "a dropout let a block through intact");
            else                    check(broke >= 30, "a lurch let too many blocks through intact");
        }
    }

    // ── The levels mean what the table says they mean ─────────────────────────
    //
    // The GAME rates are a requirement of their own (owner, 2026-09-20: Medium 40%,
    // Heavy 10%) — a level that can never load a game is a dead end, not a worn
    // tape. They come from Level::cleanPct, so they are asserted tightly; the rest
    // carry margins because they are sampled.
    {
        const int hdrL = rate(1,    19, PILOT_HDR, 40, 11);
        const int l200 = rate(1,   200, PILOT_DAT, 40, 12);
        const int c15L = rate(1,  1500, PILOT_DAT, 40, 13);
        const int scrL = rate(1,  6912, PILOT_DAT, 40, 14);
        const int gamL = rate(1, 35000, PILOT_DAT, 60, 15);
        check(hdrL == 100 && l200 == 100, "Light breaks a header or a loader block");
        check(c15L >= 95, "Light breaks a 1.5 KB block too often");
        check(scrL >= 85, "Light breaks a screen too often");
        check(gamL >= 60, "Light breaks a whole game too often");

        const int l2M  = rate(2,   200, PILOT_DAT, 40, 21);
        const int scrM = rate(2,  6912, PILOT_DAT, 40, 22);
        const int gamM = rate(2, 35000, PILOT_DAT, 200, 23);
        check(l2M >= 95, "Medium breaks a small loader block");
        check(scrM >= 55 && scrM <= 95, "Medium's screen rate left 55..95%");
        check(gamM >= 30 && gamM <= 50, "Medium's GAME rate left 30..50% (wanted 40)");

        const int hdrH = rate(3,    19, PILOT_HDR, 40, 31);
        const int l2H  = rate(3,   200, PILOT_DAT, 40, 32);
        const int c15H = rate(3,  1500, PILOT_DAT, 40, 33);
        const int scrH = rate(3,  6912, PILOT_DAT, 60, 34);
        const int gamH = rate(3, 35000, PILOT_DAT, 200, 35);
        // Heavy still gets you the name off the header and then dies — the one
        // everybody remembers. A pilot fault is survivable, which is what makes it.
        check(hdrH >= 70, "Heavy cannot even read a header");
        check(l2H  >= 50, "Heavy cannot read a 200-byte loader");
        check(c15H <= 35, "Heavy loads a 1.5 KB block too often");
        check(scrH <= 25, "Heavy loads a screen too often");
        check(gamH >= 4 && gamH <= 18, "Heavy's GAME rate left 4..18% (wanted 10)");
    }

    // ── A level change re-arms rather than carrying the old schedule ──────────
    {
        State w; w.reset(1, 8);
        // pulseFedback, so the memo is really populated before the change — with
        // plain pulse() it stays zero and "arm() clears it" cannot fail.
        uint32_t slot = BIT0;
        for (int i = 0; i < 10000; i++) { bool f = false; slot = w.pulseFedback(slot, true, f); }
        check(w.nominal != 0 && w.delivered != 0, "the feedback memo was never populated");
        const int64_t before = w.evtNext;
        check(w.sync(3), "sync(3) reported the tape as unworn");
        check(w.level == 3, "sync did not take the new level");
        check(w.evtNext != before, "a level change kept the old schedule");
        check(w.nominal == 0 && w.delivered == 0, "a level change kept the feedback memo");
        check(!w.sync(0), "sync(0) reported the tape as worn");
        bool f = false;
        check(w.pulse(BIT0, true, f) == BIT0, "level 0 after a change still perturbs");
    }

    // ── The WAV/MP3 path: dropouts on elapsed time ────────────────────────────
    {
        State w; w.reset(3, 4711);
        check(!w.audio(0), "an idle audio tick reported a fault");
        int faults = 0;
        uint8_t was = FAULT_NONE;
        // Long enough to be certain at Heavy: its interval is uniform over
        // [1.2 s, 10.8 s], so a 9 s window is a coin toss, not a test.
        for (int i = 0; i < 3000000; i++) {         // 3000000 x 79 T ~ 68 s
            w.audio(79);
            if (w.evtKind && !was) faults++;
            was = w.evtKind;
        }
        check(faults > 0, "the audio path never drops out");
        State off; off.reset(0, 1);
        check(!off.audio(79), "level 0 drops out on the audio path");
        // A gap longer than the cap (the menu held the machine) must not count.
        State g; g.reset(3, 22);
        const int64_t evt = g.evtNext;
        g.audio(MAX_PULSE_T + 1);
        check(g.evtNext == evt, "a long audio gap spent the fault budget");
    }

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("tapewear_test: OK\n");
    return 0;
}
