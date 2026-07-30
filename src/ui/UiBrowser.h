// pico-speccy — the fullscreen file browser.
//
// Return protocol (inherited from the file dialog it replaced), so every caller
// keeps its logic:
//   "R" + name   file chosen (name relative to fdir, which tracks navigation)
//   "P" + name   F5 on a disk/image file — caller opens the slot picker
//   "X" + name   F4 on a ZIP — caller extracts it into fdir
//   "\x02UP"     ".." at the root while OSD::fd_root_parent (back to locations)
//   ""           cancelled
//
// Uses the /tmp .idx cache (SortedFiles.h) and the per-type position memory
// (FileUtils::fileTypes), so a browse position survives across sessions.

#pragma once


#include <string>
#include <stdint.h>

namespace nm {

std::string browseFile(std::string& fdir, const std::string& title, uint8_t ftype);

// The F5 "Open from" level, drawn IN the browser chrome (header / location bar /
// panes / footer) rather than as a floating modal — visually it IS the browser,
// one level above the volume roots. Returns the chosen index, or -1 on Esc.
// `hints` (optional, same length) fills the right pane for the focused row.
int browseLocations(const char* const* items, const char* const* hints, int n, int initial);

// Renderer of the shared row index (fdIndexGet) — the body of OSD::fdChromeNav.
// Serves the Remote host list,
// the FTP/SFTP browser and the Web Archives catalog with the same contract:
// returns the 0-based row (or -1), *outKey = OSD::FDK_*, cursor kept in the
// classic 2-based ioFocus/ioBegin so session cursor memory keeps working.
// Stack-light on purpose: the net flows call it on the heap alt-stack.
int browseIndexNav(const std::string& title, const std::string& subtitle, int side,
                   bool utf8, int* outKey, int* ioFocus, int* ioBegin);

// Loader in the browser's status line (the footer), replacing the classic
// centered progressDialog while the new chrome is on screen — same look as the
// SD browser's "Indexing..." bar. Wired as OSD::progressOverride for the F5
// session. action: 0 = show, 1 = update, 2 = close.
void uiProgressStatus(const char* title, const char* msg, int percent, int action,
                      bool cyrillic);

} // namespace nm

