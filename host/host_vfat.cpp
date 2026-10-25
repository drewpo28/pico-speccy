// A host folder presented as a raw SD card: an MBR with one FAT32 partition, built on
// the fly from the directory tree (the idea of QEMU's vvfat). The devices that read
// the card sector by sector - the Next's DivSD, the Z-Controller, NeoGS SD, esxDOS in
// raw mode - see the same files the firmware's own FatFs layer (host_fatfs.cpp) sees.
//
// Layout: MBR at LBA 0, partition (type 0x0C) at LBA 2048: 32 reserved sectors (boot
// sector, FSInfo, backup at 6), two identical FATs, then the data area with 4 KB
// clusters. Every file and directory gets one contiguous cluster run, so a FAT sector
// is computed from the run table and nothing but the directory entries is stored.
// Directories carry LFN entries (UTF-16, from the UTF-8 host names) and 8.3 names in
// the Windows style (NAME~1.EXT when the long name does not fit).
//
// Guest -> folder. Guest writes go to an in-memory overlay (the layout must not move
// under a mounted guest), and vfat_sync() mirrors them into the folder: it parses the
// card as the guest left it (overlay over the base), compares it with the previous
// parse and creates / rewrites / deletes / renames on the host. A file whose cluster
// chain and size did not change is patched in place (only the written sectors); any
// other change is a full rewrite through a temp file + rename. Before a host file that
// backs the base is replaced or removed, its old inode is kept open (keepFd), so the
// base keeps reading the old bytes for the clusters the guest no longer owns - the
// guest's own view stays exactly what it wrote. host_disk.cpp calls vfat_sync() after
// a second without guest writes and at exit.
//
// Folder -> guest. Re-laying the card out under a mounted guest would corrupt it, so
// host-side changes are taken at the one point a guest re-mounts: a card reset
// (CMD0). vfat_card_reset() syncs, rescans the folder, and rebuilds the card when the
// folder differs from what the card was built from (or last synced to).
//
// Safety: a guest that reformats the card (BPB / MBR changed) switches the sync off
// for the session; a sync that would delete more than half of all entries is refused;
// a parse error skips the sync (the overlay keeps everything, nothing is lost).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <map>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

std::string host_cp1251_to_utf8(const std::string& s);   // host_fatfs.cpp

extern "C" {
void* hd_open(const char* path);
const char* hd_next(void* h, struct stat* st);
void hd_close(void* h);
}

namespace {

constexpr uint32_t SS = 512;          // sector size
constexpr uint32_t SPC = 8;           // sectors per cluster (4 KB)
constexpr uint32_t CL = SS * SPC;     // cluster bytes
constexpr uint32_t PART_START = 2048;
constexpr uint32_t RSVD = 32;
constexpr uint32_t NFATS = 2;
constexpr uint32_t FREE_CLUSTERS = 262144;   // 1 GB of free space shown to the guest
constexpr uint32_t MIN_CLUSTERS = 70000;     // FAT32 needs >= 65525

struct Node {
    std::string host;      // host path
    std::string name;      // UTF-8 name ("" for the root)
    bool dir = false;
    uint64_t size = 0;
    time_t mtime = 0;
    int parent = -1;
    std::vector<int> kids;
    uint32_t first = 0;    // first cluster (0 = empty file)
    uint32_t nclus = 0;
    std::vector<uint8_t> buf;   // directory contents
    int keepFd = -1;            // the original inode, kept after the host file was replaced
};

struct Run { uint32_t first, count; int node; };

std::vector<Node> s_nodes;
std::vector<Run> s_runs;            // ascending by first
uint32_t s_nclusters = 0, s_used = 0, s_fatsz = 0, s_datastart = 0, s_partsec = 0;
uint64_t s_total = 0;
bool s_built = false;
std::unordered_map<uint64_t, std::array<uint8_t, SS>> s_overlay;
int s_fdNode = -1, s_fd = -1;
std::string s_root;
std::unordered_map<std::string, int> s_hostIdx;   // host path -> base node

// Guest-write tracking for vfat_sync().
std::unordered_set<uint64_t> s_dirty;   // sectors written since the last sync
bool s_wrote = false;
uint64_t s_lastWriteMs = 0;
bool s_syncOff = false;                 // guest reformatted the card / parser self-check failed

struct GSig { bool dir; uint32_t size; uint32_t first; uint64_t chainHash; };
std::map<std::string, GSig> s_gview;    // last parse of the guest's view, rel path -> sig
struct HSig { bool dir; uint64_t size; int64_t mtimeNs; };
std::map<std::string, HSig> s_hview;    // the folder as the card knows it, rel path -> sig

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void put16(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

std::vector<uint16_t> utf8To16(const std::string& s) {
    std::vector<uint16_t> o;
    const unsigned char* p = (const unsigned char*)s.c_str();
    while (*p) {
        uint32_t u; int n;
        if (*p < 0x80) { u = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { u = ((*p & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { u = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            u = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4;
        } else { u = '_'; n = 1; }
        if (u >= 0x10000) { u -= 0x10000; o.push_back(0xD800 | (u >> 10)); o.push_back(0xDC00 | (u & 0x3FF)); }
        else o.push_back((uint16_t)u);
        p += n;
    }
    if (o.size() > 255) o.resize(255);
    return o;
}

void fatTime(time_t t, uint16_t& date, uint16_t& tm16) {
    struct tm tm;
    localtime_r(&t, &tm);
    int y = tm.tm_year + 1900 - 1980;
    if (y < 0) y = 0;
    if (y > 127) y = 127;
    date = (uint16_t)((y << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    tm16 = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

bool sfnChar(unsigned char c) {
    if (c < 0x20 || c >= 0x7F) return false;
    return !strchr("\"*+,/:;<=>?[\\]| .", c);
}

// The 8.3 name of `name` in `used` (11 bytes, space padded). needLfn: the long name
// carries information the 8.3 one does not.
std::string shortName(const std::string& name, std::unordered_set<std::string>& used, bool& needLfn) {
    std::string base, ext;
    size_t dot = name.rfind('.');
    std::string b = (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
    std::string e = (dot == std::string::npos || dot == 0) ? "" : name.substr(dot + 1);
    bool lossy = false;
    for (unsigned char c : b) {
        if (c == ' ' || c == '.') { lossy = true; continue; }
        unsigned char u = (c >= 'a' && c <= 'z') ? c - 32 : c;
        if (!sfnChar(u)) { u = '_'; lossy = true; }
        base += (char)u;
    }
    for (unsigned char c : e) {
        if (c == ' ') { lossy = true; continue; }
        unsigned char u = (c >= 'a' && c <= 'z') ? c - 32 : c;
        if (!sfnChar(u)) { u = '_'; lossy = true; }
        ext += (char)u;
    }
    if (base.empty()) { base = "_"; lossy = true; }
    if (base.size() > 8) lossy = true;
    if (ext.size() > 3) { ext.resize(3); lossy = true; }
    auto pack = [](const std::string& bb, const std::string& ee) {
        std::string s(11, ' ');
        for (size_t i = 0; i < bb.size() && i < 8; i++) s[i] = bb[i];
        for (size_t i = 0; i < ee.size() && i < 3; i++) s[8 + i] = ee[i];
        return s;
    };
    std::string exact;   // what the name is spelled as in 8.3, case included
    exact = base + (ext.empty() ? "" : "." + ext);
    needLfn = lossy || exact != name;
    if (!lossy) {
        std::string s = pack(base, ext);
        if (!used.count(s)) { used.insert(s); return s; }
    }
    for (int n = 1; n < 1000000; n++) {
        std::string tail = "~" + std::to_string(n);
        std::string bb = base.substr(0, std::min<size_t>(base.size(), 8 - tail.size())) + tail;
        std::string s = pack(bb, ext);
        if (!used.count(s)) { used.insert(s); needLfn = true; return s; }
    }
    return pack(base, ext);
}

uint8_t sfnSum(const uint8_t* s) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + s[i]);
    return sum;
}

void addSfn(std::vector<uint8_t>& d, const char* n11, uint8_t attr, uint32_t clus, uint32_t size, time_t mt) {
    uint8_t e[32] = {};
    memcpy(e, n11, 11);
    e[11] = attr;
    uint16_t dt, tm;
    fatTime(mt, dt, tm);
    put16(e + 14, tm); put16(e + 16, dt); put16(e + 18, dt);
    put16(e + 20, clus >> 16);
    put16(e + 22, tm); put16(e + 24, dt);
    put16(e + 26, clus & 0xFFFF);
    put32(e + 28, size);
    d.insert(d.end(), e, e + 32);
}

int lfnCount(const std::string& name) { return (int)((utf8To16(name).size() + 12) / 13); }

void addLfn(std::vector<uint8_t>& d, const std::string& name, uint8_t sum) {
    std::vector<uint16_t> u = utf8To16(name);
    int n = (int)((u.size() + 12) / 13);
    static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int k = n; k >= 1; k--) {
        uint8_t e[32] = {};
        e[0] = (uint8_t)(k | (k == n ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (int i = 0; i < 13; i++) {
            size_t idx = (size_t)(k - 1) * 13 + i;
            uint16_t c = idx < u.size() ? u[idx] : (idx == u.size() ? 0x0000 : 0xFFFF);
            put16(e + pos[i], c);
        }
        d.insert(d.end(), e, e + 32);
    }
}

constexpr const char* kTmpName = ".pspeccy-sync.tmp";   // vfat_sync()'s temp file

// One directory listing, the rule the card is built by. The change check walks the
// folder with the same rule, so "changed" means "the card would come out different".
void listDir(const std::string& path, std::vector<std::pair<std::string, struct stat>>& items) {
    items.clear();
    void* h = hd_open(path.c_str());
    if (!h) return;
    struct stat st;
    while (const char* n = hd_next(h, &st)) {
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size >= 0xFFFFFFFFull) continue;   // FAT32 limit
        if (!strcmp(n, kTmpName)) continue;
        items.push_back({n, st});
    }
    hd_close(h);
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.first < b.first; });
    if (items.size() > 16000) items.resize(16000);   // keep a directory under FAT's 65536 entries
}

HSig hsigOf(const struct stat& st) {
    const bool d = S_ISDIR(st.st_mode);
    return HSig{d, d ? 0 : (uint64_t)st.st_size,
                d ? 0 : (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec};
}

std::string relOf(const std::string& host) {
    return host.size() > s_root.size() ? host.substr(s_root.size() + 1) : std::string();
}

void scan(int idx, int depth) {
    if (depth > 32) return;
    std::vector<std::pair<std::string, struct stat>> items;
    listDir(s_nodes[idx].host, items);
    for (auto& it : items) {
        Node nd;
        nd.host = s_nodes[idx].host + "/" + it.first;
        nd.name = it.first;
        nd.dir = S_ISDIR(it.second.st_mode);
        nd.size = nd.dir ? 0 : (uint64_t)it.second.st_size;
        nd.mtime = it.second.st_mtime;
        nd.parent = idx;
        s_hview[relOf(nd.host)] = hsigOf(it.second);
        s_nodes.push_back(nd);
        int ci = (int)s_nodes.size() - 1;
        s_hostIdx[s_nodes[ci].host] = ci;
        s_nodes[idx].kids.push_back(ci);
        if (s_nodes[ci].dir) scan(ci, depth + 1);
    }
}

void walkHost(const std::string& path, int depth, std::map<std::string, HSig>& m) {
    if (depth > 32) return;
    std::vector<std::pair<std::string, struct stat>> items;
    listDir(path, items);
    for (auto& it : items) {
        std::string hp = path + "/" + it.first;
        m[relOf(hp)] = hsigOf(it.second);
        if (S_ISDIR(it.second.st_mode)) walkHost(hp, depth + 1, m);
    }
}

void alloc(int idx, uint32_t& next) {
    Node& n = s_nodes[idx];
    uint32_t bytes;
    if (n.dir) {
        uint32_t entries = (n.parent < 0) ? 1 : 2;   // root: volume label; others: . and ..
        for (int k : n.kids) entries += 1 + lfnCount(s_nodes[k].name);
        entries += 1;   // a zero entry ends the listing
        bytes = entries * 32;
    } else bytes = (uint32_t)n.size;
    n.nclus = (bytes + CL - 1) / CL;
    if (n.dir && n.nclus == 0) n.nclus = 1;
    if (n.nclus) {
        n.first = next;
        s_runs.push_back({next, n.nclus, idx});
        next += n.nclus;
    }
    for (int k : n.kids) alloc(k, next);
}

void fillDir(int idx) {
    Node& n = s_nodes[idx];
    if (!n.dir) return;
    std::vector<uint8_t> d;
    if (n.parent < 0) {
        addSfn(d, "PICOSPECCY ", 0x08, 0, 0, n.mtime);
    } else {
        addSfn(d, ".          ", 0x10, n.first, 0, n.mtime);
        uint32_t pc = s_nodes[n.parent].parent < 0 ? 0 : s_nodes[n.parent].first;
        addSfn(d, "..         ", 0x10, pc, 0, n.mtime);
    }
    std::unordered_set<std::string> used;
    for (int k : n.kids) {
        Node& c = s_nodes[k];
        bool needLfn;
        std::string sfn = shortName(c.name, used, needLfn);
        if (needLfn) addLfn(d, c.name, sfnSum((const uint8_t*)sfn.data()));
        addSfn(d, sfn.c_str(), c.dir ? 0x10 : 0x20, c.first, c.dir ? 0 : (uint32_t)c.size, c.mtime);
    }
    d.resize((size_t)n.nclus * CL, 0);
    n.buf.swap(d);
    for (int k : n.kids) fillDir(k);
}

const Run* runOf(uint32_t c) {
    auto it = std::upper_bound(s_runs.begin(), s_runs.end(), c,
                               [](uint32_t v, const Run& r) { return v < r.first; });
    if (it == s_runs.begin()) return nullptr;
    --it;
    return c < it->first + it->count ? &*it : nullptr;
}

uint32_t fatEntry(uint32_t c) {
    if (c == 0) return 0x0FFFFFF8;
    if (c == 1) return 0x0FFFFFFF;
    const Run* r = runOf(c);
    if (!r) return 0;
    return c + 1 < r->first + r->count ? c + 1 : 0x0FFFFFFF;
}

void readSector(uint64_t s, uint8_t* out) {
    memset(out, 0, SS);
    if (s == 0) {
        uint8_t* p = out + 446;
        p[1] = 0xFE; p[2] = 0xFF; p[3] = 0xFF;
        p[4] = 0x0C;
        p[5] = 0xFE; p[6] = 0xFF; p[7] = 0xFF;
        put32(p + 8, PART_START);
        put32(p + 12, s_partsec);
        put32(out + 440, 0x50435350);
        out[510] = 0x55; out[511] = 0xAA;
        return;
    }
    if (s < PART_START || s >= s_total) return;
    uint32_t rel = (uint32_t)(s - PART_START);
    if (rel == 0 || rel == 6) {
        static const uint8_t jmp[3] = {0xEB, 0x58, 0x90};
        memcpy(out, jmp, 3);
        memcpy(out + 3, "MSWIN4.1", 8);
        put16(out + 11, SS);
        out[13] = SPC;
        put16(out + 14, RSVD);
        out[16] = NFATS;
        out[21] = 0xF8;
        put16(out + 24, 63);
        put16(out + 26, 255);
        put32(out + 28, PART_START);
        put32(out + 32, s_partsec);
        put32(out + 36, s_fatsz);
        put32(out + 44, 2);
        put16(out + 48, 1);
        put16(out + 50, 6);
        out[64] = 0x80;
        out[66] = 0x29;
        put32(out + 67, 0x50435350);
        memcpy(out + 71, "PICOSPECCY ", 11);
        memcpy(out + 82, "FAT32   ", 8);
        out[510] = 0x55; out[511] = 0xAA;
        return;
    }
    if (rel == 1 || rel == 7) {
        put32(out, 0x41615252);
        put32(out + 484, 0x61417272);
        put32(out + 488, s_nclusters - s_used);
        put32(out + 492, 2 + s_used);
        out[510] = 0x55; out[511] = 0xAA;
        return;
    }
    if (rel == 2 || rel == 8) { out[510] = 0x55; out[511] = 0xAA; return; }
    if (rel < RSVD) return;
    if (rel < s_datastart) {
        uint32_t k = (rel - RSVD) % s_fatsz;
        for (uint32_t i = 0; i < SS / 4; i++) {
            uint32_t c = k * (SS / 4) + i;
            if (c < s_nclusters + 2) put32(out + i * 4, fatEntry(c));
        }
        return;
    }
    uint32_t c = 2 + (rel - s_datastart) / SPC;
    uint32_t off = ((rel - s_datastart) % SPC) * SS;
    const Run* r = runOf(c);
    if (!r) return;
    Node& n = s_nodes[r->node];
    uint64_t pos = (uint64_t)(c - r->first) * CL + off;
    if (n.dir) {
        if (pos + SS <= n.buf.size()) memcpy(out, n.buf.data() + pos, SS);
        return;
    }
    if (pos >= n.size) return;
    int fd = n.keepFd;
    if (fd < 0) {
        if (s_fdNode != r->node) {
            if (s_fd >= 0) close(s_fd);
            s_fd = open(n.host.c_str(), O_RDONLY);
            s_fdNode = r->node;
        }
        fd = s_fd;
    }
    if (fd >= 0) {
        ssize_t got = pread(fd, out, SS, (off_t)pos);
        (void)got;
    }
}

// ---- guest -> folder ------------------------------------------------------------------

void readView(uint64_t s, uint8_t* out) {
    auto it = s_overlay.find(s);
    if (it != s_overlay.end()) memcpy(out, it->second.data(), SS);
    else readSector(s, out);
}

uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t clusterSector(uint32_t c) { return (uint64_t)PART_START + s_datastart + (uint64_t)(c - 2) * SPC; }

std::string utf16To8(const std::vector<uint16_t>& u) {
    std::string o;
    for (size_t i = 0; i < u.size(); i++) {
        uint32_t c = u[i];
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F));
               o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}

// The 8.3 name as a host name: trailing spaces dropped, the NT lower-case flags
// (byte 12: 0x08 base, 0x10 extension) applied, high bytes taken as CP1251 like the
// rest of the firmware's names.
std::string sfnToName(const uint8_t* e) {
    std::string b((const char*)e, 8), x((const char*)e + 8, 3);
    if ((uint8_t)b[0] == 0x05) b[0] = (char)0xE5;
    while (!b.empty() && b.back() == ' ') b.pop_back();
    while (!x.empty() && x.back() == ' ') x.pop_back();
    if (e[12] & 0x08) for (char& c : b) if (c >= 'A' && c <= 'Z') c += 32;
    if (e[12] & 0x10) for (char& c : x) if (c >= 'A' && c <= 'Z') c += 32;
    return host_cp1251_to_utf8(x.empty() ? b : b + "." + x);
}

bool nameOk(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (char c : n) if (c == '/' || c == 0) return false;
    return n != kTmpName;
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

uint64_t chainHash(const std::vector<uint32_t>& c) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : c) for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; }
    return h;
}

struct PEnt {
    std::string rel;
    bool dir = false;
    uint32_t size = 0, first = 0;
    std::vector<uint32_t> chain;
    uint16_t date = 0, time = 0;
    bool bad = false;   // a file whose chain does not hold its size: left as it was
};

enum { P_OK, P_FORMAT, P_ERROR };

// The card as the guest sees it now (overlay over the base), as a flat list in
// directory pre-order (a parent always comes before its children).
struct Parser {
    std::vector<uint32_t> fat;
    std::vector<PEnt> out;
    std::string err;

    bool chain(uint32_t first, uint32_t need, std::vector<uint32_t>& c) {
        c.clear();
        if (first == 0) return need == 0;
        uint32_t cur = first;
        for (;;) {
            if (cur < 2 || cur >= s_nclusters + 2 || c.size() > s_nclusters) return false;
            c.push_back(cur);
            if (need && c.size() == need) return true;
            const uint32_t nx = fat[cur] & 0x0FFFFFFF;
            if (nx >= 0x0FFFFFF8) return need == 0;
            if (nx < 2) return false;
            cur = nx;
        }
    }

    bool dir(uint32_t first, const std::string& rel, int depth) {
        if (depth > 32) { err = "directories nested deeper than 32: " + rel; return false; }
        std::vector<uint32_t> cl;
        if (!chain(first, 0, cl) || cl.size() > 512) { err = "bad directory chain: /" + rel; return false; }
        std::vector<uint8_t> d(cl.size() * CL);
        for (size_t i = 0; i < cl.size(); i++)
            for (uint32_t j = 0; j < SPC; j++) readView(clusterSector(cl[i]) + j, d.data() + i * CL + j * SS);
        std::vector<uint16_t> lfn;
        int lfnN = 0;
        uint32_t lfnMask = 0;
        uint8_t lfnSum = 0;
        std::unordered_set<std::string> seen;
        static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (size_t o = 0; o + 32 <= d.size(); o += 32) {
            const uint8_t* e = d.data() + o;
            if (e[0] == 0x00) break;
            if (e[0] == 0xE5) { lfnN = 0; continue; }
            const uint8_t attr = e[11];
            if ((attr & 0x3F) == 0x0F) {
                const int ord = e[0] & 0x1F;
                if (e[0] & 0x40) { lfnN = ord; lfnMask = 0; lfnSum = e[13]; lfn.assign((size_t)ord * 13, 0xFFFF); }
                if (lfnN && ord >= 1 && ord <= lfnN && e[13] == lfnSum) {
                    for (int i = 0; i < 13; i++) lfn[(size_t)(ord - 1) * 13 + i] = get16(e + pos[i]);
                    lfnMask |= 1u << (ord - 1);
                } else lfnN = 0;
                continue;
            }
            if ((attr & 0x08) || e[0] == '.') { lfnN = 0; continue; }   // volume label, "." and ".."
            std::string name;
            if (lfnN > 0 && lfnN <= 20 && lfnMask == (1u << lfnN) - 1 && sfnSum(e) == lfnSum) name = utf16To8(lfn);
            else name = sfnToName(e);
            lfnN = 0;
            if (!nameOk(name)) { fprintf(stderr, "host: card sync: name '%s' in /%s skipped\n", name.c_str(), rel.c_str()); continue; }
            if (!seen.insert(lower(name)).second) continue;
            PEnt p;
            p.rel = rel.empty() ? name : rel + "/" + name;
            p.first = ((uint32_t)get16(e + 20) << 16) | get16(e + 26);
            p.time = get16(e + 22);
            p.date = get16(e + 24);
            if (attr & 0x10) {
                p.dir = true;
                out.push_back(p);
                if (!dir(p.first, p.rel, depth + 1)) return false;
            } else {
                p.size = get32(e + 28);
                p.bad = !chain(p.first, (p.size + CL - 1) / CL, p.chain);
                out.push_back(p);
            }
        }
        return true;
    }

    int run() {
        uint8_t b[SS];
        readView(0, b);
        if (b[510] != 0x55 || b[511] != 0xAA || (b[446 + 4] != 0x0C && b[446 + 4] != 0x0B) ||
            get32(b + 446 + 8) != PART_START) return P_FORMAT;
        readView(PART_START, b);
        if (get16(b + 11) != SS || b[13] != SPC || get16(b + 14) != RSVD || b[16] != NFATS ||
            get32(b + 36) != s_fatsz || get32(b + 44) != 2 || get32(b + 32) != s_partsec) return P_FORMAT;
        fat.assign((size_t)s_nclusters + 2, 0);
        const uint32_t secs = (uint32_t)(((uint64_t)fat.size() * 4 + SS - 1) / SS);
        for (uint32_t k = 0; k < secs; k++) {
            readView((uint64_t)PART_START + RSVD + k, b);
            for (uint32_t i = 0; i < SS / 4; i++) {
                const size_t c = (size_t)k * (SS / 4) + i;
                if (c < fat.size()) fat[c] = get32(b + i * 4);
            }
        }
        return dir(2, "", 0) ? P_OK : P_ERROR;
    }
};

std::string hostPath(const std::string& rel) { return rel.empty() ? s_root : s_root + "/" + rel; }

// The base node behind `hp` keeps reading its old bytes once the host file goes: the
// old inode stays open, or, if it cannot be (out of descriptors), its sectors are
// copied into the overlay while the file is still there.
void keepOld(const std::string& hp) {
    auto it = s_hostIdx.find(hp);
    if (it == s_hostIdx.end()) return;
    Node& n = s_nodes[it->second];
    if (n.dir || n.keepFd >= 0 || !n.nclus) return;
    n.keepFd = open(hp.c_str(), O_RDONLY);
    if (n.keepFd >= 0) return;
    fprintf(stderr, "host: card sync: cannot keep %s open (%s), copying it into memory\n", hp.c_str(), strerror(errno));
    for (uint32_t c = 0; c < n.nclus; c++)
        for (uint32_t j = 0; j < SPC; j++) {
            const uint64_t s = clusterSector(n.first + c) + j;
            if (s_overlay.count(s)) continue;
            std::array<uint8_t, SS> a;
            readSector(s, a.data());
            s_overlay[s] = a;
        }
}

void setMtime(const std::string& hp, uint16_t date, uint16_t tm16) {
    if (!date) return;
    struct tm t = {};
    t.tm_year = 80 + (date >> 9);
    t.tm_mon = ((date >> 5) & 15) - 1;
    t.tm_mday = date & 31;
    t.tm_hour = tm16 >> 11;
    t.tm_min = (tm16 >> 5) & 63;
    t.tm_sec = (tm16 & 31) * 2;
    t.tm_isdst = -1;
    const time_t v = mktime(&t);
    if (v == (time_t)-1) return;
    struct timespec ts[2];
    ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = v; ts[1].tv_nsec = 0;
    utimensat(AT_FDCWD, hp.c_str(), ts, 0);
}

bool writeFull(const PEnt& e, const std::string& hp) {
    const size_t slash = hp.rfind('/');
    const std::string tmp = hp.substr(0, slash) + "/" + kTmpName;
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "host: card sync: %s: %s\n", tmp.c_str(), strerror(errno)); return false; }
    std::vector<uint8_t> buf(CL);
    uint32_t remain = e.size;
    bool ok = true;
    for (uint32_t c : e.chain) {
        if (!remain) break;
        for (uint32_t j = 0; j < SPC; j++) readView(clusterSector(c) + j, buf.data() + j * SS);
        const uint32_t n = remain < CL ? remain : CL;
        if (write(fd, buf.data(), n) != (ssize_t)n) { ok = false; break; }
        remain -= n;
    }
    if (close(fd) != 0) ok = false;
    if (ok) {
        keepOld(hp);
        if (rename(tmp.c_str(), hp.c_str()) != 0) ok = false;
    }
    if (!ok) {
        fprintf(stderr, "host: card sync: cannot write %s: %s\n", hp.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return false;
    }
    setMtime(hp, e.date, e.time);
    return true;
}

// Only sectors changed, chain and size the same: write those sectors into the file.
bool patchInPlace(const PEnt& e, const std::string& hp, const std::unordered_set<uint32_t>& dc) {
    struct stat st;
    if (stat(hp.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != e.size) return false;
    const int fd = open(hp.c_str(), O_WRONLY);
    if (fd < 0) return false;
    uint8_t b[SS];
    bool ok = true;
    for (size_t i = 0; i < e.chain.size() && ok; i++) {
        if (!dc.count(e.chain[i])) continue;
        for (uint32_t j = 0; j < SPC; j++) {
            const uint64_t s = clusterSector(e.chain[i]) + j;
            const uint64_t off = (uint64_t)i * CL + (uint64_t)j * SS;
            if (off >= e.size || !s_dirty.count(s)) continue;
            readView(s, b);
            const size_t n = (size_t)std::min<uint64_t>(SS, e.size - off);
            if (pwrite(fd, b, n, (off_t)off) != (ssize_t)n) { ok = false; break; }
        }
    }
    if (close(fd) != 0) ok = false;
    if (ok) setMtime(hp, e.date, e.time);
    return ok;
}

void hviewSet(const std::string& rel) {
    struct stat st;
    if (stat(hostPath(rel).c_str(), &st) == 0) s_hview[rel] = hsigOf(st);
    else s_hview.erase(rel);
}

template <class M> void remapKeys(M& m, const std::string& from, const std::string& to) {
    std::vector<std::pair<std::string, typename M::mapped_type>> moved;
    for (auto it = m.begin(); it != m.end();) {
        if (it->first == from || it->first.compare(0, from.size() + 1, from + "/") == 0) {
            moved.push_back({to + it->first.substr(from.size()), it->second});
            it = m.erase(it);
        } else ++it;
    }
    for (auto& kv : moved) m[kv.first] = kv.second;
}

// The guest renamed `from` to `to` and the host followed: the base nodes now live
// under the new path (their inodes did not change).
void remapAll(const std::string& from, const std::string& to) {
    remapKeys(s_gview, from, to);
    remapKeys(s_hview, from, to);
    const std::string hf = hostPath(from), ht = hostPath(to);
    for (size_t i = 0; i < s_nodes.size(); i++) {
        std::string& h = s_nodes[i].host;
        if (h == hf || h.compare(0, hf.size() + 1, hf + "/") == 0) {
            s_hostIdx.erase(h);
            h = ht + h.substr(hf.size());
            s_hostIdx[h] = (int)i;
        }
    }
}

bool exists(const std::string& hp) { struct stat st; return lstat(hp.c_str(), &st) == 0; }

// Mirrors the guest's changes into the folder. Returns false when it could not run
// (the overlay still holds everything; the caller retries later).
bool syncNow() {
    if (!s_wrote) return true;
    if (s_syncOff) { s_dirty.clear(); s_wrote = false; return true; }
    Parser P;
    const int r = P.run();
    if (r == P_FORMAT) {
        fprintf(stderr, "host: card sync OFF for this session: the guest reformatted the card "
                        "(it keeps its changes in memory; the folder is not touched)\n");
        s_syncOff = true;
        s_dirty.clear();
        s_wrote = false;
        return true;
    }
    if (r != P_OK) {
        fprintf(stderr, "host: card sync postponed: %s\n", P.err.c_str());
        return false;
    }
    std::unordered_set<uint32_t> dc;
    for (uint64_t s : s_dirty)
        if (s >= (uint64_t)PART_START + s_datastart && s < s_total)
            dc.insert(2 + (uint32_t)((s - PART_START - s_datastart) / SPC));

    std::map<std::string, GSig> cur;
    for (const PEnt& e : P.out) cur[e.rel] = GSig{e.dir, e.size, e.first, e.dir ? 0 : chainHash(e.chain)};

    // Renames: an entry that left one path and shows up at another with the same
    // first cluster moved. The host renames too (no copy) - a directory first, then
    // what is left. A rename onto a path that exists on the host (a swap, an overwrite)
    // falls back to the plain remove + write below.
    auto gone = [&](const std::string& rel, const GSig& g) {
        auto it = cur.find(rel);
        return it == cur.end() || it->second.dir != g.dir;
    };
    std::unordered_map<uint64_t, std::string> arrived;   // (first, dir) -> new rel
    for (auto& [rel, g] : cur) {
        if (!g.first) continue;
        auto it = s_gview.find(rel);
        if (it == s_gview.end() || it->second.dir != g.dir) arrived[((uint64_t)g.dir << 32) | g.first] = rel;
    }
    unsigned nRen = 0;
    for (int pass = 0; pass < 2; pass++) {   // 0: directories, 1: files
        std::vector<std::string> cand;
        for (auto& [rel, g] : s_gview)
            if (g.dir == (pass == 0) && g.first && gone(rel, g)) cand.push_back(rel);
        std::sort(cand.begin(), cand.end(), [](const std::string& a, const std::string& b) {
            return std::count(a.begin(), a.end(), '/') < std::count(b.begin(), b.end(), '/');
        });
        for (const std::string& rel : cand) {
            auto gi = s_gview.find(rel);
            if (gi == s_gview.end() || !gone(rel, gi->second)) continue;   // moved with its parent
            auto ai = arrived.find(((uint64_t)gi->second.dir << 32) | gi->second.first);
            if (ai == arrived.end() || s_gview.count(ai->second)) continue;
            const std::string to = ai->second;
            if (exists(hostPath(to)) || rename(hostPath(rel).c_str(), hostPath(to).c_str()) != 0) continue;
            remapAll(rel, to);
            nRen++;
        }
    }

    std::vector<std::string> delFiles, delDirs;
    for (auto& [rel, g] : s_gview) {
        auto it = cur.find(rel);
        if (it != cur.end() && it->second.dir == g.dir) continue;
        (g.dir ? delDirs : delFiles).push_back(rel);
    }
    const size_t ndel = delFiles.size() + delDirs.size();
    if (ndel > 32 && ndel * 2 > s_gview.size()) {
        fprintf(stderr, "host: card sync OFF for this session: the guest removed %zu of %zu entries - "
                        "not mirrored to the folder (the guest keeps its view in memory)\n",
                ndel, s_gview.size());
        s_syncOff = true;
        s_dirty.clear();
        s_wrote = false;
        return true;
    }

    unsigned nNew = 0, nUpd = 0, nPatch = 0, nDel = 0, nErr = 0, nDir = 0;
    for (const std::string& rel : delFiles) {
        const std::string hp = hostPath(rel);
        keepOld(hp);
        if (unlink(hp.c_str()) == 0 || errno == ENOENT) nDel++; else nErr++;
        s_hview.erase(rel);
    }
    std::sort(delDirs.rbegin(), delDirs.rend());   // children before their parent
    for (const std::string& rel : delDirs) {
        if (rmdir(hostPath(rel).c_str()) == 0 || errno == ENOENT) nDel++;
        else fprintf(stderr, "host: card sync: cannot remove /%s: %s\n", rel.c_str(), strerror(errno));
        s_hview.erase(rel);
    }
    for (const PEnt& e : P.out) {
        if (!e.dir) continue;
        auto it = s_gview.find(e.rel);
        if (it != s_gview.end() && it->second.dir) continue;
        const std::string hp = hostPath(e.rel);
        struct stat st;
        if (stat(hp.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { hviewSet(e.rel); continue; }
        if (mkdir(hp.c_str(), 0755) == 0) { nDir++; hviewSet(e.rel); }
        else { nErr++; cur.erase(e.rel); fprintf(stderr, "host: card sync: mkdir /%s: %s\n", e.rel.c_str(), strerror(errno)); }
    }
    for (const PEnt& e : P.out) {
        if (e.dir) continue;
        auto it = s_gview.find(e.rel);
        if (e.bad) {   // a chain that does not hold the size: leave the host file alone
            if (it != s_gview.end()) cur[e.rel] = it->second; else cur.erase(e.rel);
            continue;
        }
        const GSig& g = cur[e.rel];
        const bool same = it != s_gview.end() && !it->second.dir && it->second.size == g.size &&
                          it->second.chainHash == g.chainHash;
        bool hit = false;
        for (uint32_t c : e.chain) if (dc.count(c)) { hit = true; break; }
        if (same && !hit) continue;
        const std::string hp = hostPath(e.rel);
        if (same && patchInPlace(e, hp, dc)) { nPatch++; hviewSet(e.rel); continue; }
        if (writeFull(e, hp)) { (it == s_gview.end() ? nNew : nUpd)++; hviewSet(e.rel); }
        else { nErr++; cur.erase(e.rel); }
    }
    s_gview.swap(cur);
    s_dirty.clear();
    s_wrote = false;
    if (nNew + nUpd + nPatch + nDel + nDir + nRen + nErr)
        fprintf(stderr, "host: card -> folder: %u new, %u rewritten, %u patched, %u renamed, %u dirs made, %u removed%s\n",
                nNew, nUpd, nPatch, nRen, nDir, nDel, nErr ? " (some failed, see above)" : "");
    return true;
}

bool hostChanged() {
    std::map<std::string, HSig> m;
    walkHost(s_root, 0, m);
    if (m.size() != s_hview.size()) return true;
    for (auto a = m.begin(), b = s_hview.begin(); a != m.end(); ++a, ++b)
        if (a->first != b->first || a->second.dir != b->second.dir || a->second.size != b->second.size ||
            a->second.mtimeNs != b->second.mtimeNs) return true;
    return false;
}

void releaseAll() {
    for (Node& n : s_nodes) if (n.keepFd >= 0) { close(n.keepFd); n.keepFd = -1; }
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
    s_fdNode = -1;
}

}  // namespace

// Builds the card from `root`. Returns false when the folder cannot be read.
bool vfat_build(const std::string& root) {
    if (s_built) return true;
    static bool s_rlim = false;
    if (!s_rlim) {   // keepOld() holds one descriptor per replaced host file
        s_rlim = true;
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
    releaseAll();
    s_nodes.clear(); s_runs.clear(); s_overlay.clear(); s_hostIdx.clear(); s_hview.clear();
    s_gview.clear(); s_dirty.clear();
    s_wrote = false;
    struct stat st;
    if (stat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    s_root = root;
    Node r;
    r.host = root;
    r.dir = true;
    r.mtime = st.st_mtime;
    s_nodes.push_back(r);
    scan(0, 0);
    uint32_t next = 2;
    alloc(0, next);
    s_used = next - 2;
    s_nclusters = std::max<uint32_t>(s_used + FREE_CLUSTERS, MIN_CLUSTERS);
    s_fatsz = (uint32_t)(((uint64_t)(s_nclusters + 2) * 4 + SS - 1) / SS);
    s_datastart = RSVD + NFATS * s_fatsz;
    s_partsec = s_datastart + s_nclusters * SPC;
    s_total = (uint64_t)PART_START + s_partsec;
    fillDir(0);
    s_built = true;

    // The guest's starting view: the card read back the way vfat_sync() will read it.
    // Doubles as a self-check of the parser against the builder.
    Parser P;
    if (P.run() != P_OK) {
        fprintf(stderr, "host: card sync OFF: the built card does not parse (%s)\n", P.err.c_str());
        s_syncOff = true;
    } else {
        unsigned odd = 0;
        for (const PEnt& e : P.out) {
            s_gview[e.rel] = GSig{e.dir, e.size, e.first, e.dir ? 0 : chainHash(e.chain)};
            if (!s_hview.count(e.rel) && odd++ < 4)
                fprintf(stderr, "host: card name '%s' does not map back to the folder\n", e.rel.c_str());
        }
    }
    fprintf(stderr, "host: SD card = folder %s (%zu entries, %u MB used, %llu sectors)\n",
            root.c_str(), s_nodes.size() - 1, (unsigned)((uint64_t)s_used * CL >> 20),
            (unsigned long long)s_total);
    return true;
}

uint64_t vfat_sectors() { return s_total; }

void vfat_read(uint64_t sector, uint8_t* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++, buf += SS) readView(sector + i, buf);
}

void vfat_write(uint64_t sector, const uint8_t* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++, buf += SS) {
        if (sector + i >= s_total) continue;
        uint8_t cur[SS];
        readView(sector + i, cur);
        if (!memcmp(cur, buf, SS)) continue;   // FAT / FSInfo rewritten unchanged: not a change
        std::array<uint8_t, SS>& a = s_overlay[sector + i];
        memcpy(a.data(), buf, SS);
        s_dirty.insert(sector + i);
        s_wrote = true;
        s_lastWriteMs = nowMs();
    }
}

// A guest write is pending and the guest has been quiet for `quietMs`.
bool vfat_sync_due(uint64_t quietMs) { return s_built && s_wrote && nowMs() - s_lastWriteMs >= quietMs; }

// Mirrors the guest's writes into the folder. A sync that cannot run now (the guest
// left the card mid-update) is retried after the next quiet period.
void vfat_sync() {
    if (!s_built) return;
    if (!syncNow()) s_lastWriteMs = nowMs();
}

// The guest reset the card (CMD0): it re-mounts, so this is where host-side changes
// can come in. Returns true when the card was rebuilt (cached sectors are stale).
bool vfat_card_reset() {
    if (!s_built || s_syncOff) return false;
    if (!syncNow()) return false;                   // the guest's writes come first
    if (!hostChanged()) return false;
    fprintf(stderr, "host: folder changed on the PC - card rebuilt at the guest's card reset\n");
    const std::string root = s_root;
    s_built = false;
    vfat_build(root);
    return true;
}
