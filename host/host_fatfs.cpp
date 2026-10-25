// The FatFs API over the host filesystem: the host build's "SD card" is an ordinary
// directory (host_root, default ~/pico-speccy, --root DIR). Replaces external/fatfs/ff.c,
// so the whole firmware - browser, Config, snapshots, disk images, tapes - reads and
// writes plain files on the PC.
//
// What it keeps from FAT, because the firmware relies on it:
//  - names are CP1251 (FF_CODE_PAGE 1251, FF_LFN_UNICODE 0); host names are UTF-8 and
//    are converted both ways;
//  - lookup is case-insensitive (an exact host name is tried first, then the directory
//    is scanned);
//  - volumes "SD:" / "0:" (this directory) and "USB:" / "1:" (never present), one
//    current directory, f_chdir / f_getcwd in FatFs form ("SD:/dir");
//  - f_lseek past the end extends a file opened for writing and clips otherwise;
//  - an open FIL has obj.fs != NULL (Tape tests that), f_size / f_tell / f_eof work
//    through obj.objsize / fptr.
// The raw card (disk_read for DivSD, Z-Controller, NeoGS SD) is separate: host_disk.cpp,
// an optional image given with --sd.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
#include "ff.h"
void* hd_open(const char* path);
const char* hd_next(void* h, struct stat* st);
void hd_rewind(void* h);
void hd_close(void* h);
}

std::string host_root;   // set by main(); empty = ~/pico-speccy

static std::recursive_mutex s_mx;
static FATFS s_fs;              // what obj.fs points at for every open object
static int s_curvol = 0;
static std::string s_cwd = "/"; // CP1251, absolute, no trailing '/' except the root

// ---- CP1251 <-> UTF-8 -----------------------------------------------------------------
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
std::string host_cp1251_to_utf8(const std::string& s) { return cpToUtf8(s); }   // host_vfat.cpp
static std::string utf8ToCp(const char* s) {
    std::string o;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        uint32_t u; int n;
        if (*p < 0x80) { u = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { u = ((*p & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
        else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            u = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3;
        } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            u = 0xFFFF; n = 4;
        } else { u = 0xFFFF; n = 1; }
        int c = uniToCp(u);
        o += (char)(c < 0 ? '_' : c);
        p += n;
    }
    return o;
}
static unsigned char cpFold(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDF) return c + 32;
    if (c == 0xA8) return 0xB8;
    return c;
}
static bool cpEqualNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (cpFold((unsigned char)a[i]) != cpFold((unsigned char)b[i])) return false;
    return true;
}

// ---- paths ----------------------------------------------------------------------------
static const std::string& rootDir();
const std::string& host_root_dir() { return rootDir(); }   // host_disk.cpp
static const std::string& rootDir() {
    if (host_root.empty()) {
        const char* h = getenv("HOME");
        host_root = std::string(h && *h ? h : ".") + "/pico-speccy";
    }
    return host_root;
}

static void mkdirs(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
        if (i < p.size()) cur += p[i];
    }
}

// Splits "SD:/a/b", "0:x", "a/../b" into the volume and normalized components.
static FRESULT parsePath(const TCHAR* path, int& vol, std::vector<std::string>& comps) {
    std::string s = path ? path : "";
    vol = s_curvol;
    size_t colon = s.find(':');
    size_t slash = s.find_first_of("/\\");
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
        std::string id = s.substr(0, colon);
        if (cpEqualNoCase(id, "SD") || id == "0") vol = 0;
        else if (cpEqualNoCase(id, "USB") || id == "1") vol = 1;
        else return FR_INVALID_DRIVE;
        s = s.substr(colon + 1);
    }
    std::string base = (vol == 0 && !s.empty() && s[0] != '/' && s[0] != '\\') ? s_cwd : std::string("/");
    if (vol != 0 && (s.empty() || (s[0] != '/' && s[0] != '\\'))) base = "/";
    std::string full = base + "/" + s;
    comps.clear();
    std::string cur;
    for (size_t i = 0; i <= full.size(); i++) {
        char c = i < full.size() ? full[i] : '/';
        if (c == '/' || c == '\\') {
            if (cur == "..") { if (!comps.empty()) comps.pop_back(); }
            else if (!cur.empty() && cur != ".") comps.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    return FR_OK;
}

struct Resolved {
    std::string host;   // host path (UTF-8)
    bool exists = false;
    struct stat st {};
};

// Case-insensitive walk from the root. The last component may be missing (exists =
// false, host = the name it would get); a missing intermediate one is FR_NO_PATH.
static FRESULT resolve(const std::vector<std::string>& comps, Resolved& r) {
    std::string host = rootDir();
    mkdirs(host);
    for (size_t i = 0; i < comps.size(); i++) {
        const bool last = i + 1 == comps.size();
        std::string cand = host + "/" + cpToUtf8(comps[i]);
        struct stat st;
        bool found = stat(cand.c_str(), &st) == 0;
        if (!found) {
            if (void* d = hd_open(host.c_str())) {
                while (const char* n = hd_next(d, &st)) {
                    if (cpEqualNoCase(utf8ToCp(n), comps[i])) { cand = host + "/" + n; found = true; break; }
                }
                hd_close(d);
            }
        }
        if (!found) {
            if (!last) return FR_NO_PATH;
            r.host = cand; r.exists = false;
            return FR_OK;
        }
        if (!last && !S_ISDIR(st.st_mode)) return FR_NO_PATH;
        host = cand;
        if (last) { r.st = st; r.exists = true; }
    }
    if (comps.empty()) { r.exists = true; stat(host.c_str(), &r.st); }
    r.host = host;
    return FR_OK;
}

static FRESULT lookup(const TCHAR* path, std::vector<std::string>& comps, Resolved& r) {
    int vol;
    FRESULT fr = parsePath(path, vol, comps);
    if (fr != FR_OK) return fr;
    if (vol != 0) return FR_NOT_READY;
    return resolve(comps, r);
}

static FRESULT errnoResult(int e) {
    switch (e) {
    case ENOENT: return FR_NO_FILE;
    case EEXIST: return FR_EXIST;
    case EACCES: case EPERM: case ENOTEMPTY: case EISDIR: return FR_DENIED;
    case EROFS: return FR_WRITE_PROTECTED;
    case ENAMETOOLONG: case EINVAL: return FR_INVALID_NAME;
    default: return FR_DISK_ERR;
    }
}

static void fillInfo(FILINFO* fno, const std::string& cpName, const struct stat& st) {
    if (!fno) return;
    memset(fno, 0, sizeof(*fno));
    fno->fsize = S_ISDIR(st.st_mode) ? 0 : (FSIZE_t)st.st_size;
    struct tm tm;
    time_t t = st.st_mtime;
    localtime_r(&t, &tm);
    int y = tm.tm_year + 1900 - 1980;
    if (y < 0) y = 0;
    if (y > 127) y = 127;
    fno->fdate = (WORD)((y << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    fno->ftime = (WORD)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    BYTE a = S_ISDIR(st.st_mode) ? AM_DIR : AM_ARC;
    if (!(st.st_mode & S_IWUSR)) a |= AM_RDO;
    if (!cpName.empty() && cpName[0] == '.') a |= AM_HID;
    fno->fattrib = a;
    strncpy(fno->fname, cpName.c_str(), FF_LFN_BUF);
    fno->altname[0] = 0;
}

static inline int fdOf(const FIL* fp) { return (int)fp->obj.sclust; }
#define CHECK_FIL(fp) do { if (!(fp) || (fp)->obj.fs != &s_fs) return FR_INVALID_OBJECT; } while (0)

extern "C" {

FRESULT f_mount(FATFS* fs, const TCHAR* path, BYTE opt) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> comps; int vol;
    FRESULT fr = parsePath(path, vol, comps);
    if (fr != FR_OK) return fr;
    if (vol != 0) return opt ? FR_NOT_READY : FR_OK;
    mkdirs(rootDir());
    s_fs.fs_type = FS_EXFAT;
    if (fs) fs->fs_type = FS_EXFAT;
    return FR_OK;
}

FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode) {
    if (!fp) return FR_INVALID_OBJECT;
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    fp->obj.fs = nullptr;
    std::vector<std::string> comps; Resolved r;
    FRESULT fr = lookup(path, comps, r);
    if (fr != FR_OK) return fr;
    if (comps.empty()) return FR_INVALID_NAME;
    if (r.exists && S_ISDIR(r.st.st_mode)) return FR_NO_FILE;
    int flags = (mode & FA_WRITE) ? O_RDWR : O_RDONLY;
    if (mode & FA_CREATE_NEW) {
        if (r.exists) return FR_EXIST;
        flags |= O_CREAT | O_EXCL;
    } else if (mode & FA_CREATE_ALWAYS) {
        flags |= O_CREAT | O_TRUNC;
    } else if (mode & FA_OPEN_ALWAYS) {
        flags |= O_CREAT;
    } else if (!r.exists) {
        return FR_NO_FILE;
    }
    if ((flags & O_CREAT) && !(mode & FA_WRITE)) flags = (flags & ~O_RDONLY) | O_RDWR;
    int fd = open(r.host.c_str(), flags, 0644);
    if (fd < 0) return errnoResult(errno);
    struct stat st;
    fstat(fd, &st);
    memset(fp, 0, sizeof(*fp));
    fp->obj.fs = &s_fs;
    fp->obj.sclust = (DWORD)fd;
    fp->obj.objsize = (FSIZE_t)st.st_size;
    fp->obj.attr = AM_ARC;
    fp->flag = mode & (FA_READ | FA_WRITE);
    fp->fptr = ((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND) ? fp->obj.objsize : 0;
    return FR_OK;
}

FRESULT f_close(FIL* fp) {
    CHECK_FIL(fp);
    close(fdOf(fp));
    fp->obj.fs = nullptr;
    return FR_OK;
}

FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br) {
    if (br) *br = 0;
    CHECK_FIL(fp);
    if (!(fp->flag & FA_READ)) return FR_DENIED;
    if (fp->fptr >= fp->obj.objsize) return FR_OK;
    FSIZE_t left = fp->obj.objsize - fp->fptr;
    if (btr > left) btr = (UINT)left;
    ssize_t n = pread(fdOf(fp), buff, btr, (off_t)fp->fptr);
    if (n < 0) { fp->err = FR_DISK_ERR; return FR_DISK_ERR; }
    fp->fptr += (FSIZE_t)n;
    if (br) *br = (UINT)n;
    return FR_OK;
}

FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw) {
    if (bw) *bw = 0;
    CHECK_FIL(fp);
    if (!(fp->flag & FA_WRITE)) return FR_DENIED;
    ssize_t n = pwrite(fdOf(fp), buff, btw, (off_t)fp->fptr);
    if (n < 0) { fp->err = FR_DISK_ERR; return FR_DISK_ERR; }
    fp->fptr += (FSIZE_t)n;
    if (fp->fptr > fp->obj.objsize) fp->obj.objsize = fp->fptr;
    if (bw) *bw = (UINT)n;
    return FR_OK;
}

FRESULT f_lseek(FIL* fp, FSIZE_t ofs) {
    CHECK_FIL(fp);
    if (fp->cltbl && ofs == CREATE_LINKMAP) {
        // Fast seek has nothing to map here; report a table that fits.
        if (fp->cltbl[0] < 4) { fp->cltbl[0] = 4; return FR_NOT_ENOUGH_CORE; }
        fp->cltbl[0] = 4;
        return FR_OK;
    }
    if (ofs > fp->obj.objsize) {
        if ((fp->flag & FA_WRITE) && !fp->cltbl) {
            if (ftruncate(fdOf(fp), (off_t)ofs) != 0) return FR_DISK_ERR;
            fp->obj.objsize = ofs;
        } else ofs = fp->obj.objsize;
    }
    fp->fptr = ofs;
    return FR_OK;
}

FRESULT f_truncate(FIL* fp) {
    CHECK_FIL(fp);
    if (!(fp->flag & FA_WRITE)) return FR_DENIED;
    if (fp->fptr < fp->obj.objsize) {
        if (ftruncate(fdOf(fp), (off_t)fp->fptr) != 0) return FR_DISK_ERR;
        fp->obj.objsize = fp->fptr;
    }
    return FR_OK;
}

FRESULT f_sync(FIL* fp) { CHECK_FIL(fp); return FR_OK; }

FRESULT f_expand(FIL* fp, FSIZE_t fsz, BYTE opt) {
    CHECK_FIL(fp);
    if (!(fp->flag & FA_WRITE)) return FR_DENIED;
    if (opt && fsz > fp->obj.objsize) {
        if (ftruncate(fdOf(fp), (off_t)fsz) != 0) return FR_DENIED;
        fp->obj.objsize = fsz;
    }
    return FR_OK;
}

FRESULT f_opendir(DIR* dp, const TCHAR* path) {
    if (!dp) return FR_INVALID_OBJECT;
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    dp->obj.fs = nullptr;
    std::vector<std::string> comps; Resolved r;
    FRESULT fr = lookup(path, comps, r);
    if (fr != FR_OK) return fr;
    if (!r.exists || !S_ISDIR(r.st.st_mode)) return FR_NO_PATH;
    void* h = hd_open(r.host.c_str());
    if (!h) return errnoResult(errno);
    memset(dp, 0, sizeof(*dp));
    dp->obj.fs = &s_fs;
    dp->dir = (BYTE*)h;
    return FR_OK;
}

FRESULT f_closedir(DIR* dp) {
    if (!dp || dp->obj.fs != &s_fs) return FR_INVALID_OBJECT;
    hd_close(dp->dir);
    dp->obj.fs = nullptr;
    dp->dir = nullptr;
    return FR_OK;
}

FRESULT f_readdir(DIR* dp, FILINFO* fno) {
    if (!dp || dp->obj.fs != &s_fs) return FR_INVALID_OBJECT;
    if (!fno) { hd_rewind(dp->dir); return FR_OK; }
    struct stat st;
    const char* n = hd_next(dp->dir, &st);
    if (!n) { memset(fno, 0, sizeof(*fno)); return FR_OK; }
    fillInfo(fno, utf8ToCp(n), st);
    return FR_OK;
}

FRESULT f_stat(const TCHAR* path, FILINFO* fno) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> comps; Resolved r;
    FRESULT fr = lookup(path, comps, r);
    if (fr != FR_OK) return fr;
    if (comps.empty()) return FR_INVALID_NAME;
    if (!r.exists) return FR_NO_FILE;
    const char* slash = strrchr(r.host.c_str(), '/');
    fillInfo(fno, utf8ToCp(slash ? slash + 1 : r.host.c_str()), r.st);
    return FR_OK;
}

FRESULT f_unlink(const TCHAR* path) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> comps; Resolved r;
    FRESULT fr = lookup(path, comps, r);
    if (fr != FR_OK) return fr;
    if (comps.empty()) return FR_INVALID_NAME;
    if (!r.exists) return FR_NO_FILE;
    int rc = S_ISDIR(r.st.st_mode) ? rmdir(r.host.c_str()) : unlink(r.host.c_str());
    return rc == 0 ? FR_OK : errnoResult(errno);
}

FRESULT f_rename(const TCHAR* path_old, const TCHAR* path_new) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> c1, c2; Resolved r1, r2;
    FRESULT fr = lookup(path_old, c1, r1);
    if (fr != FR_OK) return fr;
    if (c1.empty()) return FR_INVALID_NAME;
    if (!r1.exists) return FR_NO_FILE;
    fr = lookup(path_new, c2, r2);
    if (fr != FR_OK) return fr;
    if (c2.empty()) return FR_INVALID_NAME;
    if (r2.exists && !(r2.st.st_ino == r1.st.st_ino && r2.st.st_dev == r1.st.st_dev)) return FR_EXIST;
    // A case-only rename finds the old file under the new name: give it the new spelling.
    if (r2.exists) {
        std::string dir = r2.host.substr(0, r2.host.rfind('/'));
        r2.host = dir + "/" + cpToUtf8(c2.back());
    }
    return rename(r1.host.c_str(), r2.host.c_str()) == 0 ? FR_OK : errnoResult(errno);
}

FRESULT f_mkdir(const TCHAR* path) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> comps; Resolved r;
    FRESULT fr = lookup(path, comps, r);
    if (fr != FR_OK) return fr;
    if (comps.empty() || r.exists) return FR_EXIST;
    return mkdir(r.host.c_str(), 0755) == 0 ? FR_OK : errnoResult(errno);
}

FRESULT f_utime(const TCHAR* path, const FILINFO* fno) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> comps; Resolved r;
    FRESULT fr = lookup(path, comps, r);
    if (fr != FR_OK) return fr;
    if (!r.exists) return FR_NO_FILE;
    struct tm tm {};
    tm.tm_year = (fno->fdate >> 9) + 80;
    tm.tm_mon = ((fno->fdate >> 5) & 15) - 1;
    tm.tm_mday = fno->fdate & 31;
    tm.tm_hour = fno->ftime >> 11;
    tm.tm_min = (fno->ftime >> 5) & 63;
    tm.tm_sec = (fno->ftime & 31) * 2;
    tm.tm_isdst = -1;
    struct timeval tv[2] = {{mktime(&tm), 0}, {mktime(&tm), 0}};
    return utimes(r.host.c_str(), tv) == 0 ? FR_OK : errnoResult(errno);
}

FRESULT f_chdir(const TCHAR* path) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> comps; Resolved r;
    FRESULT fr = lookup(path, comps, r);
    if (fr != FR_OK) return fr;
    if (!r.exists || !S_ISDIR(r.st.st_mode)) return FR_NO_PATH;
    std::string cwd;
    for (auto& c : comps) cwd += "/" + c;
    s_cwd = cwd.empty() ? "/" : cwd;
    return FR_OK;
}

FRESULT f_chdrive(const TCHAR* path) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> comps; int vol;
    FRESULT fr = parsePath(path, vol, comps);
    if (fr != FR_OK) return fr;
    s_curvol = vol;
    return FR_OK;
}

FRESULT f_getcwd(TCHAR* buff, UINT len) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::string s = std::string(s_curvol ? "USB:" : "SD:") + (s_curvol ? "/" : s_cwd);
    if (!buff || len == 0) return FR_INVALID_PARAMETER;
    if (s.size() + 1 > len) return FR_NOT_ENOUGH_CORE;
    memcpy(buff, s.c_str(), s.size() + 1);
    return FR_OK;
}

FRESULT f_getfree(const TCHAR* path, DWORD* nclst, FATFS** fatfs) {
    std::lock_guard<std::recursive_mutex> lk(s_mx);
    std::vector<std::string> comps; int vol;
    FRESULT fr = parsePath(path, vol, comps);
    if (fr != FR_OK) return fr;
    if (vol != 0) return FR_NOT_READY;
    struct statvfs sv;
    if (statvfs(rootDir().c_str(), &sv) != 0) return FR_DISK_ERR;
    const uint64_t clusterBytes = 32768;
    uint64_t total = (uint64_t)sv.f_blocks * sv.f_frsize / clusterBytes;
    uint64_t freec = (uint64_t)sv.f_bavail * sv.f_frsize / clusterBytes;
    if (total > 0xFFFFFFF0ull) total = 0xFFFFFFF0ull;
    if (freec > total) freec = total;
    s_fs.fs_type = FS_EXFAT;
    s_fs.csize = (WORD)(clusterBytes / 512);
    s_fs.n_fatent = (DWORD)total + 2;
    s_fs.free_clst = (DWORD)freec;
    if (nclst) *nclst = (DWORD)freec;
    if (fatfs) *fatfs = &s_fs;
    return FR_OK;
}

FRESULT f_getlabel(const TCHAR* path, TCHAR* label, DWORD* vsn) {
    std::vector<std::string> comps; int vol;
    FRESULT fr = parsePath(path, vol, comps);
    if (fr != FR_OK) return fr;
    if (vol != 0) return FR_NOT_READY;
    if (label) strcpy(label, "PICOSPECCY");
    if (vsn) *vsn = 0x50435350;   // constant: the "card" never changes under us
    return FR_OK;
}

FRESULT f_setlabel(const TCHAR*) { return FR_OK; }

}  // extern "C"

// ff.c's firmware extras (PICO-SPEC PATCHes) that callers link against.
extern "C" FIL* fopen2(const TCHAR* path, BYTE mode) {
    FIL* f = (FIL*)malloc(sizeof(FIL));
    if (!f) return nullptr;
    if (f_open(f, path, mode) != FR_OK) { free(f); return nullptr; }
    return f;
}
extern "C" void fclose2(FIL* f) {
    if (!f) return;
    f_close(f);
    free(f);
}
// A guest wrote the raw card: nothing cached here (the raw card is a separate image).
extern "C" void ff_raw_write_notify(BYTE, LBA_t, const BYTE*) {}
