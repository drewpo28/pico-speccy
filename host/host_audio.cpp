// Host replacement for src/drivers/sound/pwm_audio.cpp: the emulator hands one
// frame of unipolar 8-bit samples per emulated frame to pwm_audio_write(); here they
// are scaled exactly as the firmware scales them, DC-blocked, and queued into a ring
// that an SDL audio callback drains at 31250 Hz. The Pico-Zx-Player's own ring
// (pcm_player_attach) is drained by the same callback, as the PWM timer does.
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "app/Config.h"
#include "drivers/sound/pwm_audio.h"
#include "speccy/devices/gs/GS.h"

#define VOLUME_0DB 16
static volatile uint8_t vol = VOLUME_0DB;
uint8_t link_i2s_code = 0xFF;
bool is_i2s_enabled = false;
volatile uint32_t g_pcm_tick_ct = 0;
volatile uint32_t g_pcm_hold_ct = 0;

volatile int16_t* volatile pcm_card_ring = nullptr;   // PI-CARD MUSICA
volatile uint32_t pcm_card_mask = 0;
volatile uint32_t pcm_card_r = 0;
volatile uint32_t pcm_card_w = 0;

volatile int16_t* volatile pcm_player_ring = nullptr;
volatile uint32_t pcm_player_mask = 0;
volatile uint32_t pcm_player_r = 0;
volatile uint32_t pcm_player_w = 0;

// Emulator -> callback ring, stereo int16 frames. 8192 frames = 0.26 s.
//
// The emulator delivers a whole frame (~624 samples) every 20 ms on its own clock;
// the sound card pulls 256 at a time on ITS clock. So the callback keeps a cushion
// (s_target, 65..200 ms) and reads through a linear resampler whose step follows the fill
// level by up to +-0.5%: a fuller ring is read a little faster, an emptier one a
// little slower, and the two clocks never drift apart. An empty ring is not filled
// with a held sample: the callback waits (silence, fading) until the cushion is back.
static constexpr uint32_t RING = 8192;
static constexpr uint32_t TARGET_MIN = 2048;      // frames, ~65 ms at 31250 Hz
static constexpr uint32_t TARGET_MAX = 6144;      // ~200 ms
static constexpr uint32_t TARGET_STEP = 512;      // ~16 ms more after each underrun
// The cushion grows when the ring runs dry: an audio server that pulls in big blocks
// (PipeWire with clock.quantum 2048 at 48 kHz = 43 ms) needs more than one that
// pulls every few ms, and the underrun is the measurement.
static std::atomic<uint32_t> s_target{TARGET_MIN};
// When the emulator last delivered a frame: a dry ring with the emulator STOPPED (the
// OSD menu, a reboot) is not a cushion that was too small, so it must not grow it.
static std::atomic<uint32_t> s_lastWriteMs{0};
uint32_t host_audio_target() { return s_target.load(std::memory_order_relaxed); }
static int16_t s_ring[RING][2];
static std::atomic<uint32_t> s_w{0}, s_r{0};
static int32_t s_dcL = 0, s_dcR = 0;
static int32_t s_lastL = 0, s_lastR = 0;          // last output (before DC removal)
static uint32_t s_frac = 0;                       // resampler phase, Q16
static bool s_primed = false;
// Diagnostics for host_main's 5 s report: samples in/out and the fill extremes.
volatile uint32_t host_au_in = 0, host_au_out = 0, host_au_fmin = 0xFFFFFFFF, host_au_fmax = 0, host_au_stall = 0;
static SDL_AudioDeviceID s_dev = 0;
static int s_hz = 31250;

// General Sound / NeoGS. On the board GS::getLiveLR() is called once per PCM tick,
// every 32 us, and its own ring holds only ~128 samples (GS_RING_TARGET) with a
// drain-rate servo tuned to that regular pace. The SDL callback pulls hundreds of
// samples at once, which empties that ring and repeats its last sample - audible as
// stutter. So a thread calls getLiveLR at the board's pace (31250/s, a millisecond's
// worth at a time) into a ring of our own, and the callback reads that ring with a
// cushion and the same +-0.5% follow as the ZX ring.
static constexpr uint32_t GRING = 8192;
static constexpr uint32_t GTARGET = 1024;          // ~33 ms
static int32_t s_gring[GRING][2];
static std::atomic<uint32_t> s_gw{0}, s_gr{0};
static uint32_t s_gfrac = 0;
static bool s_gprimed = false;

static void gs_sampler() {
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    uint64_t made = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
        const uint64_t due = us * 31250 / 1000000;
        if (due - made > 4096) made = due - 4096;   // after a stall: do not replay seconds
        if (!GS::enabled) { made = due; continue; }
        const int32_t vol8 = (int32_t)((uint32_t)(vol + Config::audio_boost) << 3);
        while (made < due) {
            uint8_t gL, gR;
            GS::getLiveLR(gL, gR);
            const uint32_t w = s_gw.load(std::memory_order_relaxed);
            if (w - s_gr.load(std::memory_order_acquire) >= GRING - 1) s_gr.fetch_add(1, std::memory_order_acq_rel);
            s_gring[w % GRING][0] = ((int32_t)gL - 128) * vol8;
            s_gring[w % GRING][1] = ((int32_t)gR - 128) * vol8;
            s_gw.store(w + 1, std::memory_order_release);
            made++;
        }
    }
}

// One GS sample for the callback (0 while the ring is priming).
static inline void gs_next(uint32_t step, int32_t& l, int32_t& r) {
    l = r = 0;
    uint32_t rp = s_gr.load(std::memory_order_relaxed);
    const uint32_t wp = s_gw.load(std::memory_order_acquire);
    if (!s_gprimed) { if (wp - rp >= GTARGET) s_gprimed = true; else return; }
    if (wp - rp < 2) { s_gprimed = false; return; }
    const int32_t* a = s_gring[rp % GRING];
    const int32_t* b = s_gring[(rp + 1) % GRING];
    const int64_t f = s_gfrac;
    l = a[0] + (int32_t)(((int64_t)(b[0] - a[0]) * f) >> 16);
    r = a[1] + (int32_t)(((int64_t)(b[1] - a[1]) * f) >> 16);
    s_gfrac += step;
    rp += s_gfrac >> 16;
    s_gfrac &= 0xFFFF;
    s_gr.store(rp, std::memory_order_release);
}

static void audio_cb(void*, Uint8* out, int bytes) {
    int16_t* o = (int16_t*)out;
    const int frames = bytes / 4;
    volatile int16_t* pring = pcm_player_ring;
    uint32_t step = 0x10000;
    const uint32_t TARGET = s_target.load(std::memory_order_relaxed);
    if (!pring) {
        const uint32_t fill = s_w.load(std::memory_order_acquire) - s_r.load(std::memory_order_relaxed);
        if (!s_primed && fill >= TARGET) s_primed = true;
        if (fill < host_au_fmin) host_au_fmin = fill;
        if (fill > host_au_fmax) host_au_fmax = fill;
        host_au_out += frames;
        // Speed correction: +-0.5% at a full TARGET of error, linear in between.
        int32_t err = (int32_t)fill - (int32_t)TARGET;
        if (err > (int32_t)TARGET) err = TARGET;
        if (err < -(int32_t)TARGET) err = -(int32_t)TARGET;
        step = (uint32_t)(0x10000 + (int64_t)err * 328 / (int32_t)TARGET);
        // A backlog far past the cushion (a stall, the menu): skip to the cushion.
        if (fill > 3 * TARGET) s_r.store(s_w.load(std::memory_order_acquire) - TARGET, std::memory_order_release);
    }
    uint32_t gstep = 0x10000;
    if (GS::enabled) {
        const uint32_t gfill = s_gw.load(std::memory_order_acquire) - s_gr.load(std::memory_order_relaxed);
        int32_t gerr = (int32_t)gfill - (int32_t)GTARGET;
        if (gerr > (int32_t)GTARGET) gerr = GTARGET;
        if (gerr < -(int32_t)GTARGET) gerr = -(int32_t)GTARGET;
        gstep = (uint32_t)(0x10000 + (int64_t)gerr * 328 / (int32_t)GTARGET);
    }
    for (int i = 0; i < frames; i++) {
        g_pcm_tick_ct++;
        int32_t l, r;
        if (pring) {
            const uint32_t rp = pcm_player_r;
            if (rp != pcm_player_w) {
                const uint32_t k = (rp & pcm_player_mask) << 1;
                l = pring[k];
                r = pring[k + 1];
                pcm_player_r = rp + 1;
            } else {
                l = r = 0;
            }
        } else {
            uint32_t rp = s_r.load(std::memory_order_relaxed);
            const uint32_t wp = s_w.load(std::memory_order_acquire);
            if (s_primed && wp - rp >= 2) {
                const int16_t* a = s_ring[rp % RING];
                const int16_t* b = s_ring[(rp + 1) % RING];
                const int32_t f = (int32_t)(s_frac >> 1);   // Q15
                s_lastL = a[0] + (((b[0] - a[0]) * f) >> 15);
                s_lastR = a[1] + (((b[1] - a[1]) * f) >> 15);
                s_frac += step;
                rp += s_frac >> 16;
                s_frac &= 0xFFFF;
                s_r.store(rp, std::memory_order_release);
            } else {
                // Ran dry: wait for the cushion again, and let the output settle on
                // the DC estimate (= silence once the pedestal is removed) instead of
                // repeating the last sample.
                if (s_primed) {
                    s_primed = false;
                    g_pcm_hold_ct++;
                    const bool stalled = SDL_GetTicks() - s_lastWriteMs.load(std::memory_order_relaxed) > 60;
                    if (stalled) host_au_stall++;
                    else if (TARGET < TARGET_MAX) s_target.store(TARGET + TARGET_STEP, std::memory_order_relaxed);
                }
                s_lastL = (s_dcL >> 16) + ((s_lastL - (s_dcL >> 16)) * 63 >> 6);
                s_lastR = (s_dcR >> 16) + ((s_lastR - (s_dcR >> 16)) * 63 >> 6);
            }
            // The ZX half of the mix is unipolar; take the pedestal out (the
            // firmware's coupling-cap model in pcm_call_inner).
            s_dcL += ((s_lastL << 16) - s_dcL) >> 10;
            s_dcR += ((s_lastR << 16) - s_dcR) >> 10;
            l = s_lastL - (s_dcL >> 16);
            r = s_lastR - (s_dcR >> 16);
            // The board's PCM tick (pwm_audio.cpp pcm_call_inner) adds two live
            // sources per output sample: General Sound / NeoGS and PI-CARD MUSICA.
            const int32_t vol8 = (int32_t)((uint32_t)(vol + Config::audio_boost) << 3);
            if (GS::enabled) {
                int32_t gl, gr;
                gs_next(gstep, gl, gr);
                l += gl;
                r += gr;
            }
            volatile int16_t* cring = pcm_card_ring;
            if (cring) {
                const uint32_t cr = pcm_card_r;
                if (cr != pcm_card_w) {
                    const uint32_t k = (cr & pcm_card_mask) << 1;
                    l += ((int32_t)cring[k] * vol8) >> 8;
                    r += ((int32_t)cring[k + 1] * vol8) >> 8;
                    pcm_card_r = cr + 1;
                }
            }
        }
        o[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        o[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

void pcm_setup(int hz) {
    s_hz = hz;
    s_dcL = s_dcR = 0;
    s_primed = false;
    s_frac = 0;
    if (s_dev) SDL_CloseAudioDevice(s_dev);
    SDL_AudioSpec want = {}, have = {};
    want.freq = hz;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 256;
    want.callback = audio_cb;
    s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    static bool gsThread = false;
    if (!gsThread) { gsThread = true; std::thread(gs_sampler).detach(); }
    if (s_dev) {
        SDL_PauseAudioDevice(s_dev, 0);
        fprintf(stderr, "host: audio %s, %d Hz, %u-frame buffer\n",
                SDL_GetCurrentAudioDriver(), have.freq, (unsigned)have.samples);
    } else SDL_Log("audio: %s", SDL_GetError());
}
void pcm_cleanup(void) {}
void init_sound() { pcm_setup(31250); }

esp_err_t pwm_audio_set_volume(int8_t volume) {
    if (volume < -VOLUME_0DB) { vol = 0; return ESP_OK; }
    if (volume > VOLUME_0DB) volume = VOLUME_0DB;
    vol = volume + VOLUME_0DB;
    return ESP_OK;
}

esp_err_t pwm_audio_write(uint8_t* bufL, uint8_t* bufR, size_t len, size_t* written, uint32_t) {
    const uint32_t vol8 = (uint32_t)(vol + Config::audio_boost) << 3;
    uint32_t w = s_w.load(std::memory_order_relaxed);
    const uint32_t r = s_r.load(std::memory_order_acquire);
    // Keep the latency bounded: if the emulator runs ahead, drop the oldest backlog.
    if (w - r > RING - len) s_r.store(w - len, std::memory_order_release);
    for (size_t i = 0; i < len; i++) {
        const int32_t vL = (int32_t)bufL[i] * (int32_t)vol8;
        const int32_t vR = (int32_t)bufR[i] * (int32_t)vol8;
        s_ring[w % RING][0] = vL > 32767 ? 32767 : (int16_t)vL;
        s_ring[w % RING][1] = vR > 32767 ? 32767 : (int16_t)vR;
        w++;
    }
    s_w.store(w, std::memory_order_release);
    host_au_in += len;
    s_lastWriteMs.store(SDL_GetTicks(), std::memory_order_relaxed);
    if (written) *written = len;
    return ESP_OK;
}

void pcm_player_attach(int16_t* ring, uint32_t frames) {
    pcm_player_ring = nullptr;
    pcm_player_mask = frames - 1;
    pcm_player_r = pcm_player_w = 0;
    pcm_player_ring = ring;
}
void pcm_player_detach() { pcm_player_ring = nullptr; }

void pcm_card_attach(int16_t* ring, uint32_t frames) {
    pcm_card_ring = nullptr;
    pcm_card_mask = frames - 1;
    pcm_card_r = pcm_card_w = 0;
    pcm_card_ring = ring;
}
void pcm_card_detach() { pcm_card_ring = nullptr; }

// No line-in on the host.
bool pcm_data_in(void) { return false; }
void pwm_audio_in_frame_started(void) {}
void pcm_audio_in_stop(void) {}
