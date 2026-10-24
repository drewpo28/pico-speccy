#pragma once
#include <stdlib.h>
inline void* tryMalloc(size_t n) { return malloc(n); }
inline void* tryCalloc(size_t a, size_t b) { return calloc(a, b); }
