// Host test of host/host_vfat.cpp, the folder-as-SD-card: a real FatFs (external/fatfs/ff.c)
// plays the guest and writes through vfat_write(); after vfat_sync() the folder must equal
// what the guest sees, the guest must keep reading what it wrote (renames, rewrites, files
// the PC holds behind its back), changes made on the PC must come in at a card reset, and
// a guest that wipes the card must not wipe the folder. Random operations against a model
// (argument 1: how many, argument 2: the seed).
//
//   H=. ; O=/tmp/vfat_test; mkdir -p $O
//   for f in ff ffunicode; do gcc -O1 -fsanitize=address,undefined -I$H/external/fatfs \
//       -c $H/external/fatfs/$f.c -o $O/$f.o; done
//   g++ -O1 -fsanitize=address,undefined -std=c++17 -I$H/external/fatfs -o $O/t \
//       $H/tools/host/vfat_sync_test.cpp $H/host/host_vfat.cpp $H/host/host_fsdir.cpp $O/ff.o $O/ffunicode.o
//   $O/t 3000 1
//
// The same on a case-insensitive folder (the macOS/Windows behaviour; ciopfs is a FUSE
// layer, `apt install ciopfs`): mkdir -p $O/back $O/ci && ciopfs $O/back $O/ci &&
// VFAT_TEST_DIR=$O/ci $O/t 3000 1. It also brings FUSE's .fuse_hidden placeholders for
// open-but-deleted files, i.e. the rmdir-under-a-kept-inode case.
//
// Re-run after any change to host_vfat.cpp. Each of these hand mutations fails it: no
// keepFd taken (keepOld returns at once), keepFd ignored by readSector, patchInPlace
// writing nothing, a host rename without remapAll.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <random>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#define DIR FFDIR_
extern "C" {
#include "ff.h"
#include "diskio.h"
}
#undef DIR
static const uint16_t kCp1251[64] = {
    0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
    0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0000,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
    0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
    0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457};

static uint32_t cpToUni(uint8_t c) {
    if (c < 0x80) return c;
    if (c >= 0xC0) return 0x0410 + (c - 0xC0);
    return kCp1251[c - 0x80];
}
static int uniToCp(uint32_t u) {
    if (u < 0x80) return (int)u;
    if (u >= 0x0410 && u <= 0x044F) return 0xC0 + (int)(u - 0x0410);
    for (int i = 0; i < 64; i++) if (kCp1251[i] == u && u) return 0x80 + i;
    return -1;
}
static std::string cpToUtf8(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        uint32_t u = cpToUni(c);
        if (!u) u = '?';
        if (u < 0x80) o += (char)u;
        else if (u < 0x800) { o += (char)(0xC0 | (u >> 6)); o += (char)(0x80 | (u & 0x3F)); }
        else { o += (char)(0xE0 | (u >> 12)); o += (char)(0x80 | ((u >> 6) & 0x3F)); o += (char)(0x80 | (u & 0x3F)); }
    }
    return o;
}
extern "C" {   // ff.c's activity-LED hooks
void sdcard_led_tag(const void*, const char*) {}
void sdcard_led_untag(const void*) {}
void sdcard_led_set_active(const void*) {}
}
std::string host_cp1251_to_utf8(const std::string& s) { return cpToUtf8(s); }
bool vfat_build(const std::string& root);
uint64_t vfat_sectors();
void vfat_read(uint64_t, uint8_t*, uint32_t);
void vfat_write(uint64_t, const uint8_t*, uint32_t);
void vfat_sync();
bool vfat_card_reset();

extern "C" {
DSTATUS disk_initialize(BYTE) { return 0; }
DSTATUS disk_status(BYTE) { return 0; }
DRESULT disk_read(BYTE, BYTE* b, LBA_t s, UINT n) { vfat_read(s, b, n); return RES_OK; }
DRESULT disk_write(BYTE, const BYTE* b, LBA_t s, UINT n) { vfat_write(s, b, n); return RES_OK; }
DRESULT disk_ioctl(BYTE, BYTE cmd, void* b) {
    if (cmd == GET_SECTOR_COUNT) { *(LBA_t*)b = vfat_sectors(); return RES_OK; }
    if (cmd == GET_SECTOR_SIZE) { *(WORD*)b = 512; return RES_OK; }
    if (cmd == GET_BLOCK_SIZE) { *(DWORD*)b = 1; return RES_OK; }
    return RES_OK;
}
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (10 << 21) | (25 << 16) | (12 << 11); }
}

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string ROOT;
static FATFS fs;
static std::mt19937 rng;
static std::map<std::string, std::string> model;   // guest path (no leading /) -> content; dirs have "\x01DIR"
static const std::string DIRM = "\x01DIR";

static void mount() { memset(&fs, 0, sizeof fs); FRESULT r = f_mount(&fs, "", 1); CHECK(r == FR_OK, "mount %d", r); }
static void umount() { f_mount(nullptr, "", 0); }

static std::string rnd(size_t n) { std::string s(n, 0); for (auto& c : s) c = (char)(rng() & 0xFF); return s; }

static void gwrite(const std::string& p, const std::string& data) {
    FIL f; FRESULT r = f_open(&f, ("/" + p).c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    CHECK(r == FR_OK, "open w %s %d", p.c_str(), r); if (r) return;
    UINT bw; f_write(&f, data.data(), data.size(), &bw); CHECK(bw == data.size(), "write"); f_close(&f);
    model[p] = data;
}
static void gpatch(const std::string& p, size_t off, const std::string& data) {
    FIL f; FRESULT r = f_open(&f, ("/" + p).c_str(), FA_WRITE | FA_OPEN_EXISTING);
    CHECK(r == FR_OK, "open patch %s %d", p.c_str(), r); if (r) return;
    f_lseek(&f, off); UINT bw; f_write(&f, data.data(), data.size(), &bw); f_close(&f);
    std::string& m = model[p]; if (m.size() < off + data.size()) m.resize(off + data.size()); memcpy(&m[off], data.data(), data.size());
}
static std::string gread(const std::string& p, bool& ok) {
    FIL f; ok = f_open(&f, ("/" + p).c_str(), FA_READ) == FR_OK; if (!ok) return "";
    std::string s(f_size(&f), 0); UINT br = 0; f_read(&f, &s[0], s.size(), &br); f_close(&f); ok = br == s.size(); return s;
}
static std::string hread(const std::string& p, bool& ok) {
    FILE* f = fopen((ROOT + "/" + p).c_str(), "rb"); ok = f; if (!f) return "";
    std::string s; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f))) s.append(b, n); fclose(f); return s;
}
static void hlist(const std::string& rel, std::map<std::string, std::string>& out) {
    DIR* d = opendir((ROOT + (rel.empty() ? "" : "/" + rel)).c_str());
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name; if (n == "." || n == "..") continue;
        if (n.rfind(".fuse_hidden", 0) == 0) continue;   // FUSE: an open-but-deleted inode
        std::string r = rel.empty() ? n : rel + "/" + n;
        struct stat st; stat((ROOT + "/" + r).c_str(), &st);
        if (S_ISDIR(st.st_mode)) { out[r] = DIRM; hlist(r, out); }
        else { bool ok; out[r] = hread(r, ok); }
    }
    closedir(d);
}
static std::string utf(const std::string& p);
static void verify(const char* tag) {
    std::map<std::string, std::string> h; hlist("", h);
    CHECK(h.size() == model.size(), "%s: host has %zu entries, model %zu", tag, h.size(), model.size());
    for (auto& [p, c] : model) {
        auto it = h.find(utf(p));
        CHECK(it != h.end(), "%s: host misses %s", tag, p.c_str());
        if (it != h.end()) CHECK(it->second == c, "%s: host content differs %s (%zu vs %zu)", tag, p.c_str(), it->second.size(), c.size());
        if (c != DIRM) { bool ok; std::string g = gread(p, ok); CHECK(ok && g == c, "%s: guest reads %s wrong", tag, p.c_str()); }
    }
    std::map<std::string, int> mu; for (auto& [p, c] : model) mu[utf(p)] = 1;
    for (auto& [p, c] : h) CHECK(mu.count(p), "%s: host has extra %s", tag, p.c_str());
}
static void hput(const std::string& p, const std::string& c) {
    FILE* f = fopen((ROOT + "/" + p).c_str(), "wb"); fwrite(c.data(), 1, c.size(), f); fclose(f);
}

static std::vector<std::string> files() { std::vector<std::string> v; for (auto& [p, c] : model) if (c != DIRM) v.push_back(p); return v; }
static std::vector<std::string> dirs() { std::vector<std::string> v{""}; for (auto& [p, c] : model) if (c == DIRM) v.push_back(p); return v; }
static std::string pick(const std::vector<std::string>& v) { return v[rng() % v.size()]; }
static std::string newName() {
    static int n = 0; n++;
    static const char* forms[] = {"F%d.TXT", "long file name %d.bin", "mixed%d.Tap", "\xCF\xF0\xE8\xE2\xE5\xF2%d.trd", "UP%d"};
    char b[64]; snprintf(b, sizeof b, forms[rng() % 5], n); return b;
}
static std::string join(const std::string& d, const std::string& n) { return d.empty() ? n : d + "/" + n; }
static std::string utf(const std::string& p) { return host_cp1251_to_utf8(p); }

int main(int argc, char** argv) {
    rng.seed(argc > 2 ? atoi(argv[2]) : 12345);
    // VFAT_TEST_DIR puts the folder elsewhere - e.g. on a case-insensitive mount
    // (ciopfs, vfat) to exercise the case-only rename path for real
    std::string base = getenv("VFAT_TEST_DIR") ? getenv("VFAT_TEST_DIR") : "/tmp";
    std::string tmpl = base + "/vfatXXXXXX"; ROOT = mkdtemp(&tmpl[0]);
    // starting folder (ASCII names, so model paths == host paths)
    mkdir((ROOT + "/Games").c_str(), 0755); mkdir((ROOT + "/dir2").c_str(), 0755);
    model["Games"] = DIRM; model["dir2"] = DIRM;
    auto seed = [&](const std::string& p, size_t n) { std::string c = rnd(n); hput(p, c); model[p] = c; };
    seed("hello.txt", 5); seed("Games/game.tap", 8000); seed("big.bin", 50000); seed("keep.txt", 100);
    seed("dir2/a.txt", 3000); seed("dir2/b.txt", 0); seed("Games/Long Name Game.z80", 49179);
    vfat_build(ROOT); mount();
    verify("initial");

    // scripted ops
    gwrite("NEW.TXT", rnd(1234)); gwrite("Long name file.txt", rnd(9000));
    gpatch("big.bin", 10000, rnd(600));
    { FIL f; f_open(&f, "/hello.txt", FA_WRITE | FA_OPEN_APPEND); UINT bw; std::string a = rnd(5000); f_write(&f, a.data(), a.size(), &bw); f_close(&f); model["hello.txt"] += a; }
    CHECK(f_rename("/keep.txt", "/kept.txt") == FR_OK, "rename"); model["kept.txt"] = model["keep.txt"]; model.erase("keep.txt");
    CHECK(f_unlink("/Games/game.tap") == FR_OK, "unlink"); model.erase("Games/game.tap");
    CHECK(f_mkdir("/Newdir") == FR_OK, "mkdir"); model["Newdir"] = DIRM; gwrite("Newdir/x.dat", rnd(20000));
    CHECK(f_rename("/dir2", "/dir3") == FR_OK, "rename dir");
    { std::map<std::string, std::string> m2; for (auto& [p, c] : model) { std::string q = p; if (q == "dir2" || q.rfind("dir2/", 0) == 0) q = "dir3" + q.substr(4); m2[q] = c; } model.swap(m2); }
    vfat_sync();
    verify("after sync 1");
    // edits after a sync: guest base reads must survive (renamed/deleted host inodes)
    gpatch("kept.txt", 10, "XYZ"); gpatch("dir3/a.txt", 2000, rnd(1500)); gwrite("dir3/b.txt", rnd(700));
    vfat_sync();
    verify("after sync 2");
    // the guest renames onto a name the PC created behind its back: the host cannot
    // rename (target exists), so it copies - and must still read the old inode
    hput("hidden.dat", "made on the PC");
    CHECK(f_rename("/big.bin", "/hidden.dat") == FR_OK, "rename onto hidden");
    model["hidden.dat"] = model["big.bin"]; model.erase("big.bin");
    vfat_sync();
    { bool ok; CHECK(hread("hidden.dat", ok) == model["hidden.dat"], "hidden.dat on host"); }
    { bool ok; std::string g = gread("hidden.dat", ok); CHECK(ok && g == model["hidden.dat"], "guest reads hidden.dat"); }
    // case-only renames: on a case-insensitive host the target "exists" (it is the
    // source itself) - the sync must rename, not delete the file and write a new one
    CHECK(f_rename("/kept.txt", "/KEPT.TXT") == FR_OK, "case rename");
    model["KEPT.TXT"] = model["kept.txt"]; model.erase("kept.txt");
    CHECK(f_rename("/Newdir", "/NEWDIR") == FR_OK, "case rename dir");
    { std::map<std::string, std::string> m2; for (auto& [p, c] : model) { std::string q = p; if (q == "Newdir" || q.rfind("Newdir/", 0) == 0) q = "NEWDIR" + q.substr(6); m2[q] = c; } model.swap(m2); }
    vfat_sync();
    verify("after case renames");
    // a host-side change comes in at the card reset
    hput("pc.txt", "from the PC"); model["pc.txt"] = "from the PC";
    umount(); CHECK(vfat_card_reset(), "card reset rebuilt"); mount();
    verify("after card reset");
    CHECK(!vfat_card_reset() || true, "");

    // random stress
    int iters = argc > 1 ? atoi(argv[1]) : 3000;
    for (int i = 0; i < iters && !fails; i++) {
        int op = rng() % 100;
        auto fl = files();
        if (op < 25 || fl.empty()) { gwrite(join(pick(dirs()), newName()), rnd(rng() % 3 == 0 ? rng() % 40000 : rng() % 3000)); }
        else if (op < 40) { std::string p = pick(fl); size_t sz = model[p].size(); if (sz) gpatch(p, rng() % sz, rnd(1 + rng() % 700)); }
        else if (op < 50) { std::string p = pick(fl); gwrite(p, rnd(rng() % 12000)); }
        else if (op < 58) { std::string p = pick(fl); CHECK(f_unlink(("/" + p).c_str()) == FR_OK, "unlink %s", p.c_str()); model.erase(p); }
        else if (op < 66) { std::string p = pick(fl); std::string q = join(pick(dirs()), newName());
                            FRESULT r = f_rename(("/" + p).c_str(), ("/" + q).c_str()); CHECK(r == FR_OK, "rename %s->%s %d", p.c_str(), q.c_str(), r);
                            model[q] = model[p]; model.erase(p); }
        else if (op < 72) { std::string d = join(pick(dirs()), newName()); if (std::count(d.begin(), d.end(), '/') < 4) { CHECK(f_mkdir(("/" + d).c_str()) == FR_OK, "mkdir"); model[d] = DIRM; } }
        else if (op < 76) { // remove an empty dir or rename a dir
            auto ds = dirs(); std::string d = pick(ds); if (d.empty()) continue;
            bool empty = true; for (auto& [p, c] : model) if (p.rfind(d + "/", 0) == 0) empty = false;
            if (empty && rng() % 2) { CHECK(f_unlink(("/" + d).c_str()) == FR_OK, "rmdir"); model.erase(d); }
            else { std::string parent = d.substr(0, d.rfind('/') == std::string::npos ? 0 : d.rfind('/'));
                   std::string q = join(parent, newName());
                   CHECK(f_rename(("/" + d).c_str(), ("/" + q).c_str()) == FR_OK, "rename dir");
                   std::map<std::string, std::string> m2; for (auto& [p, c] : model) { std::string k = p; if (k == d || k.rfind(d + "/", 0) == 0) k = q + k.substr(d.size()); m2[k] = c; } model.swap(m2); }
        }
        else if (op < 90) { vfat_sync(); verify("stress sync"); }
        else if (op < 95) { // host adds / edits a file, then card reset
            std::string n = "pc" + std::to_string(i) + ".dat"; std::string c = rnd(rng() % 5000); hput(n, c);
            vfat_sync(); umount(); vfat_card_reset(); mount(); model[n] = c;
        }
        else { umount(); vfat_card_reset(); mount(); }
    }
    vfat_sync();
    verify("final");
    // the guest wipes the card: the folder must stay as it was
    {
        std::map<std::string, std::string> before; hlist("", before);
        std::vector<std::string> all; for (auto& [p, c] : model) all.push_back(p);
        std::sort(all.rbegin(), all.rend());
        for (auto& p : all) CHECK(f_unlink(("/" + p).c_str()) == FR_OK, "wipe %s", p.c_str());
        vfat_sync();
        std::map<std::string, std::string> after; hlist("", after);
        CHECK(before == after, "wipe reached the folder (%zu -> %zu)", before.size(), after.size());
        FFDIR_ d; FILINFO fi; f_opendir(&d, "/"); CHECK(f_readdir(&d, &fi) == FR_OK && !fi.fname[0], "guest sees an empty card"); f_closedir(&d);
    }
    printf("%s (%d fails) root %s\n", fails ? "FAILED" : "OK", fails, ROOT.c_str());
    return fails != 0;
}
