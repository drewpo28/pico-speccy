// The POSIX calls the host/*.cpp files use, made to work on Windows (mingw-w64).
// On Linux/macOS this header only fills in O_BINARY; on Windows it maps the calls to
// their CRT twins or to host_posix_win.cpp. windows.h is NOT included here: half the
// firmware headers say `using namespace std`, and std::byte meets windows.h's byte.
#pragma once
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0   // Windows opens files in text mode without it
#endif

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
// Positional I/O on a CRT descriptor (ReadFile/WriteFile with an explicit offset).
ssize_t host_pread(int fd, void* buf, size_t n, int64_t off);
ssize_t host_pwrite(int fd, const void* buf, size_t n, int64_t off);
// Free / total bytes of the volume holding `path`.
int host_disk_space(const char* path, uint64_t* total, uint64_t* avail);
// Sets a file's modification time.
int host_set_mtime(const char* path, time_t t);
// rename() that replaces an existing target, as POSIX rename does.
int host_rename_replace(const char* from, const char* to);
// The running .exe's path.
const char* host_exe_path(void);
// Makes [p, p+n) of the image readable and writable (committing it if needed).
int host_mem_rw(void* p, size_t n);
// setenv(name, value, 0): set only if not set yet.
int host_setenv_default(const char* name, const char* value);
#ifdef __cplusplus
}
#endif

static inline struct tm* host_localtime_r(const time_t* t, struct tm* r) {
    return localtime_s(r, t) == 0 ? r : NULL;
}
#define localtime_r host_localtime_r
#define pread(fd, b, n, off) host_pread((fd), (b), (n), (int64_t)(off))
#define pwrite(fd, b, n, off) host_pwrite((fd), (b), (n), (int64_t)(off))
#define fsync(fd) _commit(fd)
#define mkdir(p, mode) _mkdir(p)
#define lstat stat
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#endif  // _WIN32
