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
#include "app/messages.h"      // OSD_DLG_SETJOYMAPDEFAULTS
#include "OSDMain.h"
#include "app/Config.h"
#include "app/ESPectrum.h"
#include "app/JoyProfiles.h"
#include "app/TryAlloc.h"
#include "fs/FileUtils.h"
#include "drivers/input/fabutils.h"
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
#define J_DEF 16
#define J_TYPE 17
#define J_CELLS 18

static const Cell kCells[J_CELLS] = {
    // The caption sits ABOVE its pill (the classic put it to the right, which
    // collided once the pills grew), so a cell owns a caption row + a pill row.
    // The D-pad column is wider: its values are the longest labels ("Joy.Right",
    // "Joy.Select"), and they must not be clipped. The two button columns are
    // narrower ("Joy.A", "None") and pushed right to make room.
    //  x    y   nl      nr       nu       nd       caption   w
    {   4,  82, -1,      J_RIGHT, J_UP,    J_DOWN,  "Left",   66 },
    {  76,  82, J_LEFT,  J_X,     J_UP,    J_DOWN,  "Right",  66 },
    {  40,  52, -1,      J_A,     J_TYPE,  J_LEFT,  "Up",     66 },
    {  40, 112, -1,      J_C,     J_LEFT,  J_START, "Down",   66 },
    {   4, 142, -1,      J_MODE,  J_DOWN,  J_OK,    "Start",  66 },
    {  76, 142, J_START, J_L2,    J_RIGHT, J_DEF,   "Select", 66 },
    { 168,  52, J_UP,    J_B,     J_TYPE,  J_X,     "A",      54 },
    { 250,  52, J_A,     -1,      J_TYPE,  J_Y,     "B",      54 },
    { 168, 112, J_DOWN,  J_Z,     J_X,     J_L2,    "C",      54 },
    { 168,  82, J_RIGHT, J_Y,     J_A,     J_C,     "X",      54 },
    { 250,  82, J_X,     -1,      J_B,     J_Z,     "Y",      54 },
    { 250, 112, J_C,     -1,      J_Y,     J_R2,    "Z",      54 },
    { 168, 142, J_DOWN,  J_R2,    J_C,     J_TEST,  "L2",     54 },
    { 250, 142, J_L2,    -1,      J_Z,     J_TEST,  "R2",     54 },
    // The button row, left to right: Save, Load defaults, Test joystick (the
    // table order is by index: J_OK, J_TEST, J_DEF).
    {  16, 172, -1,      J_DEF,   J_START, -1,      nullptr,  0  },  // Save
    { 214, 172, J_DEF,   -1,      J_R2,    -1,      nullptr,  0  },  // Test joystick
    {  88, 172, J_OK,    J_TEST,  J_MODE,  -1,      nullptr,  0  },  // Load defaults
    // The joystick type is part of the profile (the same map means other things on
    // other types), so it is the FIRST field of the page, above the pad.
    {   4,  26, -1,      -1,      -1,      J_UP,    "Type",   90 },
};

// Display names of the JOY_* types (index = value).
static const char* const kTypeShow[] = { "Cursor", "Kempston", "Sinclair 1", "Sinclair 2", "Fuller" };
static const char* typeShow(uint8_t t) { return t < 5 ? kTypeShow[t] : "?"; }

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
    // Centre the 312x190 design area in the body.
    const int body_y = JL.iy + JL.hdr_h;
    const int body_h = JL.iy + JL.ih - JL.foot_h - body_y;
    JL.ox = (Sf.w - 310 * sc) / 2;
    if (JL.ox < JL.ix) JL.ox = JL.ix;
    JL.oy = body_y + (body_h - 190) / 2 - 4;
    if (JL.oy < body_y) JL.oy = body_y;
}

static inline int cx(int i) { return JL.ox + kCells[i].x * JL.sx; }
static inline int cy(int i) { return JL.oy + kCells[i].y; }

// ── drawing ────────────────────────────────────────────────────────────────────


// The page edits a WORKING COPY of one profile: its type and its 14 targets. What
// it is a copy of is s_target: a SLOT of the profile file (the working copy's name is
// empty while that slot is still empty — a new profile), or -1 for the live map
// before it was ever given a slot.
static int  s_sel;                  // focused cell
static bool s_test;                 // JoyTest mode
static JoyProf::Profile s_work;     // working copy (name, type, map)
static JoyProf::Profile s_orig;     // what Esc compares against
static int  s_target;               // slot in the file, -1 = none chosen yet
static bool s_lit[14];              // JoyTest: control is pressed right now

static void drawCell(int i) {
    const int x = cx(i), y = cy(i);
    const bool sel = (i == s_sel);

    if (i >= J_OK && i != J_TYPE) {                   // the three buttons
        const char* txt = (i == J_OK) ? "Save" : (i == J_TEST) ? "Test joystick" : "Load defaults";
        const int w = ((int)strlen(txt) + 2) * glyphW();
        fill(x, y - 1, w, JL.pill_h, sel ? C_SEL_BG : C_PANEL_ALT);
        text(x + glyphW(), y, txt, sel ? C_WHITE : C_TEXT);
        return;
    }

    // The pad control's name goes ABOVE its pill (lit in JoyTest), the assigned
    // ZX key inside it. Both are clipped to the cell's own 56-px column, so
    // neighbours can never overlap.
    const UiColor capInk = s_test ? ((i < 14 && s_lit[i]) ? C_ACCENT : C_TEXT_DIM)
                                  : (sel ? C_WHITE : C_TEXT_DIM);
    if (kCells[i].cap) {
        fill(x, y - UI_FONT_H - 2, cellW(i), UI_FONT_H + 1, C_PANEL);
        textClip(x, y - UI_FONT_H - 1, cellW(i), kCells[i].cap, capInk);
    }

    string kn;
    if (i == J_TYPE) {
        kn = typeShow(s_work.type);
    } else {
        kn = OSD::vkToText(s_work.map[i]);
        while (!kn.empty() && kn.back() == ' ') kn.pop_back();
        while (!kn.empty() && kn.front() == ' ') kn.erase(0, 1);
    }

    const bool lit = s_test && i < 14 && s_lit[i];
    fill(x, y - 1, cellW(i), JL.pill_h, sel && !s_test ? C_SEL_BG : C_PANEL_ALT);
    frame(x, y - 1, cellW(i), JL.pill_h, lit ? C_ACCENT : C_SEP);
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
    const int tx = JL.ix + JL.pad + rainbowW() + 2 * JL.pad;
    text(tx, JL.iy + 4, TXT_JOY_MAPPING, C_WHITE);
    // Right of the title: which profile this is (or that it has none yet).
    const char* jt = s_test ? "Test joystick"
                   : s_work.name[0] ? s_work.name : TXT_JOYPROF_UNSAVED;
    const int tw = textWidth(TXT_JOY_MAPPING);
    const int room = JL.ix + JL.iw - JL.pad - (tx + tw + 2 * glyphW());
    const int jw = textWidth(jt) < room ? textWidth(jt) : room;
    if (room > 0)
        textClip(JL.ix + JL.iw - jw - JL.pad, JL.iy + 4, jw, jt,
                 s_test ? C_ACCENT : C_TEXT_DIM);
    hline(JL.ix, JL.iy + JL.hdr_h - 1, JL.iw, C_SEP);
    drawAllCells();
    drawFooterJoy();
}

// ── profile storage ────────────────────────────────────────────────────────────
// The file is small (16 slots), so every change reads it, edits it and writes it
// back whole. The list lives on the heap only for the length of the operation.
// It always holds all JoyProf::MAX slots; an empty one has name "".

struct JoyList {
    JoyProf::Profile p[JoyProf::MAX];
    int n;                              // always JoyProf::MAX
};

static JoyList* listLoad() {
    JoyList* l = (JoyList*)tryMalloc(sizeof(JoyList));
    if (!l) return nullptr;
    Config::joyProfilesLoad(l->p, JoyProf::MAX);
    l->n = JoyProf::MAX;
    return l;
}

static bool sameMap(const JoyProf::Profile& a, const JoyProf::Profile& b) {
    return a.type == b.type && !memcmp(a.map, b.map, sizeof(a.map));
}

// Make `p` the live pad and remember its name; persisted in storage.nvs.
static void applyLive(const JoyProf::Profile& p, bool named) {
    Config::joystick = p.type;
    for (int i = 0; i < 14; i++) Config::joydef[i] = p.map[i];
    Config::joy_profile = named ? p.name : "";
    Config::save();
}

static void liveToProfile(JoyProf::Profile& p) {
    memset(&p, 0, sizeof(p));
    p.type = Config::joystick < 5 ? Config::joystick : JOY_KEMPSTON;
    for (int i = 0; i < 14; i++) p.map[i] = Config::joydef[i];
}

static void invalidateRows();     // the pick list's session cache (below)

// Save the working copy into its slot. A profile that already exists is overwritten
// in place and nothing is asked; a new one (an empty slot) asks for its name first.
// The saved profile becomes the live one — editing a map is what you do to the pad
// you are about to use.
static bool saveWork() {
    if (!FileUtils::fsMount) {            // no card: no library, the live pad only
        applyLive(s_work, false);
        s_orig = s_work;
        return true;
    }
    JoyList* l = listLoad();
    if (!l) { uiToast(TXT_JOYPROF_NOMEM, true, 1500); return false; }

    int idx = s_target;
    if (s_work.name[0]) {
        // An existing profile. If the file moved under us (edited on a PC), find it
        // again by name; gone altogether = it goes back into the slot it came from.
        if (idx < 0 || strcmp(l->p[idx].name, s_work.name)) {
            const int f = JoyProf::find(l->p, l->n, s_work.name);
            if (f >= 0) idx = f;
            else if (idx >= 0 && l->p[idx].name[0]) idx = -1;   // its slot went to another profile
        }
    }
    if (idx < 0) {                        // no slot chosen (Mapping on an unsaved pad): the first free one
        for (int i = 0; i < l->n && idx < 0; i++) if (!l->p[i].name[0]) idx = i;
        if (idx < 0) { uiToast(TXT_JOYPROF_FULL, true, 2000); free(l); return false; }
    }
    if (!s_work.name[0]) {
        string nm_;
        for (;;) {
            if (!uiPrompt(TXT_JOYPROF_NAME, nm_, JoyProf::NAME_LEN - 1)) { free(l); return false; }
            JoyProf::Profile t = s_work;
            if (!JoyProf::setName(t, nm_.c_str())) continue;
            const int dup = JoyProf::find(l->p, l->n, t.name);
            if (dup >= 0 && dup != idx) { uiToast(TXT_JOYPROF_DUP, true, 1500); continue; }
            memcpy(s_work.name, t.name, sizeof(s_work.name));
            break;
        }
    }
    l->p[idx] = s_work;
    uiBusy(TXT_MSG_SAVING_SNAP);
    const bool ok = Config::joyProfilesSave(l->p, l->n);
    free(l);
    if (!ok) { uiToast(TXT_JOYPROF_SAVE_ERR, true, 2000); return false; }
    s_target = idx;
    s_orig = s_work;
    applyLive(s_work, true);
    invalidateRows();
    return true;
}

// ── the page ───────────────────────────────────────────────────────────────────

static void mappingPage() {
    gfxBegin();
    computeJoyLayout();

    memset(s_lit, 0, sizeof(s_lit));
    s_sel = J_TYPE;
    s_test = false;

    // The pad's own VKs must not steer the cursor while we are mapping it.
    const bool savedCursorAsJoy = Config::CursorAsJoy;
    Config::CursorAsJoy = false;
    // JoyTest drives the LIVE pad; what it was is put back afterwards, so testing a
    // map that is never saved leaves the running one alone.
    uint8_t  liveType = Config::joystick;
    uint16_t liveMap[14];
    memcpy(liveMap, Config::joydef, sizeof(liveMap));

    drawChromeJoy();

    auto kbd = ESPectrum::PS2Controller.keyboard();
    { fabgl::VirtualKeyItem d; while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d); }

    auto leaveTest = [&]() {
        s_test = false;
        memset(s_lit, 0, sizeof(s_lit));
        Config::joystick = liveType;
        memcpy(Config::joydef, liveMap, sizeof(liveMap));
        drawChromeJoy();
    };

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
                if (++testExit == 5) { leaveTest(); testExit = 0; }
            } else testExit = 0;
        }

        if (!kbd->virtualKeyAvailable()) { uiIdle(s_test ? 50 : 5); continue; }
        if (!ESPectrum::readKbd(&k) || !k.down) continue;

        if (k.vk == fabgl::VK_ESCAPE || k.vk == fabgl::VK_F1) {
            if (s_test) {                       // leave the test, stay on the page
                leaveTest();
                OSD::clickNoPause();
                continue;
            }
            if (!sameMap(s_work, s_orig) && uiConfirm("Save the joystick mapping?")) {
                if (!saveWork()) { drawChromeJoy(); continue; }   // name prompt backed out
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
                    s_work.map[s_sel] = fabgl::VK_NONE;
                    drawCell(s_sel);
                    OSD::clickNoPause();
                }
                continue;

            case fabgl::VK_MENU_ENTER:
                if (s_sel == J_OK) {            // Save
                    if (saveWork()) uiToast(TXT_JOYPROF_SAVED, false, 1000);
                    liveType = Config::joystick;
                    memcpy(liveMap, Config::joydef, sizeof(liveMap));
                    drawChromeJoy();
                    continue;
                }
                if (s_sel == J_TEST) {          // enter JoyTest with the working map
                    Config::joystick = s_work.type;
                    memcpy(Config::joydef, s_work.map, sizeof(liveMap));
                    s_test = true;
                    testExit = 0;
                    drawChromeJoy();
                    continue;
                }
                if (s_sel == J_DEF) {           // default map of the chosen type
                    // Into the working copy only: Save (or the question on Esc)
                    // is what makes it stick, so this can still be backed out.
                    Config::joyDefaults(s_work.type, s_work.map);
                    drawAllCells();
                    OSD::clickNoPause();
                    continue;
                }
                if (s_sel == J_TYPE) {          // pick the joystick type
                    const int t = uiPickList(TXT_JOY_TYPE, kTypeShow, 5, s_work.type);
                    if (t >= 0 && t != s_work.type) {
                        s_work.type = (uint8_t)t;
                        // Each type has its own default map. Declining keeps the map:
                        // the pad controls mean the same thing on every type.
                        if (uiConfirm(OSD_DLG_SETJOYMAPDEFAULTS, TXT_JOY_TYPE, /*default_yes=*/true))
                            Config::joyDefaults(s_work.type, s_work.map);
                    }
                    drawChromeJoy();
                    continue;
                }
                {                               // assign a key to this control
                    const int nv = OSD::joyPickKey(s_work.map[s_sel]);
                    if (nv >= 0) s_work.map[s_sel] = (uint16_t)nv;
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

// Joystick > Mapping: the live pad, as the profile it was loaded from or saved to —
// or as a new, still unnamed one (Save then asks for the name).
void joyMappingPage() {
    liveToProfile(s_work);
    s_target = -1;
    if (!Config::joy_profile.empty() && FileUtils::fsMount) {
        JoyList* l = listLoad();
        if (l) {
            const int i = JoyProf::find(l->p, l->n, Config::joy_profile.c_str());
            if (i >= 0) {
                s_target = i;
                memcpy(s_work.name, l->p[i].name, sizeof(s_work.name));
            }
            free(l);
        }
    }
    s_orig = s_work;
    mappingPage();
}

// ── Joystick > Profile: the pick list ─────────────────────────────────────────
// The Config-profiles shape: all JoyProf::MAX numbered slots are always listed,
// "#NN name" or a bare "#NN" for an empty one, value = the slot index. The table is
// cached for the menu session (the renderer asks for it per drawn row) and allocated
// only while the menu is up.

#define JP_LBL  (JoyProf::NAME_LEN + 4)
struct JoyRows {
    JoyList list;
    Option  opts[JoyProf::MAX];
    char    lbl[JoyProf::MAX][JP_LBL];
    bool    valid;
};
static JoyRows* s_rows = nullptr;

static void invalidateRows() { if (s_rows) s_rows->valid = false; }

void joyProfilesSessionBegin() { invalidateRows(); }
void joyProfilesSessionEnd()   { free(s_rows); s_rows = nullptr; }

static JoyRows* jpRows() {
    if (!s_rows) {
        s_rows = (JoyRows*)tryMalloc(sizeof(JoyRows));
        if (!s_rows) return nullptr;
        s_rows->valid = false;
    }
    if (!s_rows->valid) {
        JoyList& l = s_rows->list;
        Config::joyProfilesLoad(l.p, JoyProf::MAX);
        l.n = JoyProf::MAX;
        for (int i = 0; i < l.n; i++) {
            if (l.p[i].name[0]) snprintf(s_rows->lbl[i], JP_LBL, "#%02d %s", i + 1, l.p[i].name);
            else                snprintf(s_rows->lbl[i], JP_LBL, "#%02d", i + 1);
            s_rows->opts[i] = { s_rows->lbl[i], (int32_t)i, nullptr };
        }
        s_rows->valid = true;
    }
    return s_rows;
}

const Option* joyprof_rows(uint8_t& cnt) {
    JoyRows* r = jpRows();
    if (!r) { cnt = 0; return nullptr; }
    cnt = (uint8_t)r->list.n;
    return r->opts;
}

// Which row is the live pad: its profile's slot, -1 when it has none.
int32_t joyprof_current() {
    if (Config::joy_profile.empty()) return -1;
    JoyRows* r = jpRows();
    if (!r) return -1;
    return JoyProf::find(r->list.p, r->list.n, Config::joy_profile.c_str());
}

// The collapsed row's value: the live profile's name, with a '*' while the live pad
// no longer matches it (a type cycled with the hot key, a test left mid-edit).
static char s_vlabel[24];
const char* joyprof_vlabel() {
    const int32_t i = joyprof_current();
    if (i < 0) return TXT_JOYPROF_UNSAVED;
    JoyProf::Profile live;
    liveToProfile(live);
    const JoyProf::Profile& p = s_rows->list.p[i];
    snprintf(s_vlabel, sizeof(s_vlabel), "%.13s%s", p.name, sameMap(live, p) ? "" : "*");
    return s_vlabel;
}

// Enter = use it, F4 = edit, F6 = rename, F8 = remove. On an EMPTY slot Enter (and
// F4) add a profile there.
void joyprof_key(int32_t tag, uint8_t key) {
    JoyRows* r = jpRows();
    if (!r) { uiToast(TXT_JOYPROF_NOMEM, true, 1500); return; }
    if (tag < 0 || tag >= r->list.n) return;

    if (!r->list.p[tag].name[0]) {
        if (key != 0 && key != 3 && key != 4) return;
        // A new profile always starts from the defaults (Kempston + its default
        // map), never from the live pad. Save asks for its name.
        memset(&s_work, 0, sizeof(s_work));
        s_work.type = JOY_KEMPSTON;
        Config::joyDefaults(s_work.type, s_work.map);
        s_target = tag;
        s_orig = s_work;
        mappingPage();
        invalidateRows();
        return;
    }
    JoyProf::Profile p = r->list.p[tag];

    if (key == 0 || key == 3) {                       // use
        applyLive(p, true);
        char msg[48];
        snprintf(msg, sizeof(msg), " Joystick: %s ", p.name);
        uiToast(msg, false, 900);
        return;
    }
    if (key == 4) {                                   // edit
        s_work = p;
        s_target = tag;
        s_orig = p;
        mappingPage();
        invalidateRows();
        return;
    }
    if (key == 6) {                                   // rename
        string nn = p.name;
        for (;;) {
            if (!uiPrompt(TXT_JOYPROF_NAME, nn, JoyProf::NAME_LEN - 1)) return;
            JoyProf::Profile t = p;
            if (!JoyProf::setName(t, nn.c_str())) continue;
            if (!strcmp(t.name, p.name)) return;
            if (JoyProf::find(r->list.p, r->list.n, t.name) >= 0) {
                uiToast(TXT_JOYPROF_DUP, true, 1500);
                continue;
            }
            const bool wasLive = (Config::joy_profile == p.name);
            r->list.p[tag] = t;
            if (!Config::joyProfilesSave(r->list.p, r->list.n)) {
                uiToast(TXT_JOYPROF_SAVE_ERR, true, 2000);
            } else if (wasLive) {
                Config::joy_profile = t.name;
                Config::save();
            }
            invalidateRows();
            return;
        }
    }
    if (key == 8) {                                   // remove
        char q[64];
        snprintf(q, sizeof(q), "Remove profile \"%.20s\" ?", p.name);
        if (!uiConfirm(q)) return;
        memset(&r->list.p[tag], 0, sizeof(r->list.p[tag]));     // the slot empties, the others keep their numbers
        if (!Config::joyProfilesSave(r->list.p, r->list.n)) uiToast(TXT_JOYPROF_SAVE_ERR, true, 2000);
        // The live pad keeps its map; it just has no profile behind it any more.
        if (Config::joy_profile == p.name) { Config::joy_profile.clear(); Config::save(); }
        invalidateRows();
        return;
    }
}

} // namespace nm
