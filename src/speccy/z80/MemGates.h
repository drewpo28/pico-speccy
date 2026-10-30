// The four gates of the fast guest-memory path, packed in ONE aligned word so the
// hot accessors (CPU.cpp Z80Ops::peek8/poke8/peek16/poke16) test the whole set
// with a single load. Every byte is still written and read under its old name
// (the macros below), so the owners and their semantics are untouched:
//   fastmem  (g_ts_fastmem) VIDEO::tsFastMemRecalc   TsFastMem.h (0 off, 1 Next >= 0x4000, 2 every address)
//   memcyc   (g_ts_memcyc)  TsConf::memcycRecalc     TsConf.h (DRAM model)
//   tsconf_wr(g_tsconf_wr)  TsConf write hooks       TsConf.h
//   atm_ro   (g_atm_ro)     Atm::remap / EvoBase     Atm.h
// The READ twin g_ts_fastrd (TsFastMem.h) stays a separate byte: on the Next it
// differs from fastmem below 0x4000.
// Defined in CPU.cpp. Byte order is little-endian: fastmem is bits 0..7.
// NB tools/memdump.gdb names g_memgates.b.atm_ro — a symbol that does not exist
// there HANGS Ctrl+Alt+D, so keep the two in step.
// Ported from the ccr-05d4d17f-wh3s7v branch (2026-10-10), adapted to the Next
// line's 8K slots and two-level fastmem.
#pragma once
#include <stdint.h>

union MemGates {
    struct { uint8_t fastmem, memcyc, tsconf_wr, atm_ro; } b;
    uint32_t w;
};
extern MemGates g_memgates;

#define g_ts_fastmem (g_memgates.b.fastmem)
#define g_ts_memcyc  (g_memgates.b.memcyc)
#define g_tsconf_wr  (g_memgates.b.tsconf_wr)
#define g_atm_ro     (g_memgates.b.atm_ro)

// No DRAM model armed (reads), and no write gate either (writes): the plain
// store/load. The address test (fastrd / fastmem against 0x4000) stays separate.
#define MEMGATES_READ_PLAIN(w)  (((w) & 0xFF00u) == 0u)
#define MEMGATES_WRITE_PLAIN(w) (((w) & 0xFFFFFF00u) == 0u)
// Enterprise (EpFast.h): bit 6 of the atm_ro byte — never a window bit (those are
// 0..3) nor the ZX-Evo font bit (7). The accessors' generic branch takes the EP
// path when it is set.
#define MEMGATES_EP 0x40000000u
#define ATM_RO_EP   0x40u
