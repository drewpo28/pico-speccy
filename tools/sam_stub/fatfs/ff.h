// Host stub of FatFs for tools/sam_sim: FIL over stdio.
#pragma once
#include <stdio.h>
#include <stdint.h>
typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
enum FRESULT { FR_OK = 0, FR_DISK_ERR = 1 };
enum { FA_READ = 1, FA_WRITE = 2 };
struct FIL { FILE* f; uint32_t size; };
inline FRESULT f_lseek(FIL* f, uint32_t o) { return fseek(f->f, (long)o, SEEK_SET) ? FR_DISK_ERR : FR_OK; }
inline FRESULT f_read(FIL* f, void* b, UINT n, UINT* br) { *br = (UINT)fread(b, 1, n, f->f); return FR_OK; }
inline FRESULT f_write(FIL* f, const void* b, UINT n, UINT* bw) { *bw = (UINT)fwrite(b, 1, n, f->f); return FR_OK; }
inline FRESULT f_sync(FIL* f) { fflush(f->f); return FR_OK; }
inline uint32_t f_size(FIL* f) { return f->size; }
