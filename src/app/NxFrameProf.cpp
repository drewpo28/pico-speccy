// Host PC profiler for core0 (NEXT_TRACE builds): a hardware-alarm interrupt every
// NXPROF_US samples the interrupted PC of the emulator itself, per emulated frame;
// the samples of the worst frame of the second are kept for the NEXT line, which
// prints them as addresses to resolve against the ELF (arm-none-eabi-nm / addr2line).
// Found by the XIP counter being shared by both cores: it could not say what core0
// was doing in TX-1696's 25-35 ms frames (2026-10-09).
#include "NxFrameProf.h"
#if NEXT_TRACE || NXNEG_PROF
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/structs/timer.h"
#include "TryAlloc.h"
#include <cstring>

#define NXPROF_US  100u
#define NXPROF_N   512u

static int        s_alarm = -1;
static uint32_t   s_target = 0;
static uint32_t*  s_cur = nullptr;    // [NXPROF_N] this frame
uint32_t*         g_nxprof_worst = nullptr;   // [NXPROF_N] worst frame
volatile uint32_t g_nxprof_worst_n = 0;
static volatile uint32_t s_n = 0;
static volatile bool s_on = false;

extern "C" void __not_in_flash_func(nxprof_isr_c)(uint32_t* frame) {
    const uint32_t bit = 1u << s_alarm;
    timer_hw->intr = bit;
    s_target += NXPROF_US;
    if ((int32_t)(s_target - timer_hw->timerawl) < 10) s_target = timer_hw->timerawl + NXPROF_US;
    timer_hw->alarm[s_alarm] = s_target;
    if (s_on && s_n < NXPROF_N) { s_cur[s_n] = frame[6]; s_n = s_n + 1; }
}

extern "C" __attribute__((naked)) void __not_in_flash_func(nxprof_isr)(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b nxprof_isr_c\n");
}

void NxFrameProf::init() {
    if (s_alarm >= 0 || s_cur) return;
    uint32_t* b = (uint32_t*)tryCalloc(2 * NXPROF_N * 4);
    if (!b) return;
    s_cur = b; g_nxprof_worst = b + NXPROF_N;
    s_alarm = hardware_alarm_claim_unused(false);
    if (s_alarm < 0) return;
    const uint irq = hardware_alarm_get_irq_num(s_alarm);
    irq_set_exclusive_handler(irq, nxprof_isr);
    irq_set_priority(irq, 0xC0);          // lowest: never delays video / audio
    irq_set_enabled(irq, true);           // on core0 (the caller)
    const uint32_t bit = 1u << s_alarm;
    timer_hw->intr = bit;
    hw_set_bits(&timer_hw->inte, bit);
    s_target = timer_hw->timerawl + NXPROF_US;
    timer_hw->alarm[s_alarm] = s_target;
}

// core1: the same 100 us alarm, its own (an alarm IRQ is enabled in the NVIC of
// the core that calls irq_set_enabled), samples written into a ring the NEXT
// line reads as it is: the renderer has no frame boundary of its own. Lowest
// priority, so a sample never lands inside the HDMI line ISR — that share is the
// `isr %` the render line already prints.
uint32_t* g_nxprof1 = nullptr;
volatile uint32_t g_nxprof1_w = 0;
static int      s_alarm1 = -1;
static uint32_t s_target1 = 0;

extern "C" void __not_in_flash_func(nxprof1_isr_c)(uint32_t* frame) {
    const uint32_t bit = 1u << s_alarm1;
    timer_hw->intr = bit;
    s_target1 += NXPROF_US;
    if ((int32_t)(s_target1 - timer_hw->timerawl) < 10) s_target1 = timer_hw->timerawl + NXPROF_US;
    timer_hw->alarm[s_alarm1] = s_target1;
    const uint32_t w = g_nxprof1_w;
    g_nxprof1[w % NXPROF1_N] = frame[6];
    g_nxprof1_w = w + 1;
}

extern "C" __attribute__((naked)) void __not_in_flash_func(nxprof1_isr)(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b nxprof1_isr_c\n");
}

void NxFrameProf::initCore1() {
    if (s_alarm1 >= 0) return;
    if (!g_nxprof1) {
        g_nxprof1 = (uint32_t*)tryCalloc(NXPROF1_N * 4);
        if (!g_nxprof1) { s_alarm1 = -2; return; }
    }
    s_alarm1 = hardware_alarm_claim_unused(false);
    if (s_alarm1 < 0) return;
    const uint irq = hardware_alarm_get_irq_num(s_alarm1);
    irq_set_exclusive_handler(irq, nxprof1_isr);
    irq_set_priority(irq, 0xC0);
    irq_set_enabled(irq, true);           // on core1 (the caller)
    const uint32_t bit = 1u << s_alarm1;
    timer_hw->intr = bit;
    hw_set_bits(&timer_hw->inte, bit);
    s_target1 = timer_hw->timerawl + NXPROF_US;
    timer_hw->alarm[s_alarm1] = s_target1;
}

void NxFrameProf::frameBegin() { if (s_cur) { s_n = 0; s_on = true; } }

void NxFrameProf::frameEnd() { s_on = false; }

void NxFrameProf::keep() {
    if (!s_cur) return;
    const uint32_t n = s_n;
    memcpy(g_nxprof_worst, s_cur, n * 4);
    g_nxprof_worst_n = n;
}
#endif
