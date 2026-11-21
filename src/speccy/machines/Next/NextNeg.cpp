// [NXNEG]: where a Next frame's time went when it ran over the target (negative
// IDL) — the worst frame of every 50, logged once a second only when at least one
// frame overran. Plain builds (no NEXT_TRACE: its own 1 Hz dump stalls core0 and
// hides short dips). Fluid's level change still went negative after the card-busy
// fix (hw 2026-11-21), with an audible glitch of about a second: this line says
// which term it is before a second fix.
//   el    the frame (ESPectrum::loop, us)      cpu  CPU::loop (cpu_frame_us)
//   qw    core0 waiting for core1's row ring   sd   DivMMC card stall us / ops / writes
//   c0w   core0 waiting for core1's SD read    aud / mix / kbd  the audio DMA wait, synth, keyboard
//   rest  el - cpu - aud - mix - kbd            skip frames the renderer dropped (autoskip)
//   pc / iff / halt  the guest at the end of the worst frame
// Flash, one call per frame on the Next (ESPectrum::loop).
#include "app/Debug.h"
#include "speccy/z80/z80.h"
#include <cstdint>
#include <cstdio>
#include "app/NxFrameProf.h"
#include "speccy/core/MemESP.h"
extern "C" size_t getLargestAllocatable(void);
size_t getFreeHeap(void);

extern volatile uint32_t cpu_frame_us;
extern volatile uint32_t g_nx_qwait_us, g_nx_autoskip;
extern volatile uint32_t g_sd_stall_us, g_sd_ops, g_sd_wr_ops;
extern volatile uint32_t g_sd_as_c0wait_us;
#if Z80_HWBENCH
extern volatile uint32_t g_hb_exec, g_hb_halt, g_hb_int, g_hb_dma, g_hb_tick, g_hb_slow;   // CPU.cpp
extern volatile uint32_t endframe_us;
extern volatile uint32_t g_hb_sw_n, g_hb_ph_n, g_hb_cop, g_hb_cap, g_hb_post, g_hb_map;   // NextVideo.cpp
#endif   // sdcard.c (a global: same name in C and C++)

namespace NextNeg {

static inline uint32_t delta(uint32_t c, uint32_t& p) { const uint32_t d = c >= p ? c - p : c; p = c; return d; }

#if Z80_HWBENCH
}
namespace NextHwBench { void tick(); }
namespace NextNeg {
#endif
void frame(int32_t idle, uint32_t el, uint32_t aud, uint32_t mix, uint32_t kbd) {
#if Z80_HWBENCH
    NextHwBench::tick();   // [Z80HW]: once, ~12 s in (NextHwBench.cpp)
#endif
    static uint32_t p_cpu, p_qw, p_sd, p_ops, p_wr, p_c0w, p_skip;
    const uint32_t cpu = delta(cpu_frame_us, p_cpu), qw = delta(g_nx_qwait_us, p_qw);
    const uint32_t sd = delta(g_sd_stall_us, p_sd), ops = delta(g_sd_ops, p_ops), wr = delta(g_sd_wr_ops, p_wr);
    const uint32_t c0w = delta(g_sd_as_c0wait_us, p_c0w), skip = delta(g_nx_autoskip, p_skip);
    static uint32_t p_m1; const uint32_t m1 = delta(Z80::getFetchCounter(), p_m1);   // M1s of the frame (prefixes count)
#if Z80_HWBENCH
    static uint32_t p_hb[6];
    const uint32_t hb[7] = { delta(g_hb_exec, p_hb[0]), delta(g_hb_tick, p_hb[1]), delta(g_hb_slow, p_hb[2]),
                             delta(g_hb_int, p_hb[3]), delta(g_hb_halt, p_hb[4]), delta(g_hb_dma, p_hb[5]), endframe_us };
    static uint32_t w_hb[7];
    static uint32_t p_hb2[6], w_hb2[6];
    const uint32_t hb2[6] = { delta(g_hb_sw_n, p_hb2[0]), delta(g_hb_ph_n, p_hb2[1]), delta(g_hb_cop, p_hb2[2]),
                              delta(g_hb_cap, p_hb2[3]), delta(g_hb_post, p_hb2[4]), delta(g_hb_map, p_hb2[5]) };
#endif
    static uint32_t n = 0, neg = 0, wskip = 0, wops = 0;
    static int32_t w_idle = INT32_MAX;
    static uint32_t w[11];
    static uint16_t w_pc;
    static uint8_t w_flags;
    wskip += skip; wops += ops;
    if (idle < w_idle) {
        w_idle = idle;
        w[0] = el; w[1] = cpu; w[2] = qw; w[3] = sd; w[4] = ops; w[5] = wr; w[6] = c0w;
        w[7] = aud; w[8] = mix; w[9] = kbd; w[10] = m1;
#if Z80_HWBENCH
        for (int i = 0; i < 7; i++) w_hb[i] = hb[i];
        for (int i = 0; i < 6; i++) w_hb2[i] = hb2[i];
#endif
        w_pc = Z80::getRegPC();
        w_flags = (uint8_t)((Z80::isIFF1() ? 1 : 0) | (Z80::isHalted() ? 2 : 0));
#if NXNEG_PROF && !NEXT_TRACE
        NxFrameProf::keep();   // its CPU::loop samples (100 us): the [NXNEG] prof line
#endif
    }
    if (idle < 0) neg++;
    if (++n < 50) return;
    if (neg) {
        // the hot-page pool: pages held, free heap / largest block (could it take another frame?)
        uint8_t hp[MemESP::NEXT_HOT_MAX]; const int hn = MemESP::nextHotPages(hp);
        char hb[40]; int ho = 0; hb[0] = 0;
        for (int i = 0; i < hn && ho < (int)sizeof hb - 4; i++) ho += snprintf(hb + ho, sizeof hb - ho, "%s%02X", i ? "," : "", hp[i]);
        Debug::log("[NXNEG] hot=%s heap=%u/%u", hn ? hb : "-", (unsigned)getFreeHeap(), (unsigned)getLargestAllocatable());
        Debug::log("[NXNEG] 50f: neg=%u worst: idle=%d el=%u cpu=%u qw=%u sd=%u/%u/%uw c0w=%u aud=%u mix=%u kbd=%u rest=%d"
                   " | pc=%04X iff=%u halt=%u m1=%u (%u ns/M1 of cpu) | 50f: skip=%u sdops=%u",
                   (unsigned)neg, (int)w_idle, (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
                   (unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7], (unsigned)w[8], (unsigned)w[9],
                   (int)(w[0] - w[1] - w[7] - w[8] - w[9]),
                   (unsigned)w_pc, (unsigned)(w_flags & 1), (unsigned)(w_flags >> 1), (unsigned)w[10],
                   w[10] ? (unsigned)((uint64_t)w[1] * 1000 / w[10]) : 0u, (unsigned)wskip, (unsigned)wops);
    }
#if Z80_HWBENCH
    // the worst frame's CPU::loop split (CPU.cpp HB_*): exec = the unchecked slices (tick and
    // slow inside it), int = interrupt entries (checkINT + execute), halt = haltAdvanceTo,
    // dma = paced zxnDMA, ef = VIDEO::EndFrame; interp = exec - tick - slow
    if (neg)
        Debug::log("[NXNEG] hb: exec=%u (tick=%u slow=%u interp=%d) int=%u halt=%u dma=%u ef=%u",
                   (unsigned)w_hb[0], (unsigned)w_hb[1], (unsigned)w_hb[2], (int)(w_hb[0] - w_hb[1] - w_hb[2]),
                   (unsigned)w_hb[3], (unsigned)w_hb[4], (unsigned)w_hb[5], (unsigned)w_hb[6]);
    if (neg)
        Debug::log("[NXNEG] hb2: tick calls=%u (paper phases %u) cop=%u cap=%u post=%u map=%u us",
                   (unsigned)w_hb2[0], (unsigned)w_hb2[1], (unsigned)w_hb2[2], (unsigned)w_hb2[3], (unsigned)w_hb2[4], (unsigned)w_hb2[5]);
#endif
#if NXNEG_PROF && !NEXT_TRACE
    // The worst frame's core0 PCs in 64-byte buckets, the 16 biggest: resolve against
    // the ELF's symbols (0x2006xxxx-0x2007xxxx: .nxovl's own symbols only, the windows
    // share VMAs). Taken only inside CPU::loop.
    if (neg && g_nxprof_worst) {
        const uint32_t m = g_nxprof_worst_n;
        uint32_t pk[40]; uint16_t pc[40]; int np = 0;
        for (uint32_t i = 0; i < m; i++) {
            const uint32_t k = g_nxprof_worst[i] & ~63u;
            int j = 0; while (j < np && pk[j] != k) j++;
            if (j == np) { if (np == 40) continue; pk[np] = k; pc[np] = 0; np++; }
            pc[j]++;
        }
        for (int part = 0; part < 2; part++) {
            char pb[200]; int po = snprintf(pb, sizeof pb, part ? "[NXNEG] prof+" : "[NXNEG] prof %u:", (unsigned)m);
            for (int t = 0; t < 8; t++) {
                int b = -1; for (int j = 0; j < np; j++) if (pc[j] && (b < 0 || pc[j] > pc[b])) b = j;
                if (b < 0) break;
                po += snprintf(pb + po, sizeof pb - po, " %08X:%u", (unsigned)pk[b], (unsigned)pc[b]);
                pc[b] = 0;
            }
            Debug::log("%s", pb);
        }
    }
#endif
    n = neg = wskip = wops = 0;
    w_idle = INT32_MAX;
}

} // namespace NextNeg
