// Windows implementations behind host_posix.h. Kept apart from the rest of the host
// code because windows.h cannot meet the firmware headers (see host_posix.h).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <io.h>
#include <sys/utime.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host_posix.h"

// The default Windows timer tick is 15.6 ms: every sleep of 1-2 ms in the frame
// pacing, the audio and the core1 loop would last a whole tick (a 20 ms frame took
// ~31 ms - 32 FPS with idle time to spare). 1 ms for the life of the process.
static struct TimerRes {
    TimerRes() { timeBeginPeriod(1); }
    ~TimerRes() { timeEndPeriod(1); }
} s_timerRes;

static ssize_t pio(int fd, void* buf, size_t n, int64_t off, bool wr) {
    const HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov{};
        const uint64_t at = (uint64_t)off + done;
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        const DWORD chunk = (DWORD)((n - done) > 0x40000000u ? 0x40000000u : (n - done));
        DWORD got = 0;
        const BOOL ok = wr ? WriteFile(h, (const char*)buf + done, chunk, &got, &ov)
                           : ReadFile(h, (char*)buf + done, chunk, &got, &ov);
        if (!ok) {
            if (!wr && GetLastError() == ERROR_HANDLE_EOF) break;
            if (done) break;
            errno = EIO;
            return -1;
        }
        done += got;
        if (got < chunk) break;
    }
    return (ssize_t)done;
}

extern "C" ssize_t host_pread(int fd, void* buf, size_t n, int64_t off) { return pio(fd, buf, n, off, false); }
extern "C" ssize_t host_pwrite(int fd, const void* buf, size_t n, int64_t off) {
    return pio(fd, const_cast<void*>(buf), n, off, true);
}

extern "C" int host_disk_space(const char* path, uint64_t* total, uint64_t* avail) {
    ULARGE_INTEGER a, t, f;
    if (!GetDiskFreeSpaceExA(path, &a, &t, &f)) return -1;
    if (total) *total = t.QuadPart;
    if (avail) *avail = a.QuadPart;
    return 0;
}

extern "C" int host_set_mtime(const char* path, time_t t) {
    struct __utimbuf64 u;
    u.actime = t;
    u.modtime = t;
    return _utime64(path, &u);
}

extern "C" int host_rename_replace(const char* from, const char* to) {
    const DWORD fl = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    BOOL ok;
    if (strcmp(from, to) != 0 && _stricmp(from, to) == 0) {
        // Only the letter case changes: the target "exists" (it is the source), and
        // a direct move may do nothing. Go through a temporary name.
        const std::string tmp = std::string(to) + ".~pspeccy";
        ok = MoveFileExA(from, tmp.c_str(), fl);
        if (ok && !(ok = MoveFileExA(tmp.c_str(), to, fl))) MoveFileExA(tmp.c_str(), from, fl);
    } else {
        ok = MoveFileExA(from, to, fl);
    }
    if (ok) return 0;
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
    default: errno = EIO; break;
    }
    return -1;
}

extern "C" int host_mem_rw(void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi)) return -1;
    if (mi.State != MEM_COMMIT && !VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE)) return -1;
    DWORD old;
    return VirtualProtect(p, n, PAGE_READWRITE, &old) ? 0 : -1;
}

extern "C" const char* host_exe_path(void) {
    static char p[MAX_PATH * 4];
    if (!p[0]) GetModuleFileNameA(nullptr, p, sizeof p);
    return p;
}

extern "C" int host_setenv_default(const char* name, const char* value) {
    if (getenv(name)) return 0;
    return _putenv_s(name, value);
}
#endif  // _WIN32
