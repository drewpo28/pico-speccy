#pragma once
#include <stdio.h>
#include <stdint.h>
typedef unsigned UINT;
struct FIL { FILE* f; };
enum { FR_OK = 0, FA_READ = 1 };
inline int f_open(FIL* f, const char* p, int) { f->f = fopen(p, "rb"); return f->f ? FR_OK : 1; }
inline uint32_t f_size(FIL* f) { long c = ftell(f->f); fseek(f->f, 0, SEEK_END); long s = ftell(f->f); fseek(f->f, c, SEEK_SET); return (uint32_t)s; }
inline int f_read(FIL* f, void* b, UINT n, UINT* br) { *br = (UINT)fread(b, 1, n, f->f); return FR_OK; }
inline int f_close(FIL* f) { fclose(f->f); return FR_OK; }
