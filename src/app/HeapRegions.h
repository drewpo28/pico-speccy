// HeapRegions — the arithmetic behind CodeOverlay's _sbrk: the heap as a LIST OF
// REGIONS with one upward-growing cursor that JUMPS over a resident code-overlay
// window into the released windows above it. Header-only and dependency-free so
// tools/heap_regions_test.c can drive newlib's own dlmalloc (_mallocr.c) through
// it on the host — the allocator the firmware links, not a model of it.
//
// Why it is safe with that allocator: dlmalloc 2.6.4's malloc_extend_top treats a
// MORECORE that returns an address other than the old top end as a "foreign
// sbrk" — it fences the old top with two in-use fenceposts, frees it into the
// bins and starts a new top at the returned address. It rounds every non-initial
// extension up to a 4 KB page, so a region can only be entered by a request whose
// rounded size fits, and a region's last <4 KB are never handed out (the same
// loss the single ceiling always had at its own top) — hr_usable() says how much
// a stranded region can really give.
#ifndef HEAP_REGIONS_H
#define HEAP_REGIONS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { char* lo; char* hi; char* hwm; } HrRegion;   // hwm = highest cursor while current
typedef struct { char* ws; char* we; int loaded; } HrWindow;   // a code window, loaded = resident

typedef struct {
    HrRegion reg[4];
    unsigned n;         // regions in address order
    unsigned cur;       // region the cursor is in
    char*    cursor;    // the sbrk cursor
    char*    ceiling;   // == reg[cur].hi
} HeapRegions;

// Region list from the heap base, the windows (address order, adjacent to each
// other, the last one ending at `top`) and the stack limit `top`. Keeps the
// high-water marks of the old regions (a claim needs them: see hr_window_touched).
static inline void hr_rebuild(HeapRegions* h, char* base, const HrWindow* w, unsigned nw, char* top) {
    HrRegion old[4]; unsigned nold = h->n;
    for (unsigned i = 0; i < nold; i++) old[i] = h->reg[i];
    if (h->cursor == 0) h->cursor = base;
    unsigned n = 0;
    char* lo = base;
    for (unsigned i = 0; i < nw; i++) {
        if (w[i].we == w[i].ws || !w[i].loaded) continue;   // compiled out or released: heap
        if (w[i].ws > lo) { h->reg[n].lo = lo; h->reg[n].hi = w[i].ws; h->reg[n].hwm = lo; n++; }
        lo = w[i].we;
    }
    if (top > lo) { h->reg[n].lo = lo; h->reg[n].hi = top; h->reg[n].hwm = lo; n++; }
    if (n == 0) { h->reg[0].lo = h->reg[0].hi = h->reg[0].hwm = base; n = 1; }
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j < nold; j++)
            if (old[j].lo < h->reg[i].hi && old[j].hwm > h->reg[i].lo && old[j].hwm > h->reg[i].hwm)
                h->reg[i].hwm = old[j].hwm < h->reg[i].hi ? old[j].hwm : h->reg[i].hi;
    h->n = n;
    h->cur = 0;
    for (unsigned i = 0; i < n; i++)
        if (h->cursor >= h->reg[i].lo && h->cursor <= h->reg[i].hi) { h->cur = i; break; }
    if (h->cursor > h->reg[h->cur].hwm) h->reg[h->cur].hwm = h->cursor;
    h->ceiling = h->reg[h->cur].hi;
}

// The sbrk itself. Grows inside the current region; when a positive request does
// not fit, jumps to the first higher region that can take it whole. (char*)-1 on
// failure, exactly like the SDK's _sbrk.
static inline void* hr_sbrk(HeapRegions* h, char* base, long incr) {
    if (h->cursor == 0) h->cursor = base;
    char* const prev = h->cursor;
    char* const next = h->cursor + incr;
    if (__builtin_expect(next <= h->ceiling && next >= h->reg[h->cur].lo, 1)) {
        h->cursor = next;
        if (next > h->reg[h->cur].hwm) h->reg[h->cur].hwm = next;
        return (void*)prev;
    }
    if (incr <= 0) return (void*)-1;
    for (unsigned r = h->cur + 1; r < h->n; r++) {
        if ((size_t)(h->reg[r].hi - h->reg[r].lo) < (size_t)incr) continue;
        h->cur = r;
        h->ceiling = h->reg[r].hi;
        h->cursor = h->reg[r].lo + incr;
        h->reg[r].hwm = h->cursor;
        return (void*)h->reg[r].lo;
    }
    return (void*)-1;
}

// What a region the cursor has not reached can really give malloc: the
// page-rounded part less the top chunk's minimal overhead.
static inline size_t hr_usable(const HrRegion* r) {
    size_t sz = (size_t)(r->hi - r->lo) & ~(size_t)4095u;
    return sz > 64 ? sz - 64 : 0;
}
static inline size_t hr_stranded_bytes(const HeapRegions* h) {
    size_t n = 0;
    for (unsigned r = h->cur + 1; r < h->n; r++) n += hr_usable(&h->reg[r]);
    return n;
}
static inline size_t hr_stranded_largest(const HeapRegions* h) {
    size_t n = 0;
    for (unsigned r = h->cur + 1; r < h->n; r++) { size_t u = hr_usable(&h->reg[r]); if (u > n) n = u; }
    return n;
}

// Was a (released) window's range ever handed to malloc? True iff the high-water
// mark of the region holding it went past its base. A window above the mark is
// pristine even when the cursor has since JUMPED to a higher region — dlmalloc's
// chunks only ever cover what sbrk actually returned.
static inline int hr_window_touched(const HeapRegions* h, char* ws) {
    for (unsigned i = 0; i < h->n; i++)
        if (ws >= h->reg[i].lo && ws < h->reg[i].hi) return h->reg[i].hwm > ws;
    return 1;    // inside a loaded window or outside the heap: not claimable as free
}

#ifdef __cplusplus
}
#endif
#endif // HEAP_REGIONS_H
