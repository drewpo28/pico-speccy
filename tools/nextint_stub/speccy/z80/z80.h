#pragma once
// Host stub for tools/nextint_test.cpp
struct Z80 {
    enum IntMode { IM0, IM1, IM2 };
    static IntMode im;
    static IntMode getIM() { return im; }
    static inline bool iff1 = true;            // NextInt::reschedule (EI / RETI)
    static bool isIFF1() { return iff1; }
    static uint16_t getRegPC() { return 0; }
};
