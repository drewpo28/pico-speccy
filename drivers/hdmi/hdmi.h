#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "inttypes.h"
#include "stdbool.h"

#include "hardware/pio.h"

// HDMI lives on pio2 — deliberately, and it must not move back to pio0.
//
// A PIO holds 32 instructions. HDMI needs 18 of them (address converter 8 +
// TMDS output 10), and on MURM1 pio0 is already half full when video comes up:
// it is the only board whose SPI PSRAM runs through PIO (init_psram(), called
// well before graphics_init()), and that costs 18-20 instructions (spi_psram 9
// or its fudge variant 10, plus spi_psram_32 9/10). 18 + 18 > 32, so
// pio_add_program() hard_asserts — on core1, inside graphics_init(), which core0
// does not wait for outside SOFTTV builds. The firmware then runs perfectly with
// no video at all, which is exactly what m1 showed from v1.0.1 on (the converter
// grew 4 -> 8 instructions with the two-page CRT palette).
//
// pio2 exists on every RP2350 (this firmware is RP2350-only) and is unused by
// everything else here: pio0 = PSRAM / VGA / SOFTTV / ST7789, pio1 = PS/2
// keyboard, NESPAD, I2S audio, PICO_DV's SD. That leaves 14 free instructions on
// both pio0 and pio2 instead of a -4 overflow.
#define PIO_VIDEO pio2
#define PIO_VIDEO_ADDR pio2
#define VIDEO_DMA_IRQ (DMA_IRQ_0)

#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN (6)
#endif

#if defined(ZERO2)
	#define HDMI_PIN_RGB_notBGR (0)
	#define HDMI_PIN_invert_diffpairs (0)
	#define beginHDMI_PIN_data (HDMI_BASE_PIN)
	#define beginHDMI_PIN_clk (HDMI_BASE_PIN + 6)
#else
	#define HDMI_PIN_RGB_notBGR (1)
	#define HDMI_PIN_invert_diffpairs (1)
	#define beginHDMI_PIN_clk (HDMI_BASE_PIN)
	#define beginHDMI_PIN_data (HDMI_BASE_PIN+2)
#endif

#define TEXTMODE_COLS 53
#define TEXTMODE_ROWS 30

// HDMI audio support (RP2350 only)
// Allocates the packet queue + sample rings (~36.9 KB) on first call and
// brings the audio hardware tables up. Returns false on OOM or if the video
// mode can't carry Data Islands. Managed by HdmiAudioSubsys — prefer
// HdmiAudioSubsys::request() over calling this directly.
bool hdmi_audio_init(void);
// Stops island emission (waits out the in-flight core1 ISR) and frees the
// buffers allocated by hdmi_audio_init().
void hdmi_audio_deinit(void);
void hdmi_audio_write_sample(int16_t left, int16_t right);
// Diagnostic stage of the staged-injection debug build:
// -1 = audio disabled or staging compiled out, 0..3 = active stage
int hdmi_audio_dbg_stage(void);
// Producer/consumer counters: packet queue (wr/rd) and sample ring (wr/rd)
void hdmi_audio_dbg_stats(uint32_t *q_prod, uint32_t *q_cons, uint32_t *s_prod, uint32_t *s_cons);
// 1 Hz "HDMIAU:" health line (queue watermarks, skips/duplicates/underruns,
// worst ISR gap+duration). No-op unless HDMI audio is live. Call from the
// core0 main loop about once a second.
void hdmi_audio_health_dump(void);

// Hot video mode reinit (reuses existing PIO/DMA resources)
void hdmi_reinit(void);
// Call from core1 loop to process pending reinit
void hdmi_poll_reinit(void);

// TODO: Сделать настраиваемо
static const uint8_t textmode_palette[16] = {
    200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215
};

#ifdef __cplusplus
}
#endif
