// Per-frame host PC profiler for core0 (NEXT_TRACE builds) — see NxFrameProf.cpp.
#pragma once
#include <stdint.h>
#if NEXT_TRACE || NXNEG_PROF
namespace NxFrameProf {
    void init();                  // core0, once (heap 4 KB + one hardware alarm)
    void frameBegin();
    void frameEnd();              // stop sampling
    void keep();                  // this frame is the worst so far: keep its samples
    void initCore1();             // core1, once: a second alarm sampling core1 into a ring
}
extern uint32_t* g_nxprof1;            // [NXPROF1_N] core1 PCs, a ring (the last ~51 ms)
extern volatile uint32_t g_nxprof1_w;  // samples written so far (ring index = w % NXPROF1_N)
#define NXPROF1_N 512u
extern uint32_t* g_nxprof_worst;
extern volatile uint32_t g_nxprof_worst_n;
#endif
