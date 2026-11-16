// Hardware-in-the-loop benchmark mailbox (CMake HIL_BENCH, default OFF).
//
// tools/hil/hil_bench.py drives a board over SWD (OpenOCD + debug probe): it
// writes a command into g_hil, the firmware runs it at the next frame boundary,
// and the per-frame accumulators below give the window's FPS / IDL exactly as the
// OSD stats line computes them (core0 frames over core0 time, idle = target -
// elapsed before the idle-window I/O). Nothing here talks to the UART: reading
// RAM over SWD does not perturb the frame the way a log line does.
//
// Layout is part of the protocol (the runner reads it by offset): bump
// HIL_VERSION on any change and keep tools/hil/hil_bench.py in step.
#pragma once
#include <stdint.h>

#ifndef HIL_BENCH
#define HIL_BENCH 0
#endif

#define HIL_MAGIC   0x4C494850u   // "PHIL"
#define HIL_VERSION 1u

enum HilCmd : uint32_t {
    HIL_CMD_NONE      = 0,
    HIL_CMD_RESET     = 1,   // zero the accumulators (start a measuring window)
    HIL_CMD_LOAD_SLOT = 2,   // arg = persist slot: persistLoad(arg), then RESET
    HIL_CMD_LOAD_FILE = 3,   // path[] = snapshot (.pss/.z80/.sna/...): LoadSnapshot, then RESET
};

enum HilStatus : uint32_t {
    HIL_ST_IDLE = 0,
    HIL_ST_BUSY = 1,
    HIL_ST_OK   = 2,
    HIL_ST_FAIL = 3,
};

struct HilBench {
    // +0  identity
    uint32_t magic;          // HIL_MAGIC once the main loop has run
    uint32_t version;        // HIL_VERSION
    // +8  mailbox: the runner writes arg/path first, then cmd; the firmware
    //     sets status, clears cmd and bumps seq when the command is done.
    volatile uint32_t cmd;
    volatile uint32_t arg;
    volatile uint32_t status;
    volatile uint32_t seq;
    // +24 window accumulators (since the last RESET)
    volatile uint32_t frames;
    volatile int32_t  idle_min;
    volatile int32_t  idle_max;
    volatile uint32_t neg_frames;     // frames with idle < 0
    volatile int64_t  idle_sum;       // us
    volatile uint64_t elapsed_sum;    // us, core0 frame time
    volatile uint32_t elapsed_max;
    volatile uint32_t target_us;      // the frame target last seen
    volatile uint64_t t_first;        // time_us_64 at the first frame end of the window
    volatile uint64_t t_last;         // ... at the latest one
    volatile uint64_t cpu_sum;        // us, CPU::loop (cpu_frame_us deltas)
    volatile uint64_t mix_sum;        // us, audio mix (g_mix_us)
    volatile uint64_t kbd_sum;        // us, processKeyboard (g_kbd_us)
    volatile uint32_t frames_total;   // since boot, never reset
    volatile uint32_t pad0;
    // +112
    char path[128];                   // HIL_CMD_LOAD_FILE argument, NUL-terminated
};

#if HIL_BENCH
extern HilBench g_hil;

namespace HilBenchSvc {
// Cold path: runs a pending command. Called only when g_hil.cmd != 0.
void service();
// Once per frame, right after idle = target - elapsed (RAM hot loop: inline adds only).
static inline void frame(int64_t idle, uint64_t elapsed, int64_t target, uint64_t now,
                         uint32_t cpu_us_total, uint32_t mix_us, uint32_t kbd_us) {
    static uint32_t p_cpu = 0;
    const uint32_t d_cpu = cpu_us_total >= p_cpu ? cpu_us_total - p_cpu : cpu_us_total;
    p_cpu = cpu_us_total;
    const int32_t i = (int32_t)idle;
    const uint32_t e = (uint32_t)elapsed;
    if (g_hil.frames == 0) {
        g_hil.t_first = now;
        g_hil.idle_min = i; g_hil.idle_max = i;
    } else {
        if (i < g_hil.idle_min) g_hil.idle_min = i;
        if (i > g_hil.idle_max) g_hil.idle_max = i;
    }
    g_hil.frames = g_hil.frames + 1;
    g_hil.frames_total = g_hil.frames_total + 1;
    if (i < 0) g_hil.neg_frames = g_hil.neg_frames + 1;
    g_hil.idle_sum = g_hil.idle_sum + i;
    g_hil.elapsed_sum = g_hil.elapsed_sum + e;
    if (e > g_hil.elapsed_max) g_hil.elapsed_max = e;
    g_hil.target_us = (uint32_t)target;
    g_hil.t_last = now;
    g_hil.cpu_sum = g_hil.cpu_sum + d_cpu;
    g_hil.mix_sum = g_hil.mix_sum + mix_us;
    g_hil.kbd_sum = g_hil.kbd_sum + kbd_us;
}
}
#endif
