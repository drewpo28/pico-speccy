// pico-speccy — modal primitives of the new fullscreen UI.
//
// These replace OSD::msgDialog / osdCenteredMsg / inlineTextEdit INSIDE the new
// UI: they draw with UiGfx (own font, own palette) over whatever is on screen and
// never touch the SaveRect stack — the caller owns the repaint afterwards, exactly
// like runModal() in UiNav.cpp already does for classic dialogs.
//
// Every entry point re-installs the UI palette first (gfxResumePalette), so they
// are safe to call from an action that runs with the palette handed back to the
// guest (runModal suspends it around actions).

#pragma once


#include <string>
#include <stdint.h>

namespace nm {

// Yes/No question. `text` may contain '\n'. Returns true on Yes.
// Keys: Left/Right/Up/Down move, Enter accepts, Esc = No, Y/N direct.
// `default_yes` puts the initial highlight on Yes — only for questions whose
// answer is safe to take on a bare Enter (the F12 reboot); destructive ones
// keep the default on No.
bool uiConfirm(const char* text, const char* title = nullptr, bool default_yes = false);

// uiConfirm with a live countdown line — the post-reboot video-mode confirm.
// Standalone-safe (gfxBegin/gfxEnd around itself: it runs at boot, before any
// menu). On expiry returns the current highlight (initially No), matching the
// classic dialog, so an unattended black screen auto-reverts.
bool uiConfirmTimeout(const char* text, const char* title, int timeout_sec);

// N horizontal buttons under the text (the commit dialog's Apply/No/Cancel).
// Returns the chosen index; Esc returns `esc_result`.
int uiChoice(const char* text, const char* const* btns, int n, int initial = 0,
             int esc_result = -1);

// Centered message. timeout_ms > 0: shows for that long (any key dismisses
// early); timeout_ms == 0: waits for a key. `warn` colours the border.
void uiToast(const char* msg, bool warn = false, int timeout_ms = 1500);

// The "PAUSED" badge: a small pill near the bottom of the screen, drawn straight
// into the live framebuffer and left there (the Z80 is stopped, so nothing
// repaints over it). Deliberately NOT the centered box the classic OSD used —
// it must not bury the frozen screen it annotates.
void uiPausedBadge();

// The osdCenteredMsg replacement: border coloured by the classic LEVEL_*
// (info/ok/warn/error). ms == 0 draws and RETURNS (the box stays until the
// caller repaints) — the classic contract for "working..." notices.
void uiOsdMsg(const char* msg, uint8_t level, uint16_t ms);

// Inline single-line editor at logical pixel (x, y), `wpx` wide. Edits `io` in
// place with a movable cursor; horizontal scroll when the text outgrows the
// field. Returns true on Enter, false on Esc (io is left as edited either way —
// the caller decides what a cancel means). `mask` renders '*' (passwords).
bool uiEditLine(int x, int y, int wpx, std::string& io, size_t maxlen = 64, bool mask = false);

// Draw-only "working..." box: paints and returns immediately. Use before a
// blocking operation (WiFi scan, SNTP); the caller repaints afterwards.
void uiBusy(const char* msg);

// Modal scrollable list. Returns the chosen index or -1 on Esc.
int uiPickList(const char* title, const char* const* items, int n, int initial = 0);

// As above, but rows are fetched on demand (huge lists — tape blocks): `cb`
// formats row `idx` into `out`. `wchars` fixes the list width in characters.
// `fkey` (optional) enables per-row verbs: F8/Delete return the selected index
// with *fkey = 8 (the caller acts and may reopen); Enter returns it with 0.
// `fixedRows` (optional) keeps the box that many rows tall whatever `n` is —
// for a picker that gets reopened with a different list each time (the download
// folder chooser), so the box never shrinks and leaves the previous one showing
// around it. Rows past `n` are drawn empty. Clamped to what the screen fits.
typedef void (*UiRowCb)(int idx, char* out, size_t outsz);
int uiPickListCb(const char* title, int n, UiRowCb cb, int initial = 0, int wchars = 36,
                 uint8_t* fkey = nullptr, int fixedRows = 0);

// Boxed one-line prompt (title + edit field). Returns true on Enter with
// non-empty text (`allowEmpty` accepts an empty Enter too — optional form
// fields); false on Esc. `io` holds the result.
bool uiPrompt(const char* title, std::string& io, size_t maxlen = 64, bool mask = false,
              bool allowEmpty = false);

// Draw one text line honouring the '\x02'+letter colour markup (see
// UiDialog.cpp inkFor). The ink starts at C_TEXT for every line.
void uiMarkupLine(int x, int y, int maxw, const char* s, int len);

// Full-screen scrollable text page in the menu's chrome — the new-style
// showTextDialog. Also installed as OSD::textPageOverride while the new UI is
// on screen, so every classic info page routes here automatically.
void uiTextPage(const char* title, const char* text);

// Live variant: `refresh` is called every `period_ms` while open and may return
// the (re)built text — the page reparses and repaints, staying pinned to the tail
// if it was there (live logs) — or nullptr for "nothing changed, keep the page"
// (uptime / free-RAM tickers repaint every call; a server log only when a line
// lands). `refresh` is also where a session can pump its work (Ftpd::poll).
// `hdrRight` (optional) is drawn right-aligned in the header band, dimmed —
// a status tag like the current video mode; the title clips around it.
void uiTextPageLive(const char* title, const char* (*refresh)(), int period_ms,
                    const char* hdrRight = nullptr);

} // namespace nm

