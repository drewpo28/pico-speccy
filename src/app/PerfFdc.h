// PERF_TRACE: where a TR-DOS disk load spends its GUEST time.
//
// FPS/IDL normal and the clock the same, yet a load takes far longer — so the
// guest is waiting on something. Two instruments, both PERF_TRACE only:
//   [FDC] one line per WD1793 command (capped): guest T-states since the previous
//         command (3.5 MHz units), command, track/sector registers, drive, PC.
//   [PERF] fdc: per 60 frames — FDC port accesses by (PC, port, dir), top entries,
//         so a poll loop that spins shows up with its address.
#pragma once
#include <stdint.h>

#if PERF_TRACE
namespace PerfFdc {
    void cmd(uint8_t cmd, uint8_t trk, uint8_t sec, uint8_t drv);  // a command register write
    void port(uint8_t port, bool write);                            // any FDC-family port access
    void frame();                                                   // per-frame watch
    void dump();                                                    // the 60-frame line
    void reset();                                                   // machine reset: re-arm the budget
}
#define PERF_FDC_CMD(c, t, s, d) PerfFdc::cmd((c), (t), (s), (d))
#define PERF_FDC_PORT(p, w)      PerfFdc::port((p), (w))
#else
#define PERF_FDC_CMD(c, t, s, d) ((void)0)
#define PERF_FDC_PORT(p, w)      ((void)0)
#endif
