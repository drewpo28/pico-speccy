// Stub for tools/nextint_test.cpp: NextInt.cpp reads guest memory in its
// NEXT_LINE_DIAG probe and feeds the hot-page pool's execution counters in ack().
#pragma once
#include <stdint.h>
struct MemESP {
    static uint8_t dbgPeek(uint16_t) { return 0; }
    static constexpr int NEXT_PAGES = 224;
    static inline uint8_t mmu[8] = {};
    static inline uint16_t* nextHotHits = nullptr;
};
