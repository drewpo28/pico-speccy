// pico-speccy — fullscreen menu layout and drawing (internal to src/ui/).
//
// Layout is computed in LOGICAL UI PIXELS (see UiGfx.h), not in character cells, so
// panes, rules, insets and the selection bar are positioned freely and the Profi DS80
// surface gets its full 512-px horizontal detail.
//
// Live surfaces:
//   640x480 output          -> fb 320x240 -> 320x240 logical, glyph 6 px
//   720x480 output          -> fb 360x240 -> 360x240 logical, glyph 6 px
//   720x576 output          -> fb 360x288 -> 360x288 logical, glyph 6 px
//   any of the above + DS80 -> 512x240 logical (256 packed bytes), glyph 12 px

#pragma once

#if NEW_UI

#include <stdint.h>
#include "UiModel.h"
#include "UiGfx.h"

namespace nm {

// Private font glyphs for the arrows / Enter / chevron (the UI font carries them at
// 0x80..0x86 — see tools/mkuifont.py). Shared so the nav can label its hints too.
#define SYM_UP    "\x80"
#define SYM_DOWN  "\x81"
#define SYM_RIGHT "\x82"
#define SYM_LEFT  "\x83"
#define SYM_ENTER "\x84"
#define SYM_CHEV  "\x85"

struct Layout {
    int w, h;               // logical surface extent
    int margin;             // gap around the rounded window
    int ix, iy, iw, ih;     // window content rect (inside the border)
    int hdr_h, sub_h, foot_h;
    int body_y, body_h;     // vertical extent of the two panes
    int lx, lw;             // left pane
    int sep_x;              // separator rule
    int rx, rw;             // right pane
    int row_h;              // one list row
    int body_rows;          // rows that fit in a pane
    int pad;                // inner padding
};
extern Layout LY;

void computeLayout();
bool layoutFits();

// Dirty regions. Drawing lands straight in the live framebuffer that core1 is scanning
// out — no back buffer, no vsync hook — so a cursor move must never repaint everything.
enum Dirty : uint8_t {
    D_HEADER = 0x01, D_SUB = 0x02, D_PTITLE = 0x04,
    D_LEFT   = 0x08, D_RIGHT = 0x10, D_FOOT = 0x20,
    D_ALL    = 0x3F
};
void markDirty(uint8_t bits);
void markLeftRow(int visRow);
void markRightRow(int visRow);
void flushDirty();

void drawFrameOnce();       // static chrome: background, panes, separator

// Marquee for the focused row's label when it overflows the left pane (persist
// names, mounted-image filenames). The nav's idle loop drives it.
bool menuMarqueeTick();     // true = the focused row needs a repaint
void menuMarqueeReset();
bool menuMarqueeActive();

// Header clock — current RTC time ("HH:MM"), shown centered in the header band
// of both the menu and the browser. The RTC only holds a valid time after an
// SNTP sync over WiFi (or the guest setting it), so the clock doubles as a
// "time is synced" indicator: nothing is drawn while it is unset.
bool uiClockText(char out[8]);   // false while the RTC is unset
bool uiClockDirty();             // minute rolled over since the last draw
// Draw it at text baseline `ty`, centered in [ix, ix+iw); skipped when the
// center slot would collide with the header's left text (ends at loEnd) or
// right text (starts at hiBeg).
void uiHeaderClock(int ix, int iw, int ty, int loEnd, int hiBeg);

} // namespace nm

#endif // NEW_UI
