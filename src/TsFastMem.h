// TS-Conf fast guest-memory path (2026-09-06).
//
// While a TS-Conf whole-line video mode is live the per-access video hook is a
// pure T-state counter (VIDEO::TsDraw), every CPU bank is a plain POINTER page
// (TS-BIOS ROM included, via TsConf::romPtr), and there are no ROM overlays, no
// DivMMC window and no ProfROM tap. The generic Z80Ops accessors still walked
// all of those tests plus an indirect Draw call on EVERY byte — ~45 ARM
// instructions per guest read against ~12 here. g_ts_fastmem is recomputed by
// VIDEO::tsFastMemRecalc() at the per-frame Draw arming (EndFrame) and whenever
// a condition that must switch it off mid-frame changes (NeoGS ZX-DMA window,
// debugger memory breakpoints). Zero on every other machine, so the cost there
// is one byte load + branch per access.
#pragma once
#include <stdint.h>
#include "CPU.h"
#include "Video.h"

extern uint8_t g_ts_fastmem;

static inline void tsFastTick(uint32_t n) {
    CPU::tstates += n;
    if (__builtin_expect(CPU::tstates >= VIDEO::ts_line_t, 0)) VIDEO::tsDrawTick();
}
