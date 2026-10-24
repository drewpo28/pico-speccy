#pragma once
#include <stdint.h>
extern uint32_t g_ms;
struct absolute_time_t_ { uint64_t v; };
typedef absolute_time_t_ absolute_time_t;
static inline absolute_time_t get_absolute_time() { return {g_ms}; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)t.v; }
