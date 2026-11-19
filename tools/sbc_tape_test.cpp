// Host test: does a tape signal survive Bluetooth A2DP (SBC)?  — docs/bt-tape-plan.md
//
// The question behind "BT tape recorder" (the board as an A2DP sink, a phone plays
// the tape's audio): the phone SBC-encodes it, the board decodes it and turns the
// sign of the signal into the EAR bit. This runs that path off-line:
//
//   TAP-style pulse train (ROM timings, optional turbo scale)
//   -> 44.1 kHz PCM, a +-A square wave rendered by exact area (what tap2wav emits)
//   -> bluedroid SBC encoder   (Android's; the phone side)
//   -> bluedroid SBC decoder   (BTstack's; what the board would run)
//   -> Schmitt trigger, edge time interpolated to the zero crossing (or not: "q")
//   -> edges in T-states, compared pulse by pulse with the original.
//
// The same path without SBC is the baseline row, so what SBC adds is the
// difference. A single extra or missing edge shifts every later pulse, so
// bitErr jumps to ~all bits: read it as "this block fails", not as a BER.
//
// Build: the codec is the bluedroid copy inside BTstack (pico-sdk lib/btstack,
// i.e. PICO_BTSTACK_PATH; or a bluekitchen/btstack checkout):
//
//   B=$PICO_SDK_PATH/lib/btstack/3rd-party/bluedroid; O=/tmp/sbcobj; mkdir -p $O
//   (cd $O && gcc -O2 -w -c -I$B/decoder/include $B/decoder/srce/*.c)
//   (cd $O && gcc -O2 -w -c -I$B/encoder/include $B/encoder/srce/*.c)
//   g++ -O2 -Wall -Isrc -I$B/decoder/include -I$B/encoder/include -o /tmp/sbc_tape_test tools/sbc_tape_test.cpp $O/*.o
//   /tmp/sbc_tape_test         (~12 s)
//
// Options: --detail (per-config table: max/mean half-pulse error, interpolated
// and sample-quantized edges), --dbg (print where the first edge goes wrong).
// Exit code 0 = the expectations at the bottom of main() hold.
//
// Findings (2026-11-18, bluedroid both ends, 44.1 kHz, 16 blocks, 8 subbands,
// loudness): see docs/bt-tape-plan.md "Host test". Short form: bitpool >= 41
// is clean at every level up to 0 dBFS and every speed up to 4x; lower bitpools
// fail only near full scale (an int16 wrap inside the codec -> +-FS chatter in
// the 3-4 samples before a real edge).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

#include "speccy/devices/tape/BtTapeEdges.h"   // the firmware's receiver (BtHost)

extern "C" {
#include "oi_codec_sbc.h"
#include "sbc_encoder.h"
}

static const double TSTATES = 3500000.0;
static const int    RATE    = 44100;
static bool g_dbg = false;

// ── Pulse train ───────────────────────────────────────────────────────────────
struct Train { std::vector<int> half; std::vector<int> bitStart; std::vector<uint8_t> bits; };

static void addBlock(Train& t, const std::vector<uint8_t>& data, double s) {
    int pilotLen = data[0] < 0x80 ? 8063 : 3223;
    for (int i = 0; i < pilotLen; i++) t.half.push_back(int(2168 * s));
    t.half.push_back(int(667 * s));
    t.half.push_back(int(735 * s));
    for (uint8_t b : data)
        for (int k = 7; k >= 0; k--) {
            int bit = (b >> k) & 1;
            t.bitStart.push_back((int)t.half.size());
            t.bits.push_back(bit);
            int len = int((bit ? 1710 : 855) * s);
            t.half.push_back(len); t.half.push_back(len);
        }
    t.half.push_back(int(TSTATES * 0.5));  // pause: level held 0.5 s
}

// Header + a 6912-byte random data block; s < 1 = turbo (0.25 = 4x).
static Train makeTrain(double s, unsigned seed) {
    Train t;
    srand(seed);
    std::vector<uint8_t> hdr(19), dat(6914);
    hdr[0] = 0x00; for (int i = 1; i < 19; i++) hdr[i] = rand() & 0xFF;
    dat[0] = 0xFF; for (size_t i = 1; i < dat.size(); i++) dat[i] = rand() & 0xFF;
    t.half.push_back(int(TSTATES * 0.3));  // lead-in silence
    addBlock(t, hdr, s);
    addBlock(t, dat, s);
    return t;
}

// ── Render: exact area of a +-A square wave over each sample period ─────────────
static std::vector<float> render(const Train& t, double amp) {
    std::vector<double> edge;  // seconds
    double now = 0;
    for (int h : t.half) { now += h / TSTATES; edge.push_back(now); }
    size_t n = size_t(now * RATE) + RATE / 10;
    std::vector<float> out(n);
    double level = -1; size_t e = 0;
    for (size_t i = 0; i < n; i++) {
        double a = double(i) / RATE, b = double(i + 1) / RATE, acc = 0, p = a;
        while (e < edge.size() && edge[e] < b) { acc += level * (edge[e] - p); p = edge[e]; level = -level; e++; }
        acc += level * (b - p);
        out[i] = float(amp * acc * RATE);
    }
    return out;
}

// ── SBC round trip (the same mono signal on both channels, like a phone) ───────
struct SbcCfg { const char* name; int mode; int bitpool; };

static std::vector<float> sbcRoundTrip(const std::vector<float>& in, const SbcCfg& c) {
    SBC_ENC_PARAMS enc; memset(&enc, 0, sizeof enc);
    static uint8_t packet[1024];
    enc.s16NumOfBlocks = 16; enc.s16NumOfSubBands = 8;
    enc.s16AllocationMethod = SBC_LOUDNESS; enc.s16BitPool = c.bitpool;
    enc.s16ChannelMode = c.mode;
    enc.s16SamplingFreq = SBC_sf44100;
    enc.pu8Packet = packet;
    SBC_Encoder_Init(&enc);
    const int ch = enc.s16NumOfChannels, fr = 16 * 8;

    static OI_CODEC_SBC_DECODER_CONTEXT dctx;
    static OI_UINT32 ddata[8192];
    OI_CODEC_SBC_DecoderReset(&dctx, ddata, sizeof ddata, 2, 2, FALSE);

    std::vector<float> out; out.reserve(in.size());
    std::vector<int16_t> pcm(fr * ch);
    int16_t dec[2 * 8 * 16];
    for (size_t i = 0; i + fr <= in.size(); i += fr) {
        for (int k = 0; k < fr; k++) {
            float v = in[i + k] * 32767.0f;
            int16_t s = (int16_t)std::max(-32768.0f, std::min(32767.0f, v));
            for (int c2 = 0; c2 < ch; c2++) pcm[k * ch + c2] = s;
        }
        enc.ps16PcmBuffer = pcm.data();
        SBC_Encoder(&enc);
        const OI_BYTE* p = packet; OI_UINT32 len = enc.u16PacketLength, outBytes = sizeof dec;
        OI_STATUS st = OI_CODEC_SBC_DecodeFrame(&dctx, &p, &len, dec, &outBytes);
        if (st != OI_STATUS_SUCCESS) { fprintf(stderr, "decode status %d\n", st); exit(2); }
        int got = outBytes / 2 / 2;                       // pcmStride 2: L R L R
        for (int k = 0; k < got; k++) out.push_back(dec[2 * k] / 32768.0f);
    }
    return out;
}

static std::vector<float> through(const std::vector<float>& pcm, const SbcCfg& c) {
    return c.mode < 0 ? pcm : sbcRoundTrip(pcm, c);
}

// ── Receiver: Schmitt trigger at +-hyst, edge at the interpolated zero crossing ─
// The first 10 ms are skipped: the decoder's synthesis filter starts from zero and
// rings on its way to the first level (2 false edges). A real stream starts in
// silence, so on the board this is the "ignore until the level settles" rule.
static std::vector<double> edges(const std::vector<float>& x, double hyst, bool interp) {
    std::vector<double> e;
    const size_t skip = RATE / 100;
    if (x.size() <= skip) return e;
    int level = x[skip] > 0 ? 1 : -1;
    for (size_t i = skip; i < x.size(); i++) {
        float v = x[i];
        if ((level < 0 && v > hyst) || (level > 0 && v < -hyst)) {
            level = -level;
            double t = double(i);
            if (interp) {
                size_t j = i;
                while (j > 0 && ((level > 0) ? x[j - 1] > 0 : x[j - 1] < 0)) j--;
                if (j > 0) { double a = x[j - 1], b = x[j]; t = (j - 1) + a / (a - b); }
            }
            e.push_back(t / RATE * TSTATES);
        }
    }
    return e;
}

// The FIRMWARE receiver: src/speccy/devices/tape/BtTapeEdges.h on int16 samples
// (what the decoder hands BtHost), its Q8 sample times turned into T-states.
static std::vector<double> edgesFw(const std::vector<float>& x) {
    bttape::EdgeExtractor ex; ex.reset();
    std::vector<double> e;
    uint32_t base = 0, last = 0; bool any = false;   // undo the 2^24-sample wrap
    for (float v : x) {
        float f = v * 32768.0f;
        int16_t s = (int16_t)std::max(-32768.0f, std::min(32767.0f, f));
        uint32_t t;
        if (!ex.push(s, &t)) continue;
        t &= ~1u;
        if (any && t < last) base++;
        last = t; any = true;
        e.push_back(((double)base * 4294967296.0 + t) / 256.0 / RATE * TSTATES);
    }
    return e;
}

struct Result { long edgeDiff; int maxHalfErr; int maxBitErr; long bitErrors; double meanAbsHalf; };

static Result compare(const Train& t, const std::vector<double>& e, double s) {
    Result r{}; r.edgeDiff = (long)e.size() - (long)t.half.size();
    std::vector<double> o; double now = 0;
    for (int h : t.half) { now += h; o.push_back(now); }
    size_t n = std::min(o.size(), e.size());
    double sumAbs = 0; long nh = 0;
    for (size_t i = 1; i < n; i++) {
        if (t.half[i] > 100000 || t.half[i - 1] > 100000) continue;   // pause edges
        double he = (e[i] - e[i - 1]) - (o[i] - o[i - 1]);
        r.maxHalfErr = std::max(r.maxHalfErr, int(fabs(he)));
        sumAbs += fabs(he); nh++;
    }
    r.meanAbsHalf = nh ? sumAbs / nh : 0;
    const double thr = 2565 * s;                  // between bit0 (1710) and bit1 (3420) periods
    for (size_t k = 0; k < t.bitStart.size(); k++) {
        size_t i = t.bitStart[k];
        if (i + 1 >= n || i == 0) { r.bitErrors++; continue; }
        double per = e[i + 1] - e[i - 1];
        double ideal = double(t.half[i] + t.half[i + 1]);
        r.maxBitErr = std::max(r.maxBitErr, int(fabs(per - ideal)));
        if ((per > thr) != (t.bits[k] != 0)) r.bitErrors++;
    }
    if (g_dbg && n) {
        double off = 0; size_t m = std::min<size_t>(n, 200);
        for (size_t i = 0; i < m; i++) off += e[i] - o[i];
        off /= m;
        for (size_t i = 0; i < n; i++) if (fabs((e[i] - o[i]) - off) > 300) {
            fprintf(stderr, "first bad edge %zu: orig %.0f got %.0f (latency %.0f)\n", i, o[i], e[i], off);
            break;
        }
    }
    return r;
}

static const double SPEEDS[] = {1.0, 0.5, 0.33, 0.25};   // 1x 2x 3x 4x

// ── The live link: EdgeExtractor + EdgePlayer as BtHost / Tape_BT run them ──────
// Packets of 1024 samples (8 SBC frames) leave the phone on ITS clock (fs * (1 +
// phoneDrift)) and arrive late by 0..jitterMs, in order, with a 150 ms stall every
// ~3 s (the radio busy). The emulator's T clock runs at 3.5 MHz * (1 + emuDrift)
// of wall time, ends a frame every 20 ms (EdgePlayer::frame) and polls EAR every
// 60 T like LD-EDGE (EdgePlayer::level). Out: the edges the loader sees, in T.
struct LinkCfg { double phoneDrift, emuDrift; int jitterMs; };
struct LinkStats { uint32_t underflows, recenters, blocks; int32_t trimMin, trimMax; };

static std::vector<double> edgesLive(const std::vector<float>& y, const LinkCfg& L, LinkStats& st) {
    const int P = 1024;
    srand(4321);
    // Producer side: run the extractor packet by packet; note each packet's
    // arrival time (wall seconds) and the edges + sample clock it brings.
    struct Pkt { double arrive; uint32_t prodQ8; std::vector<uint32_t> edges; };
    std::vector<Pkt> pk;
    bttape::EdgeExtractor ex; ex.reset();
    double lastArrive = 0;
    for (size_t i = 0; i + P <= y.size(); i += P) {
        Pkt k;
        for (int j = 0; j < P; j++) {
            float f = y[i + j] * 32768.0f;
            int16_t v = (int16_t)std::max(-32768.0f, std::min(32767.0f, f));
            uint32_t e;
            if (ex.push(v, &e)) k.edges.push_back(e);
        }
        k.prodQ8 = ex.samples() << 8;
        const double sent = double(i + P) / (RATE * (1 + L.phoneDrift));
        double a = sent + (L.jitterMs ? (rand() % (L.jitterMs + 1)) / 1000.0 : 0);
        if (L.jitterMs && (i / P) % 130 == 129) a += 0.150;          // a radio stall
        if (a < lastArrive) a = lastArrive;                          // in order
        lastArrive = a;
        k.arrive = a;
        pk.push_back(std::move(k));
    }
    // Consumer side.
    std::vector<uint32_t> ring; size_t rr = 0;
    auto pop = [&](uint32_t& e) -> bool { if (rr >= ring.size()) return false; e = ring[rr++]; return true; };
    bttape::EdgePlayer pl;
    std::vector<double> out;
    size_t next = 0;
    uint32_t prodQ8 = 0;
    bool started = false;
    uint8_t lvl = 0; bool haveLvl = false;
    const double tPerWall = 3500000.0 * (1 + L.emuDrift);
    st = {0, 0, 0, 0, 0};
    const double end = lastArrive + 1.0;
    for (double wall = 0; wall < end; wall += 0.020) {                 // frames
        while (next < pk.size() && pk[next].arrive <= wall) {
            for (uint32_t e : pk[next].edges) ring.push_back(e);
            prodQ8 = pk[next].prodQ8; next++;
        }
        // Underflows count while the phone still sends: running dry after its
        // last packet is the end of the tape, not a fault.
        if (next < pk.size()) st.underflows = pl.underflows;
        const uint64_t T0 = (uint64_t)(wall * tPerWall);
        if (!started) {
            if (!prodQ8) continue;
            pl.start(RATE, T0, prodQ8); started = true;
        }
        pl.frame(T0, prodQ8, pop);
        st.trimMin = std::min(st.trimMin, pl.trimPpm()); st.trimMax = std::max(st.trimMax, pl.trimPpm());
        const uint64_t T1 = (uint64_t)((wall + 0.020) * tPerWall);
        for (uint64_t T = T0; T < T1; T += 60) {
            uint8_t l = pl.level(T, pop);
            if (!haveLvl) { lvl = l; haveLvl = true; continue; }
            if (l != lvl) { lvl = l; out.push_back((double)T / (1 + L.emuDrift)); }  // machine T
        }
    }
    st.recenters = pl.recenters;
    st.blocks = pl.blocks;
    if (g_dbg) fprintf(stderr, "live: ring %zu popped %zu played %u fill %d ms, out %zu\n",
                       ring.size(), rr, pl.played, pl.fillMs(), out.size());
    return out;
}

static void detail() {
    SbcCfg cfgs[] = {
        {"PCM (no BT)", -1, 0},
        {"JS bp53", SBC_JOINT_STEREO, 53},
        {"JS bp35", SBC_JOINT_STEREO, 35},
        {"JS bp19", SBC_JOINT_STEREO, 19},
        {"Mono bp31", SBC_MONO, 31},
    };
    printf("i = edge interpolated to the zero crossing, q = sample-quantized\n");
    printf("%-12s %5s %5s   | %6s %8s %8s %9s %8s\n", "codec", "speed", "amp", "edges", "maxHalf", "meanHalf", "maxBitPer", "bitErr");
    for (double s : SPEEDS)
        for (double a : {1.0, 0.25, 0.05}) {
            Train t = makeTrain(s, 1234);
            std::vector<float> pcm = render(t, a);
            for (auto& c : cfgs) {
                std::vector<float> y = through(pcm, c);
                for (int interp = 1; interp >= 0; interp--) {
                    Result r = compare(t, edges(y, a * 0.15, interp), s);
                    printf("%-12s %4.2fx %5.2f %s | %+6ld %8d %8.1f %9d %8ld\n", c.name, 1 / s, a,
                           interp ? "i" : "q", r.edgeDiff, r.maxHalfErr, r.meanAbsHalf, r.maxBitErr, r.bitErrors);
                }
            }
            printf("\n");
        }
}

int main(int argc, char** argv) {
    bool det = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--detail")) det = true;
        else if (!strcmp(argv[i], "--dbg")) g_dbg = true;
        else { fprintf(stderr, "usage: %s [--detail] [--dbg]\n", argv[0]); return 2; }
    }
    if (det) { detail(); return 0; }

    // Grid: worst bitErr over 1x..4x, per bitpool and level; plus the worst
    // half-pulse error (interpolated edges) where the block loaded.
    const double amps[] = {1.0, 0.9, 0.8, 0.7, 0.5, 0.25, 0.1, 0.03};
    const int    bps[]  = {0, 19, 27, 31, 35, 41, 47, 53};   // 0 = PCM baseline
    const int NA = sizeof amps / sizeof amps[0], NB = sizeof bps / sizeof bps[0];
    long worstBits[NB][NA]; int worstHalf[NB];
    std::vector<Train> trains; std::vector<std::vector<float>> pcms[NA];
    for (double s : SPEEDS) trains.push_back(makeTrain(s, 99));
    for (int ai = 0; ai < NA; ai++) for (auto& t : trains) pcms[ai].push_back(render(t, amps[ai]));

    printf("worst bitErr over 1x/2x/3x/4x (0 = every block loads); maxHalf = worst half-pulse error, T\n");
    printf("level   ");
    for (double a : amps) printf(" %6.2f", a);
    printf(" | maxHalf\n");
    for (int bi = 0; bi < NB; bi++) {
        SbcCfg c = {"", bps[bi] ? SBC_JOINT_STEREO : -1, bps[bi]};
        worstHalf[bi] = 0;
        if (bps[bi]) printf("JS bp%2d ", bps[bi]); else printf("PCM     ");
        for (int ai = 0; ai < NA; ai++) {
            long b = 0;
            for (size_t si = 0; si < trains.size(); si++) {
                Result r = compare(trains[si], edges(through(pcms[ai][si], c), amps[ai] * 0.15, true), SPEEDS[si]);
                b = std::max(b, r.bitErrors);
                if (!r.bitErrors && !r.edgeDiff) worstHalf[bi] = std::max(worstHalf[bi], r.maxHalfErr);
            }
            worstBits[bi][ai] = b;
            printf(" %6ld", b);
        }
        printf(" | %5d\n", worstHalf[bi]);
    }

    // The firmware receiver (BtTapeEdges.h) on the same signals.
    printf("\nfirmware receiver (BtTapeEdges.h), same grid:\n");
    long fwBits[NB][NA]; int fwHalf[NB];
    for (int bi = 0; bi < NB; bi++) {
        SbcCfg c = {"", bps[bi] ? SBC_JOINT_STEREO : -1, bps[bi]};
        fwHalf[bi] = 0;
        if (bps[bi]) printf("JS bp%2d ", bps[bi]); else printf("PCM     ");
        for (int ai = 0; ai < NA; ai++) {
            long b = 0;
            for (size_t si = 0; si < trains.size(); si++) {
                Result r = compare(trains[si], edgesFw(through(pcms[ai][si], c)), SPEEDS[si]);
                b = std::max(b, r.bitErrors);
                if (!r.bitErrors && !r.edgeDiff) fwHalf[bi] = std::max(fwHalf[bi], r.maxHalfErr);
            }
            fwBits[bi][ai] = b;
            printf(" %6ld", b);
        }
        printf(" | %5d\n", fwHalf[bi]);
    }

    // The live link (EdgePlayer) at bitpool 53, 0 dBFS.
    printf("\nlive link (BtTapeEdges.h EdgePlayer), bitpool 53, level 1.0; cell = bitErr+edgeDiff/underflows\n");
    const LinkCfg links[] = {
        {0, 0, 0}, {0.0005, -0.0005, 60}, {0.02, 0, 60}, {0, -0.02, 60}, {-0.015, 0.015, 60},
    };
    int linkFails = 0;
    printf("phone   emu  jit |");
    for (double s : SPEEDS) printf("   %4.2fx", 1 / s);
    printf(" | trim ppm       recenters\n");
    for (const LinkCfg& L : links) {
        printf("%+5.2f%% %+5.2f%% %2d |", L.phoneDrift * 100, L.emuDrift * 100, L.jitterMs);
        int32_t tmin = 0, tmax = 0; uint32_t rec = 0;
        for (size_t si = 0; si < trains.size(); si++) {
            SbcCfg c = {"", SBC_JOINT_STEREO, 53};
            LinkStats st;
            // pcms[0] = level 1.0
            std::vector<double> e = edgesLive(through(pcms[0][si], c), L, st);
            Result r = compare(trains[si], e, SPEEDS[si]);
            printf(" %5ld/%u", r.bitErrors + labs(r.edgeDiff), st.underflows);
            // No lost bit or edge anywhere; no underflow up to a 2 % clock gap
            // (the 3 % row is the stress case: an underflow there may land in a
            // pause, which costs nothing).
            const bool stress = fabs(L.phoneDrift - L.emuDrift) > 0.021;
            if (r.bitErrors || r.edgeDiff || (st.underflows && !stress)) linkFails++;
            if (st.blocks != 2) { printf(" [blocks %u, want 2]", st.blocks); linkFails++; }   // header + data
            tmin = std::min(tmin, st.trimMin); tmax = std::max(tmax, st.trimMax); rec += st.recenters;
        }
        printf(" | %+6d..%+6d %4u\n", tmin, tmax, rec);
    }

    // Expectations (what docs/bt-tape-plan.md relies on).
    int fails = 0;
    for (int bi = 0; bi < NB; bi++) {
        bool mustBeClean = bps[bi] == 0 || bps[bi] >= 41;          // the sink's bitpool floor
        for (int ai = 0; ai < NA; ai++) {
            bool clean = worstBits[bi][ai] == 0;
            if (mustBeClean && !clean) { printf("FAIL: bp%d level %.2f must load\n", bps[bi], amps[ai]); fails++; }
            if (bps[bi] && amps[ai] <= 0.7 && !clean) { printf("FAIL: bp%d level %.2f (<= -3 dB) must load\n", bps[bi], amps[ai]); fails++; }
        }
        // 4x margin is ~214 T; keep SBC's timing error far below it.
        if ((bps[bi] == 0 || bps[bi] >= 41) && worstHalf[bi] > 60) { printf("FAIL: bp%d half-pulse error %d T\n", bps[bi], worstHalf[bi]); fails++; }
    }
    for (int bi = 0; bi < NB; bi++) {
        if (bps[bi] != 0 && bps[bi] < 41) continue;
        for (int ai = 0; ai < NA; ai++)
            if (fwBits[bi][ai]) { printf("FAIL: firmware receiver, bp%d level %.2f must load\n", bps[bi], amps[ai]); fails++; }
        if (fwHalf[bi] > 60) { printf("FAIL: firmware receiver bp%d half-pulse error %d T\n", bps[bi], fwHalf[bi]); fails++; }
    }
    if (linkFails) { printf("FAIL: live link, %d case(s) lost bits, edges or underflowed\n", linkFails); fails += linkFails; }
    // The known failure must still show up, or the harness is not looking.
    if (worstBits[1][0] == 0) { printf("FAIL: bp19 at 0 dBFS loaded — harness blind?\n"); fails++; }
    printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
