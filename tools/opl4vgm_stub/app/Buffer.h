#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
struct Buffer {
    enum : uint32_t { ALLOC_AUTO = 0, NEED_POINTER = 1, PREFER_PSRAM = 2 };
    enum Tier { TIER_NONE = 0, TIER_HEAP, TIER_BUTTER };
    struct PoolStat { size_t total; size_t used; size_t free; };
    static PoolStat poolStat(Tier) { return { 0, 0, 0 }; }
    static void* palloc(size_t n, uint32_t = 0) { return malloc(n); }
    static void  pfree(void* p) { free(p); }
};
