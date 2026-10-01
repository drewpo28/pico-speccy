// Non-panicking heap allocation.
//
// pico_malloc's malloc/calloc PANIC on NULL (PICO_MALLOC_PANIC), so every
// `if (!p)` fallback written against them is dead code — including every
// `new (std::nothrow)` in the firmware, since libstdc++'s nothrow new is just
// malloc underneath. A subsystem that comes up on a thin heap (576p + TS-Conf +
// NeoGS + the UART console: hw 2026-09-14, "*** PANIC *** Out of memory" out of
// Subsystems::applyPending) must degrade — feature off, a log line — not take
// the whole firmware down at boot.
//
// tryMalloc gates the real allocator with getLargestAllocatable() (a binary
// search over the non-panicking __real_malloc), then allocates through the
// wrapped, mutex-taking malloc. That can only panic if another core allocates
// the probed block away in between — no core1 path in this firmware mallocs.
// cxx_shims.cpp routes operator new(std::nothrow) here, so the existing
// nothrow sites become real without touching them.
#ifndef TRYALLOC_H
#define TRYALLOC_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* tryMalloc(size_t n);
void* tryCalloc(size_t n);          // zeroed, like calloc(n, 1)
#ifdef __cplusplus
}
#include <stdlib.h>
// A scratch block that lives for ONE call: taken from the heap on entry, freed on
// exit. The replacement for the `static FIL f; static char buf[512];` locals that
// were parked in .bss to stay off the core0 stack — they cost every session ~600 B
// each for a file that is open for milliseconds. Test it: a NULL means "no heap",
// and the caller fails the operation the way an f_open() failure already does.
struct ScopedHeap {
    void* p;
    explicit ScopedHeap(size_t n) : p(tryMalloc(n)) {}
    ~ScopedHeap() { free(p); }
    ScopedHeap(const ScopedHeap&) = delete;
    ScopedHeap& operator=(const ScopedHeap&) = delete;
    template <class T> T* as() const { return (T*)p; }
    explicit operator bool() const { return p != nullptr; }
};
#endif
#endif
