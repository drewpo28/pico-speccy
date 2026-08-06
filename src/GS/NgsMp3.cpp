#include "NgsMp3.h"
#include "../Buffer.h"
#include "NgsSd.h"
#include "../Debug.h"

#include "pico.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include <string.h>
#include <math.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
// Keep minimp3's ~16.8 KB per-frame scratch off the stack — core0 has 8 KB and
// the first decoded frame faulted on it (see the patch note in minimp3.h). We
// supply it via mp3dec_external_scratch() from s_scratchBuf below.
#define MINIMP3_EXTERNAL_SCRATCH
#include "../minimp3/minimp3.h"

// ============================================================================
// Buffers — one pooled allocation, carved into the state struct below.
// ============================================================================

#define MP3_IN_SIZE   8192            // MD_SEND byte ring (core1 → core0)
#define MP3_IN_MASK   (MP3_IN_SIZE - 1)
#define MP3_ASM_SIZE  4096            // linear frame-assembly window for minimp3
                                      // (max MP3 frame 1441 B + sync-hunt slack)
// Never hand minimp3 a window that cannot hold a worst-case frame plus the
// header after it. mp3dec_decode_frame() only accepts a frame once it can
// validate the NEXT header inside the same buffer; when it cannot, it wipes its
// own state, reports the WHOLE window as consumed and returns zero samples. Our
// caller then discards those bytes, the next window again starts mid-frame, and
// the stream can never resynchronise — hw 2026-08-06: junk exactly equal to the
// input rate with fr=0, indefinitely. The tell was that toggling max speed fixed
// it: that skips the frame-pacing waits which also pump service(), so the input
// ring accumulated and the windows got big enough to sync. Worst case is a
// 1441-byte frame reached at an arbitrary offset, so two frames plus a header.
#define MP3_DECODE_MIN 3072
#define MP3_OUT_SIZE  4096            // stereo pairs @37500 Hz ≈ 109 ms
#define MP3_OUT_MASK  (MP3_OUT_SIZE - 1)

struct Mp3State {
    mp3dec_t dec;
    mp3d_sample_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];  // 1152*2 shorts
    uint8_t  in_ring[MP3_IN_SIZE];
    uint8_t  asm_buf[MP3_ASM_SIZE];
    int16_t  out_ring[MP3_OUT_SIZE * 2];                     // interleaved L,R
};

static Mp3State* s_st = nullptr;
static Buffer    s_stateBuf;

// minimp3's per-frame workspace (~16.8 KB) gets its OWN allocation, asked for
// WITHOUT PREFER_PSRAM so it lands in heap/SRAM. It is by far the hottest thing
// in the decoder — grbuf[2][576] and syn[33][64] are swept several times per
// frame — and in butter PSRAM it does not just decode slowly, it thrashes the
// XIP cache that core1's GS sample reads share. hw 2026-08-06 with the scratch
// pooled in PSRAM: playback ran at half speed, and the tell was that the OUTPUT
// side was the limit, not the decoder — out sat in its regulated band (~3055 of
// 4096) with und=0 while fr held 18/s against the 38.28/s a 44.1 kHz stream
// needs, i.e. mixTick was only draining ~17.6k of 37500 pairs/s because the
// GS-Z80 on core1 had itself dropped to half speed.
static mp3dec_scratch_t* s_scratch = nullptr;
static Buffer            s_scratchBuf;

// Handed to minimp3 in place of its stack local (MINIMP3_EXTERNAL_SCRATCH).
// Only ever reached from mp3dec_decode_frame(), which this file calls from
// service() alone — core0, never nested — so one shared instance is safe.
static mp3dec_scratch_t *mp3dec_external_scratch(void) { return s_scratch; }

// SPSC ring indices (free-running, power-of-2 masks; ARM word stores atomic).
static volatile uint32_t s_in_w = 0, s_in_r = 0;    // producer core1 / consumer core0
static volatile uint32_t s_out_w = 0, s_out_r = 0;  // producer core0 / consumer core1
static volatile bool     s_reset_pending = false;

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
    if (!s_stateBuf.alloc(sizeof(Mp3State), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM) ||
        !s_stateBuf.data()) {
        s_stateBuf.free();
        Debug::log("NgsMp3: no room for decoder state (%u B) — MP3 stubbed",
                   (unsigned)sizeof(Mp3State));
        return false;
    }
    // Heap first (see the note on s_scratch); PSRAM only as a last resort, so a
    // tight heap degrades to slow playback instead of no MP3 at all.
    if (!s_scratchBuf.alloc(sizeof(mp3dec_scratch_t), Buffer::NEED_POINTER) ||
        !s_scratchBuf.data()) {
        s_scratchBuf.free();
        if (!s_scratchBuf.alloc(sizeof(mp3dec_scratch_t),
                                Buffer::NEED_POINTER | Buffer::PREFER_PSRAM) ||
            !s_scratchBuf.data()) {
            s_scratchBuf.free();
            s_stateBuf.free();
            Debug::log("NgsMp3: no room for decoder scratch (%u B) — MP3 stubbed",
                       (unsigned)sizeof(mp3dec_scratch_t));
            return false;
        }
    }
    s_scratch = (mp3dec_scratch_t*)s_scratchBuf.data();
    s_st = (Mp3State*)s_stateBuf.data();
    mp3dec_init(&s_st->dec);
    s_in_w = s_in_r = 0;
    s_out_w = s_out_r = 0;
    s_asm_len = 0;
    s_rs_phase = 0;
    s_rs_prev_l = s_rs_prev_r = 0;
    s_gain_l = s_gain_r = 16384;
    s_decoded_samples = 0;
    s_reset_pending = false;
    Debug::log("NgsMp3: decoder ready (%u B in %s, scratch %u B in %s)",
               (unsigned)sizeof(Mp3State), s_stateBuf.tierName(),
               (unsigned)sizeof(mp3dec_scratch_t), s_scratchBuf.tierName());
    return true;
}

void NgsMp3::deinit() {
    s_st = nullptr;
    s_scratch = nullptr;
    s_stateBuf.free();
    s_scratchBuf.free();
}

void NgsMp3::reset() {
    s_reset_pending = true;
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
static void resample_frame(const mp3d_sample_t* pcm, int samples, int channels, uint32_t hz) {
    if (hz != s_src_hz) {                       // rate change: restart cleanly
        s_src_hz = hz;
        s_rs_phase = 0;
    }
    const uint32_t step = (uint32_t)(((uint64_t)hz << 16) / 37500u);
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
        l = ((int32_t)l * s_gain_l) >> 15;
        r = ((int32_t)r * s_gain_r) >> 15;
        if (!out_push((int16_t)l, (int16_t)r)) break;   // ring full — drop rest
        s_rs_phase += step;
    }
}

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
    uint32_t in_depth  = s_in_w  - s_in_r;
    uint32_t out_depth = s_out_w - s_out_r;
    bool active = df || dj || dov || du || dr || in_depth || out_depth;
    s_last_us = now;
    s_pf = s_st_frames; s_pj = s_st_junk; s_po = s_st_over;
    s_pu = s_st_under;  s_pr = s_st_resets;
    if (!active) return;
    // SD sequentiality alongside the decoder numbers: the two only make sense
    // read together (a stream that is corrupt because the guest fetched the
    // wrong sectors looks identical, in the MP3 counters alone, to one that was
    // mangled in transit).
    NgsSd::Stats sd;
    NgsSd::getStats(sd);
    static uint32_t s_p17 = 0, s_pbrk = 0;
    uint32_t d17 = sd.reads17 - s_p17, dbrk = sd.seq_break - s_pbrk;
    s_p17 = sd.reads17; s_pbrk = sd.seq_break;
    Debug::log("MP3: fr=%u junk=%u ovr=%u und=%u rst=%u hz=%u in=%u/%u out=%u/%u | SD rd17=%u brk=%u",
               (unsigned)df, (unsigned)dj, (unsigned)dov, (unsigned)du,
               (unsigned)dr, (unsigned)s_src_hz,
               (unsigned)in_depth,  (unsigned)MP3_IN_SIZE,
               (unsigned)out_depth, (unsigned)MP3_OUT_SIZE,
               (unsigned)d17, (unsigned)dbrk);
}

void NgsMp3::service() {
    if (!s_st) return;
    mp3_health_tick();
    if (s_reset_pending) {
        // Flush the pipeline: consumer-side drains for both rings (in-ring we
        // consume, out-ring the mixer skips while the flag is up), then restart.
        s_in_r = s_in_w;
        s_out_w = s_out_r;                      // producer may rewind its own index
        s_asm_len = 0;
        s_rs_phase = 0;
        s_rs_prev_l = s_rs_prev_r = 0;
        s_decoded_samples = 0;
        mp3dec_init(&s_st->dec);
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
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_st->dec, s_st->asm_buf, s_asm_len,
                                          s_st->frame_pcm, &info);
        if (info.frame_bytes <= 0) {
            // No full frame in the window. If it's completely full of junk
            // that never syncs, drop half to keep the stream moving.
            if (s_asm_len >= MP3_ASM_SIZE) {
                // Rate-limited peek at what we are about to throw away. A full
                // 4 KB window of a 320 kbps stream holds ~4 frames, so failing
                // to sync in it means the window is not contiguous file data —
                // this says whether it is filler (0xFF runs from the SD line
                // going idle mid-transfer), a repeated byte, or genuine audio
                // that minimp3 is rejecting for some other reason.
                static uint32_t s_last_dump_us = 0;
                uint32_t now = time_us_32();
                if (now - s_last_dump_us >= 1000000u) {
                    s_last_dump_us = now;
                    const uint8_t* b = s_st->asm_buf;
                    // Count 0xFF bytes across the window — one number that
                    // separates "idle line leaked in" from "real but unsyncable".
                    int ff = 0;
                    for (int i = 0; i < MP3_ASM_SIZE; i++) if (b[i] == 0xFF) ff++;
                    Debug::log("MP3 junk: ff=%d/%d head %02X %02X %02X %02X %02X %02X %02X %02X "
                               "%02X %02X %02X %02X %02X %02X %02X %02X",
                               ff, MP3_ASM_SIZE,
                               b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
                               b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                }
                memmove(s_st->asm_buf, s_st->asm_buf + MP3_ASM_SIZE / 2, MP3_ASM_SIZE / 2);
                s_asm_len = MP3_ASM_SIZE / 2;
                s_st_junk += MP3_ASM_SIZE / 2;
            }
            return;
        }
        uint8_t fhdr[16];
        memcpy(fhdr, s_st->asm_buf, 16);        // frame start, before it is consumed
        memmove(s_st->asm_buf, s_st->asm_buf + info.frame_bytes, s_asm_len - info.frame_bytes);
        s_asm_len -= info.frame_bytes;
        if (samples > 0) {
            s_st_frames++;
            s_decoded_samples += (uint32_t)samples;
            resample_frame(s_st->frame_pcm, samples, info.channels, (uint32_t)info.hz);
        } else {
            // A frame minimp3 located but decoded to nothing. Everything else
            // has been ruled out by measurement (sectors sequential, byte count
            // exact to the sector, stream starts on a valid FF FB header), so
            // the only thing left is what it actually parsed: dump the header
            // it accepted plus the fields it derived. layer!=3 or a nonsense
            // bitrate/rate means it locked onto a false sync; a sane MPEG-1
            // Layer III header here means the fault is inside the decode.
            static uint32_t s_last_bad_us = 0;
            uint32_t now = time_us_32();
            if (now - s_last_bad_us >= 1000000u) {
                s_last_bad_us = now;
                const uint8_t* b = fhdr;
                Debug::log("MP3 bad: bytes=%d layer=%d hz=%d ch=%d br=%d | "
                           "%02X %02X %02X %02X %02X %02X %02X %02X "
                           "%02X %02X %02X %02X %02X %02X %02X %02X",
                           info.frame_bytes, info.layer, info.hz, info.channels,
                           info.bitrate_kbps,
                           b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
                           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
            }
            s_st_junk += (uint32_t)info.frame_bytes;
        }
    }
}

// ============================================================================
// Core1 — guest port side + DAC mix
// ============================================================================

void __not_in_flash_func(NgsMp3::mdSend)(uint8_t v) {
    if (!s_st) return;                          // stub mode: discard
    uint32_t w = s_in_w;
    if (w - s_in_r >= MP3_IN_SIZE) { s_st_over++; return; }
    if (s_sniff_len < MP3_SNIFF_LEN) {
        s_sniff[s_sniff_len] = v;
        if (++s_sniff_len == MP3_SNIFF_LEN) { __dmb(); s_sniff_ready = true; }
    }
    s_st->in_ring[w & MP3_IN_MASK] = v;
    __dmb();
    s_in_w = w + 1;
}

bool __not_in_flash_func(NgsMp3::mddrq)() {
    if (!s_st) return true;                     // stub: infinitely fast bit bucket
    // DREQ high = the chip can take a burst (real VS1011: ≥32 bytes). Keep a
    // fat margin so a full burst between polls never overruns.
    return MP3_IN_SIZE - (s_in_w - s_in_r) >= 1024;
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
