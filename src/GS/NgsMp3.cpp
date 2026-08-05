#include "NgsMp3.h"
#include "../Buffer.h"
#include "../Debug.h"

#include "pico.h"
#include "hardware/sync.h"
#include <string.h>
#include <math.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "../minimp3/minimp3.h"

// ============================================================================
// Buffers — one pooled allocation, carved into the state struct below.
// ============================================================================

#define MP3_IN_SIZE   8192            // MD_SEND byte ring (core1 → core0)
#define MP3_IN_MASK   (MP3_IN_SIZE - 1)
#define MP3_ASM_SIZE  4096            // linear frame-assembly window for minimp3
                                      // (max MP3 frame 1441 B + sync-hunt slack)
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
    Debug::log("NgsMp3: decoder ready (%u B in %s)", (unsigned)sizeof(Mp3State),
               s_stateBuf.tierName());
    return true;
}

void NgsMp3::deinit() {
    s_st = nullptr;
    s_stateBuf.free();
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

void NgsMp3::service() {
    if (!s_st) return;
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
        if (s_asm_len < 4) return;
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_st->dec, s_st->asm_buf, s_asm_len,
                                          s_st->frame_pcm, &info);
        if (info.frame_bytes <= 0) {
            // No full frame in the window. If it's completely full of junk
            // that never syncs, drop half to keep the stream moving.
            if (s_asm_len >= MP3_ASM_SIZE) {
                memmove(s_st->asm_buf, s_st->asm_buf + MP3_ASM_SIZE / 2, MP3_ASM_SIZE / 2);
                s_asm_len = MP3_ASM_SIZE / 2;
                s_st_junk += MP3_ASM_SIZE / 2;
            }
            return;
        }
        memmove(s_st->asm_buf, s_st->asm_buf + info.frame_bytes, s_asm_len - info.frame_bytes);
        s_asm_len -= info.frame_bytes;
        if (samples > 0) {
            s_st_frames++;
            s_decoded_samples += (uint32_t)samples;
            resample_frame(s_st->frame_pcm, samples, info.channels, (uint32_t)info.hz);
        } else {
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
