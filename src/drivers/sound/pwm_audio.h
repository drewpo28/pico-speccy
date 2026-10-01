#pragma once

void init_sound();
void pcm_setup(int hz);
void pcm_cleanup(void);
typedef volatile int16_t* (*pcm_end_callback_t)(volatile size_t* size);
// internal call on core#1
bool pcm_data_in(void);
void pwm_audio_in_frame_started(void);
#if LOAD_WAV_PIO
void pcm_audio_in_stop(void);
void inInit(uint gpio);
#endif

#define esp_err_t int
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_FAIL -1

/**
 * @brief Set volume for pwm audio
 *
 * @attention when the volume is too small, it will produce serious distortion
 *
 * @param volume Volume to set (-16 ~ 16).
 *        Set to 0 for original output;
 *        Set to less then 0 for attenuation, and -16 is mute;
 *        Set to more than 0 for enlarge, and 16 is double output
 *
 * @return
 *     - ESP_OK              Success
 *     - ESP_ERR_INVALID_ARG Parameter error
 */
esp_err_t pwm_audio_set_volume(int8_t volume);

/**
 * @brief Write data to play
 *
 * @param inbuf Pointer source data to write
 * @param len length of data in bytes
 * @param[out] bytes_written Number of bytes written, if timeout, the result will be less than the size passed in.
 * @param ticks_to_wait TX buffer wait timeout in RTOS ticks. If this
 * many ticks pass without space becoming available in the DMA
 * transmit buffer, then the function will return (note that if the
 * data is written to the DMA buffer in pieces, the overall operation
 * may still take longer than this timeout.) Pass portMAX_DELAY for no
 * timeout.
 *
 * @return
 *     - ESP_OK Success
 *     - ESP_FAIL Write encounter error
 *     - ESP_ERR_INVALID_ARG  Parameter error
 */
esp_err_t pwm_audio_write(
    uint8_t *bufL,
    uint8_t *bufR,
    size_t len,
    size_t *bytes_written,
    uint32_t wait_ms
);

// ── Pico-Zx-Player PCM source ──────────────────────────────────────────────────
// While `pcm_player_ring` is set the audio timer plays THIS instead of the
// emulator's frame buffer + the live GS: a ring of interleaved int16 L,R at the
// 31250 Hz output rate, filled by the player page on core0. Frame count is a
// power of two (`pcm_player_mask` = frames - 1); read/write are free-running
// frame counters (one writer each side). An empty ring plays silence, never a
// held sample. Set/cleared only by the player (pcm_player_attach/detach).
extern volatile int16_t* volatile pcm_player_ring;
extern volatile uint32_t pcm_player_mask;
extern volatile uint32_t pcm_player_r;
extern volatile uint32_t pcm_player_w;
void pcm_player_attach(int16_t* ring, uint32_t frames);
void pcm_player_detach();
