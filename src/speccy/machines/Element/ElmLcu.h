// LCU, the LMN Coprocessor Unit of the eLeMeNt ZX: Z = (A * B + C) / D, signed, over
// a 64-byte little-endian register block. Firmware-free for tools/elm_lcu_test.cpp.
// Programmer's Reference, "LCU - LMN Coprocessor Unit". Results are stored into the
// block so that reads can be plain memory.
#pragma once
#include <stdint.h>
#include <string.h>

namespace ElmLcu {

enum { A = 0x00, B = 0x04, C = 0x08, D = 0x0C, M = 0x10, Q = 0x18, R = 0x1C, CMD = 0x20,
       CPLZ = 0x28, SUB = 0x30, SIZE = 0x40 };

static inline int32_t  get32(const uint8_t* r, int o) { uint32_t v; memcpy(&v, r + o, 4); return (int32_t)v; }
static inline void     put32(uint8_t* r, int o, uint32_t v) { memcpy(r + o, &v, 4); }
static inline int64_t  get64(const uint8_t* r, int o) { uint64_t v; memcpy(&v, r + o, 8); return (int64_t)v; }
static inline void     put64(uint8_t* r, int o, uint64_t v) { memcpy(r + o, &v, 8); }

static inline void divide(uint8_t* r) {
    const int64_t m = get64(r, M);
    const int64_t d = get32(r, D);
    int32_t q, rem;
    if (d == 0) {
        // Documented for M >= 0; the negative case mirrors the overflow rule.
        q = rem = m >= 0 ? INT32_MAX : INT32_MIN;
    } else {
        const int64_t qq = (m == INT64_MIN && d == -1) ? INT64_MAX : m / d;
        if (qq > INT32_MAX)      q = rem = INT32_MAX;
        else if (qq < INT32_MIN) q = rem = INT32_MIN;
        else { q = (int32_t)qq; rem = (int32_t)(m - qq * d); }
    }
    put32(r, Q, (uint32_t)q);
    put32(r, R, (uint32_t)rem);
    put32(r, CPLZ, ~(uint32_t)q);
    put32(r, CPLZ + 4, ~(uint32_t)rem);
}

static inline void recalc(uint8_t* r, bool fromMembers) {
    if (fromMembers) {
        const int64_t m = (int64_t)((uint64_t)(int64_t)get32(r, A) * (uint64_t)(int64_t)get32(r, B)
                                    + (uint64_t)(int64_t)get32(r, C));
        put64(r, M, (uint64_t)m);
    }
    put32(r, SUB, (uint32_t)get32(r, A) - (uint32_t)get32(r, B));
    divide(r);
}

static inline void reset(uint8_t* r) {
    memset(r, 0, SIZE);
    r[D] = 1;
    recalc(r, true);
}

// A guest write: every write to the block starts the calculation.
static inline void write(uint8_t* r, uint8_t off, uint8_t v) {
    if (off >= SIZE) return;
    if (off < Q) {                       // members and M
        r[off] = v;
        recalc(r, off < D);              // A, B, C rebuild M; D and M divide only
    } else if (off == CMD && v == 0) {
        reset(r);
    }
}

}
