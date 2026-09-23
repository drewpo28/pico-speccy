#pragma once
// HSTX back-end for the VGA driver: the serializer in place of the one-instruction
// `out pins, 8` PIO program, on the boards whose display sits on GPIO 12-19.
//
// It exists for the COLOUR, not for the transport.  The PIO drives one byte per
// pixel — two bits per channel, 64 colours, everything else spread over a Bayer 2x2
// block.  HSTX drives FOUR sub-samples per pixel (one 32-bit FIFO word), which the
// resistor ladder and the monitor's input integrate into one level per pixel: no
// spatial pattern, no beating against 1-pixel detail, and the 16 flat ZX colours
// stop needing the solid / grid-snap fork.  See vga_pwm.h for the word.
//
// Compiled to nothing unless VGA_HSTX is set.
#include <stdint.h>
#include <stdbool.h>

#ifndef VGA_HSTX
#define VGA_HSTX 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Bring the serializer up for this mode's pixel clock.  Returns false when the
// clock is not 126 MHz / k for an integer k in 1..32, in which case NOTHING is
// started and the caller must stay on the PIO: running at the nearest reachable
// clock would be a wrong refresh, and with V-Sync on that is a wrong emulated
// speed, which is far worse than a mode that refuses to change.
bool vga_hstx_start(uint32_t pixel_hz);

// Re-point the engine at a new pixel clock (a video-mode change).  Same refusal
// rule; the engine is left running at the old rate when it says no.
bool vga_hstx_set_pixel_clk(uint32_t pixel_hz);

void vga_hstx_stop(void);

// Where the line DMA writes, and its DREQ.
volatile void *vga_hstx_fifo(void);
unsigned vga_hstx_dreq(void);

// clk_hstx cycles per pixel of the running mode (CSR.N_SHIFTS), 0 when stopped.
// The PWM phase weights follow from it — see vga_pwm_weights().
int vga_hstx_cycles_live(void);

// Per-boot diagnostics: the engine settings, the eight pads and what is actually
// driving them.  Built only with -DHDMI_HSTX_TRACE=ON, beside the HDMI one.
void vga_hstx_dump_config(void);

// The HSTX FIFO status register: level, empty/full and the sticky "written while
// full" bit.  The line DMA is paced by DREQ_HSTX, so a clean WOF plus the right
// line-IRQ rate is the whole proof that the serializer is draining and the chain
// keeps up — and neither needs a monitor.
uint32_t vga_hstx_fifo_stat(void);

#ifdef __cplusplus
}
#endif
