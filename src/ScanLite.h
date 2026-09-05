#pragma once
// Tiny sscanf() replacements.
//
// newlib's sscanf is ~49 KB of flash for this firmware: the float-capable
// __ssvfscanf_r drags in strtod + mprec, and its iswspace() pulls newlib's
// Unicode category tables (jp2uc.o + categories.o, 28 KB on their own). Every
// call site here parsed a handful of small integers, so these cover exactly
// those shapes. Keep sscanf/siscanf out of the image — the linker map's
// "Archive member included" section is where a reintroduction shows up
// (2026-09-06: the +3/+3e ROMs pushed the GMX-partitioned flash over its
// ceiling by 23 KB, and this is part of what bought the room back).
#include <stdlib.h>

// Parse `n` decimal integers separated by `sep` — the sscanf "%d<sep>%d…" shape.
// Blanks before each number are skipped, an optional sign is accepted, and text
// after the last field is ignored, as sscanf does. Returns the number of fields
// parsed, so `== n` is the same success test sscanf's return value gave.
static inline int scanInts(const char* s, char sep, int* out, int n) {
    if (!s) return 0;
    for (int i = 0; i < n; i++) {
        while (*s == ' ' || *s == '\t') s++;
        if (i) { if (*s != sep) return i; s++; while (*s == ' ' || *s == '\t') s++; }
        char* end;
        long v = strtol(s, &end, 10);
        if (end == s) return i;
        out[i] = (int)v;
        s = end;
    }
    return n;
}

// Same for unsigned fields ("%u<sep>%u…"). A leading '-' is rejected rather than
// wrapped — every caller feeds this user-typed geometry, where that is a typo.
static inline int scanUints(const char* s, char sep, unsigned* out, int n) {
    if (!s) return 0;
    for (int i = 0; i < n; i++) {
        while (*s == ' ' || *s == '\t') s++;
        if (i) { if (*s != sep) return i; s++; while (*s == ' ' || *s == '\t') s++; }
        if (*s == '-') return i;
        char* end;
        unsigned long v = strtoul(s, &end, 10);
        if (end == s) return i;
        out[i] = (unsigned)v;
        s = end;
    }
    return n;
}
