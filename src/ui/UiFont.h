// pico-speccy — 6x10 UI font (see tools/mkuifont.py for the glyph art).
#pragma once


#include <stdint.h>

namespace nm {

#define UI_FONT_W      6      // cell width: 5 px art + 1 px advance gap
#define UI_FONT_H     10      // cell height; baseline row 7, descenders 8..9
#define UI_FONT_FIRST 0x20
// Past '~' the range is ours: 0x80..0x86 are the UI symbols (arrows, return, chevrons)
// used by the footer and the breadcrumb. Keep in step with tools/mkuifont.py.
#define UI_FONT_LAST  0x86
#define UI_FONT_COUNT (UI_FONT_LAST - UI_FONT_FIRST + 1)

// Row bitmaps, UI_FONT_H per glyph, bit 5 = leftmost pixel.
extern const uint8_t ui_font6x10[UI_FONT_COUNT * UI_FONT_H];

// CP1251 codes past our table (0x87..0xFF) — re-packed at draw time from the
// classic Font6x8Cyr bitmaps (UiFont.cpp), so the web-archive catalog's Cyrillic
// names render without growing the flash font. Returns a per-call static buffer.
const uint8_t* uiGlyphExt(uint8_t c);

// Rows of `ch`; CP1251 range falls through to uiGlyphExt.
inline const uint8_t* uiGlyph(char ch) {
    uint8_t c = (uint8_t)ch;
    if (c >= UI_FONT_FIRST && c <= UI_FONT_LAST)
        return &ui_font6x10[(c - UI_FONT_FIRST) * UI_FONT_H];
    return uiGlyphExt(c);
}

} // namespace nm

