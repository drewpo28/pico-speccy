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

// `root`, when given, is the highest directory this browse may go up to (it must be
// a prefix of fdir and end with '/'): Debug > Config folders opens one system folder
// and the rest of the card stays out of reach. With OSD::fd_root_parent set, ".."
// there returns "\x02UP" to the level above, as it does at a volume root.
std::string browseFile(std::string& fdir, const std::string& title, uint8_t ftype,
                       const char* root = nullptr);

// The F5 "Open from" level, drawn IN the browser chrome (header / location bar /
// panes / footer) rather than as a floating modal — visually it IS the browser,
// one level above the volume roots. Returns the chosen index, or -1 on Esc; with
// `canBack` (a sub-level, e.g. the card's partitions under "Local (SD)") Left /
// Backspace return -2 instead of being ignored.
// `hints` (optional, same length) fills the right pane for the focused row; `title`
// and `bar` override the header and the location-bar label ("Open file"/"Open from").
int browseLocations(const char* const* items, const char* const* hints, int n, int initial,
                    const char* title = nullptr, const char* bar = nullptr, bool canBack = false);

// The card's partitions as one such level ("Partition N - LABEL" rows, the ones F5
// shows under Local (SD)): returns the chosen partition number, PART_USB for the
// "[USB Drive]" row (only with `withUsb`, when a stick is there), -1 Esc, -2 Back
// (with `canBack`). `*ioCursor` keeps the row between visits. Drawn inside the
// browser chrome; the per-type dialogs reach it with ".." at a volume root.
enum { PART_USB = -3 };
int browsePartitions(int* ioCursor, bool canBack, bool withUsb);
// Make partition `no` browsable: SD:'s own needs nothing, another one is mounted as
// the SD2: volume (fs/SdVolumes; a failure is toasted) and remembered in Config.
// Returns the volume root to open ("/", "SD2:/"), "" = not available.
std::string enterPartition(uint8_t no);

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

