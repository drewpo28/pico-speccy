// PERF_HIST-only host-time attribution inside CPU::loop: where the core0 frame
// goes when the guest executes few instructions (TS-Conf TGV playback, 2026-09-30:
// 10.8k instructions a frame and 24 ms of core0 = 2.2 us per instruction).
// Scopes accumulate time_us_32() deltas; sub-microsecond calls truncate to 0 or 1
// at random phase, so a bucket of many calls is unbiased on average. Nested
// scopes overlap on purpose (a port write that starts a DMA counts in both).
#pragma once
#if PERF_TRACE && PERF_HIST
#include "hardware/timer.h"
extern uint32_t perf_port_us[512];    // [0..255] IN by low address byte, [256..511] OUT
extern uint32_t perf_port_n[512];
enum { PB_HALT, PB_DRAWTICK, PB_PALPOLL, PB_ENDFRAME, PB_N };
extern uint32_t perf_bucket_us[PB_N];
extern uint32_t perf_bucket_n[PB_N];
struct PerfPortScope {
    uint32_t t0; uint16_t i;
    explicit PerfPortScope(uint16_t idx) : t0(time_us_32()), i(idx) {}
    ~PerfPortScope() { perf_port_us[i] += time_us_32() - t0; perf_port_n[i]++; }
};
struct PerfBucketScope {
    uint32_t t0; uint8_t i;
    explicit PerfBucketScope(uint8_t idx) : t0(time_us_32()), i(idx) {}
    ~PerfBucketScope() { perf_bucket_us[i] += time_us_32() - t0; perf_bucket_n[i]++; }
};
#define PERF_PORT_SCOPE(idx)   PerfPortScope   _perf_port_scope(idx)
#define PERF_BUCKET_SCOPE(idx) PerfBucketScope _perf_bucket_scope(idx)
#else
#define PERF_PORT_SCOPE(idx)   do {} while (0)
#define PERF_BUCKET_SCOPE(idx) do {} while (0)
#endif
