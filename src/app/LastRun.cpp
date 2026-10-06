#include "LastRun.h"

namespace LastRun {

static std::string s_name, s_aliasName;
static int s_muted = 0;   // nesting depth: a reset inside a muted boot must not unmute it

static std::string baseNoExt(const std::string& p) {
    std::string n = p;
    const size_t sl = n.find_last_of('/');
    if (sl != std::string::npos) n = n.substr(sl + 1);
    const size_t col = n.find(':');          // "USB:name"
    if (col != std::string::npos) n = n.substr(col + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos && dot > 0) n = n.substr(0, dot);
    return n;
}

static std::string baseOf(const std::string& p) {
    const size_t sl = p.find_last_of('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

// The launch helpers' fixed temporary names (ZipExtract::TEMP_FILE, the web
// launcher's /tmp/_run.*, the player's /tmp/_play.*, the RZX inner snapshot).
static bool isTemp(const std::string& path) {
    if (path.compare(0, 5, "/tmp/") == 0) return true;
    const std::string b = baseOf(path);
    return b.compare(0, 12, ".zip_extract") == 0 || b.compare(0, 5, "_run.") == 0 ||
           b.compare(0, 6, "_play.") == 0 || b.compare(0, 5, "_rzx.") == 0;
}

void note(const std::string& path) {
    if (s_muted || path.empty() || path == "none") return;
    // A launch helper's temporary stands for the name it registered (zip entry,
    // remote file); without one it names nothing. Matched by kind, not by path —
    // some callers hand over only the base name (the tape keeps one).
    if (isTemp(path)) {
        if (!s_aliasName.empty()) name(baseNoExt(s_aliasName));
        return;
    }
    name(baseNoExt(path));
}

void name(const std::string& n) {
    if (s_muted || n.empty()) return;
    s_name = n;
}

void alias(const std::string& tmpPath, const std::string& realName) {
    (void)tmpPath;   // every helper temp maps to the newest alias
    s_aliasName = realName;
}

void mute(bool on) { s_muted += on ? 1 : (s_muted > 0 ? -1 : 0); }

const std::string& get() { return s_name; }

}
