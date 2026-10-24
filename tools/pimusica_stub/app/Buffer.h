#pragma once
#include <stdlib.h>
#include <stdint.h>
namespace Buffer {
enum { NEED_POINTER = 1, PREFER_PSRAM = 2, HOT_SRAM = 32 };
void* palloc(size_t n, int flags);
void pfree(void* p);
}
