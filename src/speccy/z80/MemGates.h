// The four gates of the fast guest-memory path, packed in ONE aligned word so the
// hot accessors (CPU.cpp Z80Ops::peek8/poke8/peek16/poke16, Z80::exec_nocheck_ts)
// test them with a single load: `w == 1` is "fast path on, nothing armed" — a
// plain store/load with no DRAM model, no TS-Conf write gate, no ATM ROM window.
// Every byte is still written and read under its old name (the macros below), so
// the owners and their semantics are untouched:
//   fastmem  (g_ts_fastmem) VIDEO::tsFastMemRecalc   TsFastMem.h
//   memcyc   (g_ts_memcyc)  TsConf::memcycRecalc     TsConf.h (DRAM model)
//   tsconf_wr(g_tsconf_wr)  TsConf write hooks       TsConf.h
//   atm_ro   (g_atm_ro)     Atm::remap / EvoBase     Atm.h
// Defined in CPU.cpp. Byte order is little-endian: fastmem is bits 0..7.
// NB tools/memdump.gdb names g_memgates.b.atm_ro — a symbol that does not exist
// there HANGS Ctrl+Alt+D, so keep the two in step.
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

// Read side: fast path on and no DRAM model (the write gates do not matter).
#define MEMGATES_READ_FAST(w)  (((w) & 0xFFFFu) == 1u)
// Write side: fast path on and every other gate clear.
#define MEMGATES_WRITE_FAST(w) ((w) == 1u)
