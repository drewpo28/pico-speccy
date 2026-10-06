// Host stand-in for FatFs, just enough for src/speccy/core/PssExport.cpp
// (tools/pss_export_test.cpp). stdio underneath.
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint64_t FSIZE_t;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_NO_FILE = 4, FR_EXIST = 8 } FRESULT;
struct FIL { FILE* fp; };
struct FILINFO { FSIZE_t fsize; };
#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_ALWAYS 0x08
inline FIL* fopen2(const char* p, BYTE mode) {
    FILE* f = fopen(p, (mode & FA_CREATE_ALWAYS) ? "w+b" : (mode & FA_WRITE) ? "r+b" : "rb");
    if (!f) return nullptr;
    FIL* r = new FIL; r->fp = f; return r;
}
inline void fclose2(FIL* f) { if (f) { fclose(f->fp); delete f; } }
inline FRESULT f_read(FIL* f, void* b, UINT n, UINT* br) { *br = (UINT)fread(b, 1, n, f->fp); return FR_OK; }
inline FRESULT f_write(FIL* f, const void* b, UINT n, UINT* bw) { *bw = (UINT)fwrite(b, 1, n, f->fp); return FR_OK; }
inline FRESULT f_lseek(FIL* f, FSIZE_t o) { return fseek(f->fp, (long)o, SEEK_SET) ? FR_DISK_ERR : FR_OK; }
inline FSIZE_t f_tell(FIL* f) { return (FSIZE_t)ftell(f->fp); }
inline FSIZE_t f_size(FIL* f) { long c = ftell(f->fp); fseek(f->fp, 0, SEEK_END); long e = ftell(f->fp); fseek(f->fp, c, SEEK_SET); return (FSIZE_t)e; }
inline FRESULT f_sync(FIL* f) { return fflush(f->fp) ? FR_DISK_ERR : FR_OK; }
inline FRESULT f_unlink(const char* p) { return remove(p) ? FR_NO_FILE : FR_OK; }
inline FRESULT f_rename(const char* a, const char* b) { return rename(a, b) ? FR_DISK_ERR : FR_OK; }
