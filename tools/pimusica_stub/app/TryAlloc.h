#pragma once
#include <stdlib.h>
static inline void* tryMalloc(size_t n) { return malloc(n); }
static inline void* tryCalloc(size_t n) { return calloc(n, 1); }
