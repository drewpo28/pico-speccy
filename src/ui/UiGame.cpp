// pico-speccy — Skvosh, the built-in paddle game.
//
// A tribute to andykarpov's "skvosh" (a discrete-logic AY-3-8500-style squash
// console): one paddle on the right, three walls, a ball that speeds up with
// every return. It is NATIVE — no emulated machine is involved. The page runs
// inside the fullscreen menu like any other K_PAGE (own key loop, drawn with
// the nm:: rasteriser), so it works identically in the standard 8bpp modes and
// in Profi DS80, and costs nothing when not running: no heap, no .bss beyond
// the session-best score.
//
// Controls: Up/Down (also Q/A, and the joystick via the injected VK_MENU_*),
// Space/Enter/fire serves, P pauses, Esc/F1 leaves.
//
// Sound goes straight through pwm_audio_write like OSD::clickNoPause — the
// audio DMA keeps draining while the menu is up, so a synthesized square wave
// in a stack buffer is all a bounce needs. The staging buffer holds 640
// samples (~20 ms @31250) and the mixer HOLDS the last sample after it drains,
// so every beep is capped to it and ends at 0.

#include "OSDNewMenu.h"

#include "UiActions.h"
#include "UiStrings.h"
#include "UiGfx.h"
#include "UiFont.h"
#include "UiRender.h"           // SYM_* glyphs for the footer hints
#include "ESPectrum.h"
#include "Config.h"
#include "pwm_audio.h"

#include "pico/time.h"
#include "pico/rand.h"

#include <cstdio>

namespace nm {

// Session best. Deliberately not a Config setting: a high score is not worth
// an NVS write cycle, and losing it at power-off is period-correct.
static uint16_t s_best = 0;

static inline bool vkDown(fabgl::VirtualKey vk) {
    return ESPectrum::PS2Controller.keyboard()->isVKDown(vk);
}

// One square-wave beep. `half` = half-period in samples (freq = 31250 / (2*half)),
// `len` capped to the 640-sample staging buffer. Fixed loudness like the menu
// click; silent in tape-player mode for the same reason clickNoPause is.
static void beep(int half, int len) {
    if (Config::tape_player) return;
    uint8_t buf[640];
    if (len > (int)sizeof(buf)) len = (int)sizeof(buf);
    for (int i = 0; i < len; i++) buf[i] = ((i / half) & 1) ? 0 : 26;
    buf[len - 1] = 0;               // the mixer holds the tail sample — end silent
    pwm_audio_set_volume(ESP_VOLUME_MAX);
    pwm_audio_write(buf, buf, (size_t)len, nullptr, 0);
    pwm_audio_set_volume(ESPectrum::aud_volume);
}

static inline void beepWall()   { beep(24, 320); }   // ~651 Hz, 10 ms
static inline void beepPaddle() { beep(16, 400); }   // ~977 Hz, 13 ms
static inline void beepMiss()   { beep(72, 630); }   // ~217 Hz, 20 ms

void act_gameSkvosh() {
    gfxResumePalette();             // runModal suspended the UI palette

    // ── geometry (logical px; horizontal sizes ×sc so DS80 keeps proportions) ──
    const int sc     = Sf.glyphScale;
    const int m      = 2 * sc;                      // outer margin
    const int hdr_h  = UI_FONT_H + 6;
    const int foot_h = UI_FONT_H + 4;
    const int wt  = 2;                              // top/bottom wall thickness
    const int wtx = 2 * sc;                         // left wall thickness

    const int ox0 = m, oy0 = hdr_h + 1;             // court box (walls included)
    const int ox1 = Sf.w - 1 - m, oy1 = Sf.h - 1 - foot_h - 1;
    const int ix0 = ox0 + wtx, iy0 = oy0 + wt;      // interior = ball space
    const int ix1 = ox1,       iy1 = oy1 - wt;      // right side is OPEN

    const int pw = 3 * sc, ph = 26;                 // paddle
    const int paddle_x = ix1 - pw - 2 * sc;
    const int bw = 4 * sc, bh = 4;                  // ball
    const int plane = paddle_x - bw;                // ball x when it meets the paddle

    // ── state ──────────────────────────────────────────────────────────────────
    enum { ST_SERVE, ST_PLAY, ST_OVER } st = ST_SERVE;
    int  py  = (iy0 + iy1 - ph) / 2;                // paddle top
    int  bx = 0, by = 0;                            // ball top-left, 8.8 fixed
    int  dx = 0, dy = 0;                            // 8.8 px/tick (dx pre-scale, ×sc on apply)
    int  speed  = 560;                              // |dx|, ~2.2 px/tick to start
    int  score  = 0, balls = 5;
    bool paused = false;
    uint32_t tick = 0;

    int old_bx = -1, old_by = -1, old_py = -1;      // last drawn positions

    auto rnd = [](int lo, int hi) {                 // inclusive
        return lo + (int)(get_rand_32() % (uint32_t)(hi - lo + 1));
    };

    // ── drawing ────────────────────────────────────────────────────────────────
    auto drawHud = [&]() {
        fill(m, 2, Sf.w - 2 * m, UI_FONT_H, C_PANEL_ALT);
        text(m + 2 * sc, 2, "SKVOSH", C_WHITE);
        char s[32];
        snprintf(s, sizeof(s), "SCORE %03d  BEST %03d", score, s_best);
        const int sw = textWidth(s);
        text(Sf.w - m - sw - 2 * sc, 2, s, C_TEXT);
        // the remaining balls, as little squares after the title
        int x = m + 2 * sc + textWidth("SKVOSH") + 4 * sc;
        for (int i = 0; i < 5; i++, x += 6 * sc)
            fill(x, 4, 4 * sc, 4, i < balls ? C_ACCENT : C_PANEL_ALT);
    };

    auto drawCourt = [&]() {
        fill(0, 0, Sf.w, Sf.h, C_BG);
        drawHud();
        fill(ox0, oy0, ox1 - ox0 + 1, wt, C_TEXT);                    // top wall
        fill(ox0, oy1 - wt + 1, ox1 - ox0 + 1, wt, C_TEXT);           // bottom wall
        fill(ox0, oy0, wtx, oy1 - oy0 + 1, C_TEXT);                   // left wall
        text(m, Sf.h - foot_h + 2,
             "Q/A " SYM_UP SYM_DOWN " Move  Space Serve  P Pause  Esc Exit",
             C_TEXT_DIM);
    };

    auto drawPaddle = [&]() {
        if (old_py >= 0) fill(paddle_x, old_py, pw, ph, C_BG);
        fill(paddle_x, py, pw, ph, C_ACCENT);
        old_py = py;
    };

    auto eraseBall = [&]() {
        if (old_bx >= 0) fill(old_bx, old_by, bw, bh, C_BG);
        old_bx = -1;
    };
    auto drawBall = [&]() {
        eraseBall();
        fill(bx >> 8, by >> 8, bw, bh, C_WHITE);
        old_bx = bx >> 8; old_by = by >> 8;
    };

    // Centered one-liner in the court; returns nothing, erased by rect.
    auto centerMsg = [&](const char* msg, UiColor ink) {
        text((Sf.w - textWidth(msg)) / 2, (iy0 + iy1 - UI_FONT_H) / 2, msg, ink);
    };
    auto eraseCenterMsg = [&]() {
        const int y = (iy0 + iy1 - UI_FONT_H) / 2;
        fill(ix0, y, ix1 - ix0 + 1, UI_FONT_H, C_BG);
        // The band spans the whole court width — repaint whatever it ate.
        if (old_bx >= 0 && old_by + bh > y && old_by < y + UI_FONT_H)
            fill(old_bx, old_by, bw, bh, C_WHITE);
        if (old_py >= 0 && old_py + ph > y && old_py < y + UI_FONT_H)
            fill(paddle_x, old_py, pw, ph, C_ACCENT);
    };

    auto gameOverBox = [&]() {
        char sl[40];
        snprintf(sl, sizeof(sl), "Score %d   Best %d", score, s_best);
        const char* l1 = "GAME OVER";
        const char* l3 = "Enter Play again   Esc Exit";
        int w = textWidth(l3);
        if (textWidth(sl) > w) w = textWidth(sl);
        w += 8 * sc;
        const int h = 3 * (UI_FONT_H + 2) + 8;
        const int x = (Sf.w - w) / 2, y = (iy0 + iy1 - h) / 2;
        roundRect(x, y, w, h, 2, C_SEP, C_PANEL);
        text((Sf.w - textWidth(l1)) / 2, y + 5, l1, C_WHITE);
        text((Sf.w - textWidth(sl)) / 2, y + 5 + (UI_FONT_H + 2), sl, C_TEXT);
        text((Sf.w - textWidth(l3)) / 2, y + 5 + 2 * (UI_FONT_H + 2), l3, C_TEXT_DIM);
    };

    // ── state transitions ──────────────────────────────────────────────────────
    auto placeServe = [&]() {
        bx = (paddle_x - bw - sc) << 8;
        by = (py + (ph - bh) / 2) << 8;
        st = ST_SERVE;
    };

    auto serve = [&]() {
        eraseCenterMsg();
        dx = -speed;
        dy = rnd(0, 1) ? rnd(96, 384) : -rnd(96, 384);
        st = ST_PLAY;
    };

    auto newGame = [&]() {
        score = 0; balls = 5; speed = 560; paused = false;
        py = (iy0 + iy1 - ph) / 2;
        old_bx = -1; old_py = -1;
        drawCourt();
        drawPaddle();
        placeServe();
    };

    // Drain whatever opened us (the Enter that activated the row).
    auto kbd = ESPectrum::PS2Controller.keyboard();
    { fabgl::VirtualKeyItem d; while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d); }

    newGame();

    // ── main loop: 60 ticks/s ──────────────────────────────────────────────────
    uint64_t next = time_us_64();
    while (true) {
        // edge events
        fabgl::VirtualKeyItem k;
        while (kbd->virtualKeyAvailable()) {
            if (!ESPectrum::readKbd(&k) || !k.down) continue;
            switch (k.vk) {
                case fabgl::VK_ESCAPE:
                case fabgl::VK_F1:
                case fabgl::VK_MENU_BS:
                    return;
                case fabgl::VK_SPACE:
                case fabgl::VK_RETURN:
                case fabgl::VK_MENU_ENTER:
                    if (st == ST_SERVE) serve();
                    else if (st == ST_OVER) newGame();
                    else if (paused) { paused = false; eraseCenterMsg(); }
                    break;
                case fabgl::VK_p:
                case fabgl::VK_P:
                    if (st == ST_PLAY) {
                        paused = !paused;
                        if (paused) centerMsg("PAUSE", C_TEXT_DIM);
                        else eraseCenterMsg();
                    }
                    break;
                default: break;
            }
        }

        if (st != ST_OVER && !paused) {
            // paddle follows the held keys (arrows, Q/A, joystick)
            const bool up = vkDown(fabgl::VK_UP)   || vkDown(fabgl::VK_MENU_UP)
                         || vkDown(fabgl::VK_q)    || vkDown(fabgl::VK_Q);
            const bool dn = vkDown(fabgl::VK_DOWN) || vkDown(fabgl::VK_MENU_DOWN)
                         || vkDown(fabgl::VK_a)    || vkDown(fabgl::VK_A);
            const int pv = 4;
            if (up && !dn) { py -= pv; if (py < iy0) py = iy0; }
            if (dn && !up) { py += pv; if (py > iy1 - ph + 1) py = iy1 - ph + 1; }
            if (py != old_py) drawPaddle();

            if (st == ST_SERVE) {
                // ball rides the paddle; hint blinks
                by = (py + (ph - bh) / 2) << 8;
                bx = (paddle_x - bw - sc) << 8;
                if ((bx >> 8) != old_bx || (by >> 8) != old_by) drawBall();
                if ((tick & 31) == 0) {
                    if (tick & 32) eraseCenterMsg();
                    else centerMsg("SPACE - SERVE", C_TEXT_DIM);
                }
            } else {                                 // ST_PLAY
                const int prev_x = bx;
                bx += dx * sc;
                by += dy;

                // walls
                if (by < (iy0 << 8))            { by = iy0 << 8;            dy = -dy; beepWall(); }
                if (by > ((iy1 - bh + 1) << 8)) { by = (iy1 - bh + 1) << 8; dy = -dy; beepWall(); }
                if (bx < (ix0 << 8))            { bx = ix0 << 8;            dx = speed; beepWall(); }

                // the paddle plane, crossing-tested so a fast ball cannot tunnel
                if (dx > 0 && prev_x <= (plane << 8) && bx >= (plane << 8)) {
                    const int bc = (by >> 8) + bh / 2;
                    if (bc >= py - bh / 2 && bc <= py + ph + bh / 2 - 1) {
                        bx = plane << 8;
                        if (speed < 1536) speed += speed / 24;   // ~4% per return
                        dx = -speed;
                        dy = (bc - (py + ph / 2)) * 52;          // english: edge ≈ ±3 px/t
                        if (dy > -64 && dy < 64) dy += (dy < 0 ? -64 : 64);
                        score++;
                        if (score > s_best) s_best = score;
                        drawHud();
                        beepPaddle();
                    }
                }

                // out on the right — ball lost
                if ((bx >> 8) > ix1) {
                    eraseBall();
                    beepMiss();
                    balls--;
                    drawHud();
                    if (balls <= 0) { st = ST_OVER; gameOverBox(); }
                    else placeServe();
                } else {
                    drawBall();
                }
            }
        }

        tick++;
        next += 16667;
        const int64_t left = (int64_t)(next - time_us_64());
        if (left > 0) sleep_us((uint64_t)left);
        else next = time_us_64();                    // fell behind — don't rush to catch up
    }
}

} // namespace nm
