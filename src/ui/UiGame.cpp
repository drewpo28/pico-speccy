// pico-speccy — Pico-Scwong, the built-in paddle game.
//
// A tribute to andykarpov's "skvosh" (a discrete-logic AY-3-8500-style squash
// console): paddles, walls, a ball that speeds up with every return. It is
// NATIVE — no emulated machine is involved, and nothing here touches the SD
// card (the options persist through Config::save(), which quietly keeps them
// in RAM for the session when no card is mounted). The page runs inside the
// fullscreen menu like any other K_PAGE (own key loop, drawn with the nm::
// rasteriser), so it works identically in the standard 8bpp modes and in
// Profi DS80. `gameScwongStandalone()` is the second entrance: held S in the
// boot-time R/M probe window launches it before the emulation loop ever runs.
//
// Two games behind one in-page mode menu:
//  * Solo squash — one paddle on the right, three walls, score = returns,
//    5 balls a game.
//  * Pong vs CPU — the left wall is replaced by a computer paddle, first to
//    11 points. Three difficulty levels, differing in what the CPU can DO
//    (paddle speed, a per-rally aim error, and — Hard only — predicting the
//    arrival point with wall reflections) AND in the ball's serve speed /
//    speed cap / acceleration, so Easy stays slow enough to actually be easy.
//    Hard is deliberately capped below the player: sharp-angle returns can
//    still outrun it.
//
// The Options page (last mode-menu row) picks the court and paddle colours,
// the paddle width, the ball size and the player's paddle speed — stored in
// Config as gm_* keys.
//
// Controls: Up/Down (also Q/A, and the joystick via the injected VK_MENU_*),
// Space/Enter/fire serves, P pauses, M returns to the mode menu from the
// game-over box, Esc/F1 backs out one screen at a time.
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
#include "Video.h"              // brdnextframe for the standalone exit repaint
#include "pwm_audio.h"

#include "pico/time.h"
#include "pico/rand.h"

#include <cstdio>

namespace nm {

// Session state. The best score is deliberately not a Config setting: a high
// score is not worth an NVS write cycle, and losing it at power-off is
// period-correct.
static uint16_t s_best = 0;         // solo squash best
static uint8_t  s_menu_sel = 0;     // last picked mode row

// What the CPU is allowed to do per difficulty, plus how the ball behaves in
// that game: `pv` = CPU paddle px/tick; `err` = half-range of the per-rally
// aim error in px; `predict` = compute the arrival y with wall reflections
// instead of chasing the ball's current y; `lazy` = react only once the ball
// is in the left 2/3. `serve`/`cap` = ball |dx| in 8.8 px/tick at serve and
// at most; `accel` = speed += speed/accel per return (bigger = gentler).
struct CpuSkill {
    uint8_t  pv, err;
    bool     predict, lazy;
    uint16_t serve, cap;
    uint8_t  accel;
};
static const CpuSkill k_cpu[3] = {
    { 2, 16, false, true,  480,  900, 32 },     // Easy: slow ball, wobbly aim
    { 3, 8,  false, false, 560, 1200, 28 },     // Normal
    { 3, 3,  true,  false, 560, 1536, 24 },     // Hard
};
static const char* const k_cpu_name[3] = { "Easy", "Normal", "Hard" };
// Solo squash keeps the original ball behaviour.
static const CpuSkill k_solo = { 0, 0, false, false, 560, 1536, 24 };

// ── the Options tables (values live in Config::gm_*, persisted as NVS keys) ──
static const UiColor k_fieldCol[]  = { C_BG, C_SHADOW, C_PANEL, C_SEL_BAND, C_FOOT_BG };
static const char* const k_fieldName[] = { "Dark", "Black", "Navy", "Steel", "Charcoal" };
static const UiColor k_padCol[]    = { C_ACCENT, C_WHITE, C_ICON_R, C_ICON_Y, C_ICON_C, C_SEL_BG };
static const char* const k_padName[]   = { "Green", "White", "Red", "Yellow", "Cyan", "Blue" };
static const uint8_t k_padW[3]     = { 3, 5, 7 };
static const char* const k_padWName[]  = { "Slim", "Medium", "Wide" };
// Paddle SIZE is its length along the wall — the one option that also changes
// how hard the game is, and it changes it for BOTH sides (the CPU paddle uses
// the same ph). Vertical, so it is NOT scaled by glyphScale.
static const uint8_t k_padH[3]     = { 16, 26, 38 };
static const char* const k_padHName[]  = { "Short", "Normal", "Long" };
static const uint8_t k_ballSz[3]   = { 4, 6, 8 };
static const char* const k_ballName[]  = { "Small", "Medium", "Big" };
// The ball's own palette — White first, so the default stays the classic look.
static const UiColor k_ballCol[]   = { C_WHITE, C_ICON_Y, C_ICON_C, C_ICON_R, C_ACCENT, C_SEL_BG };
static const char* const k_ballColName[] = { "White", "Yellow", "Cyan", "Red", "Green", "Blue" };
static const uint8_t k_pSpd[3]     = { 3, 4, 6 };
static const char* const k_pSpdName[]  = { "Slow", "Normal", "Fast" };
#define GM_N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static inline bool vkDown(fabgl::VirtualKey vk) {
    return ESPectrum::PS2Controller.keyboard()->isVKDown(vk);
}

// Which menu/game verb a key event carries, or SA_NONE for "not one of ours".
// Every arrow, Enter and Space arrives TWICE: the input layer queues a
// VK_MENU_* twin right beside the raw key (main.cpp kbdExtraMapping for USB,
// the PS/2 scancode table for PS/2, and repeat_handler for auto-repeat), which
// is what the nm:: menus decode. A switch that accepts BOTH therefore acts
// twice per press — the mode selection stepped two rows at a time, picking a
// mode with Space also served the ball with the twin, and an option value
// jumped two steps per Left/Right. Collapsing a repeat of the same verb inside
// one drain pass is enough (the twin is always queued immediately before its
// raw key, so both land in the same 60 Hz tick), and it keeps the keys the
// input layer sends with NO twin working — KP-Enter, and the Q/A letters.
enum ScwongAct : uint8_t {
    SA_NONE, SA_UP, SA_DOWN, SA_LEFT, SA_RIGHT, SA_FIRE, SA_BACK, SA_PAUSE, SA_MENU
};

static ScwongAct scwongAct(fabgl::VirtualKey vk) {
    switch (vk) {
        case fabgl::VK_UP:    case fabgl::VK_MENU_UP:
        case fabgl::VK_q:     case fabgl::VK_Q:         return SA_UP;
        case fabgl::VK_DOWN:  case fabgl::VK_MENU_DOWN:
        case fabgl::VK_a:     case fabgl::VK_A:         return SA_DOWN;
        case fabgl::VK_LEFT:  case fabgl::VK_MENU_LEFT:  return SA_LEFT;
        case fabgl::VK_RIGHT: case fabgl::VK_MENU_RIGHT: return SA_RIGHT;
        case fabgl::VK_SPACE: case fabgl::VK_RETURN:
        case fabgl::VK_MENU_ENTER:                       return SA_FIRE;
        case fabgl::VK_ESCAPE: case fabgl::VK_F1:
        case fabgl::VK_MENU_BS:                          return SA_BACK;
        case fabgl::VK_p: case fabgl::VK_P:              return SA_PAUSE;
        case fabgl::VK_m: case fabgl::VK_M:              return SA_MENU;
        default:                                         return SA_NONE;
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

void act_gameScwong() {
    gfxResumePalette();             // runModal suspended the UI palette

    // ── fixed geometry (logical px; horizontal sizes ×sc for DS80) ────────────
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
    const int cpu_x = ox0 + 2 * sc;                 // CPU paddle, left side (pong)
    // The serve hint sits BELOW the court centre so the pong serve ball
    // (parked exactly at the centre) never collides with its panel.
    const int msg_y = (iy0 + iy1 - UI_FONT_H) / 2 + 20;
    const int box_y = msg_y - 4, box_h = UI_FONT_H + 8;

    // ── option-derived geometry (recomputed by applyOpts) ──────────────────────
    int pw = 3, ph = 26, bw = 4, bh = 4, pv_player = 4;
    int paddle_x = 0, plane_p = 0, plane_c = 0;
    UiColor colField = C_BG, colPad = C_ACCENT, colBall = C_WHITE;
    auto applyOpts = [&]() {
        colField  = k_fieldCol[Config::gm_field % GM_N(k_fieldCol)];
        colPad    = k_padCol[Config::gm_pad % GM_N(k_padCol)];
        colBall   = k_ballCol[Config::gm_ballc % GM_N(k_ballCol)];
        pw        = k_padW[Config::gm_padw % 3] * sc;
        ph        = k_padH[Config::gm_padh % 3];
        bw        = k_ballSz[Config::gm_ball % 3] * sc;
        bh        = k_ballSz[Config::gm_ball % 3];
        pv_player = k_pSpd[Config::gm_pspd % 3];
        paddle_x  = ox1 - pw - 2 * sc;
        plane_p   = paddle_x - bw;                  // ball x when it meets the player
        plane_c   = cpu_x + pw;                     // ball x when it meets the CPU
    };
    applyOpts();

    // ── state ──────────────────────────────────────────────────────────────────
    enum { ST_MENU, ST_OPTS, ST_SERVE, ST_PLAY, ST_OVER } st = ST_MENU;
    enum { MODE_SQUASH, MODE_PONG } mode = MODE_SQUASH;
    int  diff = 0;                                  // index into k_cpu
    int  sel  = s_menu_sel;                         // mode-menu row
    int  osel = 0;                                  // options row
    bool odirty = false;                            // options changed → save on exit

    int  py = (iy0 + iy1 - ph) / 2;                 // player paddle top
    int  cy = py;                                   // CPU paddle top
    int  bx = 0, by = 0;                            // ball top-left, 8.8 fixed
    int  dx = 0, dy = 0;                            // 8.8 px/tick (dx pre-scale, ×sc on apply)
    int  speed = 560;                               // ball |dx|
    int  score = 0, balls = 5;                      // squash
    int  score_p = 0, score_c = 0;                  // pong, first to 11
    int  serve_dx = 1;                              // pong serve direction (+1 = at player)
    int  cpu_err = 0;                               // this rally's aim error
    bool paused = false;
    uint32_t tick = 0;

    int old_bx = -1, old_by = -1, old_py = -1, old_cy = -1;   // last drawn positions
    // "Is there a ball on the screen?" is its OWN flag, never `old_bx >= 0`:
    // the ball is drawn (clipped) while it leaves the court past the CPU, so a
    // perfectly live ball has a NEGATIVE x for its last few frames. Reading the
    // sentinel out of the coordinate skipped exactly those erases and left a
    // staircase of clipped slivers glued to the left screen edge after every
    // goal against the CPU. Nothing similar happens on the right — there the
    // ball exits at large x, which the sentinel reads as "drawn".
    bool ball_on = false;

    auto tune = [&]() -> const CpuSkill& {
        return mode == MODE_PONG ? k_cpu[diff] : k_solo;
    };
    auto rnd = [](int lo, int hi) {                 // inclusive
        return lo + (int)(get_rand_32() % (uint32_t)(hi - lo + 1));
    };

    // ── drawing ────────────────────────────────────────────────────────────────
    auto drawHud = [&]() {
        fill(m, 2, Sf.w - 2 * m, UI_FONT_H, C_PANEL_ALT);
        text(m + 2 * sc, 2, "PICO-SCWONG", C_WHITE);
        char s[40];
        if (mode == MODE_SQUASH) {
            snprintf(s, sizeof(s), "SCORE %03d  BEST %03d", score, s_best);
            // the remaining balls, as little squares after the title
            int x = m + 2 * sc + textWidth("PICO-SCWONG") + 4 * sc;
            for (int i = 0; i < 5; i++, x += 6 * sc)
                fill(x, 4, 4 * sc, 4, i < balls ? colPad : C_PANEL_ALT);
        } else {
            snprintf(s, sizeof(s), "%s   CPU %02d : %02d YOU",
                     k_cpu_name[diff], score_c, score_p);
        }
        text(Sf.w - m - textWidth(s) - 2 * sc, 2, s, C_TEXT);
    };

    auto centerLineDash = [&](int y0, int y1) {     // dashes clipped to [y0,y1)
        const int lx = (Sf.w - sc) / 2;
        for (int y = iy0 + 2; y < iy1 - 2; y += 8) {
            const int t = y > y0 ? y : y0;
            const int b = (y + 4 < y1) ? y + 4 : y1;
            if (t < b) fill(lx, t, sc, b - t, C_TEXT_DIM);
        }
    };

    auto drawCourt = [&]() {
        fill(0, 0, Sf.w, Sf.h, colField);
        drawHud();
        fill(ox0, oy0, ox1 - ox0 + 1, wt, C_TEXT);                    // top wall
        fill(ox0, oy1 - wt + 1, ox1 - ox0 + 1, wt, C_TEXT);           // bottom wall
        if (mode == MODE_SQUASH) {
            fill(ox0, oy0, wtx, oy1 - oy0 + 1, C_TEXT);               // left wall
        } else {
            centerLineDash(iy0, iy1);                                 // pong centre line
        }
        text(m, Sf.h - foot_h + 2,
             "Q/A " SYM_UP SYM_DOWN " Move  Space Serve  P Pause  Esc Exit",
             C_TEXT_DIM);
    };

    auto drawPaddle = [&]() {
        if (old_py >= 0) fill(paddle_x, old_py, pw, ph, colField);
        fill(paddle_x, py, pw, ph, colPad);
        old_py = py;
    };
    auto drawCpuPaddle = [&]() {
        if (old_cy >= 0) fill(cpu_x, old_cy, pw, ph, colField);
        fill(cpu_x, cy, pw, ph, colPad);
        old_cy = cy;
    };

    // Repaint the pong centre-line dashes inside a just-erased rect — the ball
    // (and the serve-hint panel) would otherwise eat holes into the line.
    auto repaintCenterLine = [&](int x, int y, int w, int h) {
        if (mode != MODE_PONG) return;
        const int lx = (Sf.w - sc) / 2;
        if (x > lx + sc - 1 || x + w - 1 < lx) return;
        centerLineDash(y, y + h);
    };

    auto eraseBall = [&]() {
        if (ball_on) {
            fill(old_bx, old_by, bw, bh, colField);
            repaintCenterLine(old_bx, old_by, bw, bh);
        }
        old_bx = -1;
        ball_on = false;
    };
    auto drawBall = [&]() {
        eraseBall();
        fill(bx >> 8, by >> 8, bw, bh, colBall);
        old_bx = bx >> 8; old_by = by >> 8;
        ball_on = true;
    };

    // Centered one-liner in the court, on its own small panel (same chrome as
    // the game-over box) so the pong centre line never shows through the text.
    auto centerMsg = [&](const char* msg, UiColor ink) {
        const int w = textWidth(msg) + 8 * sc;
        roundRect((Sf.w - w) / 2, box_y, w, box_h, 2, C_SEP, C_PANEL);
        text((Sf.w - textWidth(msg)) / 2, msg_y, msg, ink);
    };
    auto eraseCenterMsg = [&]() {
        fill(ox0, box_y, ox1 - ox0 + 1, box_h, colField);
        // The band spans the whole court width — repaint whatever it ate.
        // The squash left wall is one of them, and it is the one that shows
        // WITHOUT the player doing anything: the serve hint blinks (erase every
        // 32 ticks) from the moment a solo game starts, so the wall came up
        // with a hole in it straight away.
        if (mode == MODE_SQUASH) {
            const int t = box_y > oy0 ? box_y : oy0;
            const int b = (box_y + box_h < oy1 + 1) ? box_y + box_h : oy1 + 1;
            if (t < b) fill(ox0, t, wtx, b - t, C_TEXT);
        }
        if (ball_on && old_by + bh > box_y && old_by < box_y + box_h)
            fill(old_bx, old_by, bw, bh, colBall);
        if (old_py >= 0 && old_py + ph > box_y && old_py < box_y + box_h)
            fill(paddle_x, old_py, pw, ph, colPad);
        if (mode == MODE_PONG && old_cy >= 0 &&
            old_cy + ph > box_y && old_cy < box_y + box_h)
            fill(cpu_x, old_cy, pw, ph, colPad);
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
    static const char* const k_rows[5] = {
        "Solo squash", "vs CPU - Easy", "vs CPU - Normal", "vs CPU - Hard",
        "Options",
    };
    auto drawModeMenu = [&]() {
        fill(0, 0, Sf.w, Sf.h, C_BG);
        const char* t = "P I C O - S C W O N G";
        text((Sf.w - textWidth(t)) / 2, Sf.h / 6, t, C_WHITE);
        hline((Sf.w - textWidth(t)) / 2, Sf.h / 6 + UI_FONT_H + 1, textWidth(t), C_SEP);
        if (s_best) {
            char b[24];
            snprintf(b, sizeof(b), "solo best %d", s_best);
            text((Sf.w - textWidth(b)) / 2, Sf.h / 6 + UI_FONT_H + 5, b, C_TEXT_DIM);
        }
        const int lh = UI_FONT_H + 6;
        int rw = 0;
        for (int i = 0; i < 5; i++)
            if (textWidth(k_rows[i]) > rw) rw = textWidth(k_rows[i]);
        rw += 12 * sc;
        const int rx = (Sf.w - rw) / 2;
        const int ry = Sf.h / 6 + 3 * UI_FONT_H;
        for (int i = 0; i < 5; i++) {
            const int y = ry + i * lh;
            fill(rx, y, rw, lh - 2, i == sel ? C_SEL_BG : C_BG);
            text(rx + 6 * sc, y + 2, k_rows[i], i == sel ? C_WHITE : C_TEXT);
        }
        text(m, Sf.h - foot_h + 2,
             SYM_UP SYM_DOWN " Select  Enter Start  Esc Exit", C_TEXT_DIM);
    };

    // ── the options page ───────────────────────────────────────────────────────
    static const char* const k_optRows[] = {
        "Field colour", "Paddle colour", "Paddle width", "Paddle size",
        "Ball colour", "Ball size", "Paddle speed",
    };
    const int opt_n = GM_N(k_optRows);
    auto optValue = [&](int i) -> const char* {
        switch (i) {
            case 0:  return k_fieldName[Config::gm_field % GM_N(k_fieldCol)];
            case 1:  return k_padName[Config::gm_pad % GM_N(k_padCol)];
            case 2:  return k_padWName[Config::gm_padw % 3];
            case 3:  return k_padHName[Config::gm_padh % 3];
            case 4:  return k_ballColName[Config::gm_ballc % GM_N(k_ballCol)];
            case 5:  return k_ballName[Config::gm_ball % 3];
            default: return k_pSpdName[Config::gm_pspd % 3];
        }
    };
    auto optCycle = [&](int i, int dir) {
        auto step = [&](uint8_t& v, int n) { v = (uint8_t)(((int)v % n + n + dir) % n); };
        switch (i) {
            case 0:  step(Config::gm_field, GM_N(k_fieldCol)); break;
            case 1:  step(Config::gm_pad,   GM_N(k_padCol));   break;
            case 2:  step(Config::gm_padw,  3);                break;
            case 3:  step(Config::gm_padh,  3);                break;
            case 4:  step(Config::gm_ballc, GM_N(k_ballCol));  break;
            case 5:  step(Config::gm_ball,  3);                break;
            default: step(Config::gm_pspd,  3);                break;
        }
        odirty = true;
        applyOpts();
    };
    auto drawOptions = [&]() {
        fill(0, 0, Sf.w, Sf.h, C_BG);
        // The page is laid out from its own height and centred between the top
        // of the screen and the footer, NOT from fixed Sf.h/6 offsets: seven
        // rows plus a preview strip tall enough for the longest paddle do not
        // fit under a hardcoded top margin at 240 lines, and the strip (drawn
        // last) is what silently fell off. Adding a row now moves the block up
        // instead of pushing the preview into the footer.
        const int lh = UI_FONT_H + 6;
        const int sh = k_padH[GM_N(k_padH) - 1] + 6;     // strip = longest paddle
        const int blk = UI_FONT_H + 6 + opt_n * lh + 6 + sh + 2;
        const int foot_top = Sf.h - foot_h;
        int top = (foot_top - blk) / 2;
        if (top < 2) top = 2;
        const char* t = "OPTIONS";
        text((Sf.w - textWidth(t)) / 2, top, t, C_WHITE);
        hline((Sf.w - textWidth(t)) / 2, top + UI_FONT_H + 1, textWidth(t), C_SEP);
        const int rw = 30 * glyphW();               // 30 glyph cells wide
        const int rx = (Sf.w - rw) / 2;
        const int ry = top + UI_FONT_H + 6;
        for (int i = 0; i < opt_n; i++) {
            const int y = ry + i * lh;
            fill(rx, y, rw, lh - 2, i == osel ? C_SEL_BG : C_BG);
            text(rx + 6 * sc, y + 2, k_optRows[i], i == osel ? C_WHITE : C_TEXT);
            const char* v = optValue(i);
            text(rx + rw - textWidth(v) - 6 * sc, y + 2, v,
                 i == osel ? C_WHITE : C_TEXT_DIM);
        }
        // Live preview under the list: a strip of field colour carrying a paddle
        // and the ball at their configured sizes and colours. Its height is the
        // LONGEST paddle's, so the box does not jump about while Paddle size is
        // cycled, and the layout above reserved exactly this much for it.
        const int pbh = k_padH[Config::gm_padh % 3];
        const int sy = ry + opt_n * lh + 6;
        const int sw2 = 22 * glyphW(), sx = (Sf.w - sw2) / 2;
        const int pbw = k_ballSz[Config::gm_ball % 3];
        fill(sx, sy, sw2, sh, k_fieldCol[Config::gm_field % GM_N(k_fieldCol)]);
        frame(sx - 1, sy - 1, sw2 + 2, sh + 2, C_SEP);
        fill(sx + 4 * sc, sy + (sh - pbh) / 2, k_padW[Config::gm_padw % 3] * sc, pbh,
             k_padCol[Config::gm_pad % GM_N(k_padCol)]);
        fill(sx + sw2 / 2 - (pbw * sc) / 2, sy + (sh - pbw) / 2,
             pbw * sc, pbw, k_ballCol[Config::gm_ballc % GM_N(k_ballCol)]);
        text(m, Sf.h - foot_h + 2,
             SYM_LEFT SYM_RIGHT " Change  " SYM_UP SYM_DOWN " Select  Esc Back",
             C_TEXT_DIM);
    };

    auto leaveOptions = [&]() {
        if (odirty) { Config::save(); odirty = false; }   // no SD → kept for the session
        st = ST_MENU;
        drawModeMenu();
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
        cpu_err = rnd(-(int)tune().err, (int)tune().err);
        st = ST_PLAY;
    };

    auto newGame = [&]() {
        applyOpts();
        score = 0; balls = 5; score_p = 0; score_c = 0;
        speed = tune().serve; paused = false; serve_dx = rnd(0, 1) ? 1 : -1;
        py = (iy0 + iy1 - ph) / 2; cy = py;
        old_bx = -1; old_py = -1; old_cy = -1; ball_on = false;
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
        speed = tune().serve;                              // each rally starts calm
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
        // queued for every arrow / Enter / Space) act once — see scwongAct().
        fabgl::VirtualKeyItem k;
        ScwongAct prev_act = SA_NONE;
        while (kbd->virtualKeyAvailable()) {
            if (!ESPectrum::readKbd(&k) || !k.down) continue;
            const ScwongAct act = scwongAct(k.vk);
            if (act == SA_NONE || act == prev_act) continue;
            prev_act = act;
            switch (act) {
                case SA_BACK:
                    if (st == ST_MENU) return;
                    if (st == ST_OPTS) { leaveOptions(); break; }
                    s_menu_sel = (uint8_t)sel;
                    st = ST_MENU; paused = false;
                    drawModeMenu();
                    break;
                case SA_UP:
                    if (st == ST_MENU && sel > 0) { sel--; drawModeMenu(); }
                    else if (st == ST_OPTS && osel > 0) { osel--; drawOptions(); }
                    break;
                case SA_DOWN:
                    if (st == ST_MENU && sel < 4) { sel++; drawModeMenu(); }
                    else if (st == ST_OPTS && osel < opt_n - 1) { osel++; drawOptions(); }
                    break;
                case SA_LEFT:
                    if (st == ST_OPTS) { optCycle(osel, -1); drawOptions(); }
                    break;
                case SA_RIGHT:
                    if (st == ST_OPTS) { optCycle(osel, +1); drawOptions(); }
                    break;
                case SA_FIRE:
                    if (st == ST_MENU) {
                        s_menu_sel = (uint8_t)sel;
                        if (sel == 4) { st = ST_OPTS; osel = 0; drawOptions(); break; }
                        mode = sel == 0 ? MODE_SQUASH : MODE_PONG;
                        diff = sel == 0 ? 0 : sel - 1;
                        newGame();
                    }
                    else if (st == ST_OPTS) { optCycle(osel, +1); drawOptions(); }
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
            if (up && !dn) { py -= pv_player; if (py < iy0) py = iy0; }
            if (dn && !up) { py += pv_player; if (py > iy1 - ph + 1) py = iy1 - ph + 1; }
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
                        if (speed < (int)tune().cap) speed += speed / tune().accel;
                        dx = -speed;
                        dy = (bc - (py + ph / 2)) * 52;    // english: edge ≈ ±3 px/t
                        if (dy > -64 && dy < 64) dy += (dy < 0 ? -64 : 64);
                        cpu_err = rnd(-(int)tune().err, (int)tune().err);
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
                        if (speed < (int)tune().cap) speed += speed / tune().accel;
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

// Boot-time entrance: held S in the factory-reset probe window (ESPectrum::setup)
// runs the game before the emulation loop starts — no SD card required. Owns the
// whole gfx session (the menu's runModal is not around us here) and hands the
// screen back to the emulator on exit.
void gameScwongStandalone() {
    gfxBegin();
    act_gameScwong();
    gfxEnd();
    VIDEO::brdnextframe = true;      // the paper repaints every frame; the border won't
    auto kbd = ESPectrum::PS2Controller.keyboard();
    fabgl::VirtualKeyItem d;
    while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d);
}

} // namespace nm
