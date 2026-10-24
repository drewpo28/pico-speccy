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
// The tree is scanned once, at the first access. Guest writes go to an in-memory
// overlay: the guest sees them for the session, the folder is not changed. Files the
// host adds or edits afterwards are not seen until the next start.
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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
};

struct Run { uint32_t first, count; int node; };

std::vector<Node> s_nodes;
std::vector<Run> s_runs;            // ascending by first
uint32_t s_nclusters = 0, s_used = 0, s_fatsz = 0, s_datastart = 0, s_partsec = 0;
uint64_t s_total = 0;
bool s_built = false;
std::unordered_map<uint64_t, std::array<uint8_t, SS>> s_overlay;
int s_fdNode = -1, s_fd = -1;

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

void scan(int idx, int depth) {
    if (depth > 32) return;
    void* h = hd_open(s_nodes[idx].host.c_str());
    if (!h) return;
    struct stat st;
    std::vector<std::pair<std::string, struct stat>> items;
    while (const char* n = hd_next(h, &st)) {
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size >= 0xFFFFFFFFull) continue;   // FAT32 limit
        items.push_back({n, st});
    }
    hd_close(h);
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.first < b.first; });
    if (items.size() > 16000) items.resize(16000);   // keep a directory under FAT's 65536 entries
    for (auto& it : items) {
        Node nd;
        nd.host = s_nodes[idx].host + "/" + it.first;
        nd.name = it.first;
        nd.dir = S_ISDIR(it.second.st_mode);
        nd.size = nd.dir ? 0 : (uint64_t)it.second.st_size;
        nd.mtime = it.second.st_mtime;
        nd.parent = idx;
        s_nodes.push_back(nd);
        int ci = (int)s_nodes.size() - 1;
        s_nodes[idx].kids.push_back(ci);
        if (s_nodes[ci].dir) scan(ci, depth + 1);
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
    if (s_fdNode != r->node) {
        if (s_fd >= 0) close(s_fd);
        s_fd = open(n.host.c_str(), O_RDONLY);
        s_fdNode = r->node;
    }
    if (s_fd >= 0) {
        ssize_t got = pread(s_fd, out, SS, (off_t)pos);
        (void)got;
    }
}

}  // namespace

// Builds the card from `root`. Returns false when the folder cannot be read.
bool vfat_build(const std::string& root) {
    if (s_built) return true;
    s_nodes.clear(); s_runs.clear(); s_overlay.clear();
    struct stat st;
    if (stat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
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
    fprintf(stderr, "host: SD card = folder %s (%zu entries, %u MB used, %llu sectors)\n",
            root.c_str(), s_nodes.size() - 1, (unsigned)((uint64_t)s_used * CL >> 20),
            (unsigned long long)s_total);
    return true;
}

uint64_t vfat_sectors() { return s_total; }

void vfat_read(uint64_t sector, uint8_t* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++, buf += SS) {
        auto it = s_overlay.find(sector + i);
        if (it != s_overlay.end()) memcpy(buf, it->second.data(), SS);
        else readSector(sector + i, buf);
    }
}

void vfat_write(uint64_t sector, const uint8_t* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++, buf += SS) {
        if (sector + i >= s_total) continue;
        std::array<uint8_t, SS>& a = s_overlay[sector + i];
        memcpy(a.data(), buf, SS);
    }
}
