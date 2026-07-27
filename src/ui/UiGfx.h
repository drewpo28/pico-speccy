// pico-speccy — the new menu's own rasteriser.
//
// This does NOT go through Graphics::print/drawChar/fillRect: those are built around
// the ZX face, the ZX 16-colour palette and unclipped writes. Here we own the pixels.
//
// Two backends, chosen at gfxBegin():
//
//  * standard 8bpp indexed. One framebuffer byte per pixel (with the ^2 DMA byte
//    swizzle). The 16 UI colours are installed into palette indices 224..239 —
//    free of Gigascreen (17..136), ULA+ (0..127 incl. its |0x40 dither slots) and the
//    HDMI control block (240..255).
//
//  * Profi DS80 packed pairs. One framebuffer byte carries TWO independent 4-bit
//    pixels (profi_pair_lookup[left][right] -> slot; the HDMI driver emits ink on the
//    first pixel clock and paper on the second), so the usable surface is 512x240 —
//    twice the horizontal detail of the standard mode. The 16 UI colours are installed
//    into the 16 Profi palette entries, which DS80Guard has already made ours for the
//    duration of the menu. Single-pixel writes are read-modify-write on the byte.
//
// Coordinates are LOGICAL UI PIXELS: 1 px = 1 framebuffer pixel in standard mode and
// 1 DS80 pixel in DS80 mode. Text is drawn with a horizontal scale of 1 or 2 so a
// glyph keeps the same apparent size in both, while frames/rules/markers get the full
// DS80 detail. Everything clips.

#pragma once

#if NEW_UI

#include <stdint.h>

#include "UiFont.h"

namespace nm {

// The 16 UI colours. Values are logical slots, not palette indices.
// Budget note: exactly 16 — duplicates were merged (one white serves text/header/
// selection) to free four slots for the rainbow mark in the header.
enum UiColor : uint8_t {
    C_BG = 0,       // outside the rounded window
    C_PANEL,        // window and pane background
    C_PANEL_ALT,    // header / sub-header / pane-title bands
    C_SEP,          // rules and the window border
    C_TEXT,         // primary text
    C_TEXT_DIM,     // secondary text, footer hints
    C_WHITE,        // emphasis, text on the selection bar
    C_SEL_BG,       // selection bar, focused pane
    C_SEL_BAND,     // subtle band for the focused row of the unfocused pane
    C_ACCENT,       // chosen radio dot
    C_FOOT_BG,
    C_SHADOW,
    C_ICON_R,       // rainbow mark
    C_ICON_Y,
    C_ICON_C,
    C_DISABLED,     // "<< Back" and non-selectable rows
    C_COUNT
};

struct Surface {
    bool ds80;
    int  w, h;          // logical pixel extent of the drawable area
    int  fbx;           // framebuffer byte column of logical x=0 (standard mode)
    int  pad;           // DS80: framebuffer byte column of logical x=0 (=pad_l)
    int  oy;            // framebuffer row of logical y=0
    int  glyphScale;    // horizontal glyph scale: 1 standard, 2 DS80
};
extern Surface Sf;

// Computes the surface from the live video mode. Cheap, no side effects — safe to call
// just to test whether the layout fits.
void gfxComputeSurface();
// Computes the surface AND installs the 16 UI colours. Call once per menu open.
void gfxBegin();
// Re-installs the 16 UI colours. Needed after anything that rewrites the hardware
// palette under us (VIDEO::applyPalette() rewrites indices 0..239, ours included).
void gfxInstallPalette();
// Restores whatever the palette was before gfxBegin().
void gfxEnd();

// The 16 UI colours (RGB888, enum UiColor order) and their standard-mode palette
// base. Used by screenshot/BMP writers so a capture of the menu keeps true colours.
const uint32_t* uiPalette();
int             uiPaletteBase();
// Hardware palette byte for a UI colour — for the few places that hand a colour
// to a classic byte-level drawer (LED::drawGlyph in the legend page).
uint8_t         uiPaletteSlot(UiColor c);
// Hand the palette back to the emulator for the duration of a reused modal dialog
// (they draw with zxColor() indices, which in DS80 are our entries), then take it back.
void gfxSuspendPalette();
void gfxResumePalette();

// Metrics in logical pixels.
inline int glyphW() { return UI_FONT_W * Sf.glyphScale; }
inline int cols()   { return Sf.w / glyphW(); }
inline int rows()   { return Sf.h / UI_FONT_H; }

void px(int x, int y, UiColor c);
void hline(int x, int y, int w, UiColor c);
void vline(int x, int y, int h, UiColor c);
void fill(int x, int y, int w, int h, UiColor c);
void frame(int x, int y, int w, int h, UiColor c);

// Transparent glyph run. Returns the advance in logical pixels.
int  text(int x, int y, const char* s, UiColor ink);
// As above but stops before exceeding `maxw` logical pixels, appending ".." when the
// string did not fit.
int  textClip(int x, int y, int maxw, const char* s, UiColor ink);
int  textWidth(const char* s);

// Widgets. These scale horizontally with glyphScale so they keep the same apparent
// size in DS80, where a logical pixel is half as wide.
void radio(int x, int y, bool on, UiColor ink, UiColor accent);
int  radioW();
void chevron(int x, int y, UiColor c);          // 4x7 "descend" arrow
int  chevronW();
void icon1bpp(int x, int y, const uint16_t* rows, int n, int wbits, UiColor c);
// Rounded rectangle: 1-px border with the corners cut back by `r` pixels.
void roundRect(int x, int y, int w, int h, int r, UiColor border, UiColor fill_c);
// Re-draws ONLY the border and the outside-corner pixels of a roundRect. The
// header/footer bands are plain rectangles, so every band repaint squares the
// window corners off — call this after them to restore the rounding.
void roundRectBorder(int x, int y, int w, int h, int r, UiColor border, UiColor outside);
// The ZX rainbow mark of the header.
void rainbow(int x, int y);
int  rainbowW();

} // namespace nm

#endif // NEW_UI
