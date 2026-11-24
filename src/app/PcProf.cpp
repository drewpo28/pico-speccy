// PC-sampling profiler for core0 (PERF_HIST builds only).
//
// SysTick fires PCPROF_HZ times a second on core0; the handler takes the
// interrupted PC out of the exception frame (word 6 in both the basic and the
// FP-extended frame) and counts it in a small open-addressing table keyed by
// 64-byte code bucket. pcprof_dump() logs the hottest buckets once a second;
// resolve them with addr2line against the same ELF. Nothing else in the
// firmware uses SysTick (the SDK's time base is the TIMER block).
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "hardware/structs/systick.h"
#include "hardware/exception.h"
#include "hardware/clocks.h"
#include "Debug.h"
#include "Buffer.h"

#if PERF_TRACE && PERF_HIST

#define PCPROF_HZ    8000u
#ifndef PCPROF_SLOTS
#define PCPROF_SLOTS 256u        // power of two (2 KB with the counts)
#endif
#ifndef PCPROF_SHIFT
#define PCPROF_SHIFT 6           // 64-byte buckets (CMake PCPROF_SHIFT; 8 = 256 B)
#endif
#ifndef PCPROF_LINES
#define PCPROF_LINES 4           // 14 buckets a line: 56 named, the rest is "unlisted"
#endif
#ifndef PCPROF_CORE
#define PCPROF_CORE 0            // 1: core1's SysTick (armed by pcprof_core1_poll on core1)
#endif
#ifndef PCPROF_GSGAP
#define PCPROF_GSGAP 0           // > 0: only samples taken while GS::pump has not run for this many us
#endif
#if PCPROF_GSGAP
extern "C" volatile uint32_t gs_pump_last_us;   // GS.cpp: the last pump that ran the card
#include "hardware/timer.h"
#endif

// The table lives in PSRAM (2 x 4 B a slot): 512 slots in SRAM were 4 KB of .bss on
// a machine with 3.3 KB of heap left, and the Next could not open the menu on a
// PERF_HIST build (hw 2026-11-16). A missed sample costs nothing; the PSRAM latency
// is a few hundred ns at 8 kHz.
static uint32_t* s_key;
static uint32_t* s_cnt;
static volatile uint32_t s_total, s_lost;
static uint32_t s_hz = 0;   // clk_sys the SysTick reload was derived from
static volatile bool s_c1Arm;    // PCPROF_CORE 1: core1 arms its SysTick at its next poll
static volatile bool s_pause;    // PCPROF_CORE 1: core0 reads the table (core1's tick cannot be masked from core0)

extern "C" void __not_in_flash_func(pcprof_sample)(const uint32_t* frame) {
    if (!s_key || s_pause) return;
#if PCPROF_GSGAP
    if (time_us_32() - gs_pump_last_us < (uint32_t)PCPROF_GSGAP) return;
#endif
    // PCPROF_LR_LO/HI (CMake, hex): a sample inside that code range is counted at its
    // RETURN address instead - who calls memcpy, not that memcpy runs (2026-11-24).
    uint32_t pc = frame[6];
#if defined(PCPROF_LR_LO) && defined(PCPROF_LR_HI)
    if (pc - (uint32_t)PCPROF_LR_LO < (uint32_t)(PCPROF_LR_HI - PCPROF_LR_LO)) pc = frame[5];
#endif
    const uint32_t k = (pc >> PCPROF_SHIFT) + 1u;   // + 1: 0 means empty
    uint32_t h = (k * 2654435761u) >> 24;                  // 8 bits
    for (uint32_t i = 0; i < 16; i++, h = (h + 1) & (PCPROF_SLOTS - 1)) {
        if (s_key[h] == k) { s_cnt[h]++; s_total++; return; }
        if (s_key[h] == 0) { s_key[h] = k; s_cnt[h] = 1; s_total++; return; }
    }
    s_lost++;
}

extern "C" void __attribute__((naked)) __not_in_flash_func(pcprof_isr)(void) {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b pcprof_sample \n");
}

// SysTick counts processor clocks, so the reload follows clk_sys: pcprof_start
// runs at the end of ESPectrum::setup(), BEFORE main() switches to Config::cpu_mhz
// (a 504 MHz board sampled at 16 kHz and the line said 252 — hw 2026-10-15), so
// every dump re-derives it when the clock has moved.
static void pcprof_reload() {
    s_hz = clock_get_hz(clk_sys);
    systick_hw->csr = 0;
    systick_hw->rvr = s_hz / PCPROF_HZ - 1;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x7;   // CLKSOURCE = processor, TICKINT, ENABLE
    Debug::log("PROF: SysTick PC sampling of core%u at %u Hz (clk_sys %u)", (unsigned)PCPROF_CORE, PCPROF_HZ, (unsigned)s_hz);
}

void nxprofAlloc();     // Z80_JLS.cpp: the Next prefix / PC histograms
void perfPortAlloc();   // CPU.cpp: [PERF] ports host time
void pcprof_start() {
    // The PERF_HIST tables go to PSRAM here, before the guest runs (each user tests
    // its pointer): ~16 KB of SRAM on every PERF build otherwise.
    nxprofAlloc();
    perfPortAlloc();
    uint32_t* t = (uint32_t*)Buffer::palloc(2u * PCPROF_SLOTS * sizeof(uint32_t), Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
    if (!t) { Debug::log("PROF: no memory for %u slots, sampler off", (unsigned)PCPROF_SLOTS); return; }
    memset(t, 0, 2u * PCPROF_SLOTS * sizeof(uint32_t));
    s_cnt = t + PCPROF_SLOTS;
    s_key = t;
    exception_set_exclusive_handler(SYSTICK_EXCEPTION, pcprof_isr);
    if (PCPROF_CORE == 0) pcprof_reload();
    else s_c1Arm = true;
}
// core1's render_core loop: arms core1's own SysTick (the registers are per core).
extern "C" void __not_in_flash_func(pcprof_core1_poll)() {
    if (__builtin_expect(s_c1Arm, 0)) { s_c1Arm = false; pcprof_reload(); }
}

void pcprof_dump() {
    // Read the table in place with the tick masked (no second copy: RAM is
    // scarce on the Next). Samples missed while it is masked are ~1 ms/s.
    // The 56 hottest buckets over PCPROF_LINES lines ("PROF", "PROF+", ...):
    // the handlers are spread thinly, 14 buckets covered only ~60% of the time.
    if (!s_key) return;
    if (PCPROF_CORE == 0) systick_hw->csr &= ~0x2u;
    else { s_pause = true; __asm volatile("dmb" ::: "memory"); }
    const uint32_t total = s_total, lost = s_lost;
    for (int part = 0; total && part < PCPROF_LINES; part++) {
        char line[240];
        int n = part ? snprintf(line, sizeof line, "PROF+") :
                       snprintf(line, sizeof line, "PROF %u smp:", (unsigned)total);
        for (int k = 0; k < 14 && n < (int)sizeof line - 20; k++) {
            uint32_t best = 0, bi = 0;
            for (uint32_t i = 0; i < PCPROF_SLOTS; i++) if (s_cnt[i] > best) { best = s_cnt[i]; bi = i; }
            if (!best) break;
            n += snprintf(line + n, sizeof line - n, " %08X:%u",
                          (unsigned)((s_key[bi] - 1u) << PCPROF_SHIFT),
                          (unsigned)(best * 1000u / total));   // per mille
            s_cnt[bi] = 0;
        }
        if (part == PCPROF_LINES - 1 && lost) snprintf(line + n, sizeof line - n, " lost=%u", (unsigned)lost);
        Debug::log("%s", line);
    }
    memset(s_key, 0, PCPROF_SLOTS * sizeof(uint32_t)); memset(s_cnt, 0, PCPROF_SLOTS * sizeof(uint32_t));
    s_total = s_lost = 0;
    if (PCPROF_CORE != 0) { s_pause = false; if (clock_get_hz(clk_sys) != s_hz) s_c1Arm = true; return; }
    if (clock_get_hz(clk_sys) != s_hz) pcprof_reload();
    else systick_hw->csr |= 0x2u;
}

#else
void pcprof_start() {}
void pcprof_dump() {}
extern "C" void pcprof_core1_poll() {}
#endif
