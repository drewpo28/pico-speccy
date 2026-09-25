#include "graphics.h"
#include "hardware/clocks.h"
#include "stdbool.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/systick.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include <stdio.h>
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "stdlib.h"
#include "vga_hstx.h"

// ---------------------------------------------------------------------------
// One output pixel is a BYTE on the PIO (`out pins, 8`) and a 32-BIT WORD on the
// HSTX serializer, where it carries four PWM sub-samples instead of one 2-bit-per-
// channel value (vga_pwm.h).  Everything between the palette and the wire is
// written in PAIRS — two output pixels, which is what one source byte becomes —
// so the render loop, the pointer arithmetic and the palette tables are the same
// code either way and only the type and three DMA numbers move.
// ---------------------------------------------------------------------------
#include "vga_pwm.h"

// A pair is TWO output pixels — what one source byte becomes — in one of two
// widths, chosen at boot:
//
//   NARROW (2 B): one VGA pin byte per pixel, the 2-bit-per-channel DAC code the
//     Bayer 2x2 block quantised the colour to.  What the PIO has always driven.
//   WIDE   (8 B): four bytes per pixel = four PWM sub-samples, integrated by the
//     ladder into one level per pixel — no spatial pattern, and no solid /
//     grid-snap fork (vga_pwm.h).  On the PIO that means the SM runs at FOUR
//     times the pixel clock and every number below that counts pixels in bytes
//     moves with it; on HSTX one pixel IS one 32-bit FIFO word, so wide is the
//     only shape the transport has.
//
// The two flags are separate because HSTX pins the WIDTH but not the CONTENT:
// with PWM off it still sends 32-bit words, they just carry the same byte four
// times — which is the bisect that tells a wrong colour table from a wrong
// transport, and on HSTX it is the only way to get the PIO's picture.
typedef struct { uint32_t px[2]; } vga_wide_pair_t;   // 8 B
typedef uint16_t                   vga_flat_pair_t;   // 2 B

static bool vga_wide     = (VGA_HSTX != 0);   // 4 bytes per output pixel
static bool vga_pwm_live = (VGA_HSTX != 0);   // ...and they carry PWM phases

// Bytes one output pixel occupies, and the 32-bit DMA transfers one line needs.
static inline int vga_px_bytes(void) { return vga_wide ? 4 : 1; }
// ...published, because the SM runs that many times faster than the pixel clock and
// Video > Mode prints the divider the PIO is actually given.
int vga_sm_px_bytes(void) { return vga_px_bytes(); }
// ...and whether the pixels really carry PWM.  Not the same as Config::vga_pwm:
// the level table is heap and a failed allocation falls back to the dither.
int vga_pwm_active(void) { return vga_pwm_live ? 1 : 0; }

// The PWM level table.  On the PIO a pixel is exactly four equal phase slots,
// which is vga_pwm_weights()'s k=2 (2k = 4 half-cycles, weights 1,1,1,1, 13
// levels); on HSTX k is the mode's clk_hstx cycles per pixel and the weights can
// be unequal.  Rebuilt whenever that changes.
#define VGA_PWM_PIO_K 2
// Heap, and only while PWM is live: 1 KB of .bss on every VGA board for a table a
// narrow build never reads is not worth it, and a failed allocation has an honest
// answer — fall back to the narrow path, which is what the board did before.
static vga_pwm_tab_t *vga_pwm_tab = 0;

// A run of `n` identical output pixels, given the VGA pin byte.
static inline void vga_fill_px(void *dst, uint8_t pixel_byte, int n) {
    if (vga_wide) {
        uint32_t *d = (uint32_t *)dst;
        const uint32_t w = vga_pwm_flat_word(pixel_byte);
        while (n--) *d++ = w;
    } else {
        memset(dst, pixel_byte, (size_t)n);
    }
}

// Config::vga_pwm, published the way Config::video_driver is — this file is C and
// cannot see Config.h.  Read ONCE, into the two flags above: the widths decide how
// the palette tables and the line templates are sized, so the setting is
// reboot-class and must not move under a running allocation.
extern uint8_t vga_pwm_cfg;

static bool vga_flags_done = false;

int get_video_mode(void);   // declared again below with the other TODO: .h externs

// clk_hstx cycles per pixel for the PWM phase weights.  On the PIO a pixel is
// always four equal phase slots; on HSTX it is the mode's own k.
static int vga_pwm_k(void) {
#if VGA_HSTX
    struct video_mode_t vm = graphics_get_video_mode(get_video_mode());
    const uint32_t px = vm.vga_pixel_clk ? (uint32_t)vm.vga_pixel_clk : (uint32_t)vm.pixel_clk;
    const int k = vga_hstx_cycles(px);
    return k ? k : 5;
#else
    return VGA_PWM_PIO_K;
#endif
}

static void vga_flags_init(void) {
    if (vga_flags_done) return;
    vga_flags_done = true;
    vga_pwm_live = (vga_pwm_cfg != 0);
    // HSTX has no narrow shape: one pixel IS one 32-bit FIFO word, so the width is
    // pinned and the setting only chooses what those four bytes carry.
    vga_wide = VGA_HSTX ? true : vga_pwm_live;
    if (!vga_wide) vga_pwm_live = false;   // a 2-byte pair has nowhere to put phases
    if (vga_pwm_live) {
        extern size_t getLargestAllocatable(void);
        if (getLargestAllocatable() >= sizeof(vga_pwm_tab_t))
            vga_pwm_tab = (vga_pwm_tab_t *)calloc(1, sizeof(vga_pwm_tab_t));
        if (!vga_pwm_tab) {
            printf("vga: no room for the PWM level table (%u B) - falling back to the dither\n",
                   (unsigned)sizeof(vga_pwm_tab_t));
            vga_pwm_live = false;
#if !VGA_HSTX
            vga_wide = false;
#endif
        } else {
            vga_pwm_build(vga_pwm_tab, vga_pwm_k());
        }
    }
}

// One built pair, in both representations: the palette builders make it once and
// vga_pal_put() stores whichever the boot chose.  Cheap — this runs per palette
// entry, never per pixel.
typedef struct { uint32_t w[2]; uint16_t flat; } vga_pairv_t;

static inline void vga_pal_put(void *arr, unsigned i, vga_pairv_t v) {
    if (vga_wide) {
        uint32_t *d = (uint32_t *)arr + 2u * i;
        d[0] = v.w[0]; d[1] = v.w[1];
    } else {
        ((uint16_t *)arr)[i] = v.flat;
    }
}

/// TODO: .h
bool SELECT_VGA = false;
uint8_t* getLineBuffer(int line);
void ESPectrum_vsync();
int get_video_mode();

uint16_t pio_program_VGA_instructions[] = {
    //     .wrap_target
    0x6008, //  0: out    pins, 8
    //     .wrap
};

const struct pio_program pio_program_VGA = {
    .instructions = pio_program_VGA_instructions,
    .length = 1,
    .origin = -1,
};


static uint32_t* lines_pattern[4];
static uint32_t* lines_pattern_data = NULL;
static int _SM_VGA = -1;


//static int N_lines_total = 525;
//static int N_lines_visible = 480;
static int line_VS_begin = 490;
static int line_VS_end = 491;
static int shift_picture = 0;

static int visible_line_size = 320;
static int line_size = 800;
static int HS_SIZE = 96;
static int HS_SHIFT = 656;

// VGA_MAX_LINE_SIZE (video_modes.h) is the longest line any mode asks for; the
// templates are rendered in place on a mode switch, so they are sized to it and a
// smaller mode uses only part of each.
// 32-bit DMA transfers per line, and the words one line template occupies.  A
// NARROW line packs four pixel bytes into a word; a WIDE one is a word per pixel.
// (line_size is always a multiple of 4 — see tools/vga_timing_test.c — so the
// narrow form never truncates.)
static inline int VGA_DMA_WORDS(int line) { return line * vga_px_bytes() / 4; }
static inline int VGA_TMPL_WORDS(void)    { return VGA_MAX_LINE_SIZE * vga_px_bytes() / 4; }
// Bytes one palette entry (a pair) occupies, and the row of 256 for one parity.
static inline size_t vga_pair_bytes(void) { return vga_wide ? sizeof(vga_wide_pair_t)
                                                            : sizeof(vga_flat_pair_t); }
static inline void *pal_row(void *arr, unsigned parity) {
    return (uint8_t *)arr + (size_t)parity * 256u * vga_pair_bytes();
}

// Sync byte templates set during init. Stored to allow re-rendering line patterns
// when the mode (and thus HS_SIZE / line_size) changes at runtime.
static uint8_t TMPL_LINE8_g = 0b11000000;
static uint8_t TMPL_HS8_g   = 0b10000000;
static uint8_t TMPL_VS8_g   = 0b01000000;
static uint8_t TMPL_VHS8_g  = 0b00000000;


static int dma_chan_ctrl;
static int dma_chan;

/// TODO:
int graphics_buffer_width = 0;
int graphics_buffer_height = 0;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;

static bool is_flash_line = false;
static bool is_flash_frame = false;
bool vga_scanlines = false;
// Scanline brightness level: 0=off, 1=darkest .. 4=lightest. Level 2 is the
// legacy ~50% look and the default. Drives dim_rgb888() when (re)building the
// dimmed palette. vga_scanlines stays a fast on/off flag for the render path.
static uint8_t vga_scanline_level = 2;

// Background: the two output-pixel pairs of one 2x2 group (phase A then phase B),
// per line parity.  bg_pair[parity][phase].
static vga_pairv_t bg_pair[2][2];
static uint16_t palette16_mask = 0;

// VGA8 dithered palette LUT: /42 checkerboard dithering (7 levels per channel, 343 colors)
// [0][i] = even scanline pixel pair,  [1][i] = odd scanline pixel pair
// pico-speccy: these VGA-only tables (~4 KB total) are lazily heap-allocated and only
// exist when VGA is the active output (SELECT_VGA). On HDMI boots they stay NULL —
// the setters below no-op — so the SRAM isn't reserved. See vga_alloc_buffers().
static void *palette_vga16 = 0;            // 2 x 256 pairs (even/odd line)

// CRT mask phase-B tables: ODD source pixels index these instead, so the mask
// profile spans 4 output pixels (2 source pixels x 2 doubled pixels) with four
// independent levels — the same scheme as the HDMI backend's second palette page.
// Allocated only when the filter is switched on (~2.5 KB); while it is off, and if
// the allocation fails, these alias the A tables, which degrades to a plain
// period-2 grille rather than to nothing.
static void *palette_vga16_b = 0;
static void *palette_vga16_scanline_b = 0;
static void *palette_vga_ds80_b = 0;
static bool vga_crt_phaseb = false;    // true once the B tables are real, not aliases

// Scanline dimmed palette: dithered at reduced brightness for scanline effect.
// Rebuilt from vga_color888[] whenever the scanline level changes.
static void *palette_vga16_scanline = 0;   // 256 pairs (odd lines only)
// Original RGB888 per palette index + whether it was set via the solid path.
// Cached so the dimmed palette can be rebuilt on a brightness-level change
// without re-walking the whole palette from the emulator side.
static uint32_t *vga_color888 = (uint32_t *) 0;
static bool     *vga_color_solid = (bool *) 0;

// Profi DS80 packed-pair palette: slot byte → uint16_t with two distinct VGA pixels.
// Built by vga_set_profi_ds80_mode(); slot comes from profi_pair_lookup[p0][p1].
// low byte = left pixel (p0), high byte = right pixel (p1) — PIO right-shifts out LSB first.
// [0] = even scan lines, [1] = odd scan lines (Bayer 2×2 checkerboard dithering).
// The 1 KB DS80 palette is heap-allocated on demand to save RAM
// (heap is ~5 KB after the framebuffer there).
static void *palette_vga_ds80 = 0;         // pico-speccy: lazy (see above)

// Allocate the VGA-only palette tables on first use (VGA active only). Idempotent.
// Keeps ~4 KB out of .bss on HDMI boots, where these are never touched.
// pico_malloc/calloc PANIC on OOM instead of returning NULL, so pre-check the
// largest satisfiable block (same guard as Buffer::palloc / graphics_init_hdmi)
// — this runs on core1 right after setup, when free heap can be only a few KB.
static void vga_alloc_buffers(void) {
    if (vga_color888) return;                                   // already allocated
    // The widths and the PWM table have to be settled before the first palette
    // entry is packed, and core0 can get here (VIDEO::Init -> applyPalette) before
    // core1 has reached graphics_init.
    vga_flags_init();
    extern size_t getLargestAllocatable(void);
    const size_t pb = vga_pair_bytes();
    size_t need = 2 * 256 * pb + 256 * pb
                + 256 * sizeof(uint32_t) + 256 * sizeof(bool);
    need += 2 * 256 * pb;
    if (getLargestAllocatable() < need) {
        printf("vga_alloc_buffers: OOM allocating VGA palette tables (%u B needed)\n", (unsigned)need);
        return;
    }
    palette_vga16          = calloc(2 * 256, pb);
    palette_vga16_scanline = calloc(    256, pb);
    vga_color888           = (uint32_t *)        calloc(256,     sizeof(uint32_t));
    vga_color_solid        = (bool *)            calloc(256,     sizeof(bool));
    palette_vga_ds80       = calloc(2 * 256, pb);
    // Mask off by default: phase B aliases phase A, so the render loop's second
    // lookup is identical to the first and the output matches the old single-table
    // code exactly. vga_set_crt() promotes these to real tables when needed.
    palette_vga16_b           = palette_vga16;
    palette_vga16_scanline_b  = palette_vga16_scanline;
    palette_vga_ds80_b        = palette_vga_ds80;
    vga_crt_phaseb = false;
}

// True once the VGA tables exist. On HDMI (SELECT_VGA false) returns false so the
// palette setters become no-ops; on VGA it allocates lazily on first call.
static inline bool vga_buffers_ready(void) {
    if (vga_color888) return true;
    if (!SELECT_VGA) return false;
    vga_alloc_buffers();
    return vga_color888 != (uint32_t *) 0;
}
// Unified DS80-active flag: set by both vga_set_profi_ds80_mode and hdmi_set_profi_ds80_mode.
volatile bool profi_ds80_active = false;

static uint text_buffer_width = 0;
static uint text_buffer_height = 0;

static uint16_t txt_palette[16];

//буфер 2К текстовой палитры для быстрой работы
static uint16_t* txt_palette_fast = NULL;
//static uint16_t txt_palette_fast[256*4];

enum graphics_mode_t graphics_mode = GRAPHICSMODE_DEFAULT;

// Beam position exposed to core0 — the HDMI driver's hdmi_current_line twin.
volatile uint32_t vga_current_line = 0;
volatile uint32_t vga_current_v_active = 0;

volatile uint32_t vga_vsync_line = 0;      // see hdmi_vsync_line
void vga_set_vsync_line(uint32_t line) { vga_vsync_line = line; }

// Framebuffer row under the scanout beam (two lines per row), -1 in vertical
// blanking. See hdmi_beam_row() for the consumer.
int vga_beam_row(void) {
    const uint32_t l = vga_current_line;
    return (l < vga_current_v_active) ? (int)(l >> 1) : -1;
}

static void vga_fill_pairs(uint8_t i, uint32_t color888, bool solid);
static void vga_build_scanline_entry(uint8_t i);
void graphics_set_bgcolor(uint32_t color888);
// Last background colour, so the sync bits and the PWM table can be re-applied to
// it the same way they are to the palette.
static uint32_t vga_bg888 = 0;

// Re-pack every palette entry from its cached RGB888.  Needed whenever something
// the PACKING depends on changes and the colours do not: palette16_mask (the idle
// sync bits, only known once the mode is set) and, on HSTX, the PWM phase table
// (the pixel clock, and with it the cycles per pixel, is per mode).  Patching bits
// into the packed pairs instead only ever worked for the PIO representation.
static void vga_repack_palette(void) {
    if (!vga_color888) return;
    for (int i = 0; i < 256; i++) {
        vga_fill_pairs((uint8_t)i, vga_color888[i], vga_color_solid[i]);
        vga_build_scanline_entry((uint8_t)i);
    }
    graphics_set_bgcolor(vga_bg888);
}

#if VGA_HSTX
// Point the serializer at a mode's pixel clock and rebuild everything that depends
// on it.  The PWM phase weights are a function of the cycles per pixel, so a mode
// change is also a palette change — which is why this is one call and not two.
static void vga_hstx_apply_mode(uint32_t pixel_hz) {
    const int k_before = vga_hstx_cycles_live();
    if (!vga_hstx_set_pixel_clk(pixel_hz)) return;
    const int k = vga_hstx_cycles_live();
    if (k == k_before) return;
    if (vga_pwm_tab) vga_pwm_build(vga_pwm_tab, k);
    vga_repack_palette();
}
#endif

#if !VGA_HSTX
// The SM emits one BYTE per cycle (`out pins, 8`), so a narrow line wants the SM at
// the pixel clock and a wide one at FOUR times it — four phase bytes per pixel.
// Keep the legacy 1/16 divider quantisation for the PIO timing table. PWM
// changes the byte rate, but the requested output pixel rate stays the same.
static void vga_pio_set_pixel_clk(uint32_t pixel_hz) {
    if (_SM_VGA < 0 || !pixel_hz) return;
    const double fdiv = (double)clock_get_hz(clk_sys) / ((double)pixel_hz * vga_px_bytes());
    const uint32_t div32 = (uint32_t)(fdiv * (1 << 16) + 0.0);
    PIO_VGA->sm[_SM_VGA].clkdiv = div32 & 0xfffff000;  // quantised to 1/16, as ever
}
#endif

void __time_critical_func() dma_handler_VGA() {
    dma_hw->ints0 = 1u << dma_chan_ctrl;
#if VGA_HSTX && HDMI_HSTX_TRACE
    // The line DMA is paced by DREQ_HSTX, so this rate IS the line rate — and
    // multiplied by h_total it is the pixel clock the serializer is really running
    // at.  It is the measurement that proved the HDMI side on PCp2, and it is the
    // only one available on a board whose VGA pins go nowhere: if the engine, the
    // clock or the chain were wrong, the rate would be wrong or the chain would
    // stall here rather than in a picture nobody can see.
    {
        static uint32_t n = 0;
        static uint64_t t0 = 0;
        const uint64_t now = time_us_64();
        if (!t0) t0 = now;
        else if (++n >= 25000) {
            printf("vga_hstx: %u line IRQs/s (x%d px = %u Hz pixel), fifo_stat=%08x\n",
                   (unsigned)((uint64_t)n * 1000000u / (now - t0)), line_size,
                   (unsigned)((uint64_t)n * 1000000u * (uint32_t)line_size / (now - t0)),
                   (unsigned)vga_hstx_fifo_stat());
            n = 0; t0 = now;
        }
    }
#endif
    static uint32_t frame_number = 0;
    static uint32_t screen_line = 0;
    static uint8_t* input_buffer = NULL;
    screen_line++;

    struct video_mode_t mode = graphics_get_video_mode(get_video_mode());
    int v_total = mode.vga_v_total ? mode.vga_v_total : mode.v_total;
    int v_active = mode.vga_v_active ? mode.vga_v_active : mode.v_active;
    vga_current_line = screen_line;          // beam position for vga_beam_row()
    vga_current_v_active = (uint32_t)v_active;

    if (screen_line == v_total) {
        screen_line = 0;
        frame_number++;
        input_buffer = getLineBuffer(screen_line);
    }

    // Signal vsync at start of blanking (after last visible line), so emulator
    // renders the next frame during blanking while VGA isn't reading frameBuffer —
    // prevents tearing at the top of the screen.
    {
        const uint32_t vs = vga_vsync_line;
        if (screen_line == (vs ? vs : (uint32_t)v_active)) ESPectrum_vsync();
    }

    if (screen_line >= v_active) {
        //заполнение цветом фона
        if (screen_line == v_active | screen_line == v_active + 3) {
            uint32_t p_i = (screen_line & is_flash_line) + (frame_number & is_flash_frame) & 1;
            const vga_pairv_t a = bg_pair[p_i][0], b = bg_pair[p_i][1];
            if (vga_wide) {
                vga_wide_pair_t *o = (vga_wide_pair_t *)lines_pattern[2 + (screen_line & 1)] + shift_picture / 2;
                const vga_wide_pair_t aw = { { a.w[0], a.w[1] } };
                const vga_wide_pair_t bw = { { b.w[0], b.w[1] } };
                for (int i = visible_line_size / 2; i--;) { *o++ = aw; *o++ = bw; }
            } else {
                vga_flat_pair_t *o = (vga_flat_pair_t *)lines_pattern[2 + (screen_line & 1)] + shift_picture / 2;
                for (int i = visible_line_size / 2; i--;) { *o++ = a.flat; *o++ = b.flat; }
            }
        }

        //синхросигналы
        if (screen_line >= line_VS_begin && screen_line <= line_VS_end)
            dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[1], false); //VS SYNC
        else
            dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
        return;
    }

    if (!input_buffer) {
        dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
        return;
    } //если нет видеобуфера - рисуем пустую строку

    int y;

    uint32_t* * output_buffer = &lines_pattern[2 + (screen_line & 1)];
    switch (graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
            y = screen_line / 2 - graphics_buffer_shift_y;
            break;
/**
        case TEXTMODE_160x100:
        case TEXTMODE_53x30:
        case TEXTMODE_DEFAULT: {
            uint16_t* output_buffer_16bit = (uint16_t *)*output_buffer;
            output_buffer_16bit += shift_picture / 2;
            const uint font_height = 16;

            // "слой" символа
            uint32_t glyph_line = screen_line % font_height;
if (!text_buffer) return;
            //указатель откуда начать считывать символы
            uint8_t* text_buffer_line = &text_buffer[screen_line / font_height * text_buffer_width * 2];

            for (int x = 0; x < text_buffer_width; x++) {
                //из таблицы символов получаем "срез" текущего символа
                uint8_t glyph_pixels = font_8x16[*text_buffer_line++ * font_height + glyph_line];
                //считываем из быстрой палитры начало таблицы быстрого преобразования 2-битных комбинаций цветов пикселей
                uint16_t* color = &txt_palette_fast[*text_buffer_line++ * 4];
#if 0
                if (cursor_blink_state && !manager_started &&
                    (screen_line / 16 == CURSOR_Y && x == CURSOR_X && glyph_line >= 11 && glyph_line <= 13)) {
                    *output_buffer_16bit++ = color[3];
                    *output_buffer_16bit++ = color[3];
                    *output_buffer_16bit++ = color[3];
                    *output_buffer_16bit++ = color[3];
                    if (text_buffer_width == 40) {
                        *output_buffer_16bit++ = color[3];
                        *output_buffer_16bit++ = color[3];
                        *output_buffer_16bit++ = color[3];
                        *output_buffer_16bit++ = color[3];
                    }
                }
                else
#endif
                {
                    *output_buffer_16bit++ = color[glyph_pixels & 3];
                    if (text_buffer_width == 40) *output_buffer_16bit++ = color[glyph_pixels & 3];
                    glyph_pixels >>= 2;
                    *output_buffer_16bit++ = color[glyph_pixels & 3];
                    if (text_buffer_width == 40) *output_buffer_16bit++ = color[glyph_pixels & 3];
                    glyph_pixels >>= 2;
                    *output_buffer_16bit++ = color[glyph_pixels & 3];
                    if (text_buffer_width == 40) *output_buffer_16bit++ = color[glyph_pixels & 3];
                    glyph_pixels >>= 2;
                    *output_buffer_16bit++ = color[glyph_pixels & 3];
                    if (text_buffer_width == 40) *output_buffer_16bit++ = color[glyph_pixels & 3];
                }
            }
            dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
            return;
        }
*/
        default: {
            dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false); // TODO: ensue it is required
            return;
        }
    }

    if (y < 0) {
        dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false); // TODO: ensue it is required
        return;
    }
    if (y >= graphics_buffer_height) {
        // заполнение линии цветом фона
        if (y == graphics_buffer_height | y == graphics_buffer_height + 1 |
            y == graphics_buffer_height + 2) {
            uint32_t p_i = ((screen_line & is_flash_line) + (frame_number & is_flash_frame)) & 1;
            const vga_pairv_t a = bg_pair[p_i][0], b = bg_pair[p_i][1];
            if (vga_wide) {
                vga_wide_pair_t *o = (vga_wide_pair_t *)*output_buffer + shift_picture / 2;
                const vga_wide_pair_t aw = { { a.w[0], a.w[1] } };
                const vga_wide_pair_t bw = { { b.w[0], b.w[1] } };
                for (int i = visible_line_size / 2; i--;) { *o++ = aw; *o++ = bw; }
            } else {
                vga_flat_pair_t *o = (vga_flat_pair_t *)*output_buffer + shift_picture / 2;
                for (int i = visible_line_size / 2; i--;) { *o++ = a.flat; *o++ = b.flat; }
            }
        }
        dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
        return;
    };

    //зона прорисовки изображения
    //начальные точки буферов
    uint8_t* input_buffer_8bit ///= input_buffer + y / 2 * 80 + (y & 1) * 8192;
             = getLineBuffer(y);

    // Offset of the first picture pair, in PAIRS — the width only decides how many
    // bytes a pair is, never how many pairs a line holds.
    int out_idx = shift_picture / 2; //смещение началы вывода на размер синхросигнала

    //    g_buf_shx&=0xfffffffe;//4bit buf
    //    graphics_buffer_shift_x &= 0xfffffff1; //1bit buf
        graphics_buffer_shift_x &= 0xfffffff2; //2bit buf

    //для div_factor 2
    uint max_width = graphics_buffer_width;
    if (graphics_buffer_shift_x < 0) {
        //vbuf8-=g_buf_shx; //8bit buf
///            input_buffer_8bit -= graphics_buffer_shift_x / 8; //1bit buf
            input_buffer_8bit -= graphics_buffer_shift_x / 4; //2bit buf
        max_width += graphics_buffer_shift_x;
    }
    else {
#define div_factor (2)
        out_idx += graphics_buffer_shift_x * 2 / div_factor;
    }


    int width = MIN((visible_line_size - ((graphics_buffer_shift_x > 0) ? (graphics_buffer_shift_x) : 0)), max_width);
    if (width < 0) return; // TODO: detect a case

    // Индекс палитры в зависимости от настроек чередования строк и кадров
///    uint16_t* current_palette = palette[(y & is_flash_line) + (frame_number & is_flash_frame) & 1];

    switch (graphics_mode) {
        case GRAPHICSMODE_DEFAULT: {
            // Unrolled by 2 so even/odd source pixels can read different tables
            // (the CRT mask's phase A / phase B). Instruction count per pixel is
            // unchanged — it is the same load+lookup+store, just from a second base
            // register — so this costs nothing when the mask is off (pal_b == pal).
            // width is 320 or 360, always even; the x^2 swizzle pairs up cleanly
            // (x=0..7 -> 2,3,0,1,6,7,4,5).
            void *pal, *pal_b;
            if (profi_ds80_active) {
                pal   = pal_row(palette_vga_ds80,   screen_line & 1);
                pal_b = pal_row(palette_vga_ds80_b, screen_line & 1);
            } else {
                const bool sl = vga_scanlines && (screen_line & 1);
                pal   = sl ? palette_vga16_scanline   : pal_row(palette_vga16,   screen_line & 1);
                pal_b = sl ? palette_vga16_scanline_b : pal_row(palette_vga16_b, screen_line & 1);
            }
            // One branch per LINE, not per pixel: both loops are the same
            // load-lookup-store, only the pair is 8 bytes instead of 2.
            if (vga_wide) {
                vga_wide_pair_t *o = (vga_wide_pair_t *)(*output_buffer) + out_idx;
                const vga_wide_pair_t *p = (const vga_wide_pair_t *)pal;
                const vga_wide_pair_t *pb = (const vga_wide_pair_t *)pal_b;
                for (int x = 0; x < width; x += 2) {
                    *o++ = p [input_buffer_8bit[ x      ^ 2]];
                    *o++ = pb[input_buffer_8bit[(x + 1) ^ 2]];
                }
            } else {
                vga_flat_pair_t *o = (vga_flat_pair_t *)(*output_buffer) + out_idx;
                const vga_flat_pair_t *p = (const vga_flat_pair_t *)pal;
                const vga_flat_pair_t *pb = (const vga_flat_pair_t *)pal_b;
                for (int x = 0; x < width; x += 2) {
                    *o++ = p [input_buffer_8bit[ x      ^ 2]];
                    *o++ = pb[input_buffer_8bit[(x + 1) ^ 2]];
                }
            }
            break;
        }
        default:
            break;
    }
    dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
}

// The four line templates are INDEPENDENT buffers: the control channel is pointed
// at one of them per line and the data channel never runs past its end, so nothing
// requires them to be adjacent.  Allocating them separately turns one 14 KB
// contiguous request into four of 3.5 KB — the difference between fitting and not
// on the heap this runs against, where a fragmented 16 KB hole is exactly what is
// missing.  pico_calloc PANICs on OOM instead of returning NULL, so every block is
// pre-checked against the largest satisfiable one; a partial set is freed rather
// than left for the ISR to read from.
static bool vga_alloc_templates(void) {
    extern size_t getLargestAllocatable(void);
    const size_t words = (size_t)VGA_TMPL_WORDS();
    for (int i = 0; i < 4; i++) {
        if (getLargestAllocatable() < words * sizeof(uint32_t)) {
            for (int j = 0; j < i; j++) { free(lines_pattern[j]); lines_pattern[j] = NULL; }
            return false;
        }
        lines_pattern[i] = (uint32_t *)calloc(words, sizeof(uint32_t));
    }
    lines_pattern_data = lines_pattern[0];   // the "templates exist" flag
    return true;
}

void graphics_set_mode(enum graphics_mode_t mode) {
    if (!SELECT_VGA) {
        graphics_mode = mode;
        return;
    }
    vga_alloc_buffers();   // VGA active — the palette-mask loop below needs the tables
    text_buffer_width = 80;
    text_buffer_height = 30;
///    memset(graphics_buffer, 0, graphics_buffer_height * graphics_buffer_width);
    if (_SM_VGA < 0) return; // если  VGA не инициализирована -

    graphics_mode = mode;

    // Если мы уже проиницилизированы - выходим
    if (txt_palette_fast && lines_pattern_data) {
        return;
    };
    uint8_t TMPL_VHS8 = 0;
    uint8_t TMPL_VS8 = 0;
    uint8_t TMPL_HS8 = 0;
    uint8_t TMPL_LINE8 = 0;

    uint32_t vga_px_hz = 0;
    // line_size, HS_SIZE, HS_SHIFT are now globals — assigned from video_mode below

    switch (graphics_mode) {
        case TEXTMODE_DEFAULT:
            //текстовая палитра
            for (int i = 0; i < 16; i++) {
                txt_palette[i] = txt_palette[i] & 0x3f | palette16_mask >> 8;
            }

            if (!txt_palette_fast) {
                txt_palette_fast = (uint16_t *)calloc(256 * 4, sizeof(uint16_t));
                for (int i = 0; i < 256; i++) {
                    const uint8_t c1 = txt_palette[i & 0xf];
                    const uint8_t c0 = txt_palette[i >> 4];

                    txt_palette_fast[i * 4 + 0] = c0 | c0 << 8;
                    txt_palette_fast[i * 4 + 1] = c1 | c0 << 8;
                    txt_palette_fast[i * 4 + 2] = c0 | c1 << 8;
                    txt_palette_fast[i * 4 + 3] = c1 | c1 << 8;
                }
            }
        case GRAPHICSMODE_DEFAULT: {
            TMPL_LINE8 = 0b11000000;
            palette16_mask = 0xc0c0;
            line_VS_begin = 490;
            line_VS_end = 491;
            struct video_mode_t vMode = graphics_get_video_mode(get_video_mode());
            vga_px_hz = (uint32_t)(vMode.vga_pixel_clk ? vMode.vga_pixel_clk : vMode.pixel_clk);
            // Compute line layout from video_mode fields (×2 because VGA byte=pixel
            // and HDMI table values are in HDMI-bytes which encode 2 pixels each).
            int hs_b = vMode.vga_h_sync_bytes ? vMode.vga_h_sync_bytes : vMode.h_sync_bytes;
            int bp_b = vMode.vga_h_bp_bytes   ? vMode.vga_h_bp_bytes   : vMode.h_bp_bytes;
            int fp_b = vMode.vga_h_fp_bytes   ? vMode.vga_h_fp_bytes   : vMode.h_fp_bytes;
            int sw_b = vMode.vga_screen_width ? vMode.vga_screen_width : vMode.screen_width;
            HS_SIZE          = hs_b * 2;
            int bp           = bp_b * 2;
            visible_line_size = sw_b;
            int active_bytes = visible_line_size * 2;
            int fp           = fp_b * 2;
            line_size        = HS_SIZE + bp + active_bytes + fp;
            shift_picture    = HS_SIZE + bp;              // offset where active picture starts in line buffer
            HS_SHIFT         = line_size - shift_picture; // legacy unused
            break;
        }
        default:
            return;
    }

    //корректировка  палитры по маске бит синхры
    // The palette may have been built before palette16_mask was known.
    vga_repack_palette();

    // Save sync byte templates as globals so vga_reinit() can re-render line patterns
    TMPL_LINE8_g = TMPL_LINE8;
    TMPL_HS8_g   = TMPL_LINE8 ^ 0b01000000;
    TMPL_VS8_g   = TMPL_LINE8 ^ 0b10000000;
    TMPL_VHS8_g  = TMPL_LINE8 ^ 0b11000000;
    TMPL_VHS8 = TMPL_VHS8_g;
    TMPL_VS8  = TMPL_VS8_g;
    TMPL_HS8  = TMPL_HS8_g;

    //инициализация шаблонов строк и синхросигнала
    if (!lines_pattern_data) //выделение памяти, если не выделено
    {
#if !VGA_HSTX
        vga_pio_set_pixel_clk(vga_px_hz);
#else
        (void)vga_px_hz;   // the HSTX pixel clock is CSR.N_SHIFTS, set by vga_hstx_start()
#endif
        dma_channel_set_trans_count(dma_chan, VGA_DMA_WORDS(line_size), false);

        bool ok = vga_alloc_templates();
#if !VGA_HSTX
        // A WIDE template is four bytes per pixel against a narrow one's one, and
        // this runs when the heap is at its thinnest (core1, right after setup).
        // Losing PWM is a colour downgrade; losing the templates is NO PICTURE AT
        // ALL, because the early return below leaves lines_pattern[] NULL and the
        // DMA reads from zero.  So drop the width first and keep the display.  (On
        // HSTX there is no narrow shape to drop to — one pixel IS one FIFO word.)
        if (!ok && vga_wide) {
            printf("vga: no room for the wide line templates (4 x %u B) - PWM off\n",
                   (unsigned)((size_t)VGA_TMPL_WORDS() * sizeof(uint32_t)));
            vga_wide = false;
            vga_pwm_live = false;
            vga_repack_palette();     // they were packed 8 bytes to a pair
            vga_pio_set_pixel_clk(vga_px_hz);                    // 1x, not 4x
            dma_channel_set_trans_count(dma_chan, VGA_DMA_WORDS(line_size), false);
            ok = vga_alloc_templates();
        }
#endif
        if (!ok) {
            printf("graphics_set_mode: OOM allocating line templates (4 x %u B)\n",
                   (unsigned)((size_t)VGA_TMPL_WORDS() * sizeof(uint32_t)));
            return;
        }
    }

    // (Re-)render line templates for current line_size / HS_SIZE
    vga_fill_px(lines_pattern[0], TMPL_LINE8, line_size);   // empty line: idle level
    vga_fill_px(lines_pattern[0], TMPL_HS8,    HS_SIZE);    // hsync pulse at start

    vga_fill_px(lines_pattern[1], TMPL_VS8,  line_size);    // vsync line: vsync level
    vga_fill_px(lines_pattern[1], TMPL_VHS8, HS_SIZE);      // with hsync pulse at start

    // image line templates start as a copy of the empty line
    memcpy(lines_pattern[2], lines_pattern[0], (size_t)line_size * vga_px_bytes());
    memcpy(lines_pattern[3], lines_pattern[0], (size_t)line_size * vga_px_bytes());
}

void vga_reinit() {
    vga_alloc_buffers();   // VGA is the active output here — ensure the palette tables exist
    // Update VGA sync parameters, horizontal layout, and PIO pixel clock from current video_mode.
    // 50Hz modes use vga_pixel_clk override (lower clock + smaller v_total) so
    // monitors detect the signal as 640x480@50, not PAL 720x576@50.
    // HDMI ignores vga_* fields and keeps standard 25.175MHz / v_total=628..644.
    struct video_mode_t mode = graphics_get_video_mode(get_video_mode());
    line_VS_begin = mode.vga_vsync_start ? mode.vga_vsync_start : mode.vsync_start;
    line_VS_end = mode.vga_vsync_end ? mode.vga_vsync_end : mode.vsync_end;
    int pixel_clk = mode.vga_pixel_clk ? mode.vga_pixel_clk : mode.pixel_clk;
#if VGA_HSTX
    vga_hstx_apply_mode((uint32_t)pixel_clk);
#else
    vga_pio_set_pixel_clk((uint32_t)pixel_clk);
#endif

    // Recompute horizontal layout from video_mode and re-render line templates.
    // VGA byte=pixel; HDMI table values encode 2 pixels per byte, so multiply by 2.
    int hs_b = mode.vga_h_sync_bytes ? mode.vga_h_sync_bytes : mode.h_sync_bytes;
    int bp_b = mode.vga_h_bp_bytes   ? mode.vga_h_bp_bytes   : mode.h_bp_bytes;
    int fp_b = mode.vga_h_fp_bytes   ? mode.vga_h_fp_bytes   : mode.h_fp_bytes;
    int sw_b = mode.vga_screen_width ? mode.vga_screen_width : mode.screen_width;
    int new_HS_SIZE   = hs_b * 2;
    int new_bp        = bp_b * 2;
    int new_visible   = sw_b;
    int new_active    = new_visible * 2;
    int new_fp        = fp_b * 2;
    int new_line_size = new_HS_SIZE + new_bp + new_active + new_fp;

    if (new_line_size > VGA_MAX_LINE_SIZE) return;  // safety: don't overflow buffer

    bool layout_changed = (new_line_size != line_size) || (new_HS_SIZE != HS_SIZE)
                          || (new_visible != visible_line_size);

    HS_SIZE           = new_HS_SIZE;
    visible_line_size = new_visible;
    line_size         = new_line_size;
    shift_picture     = new_HS_SIZE + new_bp;
    HS_SHIFT          = line_size - shift_picture;

    if (layout_changed && lines_pattern_data) {
        // Re-render line templates with new HS_SIZE / line_size
        vga_fill_px(lines_pattern[0], TMPL_LINE8_g, line_size);
        vga_fill_px(lines_pattern[0], TMPL_HS8_g,    HS_SIZE);

        vga_fill_px(lines_pattern[1], TMPL_VS8_g,  line_size);
        vga_fill_px(lines_pattern[1], TMPL_VHS8_g, HS_SIZE);

        memcpy(lines_pattern[2], lines_pattern[0], (size_t)line_size * vga_px_bytes());
        memcpy(lines_pattern[3], lines_pattern[0], (size_t)line_size * vga_px_bytes());

        dma_channel_set_trans_count(dma_chan, VGA_DMA_WORDS(line_size), false);
    }
}

void graphics_set_buffer(uint8_t* buffer, const uint16_t width, const uint16_t height) {
    graphics_buffer_width = width;
    graphics_buffer_height = height;
}


void graphics_set_offset(const int x, const int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_flashmode(const bool flash_line, const bool flash_frame) {
    is_flash_frame = flash_frame;
    is_flash_line = flash_line;
}

// CRT mask profile over the 4 output pixels of one 2-source-pixel group, as
// 0..256 multipliers on each channel's code value. Raised cosine centred on tap 0:
// [1, (1+lo)/2, lo, (1+lo)/2] — vertical phosphor stripes with a soft profile,
// rather than the hard two-level mesh a period-2 grille produces.
//
//   output x:   0       1       2       3      (repeats)
//   table:    A.even  A.odd   B.even  B.odd
//
// Same numerators as the HDMI backend (hdmi_crt_mask), but used directly: HDMI has
// to snap each value to a TMDS-DC-balanced neighbour, so its columns differ from
// these by a few code units. No TMDS here.
// Levels 4..6: same machinery at PITCH 2 — taps 2/3 repeat 0/1, so phase B comes
// out identical to phase A (and needs no storage). See the note in hdmi.c on why a
// pitch-2 pattern is worth keeping alongside the pitch-4 profile.
#define VGA_CRT_LEVELS 7
static const uint16_t vga_crt_mask[VGA_CRT_LEVELS][4] = {
    { 256, 256, 256, 256 },   // 0 Off
    { 256, 228, 200, 228 },   // 1 Soft    pitch 4, lo = 0.78
    { 256, 207, 159, 207 },   // 2 Medium  pitch 4, lo = 0.62
    { 256, 186, 115, 186 },   // 3 Strong  pitch 4, lo = 0.45
    { 256, 171, 256, 171 },   // 4 Grille soft  pitch 2, exactly 2/3 — the one factor
                              //   that lands on the 4-level VGA DAC grid
    { 256, 148, 256, 148 },   // 5 Grille med   pitch 2
    { 256, 128, 256, 128 },   // 6 Grille hard  pitch 2, 1/2
};
static uint8_t vga_crt_level = 0;

// True when the level actually needs distinct phase-B tables (pitch 4). Pitch-2
// levels leave B aliasing A, so they cost no extra memory at all.
static inline bool vga_crt_needs_phaseb(uint8_t level) {
    if (level == 0 || level >= VGA_CRT_LEVELS) return false;
    return vga_crt_mask[level][2] != 256 || vga_crt_mask[level][3] != vga_crt_mask[level][1];
}

static inline uint32_t vga_crt_tap(uint32_t c, int tap) {
    if (!vga_crt_level) return c;
    const uint8_t lv = vga_crt_level < VGA_CRT_LEVELS ? vga_crt_level : 0;
    const uint32_t n = vga_crt_mask[lv][tap];
    if (n == 256) return c;
    return (((((c >> 16) & 0xff) * n) >> 8) << 16)
         | (((((c >>  8) & 0xff) * n) >> 8) <<  8)
         |   ((( c        & 0xff) * n) >> 8);
}

// The grille covers every colour slot, the OSD palette included — see the note in
// hdmi.c: exempting the OSD meant the fullscreen menu showed no preview at all.

// Bayer 2×2 ordered dithering: /21 → 13 levels/channel → 2197 perceived colors
// Threshold matrix: [0 2]  Fill order per sub-level: (0,0), (1,1), (0,1), (1,0)
//                   [3 1]
// Returns the four 6-bit VGA sub-pixels of the 2×2 block as p[x + 2*y].
static void vga_bayer4(uint32_t color888, uint8_t p[4]) {
    uint8_t r_level = ((color888 >> 16) & 0xff) / 21;
    uint8_t g_level = ((color888 >> 8) & 0xff) / 21;
    uint8_t b_level = (color888 & 0xff) / 21;

    uint8_t r_lo = r_level >> 2, r_sub = r_level & 3, r_hi = r_lo + (r_lo < 3 && r_sub);
    uint8_t g_lo = g_level >> 2, g_sub = g_level & 3, g_hi = g_lo + (g_lo < 3 && g_sub);
    uint8_t b_lo = b_level >> 2, b_sub = b_level & 3, b_hi = b_lo + (b_lo < 3 && b_sub);

    // 4 pixel positions in 2×2 block, ordered by Bayer threshold
    p[0] = ((r_sub >= 1 ? r_hi : r_lo) << 4) | ((g_sub >= 1 ? g_hi : g_lo) << 2) | (b_sub >= 1 ? b_hi : b_lo); // x0,y0
    p[1] = ((r_sub >= 3 ? r_hi : r_lo) << 4) | ((g_sub >= 3 ? g_hi : g_lo) << 2) | (b_sub >= 3 ? b_hi : b_lo); // x1,y0
    p[2] = (r_lo << 4) | (g_lo << 2) | b_lo;                                                                   // x0,y1
    p[3] = ((r_sub >= 2 ? r_hi : r_lo) << 4) | ((g_sub >= 2 ? g_hi : g_lo) << 2) | (b_sub >= 2 ? b_hi : b_lo); // x1,y1
}

// Per-level scanline brightness, as a 0..256 multiplier applied to each RGB
// channel. Index by level (1..4); level 2 == 128/256 == the legacy ~50% look.
// dark -> light. Index 0 is unused (scanlines off).
static const uint16_t scanline_dim_num[5] = { 128, 64, 128, 184, 224 };

// Dim RGB888 color for the current scanline brightness level.
static uint32_t dim_rgb888(uint32_t color888) {
    uint16_t num = scanline_dim_num[(vga_scanline_level <= 4) ? vga_scanline_level : 2];
    uint8_t r = (((color888 >> 16) & 0xff) * num) >> 8;
    uint8_t g = (((color888 >> 8) & 0xff) * num) >> 8;
    uint8_t b = ((color888 & 0xff) * num) >> 8;
    return (r << 16) | (g << 8) | b;
}

// One output-pixel pair, in BOTH representations — the caller stores whichever the
// boot chose.  They take different inputs and that is the whole point: the flat one
// gets `lo`/`hi`, the 6-bit values the Bayer 2x2 block quantised the colour to,
// while the PWM one gets the COLOURS and spreads each over four sub-samples — no
// dither, no 4-level grid, and no need for the solid / grid-snap fork the 16 flat
// ZX colours have always needed.  Runs per palette entry, never per pixel.
static inline vga_pairv_t vga_make_pair(uint32_t c_lo, uint32_t c_hi,
                                        uint8_t lo, uint8_t hi) {
    const uint8_t sync = (uint8_t)(palette16_mask & 0xff);
    vga_pairv_t v;
    v.flat = (uint16_t)((((uint16_t)hi << 8) | lo) & 0x3f3f) | palette16_mask;
    if (vga_pwm_live) {
        // Both pixels of the pair carry their colour's own phase pattern, with NO
        // offset between them: offsetting the right one by a phase was tried and
        // hw-REFUTED (2026-09-23) — it fixed the level but put a 1-pixel vertical
        // stripe on every colour whose adjacent phases differ, i.e. it traded the
        // error for the very dither PWM exists to remove.  See vga_pwm.h.
        v.w[0] = vga_pwm_word(vga_pwm_tab, c_lo, sync);
        v.w[1] = vga_pwm_word(vga_pwm_tab, c_hi, sync);
    } else {
        // Wide but not PWM: the same byte in all four phases, i.e. byte for byte
        // what the narrow path would have driven.  Only reachable on HSTX, where
        // the transport has no narrow shape — it is the colour-vs-transport bisect.
        v.w[0] = vga_pwm_flat_word((uint8_t)((lo & 0x3f) | sync));
        v.w[1] = vga_pwm_flat_word((uint8_t)((hi & 0x3f) | sync));
    }
    return v;
}

static inline uint8_t vga6_of(uint32_t c) {
    return ((((c >> 16) & 0xff) / 85) << 4) | ((((c >> 8) & 0xff) / 85) << 2) | ((c & 0xff) / 85);
}

// 6-bit sub-pixel matrix for one colour: sub[tap][bayer position], where the Bayer
// position is (x & 1) + 2 * (y & 1) — output x 0 and 2 share the even-x thresholds.
// solid=true pins tap 0 (the unattenuated colour) to the DAC grid with no dither, so
// the brightest column of the 16 ZX colours never shimmers.
//
// With the mask off every tap is the same colour, so all four rows are identical and
// the packed results below are bit-identical to the original single-colour dither.
static void vga_crt_subpixels(uint32_t c, bool solid, uint8_t sub[4][4], uint32_t tap[4]) {
    const uint32_t t0 = vga_crt_tap(c, 0);
    for (int k = 0; k < 4; k++) {
        const uint32_t tk = vga_crt_tap(c, k);
        tap[k] = tk;
        // A solid entry stays solid for every tap that is NOT attenuated. With the
        // mask off that is all four, so the result is bit-identical to the old
        // undithered path; with it on, only the unattenuated tap 0 keeps that.
        if (solid && tk == t0) {
            const uint8_t v6 = vga6_of(tk);
            sub[k][0] = sub[k][1] = sub[k][2] = sub[k][3] = v6;
        } else {
            vga_bayer4(tk, sub[k]);
        }
    }
}

// Recompute the dimmed scanline pixel for a single index from its cached color.
// Scanline dim and the CRT mask compose: the mask varies across x, the scanline dim
// across y, so together they give a full dot mask.
static void vga_build_scanline_entry(uint8_t i) {
    if (!vga_color888) return;                 // VGA tables not allocated (HDMI active)
    uint8_t sub[4][4]; uint32_t tap[4];
    vga_crt_subpixels(dim_rgb888(vga_color888[i]), vga_color_solid[i], sub, tap);
    // Odd-line table only: Bayer positions 2 (even x) and 3 (odd x).
    vga_pal_put(palette_vga16_scanline, i, vga_make_pair(tap[0], tap[1], sub[0][2], sub[1][3]));
    if (vga_crt_phaseb)
        vga_pal_put(palette_vga16_scanline_b, i, vga_make_pair(tap[2], tap[3], sub[2][2], sub[3][3]));
}

// Fill the phase-A (even source pixel, mask taps 0/1) and phase-B (odd source pixel,
// taps 2/3) pixel pairs for one palette index, for both line parities.
static void vga_fill_pairs(uint8_t i, uint32_t color888, bool solid) {
    uint8_t sub[4][4]; uint32_t tap[4];
    vga_crt_subpixels(color888, solid, sub, tap);
    vga_pal_put(pal_row(palette_vga16, 0), i,                            // even y, taps 0/1
                vga_make_pair(tap[0], tap[1], sub[0][0], sub[1][1]));
    vga_pal_put(pal_row(palette_vga16, 1), i,                            // odd  y
                vga_make_pair(tap[0], tap[1], sub[0][2], sub[1][3]));
    if (vga_crt_phaseb) {
        vga_pal_put(pal_row(palette_vga16_b, 0), i,                      // taps 2/3
                    vga_make_pair(tap[2], tap[3], sub[2][0], sub[3][1]));
        vga_pal_put(pal_row(palette_vga16_b, 1), i,
                    vga_make_pair(tap[2], tap[3], sub[2][2], sub[3][3]));
    }
}

// Update a single VGA palette LUT entry with Bayer dithering
void vga_set_palette_entry(uint8_t i, uint32_t color888) {
    if (!vga_buffers_ready()) return;          // HDMI active → VGA tables unused
    vga_color888[i] = color888;
    vga_color_solid[i] = false;
    vga_fill_pairs(i, color888, false);
    // Scanline: dithered at the current brightness level (odd pair for single-line rendering)
    vga_build_scanline_entry(i);
}

// Update a VGA palette entry WITHOUT dithering (both palettes get identical solid color)
// Use for the 16 standard Spectrum colors to avoid visible dithering artifacts.
// With the CRT mask on, only the unattenuated tap stays solid — the attenuated
// columns must dither or the 4-level DAC would quantise the profile away.
void vga_set_palette_entry_solid(uint8_t i, uint32_t color888) {
    if (!vga_buffers_ready()) return;          // HDMI active → VGA tables unused
    vga_color888[i] = color888;
    vga_color_solid[i] = true;
    vga_fill_pairs(i, color888, true);
    // Scanline: dimmed solid at the current brightness level
    vga_build_scanline_entry(i);
}

// Build VGA DS80 packed-pair palette from the 16-color Profi palette and pair_lut.
// pair_lut is profi_pair_lookup[0][0] (flat 256-byte): pair_lut[p0*16+p1] = slot.
// Each slot byte maps to a uint16_t: low byte = VGA pixel for p0 (left),
// high byte = VGA pixel for p1 (right). PIO right-shifts LSB first → correct order.
// Two tables (even/odd scan lines) implement Bayer 2×2 checkerboard dithering:
//   p0 is always at even screen x, p1 at odd screen x.
//   The even-line pair is low(even_x,even_y)/high(odd_x,even_y), the odd-line one
//   low(even_x,odd_y)/high(odd_x,odd_y) — Bayer positions 0/1 and 2/3 of vga_bayer4(),
//   packed by vga_pack_pair().
void vga_set_profi_ds80_mode(bool active,
                              const uint32_t *palette16_rgb888,
                              const uint8_t  *pair_lut) {
    if (active && palette16_rgb888 && pair_lut && vga_buffers_ready()) {
        // Dithered VGA pixel values for each of 16 Profi colors.
        uint8_t vga_even_left[16];   // even scan-line, left  pixel (even screen x)
        uint8_t vga_even_right[16];  // even scan-line, right pixel (odd screen x)
        uint8_t vga_odd_left[16];    // odd scan-line,  left  pixel
        uint8_t vga_odd_right[16];   // odd scan-line,  right pixel
        // Phase A (even source pixel) takes mask taps 0/1, phase B taps 2/3; within
        // each pair p0 = ink is the left pixel, p1 = paper the right one.
        // [phase][colour]
        uint8_t vga_even_left_b[16], vga_even_right_b[16];
        uint8_t vga_odd_left_b[16],  vga_odd_right_b[16];
        // The tap COLOUR per phase, which is what the PWM back-end packs; the 6-bit
        // values above are the PIO's.  Taps 0/1 are phase A, 2/3 phase B, and the
        // even/odd line pair differs only in the Bayer position, so one colour per
        // (phase, side) covers both parities.
        uint32_t cA_left[16], cA_right[16], cB_left[16], cB_right[16];
        for (int i = 0; i < 16; i++) {
            uint8_t sub[4][4]; uint32_t tap[4];
            vga_crt_subpixels(palette16_rgb888[i], false, sub, tap);
            vga_even_left[i]    = sub[0][0];  vga_even_right[i]    = sub[1][1];
            vga_odd_left[i]     = sub[0][2];  vga_odd_right[i]     = sub[1][3];
            vga_even_left_b[i]  = sub[2][0];  vga_even_right_b[i]  = sub[3][1];
            vga_odd_left_b[i]   = sub[2][2];  vga_odd_right_b[i]   = sub[3][3];
            cA_left[i] = tap[0]; cA_right[i] = tap[1];
            cB_left[i] = tap[2]; cB_right[i] = tap[3];
        }
        // Initialise all slots to (black, black) so unused/border slots are safe.
        const vga_pairv_t black_pair = vga_make_pair(0, 0, 0, 0);
        for (int s = 0; s < 256; s++) {
            vga_pal_put(pal_row(palette_vga_ds80, 0), s, black_pair);
            vga_pal_put(pal_row(palette_vga_ds80, 1), s, black_pair);
            if (vga_crt_phaseb) {
                vga_pal_put(pal_row(palette_vga_ds80_b, 0), s, black_pair);
                vga_pal_put(pal_row(palette_vga_ds80_b, 1), s, black_pair);
            }
        }
        // Fill every (p0, p1) combination that has a valid slot.
        // written[] guard: for merged slots (paper=8 → paper=0 for ink≤5), the first
        // pair wins so paper=0's colour is used — matches hdmi_set_profi_ds80_mode().
        bool written[256] = { false };
        for (int p0 = 0; p0 < 16; p0++) {
            for (int p1 = 0; p1 < 16; p1++) {
                uint8_t slot = pair_lut[p0 * 16 + p1];
                if (written[slot]) continue;
                written[slot] = true;
                vga_pal_put(pal_row(palette_vga_ds80, 0), slot,
                            vga_make_pair(cA_left[p0], cA_right[p1],
                                          vga_even_left[p0], vga_even_right[p1]));
                vga_pal_put(pal_row(palette_vga_ds80, 1), slot,
                            vga_make_pair(cA_left[p0], cA_right[p1],
                                          vga_odd_left[p0],  vga_odd_right[p1]));
                if (vga_crt_phaseb) {
                    vga_pal_put(pal_row(palette_vga_ds80_b, 0), slot,
                                vga_make_pair(cB_left[p0], cB_right[p1],
                                              vga_even_left_b[p0], vga_even_right_b[p1]));
                    vga_pal_put(pal_row(palette_vga_ds80_b, 1), slot,
                                vga_make_pair(cB_left[p0], cB_right[p1],
                                              vga_odd_left_b[p0],  vga_odd_right_b[p1]));
                }
            }
        }
        profi_ds80_active = true;
    } else {
        profi_ds80_active = false;
    }
}

void graphics_set_bgcolor_hdmi(uint32_t color888);
void graphics_set_bgcolor(const uint32_t color888) {
    if (!SELECT_VGA) {
        graphics_set_bgcolor_hdmi(color888);
        return;
    }
    vga_bg888 = color888;
    // Border/background is beam-drawn on a real CRT, so it takes the mask too.
    // One full 4-output-pixel mask group = two consecutive PAIRS: phase A then B.
    uint8_t sub[4][4]; uint32_t tap[4];
    vga_crt_subpixels(color888, false, sub, tap);
    bg_pair[0][0] = vga_make_pair(tap[0], tap[1], sub[0][0], sub[1][1]);   // even y, phase A
    bg_pair[0][1] = vga_make_pair(tap[2], tap[3], sub[2][0], sub[3][1]);   // even y, phase B
    bg_pair[1][0] = vga_make_pair(tap[0], tap[1], sub[0][2], sub[1][3]);   // odd  y, phase A
    bg_pair[1][1] = vga_make_pair(tap[2], tap[3], sub[2][2], sub[3][3]);   // odd  y, phase B
}

#ifndef VGA_HDMI
// Standalone VGA build: provide graphics_set_palette
void graphics_set_palette(const uint8_t i, const uint32_t color888) {
    vga_set_palette_entry(i, color888);
}
#endif

void vga_set_scanlines(uint8_t level) {
    if (!vga_buffers_ready()) return;          // HDMI active → VGA tables unused
    if (level > 4) level = 4;
    vga_scanlines = (level != 0);
    // Off keeps the previous brightness so toggling back is cheap; only a real
    // level change forces a dimmed-palette rebuild.
    if (level != 0 && level != vga_scanline_level) {
        vga_scanline_level = level;
        for (int i = 0; i < 256; ++i) vga_build_scanline_entry(i);
    }
}

// CRT aperture grille level (0=Off, 1..3). Stores the level and rebuilds the
// palette tables from the cached RGB888 — that cache (vga_color888/vga_color_solid)
// exists for exactly this, the same way vga_set_scanlines() uses it.
//
// The solid flag has to be honoured explicitly: vga_set_palette_entry() clears it,
// so a blind re-walk would silently start dithering the 16 ZX colours.
void vga_set_crt(uint8_t level) {
    if (level >= VGA_CRT_LEVELS) level = 0;
    if (level == vga_crt_level) return;
    vga_crt_level = level;
    if (!vga_buffers_ready()) return;          // HDMI active → VGA tables unused
    // Promote the phase-B tables from aliases to real storage the first time the
    // mask is switched on (~2.5 KB). If that fails the aliases stay, which yields a
    // plain period-2 grille instead of the 4-tap profile — degraded, not broken.
    if (vga_crt_needs_phaseb(level) && !vga_crt_phaseb) {
        extern size_t getLargestAllocatable(void);
        const size_t pb = vga_pair_bytes();
        const size_t need = 2 * 256 * pb   // palette_vga16_b
                          +     256 * pb   // palette_vga16_scanline_b
                          + 2 * 256 * pb;  // palette_vga_ds80_b
        if (getLargestAllocatable() >= need) {
            palette_vga16_b          = calloc(2 * 256, pb);
            palette_vga16_scanline_b = calloc(    256, pb);
            palette_vga_ds80_b       = calloc(2 * 256, pb);
            vga_crt_phaseb = palette_vga16_b && palette_vga16_scanline_b && palette_vga_ds80_b;
        }
        if (!vga_crt_phaseb) {
            printf("vga_set_crt: no room for CRT phase-B tables (%u B) — period-2 mask only\n",
                   (unsigned)need);
            palette_vga16_b          = palette_vga16;
            palette_vga16_scanline_b = palette_vga16_scanline;
            palette_vga_ds80_b       = palette_vga_ds80;
        }
    }
    for (int i = 0; i < 256; ++i) {
        if (vga_color_solid[i]) vga_set_palette_entry_solid((uint8_t)i, vga_color888[i]);
        else                    vga_set_palette_entry((uint8_t)i, vga_color888[i]);
    }
}

#ifndef VGA_HDMI
void graphics_set_scanlines(uint8_t level) {
    vga_set_scanlines(level);
}
void graphics_set_crt(uint8_t level) {
    vga_set_crt(level);
}
#endif

uint8_t linkVGA01;
void graphics_init_hdmi();
void graphics_init() {
    if (video_driver == 0) {
        #if defined(ZERO2) || defined(PICO_DV)
            SELECT_VGA = linkVGA01 == 0x1F;
        #else
            SELECT_VGA = (linkVGA01 == 0) || (linkVGA01 == 0x1F);
        #endif
    } else {
        SELECT_VGA = video_driver == 1;
    }
    if (!SELECT_VGA) {
        graphics_init_hdmi();
        return;
    }
    vga_flags_init();
    printf("vga: %s pixels, %s (%d bytes/pixel, %d levels/channel)\n",
           vga_wide ? "wide" : "narrow",
           vga_pwm_live ? "per-pixel PWM" : "Bayer 2x2 dither / solid",
           vga_px_bytes(), vga_pwm_live ? vga_pwm_maxsum(vga_pwm_tab->k) + 1 : 13);
    //инициализация палитры по умолчанию
    //текстовая палитра
    for (int i = 0; i < 16; i++) {
        const uint8_t b = i & 1 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t r = i & 4 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t g = i & 2 ? (i >> 3 ? 3 : 2) : 0;

        const uint8_t c = r << 4 | g << 2 | b;

        txt_palette[i] = c & 0x3f | 0xc0;
    }
#if VGA_HSTX
    // The serializer, not the PIO: one 32-bit word per output pixel, four PWM
    // sub-samples inside it.  _SM_VGA stays >= 0 because the rest of the driver
    // uses it as "the output is up".
    {
        struct video_mode_t vm = graphics_get_video_mode(get_video_mode());
        const uint32_t px = vm.vga_pixel_clk ? (uint32_t)vm.vga_pixel_clk
                                             : (uint32_t)vm.pixel_clk;
        if (!vga_hstx_start(px)) {
            printf("vga: HSTX refused %u Hz - no picture. Check video_mode_table.h "
                   "against tools/vga_timing_test.c\n", (unsigned)px);
        }
        vga_hstx_dump_config();   // no-op without -DHDMI_HSTX_TRACE=ON
        _SM_VGA = 0;
    }
#else
    //инициализация PIO
    //загрузка программы в один из PIO
    const uint offset = pio_add_program(PIO_VGA, &pio_program_VGA);
    _SM_VGA = pio_claim_unused_sm(PIO_VGA, true);
    const uint sm = _SM_VGA;

    for (int i = 0; i < 8; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(PIO_VGA, VGA_BASE_PIN + i);
    }; //резервируем под выход PIO

    pio_sm_set_consecutive_pindirs(PIO_VGA, sm, VGA_BASE_PIN, 8, true); //конфигурация пинов на выход

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + (pio_program_VGA.length - 1));

    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX); //увеличение буфера TX за счёт RX до 8-ми
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, VGA_BASE_PIN, 8);
    pio_sm_init(PIO_VGA, sm, offset, &c);

    pio_sm_set_enabled(PIO_VGA, sm, true);
#endif

    //инициализация DMA
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    //основной ДМА канал для данных
    dma_channel_config c0 = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);

    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);

#if VGA_HSTX
    const uint dreq = vga_hstx_dreq();
    volatile void *const sink = vga_hstx_fifo();
#else
    uint dreq = DREQ_PIO1_TX0 + sm;
    if (PIO_VGA == pio0) dreq = DREQ_PIO0_TX0 + sm;
    volatile void *const sink = (volatile void *)&PIO_VGA->txf[sm];
#endif

    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, dma_chan_ctrl); // chain to other channel

    dma_channel_configure(
        dma_chan,
        &c0,
        (void *)sink, // Write address
        lines_pattern[0], // read address
        600 / 4, //
        false // Don't start yet
    );
    //канал DMA для контроля основного канала
    dma_channel_config c1 = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);

    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_chain_to(&c1, dma_chan); // chain to other channel

    dma_channel_configure(
        dma_chan_ctrl,
        &c1,
        &dma_hw->ch[dma_chan].read_addr, // Write address
        &lines_pattern[0], // read address
        1, //
        false // Don't start yet
    );

    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    irq_set_exclusive_handler(VGA_DMA_IRQ, dma_handler_VGA);
    dma_channel_set_irq0_enabled(dma_chan_ctrl, true);
    irq_set_enabled(VGA_DMA_IRQ, true);
    dma_start_channel_mask(1u << dma_chan);
}
