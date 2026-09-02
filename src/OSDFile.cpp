/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023, 2024 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or 
visit https://zxespectrum.speccy.org/contacto

*/

#include <string>
#include <algorithm>
#include <sys/stat.h>
#include "errno.h"

using namespace std;

#include "OSDMain.h"
#include "FileUtils.h"
#include "UsbMsc.h"
#include "AlfCart.h"
#include "Config.h"
#include "ESPectrum.h"
#include "CPU.h"
#include "Video.h"
#include "messages.h"
#include <math.h>
#include "Z80_JLS/z80.h"
#include "Tape.h"
#include "wd1793.h"
#include "ZipExtract.h"
#include "FileInfo.h"
#include "ui/OSDNewMenu.h"
#include "ui/UiBrowser.h"
#include "ui/UiDialog.h"

#include "ff.h"

#include "Debug.h"
#include "PinSerialData_595.h"
#if ZIFI_NET_CLIENT
#include "RemoteFs.h"
#include "Snapshot.h"
#endif

// The index class + SORT_VERSION moved to SortedFiles.h, shared with the new
// fullscreen browser (src/ui/UiBrowser.cpp) so both use the same /tmp .idx cache.
#include "SortedFiles.h"

inline static size_t crc(const std::string& s) { return sf_name_crc(s); }

static sorted_files filenames;

// Read-only view of the shared index for the new-chrome renderer
// (nm::browseIndexNav) — it draws the very same rows the classic chrome would.
size_t fdIndexSize() { return filenames.size(); }
string fdIndexGet(size_t i) { return filenames.get(i); }




// SD browser: show a ".." even at root "/" (→ return "" → locations). See OSDMain.h.
bool OSD::fd_root_parent = false;
// Last cwd shown by remoteFileDialog (caller records it as the global last F5 location).
string OSD::net_last_path;
bool   OSD::net_close_all = false;   // Esc in a net browser → close the whole OSD




size_t fread(uint8_t* v, size_t sz1, size_t sz2, FIL& f);
int fseek (FIL* stream, long offset, int origin);
inline void fclose(FIL& f) {
    f_close(&f);
}
inline void rewind(FIL& f) {
    f_lseek(&f, 0);
}
void fgets(char* b, size_t sz, FIL& f) {
    UINT br;
    char c;
    do {
        f_read(&f, b, 1, &br);
        c = *b++;
    } while (br == 1 && c != '\n' && !f_eof(&f) && sz--);
    *b = 0;
}
#define ftell(x) f_tell(&x)
#define feof(x) f_eof(&x)

// Display-only volume prefix for the dialog's path row: unprefixed paths live
// on the default volume — "SD:" normally, "USB:" when the stick is the root
// (booted without an SD card). "USB:/..." paths already carry their volume.
static string fdDisplayPath(const string& fdir) {
    if (fdir.find(':') != string::npos) return fdir;
    return (FileUtils::usbRoot ? "USB:" : "SD:") + fdir;
}




// ─── Shared file-browser chrome (locations / remotes / remote+web files) ─────
// Renders the already-populated `filenames` index in the Open File window + the
// per-location sidebar, then runs a navigation loop. Reuses the SD render path
// (fd_Redraw/fd_PrintRow/fd_DrawSidebar) so every location looks like the SD
// browser. Returns the selected row (0-based) or -1; *outKey = FDK_*. Stack-
// agnostic (pure draw + input) → safe on the core stack AND the net alt-stack.
// Every browser list (Remote hosts, FTP/SFTP browse, Web Archives catalog) is drawn
// by the fullscreen chrome; this is the one hook they all share.
int OSD::fdChromeNav(const string& title, const string& subtitle, int side,
                     bool utf8, int* outKey, int* ioFocus, int* ioBegin) {
    return nm::browseIndexNav(title, subtitle, side, utf8, outKey, ioFocus, ioBegin);
}
int OSD::fdChromeList(const vector<string>& rows, const string& title,
                      const string& subtitle, int side, bool utf8, int* outKey,
                      int* ioFocus, int* ioBegin) {
    filenames.init("__fdvirt__");
    filenames.unlink();                 // truncate to empty
    for (const auto& r : rows) filenames.push(r);   // keep caller order (no sort)
    int sel = fdChromeNav(title, subtitle, side, utf8, outKey, ioFocus, ioBegin);
    filenames.close();
    return sel;
}

#if ZIFI_NET_CLIENT
// ─── Remote (FTP/SFTP) file browser ─────────────────────────────────────────
// Bounded RAM for any directory size: each listing is streamed straight into a
// sorted_files SD index (fixed 256-byte records, on-disk quicksort, dirs first),
// and only the visible window is read back per redraw — mirrors how the SD browser
// handles huge SD directories. No vector / no giant menu string in RAM.

// listStream callback: push one entry into the index (DIR_MARKER prefix = dir,
// so the on-disk sort groups directories first).
static void rfd_push(void* ctx, const char* name, bool isDir, uint32_t size) {
    (void)size;
    sorted_files* idx = (sorted_files*)ctx;
    std::string rec;
    if (isDir) rec += (char)DIR_MARKER;
    rec += name;
    idx->push(rec);
    // The listing fetch has no byte-level progress, so pulse the dialog's bar as
    // entries stream in — visible proof it's loading (not a frozen empty bar).
    static unsigned tick = 0;
    if ((++tick & 0x7) == 0)
        OSD::progressDialog("", "", (int)((tick >> 3) % 20) * 5, 1); // 0,5,…,95, wrap
}

// Transfer progress: update the dialog; Esc/F1 aborts.
static string rfd_xfer_title;
static bool rfd_progress(uint32_t done, uint32_t total) {
    // total == 0 = the transport never learned a length (a chunked source with no
    // Content-Length, e.g. archive.org's view_archive.php behind the TOSEC mirror;
    // HttpCatalogFs::get substitutes the listing's size where it has one). Pulse
    // the bar by bytes received instead of freezing it at 0% — same "it IS working"
    // signal rfd_push gives while a listing streams.
    int pct = total ? (int)((uint64_t)done * 100 / total)
                    : (int)((done >> 12) % 20) * 5;   // wrap every 4 KB, 0,5,…,95
    if (pct > 100) pct = 100;
    OSD::progressDialog(rfd_xfer_title, "", pct, 1);
    fabgl::VirtualKeyItem k;
    if (ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable() && ESPectrum::readKbd(&k))
        if (k.down && is_back(k.vk)) return false;
    return true;
}

// Row source for the picker below: uiPickListCb takes a plain callback, so the
// list it walks lives in these statics for the duration of the call.
static sorted_files*      s_rfd_idx;
static const char* const* s_rfd_synth;
static int                s_rfd_nsynth;

static void rfd_rowCb(int i, char* out, size_t n) {
    if (i < s_rfd_nsynth) { snprintf(out, n, "%s", s_rfd_synth[i]); return; }
    const string rec = s_rfd_idx->get(i - s_rfd_nsynth);
    if (!rec.empty() && (uint8_t)rec[0] == DIR_MARKER) snprintf(out, n, "%s/", rec.c_str() + 1);
    else                                               snprintf(out, n, "%s", rec.c_str());
}

// Shared windowed picker over an SD index, in the new UI chrome. Rows
// [0,nsynth) are the synthetic labels in `synth` (e.g. "[Select this folder]",
// ".."); rows [nsynth..) are idx entries (a leading DIR_MARKER byte = directory
// → shown with a "/" suffix). Only the visible window is read from the index
// per redraw, so RAM stays bounded. Returns the chosen absolute row index, or
// -1 on Esc.
static int rfd_scroll(const string& title, sorted_files& idx,
                      const char* const* synth, int nsynth, int initial = 0) {
    const int total = nsynth + (int)idx.size();
    if (total <= 0) return -1;
    s_rfd_idx = &idx; s_rfd_synth = synth; s_rfd_nsynth = nsynth;
    // Fixed geometry: the picker is reopened for every folder we descend into,
    // and a box sized to the row count would leave the taller previous one
    // showing around a shorter list (uiPickListCb saves no background). Width is
    // constant already — the title is clipped to the same WCH the rows use.
    constexpr int WCH = 36, VIS = 16;
    string t = title;
    if ((int)t.size() > WCH) t = "..." + t.substr(t.size() - (WCH - 3));
    return nm::uiPickListCb(t.c_str(), total, rfd_rowCb, initial, WCH, nullptr, VIS);
}

// Volume helpers for the local pickers. When both the SD card and a USB stick
// are present the pickers can cross between them ("USB:/..." paths; unprefixed
// = SD). In usbRoot mode the stick IS the only volume — no switching.
#define MSG_RFD_TO_USB "[USB Drive]"
#define MSG_RFD_TO_SD "[SD Card]"
static bool rfd_on_usb(const string& p) { return p.compare(0, 4, "USB:") == 0; }
static bool rfd_can_switch() { return UsbMsc::ready() && !FileUtils::usbRoot; }
// One level up; stays at the volume root ("/" or "USB:/").
static string rfd_parent(const string& cur) {
    size_t s = cur.find_last_of('/');
    if (rfd_on_usb(cur)) return (s == string::npos || s <= 4) ? "USB:/" : cur.substr(0, s);
    return (s == 0 || s == string::npos) ? "/" : cur.substr(0, s);
}
// A remembered start dir can be stale (e.g. "USB:/..." with the stick gone).
static string rfd_start_dir(const string& start) {
    string cur = start.empty() ? "/" : start;
    DIR dp;
    if (f_opendir(&dp, cur.c_str()) == FR_OK) f_closedir(&dp); else cur = "/";
    return cur;
}

// Pick a destination folder on the SD card / USB stick (for downloads).
// Navigates local directories with the same bounded-RAM scroller. Returns the
// chosen absolute path, or "" if cancelled.
static string rfd_choose_folder(const string& start) {
    string cur = rfd_start_dir(start);
    sorted_files idx;
    idx.init("__sdfolder__");
    while (1) {
        idx.unlink();
        DIR dp; FILINFO fno;
        if (f_opendir(&dp, cur.c_str()) == FR_OK) {
            while (f_readdir(&dp, &fno) == FR_OK && fno.fname[0]) {
                if (fno.fattrib & AM_DIR) {
                    string rec; rec += (char)DIR_MARKER; rec += fno.fname;
                    idx.push(rec);
                }
            }
            f_closedir(&dp);
        }
        idx.sort();
        const char* synth[3] = { "[Select this folder]", "..", nullptr };
        int ns = 2;
        if (rfd_can_switch())
            synth[ns++] = rfd_on_usb(cur) ? MSG_RFD_TO_SD : MSG_RFD_TO_USB;
        int sel = rfd_scroll(cur, idx, synth, ns);
        if (sel < 0)  { idx.unlink(); return ""; }   // cancel
        if (sel == 0) { idx.unlink(); return cur; }  // choose current
        if (sel == 1) {                              // parent
            string up = rfd_parent(cur);
            if (up == cur && rfd_on_usb(cur) && rfd_can_switch()) up = "/"; // ".." at USB:/ → SD root
            cur = up;
            continue;
        }
        if (sel == 2 && ns == 3) {                   // volume switch
            cur = rfd_on_usb(cur) ? "/" : "USB:/";
            continue;
        }
        string rec = idx.get(sel - ns);
        string name = (!rec.empty() && (uint8_t)rec[0] == DIR_MARKER) ? rec.substr(1) : rec;
        if (cur.back() != '/') cur += '/';
        cur += name;
    }
}

// Pick ANY file on the SD card (for uploads) — unlike the type-filtered browser, no extension
// filter, so the user can upload arbitrary files. Navigates dirs + files with
// the bounded-RAM scroller. Returns the chosen absolute file path, or "".
static string rfd_choose_file(const string& start) {
    string cur = rfd_start_dir(start);
    sorted_files idx;
    idx.init("__sdfile__");
    while (1) {
        idx.unlink();
        DIR dp; FILINFO fno;
        if (f_opendir(&dp, cur.c_str()) == FR_OK) {
            while (f_readdir(&dp, &fno) == FR_OK && fno.fname[0]) {
                string rec;
                if (fno.fattrib & AM_DIR) rec += (char)DIR_MARKER;
                rec += fno.fname;
                idx.push(rec);
            }
            f_closedir(&dp);
        }
        idx.sort();
        const char* synth[2] = { "..", nullptr };
        int ns = 1;
        if (rfd_can_switch())
            synth[ns++] = rfd_on_usb(cur) ? MSG_RFD_TO_SD : MSG_RFD_TO_USB;
        int sel = rfd_scroll(cur, idx, synth, ns);
        if (sel < 0)  { idx.unlink(); return ""; }   // cancel
        if (sel == 0) {                              // parent
            string up = rfd_parent(cur);
            if (up == cur && rfd_on_usb(cur) && rfd_can_switch()) up = "/"; // ".." at USB:/ → SD root
            cur = up;
            continue;
        }
        if (sel == 1 && ns == 2) {                   // volume switch
            cur = rfd_on_usb(cur) ? "/" : "USB:/";
            continue;
        }
        string rec = idx.get(sel - ns);
        bool isDir = (!rec.empty() && (uint8_t)rec[0] == DIR_MARKER);
        string name = isDir ? rec.substr(1) : rec;
        if (cur.back() != '/') cur += '/';
        if (isDir) { cur += name; continue; }
        idx.unlink();
        return cur + name; // a file → return its full path
    }
}

// FAT rejects \ / : * ? " < > | and control bytes in names — and catalog
// display names routinely carry them (game titles like "Gulliver Boy: …").
// Every SD path built from a catalog name goes through this, or f_mkdir/f_open
// fails and the whole download dies as a generic transfer error. Trailing
// dots/spaces go too (FatFs strips them itself, silently renaming the dir the
// rest of the copy then misses). UTF-8 continuation bytes (>= 0x80) pass.
static std::string rfd_fat_name(const std::string& n) {
    std::string s = n;
    for (char& c : s)
        if ((uint8_t)c < 0x20 || strchr("\\/:*?\"<>|", c)) c = '-';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? "-" : s;
}

// A downloaded .vgz is a gzip-wrapped .vgm (the format vgmrips.net serves VGM
// tracks in); the DivMMC VGM-player plugin reads plain .vgm, so unwrap right
// after the transfer and drop the wrapper. Best-effort: on a failed unwrap the
// .vgz stays as downloaded (the bytes are not lost, just still wrapped).
static void rfd_gunzip_vgz(std::string& path) {
    if (FileUtils::getLCaseExt(path) != "vgz") return;
    std::string out = path.substr(0, path.size() - 4) + ".vgm";
    if (!ZipExtract::gunzip(path, out).empty()) {
        f_unlink(path.c_str());
        path = out;
    }
}

// Collect a directory's entries into a vector (one recursion level at a time) —
// we can't recurse inside listStream's callback (channel reentrancy), so we
// enumerate first, then act. Bounded per-level (fine for typical folders).
static void rfd_copy_collect(void* ctx, const char* name, bool isDir, uint32_t sz) {
    (void)sz;
    auto v = (std::vector<std::string>*)ctx;
    std::string rec; if (isDir) rec += (char)DIR_MARKER; rec += name;
    v->push_back(rec);
}

// Recursively copy the CURRENT remote directory into the SD path `destSd`
// (created by the caller). cwd is restored to where it started. Returns false on
// error or user abort (Esc during a file).
static bool rfd_copy_tree(RemoteFs* fs, const std::string& destSd, int depth) {
    if (depth > 16) return false; // runaway guard
    std::vector<std::string> names;
    if (!fs->listStream("", rfd_copy_collect, &names)) return false;
    for (auto& rec : names) {
        bool isDir = (!rec.empty() && (uint8_t)rec[0] == DIR_MARKER);
        std::string nm = isDir ? rec.substr(1) : rec;
        std::string dst = destSd + "/" + rfd_fat_name(nm);
        if (isDir) {
            if (!FileUtils::mkdirParents(dst.c_str())) return false;
            if (!fs->cwd(nm)) return false;
            bool ok = rfd_copy_tree(fs, dst, depth + 1);
            fs->cwd("..");
            if (!ok) return false;
        } else {
            // Save under the real filename (catalog display names carry no extension);
            // for FTP/SFTP downloadBasename() returns the name unchanged.
            std::string fbase = fs->downloadBasename(nm);
            if (fbase.empty()) fbase = nm;
            std::string fdst = destSd + "/" + rfd_fat_name(fbase);
            rfd_xfer_title = MSG_NET_COPYING;
            OSD::progressDialog(rfd_xfer_title, nm, 0, 0, fs->utf8Names());
            bool got = fs->get(nm, fdst, rfd_progress);
            OSD::progressDialog("", "", 0, 2);
            if (!got) return false;
            rfd_gunzip_vgz(fdst);       // .vgz → ready .vgm beside the rest
        }
    }
    return true;
}

// Launch a file that was just downloaded to /tmp (Alt+Enter in the catalog
// browser). Tape images auto-run (flashload), snapshots auto-run, TR-DOS disk
// images mount into Drive A. A .zip is unpacked first and its first usable inner
// file launched. Returns true if something was loaded — the caller then closes
// the whole OSD so the freshly loaded program runs.
static bool rfd_launch_tmp(string path) {
    string ext = FileUtils::getLCaseExt(path);

    // A .vgz only needs its gzip wrapper removed — there is nothing to "launch"
    // (VGM playback is the guest-side DivMMC plugin's job), but Enter shouldn't
    // leave gzip bytes in /tmp either. F5 Save is the real path for VGM packs.
    if (ext == "vgz") {
        rfd_gunzip_vgz(path);
        return false;
    }

    // Downloaded archive: unpack to /tmp and launch the first usable inner file.
    if (ext == "zip") {
        string inner = ZipExtract::extract(path, DISK_ALLFILE); // → /tmp/...
        if (inner.empty() || inner == "\x1b") {
            OSD::osdCenteredMsg(ZipExtract::errMsg(), LEVEL_WARN);
            return false;
        }
        path = inner;
        ext  = FileUtils::getLCaseExt(path);
    }

    size_t slash = path.find_last_of('/');
    string dir   = (slash == string::npos) ? "/" : path.substr(0, slash + 1); // keep trailing '/'
    string base  = (slash == string::npos) ? path : path.substr(slash + 1);

    if (ext == "tap" || ext == "tzx" || ext == "pzx" || ext == "wav" || ext == "mp3") {
        FileUtils::TAP_Path = dir;
        Config::save();
        // Respect the Auto-start toggle:
        //   "R" = run (flashload if enabled), "L" = load only / never flashload.
        // WAV/MP3 are real audio and always start playing inside LoadTape.
        // Don't press Play here: leaving the tape STOPPED until the guest actually
        // polls (the runtime turbo/ROM heuristic then starts it) keeps F8 stats out
        // of tape mode while the tape is merely mounted, not loading.
        Tape::LoadTape((Config::tape_autostart ? "R" : "L") + base); // LoadTape prepends TAP_Path
        return true;
    }
    if (ext == "sna" || ext == "z80" || ext == "p") {
        FileUtils::SNA_Path = dir;
        Config::save();
        if (!LoadSnapshot(path, A_NONE, R_NONE)) {
            OSD::osdCenteredMsg(OSD_PSNA_LOAD_ERR, LEVEL_WARN);
            return false;
        }
        // /tmp snapshots are transient — don't pin them as the Alt+Backspace reload slot.
        Config::ram_file = NO_RAM_FILE;
        Config::last_ram_file = NO_RAM_FILE;
        return true;
    }
    if (FileUtils::ifaceForExt(ext) == IFACE_BETA) {
        FileUtils::DSK_Path = dir;
        Config::betadisk = true;       // ensure the TR-DOS controller is active for the mount
        rvmWD1793InsertDisk(&ESPectrum::fdd, 0, path);
        if (ESPectrum::fdd.disk[0])
            ESPectrum::fdd.disk[0]->writeprotect =
                Config::driveWP[0] || ESPectrum::fdd.disk[0]->IsTD0File;
        Config::save();
        OSD::bootTrdos();              // cold-boot into TR-DOS so the disk auto-runs
        return true;
    }
    if (ext == "rom" || ext == "bin") {
        return OSD::loadAlfCart(path); // ALF cartridge — lazy-mount from SD + switch into ALF
    }
    OSD::osdCenteredMsg(string(MSG_NET_UNSUPPORTED) + " (." + ext + ")",
                        LEVEL_WARN, 2200);
    return false;
}

// Quick-start always reuses a fixed /tmp/_run.<ext>. If a previous quick-start is
// still holding that exact file open — a disk mounted in the WD1793, or a tape still
// loaded — re-downloading into it (fopen2 FA_CREATE_ALWAYS over an open file) fails,
// which showed up as an empty progress bar that never advanced. Release the owner
// of `tmpp` first so the fresh download can truncate and rewrite it.
static void rfd_release_tmp(const string& tmpp) {
    for (int u = 0; u < 4; u++)
        if (ESPectrum::fdd.disk[u] && ESPectrum::fdd.disk[u]->fname == tmpp)
            wdDiskEject(&ESPectrum::fdd, u);
    if (Tape::tapeFileType != TAPE_FTYPE_EMPTY &&
        FileUtils::TAP_Path + Tape::tapeFileName == tmpp)
        Tape::Init();   // closes the open tape FIL
    // An ALF cart mounted lazily from this temp path holds the FIL open; release it
    // so the next quick-start can truncate/rewrite the same /tmp/_run.<ext> file.
    if (AlfCart::active() && AlfCart::path() == tmpp) AlfCart::unmount();
}

// ── Listing-index cache (Remote/Web) ─────────────────────────────────────────
// Cache key = FNV-1a of "cacheId()|cwdPath()" (per source, per folder), namespaced
// in /tmp so it survives reboot. Freshness policy: session-fresh (fetch a folder
// once per power session, reuse for the rest of the session) + a cheap revalidate
// for sources that support one (Web → HTTP 304) on the first visit after reboot +
// a manual F2 refresh. The session set is RAM-only, so a reboot invalidates it.
#define NET_CACHE_VERSION 2   // bump to invalidate stale idx (e.g. cached before the ".." row)
static uint32_t g_net_sess[128];
static int      g_net_sess_n = 0;
static bool netSessSeen(uint32_t h) {
    for (int i = 0; i < g_net_sess_n; i++) if (g_net_sess[i] == h) return true;
    return false;
}
static void netSessAdd(uint32_t h) {
    if (netSessSeen(h)) return;
    if (g_net_sess_n < (int)(sizeof(g_net_sess) / sizeof(g_net_sess[0]))) g_net_sess[g_net_sess_n++] = h;
}
static void netSessForget(uint32_t h) {
    for (int i = 0; i < g_net_sess_n; i++)
        if (g_net_sess[i] == h) { g_net_sess[i] = g_net_sess[--g_net_sess_n]; return; }
}
static uint32_t netHash(const std::string& s) {
    uint32_t h = 2166136261u ^ (uint32_t)NET_CACHE_VERSION;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}
static string netIdxName(uint32_t h) { char b[24]; snprintf(b, sizeof(b), "net_%08lx", (unsigned long)h); return b; }
static string netValPath(uint32_t h) { char b[40]; snprintf(b, sizeof(b), "/tmp/.net_%08lx.val", (unsigned long)h); return b; }
static string netValRead(uint32_t h) {
    FIL* f = fopen2(netValPath(h).c_str(), FA_READ);
    if (!f) return "";
    char buf[80]; UINT br = 0; f_read(f, buf, sizeof(buf) - 1, &br); fclose2(f); buf[br] = '\0';
    return string(buf);
}
static void netValWrite(uint32_t h, const string& v) {
    if (v.empty()) { f_unlink(netValPath(h).c_str()); return; }
    FIL* f = fopen2(netValPath(h).c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!f) return;
    UINT bw; f_write(f, v.data(), v.size(), &bw); fclose2(f);
}

// Session cursor memory for the net browser (RAM only, NOT persisted to config) — the
// cursor for the last folder shown, so F5 reopening that folder restores it (like SD's
// fileTypes slot persists the SD cursor across sessions). Keyed by the folder's path.
static int    g_net_cur_focus = 2, g_net_cur_begin = 2;
static string g_net_cur_path;

void OSD::remoteFileDialog(RemoteFs* fs) {
    // Read-only sources (online catalog) hide upload/delete and use the WEB sidebar.
    const bool ro = fs->readOnly();
    const int  side = ro ? FD_SIDE_WEB : FD_SIDE_REMOTE;
    const string title = MENU_ALL_TITLE;   // same "Open File" window as SD

    // Cursor memory (like the SD browser): curFocus/curBegin persist across re-lists of
    // the same folder; a small stack saves/restores the position when descending/ascending.
    int curFocus = 2, curBegin = 2;
    // Seed from the session memory when reopening the same folder (e.g. F5 restore) so the
    // cursor lands where it was, not at the top.
    if (fs->cwdPath() == g_net_cur_path) { curFocus = g_net_cur_focus; curBegin = g_net_cur_begin; }
    const int MAXDEPTH = 24;
    int stkF[MAXDEPTH], stkB[MAXDEPTH], stkN = 0;
    auto pushPos = [&]() { if (stkN < MAXDEPTH) { stkF[stkN] = curFocus; stkB[stkN] = curBegin; stkN++; } curFocus = curBegin = 2; };
    auto popPos  = [&]() { if (stkN > 0) { stkN--; curFocus = stkF[stkN]; curBegin = stkB[stkN]; } else { curFocus = curBegin = 2; } };

    while (1) {
        OSD::net_last_path = fs->cwdPath();   // remembered as the global last F5 location
        // Path shown in the header: prefix the remote cwd with the scheme label
        // ("FTP:"/"SSH:"/"WEB:") so fdDisplayPath() doesn't stamp it "SD:". Display
        // only — cache key / net_last_path / cursor memory still use the raw cwdPath.
        const string dispPath = fs->schemeLabel() + fs->cwdPath();
        // ── Open (or build) the per-folder listing index into the shared `filenames`
        // (so the SD render path can draw it). Reuse cache when known-fresh. ──
        uint32_t key = netHash(fs->cacheId() + "|" + fs->cwdPath());
        filenames.init(netIdxName(key));    // /tmp/.net_<hash>.idx (may already exist)
        bool haveCache = (filenames.size() > 0);
        bool reuse = false;
        if (haveCache && netSessSeen(key)) {
            reuse = true;                   // validated earlier this session → instant
        } else if (haveCache) {
            // First visit this session but a cache survived reboot — try a cheap
            // revalidate (Web → 304). FTP/SFTP return UNKNOWN → fall through to refetch.
            string stored = netValRead(key), fresh;
            reuse = (fs->revalidate("", stored, fresh) == RemoteFs::CACHE_FRESH);
        }
        if (!reuse) {
            OSD::progressDialog(MSG_NET_CONNECTING, dispPath, 0, 0, fs->utf8Names());
            filenames.unlink();          // truncate to empty (also (re)creates the file)
            // ".." row (double DIR_MARKER → sorts/renders first), like the SD browser:
            // select it to go up a level, and from the top it exits toward the root.
            filenames.push(string(2, (char)DIR_MARKER) + "..");
            bool ok = fs->listStream("", rfd_push, &filenames);
            OSD::progressDialog("", "", 0, 2);
            if (!ok) { OSD::osdCenteredMsg(MSG_NET_XFER_ERR, LEVEL_WARN, 2000); filenames.close(); return; }
            if (!fs->preSorted()) filenames.sort(); // pre-sorted (static catalog) skips the slow sort
            netValWrite(key, fs->lastValidator()); // persist this fetch's validator (Web; "" else)
        }
        netSessAdd(key);

        int outKey = FDK_ESC;
        int sel = fdChromeNav(title, dispPath, side, fs->utf8Names(), &outKey, &curFocus, &curBegin);
        // Remember this folder's cursor for the session (so F5 reopen restores it).
        g_net_cur_path = fs->cwdPath(); g_net_cur_focus = curFocus; g_net_cur_begin = curBegin;

        // ── Map the chrome's key/selection to an action ──
        if (sel < 0) {
            if (outKey == FDK_BACK) {                              // Backspace → parent (same as "..")
                string before = fs->cwdPath();
                fs->cwd("..");
                if (fs->cwdPath() == before) { filenames.close(); return; } // at root → leave
                popPos();                                          // restore the parent's cursor
                continue;
            }
            if (outKey == FDK_F7 && !ro) {                          // F7 → upload an SD file
                string local = rfd_choose_file(Config::net_ul_dir);
                if (!local.empty()) {
                    Config::net_ul_dir = rfd_parent(local);   // dirname, volume-aware
                    Config::saveWifiConfig();
                    string base = local.substr(local.find_last_of('/') + 1);
                    rfd_xfer_title = MSG_NET_UPLOADING;
                    OSD::progressDialog(rfd_xfer_title, base, 0, 0);
                    bool put_ok = fs->put(local, base, rfd_progress);
                    OSD::progressDialog("", "", 0, 2);
                    OSD::osdCenteredMsg(put_ok ? MSG_NET_XFER_OK : MSG_NET_XFER_ERR,
                                        put_ok ? LEVEL_INFO : LEVEL_WARN, 1800);
                    if (put_ok) { netSessForget(key); filenames.unlink(); netValWrite(key, ""); }
                }
                continue;
            }
            OSD::net_close_all = true;    // Esc → close the whole OSD (".."/Backspace climb)
            filenames.close(); return;
        }

        string rec = filenames.get(sel);
        string nm = rec;
        while (!nm.empty() && (uint8_t)nm[0] == DIR_MARKER) nm.erase(0, 1); // strip 1-2 markers
        bool isDir = (nm.size() < rec.size());

        if (outKey == FDK_F2) {           // F2 → force a re-fetch of the current dir
            netSessForget(key); filenames.unlink(); netValWrite(key, "");
            continue;
        }
        if (nm == "..") {                 // parent row — Enter goes up; from the top, exit
            if (outKey == FDK_ENTER) {
                string before = fs->cwdPath();
                fs->cwd("..");
                if (fs->cwdPath() == before) { filenames.close(); return; } // already at root → leave
                popPos();                 // restore the parent's cursor
            }
            continue;                     // ignore F5/F8/Alt on ".."
        }
        if (outKey == FDK_F8) {           // F8/Del → delete a remote entry
            if (OSD::msgDialog(nm, MSG_NET_DELETE_Q) == DLG_YES) {
                bool rok = fs->remove(nm, isDir);
                OSD::osdCenteredMsg(rok ? MSG_NET_XFER_OK : MSG_NET_XFER_ERR,
                                    rok ? LEVEL_INFO : LEVEL_WARN, 1500);
                if (rok) { netSessForget(key); filenames.unlink(); netValWrite(key, ""); }
            }
            continue;
        }
        if (outKey == FDK_F5) {           // F5 → save file/folder to a chosen SD folder
            string destBase = rfd_choose_folder(Config::net_dl_dir);
            if (!destBase.empty()) {
                Config::net_dl_dir = destBase;
                Config::saveWifiConfig();
                bool ok;
                if (isDir) {                 // recursive folder copy (keep the dir name)
                    string dst = destBase + (destBase.back() == '/' ? "" : "/") + rfd_fat_name(nm);
                    ok = FileUtils::mkdirParents(dst.c_str());
                    if (ok) {
                        fs->cwd(nm);
                        ok = rfd_copy_tree(fs, dst, 0);
                        fs->cwd("..");
                    }
                } else {                     // single file — use the real filename (catalog
                                             // display names carry no extension)
                    rfd_xfer_title = MSG_NET_DOWNLOADING;
                    OSD::progressDialog(rfd_xfer_title, nm, 0, 0, fs->utf8Names()); // show first
                    string base = fs->downloadBasename(nm);   // (catalog: HTTP listing read)
                    if (base.empty()) base = nm;
                    string dst = destBase + (destBase.back() == '/' ? "" : "/") + rfd_fat_name(base);
                    ok = fs->get(nm, dst, rfd_progress);
                    OSD::progressDialog("", "", 0, 2);
                    if (ok) rfd_gunzip_vgz(dst);   // .vgz → ready .vgm on the SD
                }
                OSD::osdCenteredMsg(ok ? MSG_NET_XFER_OK : MSG_NET_XFER_ERR,
                                    ok ? LEVEL_INFO : LEVEL_WARN, 1800);
            }
            continue;
        }

        // Enter on the selected row.
        if (isDir) { pushPos(); fs->cwd(nm); continue; } // descend → save cursor, start at top

        // Plain Enter: quick start — download to /tmp and launch. Always use a fixed
        // name (per extension) so /tmp isn't filled with one file per launch — it's
        // overwritten each time. The extension is kept (rfd_launch_tmp needs it to tell
        // the file type). For a real filename, use F5 Save instead.
        rfd_xfer_title = MSG_NET_DOWNLOADING;
        OSD::progressDialog(rfd_xfer_title, nm, 0, 0, fs->utf8Names());
        // Extension straight from the display name only when the suffix is a *known*
        // launchable extension — avoids an extra request. Catalog titles routinely
        // carry dots mid-name (version tags like "Z-Player v3.4", or "...3; Demo
        // (SL+SSROM)"), and a bare "short alphanumeric suffix" test wrongly takes ".4"
        // as the type. Anything unrecognised falls back to downloadBasename() (the real
        // name from the locator), which is the authoritative source of the extension.
        auto isLaunchExt = [](const string& lc) {
            return lc == "tap" || lc == "tzx" || lc == "pzx" || lc == "wav" || lc == "mp3"
                || lc == "sna" || lc == "z80" || lc == "p"   || lc == "zip"
                || lc == "rom" || lc == "bin"
                || FileUtils::ifaceForExt(lc) != IFACE_NONE; // trd/scl/fdi/udi/td0/pro/mbd/mmc/hdf
        };
        string ext;
        size_t slash = nm.find_last_of('/'), dot = nm.find_last_of('.');
        if (dot != string::npos && (slash == string::npos || dot > slash) &&
            isLaunchExt(FileUtils::getLCaseExt(nm)))
            ext = nm.substr(dot);                          // includes the '.'
        if (ext.empty()) {
            string base = fs->downloadBasename(nm);
            size_t d2 = base.find_last_of('.');
            if (d2 != string::npos) ext = base.substr(d2);
        }
        string tmpp = string("/tmp/_run") + ext;
        rfd_release_tmp(tmpp);   // free the fixed /tmp target if a prior launch still holds it
        bool got = fs->get(nm, tmpp, rfd_progress);
        OSD::progressDialog("", "", 0, 2);
        if (!got) {
            OSD::osdCenteredMsg(MSG_NET_XFER_ERR, LEVEL_WARN, 2000);
        } else {
            OSD::osdCenteredMsg(MSG_NET_LAUNCHING, LEVEL_INFO, 400);
            if (rfd_launch_tmp(tmpp)) {
                OSD::net_launch_close = true; // signal the menu stack to close
                filenames.close();
                return;                       // keep the listing cache on disk
            }
        }
    }
}
#endif // ZIFI_NET_CLIENT
