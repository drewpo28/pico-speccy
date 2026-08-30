// pico-speccy — Skvosh, the built-in paddle game.
//
// A tribute to andykarpov's "skvosh" (a discrete-logic AY-3-8500-style squash
// console): paddles, walls, a ball that speeds up with every return. It is
// NATIVE — no emulated machine is involved. The page runs inside the
// fullscreen menu like any other K_PAGE (own key loop, drawn with the nm::
// rasteriser), so it works identically in the standard 8bpp modes and in
// Profi DS80, and costs nothing when not running: no heap, no .bss beyond the
// session-best score and the remembered mode row.
//
// Two games behind one root row, picked on an in-page menu:
//  * Solo squash — one paddle on the right, three walls, score = returns,
//    5 balls a game.
//  * Pong vs CPU — the left wall is replaced by a computer paddle, first to
//    11 points. Three difficulty levels, differing in what the CPU can DO,
//    not in the ball: paddle speed, a per-rally aim error, and (hard only)
//    predicting the ball's arrival point with wall reflections instead of
//    chasing its current y. Hard is deliberately capped at the player's own
//    paddle speed minus one, so sharp-angle returns can still outrun it.
//
// Controls: Up/Down (also Q/A, and the joystick via the injected VK_MENU_*),
// Space/Enter/fire serves, P pauses, M returns to the mode menu from the
// game-over box, Esc/F1 leaves.
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

// Session state. Deliberately not Config settings: a high score is not worth
// an NVS write cycle, and losing it at power-off is period-correct.
static uint16_t s_best = 0;         // solo squash best
static uint8_t  s_menu_sel = 0;     // last picked mode row

// What the CPU is allowed to do per difficulty. `pv` = paddle px/tick against
// the player's 4; `err` = half-range of the per-rally aim error in px;
// `predict` = compute the arrival y with wall reflections instead of chasing
// the ball's current y; `lazy` = react only once the ball is in the left 2/3.
struct CpuSkill { uint8_t pv; uint8_t err; bool predict; bool lazy; };
static const CpuSkill k_cpu[3] = {
    { 2, 12, false, true  },        // Easy
    { 3, 6,  false, false },        // Normal
    { 3, 3,  true,  false },        // Hard
};
static const char* const k_cpu_name[3] = { "Easy", "Normal", "Hard" };

static inline bool vkDown(fabgl::VirtualKey vk) {
    return ESPectrum::PS2Controller.keyboard()->isVKDown(vk);
}

// Which menu/game verb a key event carries, or 0 for "not one of ours".
// Every arrow, Enter and Space arrives TWICE: the input layer queues a
// VK_MENU_* twin right beside the raw key (main.cpp kbdExtraMapping for USB,
// the PS/2 scancode table for PS/2, and repeat_handler for auto-repeat), which
// is what the nm:: menus decode. A switch that accepts BOTH therefore acts
// twice per press — the mode selection stepped two rows at a time, and picking
// a mode with Space also served the ball with the twin. Collapsing a repeat of
// the same verb inside one drain pass is enough (the twin is always queued
// immediately before its raw key, so both land in the same 60 Hz tick), and it
// keeps the keys the input layer sends with NO twin working — KP-Enter, and
// the Q/A letters.
enum SkvoshAct : uint8_t { SA_NONE, SA_UP, SA_DOWN, SA_FIRE, SA_BACK, SA_PAUSE, SA_MENU };

static SkvoshAct skvoshAct(fabgl::VirtualKey vk) {
    switch (vk) {
        case fabgl::VK_UP:   case fabgl::VK_MENU_UP:
        case fabgl::VK_q:    case fabgl::VK_Q:          return SA_UP;
        case fabgl::VK_DOWN: case fabgl::VK_MENU_DOWN:
        case fabgl::VK_a:    case fabgl::VK_A:          return SA_DOWN;
        case fabgl::VK_SPACE: case fabgl::VK_RETURN:
        case fabgl::VK_MENU_ENTER:                      return SA_FIRE;
        case fabgl::VK_ESCAPE: case fabgl::VK_F1:
        case fabgl::VK_MENU_BS:                         return SA_BACK;
        case fabgl::VK_p: case fabgl::VK_P:             return SA_PAUSE;
        case fabgl::VK_m: case fabgl::VK_M:             return SA_MENU;
        default:                                        return SA_NONE;
    }
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
static inline void beepScore()  { beep(12, 500); }   // ~1302 Hz — a point for YOU

void act_gameSkvosh() {
    gfxResumePalette();             // runModal suspended the UI palette

    // ── geometry (logical px; horizontal sizes ×sc so DS80 keeps proportions) ──
    const int sc     = Sf.glyphScale;
    const int m      = 2 * sc;                      // outer margin
    const int hdr_h  = UI_FONT_H + 6;
    const int foot_h = UI_FONT_H + 4;
    const int wt  = 2;                              // top/bottom wall thickness
    const int wtx = 2 * sc;                         // left wall thickness (squash)

    const int ox0 = m, oy0 = hdr_h + 1;             // court box (walls included)
    const int ox1 = Sf.w - 1 - m, oy1 = Sf.h - 1 - foot_h - 1;
    const int iy0 = oy0 + wt;                       // interior = ball space
    const int iy1 = oy1 - wt;
    const int wall_ix0 = ox0 + wtx;                 // squash: right face of the wall

    const int pw = 3 * sc, ph = 26;                 // both paddles
    const int paddle_x = ox1 - pw - 2 * sc;         // player, right side
    const int cpu_x    = ox0 + 2 * sc;              // CPU, left side (pong)
    const int bw = 4 * sc, bh = 4;                  // ball
    const int plane_p = paddle_x - bw;              // ball x when it meets the player
    const int plane_c = cpu_x + pw;                 // ball x when it meets the CPU
    // The serve hint sits BELOW the court centre so the pong serve ball
    // (parked exactly at the centre) never collides with the text.
    const int msg_y = (iy0 + iy1 - UI_FONT_H) / 2 + 20;

    // ── state ──────────────────────────────────────────────────────────────────
    enum { ST_MENU, ST_SERVE, ST_PLAY, ST_OVER } st = ST_MENU;
    enum { MODE_SQUASH, MODE_PONG } mode = MODE_SQUASH;
    int  diff = 0;                                  // index into k_cpu
    int  sel  = s_menu_sel;                         // mode-menu row

    int  py = (iy0 + iy1 - ph) / 2;                 // player paddle top
    int  cy = py;                                   // CPU paddle top
    int  bx = 0, by = 0;                            // ball top-left, 8.8 fixed
    int  dx = 0, dy = 0;                            // 8.8 px/tick (dx pre-scale, ×sc on apply)
    int  speed  = 560;                              // |dx|, ~2.2 px/tick to start
    int  score = 0, balls = 5;                      // squash
    int  score_p = 0, score_c = 0;                  // pong, first to 11
    int  serve_dx = 1;                              // pong serve direction (+1 = at player)
    int  cpu_err = 0;                               // this rally's aim error
    bool paused = false;
    uint32_t tick = 0;

    int old_bx = -1, old_by = -1, old_py = -1, old_cy = -1;   // last drawn positions

    auto rnd = [](int lo, int hi) {                 // inclusive
        return lo + (int)(get_rand_32() % (uint32_t)(hi - lo + 1));
    };

    // ── drawing ────────────────────────────────────────────────────────────────
    auto drawHud = [&]() {
        fill(m, 2, Sf.w - 2 * m, UI_FONT_H, C_PANEL_ALT);
        text(m + 2 * sc, 2, "SKVOSH", C_WHITE);
        char s[40];
        if (mode == MODE_SQUASH) {
            snprintf(s, sizeof(s), "SCORE %03d  BEST %03d", score, s_best);
            // the remaining balls, as little squares after the title
            int x = m + 2 * sc + textWidth("SKVOSH") + 4 * sc;
            for (int i = 0; i < 5; i++, x += 6 * sc)
                fill(x, 4, 4 * sc, 4, i < balls ? C_ACCENT : C_PANEL_ALT);
        } else {
            snprintf(s, sizeof(s), "%s   CPU %02d : %02d YOU",
                     k_cpu_name[diff], score_c, score_p);
        }
        text(Sf.w - m - textWidth(s) - 2 * sc, 2, s, C_TEXT);
    };

    auto drawCourt = [&]() {
        fill(0, 0, Sf.w, Sf.h, C_BG);
        drawHud();
        fill(ox0, oy0, ox1 - ox0 + 1, wt, C_TEXT);                    // top wall
        fill(ox0, oy1 - wt + 1, ox1 - ox0 + 1, wt, C_TEXT);           // bottom wall
        if (mode == MODE_SQUASH) {
            fill(ox0, oy0, wtx, oy1 - oy0 + 1, C_TEXT);               // left wall
        } else {
            // centre line, pong-style
            for (int y = iy0 + 2; y < iy1 - 2; y += 8)
                fill((Sf.w - sc) / 2, y, sc, 4, C_TEXT_DIM);
        }
        text(m, Sf.h - foot_h + 2,
             "Q/A " SYM_UP SYM_DOWN " Move  Space Serve  P Pause  Esc Exit",
             C_TEXT_DIM);
    };

    auto drawPaddle = [&]() {
        if (old_py >= 0) fill(paddle_x, old_py, pw, ph, C_BG);
        fill(paddle_x, py, pw, ph, C_ACCENT);
        old_py = py;
    };
    auto drawCpuPaddle = [&]() {
        if (old_cy >= 0) fill(cpu_x, old_cy, pw, ph, C_BG);
        fill(cpu_x, cy, pw, ph, C_ACCENT);
        old_cy = cy;
    };

    // Repaint the pong centre-line dashes inside a just-erased rect — the ball
    // (and the serve-hint band) would otherwise eat holes into the line.
    auto repaintCenterLine = [&](int x, int y, int w, int h) {
        if (mode != MODE_PONG) return;
        const int lx = (Sf.w - sc) / 2;
        if (x > lx + sc - 1 || x + w - 1 < lx) return;
        for (int dyy = iy0 + 2; dyy < iy1 - 2; dyy += 8) {
            const int t = dyy > y ? dyy : y;
            const int b = (dyy + 4 < y + h) ? dyy + 4 : y + h;
            if (t < b) fill(lx, t, sc, b - t, C_TEXT_DIM);
        }
    };

    auto eraseBall = [&]() {
        if (old_bx >= 0) {
            fill(old_bx, old_by, bw, bh, C_BG);
            repaintCenterLine(old_bx, old_by, bw, bh);
        }
        old_bx = -1;
    };
    auto drawBall = [&]() {
        eraseBall();
        fill(bx >> 8, by >> 8, bw, bh, C_WHITE);
        old_bx = bx >> 8; old_by = by >> 8;
    };

    // Centered one-liner in the court, on its own small panel (same chrome as
    // the game-over box) so the pong centre line never shows through the text.
    const int box_y = msg_y - 4, box_h = UI_FONT_H + 8;
    auto centerMsg = [&](const char* msg, UiColor ink) {
        const int w = textWidth(msg) + 8 * sc;
        roundRect((Sf.w - w) / 2, box_y, w, box_h, 2, C_SEP, C_PANEL);
        text((Sf.w - textWidth(msg)) / 2, msg_y, msg, ink);
    };
    auto eraseCenterMsg = [&]() {
        fill(ox0, box_y, ox1 - ox0 + 1, box_h, C_BG);
        // The band spans the whole court width — repaint whatever it ate.
        // The squash left wall is one of them: the serve hint blinks (erase
        // every 32 ticks) from the moment a solo game starts, so the wall came
        // up with a hole in it before the player had touched anything.
        if (mode == MODE_SQUASH) {
            const int t = box_y > oy0 ? box_y : oy0;
            const int b = (box_y + box_h < oy1 + 1) ? box_y + box_h : oy1 + 1;
            if (t < b) fill(ox0, t, wtx, b - t, C_TEXT);
        }
        if (old_bx >= 0 && old_by + bh > box_y && old_by < box_y + box_h)
            fill(old_bx, old_by, bw, bh, C_WHITE);
        if (old_py >= 0 && old_py + ph > box_y && old_py < box_y + box_h)
            fill(paddle_x, old_py, pw, ph, C_ACCENT);
        if (mode == MODE_PONG && old_cy >= 0 &&
            old_cy + ph > box_y && old_cy < box_y + box_h)
            fill(cpu_x, old_cy, pw, ph, C_ACCENT);
        repaintCenterLine(ox0, box_y, ox1 - ox0 + 1, box_h);
    };

    auto gameOverBox = [&]() {
        char sl[40];
        const char* l1;
        if (mode == MODE_SQUASH) {
            l1 = "GAME OVER";
            snprintf(sl, sizeof(sl), "Score %d   Best %d", score, s_best);
        } else {
            l1 = score_p > score_c ? "YOU WIN!" : "CPU WINS";
            snprintf(sl, sizeof(sl), "CPU %d : %d YOU  (%s)",
                     score_c, score_p, k_cpu_name[diff]);
        }
        const char* l3 = "Enter Again  M Menu  Esc Exit";
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

    // ── the mode menu ──────────────────────────────────────────────────────────
    static const char* const k_rows[4] = {
        "Solo squash", "vs CPU - Easy", "vs CPU - Normal", "vs CPU - Hard",
    };
    auto drawModeMenu = [&]() {
        fill(0, 0, Sf.w, Sf.h, C_BG);
        const char* t = "S K V O S H";
        text((Sf.w - textWidth(t)) / 2, Sf.h / 5, t, C_WHITE);
        hline((Sf.w - textWidth(t)) / 2, Sf.h / 5 + UI_FONT_H + 1, textWidth(t), C_SEP);
        if (s_best) {
            char b[24];
            snprintf(b, sizeof(b), "solo best %d", s_best);
            text((Sf.w - textWidth(b)) / 2, Sf.h / 5 + UI_FONT_H + 5, b, C_TEXT_DIM);
        }
        const int lh = UI_FONT_H + 6;
        int rw = 0;
        for (int i = 0; i < 4; i++)
            if (textWidth(k_rows[i]) > rw) rw = textWidth(k_rows[i]);
        rw += 12 * sc;
        const int rx = (Sf.w - rw) / 2;
        const int ry = Sf.h / 5 + 3 * UI_FONT_H;
        for (int i = 0; i < 4; i++) {
            const int y = ry + i * lh;
            fill(rx, y, rw, lh - 2, i == sel ? C_SEL_BG : C_BG);
            text(rx + 6 * sc, y + 2, k_rows[i], i == sel ? C_WHITE : C_TEXT);
        }
        text(m, Sf.h - foot_h + 2,
             SYM_UP SYM_DOWN " Select  Enter Start  Esc Exit", C_TEXT_DIM);
    };

    // ── state transitions ──────────────────────────────────────────────────────
    auto placeServe = [&]() {
        if (mode == MODE_SQUASH) {
            bx = (paddle_x - bw - sc) << 8;
            by = (py + (ph - bh) / 2) << 8;
        } else {
            bx = ((Sf.w - bw) / 2) << 8;
            by = ((iy0 + iy1 - bh) / 2) << 8;
            drawBall();
        }
        st = ST_SERVE;
    };

    auto serve = [&]() {
        eraseCenterMsg();
        dx = (mode == MODE_PONG && serve_dx > 0) ? speed : -speed;
        dy = rnd(0, 1) ? rnd(96, 384) : -rnd(96, 384);
        cpu_err = rnd(-(int)k_cpu[diff].err, (int)k_cpu[diff].err);
        st = ST_PLAY;
    };

    auto newGame = [&]() {
        score = 0; balls = 5; score_p = 0; score_c = 0;
        speed = 560; paused = false; serve_dx = rnd(0, 1) ? 1 : -1;
        py = (iy0 + iy1 - ph) / 2; cy = py;
        old_bx = -1; old_py = -1; old_cy = -1;
        drawCourt();
        drawPaddle();
        if (mode == MODE_PONG) drawCpuPaddle();
        placeServe();
    };

    // Where the ball's centre will be when it reaches the CPU plane, with wall
    // reflections folded in. All 8.8 fixed; used by the Hard CPU only.
    auto predictY = [&]() -> int {
        const int span = (bx >> 8) - plane_c;             // logical px to travel
        if (span <= 0 || dx >= 0) return (by >> 8) + bh / 2;
        const int ticks = (span << 8) / ((-dx) * sc);     // whole ticks to arrival
        int y = by + dy * ticks;
        const int lo = iy0 << 8, hi = (iy1 - bh + 1) << 8, range = hi - lo;
        y -= lo;
        y %= 2 * range; if (y < 0) y += 2 * range;
        if (y > range) y = 2 * range - y;
        return ((y + lo) >> 8) + bh / 2;
    };

    // One CPU step: pick a target, move at most `pv` toward it.
    auto cpuStep = [&]() {
        const CpuSkill& sk = k_cpu[diff];
        int target;                                        // desired ball-centre y
        if (st != ST_PLAY || dx > 0) {
            target = (iy0 + iy1) / 2;                      // ball going away — recentre
        } else if (sk.lazy && (bx >> 8) > ox0 + ((ox1 - ox0) * 2) / 3) {
            return;                                        // Easy: hasn't noticed yet
        } else {
            target = sk.predict ? predictY() : (by >> 8) + bh / 2;
            target += cpu_err;
        }
        int want = target - ph / 2;
        if (want < iy0) want = iy0;
        if (want > iy1 - ph + 1) want = iy1 - ph + 1;
        if      (cy < want) { cy += sk.pv; if (cy > want) cy = want; }
        else if (cy > want) { cy -= sk.pv; if (cy < want) cy = want; }
        if (cy != old_cy) drawCpuPaddle();
    };

    // A pong point was decided. `player_scored` names the winner of the rally.
    auto pongPoint = [&](bool player_scored) {
        eraseBall();
        if (player_scored) { score_p++; beepScore(); } else { score_c++; beepMiss(); }
        drawHud();
        speed = 560;                                       // each rally starts calm
        if (score_p >= 11 || score_c >= 11) { st = ST_OVER; gameOverBox(); return; }
        serve_dx = player_scored ? -1 : 1;                 // the loser receives
        placeServe();
    };

    // Drain whatever opened us (the Enter that activated the row).
    auto kbd = ESPectrum::PS2Controller.keyboard();
    { fabgl::VirtualKeyItem d; while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d); }

    drawModeMenu();

    // ── main loop: 60 ticks/s ──────────────────────────────────────────────────
    uint64_t next = time_us_64();
    while (true) {
        // edge events. Decoded to verbs so a key and its VK_MENU_* twin (both
        // queued for every arrow / Enter / Space) act once — see skvoshAct().
        fabgl::VirtualKeyItem k;
        SkvoshAct prev_act = SA_NONE;
        while (kbd->virtualKeyAvailable()) {
            if (!ESPectrum::readKbd(&k) || !k.down) continue;
            const SkvoshAct act = skvoshAct(k.vk);
            if (act == SA_NONE || act == prev_act) continue;
            prev_act = act;
            switch (act) {
                case SA_BACK:
                    if (st == ST_MENU) return;
                    s_menu_sel = (uint8_t)sel;
                    st = ST_MENU; paused = false;
                    drawModeMenu();
                    break;
                case SA_UP:
                    if (st == ST_MENU && sel > 0) { sel--; drawModeMenu(); }
                    break;
                case SA_DOWN:
                    if (st == ST_MENU && sel < 3) { sel++; drawModeMenu(); }
                    break;
                case SA_FIRE:
                    if (st == ST_MENU) {
                        s_menu_sel = (uint8_t)sel;
                        mode = sel == 0 ? MODE_SQUASH : MODE_PONG;
                        diff = sel == 0 ? 0 : sel - 1;
                        newGame();
                    }
                    else if (st == ST_SERVE) serve();
                    else if (st == ST_OVER) newGame();
                    else if (paused) { paused = false; eraseCenterMsg(); }
                    break;
                case SA_PAUSE:
                    if (st == ST_PLAY) {
                        paused = !paused;
                        if (paused) centerMsg("PAUSE", C_TEXT_DIM);
                        else eraseCenterMsg();
                    }
                    break;
                case SA_MENU:
                    if (st == ST_OVER) {
                        s_menu_sel = (uint8_t)sel;
                        st = ST_MENU;
                        drawModeMenu();
                    }
                    break;
                default: break;
            }
        }

        if ((st == ST_SERVE || st == ST_PLAY) && !paused) {
            // player paddle follows the held keys (arrows, Q/A, joystick)
            const bool up = vkDown(fabgl::VK_UP)   || vkDown(fabgl::VK_MENU_UP)
                         || vkDown(fabgl::VK_q)    || vkDown(fabgl::VK_Q);
            const bool dn = vkDown(fabgl::VK_DOWN) || vkDown(fabgl::VK_MENU_DOWN)
                         || vkDown(fabgl::VK_a)    || vkDown(fabgl::VK_A);
            const int pv = 4;
            if (up && !dn) { py -= pv; if (py < iy0) py = iy0; }
            if (dn && !up) { py += pv; if (py > iy1 - ph + 1) py = iy1 - ph + 1; }
            if (py != old_py) drawPaddle();

            if (mode == MODE_PONG) cpuStep();

            if (st == ST_SERVE) {
                if (mode == MODE_SQUASH) {
                    // ball rides the paddle
                    by = (py + (ph - bh) / 2) << 8;
                    bx = (paddle_x - bw - sc) << 8;
                    if ((bx >> 8) != old_bx || (by >> 8) != old_by) drawBall();
                }
                if ((tick & 31) == 0) {                    // hint blinks
                    if (tick & 32) eraseCenterMsg();
                    else centerMsg("SPACE - SERVE", C_TEXT_DIM);
                }
            } else {                                       // ST_PLAY
                const int prev_x = bx;
                bx += dx * sc;
                by += dy;

                // walls
                if (by < (iy0 << 8))            { by = iy0 << 8;            dy = -dy; beepWall(); }
                if (by > ((iy1 - bh + 1) << 8)) { by = (iy1 - bh + 1) << 8; dy = -dy; beepWall(); }
                if (mode == MODE_SQUASH && bx < (wall_ix0 << 8)) {
                    bx = wall_ix0 << 8; dx = speed; beepWall();
                }

                // paddle planes, crossing-tested so a fast ball cannot tunnel
                if (dx > 0 && prev_x <= (plane_p << 8) && bx >= (plane_p << 8)) {
                    const int bc = (by >> 8) + bh / 2;
                    if (bc >= py - bh / 2 && bc <= py + ph + bh / 2 - 1) {
                        bx = plane_p << 8;
                        if (speed < 1536) speed += speed / 24;   // ~4% per return
                        dx = -speed;
                        dy = (bc - (py + ph / 2)) * 52;          // english: edge ≈ ±3 px/t
                        if (dy > -64 && dy < 64) dy += (dy < 0 ? -64 : 64);
                        cpu_err = rnd(-(int)k_cpu[diff].err, (int)k_cpu[diff].err);
                        if (mode == MODE_SQUASH) {
                            score++;
                            if (score > s_best) s_best = score;
                            drawHud();
                        }
                        beepPaddle();
                    }
                }
                if (mode == MODE_PONG &&
                    dx < 0 && prev_x >= (plane_c << 8) && bx <= (plane_c << 8)) {
                    const int bc = (by >> 8) + bh / 2;
                    if (bc >= cy - bh / 2 && bc <= cy + ph + bh / 2 - 1) {
                        bx = plane_c << 8;
                        if (speed < 1536) speed += speed / 24;
                        dx = speed;
                        dy = (bc - (cy + ph / 2)) * 52;
                        if (dy > -64 && dy < 64) dy += (dy < 0 ? -64 : 64);
                        beepPaddle();
                    }
                }

                // out on either open side
                if ((bx >> 8) > ox1) {                     // past the player
                    if (mode == MODE_SQUASH) {
                        eraseBall();
                        beepMiss();
                        balls--;
                        drawHud();
                        if (balls <= 0) { st = ST_OVER; gameOverBox(); }
                        else placeServe();
                    } else {
                        pongPoint(false);
                    }
                } else if (mode == MODE_PONG && (bx >> 8) + bw < ox0) {  // past the CPU
                    pongPoint(true);
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
