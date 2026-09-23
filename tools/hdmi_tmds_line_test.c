// Host test for drivers/hdmi/hdmi_tmds_line.h — the HSTX command-list line model
// behind HDMI_HSTX == 2.
//
//   gcc -O2 -Wall -Wextra -Idrivers/hdmi -o /tmp/hdmi_tmds_line_test tools/hdmi_tmds_line_test.c
//   /tmp/hdmi_tmds_line_test
//
// For both line geometries the mode table has (640-wide: 48/24/8 bytes + 320,
// 720-wide: 16/16/8 + 360), audio off and on, V idle and V asserted, it builds every
// line type, plays it back through hdmi_tl_play() and checks pixel by pixel that the
// line is exactly h_total pixels long and that every pixel is the word the reference
// says belongs there: island preamble / guards / the 32 characters in order, the
// hsync remainder at H=0, the back porch at H=1, the video preamble and guard right
// before the TMDS block, the active pixels in order, the front porch. Also that the
// word count stays under HDMI_TL_MAX_WORDS and that a zero-length run emits nothing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdmi_tmds_line.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Distinct marker words so a misplaced one is caught by value.
#define W_SYNC(v, h)   (0x01000000u | ((v) << 4) | (h))
#define W_DIPRE(v)     (0x02000000u | (v))
#define W_DIG(v, p)    (0x03000000u | ((v) << 4) | (p))
#define W_DIT(v, p)    (0x04000000u | ((v) << 4) | (p))
#define W_VPRE         0x05000000u
#define W_VGUARD       0x06000000u
#define W_CHAR(i)      (0x07000000u | (i))
#define W_PIX(i)       (0x08000000u | (i))
#define W_GRAY         0x09000000u

static hdmi_tl_words_t words(void) {
    hdmi_tl_words_t w;
    for (int v = 0; v < 2; v++) {
        for (int h = 0; h < 2; h++) w.sync[v][h] = W_SYNC(v, h);
        w.di_pre[v] = W_DIPRE(v);
        for (int p = 0; p < 2; p++) { w.di_guard[v][p] = W_DIG(v, p); w.di_trail[v][p] = W_DIT(v, p); }
    }
    w.video_pre = W_VPRE;
    w.video_guard = W_VGUARD;
    return w;
}

typedef struct {
    const hdmi_tl_geom_t *g;
    int v, audio;
    int type;                 // 0 active, 1 blank, 2 scanline
    unsigned count;
} ref_t;

// What pixel `px` of such a line must be.
static uint32_t ref_word(const ref_t *r, unsigned px, int *kind) {
    const hdmi_tl_geom_t *g = r->g;
    const unsigned bp_end = (unsigned)g->hs_px + g->bp_px;
    const int island = r->audio && r->type != 2;
    *kind = 0;
    if (island && px < HDMI_TL_DI_PX) {
        if (px < 8)  return W_DIPRE(r->v);
        if (px < 10) return W_DIG(r->v, px - 8);
        if (px < 42) return W_CHAR(px - 10);
        return W_DIT(r->v, px - 42);
    }
    if (r->type == 1) {                                    // blank: sync to the end
        return px < g->hs_px ? W_SYNC(r->v, 0) : W_SYNC(r->v, 1);
    }
    const unsigned guards = r->audio ? HDMI_TL_GUARDS_PX : 0;
    if (px < g->hs_px) return W_SYNC(r->v, 0);
    if (px < bp_end - guards) return W_SYNC(r->v, 1);
    if (px < bp_end - HDMI_TL_VG_PX) return W_VPRE;
    if (px < bp_end) return W_VGUARD;
    if (px < bp_end + g->active_px) { *kind = 1; return r->type == 2 ? W_GRAY : W_PIX(px - bp_end); }
    return W_SYNC(r->v, 1);
}

static void emit(void *ctx, unsigned px, int kind, uint32_t word) {
    ref_t *r = (ref_t *)ctx;
    int rk;
    const uint32_t want = ref_word(r, px, &rk);
    if (px < r->g->total_px) {
        CHECK(word == want && kind == rk,
              "type %d audio %d v %d hs %u: px %u got %08x/%d want %08x/%d",
              r->type, r->audio, r->v, r->g->hs_px, px, word, kind, want, rk);
    }
    r->count++;
}

static void run_geom(int hs_b, int bp_b, int fp_b, int scr_b) {
    const hdmi_tl_geom_t g = hdmi_tl_geom(hs_b, bp_b, fp_b, scr_b);
    const hdmi_tl_words_t wd = words();
    static uint32_t buf[HDMI_TL_MAX_WORDS + 64];
    CHECK(g.total_px == 800, "total %u", g.total_px);
    for (int audio = 0; audio < 2; audio++) {
        for (int v = 0; v < 2; v++) {
            for (int type = 0; type < 3; type++) {
                if (type == 2 && v == 0) continue;              // scanline lines are active lines: V idle
                memset(buf, 0xEE, sizeof buf);
                uint32_t *px = 0, *chars = 0;
                int n;
                if (type == 0)      n = hdmi_tl_active(buf, &g, &wd, v, audio, &px, &chars);
                else if (type == 1) n = hdmi_tl_blank(buf, &g, &wd, v, audio, &chars);
                else                n = hdmi_tl_scanline(buf, &g, &wd, audio, W_GRAY);
                CHECK(n > 0 && n <= HDMI_TL_MAX_WORDS, "n=%d", n);
                if (type == 0) {
                    CHECK(px != 0, "no pixel pointer");
                    for (unsigned i = 0; i < g.active_px; i++) px[i] = W_PIX(i);
                    CHECK(px + g.active_px <= buf + n, "pixels run past the list");
                }
                if (audio && type != 2) {
                    CHECK(chars != 0, "no island pointer");
                    for (int i = 0; i < HDMI_TL_DI_CHARS; i++) chars[i] = W_CHAR(i);
                } else {
                    CHECK(chars == 0, "island pointer on a line without an island");
                }
                ref_t r = { &g, v, audio, type, 0 };
                const int played = hdmi_tl_play(buf, n, emit, &r);
                CHECK(played == (int)g.total_px, "type %d audio %d v %d: played %d of %u",
                      type, audio, v, played, g.total_px);
                CHECK(buf[n] == 0xEEEEEEEEu, "builder wrote past its own count");
            }
        }
    }
}

int main(void) {
    run_geom(48, 24, 8, 320);   // 640x480 family
    run_geom(16, 16, 8, 360);   // 720-wide family (island tail crosses into the back porch)

    // A zero-length run must emit nothing at all.
    { uint32_t b[4] = {1, 1, 1, 1}; CHECK(hdmi_tl_rep(b, 0, 5) == b && b[0] == 1, "zero run wrote"); }
    // Malformed lists are refused by the player.
    { uint32_t b[2] = { HSTX_CMD_RAW | 5, 0 }; ref_t r = {0,0,0,0,0}; (void)r;
      CHECK(hdmi_tl_play(b, 2, emit, &r) == -1, "overrun accepted"); }
    { uint32_t b[1] = { 0x7000u }; ref_t r = {0,0,0,0,0};
      CHECK(hdmi_tl_play(b, 1, emit, &r) == -1, "unknown command accepted"); }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
