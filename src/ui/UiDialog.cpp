// pico-speccy — modal primitives of the new fullscreen UI (see UiDialog.h).

#include "OSDNewMenu.h"

#if NEW_UI

#include <string.h>
#include <string>

#include "UiDialog.h"
#include "UiGfx.h"
#include "UiRender.h"   // SYM_* glyph names
#include "UiFont.h"
#include "OSDMain.h"
#include "ESPectrum.h"
#include "Video.h"
#include "Debug.h"
#include "fabutils.h"
#include <pico/stdlib.h>

using std::string;

// The classic 6x8 OSD face, for the PAUSED band (it must match the F8 stats
// readout exactly, and that one is drawn with this font).
extern Font Font6x8;

namespace nm {

// ── input ──────────────────────────────────────────────────────────────────────
// Same discipline as UiNav: only the synthetic VK_MENU_* twins are used for the
// arrows/Enter (kbdExtraMapping injects one per raw press and the repeat handler
// re-injects them), so a press never acts twice.

static bool nextKeyDown(fabgl::VirtualKeyItem& k) {
    while (ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
        if (!ESPectrum::readKbd(&k)) continue;
        if (k.down) return true;
    }
    return false;
}

// ── shared box geometry ────────────────────────────────────────────────────────

struct Box { int x, y, w, h; };

static int lineCount(const char* s) {
    int n = 1;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

static int maxLineWidth(const char* s) {
    int best = 0, cur = 0;
    for (; *s; s++) {
        if (*s == '\n') { if (cur > best) best = cur; cur = 0; }
        else cur++;
    }
    if (cur > best) best = cur;
    return best * glyphW();
}

// Draw the box chrome and the body text; returns the box so callers can add
// buttons under the text. `extra_h` reserves room below the text. `shadow` is
// off for the transient status notices drawn over the LIVE guest screen
// (uiOsdMsg): a solid offset slab over a running game reads as garbage there,
// while over the menu chrome it is what separates a modal from the page.
static Box drawBox(const char* text_body, const char* title, int extra_h, UiColor border,
                   int min_w = 0, bool shadow = true) {
    const int pad  = 4 * Sf.glyphScale;
    const int lh   = UI_FONT_H + 2;
    const int tl   = title ? lh + 2 : 0;
    int tw = maxLineWidth(text_body);
    if (title) { int t2 = textWidth(title); if (t2 > tw) tw = t2; }

    Box b;
    b.w = tw + 2 * pad;
    if (b.w < min_w) b.w = min_w;
    const int wmax = Sf.w - 8 * Sf.glyphScale;
    if (b.w > wmax) b.w = wmax;
    b.h = tl + lineCount(text_body) * lh + extra_h + 2 * pad;
    if (b.h > Sf.h - 8) b.h = Sf.h - 8;
    b.x = (Sf.w - b.w) / 2;
    b.y = (Sf.h - b.h) / 2;

    // Drop shadow (optional), then the window.
    if (shadow) fill(b.x + 2 * Sf.glyphScale, b.y + 2, b.w, b.h, C_SHADOW);
    roundRect(b.x, b.y, b.w, b.h, 3, border, C_PANEL_ALT);

    int y = b.y + pad;
    if (title) {
        text(b.x + pad, y, title, C_WHITE);
        hline(b.x + pad, y + lh, b.w - 2 * pad, C_SEP);
        y += tl + 2;
    }
    // Body lines.
    const char* p = text_body;
    char line[64];
    while (*p) {
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
        line[i] = 0;
        if (*p == '\n') p++;
        textClip(b.x + pad, y, b.w - 2 * pad, line, C_TEXT);
        y += lh;
        if (y > b.y + b.h - pad) break;
    }
    return b;
}

// ── confirm ────────────────────────────────────────────────────────────────────

static void drawButtons(const Box& b, bool yes) {
    const int pad = 4 * Sf.glyphScale;
    const int lh  = UI_FONT_H + 2;
    const int by  = b.y + b.h - pad - lh;
    const int bw  = 5 * glyphW();          // " Yes " / " No  "
    const int gap = 2 * glyphW();
    int bx = b.x + (b.w - 2 * bw - gap) / 2;

    fill(bx, by - 1, bw, lh, yes ? C_SEL_BG : C_PANEL_ALT);
    text(bx + (bw - 3 * glyphW()) / 2, by, "Yes", yes ? C_WHITE : C_TEXT_DIM);
    bx += bw + gap;
    fill(bx, by - 1, bw, lh, yes ? C_PANEL_ALT : C_SEL_BG);
    text(bx + (bw - 2 * glyphW()) / 2, by, "No", yes ? C_TEXT_DIM : C_WHITE);
}

bool uiConfirm(const char* text_body, const char* title) {
    Debug::log("uiConfirm: sp=%08x\n", debug_sp());
    gfxResumePalette();
    const int lh = UI_FONT_H + 2;
    Box b = drawBox(text_body, title, lh + 6, C_SEP);

    bool yes = false;                       // default lands on No: Enter must not destroy
    drawButtons(b, yes);

    fabgl::VirtualKeyItem k;
    while (1) {
        if (nextKeyDown(k)) {
            switch (k.vk) {
                case fabgl::VK_MENU_LEFT: case fabgl::VK_MENU_RIGHT:
                case fabgl::VK_MENU_UP:   case fabgl::VK_MENU_DOWN:
                    yes = !yes; drawButtons(b, yes); OSD::clickNoPause(); break;
                case fabgl::VK_MENU_ENTER:
                    OSD::clickNoPause(); return yes;
                case fabgl::VK_ESCAPE: case fabgl::VK_F1:
                    OSD::clickNoPause(); return false;
                case fabgl::VK_y: case fabgl::VK_Y:
                    OSD::clickNoPause(); return true;
                case fabgl::VK_n: case fabgl::VK_N:
                    OSD::clickNoPause(); return false;
                default: break;
            }
        }
        sleep_ms(5);
    }
}

// ── N-button choice ────────────────────────────────────────────────────────────

int uiChoice(const char* text_body, const char* const* btns, int n, int initial,
             int esc_result) {
    gfxResumePalette();
    const int lh = UI_FONT_H + 2;

    // The box must fit the button row too.
    int bw_total = 0;
    for (int i = 0; i < n; i++) bw_total += ((int)strlen(btns[i]) + 2) * glyphW();
    bw_total += (n - 1) * 2 * glyphW();

    Box b = drawBox(text_body, nullptr, lh + 6, C_SEP,
                    bw_total + 8 * Sf.glyphScale);      // wide enough for the buttons

    int sel = (initial >= 0 && initial < n) ? initial : 0;
    const int by = b.y + b.h - 4 * Sf.glyphScale - lh;
    auto drawBtns = [&]() {
        int x = b.x + (b.w - bw_total) / 2;
        for (int i = 0; i < n; i++) {
            const int w = ((int)strlen(btns[i]) + 2) * glyphW();
            fill(x, by - 1, w, lh, i == sel ? C_SEL_BG : C_PANEL_ALT);
            text(x + glyphW(), by, btns[i], i == sel ? C_WHITE : C_TEXT_DIM);
            x += w + 2 * glyphW();
        }
    };
    drawBtns();

    fabgl::VirtualKeyItem k;
    while (1) {
        if (nextKeyDown(k)) {
            switch (k.vk) {
                case fabgl::VK_MENU_LEFT:
                    sel = (sel + n - 1) % n; drawBtns(); OSD::clickNoPause(); break;
                case fabgl::VK_MENU_RIGHT:
                case fabgl::VK_TAB:
                    sel = (sel + 1) % n; drawBtns(); OSD::clickNoPause(); break;
                case fabgl::VK_MENU_ENTER:
                    OSD::clickNoPause(); return sel;
                case fabgl::VK_ESCAPE: case fabgl::VK_F1:
                    OSD::clickNoPause(); return esc_result;
                default: break;
            }
        }
        sleep_ms(5);
    }
}

// ── toast ──────────────────────────────────────────────────────────────────────

void uiToast(const char* msg, bool warn, int timeout_ms) {
    gfxResumePalette();
    drawBox(msg, nullptr, 0, warn ? C_ICON_R : C_ACCENT);

    fabgl::VirtualKeyItem k;
    int waited = 0;
    while (timeout_ms == 0 || waited < timeout_ms) {
        if (nextKeyDown(k)) return;
        sleep_ms(5);
        waited += 5;
    }
}

void uiPausedBadge() {
    // Same look as the F8 stats readout: the 6x8 OSD face on a UI-palette band in
    // the bottom border, no frame and no shadow — it annotates the frozen screen
    // instead of burying it. Geometry follows OSD::drawStats().
    gfxComputeSurface();
    gfxInstallPalette();            // applyPalette() may have rewritten our block
    const int base = uiPaletteBase();
    const char* txt = " PAUSED ";
    const int w = (int)strlen(txt) * OSD_FONT_W;
    int x, y;
    if (VIDEO::isFullBorder288())      { x = 188; y = 268; }
    else if (VIDEO::isFullBorder240()) { x = 188; y = 220; }
    else                               { x = 168; y = 220; }
    // Right-aligned to the same 144-px band the stats occupy, first line.
    x += (144 - w) / 2;
    VIDEO::vga.setTextColor((uint8_t)(base + C_WHITE), (uint8_t)(base + C_SEL_BG));
    VIDEO::vga.setFont(Font6x8);
    VIDEO::vga.setCursor(x, y);
    VIDEO::vga.print(txt);
}

// The classic centered-message contract in the new skin. LEVEL_* values are the
// messages.h ones (0 info, 1 ok, 2 warn, 3 error).
void uiOsdMsg(const char* msg, uint8_t level, uint16_t ms) {
    gfxBegin();                    // standalone-safe: hotkey paths have no gfx yet
    UiColor border;
    switch (level) {
        case 1:  border = C_ACCENT; break;      // LEVEL_OK
        case 2:  border = C_ICON_Y; break;      // LEVEL_WARN
        case 3:  border = C_ICON_R; break;      // LEVEL_ERROR
        default: border = C_SEP;    break;      // LEVEL_INFO
    }
    drawBox(msg, nullptr, 0, border, 0, /*shadow=*/false);   // over the live screen
    if (!ms) return;               // persistent notice: caller repaints later

    fabgl::VirtualKeyItem k;
    int waited = 0;
    while (waited < (int)ms) {
        if (nextKeyDown(k)) break;
        sleep_ms(5);
        waited += 5;
    }
    gfxEnd();
}

// ── inline line editor ─────────────────────────────────────────────────────────

bool uiEditLine(int x, int y, int wpx, string& io, size_t maxlen, bool mask) {
    Debug::log("uiEditLine: sp=%08x wpx=%d len=%u\n", debug_sp(), wpx, (unsigned)io.size());
    gfxResumePalette();
    const int lh   = UI_FONT_H + 2;
    const int vis  = wpx / glyphW();
    if (vis < 2) return false;

    auto kbd = ESPectrum::PS2Controller.keyboard();
    // Drain whatever is still queued (the Enter that opened this prompt, or a
    // repeat-injected synthetic) so it cannot confirm an untouched field.
    { fabgl::VirtualKeyItem drain; while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&drain); }

    bool reveal = false;                    // masked field: TAB shows the text

    size_t cur = io.size();                 // cursor = insertion point
    int scroll = 0;

    fabgl::VirtualKeyItem k;
    uint8_t blink = 0;
    bool cursorOn = true;

    while (1) {
        // Keep the cursor inside the window.
        if ((int)cur < scroll) scroll = (int)cur;
        if ((int)cur > scroll + vis - 1) scroll = (int)cur - (vis - 1);

        // Repaint the field.
        fill(x, y - 1, wpx, lh, C_BG);
        for (int p = 0; p < vis; p++) {
            const size_t i = (size_t)(scroll + p);
            const char ch = (i < io.size()) ? ((mask && !reveal) ? '*' : io[i]) : ' ';
            const bool atCur = (i == cur);
            const int cx = x + p * glyphW();
            if (atCur && cursorOn) fill(cx, y - 1, glyphW(), lh, C_SEL_BG);
            if (ch != ' ') {
                char s[2] = { ch, 0 };
                text(cx, y, s, atCur && cursorOn ? C_WHITE : C_TEXT);
            }
        }

        // Wait for a key, blinking at ~300 ms.
        while (1) {
            if (nextKeyDown(k)) break;
            sleep_ms(5);
            if ((++blink & 0x3F) == 0) { cursorOn = !cursorOn; goto repaint; }
        }
        cursorOn = true;

        switch (k.vk) {
            case fabgl::VK_MENU_ENTER:  OSD::clickNoPause(); return true;
            case fabgl::VK_ESCAPE:
            case fabgl::VK_F1:          OSD::clickNoPause(); return false;
            case fabgl::VK_MENU_LEFT:   if (cur) cur--; break;
            case fabgl::VK_MENU_RIGHT:  if (cur < io.size()) cur++; break;
            case fabgl::VK_MENU_HOME:
            case fabgl::VK_HOME:        cur = 0; break;
            case fabgl::VK_END:         cur = io.size(); break;
            case fabgl::VK_MENU_BS:
                if (cur) { io.erase(cur - 1, 1); cur--; }
                break;
            case fabgl::VK_DELETE:
                if (cur < io.size()) io.erase(cur, 1);
                break;
            case fabgl::VK_TAB:
                if (mask) reveal = !reveal;     // peek at the password
                break;
            default:
                if (k.ASCII >= 32 && k.ASCII < 127 && io.size() < maxlen) {
                    // The keyboard layer reports letters as 'A'..'Z' and symbols
                    // unshifted — apply case and the US shift map here, exactly
                    // like the classic inlineTextEdit (passwords need both).
                    char c = (char)k.ASCII;
                    const bool shift = kbd->isVKDown(fabgl::VK_LSHIFT) ||
                                       kbd->isVKDown(fabgl::VK_RSHIFT);
                    if (c >= 'A' && c <= 'Z') {
                        const bool caps = kbd->isVKDown(fabgl::VK_CAPSLOCK);
                        if (!shift && !caps) c = (char)(c - 'A' + 'a');
                    } else if (shift) {
                        c = shiftSymUS(c);
                    }
                    io.insert(cur, 1, c);
                    cur++;
                }
                break;
        }
    repaint:;
    }
}

// ── busy box ───────────────────────────────────────────────────────────────────

void uiBusy(const char* msg) {
    gfxResumePalette();
    drawBox(msg, nullptr, 0, C_ACCENT);
}

// ── modal list picker ──────────────────────────────────────────────────────────

int uiPickListCb(const char* title, int n, UiRowCb cb, int initial, int wchars,
                 uint8_t* fkey) {
    if (n <= 0) return -1;
    if (fkey) *fkey = 0;
    gfxResumePalette();
    const int pad = 4 * Sf.glyphScale;
    const int lh  = UI_FONT_H + 2;

    int tw = wchars * glyphW();
    { const int t2 = textWidth(title); if (t2 > tw) tw = t2; }

    int rows = n;
    const int maxRows = (Sf.h - 6 * lh) / lh;
    if (rows > maxRows) rows = maxRows;

    Box b;
    b.w = tw + 3 * pad;
    const int wmax = Sf.w - 8 * Sf.glyphScale;
    if (b.w > wmax) b.w = wmax;
    b.h = (lh + 2) + rows * lh + 2 * pad;
    b.x = (Sf.w - b.w) / 2;
    b.y = (Sf.h - b.h) / 2;

    int sel = (initial >= 0 && initial < n) ? initial : 0;
    int top = 0;
    if (sel >= rows) top = sel - rows + 1;
    const int maxTop = n - rows;

    char row[96];
    auto drawIt = [&]() {
        fill(b.x + 2 * Sf.glyphScale, b.y + 2, b.w, b.h, C_SHADOW);
        roundRect(b.x, b.y, b.w, b.h, 3, C_SEP, C_PANEL_ALT);
        text(b.x + pad, b.y + pad - 1, title, C_WHITE);
        if (n > rows) {
            char pos[16];
            snprintf(pos, sizeof(pos), "%d/%d", sel + 1, n);
            text(b.x + b.w - pad - textWidth(pos), b.y + pad - 1, pos, C_TEXT_DIM);
        }
        hline(b.x + pad, b.y + pad + lh - 2, b.w - 2 * pad, C_SEP);
        const int ly0 = b.y + pad + lh + 1;
        for (int r = 0; r < rows; r++) {
            const int i = top + r;
            const int y = ly0 + r * lh;
            const bool s = (i == sel);
            fill(b.x + 2, y - 1, b.w - 4, lh, s ? C_SEL_BG : C_PANEL_ALT);
            if (i < n) {
                row[0] = 0;
                cb(i, row, sizeof(row));
                textClip(b.x + pad, y, b.w - 2 * pad, row, s ? C_WHITE : C_TEXT);
            }
        }
    };
    drawIt();

    fabgl::VirtualKeyItem k;
    while (1) {
        if (nextKeyDown(k)) {
            int ns = sel;
            switch (k.vk) {
                case fabgl::VK_MENU_UP:    ns = sel - 1; break;
                case fabgl::VK_MENU_DOWN:  ns = sel + 1; break;
                case fabgl::VK_PAGEUP:     ns = sel - rows; break;
                case fabgl::VK_PAGEDOWN:   ns = sel + rows; break;
                case fabgl::VK_MENU_HOME:
                case fabgl::VK_HOME:       ns = 0; break;
                case fabgl::VK_END:        ns = n - 1; break;
                case fabgl::VK_MENU_ENTER: OSD::clickNoPause(); return sel;
                case fabgl::VK_F8:
                case fabgl::VK_DELETE:
                    if (fkey) { *fkey = 8; OSD::clickNoPause(); return sel; }
                    break;
                case fabgl::VK_ESCAPE: case fabgl::VK_F1:
                case fabgl::VK_MENU_LEFT:  OSD::clickNoPause(); return -1;
                default: break;
            }
            if (ns < 0) ns = 0;
            if (ns > n - 1) ns = n - 1;
            if (ns != sel) {
                sel = ns;
                if (sel < top) top = sel;
                if (sel >= top + rows) top = sel - rows + 1;
                if (top > maxTop) top = maxTop > 0 ? maxTop : 0;
                drawIt();
                OSD::clickNoPause();
            }
        }
        sleep_ms(5);
    }
}

// Array-backed variant: width from the widest visible item.
static const char* const* s_pl_items;
static void plArrayCb(int i, char* out, size_t n) { snprintf(out, n, "%s", s_pl_items[i]); }

int uiPickList(const char* title, const char* const* items, int n, int initial) {
    int wch = 8;
    for (int i = 0; i < n; i++) {
        const int w = (int)strlen(items[i]);
        if (w > wch) wch = w;
    }
    s_pl_items = items;
    return uiPickListCb(title, n, plArrayCb, initial, wch);
}

// ── full-screen text page ──────────────────────────────────────────────────────
// The new-style replacement for OSD::showTextDialog: a scrollable page in the
// menu's chrome. Zero-copy line index into the caller's text, like the classic.

// Inline colour markup: '\x02' + letter switches the ink for the rest of the
// line (About tints names like the classic credits did). Letters deliberately
// avoid hex digits so "\x02W..." never merges into a longer escape in the C
// literals that carry the text. The ink resets to C_TEXT for every line, so a
// page renders the same no matter where the scroll window starts.
static UiColor inkFor(char c) {
    switch (c) {
        case 'W': return C_WHITE;
        case 'M': return C_TEXT_DIM;
        case 'G': return C_ACCENT;
        case 'Y': return C_ICON_Y;
        case 'S': return C_ICON_C;
        case 'R': return C_ICON_R;
        default:  return C_TEXT;
    }
}

void uiMarkupLine(int x, int y, int maxw, const char* s, int len) {
    char seg[96];
    UiColor ink = C_TEXT;
    int cx = x;
    const int maxx = x + maxw;
    const char* p = s;
    const char* e = s + len;
    int si = 0;
    auto flushSeg = [&]() {
        if (!si) return;
        seg[si] = 0;
        cx += textClip(cx, y, maxx - cx, seg, ink);
        si = 0;
    };
    while (p < e && cx < maxx) {
        if (*p == '\x02' && p + 1 < e) {
            flushSeg();
            ink = inkFor(p[1]);
            p += 2;
            continue;
        }
        if (si < (int)sizeof(seg) - 1) seg[si++] = *p;
        p++;
    }
    flushSeg();
}

static void textPageRun(const char* title, const char* body,
                        const char* (*refresh)(), int period_ms,
                        const char* hdrRight = nullptr) {
    gfxResumePalette();
    // Drain the key that opened this page (F1/Enter may still be held — its
    // auto-repeat would close the page the moment it appears).
    { auto kbd = ESPectrum::PS2Controller.keyboard();
      fabgl::VirtualKeyItem d;
      while (kbd->virtualKeyAvailable()) kbd->getNextVirtualKey(&d); }
    const int sc  = Sf.glyphScale;
    const int lh  = UI_FONT_H + 2;
    // Same inset the menu and the browser use (LY.pad/L.pad = 2*sc), so the
    // header title and the footer hints line up across every screen.
    const int pad = 2 * sc;

    const int margin = 2 * sc;
    const int ix = margin + sc, iy = 3;
    const int iw = Sf.w - 2 * ix, ih = Sf.h - iy - 3;
    const int hdr_h = UI_FONT_H + 6, foot_h = UI_FONT_H + 4;
    const int body_y = iy + hdr_h + 2;
    const int body_h = iy + ih - foot_h - body_y;
    const int rows = body_h / lh;

    // Line index (start + length), capped generously above any info page.
    const int MAXL = 128;
    const char* ls[MAXL];
    uint16_t ll[MAXL];
    int nlines = 0;
    auto parse = [&]() {
        nlines = 0;
        for (const char* p = body; *p && nlines < MAXL; ) {
            ls[nlines] = p;
            const char* e = p;
            while (*e && *e != '\n') e++;
            ll[nlines++] = (uint16_t)(e - p);
            p = *e ? e + 1 : e;
        }
    };
    parse();

    int top = 0;
    int maxTop = nlines > rows ? nlines - rows : 0;

    // static chrome
    fill(0, 0, Sf.w, Sf.h, C_BG);
    roundRect(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, C_SEP, C_PANEL);
    fill(ix, iy, iw, hdr_h, C_PANEL);
    rainbow(ix + pad, iy + 3);
    int title_w = iw - 4 * pad - rainbowW();
    if (hdrRight && *hdrRight) {
        const int rw = textWidth(hdrRight);
        text(ix + iw - pad - rw, iy + 4, hdrRight, C_TEXT_DIM);
        title_w -= rw + 2 * pad;
    }
    textClip(ix + pad + rainbowW() + 2 * pad, iy + 4, title_w, title, C_WHITE);
    hline(ix, iy + hdr_h - 1, iw, C_SEP);
    const int fy = iy + ih - foot_h;
    fill(ix, fy, iw, foot_h, C_FOOT_BG);
    hline(ix, fy, iw, C_SEP);
    text(ix + pad, fy + 3, SYM_UP SYM_DOWN " Scroll   Esc Close", C_TEXT_DIM);
    // Bands are square fills — restore the window's corner curve over them.
    roundRectBorder(margin, iy - 1, Sf.w - 2 * margin, ih + 2, 4, C_SEP, C_BG);

    auto drawBody = [&]() {
        for (int r = 0; r < rows; r++) {
            const int y = body_y + r * lh;
            fill(ix, y, iw, lh, C_PANEL);
            const int li = top + r;
            if (li >= nlines) continue;
            uiMarkupLine(ix + pad, y + 1, iw - 2 * pad, ls[li], ll[li]);
        }
        if (maxTop) {
            char pos[16];
            snprintf(pos, sizeof(pos), "%d-%d/%d", top + 1,
                     top + rows < nlines ? top + rows : nlines, nlines);
            const int pw = textWidth(pos);
            fill(ix + iw - pw - 2 * pad, fy + 1, pw + 2 * pad, foot_h - 2, C_FOOT_BG);
            text(ix + iw - pw - pad, fy + 3, pos, C_TEXT_DIM);
        }
    };
    drawBody();

    fabgl::VirtualKeyItem k;
    int idle_ms = 0;
    while (1) {
        if (nextKeyDown(k)) {
            int nt = top;
            switch (k.vk) {
                case fabgl::VK_MENU_UP:    nt = top - 1; break;
                case fabgl::VK_MENU_DOWN:  nt = top + 1; break;
                case fabgl::VK_PAGEUP:     nt = top - rows; break;
                case fabgl::VK_PAGEDOWN:   nt = top + rows; break;
                case fabgl::VK_MENU_HOME:
                case fabgl::VK_HOME:       nt = 0; break;
                case fabgl::VK_END:        nt = maxTop; break;
                case fabgl::VK_MENU_ENTER:
                case fabgl::VK_ESCAPE: case fabgl::VK_F1:
                case fabgl::VK_MENU_LEFT:
                    OSD::clickNoPause();
                    return;
                default: break;
            }
            if (nt < 0) nt = 0;
            if (nt > maxTop) nt = maxTop;
            if (nt != top) { top = nt; drawBody(); OSD::clickNoPause(); }
        }
        sleep_ms(5);
        if (refresh && (idle_ms += 5) >= period_ms) {
            idle_ms = 0;
            const char* nb = refresh();
            if (nb) {                   // nullptr = nothing changed: skip the repaint
                const bool atTail = (top == maxTop);
                body = nb;
                parse();
                maxTop = nlines > rows ? nlines - rows : 0;
                // A live log follows its tail unless the user scrolled away.
                if (atTail || top > maxTop) top = maxTop;
                drawBody();
            }
        }
    }
}

void uiTextPage(const char* title, const char* body) {
    textPageRun(title, body, nullptr, 0);
}

void uiTextPageLive(const char* title, const char* (*refresh)(), int period_ms,
                    const char* hdrRight) {
    const char* b = refresh();          // first build may already say "no change"
    textPageRun(title, b ? b : "", refresh, period_ms, hdrRight);
}

// ── boxed prompt ───────────────────────────────────────────────────────────────

bool uiPrompt(const char* title, string& io, size_t maxlen, bool mask, bool allowEmpty) {
    Debug::log("uiPrompt: sp=%08x '%s'\n", debug_sp(), title);
    gfxResumePalette();
    const int pad = 4 * Sf.glyphScale;
    const int lh  = UI_FONT_H + 2;

    Box b;
    b.w = 34 * glyphW() + 2 * pad;
    const int wmax = Sf.w - 8 * Sf.glyphScale;
    if (b.w > wmax) b.w = wmax;
    b.h = (lh + 2) + lh + 2 * pad + 2;
    b.x = (Sf.w - b.w) / 2;
    b.y = (Sf.h - b.h) / 2;

    fill(b.x + 2 * Sf.glyphScale, b.y + 2, b.w, b.h, C_SHADOW);
    roundRect(b.x, b.y, b.w, b.h, 3, C_SEP, C_PANEL_ALT);
    textClip(b.x + pad, b.y + pad - 1, b.w - 2 * pad, title, C_WHITE);
    hline(b.x + pad, b.y + pad + lh - 2, b.w - 2 * pad, C_SEP);

    const bool ok = uiEditLine(b.x + pad, b.y + pad + lh + 2, b.w - 2 * pad, io, maxlen, mask);
    return ok && (allowEmpty || !io.empty());
}

} // namespace nm

#endif // NEW_UI
