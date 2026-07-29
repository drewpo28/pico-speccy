// pico-speccy — fullscreen menu layout and drawing.
//
//  +-----------------------------------------------------+  rounded window, 1-px border
//  | [rainbow] Pico-Speccy                       v0.0.1  |  header
//  |-----------------------------------------------------|
//  | Settings > Storage            Machine: Pentagon 128 |  sub-header
//  |--------------------+--------------------------------|
//  | << Back            | esxDOS interface               |  dim back row / pane title
//  | Tape             > |--------------------------------|
//  | Betadisk         > | Off                          o |
//  | esxDOS      DivMMC | DivMMC                       * |  selection = blue bar
//  |--------------------+--------------------------------|
//  | ^v Move   > Select   Esc / << Back                  |  footer
//  +-----------------------------------------------------+
//
// Everything goes through UiGfx: own font, own 16-colour palette, own clipped blitter.
// No Graphics::print, no zxColor, no character grid.

#include "OSDNewMenu.h"

#if NEW_UI

#include <stdio.h>
#include <string.h>

#include "UiRender.h"
#include "UiNav.h"
#include "UiGfx.h"
#include "UiFont.h"
#include "UiStage.h"
#include "OSDMain.h"
#include "Config.h"
#include "RTC.h"

namespace nm {

Layout LY;

// Footer/breadcrumb symbols live in the private glyph range past '~'
// (see tools/mkuifont.py): they are ours, so no Unicode and no ZX font needed.

// ── layout ─────────────────────────────────────────────────────────────────────

void computeLayout() {
    const int sc = Sf.glyphScale;
    LY.w = Sf.w;
    LY.h = Sf.h;
    LY.pad = 2 * sc;                        // inner text padding

    // The window: small margin, 1-px rounded border, content inset one more pixel.
    LY.margin = 2 * sc;
    LY.ix = LY.margin + sc;
    LY.iy = 3;
    LY.iw = LY.w - 2 * LY.ix;
    LY.ih = LY.h - LY.iy - 3;

    LY.row_h  = UI_FONT_H + 2;              // 12 px
    LY.hdr_h  = UI_FONT_H + 6;
    LY.sub_h  = UI_FONT_H + 4;
    LY.foot_h = UI_FONT_H + 4;

    LY.body_y = LY.iy + LY.hdr_h + LY.sub_h;
    LY.body_h = LY.iy + LY.ih - LY.foot_h - LY.body_y;

    // Left pane: a fixed character budget so labels stay readable. Wide enough
    // for a long label AND its value column ("Preferred machine  128K") — the
    // right pane's longest content (Profi ROM sets + radio mark, ~20 chars)
    // still fits in what remains.
    const int lchars = (cols() >= 50) ? 24 : 19;
    LY.lx = LY.ix;
    LY.lw = lchars * glyphW() + 2 * LY.pad;
    LY.sep_x = LY.lx + LY.lw;
    LY.rx = LY.sep_x + sc;
    LY.rw = LY.ix + LY.iw - LY.rx;

    // Both panes spend their first row on a title ("<< Back" / the focused item).
    LY.body_rows = LY.body_h / LY.row_h - 1;
    if (LY.body_rows < 1) LY.body_rows = 1;
}

bool layoutFits() {
    return cols() >= 40 && LY.body_rows >= 6;
}

// ── dirty tracking ─────────────────────────────────────────────────────────────

static uint8_t s_dirty;
static int8_t  s_lrow[2] = { -1, -1 };
static int8_t  s_rrow[2] = { -1, -1 };

void markDirty(uint8_t bits) { s_dirty |= bits; }

static void pushRow(int8_t* slots, int row, uint8_t whole) {
    if (row < 0) return;
    for (int i = 0; i < 2; i++) {
        if (slots[i] == row) return;
        if (slots[i] < 0) { slots[i] = (int8_t)row; return; }
    }
    slots[0] = slots[1] = -1;                // more than two queued: repaint the pane
    s_dirty |= whole;
}

void markLeftRow(int visRow)  { pushRow(s_lrow, visRow, D_LEFT); }
void markRightRow(int visRow) { pushRow(s_rrow, visRow, D_RIGHT); }

// ── static chrome ──────────────────────────────────────────────────────────────

void drawFrameOnce() {
    fill(0, 0, LY.w, LY.h, C_BG);
    roundRect(LY.margin, LY.iy - 1, LY.w - 2 * LY.margin, LY.ih + 2, 4, C_SEP, C_PANEL);
    vline(LY.sep_x, LY.body_y, LY.body_h, C_SEP);
}

// ── bands ──────────────────────────────────────────────────────────────────────

static void drawHeader() {
    const int y = LY.iy;
    fill(LY.ix, y, LY.iw, LY.hdr_h, C_PANEL);
    rainbow(LY.ix + LY.pad, y + 3);
    int tx = LY.ix + LY.pad + rainbowW() + 2 * LY.pad;
    tx += text(tx, y + 4, "Pico-Speccy", C_WHITE);
    const char* ver = "v" PORT_VERSION;
    const int vx = LY.ix + LY.iw - textWidth(ver) - LY.pad;
    text(vx, y + 4, ver, C_TEXT_DIM);
    uiHeaderClock(LY.ix, LY.iw, y + 4, tx + 2 * LY.pad, vx - 2 * LY.pad);
    hline(LY.ix, y + LY.hdr_h - 1, LY.iw, C_SEP);
}

static void drawSubHeader() {
    const int y = LY.iy + LY.hdr_h;
    fill(LY.ix, y, LY.iw, LY.sub_h, C_PANEL_ALT);

    // "Settings > Storage > ..." — chevrons dim, deepest level bright, so the eye lands
    // on where it actually is.
    int x = LY.ix + LY.pad;
    x += text(x, y + 2, "Settings", S.depth ? C_TEXT_DIM : C_WHITE);
    for (uint8_t d = 1; d <= S.depth; d++) {
        const Node* p = S.lv[d].parent;
        if (!p) break;
        x += text(x, y + 2, " " SYM_CHEV " ", C_DISABLED);
        x += text(x, y + 2, p->label, d == S.depth ? C_WHITE : C_TEXT_DIM);
    }

    char mach[40];
    snprintf(mach, sizeof(mach), "Machine: %s",
             archToStr(archDisplay(Config::arch, Config::romSet)));
    const int mw = textWidth(mach);
    if (x + mw + 2 * LY.pad < LY.ix + LY.iw)
        text(LY.ix + LY.iw - mw - LY.pad, y + 2, mach, C_TEXT_DIM);
    hline(LY.ix, y + LY.sub_h - 1, LY.iw, C_SEP);
}

// ── header clock ───────────────────────────────────────────────────────────────
// Shared with the browser's header. One "last drawn" slot is enough — the menu
// and the browser never run concurrently.

static char s_clk_drawn[8];

bool uiClockText(char out[8]) {
    int Y, M, D, h, m, s;
    if (!RTC::now(Y, M, D, h, m, s)) return false;
    snprintf(out, 8, "%02d:%02d", h, m);
    return true;
}

bool uiClockDirty() {
    char clk[8];
    if (!uiClockText(clk)) return false;
    return strcmp(clk, s_clk_drawn) != 0;
}

void uiHeaderClock(int ix, int iw, int ty, int loEnd, int hiBeg) {
    char clk[8];
    if (!uiClockText(clk)) return;
    // Recorded even when the slot is too crowded to draw, so uiClockDirty()
    // doesn't ask for a header repaint every idle tick.
    strcpy(s_clk_drawn, clk);
    const int cw = textWidth(clk);
    const int cx = ix + (iw - cw) / 2;
    if (cx < loEnd || cx + cw > hiBeg) return;
    text(cx, ty, clk, C_TEXT_DIM);
}

// ── marquee for over-long focused labels ───────────────────────────────────────
// Same cadence as the browser: ~1 s idle, then one character per ~200 ms, hold
// on the tail, restart. Any keypress resets (UiNav calls menuMarqueeReset).

static char s_mq_lbl[NM_DYN_LABEL_LEN];
static int  s_mq_avail, s_mq_off, s_mq_idle, s_mq_tick;

void menuMarqueeReset()  { s_mq_off = 0; s_mq_idle = 0; s_mq_tick = 0; }
bool menuMarqueeActive() { return s_mq_off != 0; }

bool menuMarqueeTick() {
    const int fits = s_mq_avail / glyphW();
    if (fits <= 0 || (int)strlen(s_mq_lbl) <= fits) return false;
    if (s_mq_idle < 200) { s_mq_idle++; return false; }
    if (++s_mq_tick < 40) return false;
    s_mq_tick = 0;
    if ((int)strlen(s_mq_lbl) - s_mq_off <= fits) { s_mq_off = 0; s_mq_idle = 0; }
    else s_mq_off++;
    return true;
}

// Draw a (possibly scrolled) focused label: hard window, no ".." while sliding.
static void mqLabel(int x, int y, int availw, const char* lbl, UiColor ink) {
    if (s_mq_off > 0) {
        const int fits = availw / glyphW();
        const int len = (int)strlen(lbl);
        const char* p = lbl + (s_mq_off < len ? s_mq_off : len);
        char vis[NM_DYN_LABEL_LEN];
        snprintf(vis, sizeof(vis), "%.*s", fits, p);
        text(x, y, vis, ink);
    } else {
        textClip(x, y, availw, lbl, ink);
    }
}

// ── rows ───────────────────────────────────────────────────────────────────────

static inline int rowY(int visRow) { return LY.body_y + LY.row_h * (visRow + 1); }

static void drawPaneTitles() {
    const int y = LY.body_y;
    fill(LY.lx, y, LY.lw, LY.row_h, C_PANEL);
    if (S.depth)
        text(LY.lx + LY.pad, y + 1, SYM_LEFT SYM_LEFT " Back", C_DISABLED);

    fill(LY.rx, y, LY.rw, LY.row_h, C_PANEL);
    const Node* dynNode = dynOwner();
    const char* title = dynNode ? dynNode->label : rightTitle(curNode());
    while (*title == ' ') title++;          // NM_IND child rows: indent is left-pane only
    textClip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad, title, C_WHITE);
    hline(LY.rx, y + LY.row_h - 1, LY.rw, C_SEP);
}

static void drawLeftRow(int visRow) {
    if (visRow < 0 || visRow >= LY.body_rows) return;
    Level& lv = curLevel();
    const int y = rowY(visRow);
    const int idx = lv.top + visRow;

    const bool sel = (idx < lv.nvis) && (idx == lv.sel);
    fill(LY.lx, y, LY.lw, LY.row_h,
         sel ? (S.focus == FOCUS_LEFT ? C_SEL_BG : C_SEL_BAND) : C_PANEL);
    if (idx >= lv.nvis) return;

    if (lv.dyn) {
        // Pool row: label left, current contents right. A dimmed row is informational
        // (e.g. a slot the active interface does not expose) and cannot be selected.
        const uint8_t r = lv.vis[idx];
        const bool dim = S.dyn.dim[r];
        const UiColor ink = dim ? C_DISABLED : (sel ? C_WHITE : C_TEXT);
        const char* val = S.dyn.value[r];
        // The value column never gets more than half the pane, so an over-long
        // filename cannot push the label out of the row.
        int rw = (val && *val) ? textWidth(val) : 0;
        const int maxv = (LY.lw - 2 * LY.pad) / 2;
        if (rw > maxv) rw = maxv;
        int availw = LY.lw - 2 * LY.pad - rw - (rw ? LY.pad : 0);
        // A badge sits between label and value, in the label's colour scheme but with
        // its own ink, so its state reads at a glance (write-protect: red = on).
        const char* bdg = S.dyn.badge_st[r] ? S.dyn.badge[r] : nullptr;
        const int bw = bdg ? textWidth(bdg) + LY.pad : 0;
        availw -= bw;
        if (availw < 0) availw = 0;
        if (sel && !dim) {
            snprintf(s_mq_lbl, sizeof(s_mq_lbl), "%s", S.dyn.label[r]);
            s_mq_avail = availw;
            mqLabel(LY.lx + LY.pad, y + 1, availw, S.dyn.label[r], ink);
        } else {
            textClip(LY.lx + LY.pad, y + 1, availw, S.dyn.label[r], ink);
        }
        if (bdg) {
            const int lw = textWidth(S.dyn.label[r]);
            const int bx = LY.lx + LY.pad + (lw < availw ? lw : availw) + LY.pad;
            text(bx, y + 1, bdg,
                 dim ? C_DISABLED : (S.dyn.badge_st[r] == 1 ? C_ICON_R : C_DISABLED));
        }
        if (rw)
            textClip(LY.lx + LY.lw - LY.pad - rw, y + 1, rw, val,
                     dim ? C_DISABLED : (sel ? C_WHITE : C_TEXT_DIM));
        return;
    }

    const Node& n = lv.nodes[lv.vis[idx]];
    const bool en = nodeEnabled(n);
    const UiColor ink = !en ? C_DISABLED : (sel ? C_WHITE : C_TEXT);

    const bool isSub = (n.kind == K_SUB || n.kind == K_DYNAMIC);
    const char* val  = isSub ? nullptr : nodeValueLabel(n);
    const int rightW = isSub ? chevronW() : ((val && *val) ? textWidth(val) : 0);

    textClip(LY.lx + LY.pad, y + 1,
             LY.lw - 2 * LY.pad - rightW - (rightW ? LY.pad : 0), n.label, ink);
    if (isSub)
        chevron(LY.lx + LY.lw - LY.pad - chevronW(), y + LY.row_h / 2 - 3,
                !en ? C_DISABLED : (sel ? C_WHITE : C_DISABLED));
    else if (val && *val)
        text(LY.lx + LY.lw - LY.pad - rightW, y + 1, val,
             !en ? C_DISABLED : (sel ? C_WHITE : C_TEXT_DIM));
}

static void drawRightRow(int visRow) {
    if (visRow < 0 || visRow >= LY.body_rows) return;
    const int y = rowY(visRow);
    const int idx = S.rtop + visRow;

    if (curLevel().dyn) {
        fill(LY.rx, y, LY.rw, LY.row_h, C_PANEL);
        const char* h = dynHint(idx);
        if (h) textClip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad, h, C_TEXT_DIM);
        return;
    }

    const Node* n = curNode();

    const bool pick = n && (n->kind == K_RADIO || n->kind == K_BOOL || n->kind == K_INT);
    const bool sel  = pick && idx == S.rsel && idx < S.rcount;
    fill(LY.rx, y, LY.rw, LY.row_h,
         sel ? (S.focus == FOCUS_RIGHT ? C_SEL_BG : C_SEL_BAND) : C_PANEL);
    if (!n || idx >= S.rcount) return;

    switch (n->kind) {
        case K_RADIO:
        case K_BOOL: {
            uint8_t cnt; const Option* os = nodeOptions(*n, cnt);
            if (idx >= cnt) break;
            const Option& o = os[idx];
            const bool on = (nodeValue(*n) == o.value);
            // Marker on the right, as in the design: hollow ring, filled when chosen.
            radio(LY.rx + LY.rw - LY.pad - radioW(), y + (LY.row_h - 5) / 2, on,
                  sel ? C_WHITE : C_DISABLED, C_ACCENT);
            textClip(LY.rx + LY.pad, y + 1, LY.rw - 3 * LY.pad - radioW(), o.label,
                     sel ? C_WHITE : (on ? C_TEXT : C_TEXT_DIM));
            break;
        }
        case K_SUB: {
            const Node* c = subVisibleChild(n, idx);
            if (c) textClip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad, c->label, C_TEXT_DIM);
            break;
        }
        case K_INT: {
            // One slider row: [====----] 13/16. Vertical keys adjust while focused.
            const int32_t v = nodeValue(*n);
            char buf[16];
            snprintf(buf, sizeof(buf), "%ld/%ld", (long)(v - n->lo), (long)(n->hi - n->lo));
            const int lw = textWidth(buf);
            const int bx = LY.rx + LY.pad;
            const int bw = LY.rw - 3 * LY.pad - lw;
            const int bh = LY.row_h - 4;
            if (bw > 8 && n->hi > n->lo) {
                frame(bx, y + 2, bw, bh, sel ? C_WHITE : C_SEP);
                const int fillw = (int)((int64_t)(bw - 2) * (v - n->lo) / (n->hi - n->lo));
                if (fillw > 0) fill(bx + 1, y + 3, fillw, bh - 2, C_ACCENT);
            }
            text(LY.rx + LY.rw - LY.pad - lw, y + 1, buf, sel ? C_WHITE : C_TEXT_DIM);
            break;
        }
        default:
            if (idx == 0)
                textClip(LY.rx + LY.pad, y + 1, LY.rw - 2 * LY.pad,
                         SYM_ENTER " to open", C_TEXT_DIM);
            break;
    }
}

// ── footer ─────────────────────────────────────────────────────────────────────

static void drawFooter() {
    const int y = LY.iy + LY.ih - LY.foot_h;
    fill(LY.ix, y, LY.iw, LY.foot_h, C_FOOT_BG);
    hline(LY.ix, y, LY.iw, C_SEP);
    const Node* fn_ = curLevel().dyn ? nullptr : curNode();
    const bool intPane = (S.focus == FOCUS_RIGHT) && fn_ && fn_->kind == K_INT;
    const char* hint = (S.focus == FOCUS_LEFT)
        ? SYM_UP SYM_DOWN " Move   " SYM_RIGHT " Select   Esc / " SYM_LEFT SYM_LEFT " Back"
        : intPane
        ? SYM_UP SYM_DOWN " Adjust   " SYM_ENTER " / " SYM_LEFT " Back"
        : SYM_UP SYM_DOWN " Move   " SYM_ENTER " Apply   " SYM_LEFT " Back";
    text(LY.ix + LY.pad, y + 3, hint, C_TEXT_DIM);

    // Pending-changes marker: settings are applied when the menu closes, so the user
    // needs to see that something is waiting.
    const uint8_t pending = Stage::dirtyCount();
    if (pending) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%u pending", (unsigned)pending);
        text(LY.ix + LY.iw - textWidth(buf) - LY.pad, y + 3, buf, C_ACCENT);
    }
}

// ── flush ──────────────────────────────────────────────────────────────────────

void flushDirty() {
    if (s_dirty & D_HEADER) drawHeader();
    if (s_dirty & D_SUB)    drawSubHeader();
    if (s_dirty & D_PTITLE) drawPaneTitles();

    if (s_dirty & D_LEFT)  for (int r = 0; r < LY.body_rows; r++) drawLeftRow(r);
    else for (int i = 0; i < 2; i++) if (s_lrow[i] >= 0) drawLeftRow(s_lrow[i]);

    if (s_dirty & D_RIGHT) for (int r = 0; r < LY.body_rows; r++) drawRightRow(r);
    else for (int i = 0; i < 2; i++) if (s_rrow[i] >= 0) drawRightRow(s_rrow[i]);

    if (s_dirty & D_FOOT) drawFooter();

    // The header/footer fills are square rectangles — restore the corner curve.
    if (s_dirty & (D_HEADER | D_FOOT))
        roundRectBorder(LY.margin, LY.iy - 1, LY.w - 2 * LY.margin, LY.ih + 2, 4,
                        C_SEP, C_BG);

    s_dirty = 0;
    s_lrow[0] = s_lrow[1] = -1;
    s_rrow[0] = s_rrow[1] = -1;
}

} // namespace nm

#endif // NEW_UI
