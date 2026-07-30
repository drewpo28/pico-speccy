// pico-speccy — the joystick keyboard-mapping page of the new UI.
//
// The classic joyDialog's value is its SPATIAL layout: you see where a pad
// button physically is and what ZX key it fires, and JoyTest lights the real
// button up as you press it. That is preserved here 1:1 — same 14 cells, same
// neighbour table (each cell names its left/right/up/down neighbour, so the
// cursor walks the pad's plan, not a list), same JoyTest. What changed is the
// rendering: UiGfx primitives and the UI palette instead of zxColor()/vga.rect,
// with the menu's own header and footer around it.
//
// The key picker reuses the classic tables via OSD::joyPickKey(), so the option
// -> VirtualKey mapping lives in exactly one place.

#include "OSDNewMenu.h"


#include <string.h>
#include <stdio.h>

#include "UiJoy.h"
#include "UiGfx.h"
#include "UiFont.h"
#include "UiRender.h"       // SYM_* glyphs
#include "UiDialog.h"
#include "UiStrings.h"
#include "OSDMain.h"
#include "Config.h"
#include "ESPectrum.h"
#include "fabutils.h"
#include <pico/stdlib.h>

namespace nm {

// ── the pad's plan ─────────────────────────────────────────────────────────────
// Cell geometry is in "design pixels" of a 320x240 surface and scaled to the live
// one at draw time. Neighbours are the classic joyDialog's table (its rows 0..13
// plus the two buttons), which is what makes the cursor move like a pad and not
// like a list.
struct Cell {
    int16_t x, y;             // top-left of the value pill, design pixels
    int8_t  nl, nr, nu, nd;   // neighbour indices, -1 = edge
    const char* cap;          // caption, drawn above the pill
    int8_t  w;                // pill width in design pixels (0 = default)
};

#define J_LEFT 0
#define J_RIGHT 1
#define J_UP 2
#define J_DOWN 3
#define J_START 4
#define J_MODE 5
#define J_A 6
#define J_B 7
#define J_C 8
#define J_X 9
#define J_Y 10
#define J_Z 11
#define J_L2 12
#define J_R2 13
#define J_OK 14
#define J_TEST 15
#define J_CELLS 16

static const Cell kCells[J_CELLS] = {
    // The caption sits ABOVE its pill (the classic put it to the right, which
    // collided once the pills grew), so a cell owns a caption row + a pill row.
    // The D-pad column is wider: its values are the longest labels ("Joy.Right",
    // "Joy.Select"), and they must not be clipped. The two button columns are
    // narrower ("Joy.A", "None") and pushed right to make room.
    //  x    y   nl      nr       nu       nd       caption   w
    {   4,  56, -1,      J_RIGHT, J_UP,    J_DOWN,  "Left",   66 },
    {  76,  56, J_LEFT,  J_X,     J_UP,    J_DOWN,  "Right",  66 },
    {  40,  26, -1,      J_A,     -1,      J_LEFT,  "Up",     66 },
    {  40,  86, -1,      J_C,     J_LEFT,  J_START, "Down",   66 },
    {   4, 116, -1,      J_MODE,  J_DOWN,  -1,      "Start",  66 },
    {  76, 116, J_START, J_OK,    J_RIGHT, -1,      "Select", 66 },
    { 168,  26, J_UP,    J_B,     -1,      J_X,     "A",      54 },
    { 250,  26, J_A,     -1,      -1,      J_Y,     "B",      54 },
    { 168,  86, J_DOWN,  J_Z,     J_X,     J_L2,    "C",      54 },
    { 168,  56, J_RIGHT, J_Y,     J_A,     J_C,     "X",      54 },
    { 250,  56, J_X,     -1,      J_B,     J_Z,     "Y",      54 },
    { 250,  86, J_C,     -1,      J_Y,     J_R2,    "Z",      54 },
    { 168, 116, J_DOWN,  J_R2,    J_C,     J_OK,    "L2",     54 },
    { 250, 116, J_L2,    -1,      J_Z,     J_TEST,  "R2",     54 },
    {  16, 146, -1,      J_TEST,  J_START, -1,      nullptr,  0  },  // Save
    { 168, 146, J_OK,    -1,      J_L2,    -1,      nullptr,  0  },  // JoyTest
};

// Pill width of a cell, in live pixels.
static inline int cellW(int i) { return (kCells[i].w ? kCells[i].w : 56) * Sf.glyphScale; }

// JoyTest: the VK a physical pad control reports -> our cell.
static int cellForJoyVk(int vk) {
    switch (vk) {
        case fabgl::VK_JOY_LEFT:  return J_LEFT;
        case fabgl::VK_JOY_RIGHT: return J_RIGHT;
        case fabgl::VK_JOY_UP:    return J_UP;
        case fabgl::VK_JOY_DOWN:  return J_DOWN;
        case fabgl::VK_JOY_START: return J_START;
        case fabgl::VK_JOY_MODE:  return J_MODE;
        case fabgl::VK_JOY_A:     return J_A;
        case fabgl::VK_JOY_B:     return J_B;
        case fabgl::VK_JOY_C:     return J_C;
        case fabgl::VK_JOY_X:     return J_X;
        case fabgl::VK_JOY_Y:     return J_Y;
        case fabgl::VK_JOY_Z:     return J_Z;
        case fabgl::VK_JOY_L2:    return J_L2;
        case fabgl::VK_JOY_R2:    return J_R2;
        default:                  return -1;
    }
}

// ── layout ─────────────────────────────────────────────────────────────────────

struct JoyLayout {
    int ix, iy, iw, ih, margin, hdr_h, foot_h, pad;
    int ox, oy;                 // origin of the design grid inside the body
    int sx;                     // horizontal scale (DS80 doubles it)
    int pill_w, pill_h;
};
static JoyLayout JL;

static void computeJoyLayout() {
    const int sc = Sf.glyphScale;
    JL.pad = 2 * sc;
    JL.margin = 2 * sc;
    JL.ix = JL.margin + sc;
    JL.iy = 3;
    JL.iw = Sf.w - 2 * JL.ix;
    JL.ih = Sf.h - JL.iy - 3;
    JL.hdr_h = UI_FONT_H + 6;
    JL.foot_h = UI_FONT_H + 4;
    JL.sx = sc;                             // design grid is 320 px wide
    JL.pill_w = 56 * sc;
    JL.pill_h = 11;
    // Centre the 312x168 design area in the body.
    const int body_y = JL.iy + JL.hdr_h;
    const int body_h = JL.iy + JL.ih - JL.foot_h - body_y;
    JL.ox = (Sf.w - 310 * sc) / 2;
    if (JL.ox < JL.ix) JL.ox = JL.ix;
    JL.oy = body_y + (body_h - 168) / 2 - 8;
    if (JL.oy < body_y) JL.oy = body_y;
}

static inline int cx(int i) { return JL.ox + kCells[i].x * JL.sx; }
static inline int cy(int i) { return JL.oy + kCells[i].y; }

// ── drawing ────────────────────────────────────────────────────────────────────

static int  s_sel;                  // focused cell
static bool s_test;                 // JoyTest mode
static int  s_vk[14];               // working copy of Config::joydef
static bool s_lit[14];              // JoyTest: control is pressed right now

static void drawCell(int i) {
    const int x = cx(i), y = cy(i);
    const bool sel = (i == s_sel);

    if (i >= J_OK) {                                  // the two buttons
        const char* txt = (i == J_OK) ? "Save" : "JoyTest";
        const int w = ((int)strlen(txt) + 2) * glyphW();
        fill(x, y - 1, w, JL.pill_h, sel ? C_SEL_BG : C_PANEL_ALT);
        text(x + glyphW(), y, txt, sel ? C_WHITE : C_TEXT);
        return;
    }

    // The pad control's name goes ABOVE its pill (lit in JoyTest), the assigned
    // ZX key inside it. Both are clipped to the cell's own 56-px column, so
    // neighbours can never overlap.
    const UiColor capInk = s_test ? (s_lit[i] ? C_ACCENT : C_TEXT_DIM)
                                  : (sel ? C_WHITE : C_TEXT_DIM);
    if (kCells[i].cap) {
        fill(x, y - UI_FONT_H - 2, cellW(i), UI_FONT_H + 1, C_PANEL);
        textClip(x, y - UI_FONT_H - 1, cellW(i), kCells[i].cap, capInk);
    }

    string kn = OSD::vkToText(s_vk[i]);
    while (!kn.empty() && kn.back() == ' ') kn.pop_back();

    fill(x, y - 1, cellW(i), JL.pill_h, sel && !s_test ? C_SEL_BG : C_PANEL_ALT);
    frame(x, y - 1, cellW(i), JL.pill_h, s_test && s_lit[i] ? C_ACCENT : C_SEP);
    textClip(x + 2 * Sf.glyphScale, y, cellW(i) - 4 * Sf.glyphScale, kn.c_str(),
             sel && !s_test ? C_WHITE : C_TEXT);
}

static void drawAllCells() {
    for (int i = 0; i < J_CELLS; i++) drawCell(i);
}

static void drawFooterJoy() {
    const int fy = JL.iy + JL.ih - JL.foot_h;
    fill(JL.ix, fy, JL.iw, JL.foot_h, C_FOOT_BG);
    hline(JL.ix, fy, JL.iw, C_SEP);
    text(JL.ix + JL.pad, fy + 3,
         s_test ? "Press pad buttons   Esc Leave test"
                : SYM_UP SYM_DOWN SYM_LEFT SYM_RIGHT " Move  " SYM_ENTER " Assign  Del Clear  Esc Close",
         C_TEXT_DIM);
    roundRectBorder(JL.margin, JL.iy - 1, Sf.w - 2 * JL.margin, JL.ih + 2, 4, C_SEP, C_BG);
}

static void drawChromeJoy() {
    fill(0, 0, Sf.w, Sf.h, C_BG);
    roundRect(JL.margin, JL.iy - 1, Sf.w - 2 * JL.margin, JL.ih + 2, 4, C_SEP, C_PANEL);
    fill(JL.ix, JL.iy, JL.iw, JL.hdr_h, C_PANEL);
    rainbow(JL.ix + JL.pad, JL.iy + 3);
    text(JL.ix + JL.pad + rainbowW() + 2 * JL.pad, JL.iy + 4, TXT_JOY_MAPPING, C_WHITE);
    const char* jt = s_test ? "JoyTest" : Config::joystick == JOY_FULLER ? "Fuller" : "Kempston";
    text(JL.ix + JL.iw - textWidth(jt) - JL.pad, JL.iy + 4, jt,
         s_test ? C_ACCENT : C_TEXT_DIM);
    hline(JL.ix, JL.iy + JL.hdr_h - 1, JL.iw, C_SEP);
    drawAllCells();
    drawFooterJoy();
}

// ── the page ───────────────────────────────────────────────────────────────────

void joyMappingPage() {
    gfxBegin();
    computeJoyLayout();

    for (int i = 0; i < 14; i++) s_vk[i] = Config::joydef[i];
    memset(s_lit, 0, sizeof(s_lit));
    s_sel = J_UP;
    s_test = false;

    // The pad's own VKs must not steer the cursor while we are mapping it.
    const bool savedCursorAsJoy = Config::CursorAsJoy;
    Config::CursorAsJoy = false;

    drawChromeJoy();

    auto kbd = ESPectrum::PS2Controller.keyboard();
    { fabgl::VirtualKeyItem d; while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d); }

    int testExit = 0;
    fabgl::VirtualKeyItem k;
    while (1) {
        // ── JoyTest: light the pressed controls, B held for ~5 ticks leaves ────
        if (s_test) {
            bool dirty = false;
            for (int vk = fabgl::VK_JOY_RIGHT; vk <= fabgl::VK_JOY_R2; vk++) {
                const int c = cellForJoyVk(vk);
                if (c < 0) continue;
                const bool down = kbd->isVKDown((fabgl::VirtualKey)vk);
                if (down != s_lit[c]) { s_lit[c] = down; dirty = true; }
            }
            if (dirty) drawAllCells();
            if (kbd->isVKDown(fabgl::VK_JOY_B)) {
                if (++testExit == 5) {
                    s_test = false;
                    memset(s_lit, 0, sizeof(s_lit));
                    drawChromeJoy();
                    testExit = 0;
                }
            } else testExit = 0;
        }

        if (!kbd->virtualKeyAvailable()) { sleep_ms(s_test ? 50 : 5); continue; }
        if (!ESPectrum::readKbd(&k) || !k.down) continue;

        if (k.vk == fabgl::VK_ESCAPE || k.vk == fabgl::VK_F1) {
            if (s_test) {                       // leave the test, stay on the page
                s_test = false;
                memset(s_lit, 0, sizeof(s_lit));
                drawChromeJoy();
                OSD::clickNoPause();
                continue;
            }
            bool changed = false;
            for (int i = 0; i < 14; i++)
                if (Config::joydef[i] != s_vk[i]) { changed = true; break; }
            if (changed && uiConfirm("Save the joystick mapping?")) {
                for (int i = 0; i < 14; i++) Config::joydef[i] = s_vk[i];
                Config::save();
            }
            break;
        }
        if (s_test) continue;                   // in test mode only Esc/pad matter

        int ns = s_sel;
        switch (k.vk) {
            case fabgl::VK_MENU_LEFT:  ns = kCells[s_sel].nl; break;
            case fabgl::VK_MENU_RIGHT: ns = kCells[s_sel].nr; break;
            case fabgl::VK_MENU_UP:    ns = kCells[s_sel].nu; break;
            case fabgl::VK_MENU_DOWN:  ns = kCells[s_sel].nd; break;

            case fabgl::VK_DELETE:
                if (s_sel < 14) {
                    s_vk[s_sel] = fabgl::VK_NONE;
                    drawCell(s_sel);
                    OSD::clickNoPause();
                }
                continue;

            case fabgl::VK_MENU_ENTER:
                if (s_sel == J_OK) {            // Save
                    for (int i = 0; i < 14; i++) Config::joydef[i] = s_vk[i];
                    Config::save();
                    uiToast("Mapping saved", false, 1000);
                    drawChromeJoy();
                    continue;
                }
                if (s_sel == J_TEST) {          // enter JoyTest
                    // The test drives the live mapping, so commit first — same as
                    // the classic dialog did.
                    for (int i = 0; i < 14; i++) Config::joydef[i] = s_vk[i];
                    Config::save();
                    s_test = true;
                    testExit = 0;
                    drawChromeJoy();
                    continue;
                }
                {                               // assign a key to this control
                    const int nv = OSD::joyPickKey(s_vk[s_sel]);
                    if (nv >= 0) s_vk[s_sel] = nv;
                    drawChromeJoy();            // the picker drew over us
                }
                continue;

            default: continue;
        }
        if (ns >= 0 && ns != s_sel) {
            const int old = s_sel;
            s_sel = ns;
            drawCell(old);
            drawCell(s_sel);
            OSD::clickNoPause();
        }
    }

    Config::CursorAsJoy = savedCursorAsJoy;
    gfxEnd();
}

} // namespace nm

