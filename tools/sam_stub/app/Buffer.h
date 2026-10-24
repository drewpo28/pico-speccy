// Host stub of app/Buffer.h for tools/sam_sim (SamFdc.cpp only needs palloc/pfree).
#pragma once
#include <stdlib.h>
#include <stdint.h>
namespace Buffer {
    enum { NEED_POINTER = 1, PREFER_PSRAM = 2, HOT_SRAM = 4 };
    inline void* palloc(uint32_t n, int) { return malloc(n); }
    inline void  pfree(void* p) { free(p); }
}
