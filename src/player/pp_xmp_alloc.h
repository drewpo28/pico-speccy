/* pico-speccy — forced into every libxmp-lite translation unit (CMakeLists,
 * -include). libxmp keeps the whole module — samples included — in heap blocks;
 * on this machine those belong in butter PSRAM, not in the SRAM heap (an IT
 * module is easily megabytes). The allocator lives in PlayerXmp.cpp. */
#pragma once
#include <stddef.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
void* pp_xmp_malloc(size_t n);
void* pp_xmp_calloc(size_t n, size_t m);
void* pp_xmp_realloc(void* p, size_t n);
void  pp_xmp_free(void* p);
#ifdef __cplusplus
}
#endif
#define malloc  pp_xmp_malloc
#define calloc  pp_xmp_calloc
#define realloc pp_xmp_realloc
#define free    pp_xmp_free
