// Host replacement for src/drivers/sound/pwm_audio.cpp: the emulator hands one
// frame of unipolar 8-bit samples per emulated frame to pwm_audio_write(); here they
// are scaled exactly as the firmware scales them, DC-blocked, and queued into a ring
// that an SDL audio callback drains at 31250 Hz. The Pico-Zx-Player's own ring
// (pcm_player_attach) is drained by the same callback, as the PWM timer does.
#include <SDL.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "app/Config.h"
#include "drivers/sound/pwm_audio.h"

#define VOLUME_0DB 16
static volatile uint8_t vol = VOLUME_0DB;
uint8_t link_i2s_code = 0xFF;
bool is_i2s_enabled = false;
volatile uint32_t g_pcm_tick_ct = 0;
volatile uint32_t g_pcm_hold_ct = 0;

volatile int16_t* volatile pcm_player_ring = nullptr;
volatile uint32_t pcm_player_mask = 0;
volatile uint32_t pcm_player_r = 0;
volatile uint32_t pcm_player_w = 0;

// Emulator -> callback ring, stereo int16 frames. 8192 frames = 0.26 s.
static constexpr uint32_t RING = 8192;
static int16_t s_ring[RING][2];
static std::atomic<uint32_t> s_w{0}, s_r{0};
static int32_t s_dcL = 0, s_dcR = 0;
static int16_t s_holdL = 0, s_holdR = 0;
static SDL_AudioDeviceID s_dev = 0;
static int s_hz = 31250;

static void audio_cb(void*, Uint8* out, int bytes) {
    int16_t* o = (int16_t*)out;
    const int frames = bytes / 4;
    for (int i = 0; i < frames; i++) {
        g_pcm_tick_ct++;
        int32_t l, r;
        volatile int16_t* pring = pcm_player_ring;
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
            const uint32_t rp = s_r.load(std::memory_order_relaxed);
            if (rp != s_w.load(std::memory_order_acquire)) {
                s_holdL = s_ring[rp % RING][0];
                s_holdR = s_ring[rp % RING][1];
                s_r.store(rp + 1, std::memory_order_release);
            } else {
                g_pcm_hold_ct++;
            }
            // The ZX half of the mix is unipolar; take the pedestal out (the
            // firmware's coupling-cap model in pcm_call_inner).
            s_dcL += (((int32_t)s_holdL << 16) - s_dcL) >> 10;
            s_dcR += (((int32_t)s_holdR << 16) - s_dcR) >> 10;
            l = s_holdL - (s_dcL >> 16);
            r = s_holdR - (s_dcR >> 16);
        }
        o[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        o[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

void pcm_setup(int hz) {
    s_hz = hz;
    s_dcL = s_dcR = 0;
    if (s_dev) SDL_CloseAudioDevice(s_dev);
    SDL_AudioSpec want = {}, have = {};
    want.freq = hz;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_cb;
    s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (s_dev) SDL_PauseAudioDevice(s_dev, 0);
    else SDL_Log("audio: %s", SDL_GetError());
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

// No line-in on the host.
bool pcm_data_in(void) { return false; }
void pwm_audio_in_frame_started(void) {}
void pcm_audio_in_stop(void) {}
