// Host test of the eLeMeNt ZX LCU against the table in the Programmer's Reference.
// g++ -O2 -Wall -Wextra -Isrc -o /tmp/elm_lcu tools/elm_lcu_test.cpp && /tmp/elm_lcu
#include <stdio.h>
#include "speccy/machines/Element/ElmLcu.h"
using namespace ElmLcu;
static int fails, checks;
static void eq(const char* what, int64_t got, int64_t want) {
    checks++;
    if (got != want) { fails++; printf("FAIL %s: got %lld want %lld\n", what, (long long)got, (long long)want); }
}
// Byte writes in the order a Z80 makes them.
static void w32(uint8_t* r, int o, int32_t v) {
    for (int i = 0; i < 4; i++) write(r, (uint8_t)(o + i), (uint8_t)((uint32_t)v >> (8 * i)));
}
static void w64(uint8_t* r, int o, int64_t v) {
    for (int i = 0; i < 8; i++) write(r, (uint8_t)(o + i), (uint8_t)((uint64_t)v >> (8 * i)));
}
int main() {
    uint8_t r[SIZE];
    struct { int32_t a, b, c, d; int64_t m; int32_t q, rem; } t[] = {
        { 13, 11, 0, 1, 143, 143, 0 },
        { 3, 5, 7, 4, 22, 5, 2 },
        { 7, -3, 0, 2, -21, -10, -1 },
        { 1, 0, -5, 2, -5, -2, -1 },
        { 13, 11, 0, 0, 143, INT32_MAX, INT32_MAX },
        { 0x40000000, 0x40000000, 0, 1, 0x1000000000000000ll, INT32_MAX, INT32_MAX },
    };
    for (auto& e : t) {
        reset(r);
        w32(r, A, e.a); w32(r, B, e.b); w32(r, C, e.c); w32(r, D, e.d);
        eq("M", get64(r, M), e.m); eq("Q", get32(r, Q), e.q); eq("R", get32(r, R), e.rem);
    }
    reset(r);
    eq("reset D", get32(r, D), 1); eq("reset CPL", get64(r, CPLZ), -1);
    w32(r, A, 3); w32(r, B, 5); w32(r, C, 7); w32(r, D, 4);
    static const uint8_t cpl[8] = { 0xFA, 0xFF, 0xFF, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF };
    eq("CPL Z", memcmp(r + CPLZ, cpl, 8), 0);
    eq("SUB", (int32_t)get32(r, SUB), -2);
    reset(r);
    w64(r, M, 1000); w32(r, D, 7);
    eq("direct Q", get32(r, Q), 142); eq("direct R", get32(r, R), 6);
    eq("direct keeps M", get64(r, M), 1000);
    write(r, Q, 0x55); eq("Q read only", get32(r, Q), 142);
    w64(r, M, (int64_t)INT32_MIN); w32(r, D, -1);
    eq("min/-1 saturates", get32(r, Q), INT32_MAX);
    write(r, CMD, 1); eq("CMD 1 ignored", get64(r, M), (int64_t)INT32_MIN);
    write(r, CMD, 0); eq("CMD 0 resets", get64(r, M), 0);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
