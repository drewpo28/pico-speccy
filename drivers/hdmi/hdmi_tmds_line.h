#pragma once
// The HDMI line as an HSTX COMMAND LIST — the model behind HDMI_HSTX == 2.
//
// With the command expander on, one scanline is a small program the serializer
// runs: `RAW_REPEAT | n` + word for the control periods (sync, porches, preambles,
// guard bands), `RAW | 36` + 36 words for a Data Island, `TMDS | active` followed by
// one XRGB8888 word per output pixel that the hardware TMDS-encodes. Nothing in it
// is a palette index any more, which is what gives the 8bpp framebuffer its sync
// and island indices back (see docs/hstx-m2p2-plan.md, "Palette slots").
//
// Depends on <stdint.h> only. The firmware builds lines with these in the core1 line
// ISR; tools/hdmi_tmds_line_test.c plays every line type back through hdmi_tl_play()
// and checks that each one is exactly h_total pixels of the right kind in the right
// order. Re-run it after ANY change here:
//
//   gcc -O2 -Wall -Wextra -Idrivers/hdmi -o /tmp/hdmi_tmds_line_test tools/hdmi_tmds_line_test.c
//   /tmp/hdmi_tmds_line_test
#include <stdint.h>

// Command words, bits 15..12; bits 11..0 = count in output pixels (RAW/TMDS: the
// words that follow; *_REPEAT: how often the ONE word that follows is emitted).
// Not in the SDK — these are pico-examples' constants (dvi_out_hstx_encoder.c).
#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xFu << 12)
#define HSTX_CMD_COUNT_MAX   4095u

// Data Island geometry, in output pixels, at the head of the line (inside the hsync
// pulse — the SpeccyP placement the PIO path has always used): 8 preamble, 2 leading
// guard, 32 packet characters, 2 trailing guard = 44.
#define HDMI_TL_DI_PRE_PX    8
#define HDMI_TL_DI_GUARD_PX  2
#define HDMI_TL_DI_CHARS     32
#define HDMI_TL_DI_RAW       (2 * HDMI_TL_DI_GUARD_PX + HDMI_TL_DI_CHARS)   // 36 raw words
#define HDMI_TL_DI_PX        (HDMI_TL_DI_PRE_PX + HDMI_TL_DI_RAW)           // 44
// ...and in COMMAND WORDS, which is a different number: the preamble is one
// RAW_REPEAT command plus its word, then one RAW command plus 36 words = 39.
// Anything that reasons about where the DMA's read pointer is inside the island
// wants this one, not the pixel count (hdmi_isl_second_play).
#define HDMI_TL_DI_WORDS     (2 + 1 + HDMI_TL_DI_RAW)                       // 39
// Video preamble + guard band, at the end of the back porch (HDMI mode only).
#define HDMI_TL_VP_PX        8
#define HDMI_TL_VG_PX        2
#define HDMI_TL_GUARDS_PX    (HDMI_TL_VP_PX + HDMI_TL_VG_PX)

// Worst case is a BLANKING line, which is paced word per pixel (see hdmi_tl_blank):
// island 39 words + 1 command + (h_total - 44) words = 796 at h_total 800; an active
// line is ~800 with its front porch paced word per pixel too. Room to spare.
#define HDMI_TL_MAX_WORDS    896

typedef struct {
    uint16_t hs_px, bp_px, fp_px, active_px, total_px;
} hdmi_tl_geom_t;

// Every non-pixel word a line needs, as 30-bit raw TMDS triples (hdmi_pack3_hstx
// order: ch0 | ch1 << 10 | ch2 << 20). Filled by hdmi.c from its own symbol tables,
// so the constants stay where they always were.
typedef struct {
    uint32_t sync[2][2];        // [v][h]: control word with ch0 = CTRL(H=h, V=v)
    uint32_t di_pre[2];         // [v] Data Island preamble (CTL 1010), ch0 = {H=0, V=v}
    uint32_t di_guard[2][2];    // [v][pixel 0/1] leading island guard band
    uint32_t di_trail[2][2];    // [v][pixel 0/1] trailing guard (H may differ: it can
                                //   sit in the back porch on the 32-px-sync modes)
    uint32_t video_pre;         // video preamble (CTL 1000), ch0 = {H=1, V=1}
    uint32_t video_guard;       // video guard band
} hdmi_tl_words_t;

static inline hdmi_tl_geom_t hdmi_tl_geom(int h_sync_bytes, int h_bp_bytes, int h_fp_bytes,
                                          int screen_width_bytes) {
    hdmi_tl_geom_t g;
    g.hs_px     = (uint16_t)(2 * h_sync_bytes);
    g.bp_px     = (uint16_t)(2 * h_bp_bytes);
    g.fp_px     = (uint16_t)(2 * h_fp_bytes);
    g.active_px = (uint16_t)(2 * screen_width_bytes);
    g.total_px  = (uint16_t)(g.hs_px + g.bp_px + g.active_px + g.fp_px);
    return g;
}

// `n` pixels of one raw word. A zero-length run emits nothing — a count of 0 is
// not "nothing" to the expander.
static inline __attribute__((always_inline))
uint32_t *hdmi_tl_rep(uint32_t *w, unsigned n, uint32_t word) {
    if (n) { *w++ = HSTX_CMD_RAW_REPEAT | (n & HSTX_CMD_COUNT_MAX); *w++ = word; }
    return w;
}

// `n` pixels of one raw word as a RAW block, ONE WORD PER PIXEL. Used for the tail
// of a line: the DMA can only run as far ahead of the raster as the words it has to
// read, and a RAW_REPEAT tail (2 words for the whole front porch) let it finish the
// line ~fp_px early — the line IRQ then fired that much before the next line, and
// the second-play island refill (hdmi_isl_second_play), which waits for the DMA to
// get past the island at the head of the playing buffer, ran out of its 3 us guard
// inside the island: ~2200-2600 refused refills a second at 25.2 MHz (hw 2026-09-24,
// MURM2 Speed Test `skip@ buf0 word 36..38`). The blank lines were paced this way
// already, for the same reason (see hdmi_tl_blank).
static inline __attribute__((always_inline))
uint32_t *hdmi_tl_raw_run(uint32_t *w, unsigned n, uint32_t word) {
    if (n) {
        *w++ = HSTX_CMD_RAW | (n & HSTX_CMD_COUNT_MAX);
        for (unsigned i = 0; i < n; i++) *w++ = word;
    }
    return w;
}

// The island block: preamble run, then ONE raw block of 36 words = leading guard,
// 32 packet characters, trailing guard. Writes the guards, leaves the 32 characters
// to the caller (*chars), and returns the write pointer past the block.
static inline __attribute__((always_inline))
uint32_t *hdmi_tl_island(uint32_t *w, const hdmi_tl_words_t *wd, int v, uint32_t **chars) {
    w = hdmi_tl_rep(w, HDMI_TL_DI_PRE_PX, wd->di_pre[v]);
    *w++ = HSTX_CMD_RAW | HDMI_TL_DI_RAW;
    w[0] = wd->di_guard[v][0];
    w[1] = wd->di_guard[v][1];
    *chars = w + HDMI_TL_DI_GUARD_PX;
    w[HDMI_TL_DI_GUARD_PX + HDMI_TL_DI_CHARS + 0] = wd->di_trail[v][0];
    w[HDMI_TL_DI_GUARD_PX + HDMI_TL_DI_CHARS + 1] = wd->di_trail[v][1];
    return w + HDMI_TL_DI_RAW;
}

// Control periods from pixel `pos` up to the start of the active area (or of the
// video guards, in HDMI mode): the rest of the hsync pulse at H=0, then the back
// porch at H=1. Shared by every line type.
static inline __attribute__((always_inline))
uint32_t *hdmi_tl_porch_to(uint32_t *w, const hdmi_tl_geom_t *g, const hdmi_tl_words_t *wd,
                           int v, unsigned pos, unsigned end) {
    if (g->hs_px > pos) { w = hdmi_tl_rep(w, g->hs_px - pos, wd->sync[v][0]); pos = g->hs_px; }
    if (end > pos)      { w = hdmi_tl_rep(w, end - pos, wd->sync[v][1]); }
    return w;
}

// An active line: [island] [hsync rest] [back porch] [video preamble + guard]
// TMDS | active_px, <active_px pixel words — NOT written, *px points at them>,
// [front porch]. Returns the word count. `v` is the line's VSYNC level (1 = idle).
static inline __attribute__((always_inline))
int hdmi_tl_active(uint32_t *buf, const hdmi_tl_geom_t *g, const hdmi_tl_words_t *wd,
                   int v, int audio, uint32_t **px, uint32_t **chars) {
    uint32_t *w = buf;
    unsigned pos = 0;
    if (audio) { w = hdmi_tl_island(w, wd, v, chars); pos = HDMI_TL_DI_PX; }
    else *chars = 0;
    const unsigned bp_end = (unsigned)g->hs_px + g->bp_px;
    w = hdmi_tl_porch_to(w, g, wd, v, pos, bp_end - (audio ? HDMI_TL_GUARDS_PX : 0));
    if (audio) {
        w = hdmi_tl_rep(w, HDMI_TL_VP_PX, wd->video_pre);
        w = hdmi_tl_rep(w, HDMI_TL_VG_PX, wd->video_guard);
    }
    *w++ = HSTX_CMD_TMDS | g->active_px;
    *px = w;
    w += g->active_px;
    w = hdmi_tl_raw_run(w, g->fp_px, wd->sync[v][1]);   // paced: see hdmi_tl_raw_run
    return (int)(w - buf);
}

// A blanking line: [island] then control words to the end of the line. In HDMI
// mode a blanking line carries an island exactly like an active one.
//
// The control words are ONE RAW BLOCK, a word per pixel, NOT two RAW_REPEAT runs.
// That costs ~3 KB of DMA per blanking line and buys the line ISR its cadence: a
// buffer plays twice (two consecutive lines, same words) and the second play's
// audio island has to be replaced by a Null packet from the ISR — which is only
// possible if the DMA has not yet re-read those words. With RAW_REPEAT the whole
// 43-word line is in the FIFO microseconds after the trigger, the ctrl channel has
// already loaded the SAME buffer for its second play, and its island is being read
// before the ISR even runs (that is also the ~30 us "early cadence" the blanking
// IRQs used to have). Paced word per pixel, the DMA trails the raster by 8 words on
// every line type alike, the ISR fires ~1.5 us before each line and the second
// play's island is read ~30 us later — the same window an active line gives.
static inline __attribute__((always_inline))
int hdmi_tl_blank(uint32_t *buf, const hdmi_tl_geom_t *g, const hdmi_tl_words_t *wd,
                  int v, int audio, uint32_t **chars) {
    uint32_t *w = buf;
    unsigned pos = 0;
    if (audio) { w = hdmi_tl_island(w, wd, v, chars); pos = HDMI_TL_DI_PX; }
    else *chars = 0;
    const unsigned n = g->total_px - pos;
    *w++ = HSTX_CMD_RAW | (n & HSTX_CMD_COUNT_MAX);
    unsigned hs = (g->hs_px > pos) ? g->hs_px - pos : 0;    // rest of the hsync pulse
    for (unsigned i = 0; i < hs; i++) *w++ = wd->sync[v][0];
    for (unsigned i = hs; i < n; i++) *w++ = wd->sync[v][1];
    return (int)(w - buf);
}

// The static scanline line (Video > Scanlines): an active line whose every pixel is
// the same dark grey — TMDS_REPEAT, one word — and no island (its head is control,
// as the PIO path's static buffer always was).
static inline __attribute__((always_inline))
int hdmi_tl_scanline(uint32_t *buf, const hdmi_tl_geom_t *g, const hdmi_tl_words_t *wd,
                     int audio, uint32_t gray) {
    uint32_t *w = buf;
    const unsigned bp_end = (unsigned)g->hs_px + g->bp_px;
    w = hdmi_tl_porch_to(w, g, wd, 1, 0, bp_end - (audio ? HDMI_TL_GUARDS_PX : 0));
    if (audio) {
        w = hdmi_tl_rep(w, HDMI_TL_VP_PX, wd->video_pre);
        w = hdmi_tl_rep(w, HDMI_TL_VG_PX, wd->video_guard);
    }
    *w++ = HSTX_CMD_TMDS_REPEAT | g->active_px;
    *w++ = gray;
    w = hdmi_tl_raw_run(w, g->fp_px, wd->sync[1][1]);   // paced like an active line's
    return (int)(w - buf);
}

// ---------------------------------------------------------------------------
// Playback model for the host test: walks a command list the way the expander
// does and hands every output pixel to `emit(ctx, px, kind, word)`, kind 0 = raw,
// 1 = TMDS. Returns the pixel count, or -1 if the list is malformed (a command whose
// operands run past the end, an unknown command, or trailing words).
// ---------------------------------------------------------------------------
typedef void (*hdmi_tl_emit_t)(void *ctx, unsigned px, int kind, uint32_t word);

static inline int hdmi_tl_play(const uint32_t *buf, int n, hdmi_tl_emit_t emit, void *ctx) {
    const uint32_t *w = buf, *end = buf + n;
    unsigned px = 0;
    while (w < end) {
        const uint32_t cmd = *w++;
        const unsigned cnt = cmd & HSTX_CMD_COUNT_MAX;
        switch (cmd & 0xF000u) {
        case HSTX_CMD_RAW:
            if (w + cnt > end) return -1;
            for (unsigned i = 0; i < cnt; i++) emit(ctx, px++, 0, *w++);
            break;
        case HSTX_CMD_TMDS:
            if (w + cnt > end) return -1;
            for (unsigned i = 0; i < cnt; i++) emit(ctx, px++, 1, *w++);
            break;
        case HSTX_CMD_RAW_REPEAT:
            if (w >= end) return -1;
            for (unsigned i = 0; i < cnt; i++) emit(ctx, px++, 0, *w);
            w++;
            break;
        case HSTX_CMD_TMDS_REPEAT:
            if (w >= end) return -1;
            for (unsigned i = 0; i < cnt; i++) emit(ctx, px++, 1, *w);
            w++;
            break;
        case HSTX_CMD_NOP:
            break;
        default:
            return -1;
        }
    }
    return (int)px;
}
