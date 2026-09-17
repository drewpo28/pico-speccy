#pragma once
// HSTX back-end for the HDMI driver: the serializer that replaces the 10-instruction
// TMDS PIO program on the boards whose display sits on GPIO 12-19. Everything above
// the wire is unchanged — the line ISR still writes palette indices, the converter
// SM still turns them into LUT addresses, and the DMA chain still feeds two words
// per index; only the sink and the word format move (see hdmi_word.h).
//
// Compiled to nothing unless HDMI_HSTX is set.
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring the serializer up for a TMDS bit clock of `tmds_mhz` (252 = the 25.2 MHz
// pixel every standard mode uses). Configures clk_hstx, the shift/clock engine, the
// per-pin lane map and the eight pads. Returns false when clk_sys cannot produce
// the clock with an integer divider — it still runs, at the nearest divider, and
// says so in the log, because a wrong video clock is a picture worth looking at.
bool hdmi_hstx_start(unsigned tmds_mhz);

// Stops the engine (pads keep whatever they were last set to). Safe before a restart.
void hdmi_hstx_stop(void);

// Diagnostics. `dump_config` prints what the hardware was actually given — the
// engine settings, the per-pin lane map and the eight function selects — and
// `fifo_stat` is the HSTX FIFO status register: level, empty/full, and the sticky
// "written while full" bit. Between them they answer "is the serializer really the
// thing driving these pins, and is it keeping up".
// Built only with -DHDMI_HSTX_TRACE=ON.
void hdmi_hstx_dump_config(void);
uint32_t hdmi_hstx_fifo_stat(void);

// The FIFO the palette DMA writes into. An accessor so every HSTX register name
// stays inside hdmi_hstx.c — hdmi.c only needs to know where its words go.
volatile void *hdmi_hstx_fifo(void);

// The pixel clock this back-end produces for a given TMDS rate: five clk_hstx
// cycles per pixel, with clk_hstx an integer divide of clk_sys.
//
// PURE ARITHMETIC, and that is the whole point — it must NOT read the live clk_hstx.
// hdmi_audio_hw_init() runs on core0 inside ESPectrum::setup(), which is BEFORE core1
// reaches graphics_init() and configures the serializer, so a register read there
// returns the SDK's boot default (150 MHz -> a 30 MHz "pixel clock"). That fed the
// ACR a CTS of 30000 instead of 25200 and the sample pacing 37500 lines/s instead of
// 31500 — audio delivered at 40.3 kHz against the 48 kHz the sink was told to expect,
// i.e. sound that plays and then breaks up (hw 2026-09-17, PCp2). The PIO path never
// had the problem because it derives the same number from clk_sys and the mode.
uint32_t hdmi_hstx_pixel_hz(unsigned tmds_mhz);

#ifdef __cplusplus
}
#endif
