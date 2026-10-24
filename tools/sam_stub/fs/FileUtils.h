// Host stub of fs/FileUtils.h for tools/sam_sim: fopen2/fclose2 only.
#pragma once
#include "fatfs/ff.h"
inline FIL* fopen2(const char* path, int mode) {
    FILE* f = fopen(path, (mode & FA_WRITE) ? "r+b" : "rb");
    if (!f) return nullptr;
    FIL* r = new FIL; r->f = f;
    fseek(f, 0, SEEK_END); r->size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
    return r;
}
inline void fclose2(FIL* f) { if (f) { fclose(f->f); delete f; } }
