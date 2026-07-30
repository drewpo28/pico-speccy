// pico-speccy — the new menu's own rasteriser (see UiGfx.h).

#include "OSDNewMenu.h"


#include <string.h>

#include "UiGfx.h"
#include "UiFont.h"
#include "OSDMain.h"
#include "Video.h"

extern "C" volatile bool profi_ds80_active;
extern "C" void graphics_set_palette(uint8_t i, uint32_t color888);

namespace nm {

Surface Sf;

// ── the UI palette ─────────────────────────────────────────────────────────────
// A cool neutral scheme: this is the whole point of owning the palette instead of
// borrowing the 16 ZX colours. Index order matches enum UiColor.
static const uint32_t kUiPalette[C_COUNT] = {
    0x0F1218,   // C_BG
    0x1E2431,   // C_PANEL
    0x262D3C,   // C_PANEL_ALT
    0x333B4D,   // C_SEP
    0xE6EBF2,   // C_TEXT
    0x8B95A7,   // C_TEXT_DIM
    0xFFFFFF,   // C_WHITE
    0x3B6EF5,   // C_SEL_BG
    0x2B3346,   // C_SEL_BAND
    0x4ADE80,   // C_ACCENT
    0x191F2A,   // C_FOOT_BG
    0x0B0E14,   // C_SHADOW
    0xE23B3B,   // C_ICON_R
    0xF2C43B,   // C_ICON_Y
    0x3BC7E2,   // C_ICON_C
    0x5A6478,   // C_DISABLED
};

// Standard-mode palette block.
//
// Every claimant of the 8-bit palette, and why 152 is the only kind of place this can
// live (getting this wrong is silent: graphics_set_palette() DROPS refused writes, and
// drawing with a reserved index puts TMDS control words on screen — a uniform violet
// wash with no text, which is exactly what index 224 did once HDMI audio was on):
//
//   0..16    ZX colours + ORANGE                     always            Video.cpp
//   0..63    ULA+ palette                            with ULA+         Video.cpp:1020
//   64..127  ULA+ dither slots (i | 0x40)            with ULA+         Video.cpp:1031
//   17..136  Gigascreen blends, exactly 120 entries   with Gigascreen   Video.cpp:2463
//            (120 = the i<j pairs of 16 colours, so the extent is fixed by arithmetic)
//   184..199 HDMI Data Island, set 1                  with HDMI audio   hdmi.c:93
//   216..239 HDMI Data Island set 0, preamble, guard  with HDMI audio   hdmi.c:90-99
//   240..255 HDMI control block                       always           hdmi.c:83
//
// That leaves 137..183 and 200..215. We take the middle of the larger window, which
// keeps ~16 slots of margin from both neighbours.
#define UI_PAL_BASE 152
static_assert(UI_PAL_BASE >= 137 && UI_PAL_BASE + (int)C_COUNT <= 184,
              "UI palette block must stay inside 137..183: 184+ is claimed by the HDMI "
              "audio data islands and <=136 by the Gigascreen blend table");

// DS80: slot byte for a pair of 4-bit colours, and the reverse lookup so a
// single-pixel write can preserve the neighbour half.
static uint8_t ds80_unpair[256];

static inline uint8_t palByte(UiColor c) {
    return Sf.ds80 ? (uint8_t)c : (uint8_t)(UI_PAL_BASE + (uint8_t)c);
}

// ── surface / palette lifecycle ────────────────────────────────────────────────

void gfxComputeSurface() {
    memset(&Sf, 0, sizeof(Sf));
    Sf.ds80 = profi_ds80_active;

    if (Sf.ds80) {
        // The whole framebuffer, border area included: DS80's border is per-T-state
        // writes into the SAME packed-pair buffer (Video.cpp), so unlike a real ZX
        // border it is addressable — the menu owns the full screen instead of a
        // 512-px paper window framed by the guest's border. Exit repaint is already
        // covered: processKeyboard sets VIDEO::brdnextframe after every do_OSD.
        Sf.pad = 0;
        Sf.w   = (int)OSD::scrW * 2;   // scrW = row width in BYTES, 2 px per byte
        Sf.h   = (int)OSD::scrH;
        Sf.oy  = 0;
        Sf.glyphScale = 2;

        for (int ink = 0; ink < 16; ink++)
            for (int paper = 0; paper < 16; paper++)
                ds80_unpair[VIDEO::profi_pair_lookup[ink][paper]] =
                    (uint8_t)((ink << 4) | paper);

    } else {
        Sf.fbx = 0;
        Sf.w   = OSD::scrW;
        Sf.h   = OSD::scrH;
        Sf.oy  = 0;
        Sf.glyphScale = 1;
    }
}

// The UI palette is (re)installed on every open: VIDEO::applyPalette() (a palette
// change, a reset) rewrites 224..239 from the G3R3B2 ramp, so there is no stale state
// to worry about either way.
// Every DS80 palette touch below checks the LIVE profi_ds80_active, not just the
// Sf.ds80 snapshot: a machine switch inside the commit can leave DS80 under a menu
// that opened in it (Profi -> Pentagon). Applying then would re-arm the DS80 driver
// over a standard framebuffer — garbled screen / "DS80 stuck" (hw 2026-07-27).
void gfxInstallPalette() {
    if (Sf.ds80 && profi_ds80_active) {
        VIDEO::applyUiDS80Palette(kUiPalette);
    } else {
        for (int i = 0; i < C_COUNT; i++)
            graphics_set_palette((uint8_t)(UI_PAL_BASE + i), kUiPalette[i]);
    }
}

void gfxBegin() {
    gfxComputeSurface();
    gfxInstallPalette();
}

void gfxSuspendPalette() {
    // Standard mode needs nothing: the dialogs use indices 0..16, we own 224..239.
    if (Sf.ds80 && profi_ds80_active) VIDEO::restoreUiDS80Palette();
}

void gfxResumePalette() {
    if (Sf.ds80 && profi_ds80_active) VIDEO::applyUiDS80Palette(kUiPalette);
}

const uint32_t* uiPalette()     { return kUiPalette; }
int             uiPaletteBase() { return UI_PAL_BASE; }
uint8_t         uiPaletteSlot(UiColor c) { return palByte(c); }

void gfxEnd() {
    if (Sf.ds80 && profi_ds80_active) VIDEO::restoreUiDS80Palette();
    // Standard mode: nothing to undo. 224..239 belong to the UI; the next
    // VIDEO::applyPalette() (palette change / reset) rewrites them from the G3R3B2
    // ramp, and gfxBegin() re-installs on every open, so there is no stale state.
}

// ── pixels ─────────────────────────────────────────────────────────────────────

static inline uint8_t* fbRow(int y) {
    return (uint8_t*)VIDEO::vga.frameBuffer[Sf.oy + y];
}

void px(int x, int y, UiColor c) {
    if (x < 0 || y < 0 || x >= Sf.w || y >= Sf.h) return;
    uint8_t* row = fbRow(y);
    if (!row) return;
    if (Sf.ds80) {
        const int b = Sf.pad + (x >> 1);
        const uint8_t lr = ds80_unpair[row[b ^ 2]];
        uint8_t l = (uint8_t)(lr >> 4), r = (uint8_t)(lr & 0x0F);
        if (x & 1) r = (uint8_t)c; else l = (uint8_t)c;
        row[b ^ 2] = VIDEO::profi_pair_lookup[l][r];
    } else {
        row[(Sf.fbx + x) ^ 2] = palByte(c);
    }
}

void hline(int x, int y, int w, UiColor c) {
    if (y < 0 || y >= Sf.h || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > Sf.w) w = Sf.w - x;
    if (w <= 0) return;
    uint8_t* row = fbRow(y);
    if (!row) return;

    if (Sf.ds80) {
        // Whole-byte writes for the aligned middle; the odd ends go through px().
        int xs = x, xe = x + w;
        if (xs & 1) { px(xs, y, c); xs++; }
        if (xe & 1) { px(xe - 1, y, c); xe--; }
        const uint8_t pair = VIDEO::profi_pair_lookup[(uint8_t)c][(uint8_t)c];
        for (int bx = xs >> 1; bx < (xe >> 1); bx++) row[(Sf.pad + bx) ^ 2] = pair;
    } else {
        const uint8_t v = palByte(c);
        for (int i = 0; i < w; i++) row[(Sf.fbx + x + i) ^ 2] = v;
    }
}

void vline(int x, int y, int h, UiColor c) {
    if (y < 0) { h += y; y = 0; }
    if (y + h > Sf.h) h = Sf.h - y;
    for (int i = 0; i < h; i++) px(x, y + i, c);
}

void fill(int x, int y, int w, int h, UiColor c) {
    if (y < 0) { h += y; y = 0; }
    if (y + h > Sf.h) h = Sf.h - y;
    for (int i = 0; i < h; i++) hline(x, y + i, w, c);
}

void frame(int x, int y, int w, int h, UiColor c) {
    if (w <= 0 || h <= 0) return;
    hline(x, y, w, c);
    hline(x, y + h - 1, w, c);
    vline(x, y, h, c);
    vline(x + w - 1, y, h, c);
}

// ── text ───────────────────────────────────────────────────────────────────────

static void glyph(int x, int y, char ch, UiColor ink) {
    const uint8_t* g = uiGlyph(ch);
    const int sc = Sf.glyphScale;
    for (int r = 0; r < UI_FONT_H; r++) {
        uint8_t bits = g[r];
        if (!bits) continue;
        for (int b = 0; b < UI_FONT_W; b++) {
            if (!(bits & (0x20 >> b))) continue;
            if (sc == 1) px(x + b, y + r, ink);
            else for (int s = 0; s < sc; s++) px(x + b * sc + s, y + r, ink);
        }
    }
}

int textWidth(const char* s) {
    if (!s) return 0;
    return (int)strlen(s) * glyphW();
}

int text(int x, int y, const char* s, UiColor ink) {
    if (!s) return 0;
    const int adv = glyphW();
    int cx = x;
    for (; *s; ++s) {
        if (cx >= Sf.w) break;
        if (*s != ' ') glyph(cx, y, *s, ink);   // space costs nothing to draw
        cx += adv;
    }
    return cx - x;
}

int textClip(int x, int y, int maxw, const char* s, UiColor ink) {
    if (!s || maxw <= 0) return 0;
    const int adv = glyphW();
    const int fits = maxw / adv;
    const int len = (int)strlen(s);
    if (len <= fits) return text(x, y, s, ink);
    // Truncate and mark it: two dots cost one cell each, so keep fits-2 characters.
    int keep = fits - 2;
    if (keep < 0) keep = 0;
    int cx = x;
    for (int i = 0; i < keep; i++, cx += adv)
        if (s[i] != ' ') glyph(cx, y, s[i], ink);
    for (int i = 0; i < 2 && cx < x + maxw; i++, cx += adv) glyph(cx, y, '.', ink);
    return cx - x;
}

// ── widgets ────────────────────────────────────────────────────────────────────

// A DS80 logical pixel is half as WIDE as a standard one (512 vs 320 across the same
// screen) and exactly as tall, so any widget that should look square must be scaled
// horizontally by glyphScale. Hairline rules are deliberately left unscaled: there they
// are genuinely finer, which is the point of rendering natively in DS80.
void radio(int x, int y, bool on, UiColor ink, UiColor accent) {
    const int sc = Sf.glyphScale;
    const int w = 5 * sc, h = 5;
    hline(x + sc, y,         w - 2 * sc, ink);
    hline(x + sc, y + h - 1, w - 2 * sc, ink);
    fill (x,          y + 1, sc, h - 2, ink);
    fill (x + w - sc, y + 1, sc, h - 2, ink);
    if (on) fill(x + sc, y + 1, w - 2 * sc, h - 2, accent);
}

int radioW() { return 5 * Sf.glyphScale; }

void chevron(int x, int y, UiColor c) {
    const int sc = Sf.glyphScale;
    for (int i = 0; i < 4; i++) {
        fill(x + i * sc, y + i,     sc, 1, c);
        fill(x + i * sc, y + 6 - i, sc, 1, c);
    }
}

int chevronW() { return 4 * Sf.glyphScale; }

// Corner cut-back table for r<=4: how many pixels to inset on each of the first r rows.
// Quarter-circle-ish: the outermost row cuts deepest so the curve actually reads.
static const uint8_t kCorner[5] = { 0, 1, 1, 2, 4 };

static inline int cornerCut(int i, int h, int r) {
    if (i < r)           return kCorner[r - i];
    if (i >= h - r)      return kCorner[r - (h - 1 - i)];
    return 0;
}

void roundRect(int x, int y, int w, int h, int r, UiColor border, UiColor fill_c) {
    if (w <= 0 || h <= 0) return;
    if (r > 4) r = 4;
    const int sc = Sf.glyphScale;           // corners stay square on screen in DS80
    for (int i = 0; i < h; i++) {
        const int cut = cornerCut(i, h, r) * sc;
        hline(x + cut, y + i, w - 2 * cut, fill_c);
    }
    // Border. In the corner region each row's segment stretches to the deeper cut
    // of its neighbouring row, so the steps CONNECT into a curve instead of
    // leaving single pixels with gaps between them.
    for (int i = 0; i < h; i++) {
        const int cut = cornerCut(i, h, r) * sc;
        if (i == 0 || i == h - 1) { hline(x + cut, y + i, w - 2 * cut, border); continue; }
        const int above = cornerCut(i - 1, h, r) * sc;
        const int below = cornerCut(i + 1, h, r) * sc;
        const int adj   = above > below ? above : below;
        const int wseg  = (adj > cut) ? (adj - cut + sc) : sc;
        fill(x + cut, y + i, wseg, 1, border);
        fill(x + w - cut - wseg, y + i, wseg, 1, border);
    }
}

void roundRectBorder(int x, int y, int w, int h, int r, UiColor border, UiColor outside) {
    if (w <= 0 || h <= 0) return;
    if (r > 4) r = 4;
    const int sc = Sf.glyphScale;
    for (int i = 0; i < h; i++) {
        const int cut = cornerCut(i, h, r) * sc;
        if (cut) {                              // clear the outside of the curve
            fill(x, y + i, cut, 1, outside);
            fill(x + w - cut, y + i, cut, 1, outside);
        }
        if (i == 0 || i == h - 1) { hline(x + cut, y + i, w - 2 * cut, border); continue; }
        const int above = cornerCut(i - 1, h, r) * sc;
        const int below = cornerCut(i + 1, h, r) * sc;
        const int adj   = above > below ? above : below;
        const int wseg  = (adj > cut) ? (adj - cut + sc) : sc;
        fill(x + cut, y + i, wseg, 1, border);
        fill(x + w - cut - wseg, y + i, wseg, 1, border);
    }
}

// ZX rainbow: four slanted bands. 11x9 logical px (x2 wide in DS80).
void rainbow(int x, int y) {
    const int sc = Sf.glyphScale;
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 11; c++) {
            const int k = c + (8 - r) / 3;
            const UiColor col = (k < 3) ? C_ACCENT : (k < 6) ? C_ICON_Y
                              : (k < 9) ? C_ICON_R : C_ICON_C;
            fill(x + c * sc, y + r, sc, 1, col);
        }
    }
}

int rainbowW() { return 11 * Sf.glyphScale; }

void icon1bpp(int x, int y, const uint16_t* rows, int n, int wbits, UiColor c) {
    const int sc = Sf.glyphScale;
    for (int r = 0; r < n; r++)
        for (int b = 0; b < wbits; b++)
            if (rows[r] & (1 << (wbits - 1 - b)))
                fill(x + b * sc, y + r, sc, 1, c);
}

} // namespace nm

