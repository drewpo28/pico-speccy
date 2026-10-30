// Directory listing for host_fatfs.cpp. A separate file because FatFs and POSIX both
// define a type called DIR, so ff.h and <dirent.h> cannot meet in one translation unit.
#include <dirent.h>
#include <sys/stat.h>
#include "host_posix.h"
#include <string>

struct HostDir {
    ::DIR* d;
    std::string path;
};

extern "C" void* hd_open(const char* path) {
    ::DIR* d = opendir(path);
    if (!d) return nullptr;
    return new HostDir{d, path};
}

// Next entry, "." and ".." skipped. Returns nullptr at the end. st is filled with
// the entry's stat (symlinks followed; an entry that cannot be stat'ed is skipped).
extern "C" const char* hd_next(void* h, struct stat* st) {
    HostDir* hd = (HostDir*)h;
    for (;;) {
        struct dirent* e = readdir(hd->d);
        if (!e) return nullptr;
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
#ifdef _WIN32
        if (stat((hd->path + "/" + e->d_name).c_str(), st) != 0) continue;
#else
        if (fstatat(dirfd(hd->d), e->d_name, st, 0) != 0) continue;
#endif
        return e->d_name;
    }
}

extern "C" void hd_rewind(void* h) { rewinddir(((HostDir*)h)->d); }

extern "C" void hd_close(void* h) {
    HostDir* hd = (HostDir*)h;
    closedir(hd->d);
    delete hd;
}
