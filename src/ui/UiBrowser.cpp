// pico-speccy — fullscreen file browser of the new UI (see UiBrowser.h).
//
//  +-----------------------------------------------------+
//  | [rainbow] Open file                          12/345 |  header
//  |-----------------------------------------------------|
//  | SD:/spec/games/                                     |  path bar
//  |----------------------------------+------------------|
//  | .. (up)                        » | game.tap         |  info pane: full name,
//  | demos                          » | Tape image       |  size, date, then the
//  | GAME.TAP                       * | 48,562 bytes     |  verbs that apply to
//  | readme.txt                       | 2026-07-12 14:03 |  the focused entry
//  |   ...                            | ⏎ Open  F1 Info  |
//  |----------------------------------+------------------|
//  | ^v Move  ⏎ Open  << Up  F3 Find  Esc Close          |  footer / search field
//  +-----------------------------------------------------+
//
// Directory data comes from the shared sorted_files index (SortedFiles.h): a flat
// file under /tmp, one record per name, dirs first (DIR_MARKER prefix). Nothing
// here allocates proportionally to the directory size — pages are fetched row by
// row from the index, exactly like the classic dialog, so a 5000-file directory
// costs the same RAM as an empty one.

#include "OSDNewMenu.h"


#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <string>
#include <vector>

#include "UiBrowser.h"
#include "UiGfx.h"
#include "UiFont.h"
#include "UiDialog.h"
#include "OSDMain.h"
#include "UiRender.h"        // SYM_* glyph names
#include "FileUtils.h"
#include "FileInfo.h"
#include "ZipExtract.h"
#include "Config.h"
#include "ESPectrum.h"
#include "Video.h"
#include "wd1793.h"
#include "PinSerialData_595.h"
#include "Debug.h"
#include "SortedFiles.h"
#include <pico/stdlib.h>

using std::string;

namespace nm {

// ── state ──────────────────────────────────────────────────────────────────────

static sorted_files s_idx;

static string  s_dir;             // current directory, always ends with '/'
static string  s_title;
static uint8_t s_ftype;
static std::vector<string> s_exts;   // ".tap", ".sna", ... (with the dot)

static int  s_total;              // records in the index (incl. the ".." row)
static unsigned s_ndirs, s_nfiles;
static int  s_visTotal;           // rows the filter lets through
static int  s_sel, s_top;         // selection / first visible row (vis domain)
static bool s_search;             // incremental filter active
static string s_query;

static string s_goto;             // name to land on after a rescan

// Marquee for an over-long focused name: after ~1 s idle the visible window
// slides one character every ~200 ms, holds on the tail, restarts. Any keypress
// resets it. The focused name is cached here so the idle tick never re-reads
// the index from SD.
static std::string s_mq_name;     // display form of the focused row
static int s_mq_avail;            // its pixel budget in the list column
static int s_mq_off, s_mq_idle, s_mq_tick;

static void mqReset() { s_mq_off = 0; s_mq_idle = 0; s_mq_tick = 0; }

// Advance the marquee during an idle tick; true = the focused row needs repaint.
static bool mqTick() {
    const int fits = s_mq_avail / glyphW();
    if (fits <= 0 || (int)s_mq_name.size() <= fits) return false;
    if (s_mq_idle < 200) { s_mq_idle++; return false; }   // ~1 s before it starts
    if (++s_mq_tick < 40) return false;                   // ~200 ms per step
    s_mq_tick = 0;
    if ((int)s_mq_name.size() - s_mq_off <= fits) {       // tail shown: hold, restart
        s_mq_off = 0;
        s_mq_idle = 0;
    } else {
        s_mq_off++;
    }
    return true;
}

// Draw a (possibly scrolled) focused name: hard window, no ".." while sliding.
static void mqText(int x, int y, int availw, const string& nm_, UiColor ink) {
    if (s_mq_off > 0) {
        const int fits = availw / glyphW();
        string vis = (int)nm_.size() > s_mq_off ? nm_.substr((size_t)s_mq_off) : nm_;
        if ((int)vis.size() > fits) vis.resize((size_t)fits);
        text(x, y, vis.c_str(), ink);
    } else {
        textClip(x, y, availw, nm_.c_str(), ink);
    }
}

// Directory position stack, so backing out of a folder restores the cursor.
static constexpr int POS_STACK_MAX = 16;
static struct { int top, sel; } s_stack[POS_STACK_MAX];
static int s_stack_top;

// ── layout ─────────────────────────────────────────────────────────────────────

struct BrowserLayout {
    int ix, iy, iw, ih;           // window content rect
    int margin;
    int hdr_h, path_h, foot_h;
    int body_y, body_h, row_h, rows;
    int lx, lw;                   // list pane
    int sep_x;
    int rx, rw;                   // info pane
    int pad;
};
static BrowserLayout L;

static void computeBL() {
    const int sc = Sf.glyphScale;
    L.pad    = 2 * sc;
    L.margin = 2 * sc;
    L.ix = L.margin + sc;
    L.iy = 3;
    L.iw = Sf.w - 2 * L.ix;
    L.ih = Sf.h - L.iy - 3;

    L.hdr_h  = UI_FONT_H + 6;
    L.path_h = UI_FONT_H + 4;
    L.foot_h = UI_FONT_H + 4;
    L.row_h  = UI_FONT_H + 2;

    L.body_y = L.iy + L.hdr_h + L.path_h;
    L.body_h = L.iy + L.ih - L.foot_h - L.body_y;
    L.rows   = L.body_h / L.row_h;

    // Info pane: a fixed character budget; the list takes the rest.
    const int ichars = (cols() >= 52) ? 17 : 14;
    L.rw    = ichars * glyphW() + 2 * L.pad;
    L.rx    = L.ix + L.iw - L.rw;
    L.sep_x = L.rx - sc;
    L.lx    = L.ix;
    L.lw    = L.sep_x - L.lx;
}

// ── filter (incremental search) ────────────────────────────────────────────────
// Directories always pass; files pass on a case-insensitive substring match.
// Same semantics as the classic fdMode search, over the same index.

static bool nameMatches(const string& rec) {
    if (s_query.empty() || (uint8_t)rec[0] == DIR_MARKER) return true;
    char up[FF_LFN_BUF + 1];
    size_t i = 0;
    for (; i < rec.size() && i < FF_LFN_BUF; i++) up[i] = (char)toupper((uint8_t)rec[i]);
    up[i] = 0;
    return strstr(up, s_query.c_str()) != nullptr;
}

static void recountVisible() {
    if (s_query.empty()) { s_visTotal = s_total; return; }
    s_visTotal = 0;
    for (int i = 0; i < s_total; i++)
        if (nameMatches(s_idx.get(i))) s_visTotal++;
}

// vis row -> index record. Identity without a filter; a linear scan with one.
static int visToIdx(int v) {
    if (v < 0 || v >= s_visTotal) return -1;
    if (s_query.empty()) return v;
    int seen = 0;
    for (int i = 0; i < s_total; i++)
        if (nameMatches(s_idx.get(i)))
            if (seen++ == v) return i;
    return -1;
}

static string visName(int v) {
    const int i = visToIdx(v);
    return i < 0 ? string() : s_idx.get(i);
}

// ── small helpers ──────────────────────────────────────────────────────────────

static string displayPath(const string& fdir) {
    if (fdir.find(':') != string::npos) return fdir;
    return (FileUtils::usbRoot ? "USB:" : "SD:") + fdir;
}

static bool isDirRec(const string& rec)  { return !rec.empty() && (uint8_t)rec[0] == DIR_MARKER; }
static bool isUpRec(const string& rec)   { return rec.size() > 1 && (uint8_t)rec[0] == DIR_MARKER
                                                              && (uint8_t)rec[1] == DIR_MARKER; }
static string plainName(const string& rec) {
    size_t i = 0;
    while (i < rec.size() && (uint8_t)rec[i] == DIR_MARKER) i++;
    return rec.substr(i);
}

static bool extMatches(const string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == string::npos) return false;
    const string e = name.substr(dot);
    for (const auto& x : s_exts) if (e == x) return true;
    return false;
}

static const char* typeLabel(const string& lcext) {
    if (lcext == "tap" || lcext == "tzx" || lcext == "pzx") return "Tape image";
    if (lcext == "wav" || lcext == "mp3")                   return "Audio tape";
    if (lcext == "sna" || lcext == "z80" || lcext == "p")   return "Snapshot";
    if (lcext == "trd" || lcext == "scl" || lcext == "fdi"
                       || lcext == "udi")                   return "TR-DOS disk";
    if (lcext == "mbd")                                     return "MB-02 disk";
    if (lcext == "dsk")                                     return "+3 disk";
    if (lcext == "mmc" || lcext == "hdf")                   return "esxDOS image";
    if (lcext == "rom" || lcext == "bin")                   return "ROM image";
    if (lcext == "zip")                                     return "ZIP archive";
    if (lcext == "scr")                                     return "ZX screen";
    if (lcext == "dls")                                     return "DLS soundbank";
    return "File";
}

// ── drawing ────────────────────────────────────────────────────────────────────

static void drawChrome() {
    fill(0, 0, Sf.w, Sf.h, C_BG);
    roundRect(L.margin, L.iy - 1, Sf.w - 2 * L.margin, L.ih + 2, 4, C_SEP, C_PANEL);
    vline(L.sep_x, L.body_y, L.body_h, C_SEP);
}

static void drawHeader() {
    const int y = L.iy;
    fill(L.ix, y, L.iw, L.hdr_h, C_PANEL);
    rainbow(L.ix + L.pad, y + 3);
    int tx = L.ix + L.pad + rainbowW() + 2 * L.pad;
    tx += text(tx, y + 4, s_title.c_str(), C_WHITE);
    char cnt[20];
    if (s_visTotal > 0)
        snprintf(cnt, sizeof(cnt), "%d/%d", s_sel + 1, s_visTotal);
    else
        snprintf(cnt, sizeof(cnt), "empty");
    const int cx = L.ix + L.iw - textWidth(cnt) - L.pad;
    text(cx, y + 4, cnt, C_TEXT_DIM);
    uiHeaderClock(L.ix, L.iw, y + 4, tx + 2 * L.pad, cx - 2 * L.pad);
    hline(L.ix, y + L.hdr_h - 1, L.iw, C_SEP);
}

static void drawPathBar() {
    const int y = L.iy + L.hdr_h;
    fill(L.ix, y, L.iw, L.path_h, C_PANEL_ALT);
    const string p = displayPath(s_dir);
    // Long paths keep their tail (the deepest dirs are what orients the user).
    const int fits = (L.iw - 2 * L.pad) / glyphW();
    if ((int)p.size() > fits && fits > 2)
        text(L.ix + L.pad, y + 2, (".." + p.substr(p.size() - fits + 2)).c_str(), C_TEXT);
    else
        text(L.ix + L.pad, y + 2, p.c_str(), C_TEXT);
    hline(L.ix, y + L.path_h - 1, L.iw, C_SEP);
}

static void drawListRow(int visRow) {
    if (visRow < 0 || visRow >= L.rows) return;
    const int y = L.body_y + visRow * L.row_h;
    const int v = s_top + visRow;
    const bool sel = (v == s_sel) && (v < s_visTotal);

    // Room for the scrollbar hairline at the right edge of the pane.
    const int sb = (s_visTotal > L.rows) ? 2 * Sf.glyphScale : 0;
    fill(L.lx, y, L.lw - sb, L.row_h, sel ? C_SEL_BG : C_PANEL);
    if (v >= s_visTotal) return;

    const string rec = visName(v);
    if (rec.empty()) return;
    const bool dir = isDirRec(rec);
    string nm_ = isUpRec(rec) ? ".." : plainName(rec);

    UiColor ink;
    if (sel)        ink = C_WHITE;
    else if (dir)   ink = C_TEXT;
    else            ink = extMatches(nm_) ? C_TEXT : C_TEXT_DIM;

    const int markW = dir ? (chevronW() + L.pad) : 0;
    const int availw = L.lw - sb - 2 * L.pad - markW;
    if (sel) {
        s_mq_name = nm_;                  // cache for the idle marquee tick
        s_mq_avail = availw;
        mqText(L.lx + L.pad, y + 1, availw, nm_, ink);
    } else {
        textClip(L.lx + L.pad, y + 1, availw, nm_.c_str(), ink);
    }
    if (dir)
        chevron(L.lx + L.lw - sb - L.pad - chevronW(), y + L.row_h / 2 - 3,
                sel ? C_WHITE : C_DISABLED);
}

static void drawScrollbar() {
    if (s_visTotal <= L.rows) return;
    const int sc = Sf.glyphScale;
    const int x = L.sep_x - 2 * sc;
    fill(x, L.body_y, sc, L.body_h, C_PANEL_ALT);
    int th = L.body_h * L.rows / s_visTotal;
    if (th < 6) th = 6;
    const int span = L.body_h - th;
    const int maxTop = s_visTotal - L.rows;
    const int ty = L.body_y + (maxTop > 0 ? span * s_top / maxTop : 0);
    fill(x, ty, sc, th, C_DISABLED);
}

static void drawList() {
    for (int r = 0; r < L.rows; r++) drawListRow(r);
    drawScrollbar();
}

// The info pane: everything known about the focused entry, then the verbs.
static void drawInfo() {
    const int x = L.rx, w = L.rw;
    fill(x, L.body_y, w, L.body_h, C_PANEL);
    int y = L.body_y + 2;
    const int lh = UI_FONT_H + 2;
    const int tx = x + L.pad, tw = w - 2 * L.pad;
    const int fits = tw / glyphW();

    const string rec = visName(s_sel);
    if (rec.empty()) {
        text(tx, y, "No files", C_TEXT_DIM);
        return;
    }
    const bool dir = isDirRec(rec);
    const bool up  = isUpRec(rec);
    const string nm_ = up ? ".." : plainName(rec);

    // Full name, wrapped over up to 3 lines (the list may have clipped it).
    for (int ln = 0; ln < 3 && ln * fits < (int)nm_.size(); ln++) {
        text(tx, y, nm_.substr((size_t)ln * fits, fits).c_str(), C_WHITE);
        y += lh;
    }
    y += 2;

    if (up) {
        text(tx, y, "Parent folder", C_TEXT_DIM);
    } else if (dir) {
        text(tx, y, "Folder", C_TEXT_DIM);
    } else {
        const string lc = FileUtils::getLCaseExt(nm_);
        text(tx, y, typeLabel(lc), extMatches(nm_) ? C_ACCENT : C_TEXT_DIM);
        y += lh;
        FILINFO fi;
        if (f_stat((s_dir + nm_).c_str(), &fi) == FR_OK) {
            char buf[24];
            if (fi.fsize < 10000)
                snprintf(buf, sizeof(buf), "%u bytes", (unsigned)fi.fsize);
            else if (fi.fsize < 10u * 1024 * 1024)
                snprintf(buf, sizeof(buf), "%u KB", (unsigned)(fi.fsize >> 10));
            else
                snprintf(buf, sizeof(buf), "%u MB", (unsigned)(fi.fsize >> 20));
            text(tx, y, buf, C_TEXT);
            y += lh;
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                     (fi.fdate >> 9) + 1980, (fi.fdate >> 5) & 15, fi.fdate & 31,
                     fi.ftime >> 11, (fi.ftime >> 5) & 63);
            text(tx, y, buf, C_TEXT_DIM);
        }
    }
    y += lh + 3;

    // Verbs from the bottom up would jump around; a fixed block reads better.
    hline(tx, y, tw, C_SEP);
    y += 3;
    struct Verb { const char* k; const char* what; bool on; };
    const bool all = (s_ftype == DISK_ALLFILE);
    const bool zip = !dir && FileUtils::hasZIPextension(nm_);
    const bool dsk = !dir && FileUtils::ifaceForExt(FileUtils::getLCaseExt(nm_)) != IFACE_NONE;
    const Verb verbs[] = {
        { SYM_ENTER, dir ? "Open" : "Run",  true },
        { "F1", "Info",     all && !dir },
        { "F3", "Find",     true },
        { "F4", "Unzip",    all && zip },
        { "F5", "To slot",  all && dsk },
        { "F6", "Rename",   all && !up },
        { "F7", "New dir",  all },
        { "F8", "Delete",   all && !up },
        { "F9", "New TRD",  all },
    };
    for (const auto& vb : verbs) {
        if (!vb.on) continue;
        if (y + lh > L.body_y + L.body_h) break;
        const int kw = textWidth(vb.k);
        text(tx, y, vb.k, C_TEXT_DIM);
        text(tx + kw + glyphW(), y, vb.what, C_TEXT);
        y += lh;
    }
}

static void drawFooter() {
    const int y = L.iy + L.ih - L.foot_h;
    fill(L.ix, y, L.iw, L.foot_h, C_FOOT_BG);
    hline(L.ix, y, L.iw, C_SEP);
    if (s_search) {
        const int lx = L.ix + L.pad;
        text(lx, y + 3, "Find:", C_ACCENT);
        const int qx = lx + 6 * glyphW();
        string q = s_query + "_";
        textClip(qx, y + 3, L.ix + L.iw - qx - L.pad, q.c_str(), C_WHITE);
    } else {
        text(L.ix + L.pad, y + 3,
             SYM_UP SYM_DOWN " Move  " SYM_ENTER " Open  " SYM_LEFT " Up  F3 Find  Esc Close",
             C_TEXT_DIM);
    }
}

// Bands square the window corners off — the border re-pass restores them.
static void frameFix() {
    roundRectBorder(L.margin, L.iy - 1, Sf.w - 2 * L.margin, L.ih + 2, 4, C_SEP, C_BG);
}

static void drawAll() {
    drawChrome();
    drawHeader();
    drawPathBar();
    drawList();
    drawInfo();
    drawFooter();
    frameFix();
}

// A transient status line over the footer (indexing / deleting progress).
static void status(const char* msg, int percent) {
    const int y = L.iy + L.ih - L.foot_h;
    fill(L.ix, y + 1, L.iw, L.foot_h - 1, C_FOOT_BG);
    text(L.ix + L.pad, y + 3, msg, C_TEXT);
    if (percent >= 0) {
        const int bx = L.ix + L.iw / 2, bw = L.iw / 2 - 2 * L.pad;
        frame(bx, y + 3, bw, UI_FONT_H - 2, C_SEP);
        fill(bx + 1, y + 4, (bw - 2) * percent / 100, UI_FONT_H - 4, C_ACCENT);
    }
}

// ── indexing ───────────────────────────────────────────────────────────────────
// Same cache discipline as the classic dialog: count + content-CRC first, rebuild
// the .idx only when the directory really changed. F1 aborts a long pass.

static bool abortKey() {
    if (!ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) return false;
    return get_last_key_pressed() == fabgl::VirtualKey::VK_F1;
}

static bool indexCurrent() {
    Debug::log("UiBrowser: index '%s'\n", s_dir.c_str());
    s_idx.init(s_dir);
    Debug::log("UiBrowser: idx open=%d cached=%u\n", (int)s_idx.is_open(), (unsigned)s_idx.size());
    s_ndirs = 0; s_nfiles = 0;

    DIR d;
    FILINFO fi;
    if (f_opendir(&d, s_dir.c_str()) != FR_OK) {
        Debug::log("UiBrowser: f_opendir failed\n");
        return false;
    }

    status("Scanning...", -1);
    size_t crc = SORT_VERSION;
    const bool withUp = s_dir.size() > 1 || OSD::fd_root_parent;
    if (withUp) { s_ndirs++; crc += sf_name_crc(string(2, DIR_MARKER) + ".."); }

    FRESULT frd;
    while ((frd = f_readdir(&d, &fi)) == FR_OK && fi.fname[0] != '\0') {
        if (abortKey()) break;
        const string fname = fi.fname;
        if (fname.compare(0, 1, ".") == 0) continue;
        if (fi.fattrib & AM_DIR) { s_ndirs++;  crc += sf_name_crc(char(DIR_MARKER) + fname); }
        else                     { s_nfiles++; crc += sf_name_crc(fname); }
    }
    f_closedir(&d);
    if (frd != FR_OK)
        Debug::log("UiBrowser: f_readdir('%s') err=%d\n", s_dir.c_str(), frd);
    Debug::log("UiBrowser: counted %u dirs %u files\n", s_ndirs, s_nfiles);

    const size_t idx_crc = s_idx.crc();
    Debug::log("UiBrowser: crc live=%u idx=%u -> %s\n", (unsigned)crc, (unsigned)idx_crc,
               idx_crc != crc ? "REBUILD" : "cached");
    if (idx_crc != crc) {                        // rebuild
        s_idx.unlink();
        if (withUp) s_idx.push(string(2, DIR_MARKER) + "..");
        if (f_opendir(&d, s_dir.c_str()) != FR_OK) return false;
        const unsigned expect = s_ndirs + s_nfiles;
        unsigned done = 0;
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0') {
            if (abortKey()) break;
            const string fname = fi.fname;
            if (fname.compare(0, 1, ".") == 0) continue;
            s_idx.push((fi.fattrib & AM_DIR) ? char(DIR_MARKER) + fname : fname);
            if ((++done & 15) == 0 && expect)
                status("Indexing...", (int)(done * 90u / expect));
        }
        f_closedir(&d);
        Debug::log("UiBrowser: pushed %u records, sorting\n", (unsigned)s_idx.size());
        status("Sorting...", 95);
        s_idx.sort();
        Debug::log("UiBrowser: sort done\n");
    }
    s_total = (int)s_idx.size();
    Debug::log("UiBrowser: total=%d\n", s_total);
    return true;
}

// Land the cursor on s_goto (or its alphabetic neighbour when it was deleted).
static void applyGoto() {
    if (s_goto.empty()) return;
    int found = -1;
    for (int i = 0; i < s_total; i++) {
        if (s_idx.get(i) == s_goto) { found = i; break; }
    }
    if (found < 0) {
        found = 0;
        for (int i = 0; i < s_total; i++) {
            if (s_idx.get(i) >= s_goto) { found = i; break; }
            found = i;
        }
    }
    s_goto.clear();
    // The goto target is an index position; with no filter vis == idx (a rescan
    // clears the filter only when the user left search mode, so map it).
    if (!s_query.empty()) {
        int v = 0;
        for (int i = 0; i < found && i < s_total; i++)
            if (nameMatches(s_idx.get(i))) v++;
        found = v;
    }
    s_sel = found < s_visTotal ? found : (s_visTotal ? s_visTotal - 1 : 0);
}

static void clampView() {
    if (s_sel >= s_visTotal) s_sel = s_visTotal ? s_visTotal - 1 : 0;
    if (s_sel < 0) s_sel = 0;
    if (s_top > s_sel) s_top = s_sel;
    if (s_sel >= s_top + L.rows) s_top = s_sel - L.rows + 1;
    const int maxTop = s_visTotal - L.rows;
    if (s_top > maxTop) s_top = maxTop > 0 ? maxTop : 0;
    if (s_top < 0) s_top = 0;
}

// ── movement ───────────────────────────────────────────────────────────────────

static void moveSel(int delta) {
    if (!s_visTotal) return;
    int ns = s_sel + delta;
    if (ns < 0) ns = 0;
    if (ns > s_visTotal - 1) ns = s_visTotal - 1;
    if (ns == s_sel) return;
    const int oldTop = s_top, oldSel = s_sel;
    s_sel = ns;
    clampView();
    if (s_top != oldTop) drawList();
    else { drawListRow(oldSel - s_top); drawListRow(s_sel - s_top); drawScrollbar(); }
    drawHeader();
    drawInfo();
}

// ── modal helpers ──────────────────────────────────────────────────────────────

// Ask for a line of text in the footer. Returns false on Esc/empty.
static bool footerAsk(const char* label, string& io) {
    const int y = L.iy + L.ih - L.foot_h;
    fill(L.ix, y + 1, L.iw, L.foot_h - 1, C_FOOT_BG);
    text(L.ix + L.pad, y + 3, label, C_ACCENT);
    const int fx = L.ix + L.pad + (int)(strlen(label) + 1) * glyphW();
    const bool ok = uiEditLine(fx, y + 3, L.ix + L.iw - fx - L.pad, io, FF_LFN_BUF - 8);
    drawFooter();
    return ok && !io.empty();
}

// ── main loop ──────────────────────────────────────────────────────────────────

static string runLoop() {
    DISK_FTYPE& ft = FileUtils::fileTypes[s_ftype];

    // Adopt the classic position memory (rows there are offset by 2).
    s_top    = ft.begin_row - 2; if (s_top < 0) s_top = 0;
    s_sel    = s_top + (ft.focus - 2); if (s_sel < 0) s_sel = 0;
    s_search = ft.fdMode != 0;
    s_query  = ft.fileSearch;
    for (auto& c : s_query) c = (char)toupper((uint8_t)c);
    s_stack_top = 0;
    s_goto.clear();

    // One exit point keeps the position write-back and index close together.
    auto leave = [&](const string& r) -> string {
        ft.begin_row  = s_top + 2;
        ft.focus      = s_sel - s_top + 2;
        ft.fdMode     = s_search ? 1 : 0;
        ft.fileSearch = s_query;
        s_idx.close();
        return r;
    };

    while (1) {                                       // per directory
        DIR probe;
        if (f_opendir(&probe, s_dir.c_str()) != FR_OK) {
            Debug::log("UiBrowser: cannot open '%s', falling back to /\n", s_dir.c_str());
            s_dir = "/";
            s_sel = s_top = 0;
            if (f_opendir(&probe, s_dir.c_str()) != FR_OK) return leave("");
        }
        f_closedir(&probe);

        drawAll();
        if (!indexCurrent()) return leave("");
        recountVisible();
        applyGoto();
        clampView();
        mqReset();
        drawAll();
        Debug::log("UiBrowser: ready vis=%d sel=%d top=%d\n", s_visTotal, s_sel, s_top);

        fabgl::VirtualKeyItem k;
        while (1) {                                   // keys
            if (!ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
                sleep_ms(5);
                if (mqTick()) drawListRow(s_sel - s_top);
                if (uiClockDirty()) drawHeader();
                continue;
            }
            if (!ESPectrum::readKbd(&k) || !k.down) continue;
            if (s_mq_off) { mqReset(); drawListRow(s_sel - s_top); }
            else mqReset();

            const string rec = visName(s_sel);
            const bool onDir  = isDirRec(rec);
            const bool onUp   = isUpRec(rec);
            const string name = onUp ? ".." : plainName(rec);

            // Movement keys are too chatty; everything else is worth a watermark.
            switch (k.vk) {
                case fabgl::VK_MENU_UP: case fabgl::VK_MENU_DOWN:
                case fabgl::VK_PAGEUP:  case fabgl::VK_PAGEDOWN:
                case fabgl::VK_MENU_HOME: case fabgl::VK_HOME: case fabgl::VK_END:
                    break;
                default:
                    Debug::log("UiBrowser: key vk=%d sp=%08x\n", (int)k.vk, debug_sp());
                    break;
            }

            switch (k.vk) {
                case fabgl::VK_MENU_UP:    moveSel(-1); OSD::clickNoPause(); continue;
                case fabgl::VK_MENU_DOWN:  moveSel(+1); OSD::clickNoPause(); continue;
                case fabgl::VK_PAGEUP:     moveSel(-L.rows); OSD::clickNoPause(); continue;
                case fabgl::VK_PAGEDOWN:   moveSel(+L.rows); OSD::clickNoPause(); continue;
                case fabgl::VK_MENU_HOME:
                case fabgl::VK_HOME:       moveSel(-s_visTotal); OSD::clickNoPause(); continue;
                case fabgl::VK_END:        moveSel(+s_visTotal); OSD::clickNoPause(); continue;
                default: break;
            }

            // ── going up: Left, Backspace, or Enter/Right on ".." ──────────────
            const bool wantUp =
                (k.vk == fabgl::VK_MENU_LEFT) ||
                (k.vk == fabgl::VK_MENU_BS && !s_search) ||
                ((k.vk == fabgl::VK_MENU_ENTER || k.vk == fabgl::VK_MENU_RIGHT) && onUp);
            if (wantUp) {
                Debug::log("UiBrowser: UP from '%s' (stack=%d)\n", s_dir.c_str(), s_stack_top);
                OSD::clickNoPause();
                if (s_dir == "/" || s_dir == "USB:/") {
                    if (OSD::fd_root_parent) return leave("\x02UP");
                    if (s_dir == "/") continue;               // nowhere higher
                    // Out of the stick to the SD root (per-type dialog case).
                    s_dir = "/";
                } else {
                    s_dir.pop_back();
                    s_dir = s_dir.substr(0, s_dir.find_last_of('/') + 1);
                }
                if (s_stack_top > 0) { s_top = s_stack[--s_stack_top].top; s_sel = s_stack[s_stack_top].sel; }
                else { s_top = s_sel = 0; }
                Debug::log("UiBrowser: UP -> '%s' top=%d sel=%d\n", s_dir.c_str(), s_top, s_sel);
                break;                                // rescan the parent
            }

            // ── enter: descend or pick ─────────────────────────────────────────
            if (k.vk == fabgl::VK_MENU_ENTER || k.vk == fabgl::VK_MENU_RIGHT) {
                if (!s_visTotal) continue;
                OSD::clickNoPause();
                if (onDir && !onUp) {
                    if (s_stack_top < POS_STACK_MAX) s_stack[s_stack_top++] = { s_top, s_sel };
                    s_dir += name + "/";
                    s_sel = s_top = 0;
                    Debug::log("UiBrowser: ENTER -> '%s'\n", s_dir.c_str());
                    break;                            // rescan the child
                }
                if (k.vk == fabgl::VK_MENU_RIGHT) continue;   // Right only navigates
                return leave("R" + name);
            }

            // ── search mode ────────────────────────────────────────────────────
            if (k.vk == fabgl::VK_F3) {
                s_search = !s_search;
                if (!s_search && !s_query.empty()) {
                    s_query.clear();
                    recountVisible();
                    s_sel = s_top = 0;
                    drawList(); drawInfo(); drawHeader();
                }
                drawFooter();
                OSD::clickNoPause();
                continue;
            }
            if (s_search && k.vk == fabgl::VK_MENU_BS) {
                if (!s_query.empty()) {
                    s_query.pop_back();
                    recountVisible();
                    s_sel = s_top = 0;
                    drawList(); drawInfo(); drawHeader(); drawFooter();
                    OSD::clickNoPause();
                }
                continue;
            }

            // ── per-file verbs (full browser only) ─────────────────────────────
            const bool all = (s_ftype == DISK_ALLFILE);
            if (all && k.vk == fabgl::VK_F1 && !onDir && s_visTotal) {
                OSD::clickNoPause();
                gfxSuspendPalette();
                if (FileUtils::hasZIPextension(name)) ZipExtract::viewInfo(s_dir + name);
                else FileInfo::viewInfo(s_dir + name);
                gfxResumePalette();
                drawAll();
                continue;
            }
            if (!all && k.vk == fabgl::VK_F1) return leave("");
            if (k.vk == fabgl::VK_ESCAPE) return leave("");

            if (all && k.vk == fabgl::VK_F4 && !onDir && s_visTotal
                    && FileUtils::hasZIPextension(name)) {
                OSD::clickNoPause();
                return leave("X" + name);
            }
            if (all && k.vk == fabgl::VK_F5 && !onDir && s_visTotal
                    && FileUtils::ifaceForExt(FileUtils::getLCaseExt(name)) != IFACE_NONE) {
                OSD::clickNoPause();
                return leave("P" + name);
            }
            if (all && k.vk == fabgl::VK_F6 && !onUp && s_visTotal) {   // rename
                Debug::log("UiBrowser: F6 rename '%s' sp=%08x\n", name.c_str(), debug_sp());
                OSD::clickNoPause();
                string nn = name;
                if (footerAsk("Rename:", nn) && nn != name) {
                    f_rename((s_dir + name).c_str(), (s_dir + nn).c_str());
                    s_goto = onDir ? char(DIR_MARKER) + nn : nn;
                    break;
                }
                continue;
            }
            if (all && k.vk == fabgl::VK_F7) {                          // mkdir
                OSD::clickNoPause();
                string nn;
                if (footerAsk("New dir:", nn)) {
                    f_mkdir((s_dir + nn).c_str());
                    s_goto = char(DIR_MARKER) + nn;
                    break;
                }
                continue;
            }
            if (all && (k.vk == fabgl::VK_F8 || k.vk == fabgl::VK_DELETE)
                    && !onUp && s_visTotal) {                           // delete
                OSD::clickNoPause();
                char q[80];
                snprintf(q, sizeof(q), "Delete %s\n%.40s ?", onDir ? "folder" : "file",
                         name.c_str());
                const bool yes = uiConfirm(q);
                drawAll();
                if (yes) {
                    if (onDir) {
                        status("Deleting...", -1);
                        FileUtils::deleteDirRecursive((s_dir + name).c_str());
                        s_goto = char(DIR_MARKER) + name;
                    } else {
                        f_unlink((s_dir + name).c_str());
                        s_goto = name;
                    }
                    break;
                }
                continue;
            }
            if (all && k.vk == fabgl::VK_F9) {                          // new TRD
                OSD::clickNoPause();
                string nn;
                if (footerAsk("New TRD:", nn)) {
                    if (nn.size() < 4 || nn.substr(nn.size() - 4) != ".trd") nn += ".trd";
                    status("Creating TRD...", -1);
                    rvmWD1793CreateEmptyTRD((s_dir + nn).c_str());
                    s_goto = nn;
                    break;
                }
                continue;
            }

            // ── typing: search input or first-letter jump ──────────────────────
            if (k.ASCII >= 32 && k.ASCII < 127) {
                if (s_search) {
                    if (s_query.size() < 24) {
                        s_query += (char)toupper(k.ASCII);
                        recountVisible();
                        s_sel = s_top = 0;
                        drawList(); drawInfo(); drawHeader(); drawFooter();
                        OSD::clickNoPause();
                    }
                    continue;
                }
                if (isalnum((uint8_t)k.ASCII) && s_visTotal) {
                    const char want = (char)toupper(k.ASCII);
                    char curFirst = 0;
                    { const string cn = onUp ? "" : plainName(rec);
                      if (!cn.empty()) curFirst = (char)toupper((uint8_t)cn[0]); }
                    const int start = (curFirst == want) ? s_sel + 1 : 0;
                    for (int i = 0; i < s_visTotal; i++) {
                        const int v = (start + i) % s_visTotal;
                        const string s = visName(v);
                        const string pn = isUpRec(s) ? "" : plainName(s);
                        if (!pn.empty() && (char)toupper((uint8_t)pn[0]) == want) {
                            if (v != s_sel) moveSel(v - s_sel);
                            OSD::clickNoPause();
                            break;
                        }
                    }
                }
                continue;
            }
        }
    }
}

// ── footer loader for the net flows (OSD::progressOverride) ────────────────────

void uiProgressStatus(const char* title, const char* msg, int percent, int action,
                      bool cyrillic) {
    gfxComputeSurface();          // cheap; keeps L honest after any classic draw
    computeBL();
    gfxResumePalette();           // DS80: the fetch runs between chrome sessions
    const int y = L.iy + L.ih - L.foot_h;
    fill(L.ix, y + 1, L.iw, L.foot_h - 1, C_FOOT_BG);
    if (action == 2) return;      // close: leave a clean footer, the next draw owns it

    string m = title ? title : "";
    if (msg && msg[0]) {
        if (!m.empty()) m += "  ";
        m += msg;
    }
    if (cyrillic) m = FileUtils::utf8ToCp1251(m);

    const int bw = L.iw / 3;
    const int bx = L.ix + L.iw - L.pad - bw;
    textClip(L.ix + L.pad, y + 3, bx - L.ix - 3 * L.pad, m.c_str(), C_TEXT);
    frame(bx, y + 3, bw, UI_FONT_H - 2, C_SEP);
    if (percent > 0) {
        const int p = percent > 100 ? 100 : percent;
        fill(bx + 1, y + 4, (bw - 2) * p / 100, UI_FONT_H - 4, C_ACCENT);
    }
    roundRectBorder(L.margin, L.iy - 1, Sf.w - 2 * L.margin, L.ih + 2, 4, C_SEP, C_BG);
}

// ── the "Open from" locations level ────────────────────────────────────────────
// Same chrome as the file browser: this is the level ABOVE the volume roots, so
// it must look like part of the browser, not a modal on top of it.

int browseLocations(const char* const* items, const char* const* hints, int n, int initial) {
    if (n <= 0) return -1;
    gfxBegin();
    computeBL();

    int sel = (initial >= 0 && initial < n) ? initial : 0;
    const int lh = UI_FONT_H + 2;

    auto drawRow = [&](int i) {
        const int y = L.body_y + i * L.row_h;
        const bool s = (i == sel);
        fill(L.lx, y, L.lw, L.row_h, s ? C_SEL_BG : C_PANEL);
        if (i >= n) return;
        textClip(L.lx + L.pad, y + 1, L.lw - 2 * L.pad - chevronW() - L.pad,
                 items[i], s ? C_WHITE : C_TEXT);
        chevron(L.lx + L.lw - L.pad - chevronW(), y + L.row_h / 2 - 3,
                s ? C_WHITE : C_DISABLED);
    };
    auto drawInfoPane = [&]() {
        fill(L.rx, L.body_y, L.rw, L.body_h, C_PANEL);
        int y = L.body_y + 2;
        const int tx = L.rx + L.pad, tw = L.rw - 2 * L.pad;
        if (hints && hints[sel] && hints[sel][0]) {
            // Wrap the hint over a few lines (same fixed-width wrap as the name box).
            const int fits = tw / glyphW();
            const char* h = hints[sel];
            const int len = (int)strlen(h);
            for (int ln = 0; ln < 4 && ln * fits < len; ln++) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%.*s", fits, h + ln * fits);
                text(tx, y, buf, C_TEXT_DIM);
                y += lh;
            }
            y += 3;
        }
        hline(tx, y, tw, C_SEP);
        y += 3;
        text(tx, y, SYM_ENTER " Open", C_TEXT);
    };
    auto drawAllLoc = [&]() {
        drawChrome();
        drawHeader();                              // uses s_title/s_sel/s_visTotal
        // Location bar: where the path normally lives — this level's "path".
        const int py = L.iy + L.hdr_h;
        fill(L.ix, py, L.iw, L.path_h, C_PANEL_ALT);
        text(L.ix + L.pad, py + 2, "Open from", C_TEXT);
        hline(L.ix, py + L.path_h - 1, L.iw, C_SEP);
        for (int r = 0; r < L.rows; r++) drawRow(r);
        drawInfoPane();
        const int fy = L.iy + L.ih - L.foot_h;
        fill(L.ix, fy, L.iw, L.foot_h, C_FOOT_BG);
        hline(L.ix, fy, L.iw, C_SEP);
        text(L.ix + L.pad, fy + 3,
             SYM_UP SYM_DOWN " Move  " SYM_ENTER " Open  Esc Close", C_TEXT_DIM);
        frameFix();
    };

    // Borrow the header state so drawHeader shows title + n/N like the browser.
    s_title = "Open file";
    s_visTotal = n;
    s_sel = sel;
    s_top = 0;
    drawAllLoc();

    int ret = -1;
    fabgl::VirtualKeyItem k;
    while (1) {
        if (!ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
            sleep_ms(5);
            if (uiClockDirty()) drawHeader();
            continue;
        }
        if (!ESPectrum::readKbd(&k) || !k.down) continue;
        int ns = sel;
        switch (k.vk) {
            case fabgl::VK_MENU_UP:    ns = sel - 1; break;
            case fabgl::VK_MENU_DOWN:  ns = sel + 1; break;
            case fabgl::VK_MENU_HOME:
            case fabgl::VK_HOME:       ns = 0; break;
            case fabgl::VK_END:        ns = n - 1; break;
            case fabgl::VK_MENU_ENTER:
            case fabgl::VK_MENU_RIGHT:
                OSD::clickNoPause();
                ret = sel;
                goto out;
            case fabgl::VK_ESCAPE: case fabgl::VK_F1:
                OSD::clickNoPause();
                ret = -1;
                goto out;
            case fabgl::VK_MENU_LEFT: case fabgl::VK_MENU_BS:
                // This is the top level — there is nothing to back out to, and Back
                // must never close the OSD (Esc/F1 only, as the footer says).
                OSD::clickNoPause();
                break;
            default: break;
        }
        if (ns < 0) ns = 0;
        if (ns > n - 1) ns = n - 1;
        if (ns != sel) {
            const int old = sel;
            sel = ns;
            s_sel = sel;
            drawRow(old);
            drawRow(sel);
            drawInfoPane();
            drawHeader();
            OSD::clickNoPause();
        }
    }
out:
    gfxEnd();
    return ret;
}

// ── new-chrome renderer for the classic index (remote / web lists) ─────────────
// The classic net flows fill the shared OSDFile index and loop on fdChromeNav;
// this is that nav loop in the new chrome. All verbs and the return contract
// match fdChromeNav, so remoteFileDialog / remoteHostsBrowse / the web catalog
// need no changes.

struct NavVerb { const char* k; const char* what; };
static const NavVerb kNvLoc[]    = { { SYM_ENTER, "Open" } };
static const NavVerb kNvHosts[]  = { { SYM_ENTER, "Connect" },
                                     { "F8", "Forget" } };
static const NavVerb kNvRemote[] = { { SYM_ENTER, "Run / Open" },
                                     { "F2", "Reload" },
                                     { "F5", "Save to SD" },
                                     { "F7", "Upload" },
                                     { "F8", "Delete" } };
static const NavVerb kNvWeb[]    = { { SYM_ENTER, "Run / Open" },
                                     { "F2", "Reload" },
                                     { "F5", "Save to SD" } };

int browseIndexNav(const string& title, const string& subtitle, int side,
                   bool utf8, int* outKey, int* ioFocus, int* ioBegin) {
    gfxBegin();
    computeBL();

    const int total = (int)fdIndexSize();
    const NavVerb* verbs; int nverbs;
    switch (side) {
        case OSD::FD_SIDE_HOSTS:  verbs = kNvHosts;  nverbs = 2; break;
        case OSD::FD_SIDE_REMOTE: verbs = kNvRemote; nverbs = 5; break;
        case OSD::FD_SIDE_WEB:    verbs = kNvWeb;    nverbs = 3; break;
        default:                  verbs = kNvLoc;    nverbs = 1; break;
    }
    const bool allowF2 = (side == OSD::FD_SIDE_REMOTE || side == OSD::FD_SIDE_WEB);
    const bool allowF5 = allowF2;
    const bool allowF7 = (side == OSD::FD_SIDE_REMOTE);
    const bool allowF8 = (side == OSD::FD_SIDE_REMOTE || side == OSD::FD_SIDE_HOSTS);

    // Adopt the classic 2-based cursor so per-folder session memory keeps working.
    int top = (ioBegin ? *ioBegin : 2) - 2;
    int sel = top + ((ioFocus ? *ioFocus : 2) - 2);
    if (sel < 0) sel = 0;
    if (sel > total - 1) sel = total ? total - 1 : 0;
    if (top < 0) top = 0;
    if (top > sel) top = sel;
    if (sel >= top + L.rows) top = sel - L.rows + 1;
    { const int maxTop = total - L.rows; if (top > maxTop) top = maxTop > 0 ? maxTop : 0; }

    // Display form of a record: strip markers, transcode UTF-8 catalog names.
    auto disp = [&](const string& rec) -> string {
        string nm_ = isUpRec(rec) ? ".." : plainName(rec);
        return utf8 ? FileUtils::utf8ToCp1251(nm_) : nm_;
    };

    auto drawRowN = [&](int visRow) {
        if (visRow < 0 || visRow >= L.rows) return;
        const int y = L.body_y + visRow * L.row_h;
        const int v = top + visRow;
        const bool s = (v == sel) && (v < total);
        const int sb = (total > L.rows) ? 2 * Sf.glyphScale : 0;
        fill(L.lx, y, L.lw - sb, L.row_h, s ? C_SEL_BG : C_PANEL);
        if (v >= total) return;
        const string rec = fdIndexGet((size_t)v);
        const bool dir = isDirRec(rec);
        const int markW = dir ? (chevronW() + L.pad) : 0;
        const int availw = L.lw - sb - 2 * L.pad - markW;
        // Every row here is actionable (a host to connect to, a folder, a file to
        // run) — unlike the SD browser there is no "extension does not match this
        // dialog" state, so nothing may render dimmed.
        const UiColor ink = s ? C_WHITE : C_TEXT;
        if (s) {
            s_mq_name = disp(rec);        // cache for the idle marquee tick
            s_mq_avail = availw;
            mqText(L.lx + L.pad, y + 1, availw, s_mq_name, ink);
        } else {
            textClip(L.lx + L.pad, y + 1, availw, disp(rec).c_str(), ink);
        }
        if (dir)
            chevron(L.lx + L.lw - sb - L.pad - chevronW(), y + L.row_h / 2 - 3,
                    s ? C_WHITE : C_DISABLED);
    };
    auto drawScroll = [&]() {
        if (total <= L.rows) return;
        const int sc = Sf.glyphScale;
        const int x = L.sep_x - 2 * sc;
        fill(x, L.body_y, sc, L.body_h, C_PANEL_ALT);
        int th = L.body_h * L.rows / total;
        if (th < 6) th = 6;
        const int span = L.body_h - th;
        const int maxTop = total - L.rows;
        fill(x, L.body_y + (maxTop > 0 ? span * top / maxTop : 0), sc, th, C_DISABLED);
    };
    auto drawInfoN = [&]() {
        fill(L.rx, L.body_y, L.rw, L.body_h, C_PANEL);
        int y = L.body_y + 2;
        const int lh = UI_FONT_H + 2;
        const int tx = L.rx + L.pad, tw = L.rw - 2 * L.pad;
        const int fits = tw / glyphW();
        if (sel < total) {
            const string rec = fdIndexGet((size_t)sel);
            const string nm_ = disp(rec);
            for (int ln = 0; ln < 3 && ln * fits < (int)nm_.size(); ln++) {
                text(tx, y, nm_.substr((size_t)ln * fits, fits).c_str(), C_WHITE);
                y += lh;
            }
            y += 2;
            text(tx, y, isUpRec(rec) ? "Parent folder"
                                     : (isDirRec(rec) ? "Folder" : "File"), C_TEXT_DIM);
            y += lh + 3;
        } else {
            text(tx, y, "Empty", C_TEXT_DIM);
            y += lh + 3;
        }
        hline(tx, y, tw, C_SEP);
        y += 3;
        for (int i = 0; i < nverbs; i++) {
            if (y + lh > L.body_y + L.body_h) break;
            text(tx, y, verbs[i].k, C_TEXT_DIM);
            text(tx + textWidth(verbs[i].k) + glyphW(), y, verbs[i].what, C_TEXT);
            y += lh;
        }
    };
    auto drawAllN = [&]() {
        drawChrome();
        s_title = title;
        s_visTotal = total;
        s_sel = sel;
        drawHeader();
        const int py = L.iy + L.hdr_h;
        fill(L.ix, py, L.iw, L.path_h, C_PANEL_ALT);
        {
            const string p = utf8 ? FileUtils::utf8ToCp1251(subtitle) : subtitle;
            const int fits = (L.iw - 2 * L.pad) / glyphW();
            if ((int)p.size() > fits && fits > 2)
                text(L.ix + L.pad, py + 2, (".." + p.substr(p.size() - fits + 2)).c_str(), C_TEXT);
            else
                text(L.ix + L.pad, py + 2, p.c_str(), C_TEXT);
        }
        hline(L.ix, py + L.path_h - 1, L.iw, C_SEP);
        for (int r = 0; r < L.rows; r++) drawRowN(r);
        drawScroll();
        drawInfoN();
        const int fy = L.iy + L.ih - L.foot_h;
        fill(L.ix, fy, L.iw, L.foot_h, C_FOOT_BG);
        hline(L.ix, fy, L.iw, C_SEP);
        text(L.ix + L.pad, fy + 3,
             SYM_UP SYM_DOWN " Move  " SYM_ENTER " Open  " SYM_LEFT " Back  Esc Close",
             C_TEXT_DIM);
        frameFix();
    };
    mqReset();
    drawAllN();

    int ret = -1, rkey = OSD::FDK_ESC;
    fabgl::VirtualKeyItem k;
    while (1) {
        if (!ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
            sleep_ms(5);
            if (mqTick()) drawRowN(sel - top);
            if (uiClockDirty()) drawHeader();
            continue;
        }
        if (!ESPectrum::readKbd(&k) || !k.down) continue;
        if (s_mq_off) { mqReset(); drawRowN(sel - top); }
        else mqReset();

        int ns = sel;
        switch (k.vk) {
            case fabgl::VK_MENU_UP:    ns = sel - 1; break;
            case fabgl::VK_MENU_DOWN:  ns = sel + 1; break;
            case fabgl::VK_PAGEUP:     ns = sel - L.rows; break;
            case fabgl::VK_PAGEDOWN:   ns = sel + L.rows; break;
            case fabgl::VK_MENU_HOME:
            case fabgl::VK_HOME:       ns = 0; break;
            case fabgl::VK_END:        ns = total - 1; break;

            case fabgl::VK_MENU_ENTER:
            case fabgl::VK_MENU_RIGHT:
                if (total > 0) { ret = sel; rkey = OSD::FDK_ENTER; goto out; }
                continue;
            case fabgl::VK_MENU_LEFT:
            case fabgl::VK_MENU_BS:
                ret = -1; rkey = OSD::FDK_BACK; goto out;
            case fabgl::VK_ESCAPE: case fabgl::VK_F1:
                ret = -1; rkey = OSD::FDK_ESC; goto out;
            case fabgl::VK_F2:
                if (allowF2) { ret = sel; rkey = OSD::FDK_F2; goto out; }
                continue;
            case fabgl::VK_F5:
                if (allowF5 && total > 0) { ret = sel; rkey = OSD::FDK_F5; goto out; }
                continue;
            case fabgl::VK_F7:
                if (allowF7) { ret = -1; rkey = OSD::FDK_F7; goto out; }
                continue;
            case fabgl::VK_F8: case fabgl::VK_DELETE:
                if (allowF8 && total > 0) { ret = sel; rkey = OSD::FDK_F8; goto out; }
                continue;
            default: continue;
        }
        if (ns < 0) ns = 0;
        if (ns > total - 1) ns = total ? total - 1 : 0;
        if (ns != sel) {
            const int oldSel = sel, oldTop = top;
            sel = ns;
            s_sel = sel;
            if (sel < top) top = sel;
            if (sel >= top + L.rows) top = sel - L.rows + 1;
            { const int mt = total - L.rows; if (top > mt) top = mt > 0 ? mt : 0; if (top < 0) top = 0; }
            if (top != oldTop) { for (int r = 0; r < L.rows; r++) drawRowN(r); drawScroll(); }
            else { drawRowN(oldSel - top); drawRowN(sel - top); }
            drawHeader();
            drawInfoN();
            OSD::clickNoPause();
        }
    }
out:
    OSD::clickNoPause();
    if (ioFocus) *ioFocus = sel - top + 2;
    if (ioBegin) *ioBegin = top + 2;
    if (outKey) *outKey = rkey;
    gfxEnd();
    return ret;
}

// ── entry point ────────────────────────────────────────────────────────────────

string browseFile(string& fdir, const string& title, uint8_t ftype) {
    if (Config::audio_driver == 3) send_to_595(LOW(AY_Enable));

    gfxBegin();
    computeBL();
    // F1 Info pages (FileInfo/ZipExtract) render in the new style too. Save and
    // restore: when the browser was opened from the menu the override is already set.
    void (*prevOverride)(const char*, const char*) = OSD::textPageOverride;
    OSD::textPageOverride = uiTextPage;

    s_dir   = fdir;
    // Self-heal: an earlier bug could persist "/../../" chains (Right on the ".."
    // row descended into a literal ".."). FatFs resolves them to the root anyway.
    if (s_dir.find("..") != string::npos) s_dir = "/";
    s_title = title;
    s_ftype = ftype;
    Debug::log("UiBrowser: open dir='%s' ftype=%u root_parent=%d\n",
               s_dir.c_str(), (unsigned)ftype, (int)OSD::fd_root_parent);

    // Split the type's extension list once ("(.tap,.tzx,...)" style comma list).
    s_exts.clear();
    string ss = FileUtils::fileTypes[ftype].fileExts;
    size_t pos;
    while ((pos = ss.find(',')) != string::npos) {
        s_exts.push_back(ss.substr(0, pos));
        ss.erase(0, pos + 1);
    }
    s_exts.push_back(ss);

    const string ret = runLoop();
    fdir = s_dir;                         // navigation is part of the contract
    Debug::log("UiBrowser: close ret='%.1s%s' dir='%s'\n",
               ret.c_str(), ret.size() > 1 ? "..." : "", s_dir.c_str());

    OSD::textPageOverride = prevOverride;
    gfxEnd();
    if (Config::audio_driver == 3) send_to_595(HIGH(AY_Enable));
    return ret;
}

} // namespace nm

