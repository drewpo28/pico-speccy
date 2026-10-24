#pragma once
#include <stdint.h>
extern uint32_t g_now_us;
static inline uint32_t time_us_32() { return g_now_us; }
