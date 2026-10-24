#pragma once
#include <stdint.h>
typedef struct { int id; } spin_lock_t;
static spin_lock_t g_sl[32];
extern int g_lock_depth;
static inline void spin_lock_unsafe_blocking(spin_lock_t*) { g_lock_depth++; }
static inline void spin_unlock_unsafe(spin_lock_t*) { g_lock_depth--; }
static inline int spin_lock_claim_unused(bool) { return 5; }
static inline void spin_lock_unclaim(int) {}
static inline spin_lock_t* spin_lock_init(int id) { g_sl[id].id = id; return &g_sl[id]; }
#define __dmb() do{}while(0)
