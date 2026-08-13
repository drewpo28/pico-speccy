#include "NgsMp3.h"
#include "../Buffer.h"
#include "NgsSd.h"
#include "../Debug.h"

#include "pico.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

// Helix (drivers/picomp3lib), not minimp3. Fixed point, no float and no huge
// stack frame, and — the reason for the switch — its short-buffer behaviour is
// benign: it answers ERR_MP3_INDATA_UNDERFLOW and leaves the stream alone,
// where minimp3 wiped its own state, declared the whole window consumed and
// could never resynchronise (hw 2026-08-06; that trap is what MP3_DECODE_MIN
// below existed to dodge). minimp3.h stays in the tree, unused, with its
// external-scratch patch intact.
extern "C" {
#include "mp3dec.h"
}

extern size_t getFreeHeap(void);                // OSDMain.cpp — reported at alloc time
extern "C" size_t getLargestAllocatable(void);  // OSDMain.cpp — decides the arena's tier

// Helix allocates its eight state structures (23.9 KB total) through this hook
// at MP3InitDecoder() time and NEVER frees them — buffers.c leaves MPDEC_FREE
// undefined, so its SAFE_FREE only nulls the pointer. That makes a bump
// allocator over a Buffer exactly right: we pick the tier and can hand the whole
// thing back in one call.
//
// TWO arenas, and the split is the point. SRAM is where this state belongs — it is
// the decoder's hot working set, and in butter PSRAM it shares the XIP cache with
// core1's GS opcode fetches, which cost NPL its smooth playback and the emulator
// its FPS (hw 2026-08-13). But at 720x576 the framebuffer takes 104 KB and the
// runtime heap is ~43 KB with a few KB of fragmentation, so ONE 24 KB SRAM block
// is often unreachable while 12-20 KB is there for the taking. So: fill an SRAM
// arena as large as the heap will give (largest first, stepping down), and send
// the overflow to butter.
//
// The order Helix asks in decides who wins, and it is already the right order —
// buffers.c requests MP3DecInfo, then HuffmanInfo (4624), SubbandInfo (8712),
// IMDCTInfo (6944), and only then the four small ones (DequantInfo, SideInfo,
// ScaleFactorInfo, FrameHeader, ~3.3 KB together). Greedy "SRAM until full" thus
// keeps the per-sample polyphase/IMDCT/Huffman buffers in SRAM and pushes the
// cold per-frame headers out, with no per-structure knowledge here.
#define HELIX_ARENA_SIZE 24576
// SRAM sizes to try, largest first. 12 KB is the floor worth having: below that the
// three hot structures cannot fit anyway and the whole arena may as well be butter.
static const uint32_t kHelixSramTry[] = { 24576, 20480, 16384, 12288 };
static Buffer   s_helixBuf;       // SRAM (heap) part — may be empty
static Buffer   s_helixPsBuf;     // butter part — holds whatever did not fit
static uint8_t* s_helix_arena = nullptr;   // SRAM base (also the "carving" marker)
static uint32_t s_helix_cap   = 0;         // SRAM capacity
static uint32_t s_helix_used  = 0;         // SRAM bytes handed out
static uint8_t* s_helix_ps    = nullptr;   // butter base
static uint32_t s_helix_ps_cap  = 0;
static uint32_t s_helix_ps_used = 0;

// ── Lazy allocation ──────────────────────────────────────────────────────────
// init() used to run from GS::init for every NeoGS boot, so ~24 KB of SRAM heap
// (this arena) and ~33 KB of butter went to the decoder on every session — while
// almost nothing on a NeoGS card plays MP3 (NPL and the Z-Players do; demos,
// trackers and every FH1-style loader never touch MD_SEND). At 720x576 that SRAM
// is the difference between booting and not: the main framebuffer needs one
// contiguous 104 040 B block and setup() OOM-panicked in VIDEO::Init with 149 KB
// free (hw 2026-08-13, 576p + NeoGS + MIDI). So the decoder is now allocated on
// the first MD_SEND byte instead.
//
// core1 (mdSend) only raises a flag; core0's service() does the allocation, one
// attempt — a failure means stub mode for the rest of the session, exactly as
// before. Bytes that arrive before core0 gets there are DROPPED and mddrq() keeps
// answering 1: that is the established stub contract (a 0 wedges NPL's init poll,
// see GS.cpp SSTAT), and the cost is under a frame of stream head, which Helix
// resynchronises past on the next sync word.
// The arena is given BACK when the stream stops, and that is not a nicety: it is
// what lets the SRAM attempt run with Buffer's small HOT_SRAM margin. Holding 24 KB
// of a ~43 KB runtime heap (720x576 + NeoGS + MIDI) for a whole session would leave
// the OSD/browser/ZIP paths too thin; holding it only while a player is actually
// streaming costs them nothing, because those paths are not in use then. Freeing
// crosses cores, so it is two-phase: core0 drops `s_st` (every core1 entry point
// tests it first), then waits GRACE frames before releasing the memory, by which
// time a core1 call that had already loaded the old pointer is long finished.
#define MP3_IDLE_RELEASE_FRAMES 1500   // ~30 s at 50 Hz — long enough that NPL's
                                       // multi-second between-track SD loads, and a
                                       // paused track, never churn the allocation
#define MP3_FREE_GRACE_FRAMES   2
static volatile bool s_want_init   = false;  // core1 → core0: MP3 data is flowing
static volatile bool s_init_failed = false;  // core0 → core1: tried once, no room
static volatile bool s_release_req = false;  // machine reset → core0: free it now
static uint32_t      s_idle_frames = 0;      // core0: service() calls with nothing to do
static uint8_t       s_free_pending = 0;     // core0: frames left before the buffers go

extern "C" void* ngs_helix_alloc(size_t sz) {
    uint32_t need = ((uint32_t)sz + 3u) & ~3u;          // keep 4-byte alignment
    uint8_t* p = nullptr;
    if (s_helix_arena && s_helix_used + need <= s_helix_cap) {
        p = s_helix_arena + s_helix_used;               // SRAM while it lasts
        s_helix_used += need;
    } else if (s_helix_ps && s_helix_ps_used + need <= s_helix_ps_cap) {
        p = s_helix_ps + s_helix_ps_used;               // then butter
        s_helix_ps_used += need;
    }
    if (!p) return nullptr;
    memset(p, 0, need);   // Helix relies on zeroed state on first use
    return p;
}

// (buffers.c routes its own diagnostics to the project-wide osd_printf.)

// Largest frame Helix can emit: MAX_NGRAN * MAX_NSAMP * MAX_NCHAN shorts.
#define MP3_MAX_SAMPLES (2 * 576 * 2)

// ============================================================================
// Buffers — one pooled allocation, carved into the state struct below.
// ============================================================================

#define MP3_IN_SIZE   8192            // MD_SEND byte ring (core1 → core0)
#define MP3_IN_MASK   (MP3_IN_SIZE - 1)
#define MP3_ASM_SIZE  4096            // linear frame-assembly window for the decoder
                                      // (max MP3 frame 1441 B + sync-hunt slack)
// Helix needs the whole frame present before it can decode it, but says so
// politely (ERR_MP3_INDATA_UNDERFLOW) and consumes nothing, so this is only an
// efficiency gate: it stops us re-parsing a header on every service() call
// while the window fills. It is NOT the trap-avoidance minimp3 required — that
// decoder wiped its state and swallowed the whole window on a short buffer,
// which is why this constant used to be 3072 and load-bearing.
#define MP3_DECODE_MIN 1600
#define MP3_OUT_SIZE  4096            // stereo pairs @37500 Hz ≈ 109 ms
#define MP3_OUT_MASK  (MP3_OUT_SIZE - 1)

struct Mp3State {
    short    frame_pcm[MP3_MAX_SAMPLES];                     // interleaved L,R
    uint8_t  in_ring[MP3_IN_SIZE];
    uint8_t  asm_buf[MP3_ASM_SIZE];
    int16_t  out_ring[MP3_OUT_SIZE * 2];                     // interleaved L,R
};
static HMP3Decoder s_dec = nullptr;

static Mp3State* s_st = nullptr;
static Buffer    s_stateBuf;

// SPSC ring indices (free-running, power-of-2 masks; ARM word stores atomic).
static volatile uint32_t s_in_w = 0, s_in_r = 0;    // producer core1 / consumer core0
static volatile uint32_t s_out_w = 0, s_out_r = 0;  // producer core0 / consumer core1
static volatile bool     s_reset_pending = false;
static bool              s_drq_high = true;   // DREQ state (hysteretic, see mddrq)

static int      s_asm_len = 0;        // valid bytes in asm_buf (core0 only)
static uint32_t s_src_hz = 44100;     // last decoded frame's rate
// Linear-interpolation resampler to 37500 Hz: Q16 phase into the source frame.
static uint32_t s_rs_phase = 0;
static int16_t  s_rs_prev_l = 0, s_rs_prev_r = 0;   // last source pair (carry-over)
// SCI_VOL attenuation → Q15 gains (VS1011: 0.5 dB per step, 0xFE/0xFF = mute).
static uint16_t s_gain_l = 32768 >> 1, s_gain_r = 32768 >> 1;  // note: applied Q15 incl. /2 mix scale
static uint64_t s_decoded_samples = 0;              // per channel, at s_src_hz

static uint32_t s_st_frames = 0, s_st_junk = 0, s_st_over = 0, s_st_under = 0;
static uint32_t s_st_resets = 0;      // pipeline flushes (MPXRS / SCI soft reset)

// SCI_HDAT1 / SCI_HDAT0 — the VS10xx "what am I decoding" registers, and the
// only way a player learns the stream's rate and bitrate: NPL reads both and
// treats a zero LOW byte of HDAT1 as "nothing is playing", which is why its
// Hz / kbps / Time Play fields all stayed blank while audio was coming out
// (hw 2026-08-06). The layout is simply the MPEG frame header: HDAT1 = header
// bytes 0-1 (syncword, ID, layer, protect), HDAT0 = bytes 2-3 (bitrate index,
// sample rate, padding, mode, emphasis) — so we publish the header of the last
// frame that actually decoded, and zero both when the pipeline is flushed.
static volatile uint16_t s_hdat1 = 0, s_hdat0 = 0;

// First bytes of each stream, captured on core1 in mdSend() and printed by the
// core0 health tick (Debug::log from core1 blocks for milliseconds — never put
// it on the guest's port path). This is the one measurement that separates "the
// decoder cannot sync" from "the bytes reaching it are not the file": a stream
// that starts 49 44 33 is a real MP3 with an ID3 tag, anything constant or
// obviously not file content means the SD_RSTR → MD_SEND transport is at fault.
#define MP3_SNIFF_LEN 32
static uint8_t  s_sniff[MP3_SNIFF_LEN];
static volatile uint32_t s_sniff_len = 0;
static volatile bool     s_sniff_ready = false;

// ============================================================================
// SCI_BASS and SM_DIFF — the VS1011's post-decoder tone stage
//
// Neither has anything to do with MP3 decoding: on the real card the decoder
// feeds a small DSP block on its way to the DAC, and Helix stops at the PCM —
// so these have to be ours. NPL drives both (key 9 "Treble/Bass" writes
// SCI_BASS, key 8 "Surround" toggles SM_DIFF) and reads SCI_BASS back for its
// display, so the registers themselves always worked; they just did nothing to
// the audio.
//
// SCI_BASS layout (VS1011e):
//   15:12  ST_AMPLITUDE  treble, signed -8..+7, 1.5 dB per step (0 = off)
//   11:8   ST_FREQLIMIT  treble corner, 1 kHz per step (1..15)
//    7:4   SB_AMPLITUDE  bass boost, 0..15 dB (0 = off)
//    3:0   SB_FREQLIMIT  bass corner, 10 Hz per step (2..15)
//
// One first-order shelf per band per channel (Zolzer's direct forms), designed
// once per register write at the fixed 37500 Hz output rate. VLSI's own bass
// enhancer is a dynamic psychoacoustic thing rather than a static shelf, so
// this matches it in character and direction, not sample for sample. Signal
// order follows the chip: decoder -> tone -> SCI_VOL -> DAC.
// ============================================================================
#define TONE_Q 24            // coefficient fraction bits
#define TONE_S 8             // extra fraction bits carried in the filter state

struct Shelf {
    int32_t b0, b1, a1;      // Q24 coefficients
    int32_t x1, y1;          // history, samples in Q8 (headroom for +15 dB)
};
static Shelf s_sh_bass[2], s_sh_treb[2];       // [0] = left, [1] = right
static bool  s_bass_on = false, s_treb_on = false;      // core0
// Written by core1 in the port handler, applied by core0 in service(): the
// coefficient set is six words and must not be recomputed under the reader.
static volatile uint16_t s_bass_reg   = 0;
static volatile bool     s_tone_dirty = false;
static volatile bool     s_sm_diff    = false;  // SCI_MODE b0: left inverted

static inline int32_t sat16(int32_t v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

// y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1], everything Q8 in and out. int64 is
// not a luxury here: a +15 dB shelf sits on a pole at ~0.99, so both the
// coefficient precision and the state precision are what keep its DC gain
// where it was designed.
static inline int32_t shelf_run(Shelf& f, int32_t x) {
    int64_t acc = (int64_t)f.b0 * x + (int64_t)f.b1 * f.x1 - (int64_t)f.a1 * f.y1;
    int32_t y = (int32_t)((acc + (1 << (TONE_Q - 1))) >> TONE_Q);
    f.x1 = x;
    f.y1 = y;
    return y;
}

static inline int32_t tone_q24(float v) {
    return (int32_t)(v * 16777216.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

// First-order shelving filter, low (high=false) or high (high=true). Cut is
// the reciprocal of boost, hence the two branches.
static void shelf_design(Shelf& f, float fc, float gain_db, bool high) {
    const float K  = tanf(3.14159265f * fc / 37500.0f);
    const float V0 = powf(10.0f, fabsf(gain_db) / 20.0f);
    float b0, b1, a1;
    if (gain_db >= 0.0f) {
        const float d = 1.0f + K;
        b0 = (high ? (V0 + K)        : (1.0f + V0 * K)) / d;
        b1 = (high ? (K - V0)        : (V0 * K - 1.0f)) / d;
        a1 = (K - 1.0f) / d;
    } else {
        const float d = high ? (V0 + K) : (1.0f + V0 * K);
        b0 = (1.0f + K) / d;
        b1 = (K - 1.0f) / d;
        a1 = (high ? (K - V0) : (V0 * K - 1.0f)) / d;
    }
    f.b0 = tone_q24(b0);
    f.b1 = tone_q24(b1);
    f.a1 = tone_q24(a1);
}

static void tone_reset() {
    for (int c = 0; c < 2; c++) {
        s_sh_bass[c].x1 = s_sh_bass[c].y1 = 0;
        s_sh_treb[c].x1 = s_sh_treb[c].y1 = 0;
    }
}

// core0: turn the latest SCI_BASS write into coefficients. A band that was
// already running keeps its state (a shelf is stable across a coefficient
// nudge, and NPL sends one per key repeat); a band coming back from bypass
// starts clean, since its history is whatever the stream looked like when it
// was last switched off.
static void tone_update(uint16_t reg) {
    int st_amp = (reg >> 12) & 0x0F;
    if (st_amp > 7) st_amp -= 16;                       // signed 4-bit
    const int  st_freq = (reg >> 8) & 0x0F;
    const int  sb_amp  = (reg >> 4) & 0x0F;
    const int  sb_freq = reg & 0x0F;
    const bool bass_on = sb_amp != 0;
    const bool treb_on = st_amp != 0;
    if (bass_on) {
        const float fc = (sb_freq < 2 ? 2 : sb_freq) * 10.0f;
        for (int c = 0; c < 2; c++) {
            if (!s_bass_on) s_sh_bass[c].x1 = s_sh_bass[c].y1 = 0;
            shelf_design(s_sh_bass[c], fc, (float)sb_amp, false);
        }
    }
    if (treb_on) {
        const float fc = (st_freq < 1 ? 1 : st_freq) * 1000.0f;
        for (int c = 0; c < 2; c++) {
            if (!s_treb_on) s_sh_treb[c].x1 = s_sh_treb[c].y1 = 0;
            shelf_design(s_sh_treb[c], fc, st_amp * 1.5f, true);
        }
    }
    s_bass_on = bass_on;
    s_treb_on = treb_on;
}

static uint16_t vol_gain_q15(uint8_t att) {
    if (att >= 0xFE) return 0;                      // analog powerdown / mute
    // 10^(-att*0.5/20), then halved: the card sums DAC+MP3 into one output
    // stage — half scale keeps the int16 mix from clipping (GS side ≈ ±16k).
    float g = powf(10.0f, -(float)att / 40.0f);
    return (uint16_t)(g * 16384.0f);
}

// ============================================================================
// Core0 — allocation and decode pump
// ============================================================================

bool NgsMp3::init() {
    if (s_st) return true;
    const size_t largest_before = getLargestAllocatable();   // logged: it decides the tier
    if (!s_stateBuf.alloc(sizeof(Mp3State), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM) ||
        !s_stateBuf.data()) {
        s_stateBuf.free();
        Debug::log("NgsMp3: no room for decoder state (%u B) — MP3 stubbed",
                   (unsigned)sizeof(Mp3State));
        return false;
    }
    // Helix state: as much SRAM as the heap will give (largest step first, HOT_SRAM
    // margin), remainder in butter. See the two-arena note at the top — the split
    // exists because 576p leaves ~43 KB of heap where one 24 KB block usually is not
    // available but 12-20 KB is, and the hot structures are the ones Helix asks for
    // first, so greedy carving puts exactly them in SRAM.
    for (uint32_t want : kHelixSramTry) {
        if (s_helixBuf.alloc(want, Buffer::NEED_POINTER | Buffer::HOT_SRAM) &&
            s_helixBuf.data() && s_helixBuf.tier() == Buffer::TIER_HEAP) {
            s_helix_arena = (uint8_t*)s_helixBuf.data();
            s_helix_cap   = want;
            break;
        }
        // HOT_SRAM is a preference, not a promise: Buffer falls through to butter on
        // its own. A non-heap tier here is not the split we are building, so drop it
        // and try a smaller SRAM step; the remainder allocation below covers butter.
        s_helixBuf.free();
    }
    s_helix_used = 0;
    // Whatever SRAM did not cover, plus slack for the per-structure 4-byte rounding.
    if (s_helix_cap < HELIX_ARENA_SIZE) {
        const uint32_t rest = HELIX_ARENA_SIZE - s_helix_cap + 64;
        if (!s_helixPsBuf.alloc(rest, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM) ||
            !s_helixPsBuf.data()) {
            s_helixPsBuf.free();
            s_helixBuf.free(); s_helix_arena = nullptr; s_helix_cap = 0;
            s_stateBuf.free();
            Debug::log("NgsMp3: no room for Helix arena (%u B) — MP3 stubbed",
                       (unsigned)HELIX_ARENA_SIZE);
            return false;
        }
        s_helix_ps      = (uint8_t*)s_helixPsBuf.data();
        s_helix_ps_cap  = rest;
        s_helix_ps_used = 0;
    }
    s_dec = MP3InitDecoder();
    if (!s_dec) {
        s_helixBuf.free();   s_helix_arena = nullptr; s_helix_cap = 0;
        s_helixPsBuf.free(); s_helix_ps    = nullptr; s_helix_ps_cap = 0;
        s_stateBuf.free();
        Debug::log("NgsMp3: MP3InitDecoder failed (arena %u B) — MP3 stubbed",
                   (unsigned)HELIX_ARENA_SIZE);
        return false;
    }
    s_st = (Mp3State*)s_stateBuf.data();
    s_in_w = s_in_r = 0;
    s_out_w = s_out_r = 0;
    s_asm_len = 0;
    s_rs_phase = 0;
    s_rs_prev_l = s_rs_prev_r = 0;
    s_decoded_samples = 0;
    s_hdat1 = s_hdat0 = 0;
    s_reset_pending = false;
    // NOT reset here: s_gain_l/r (SCI_VOL), s_bass_reg (SCI_BASS) and s_sm_diff
    // (SCI_MODE SM_DIFF) mirror registers the GUEST writes, and since this runs on
    // the first MD_SEND byte rather than at GS::init, a player that set its volume
    // or tone before streaming has already written them — clearing them here would
    // play the first track at the wrong level. Their power-on values live in the
    // definitions above, and a chip reset goes through NgsMp3::reset(). The filter
    // state IS ours, so re-derive it from whatever s_bass_reg currently holds.
    s_bass_on = s_treb_on = false;
    tone_reset();
    s_tone_dirty = true;                // core0 designs the filters on the next call
    s_idle_frames = 0;
    s_free_pending = 0;
    // How the arena actually split, and the number that decided it. `sram` under
    // ~20 KB means the hot structures started spilling into butter, which is a
    // PERFORMANCE problem (hw 2026-08-13: NPL stuttered and the emulator lost FPS
    // with the whole arena there) — so the log has to say how close the heap came,
    // not just where things ended up.
    Debug::log("NgsMp3: Helix ready (buffers %u B in %s, state sram=%u/%u butter=%u, "
               "freeHeap=%u largest_before=%u)",
               (unsigned)sizeof(Mp3State), s_stateBuf.tierName(),
               (unsigned)s_helix_used, (unsigned)s_helix_cap,
               (unsigned)s_helix_ps_used,
               (unsigned)getFreeHeap(), (unsigned)largest_before);
    return true;
}

// Hand the memory back (core0). Callers must already have dropped `s_st` and let
// MP3_FREE_GRACE_FRAMES pass, or a core1 mixTick/mdSend holding the old pointer
// would touch freed memory.
static void mp3_release_buffers() {
    s_dec = nullptr;                 // Helix never frees; the arenas go as a whole
    s_helix_arena = nullptr;
    s_helix_used = s_helix_cap = 0;
    s_helix_ps = nullptr;
    s_helix_ps_used = s_helix_ps_cap = 0;
    s_stateBuf.free();
    s_helixBuf.free();
    s_helixPsBuf.free();
    s_free_pending = 0;
    s_idle_frames  = 0;
    // Re-arm the lazy path: the next MD_SEND byte allocates again. NOT s_init_failed
    // — that latch means "there was no room", and it stays for the session.
    s_want_init = false;
}

void NgsMp3::deinit() {
    s_st = nullptr;
    mp3_release_buffers();
    s_init_failed = false;           // a fresh GS::init may try again
    s_release_req = false;
}

void NgsMp3::reset() {
    // A chip reset zeroes the SCI file (GS.cpp's ngs_mp3_reset does that for the
    // register images), so the tone stage and SM_DIFF go with it. NOTE this is the
    // CHIP reset and deliberately frees NOTHING: NPL soft-resets the decoder at every
    // track change, and dropping ~24 KB of SRAM + 33 KB of butter there would churn
    // the allocation mid-session (and could come back in a worse tier). Only a
    // MACHINE reset releases — see releaseNow().
    s_bass_reg      = 0;
    s_tone_dirty    = true;
    s_sm_diff       = false;
    s_reset_pending = true;
}

void NgsMp3::releaseNow() {
    // The ZX session that was streaming has been rebooted, so nothing is going to
    // read the pipeline again: hand the memory back without waiting out the ~30 s
    // idle timer. Only a request — service() runs the two-phase free on core0,
    // because core1 may be inside mixTick/mdSend with the old pointer right now.
    s_release_req = true;
    // One fresh allocation attempt for the next session: the latch means "there was
    // no room", and after a reset the heap is a different shape (this also re-opens
    // the SRAM tier for a decoder that had been forced into butter).
    s_init_failed = false;
}

bool NgsMp3::active() { return s_st != nullptr; }

uint16_t NgsMp3::decodeTimeSec() {
    uint32_t hz = s_src_hz ? s_src_hz : 44100;
    return (uint16_t)(s_decoded_samples / hz);
}

void NgsMp3::sciWrite(uint8_t addr, uint16_t v) {
    switch (addr) {
        case 0x00:                                   // SCI_MODE: b2 = soft reset
            if (v & 0x0004) s_reset_pending = true;
            s_sm_diff = (v & 0x0001) != 0;           // SM_DIFF — NPL's "Surround"
            break;
        case 0x02:                                   // SCI_BASS: bass/treble stage
            s_bass_reg   = v;
            s_tone_dirty = true;                     // core0 designs the filters
            break;
        case 0x04:                                   // SCI_DECODE_TIME write resets it
            s_decoded_samples = 0;
            break;
        case 0x0B:                                   // SCI_VOL: hi = left, lo = right att
            s_gain_l = vol_gain_q15((uint8_t)(v >> 8));
            s_gain_r = vol_gain_q15((uint8_t)v);
            break;
        default:
            break;
    }
}

// Push one resampled 37500 Hz stereo pair; drops when the ring is full (the
// free-space gate in service() makes that the rare race, not the norm).
static inline bool out_push(int16_t l, int16_t r) {
    uint32_t w = s_out_w;
    if (w - s_out_r >= MP3_OUT_SIZE) return false;
    s_st->out_ring[(w & MP3_OUT_MASK) * 2]     = l;
    s_st->out_ring[(w & MP3_OUT_MASK) * 2 + 1] = r;
    __dmb();                                    // data before index
    s_out_w = w + 1;
    return true;
}

// Resample one decoded frame (samples per channel at hz) to 37500 Hz with
// linear interpolation, carrying phase and the last pair across frames.
static void resample_frame(const short* pcm, int samples, int channels, uint32_t hz) {
    if (hz != s_src_hz) {                       // rate change: restart cleanly
        s_src_hz = hz;
        s_rs_phase = 0;
    }
    const uint32_t step = (uint32_t)(((uint64_t)hz << 16) / 37500u);
    // Snapshot the tone stage once per frame: core1 can flip these mid-frame,
    // and a band appearing between two samples would run on stale history.
    const bool bass = s_bass_on, treb = s_treb_on, tone = bass || treb;
    const bool diff = s_sm_diff;
    // Source stream = prev pair (index -1 in Q16 phase space) + this frame.
    while (true) {
        uint32_t idx = s_rs_phase >> 16;
        if ((int)idx >= samples) {              // frame exhausted — carry tail
            s_rs_phase -= (uint32_t)samples << 16;
            int16_t last_l, last_r;
            if (channels == 2) {
                last_l = pcm[(samples - 1) * 2];
                last_r = pcm[(samples - 1) * 2 + 1];
            } else {
                last_l = last_r = pcm[samples - 1];
            }
            s_rs_prev_l = last_l;
            s_rs_prev_r = last_r;
            break;
        }
        int16_t a_l, a_r, b_l, b_r;
        if (idx == 0) { a_l = s_rs_prev_l; a_r = s_rs_prev_r; }
        else if (channels == 2) { a_l = pcm[(idx - 1) * 2]; a_r = pcm[(idx - 1) * 2 + 1]; }
        else { a_l = a_r = pcm[idx - 1]; }
        if (channels == 2) { b_l = pcm[idx * 2]; b_r = pcm[idx * 2 + 1]; }
        else { b_l = b_r = pcm[idx]; }
        uint32_t frac = s_rs_phase & 0xFFFF;
        int32_t l = a_l + (((int32_t)(b_l - a_l) * (int32_t)frac) >> 16);
        int32_t r = a_r + (((int32_t)(b_r - a_r) * (int32_t)frac) >> 16);
        if (tone) {
            int32_t ql = l << TONE_S, qr = r << TONE_S;
            if (bass) {
                ql = shelf_run(s_sh_bass[0], ql);
                qr = shelf_run(s_sh_bass[1], qr);
            }
            if (treb) {
                ql = shelf_run(s_sh_treb[0], ql);
                qr = shelf_run(s_sh_treb[1], qr);
            }
            // The tone stage clips at full scale, as it does on the chip — and
            // it has to happen before the gain multiply, which assumes int16.
            l = sat16((ql + (1 << (TONE_S - 1))) >> TONE_S);
            r = sat16((qr + (1 << (TONE_S - 1))) >> TONE_S);
        }
        l = ((int32_t)l * s_gain_l) >> 15;
        r = ((int32_t)r * s_gain_r) >> 15;
        if (diff) l = -l;                   // SM_DIFF: left channel inverted
        if (!out_push((int16_t)l, (int16_t)r)) break;   // ring full — drop rest
        s_rs_phase += step;
    }
}

#if NGS_TRACE
// 1 Hz decoder health line. Deliberately NOT under NGS_TRACE: that toggle also
// turns on the GS health line and (at level 2) the SD command flood, and the
// MP3 path needs watching on its own — "sound keeps cutting out" is a rate
// problem, and only these numbers say which rate. Costs nothing when idle: it
// prints solely while a stream is moving, and one UART line per second is the
// same budget the other health lines already spend.
//
// Reading the line: fr should sit at the stream's frame rate (~38/s for
// 44.1 kHz), und must be 0. und>0 with in≈0 means the guest is not feeding us
// fast enough (look at MDDRQ / the player's DREQ loop); und>0 with in large
// means service() is not being called often enough to keep up. junk>0 is a
// desynced byte stream, ovr>0 means we are draining the input ring too slowly,
// and rst climbing during playback means something keeps soft-resetting the
// decoder — each flush is an audible gap on its own.
static void mp3_health_tick() {
    if (s_sniff_ready) {
        s_sniff_ready = false;
        __dmb();
        Debug::log("MP3 head: %02X %02X %02X %02X %02X %02X %02X %02X "
                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                   s_sniff[0],  s_sniff[1],  s_sniff[2],  s_sniff[3],
                   s_sniff[4],  s_sniff[5],  s_sniff[6],  s_sniff[7],
                   s_sniff[8],  s_sniff[9],  s_sniff[10], s_sniff[11],
                   s_sniff[12], s_sniff[13], s_sniff[14], s_sniff[15],
                   s_sniff[16], s_sniff[17], s_sniff[18], s_sniff[19],
                   s_sniff[20], s_sniff[21], s_sniff[22], s_sniff[23],
                   s_sniff[24], s_sniff[25], s_sniff[26], s_sniff[27],
                   s_sniff[28], s_sniff[29], s_sniff[30], s_sniff[31]);
    }
    static uint32_t s_last_us = 0;
    static uint32_t s_pf = 0, s_pj = 0, s_po = 0, s_pu = 0, s_pr = 0;
    uint32_t now = time_us_32();
    if (now - s_last_us < 1000000u) return;
    uint32_t df = s_st_frames - s_pf, dj = s_st_junk  - s_pj;
    uint32_t dov = s_st_over  - s_po, du = s_st_under - s_pu;
    uint32_t dr = s_st_resets - s_pr;
    extern volatile uint16_t gs_dbg_bb_pc;
    extern volatile uint16_t gs_dbg_bb_ret;
    extern volatile uint8_t  gs_dbg_bb_st;
    extern volatile uint32_t gs_dbg_bb_polls;
    extern uint16_t gs_dbg_gs_pc(void);
    static uint32_t s_pbb = 0;
    uint32_t dbb = gs_dbg_bb_polls - s_pbb;
    s_pbb = gs_dbg_bb_polls;
    uint32_t in_depth  = s_in_w  - s_in_r;
    uint32_t out_depth = s_out_w - s_out_r;
    // Print while the host is hammering #BB even if every other counter is
    // zero: a wedged player is all zeroes, and staying quiet there is exactly
    // how the first hang capture lost its last seconds.
    bool active = df || dj || dov || du || dr || in_depth || out_depth || dbb > 1000;
    s_last_us = now;
    s_pf = s_st_frames; s_pj = s_st_junk; s_po = s_st_over;
    s_pu = s_st_under;  s_pr = s_st_resets;
    if (!active) return;
    // // SD sequentiality alongside the decoder numbers: the two only make sense
    // // read together (a stream that is corrupt because the guest fetched the
    // // wrong sectors looks identical, in the MP3 counters alone, to one that was
    // // mangled in transit).
    NgsSd::Stats sd;
    NgsSd::getStats(sd);
    static uint32_t s_p17 = 0, s_pbrk = 0;
    uint32_t d17 = sd.reads17 - s_p17, dbrk = sd.seq_break - s_pbrk;
    s_p17 = sd.reads17; s_pbrk = sd.seq_break;
    // Host #BB spin state (GS.cpp): polls/s plus where the host is spinning.
    // Printed here because a wedged player and a starved decoder look the same
    // from the outside, and these two lines together tell them apart.
    Debug::log("MP3: fr=%u junk=%u ovr=%u und=%u rst=%u hz=%u in=%u/%u out=%u/%u | SD rd17=%u brk=%u"
               " | BB %u/s zxpc=%04X ret=%04X st=%02X gspc=%04X hdat=%04X/%04X",
               (unsigned)df, (unsigned)dj, (unsigned)dov, (unsigned)du,
               (unsigned)dr, (unsigned)s_src_hz,
               (unsigned)in_depth,  (unsigned)MP3_IN_SIZE,
               (unsigned)out_depth, (unsigned)MP3_OUT_SIZE,
               (unsigned)d17, (unsigned)dbrk,
               (unsigned)dbb, (unsigned)gs_dbg_bb_pc, (unsigned)gs_dbg_bb_ret,
               (unsigned)gs_dbg_bb_st,
               (unsigned)gs_dbg_gs_pc(),
               (unsigned)s_hdat1, (unsigned)s_hdat0);
    // The host handshake ring. Nothing about it is MP3-specific — it is the
    // #B3/#BB/#33 exchange, and it is the main tool for ANY NeoGS wedge, so it
    // is labelled "NGS hs:" (it used to say "MP3 hs:", which sent every reader
    // looking for a decoder problem in demos that never touch the decoder).
    if (dbb > 1000) {
        extern void gs_dbg_handshake(char* out, int cap);
        extern void gs_dbg_npl_vars(char* out, int cap);
        // MUST stay under Debug::log's own 256-byte line buffer, minus the
        // "NGS hs: " prefix and the log's counter/timestamp header. A bigger
        // buffer here is worthless: vsnprintf truncates the line afterwards,
        // and it truncates the TAIL — the newest entries, i.e. the only ones
        // that name a wedge. That is how the ring kept arriving cut mid-token
        // ("... N40 W0") even after gs_dbg_handshake was taught to print the
        // newest entries first (hw 2026-08-07, two captures lost to it).
        // gs_dbg_handshake budgets backwards from `cap`, so sizing the buffer
        // to what actually survives is what keeps the tail intact.
        char buf[200];
        // NPL's state block only exists when NPL is the thing running. Printed
        // unconditionally it just decodes whatever bytes happen to live at card
        // address 0x4168 — in TheLink that came out as "ftype=57 chip=8D
        // tmo=7975", pure noise that reads like real state. Gate it on the
        // decoder having actually been fed: no MP3 stream, no NPL.
        if (s_st_frames || s_st_junk || s_in_w != s_in_r) {
            gs_dbg_npl_vars(buf, sizeof(buf));
            Debug::log("NPL: %s", buf);
        }
        gs_dbg_handshake(buf, sizeof(buf));
        Debug::log("NGS hs: %s", buf);
    }
}

#else
#define mp3_health_tick() ((void)0)
#endif  // NGS_TRACE

void NgsMp3::service() {
    if (!s_st) {
        // Second phase of the idle release: core1 dropped out of the picture when
        // s_st went null, so after the grace frames the memory can go. This runs
        // BEFORE the re-init check so a stream arriving inside the grace window
        // cannot allocate on top of buffers that are still being retired.
        if (s_free_pending) {
            if (--s_free_pending == 0) {
                const unsigned sram = s_helix_cap;
                const bool     rst  = s_release_req;
                s_release_req = false;
                mp3_release_buffers();
                Debug::log("NgsMp3: %s — decoder released (%u B SRAM back, freeHeap=%u)",
                           rst ? "reset" : "idle", sram, (unsigned)getFreeHeap());
            }
            return;
        }
        s_release_req = false;        // nothing allocated: the request is already met
        // Lazy allocation (see the note at s_want_init). One attempt per session:
        // if there is no room now there will not be room a millisecond later, and
        // retrying on every MD_SEND byte would probe the heap thousands of times
        // a second. Decoding starts on the next call, with the ring empty — the
        // bytes the guest sent while we were stubbed are gone by design.
        if (s_want_init && !s_init_failed) {
            s_want_init = false;
            if (!init()) s_init_failed = true;
        }
        return;
    }
    // A machine reset (F11) frees immediately, whatever is still in the rings — the
    // guest that queued those bytes no longer exists. Same two-phase handoff as the
    // idle path: drop the pointer here, release in the branch above.
    if (s_release_req) {
        s_st = nullptr;
        __dmb();                                // core1 sees the null before the free
        s_free_pending = MP3_FREE_GRACE_FRAMES;  // s_release_req is consumed there
        return;
    }
    // Nothing queued and nothing left to play: after MP3_IDLE_RELEASE_FRAMES give
    // the buffers back (first phase — drop the pointer core1 tests, then let the
    // grace frames run in the branch above). Any traffic at all rearms the count.
    if (s_in_w == s_in_r && s_out_w == s_out_r) {
        if (++s_idle_frames >= MP3_IDLE_RELEASE_FRAMES) {
            s_st = nullptr;
            __dmb();                            // core1 sees the null before the free
            s_free_pending = MP3_FREE_GRACE_FRAMES;
            return;
        }
    } else {
        s_idle_frames = 0;
    }
    mp3_health_tick();
    if (s_tone_dirty) {                     // SCI_BASS changed (core1)
        s_tone_dirty = false;
        __dmb();
        tone_update(s_bass_reg);
    }
    if (s_reset_pending) {
        // Flush the pipeline: consumer-side drains for both rings (in-ring we
        // consume, out-ring the mixer skips while the flag is up), then restart.
        s_in_r = s_in_w;
        s_out_w = s_out_r;                      // producer may rewind its own index
        s_asm_len = 0;
        s_rs_phase = 0;
        s_rs_prev_l = s_rs_prev_r = 0;
        s_decoded_samples = 0;
        tone_reset();                       // stale shelf history is a thump
        s_hdat1 = s_hdat0 = 0;              // "nothing playing" until a frame lands
        // Helix has no reset entry point and needs none: flushing our buffers
        // is enough. The next frames come back MAINDATA_UNDERFLOW while its bit
        // reservoir refers to bytes we dropped, then it picks the stream up.
        s_st_resets++;
        s_sniff_len = 0;                        // re-arm the head snapshot
        s_sniff_ready = false;
        __dmb();
        s_reset_pending = false;
        return;
    }
    // One frame per call: a frame ≈ 26 ms of audio for ≈ 2 ms of M33 time,
    // and service() runs many times per video frame — the cap bounds the
    // stall a single call can add to a frame-pacing wait (same order as one
    // NgsSd sector op).
    {
        // Output headroom for a worst-case frame (1152 @32 kHz → 1350 @37.5k).
        if (MP3_OUT_SIZE - (s_out_w - s_out_r) < 1400) return;
        // Top up the assembly window from the input ring.
        uint32_t r = s_in_r, w = s_in_w;
        while (s_asm_len < MP3_ASM_SIZE && r != w) {
            s_st->asm_buf[s_asm_len++] = s_st->in_ring[r & MP3_IN_MASK];
            r++;
        }
        s_in_r = r;
        // Prime the window before decoding (see MP3_DECODE_MIN). In steady
        // state this costs nothing: the guest runs ahead of playback and the
        // input ring sits near full, so asm_buf tops up to 4096 every call. It
        // only waits while the pipeline is filling after a start or a reset.
        // The last few KB of a track are dropped rather than decoded from a
        // short window — under 80 ms, and the player flushes us between tracks.
        if (s_asm_len < MP3_DECODE_MIN) return;

        // Helix wants inbuf pointing AT the sync word.
        int off = MP3FindSyncWord(s_st->asm_buf, s_asm_len);
        if (off < 0) {
            // Nothing frame-like in the whole window. Keep the last byte: a
            // sync word can straddle the boundary.
            s_st_junk += (uint32_t)(s_asm_len - 1);
            s_st->asm_buf[0] = s_st->asm_buf[s_asm_len - 1];
            s_asm_len = 1;
            return;
        }
        if (off > 0) {                       // leading garbage (ID3 tag, padding)
            memmove(s_st->asm_buf, s_st->asm_buf + off, s_asm_len - off);
            s_asm_len -= off;
            s_st_junk += (uint32_t)off;
        }

        const uint8_t  h0 = s_st->asm_buf[0], h1 = s_st->asm_buf[1];
        const uint8_t  h2 = s_st->asm_buf[2], h3 = s_st->asm_buf[3];
        unsigned char* p    = s_st->asm_buf;
        int            left = s_asm_len;
        int            err  = MP3Decode(s_dec, &p, &left, s_st->frame_pcm, 0);

        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            // The frame is real but not fully here yet. Unlike minimp3 this
            // costs nothing and consumes nothing — just wait for more bytes.
            // (MP3Decode did advance its local copy of the pointer; ours is
            // untouched, which is the whole point of passing a copy.)
            return;
        }
        int consumed = s_asm_len - left;
        if (err && err != ERR_MP3_MAINDATA_UNDERFLOW) {
            // False sync or a damaged frame: step over one byte and let
            // MP3FindSyncWord try again from the next position.
            memmove(s_st->asm_buf, s_st->asm_buf + 1, s_asm_len - 1);
            s_asm_len -= 1;
            s_st_junk += 1;
            return;
        }
        if (consumed > 0 && consumed <= s_asm_len) {
            memmove(s_st->asm_buf, s_st->asm_buf + consumed, s_asm_len - consumed);
            s_asm_len -= consumed;
        }
        if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
            // Valid frame whose bit reservoir refers to data we flushed (the
            // first frames after a start or a soft reset). Consume it, emit
            // nothing, and the stream picks up within a frame or two.
            s_st_junk += (uint32_t)consumed;
            return;
        }

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(s_dec, &fi);
        int ch = fi.nChans ? fi.nChans : 2;
        int per_ch = fi.outputSamps / ch;         // Helix reports the total
        if (per_ch > 0) {
            s_hdat1 = (uint16_t)((h0 << 8) | h1);
            s_hdat0 = (uint16_t)((h2 << 8) | h3);
            s_st_frames++;
            s_decoded_samples += (uint32_t)per_ch;
            resample_frame(s_st->frame_pcm, per_ch, ch, (uint32_t)fi.samprate);
        }
    }
}

// ============================================================================
// Core1 — guest port side + DAC mix
// ============================================================================

void __not_in_flash_func(NgsMp3::mdSend)(uint8_t v) {
    if (!s_st) {
        // Not allocated yet — ask core0 for the decoder and drop this byte. One
        // store, no barrier needed: core0 polls the flag every service() call and
        // a byte lost to the race just arrives on the next MD_SEND.
        if (!s_init_failed) s_want_init = true;
        return;                                 // stub mode: discard
    }
    uint32_t w = s_in_w;
    if (w - s_in_r >= MP3_IN_SIZE) { s_st_over++; return; }
#if NGS_TRACE
    if (s_sniff_len < MP3_SNIFF_LEN) {
        s_sniff[s_sniff_len] = v;
        if (++s_sniff_len == MP3_SNIFF_LEN) { __dmb(); s_sniff_ready = true; }
    }
#endif
    s_st->in_ring[w & MP3_IN_MASK] = v;
    __dmb();
    s_in_w = w + 1;
}

bool __not_in_flash_func(NgsMp3::mddrq)() {
    if (!s_st) return true;                     // stub: infinitely fast bit bucket
    // DREQ, WITH HYSTERESIS — and the hysteresis is the whole point.
    //
    // A real VS1011 drains its FIFO smoothly at the bitrate, so DREQ flickers
    // and the feeding loop always makes progress. We drain in ~1 KB lumps once
    // per decoded frame, and a bare threshold turns that into a pathological
    // trickle: the decoder frees ~1044 bytes, DREQ opens, the guest pushes its
    // next 32-byte block, free drops back under the mark and it stalls again.
    // NPL's RON_MP3 rechecks DREQ every 32 bytes but only returns to its
    // command poll (OPROS) after a WHOLE 512-byte sector, so one sector took a
    // dozen-plus frames and the host got serviced barely twice a second — the
    // player's clock stopped and its keys went dead while audio played on
    // (hw 2026-08-06, with the input ring pinned at 7170..7199 of 8192).
    //
    // So: once high, stay high until the ring is genuinely full; once low, wait
    // for a real chunk of room before re-arming. The guest then dumps several
    // sectors in a burst, polling the host between each, and parks cleanly.
    uint32_t freeb = MP3_IN_SIZE - (s_in_w - s_in_r);
    if (s_drq_high) { if (freeb < 256)  s_drq_high = false; }
    else            { if (freeb >= 2048) s_drq_high = true;  }
    return s_drq_high;
}

void __not_in_flash_func(NgsMp3::mixTick)(int32_t& l, int32_t& r) {
    if (!s_st || s_reset_pending) return;
    uint32_t rd = s_out_r;
    if (rd == s_out_w) {
        // Ring empty: silence. Only count it as an underrun while a stream is
        // actually flowing (input pending or recent output), else idle counts.
        if (s_in_w != s_in_r) s_st_under++;
        return;
    }
    l += s_st->out_ring[(rd & MP3_OUT_MASK) * 2];
    r += s_st->out_ring[(rd & MP3_OUT_MASK) * 2 + 1];
    s_out_r = rd + 1;
}

void NgsMp3::getStats(Stats& out) {
    out.frames    = s_st_frames;
    out.junk      = s_st_junk;
    out.overruns  = s_st_over;
    out.underruns = s_st_under;
    out.hz        = s_src_hz;
}

uint16_t NgsMp3::hdat1() { return s_hdat1; }
uint16_t NgsMp3::hdat0() { return s_hdat0; }
