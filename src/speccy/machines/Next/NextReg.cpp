/*

pico-speccy: ZX Spectrum Next (TBBlue) NextReg register file

*/

#include "NextReg.h"
#include "speccy/devices/sound/AySound.h"
#include "speccy/z80/MemGates.h"
#include "speccy/machines/Next/NextIdle.h"
#include "app/NxFrameProf.h"
#include "speccy/core/MemESP.h"
#include "speccy/core/Ports.h"
#include "speccy/z80/CPU.h"
#include "app/ESPectrum.h"
#include "speccy/video/Video.h"
#include "app/Debug.h"
#include "app/CodeOverlay.h"   // NX_OVL_CODE
#include "app/Config.h"
#include "hardware/regs/xip.h"
#include "hardware/regs/addressmap.h"
#include "NextVideo.h"
#include "NextRtc.h"
#include "NextInt.h"
#include "NextMf.h"
#include "NextBoard.h"
#include "NextPorts.h"
#include "speccy/devices/Z80DMA.h"
#include "app/Debug.h"
#include "speccy/z80/z80.h"
#include "speccy/devices/storage/DivMMC.h"
#include <cstdio>
#include <cstring>
extern "C" volatile uint32_t g_sd_as_c1_us, g_sd_as_c1_max, g_sd_as_c0wait_us, g_sd_as_c0wait_max;

static void applyTimexMode(uint8_t v);   // port FF / NR 0x69 screen-mode bits (below)
static bool s_hiresSeen = false;         // a Timex hi-res mode was set somewhere in this frame

extern "C" const unsigned char gb_rom_tbblue_loader[];  // src/speccy/roms/next/

#pragma GCC optimize("O3")

uint8_t NextReg::reg[256];
uint8_t NextReg::selected = 0;

uint8_t NextReg::port7FFD = 0;
uint8_t NextReg::portDFFD = 0;
uint8_t NextReg::port1FFD = 0;
uint8_t NextReg::port123B = 0;
uint8_t NextReg::l2Offset = 0;
uint8_t NextReg::s_ioTrapCause = 0;
uint16_t NextReg::extKeys = 0;
bool    NextReg::l2MapHigh = false;

#if NEXT_TRACE
// NEXT io: line — Next port I/O per port (16-bit address; a 0x?xFD-style port
// that decodes on the low byte still shows its high byte, which is the point:
// NR access is 243B/253B whatever the code loads in B).
struct NxIoStat { uint16_t port; uint8_t out, _p; uint32_t n, us; };
static NxIoStat s_io[32];
static uint32_t s_io_n[2], s_io_us[2];
void __not_in_flash_func(nxIoNote)(uint16_t port, uint32_t us, bool out) {
    // the 0x?x3B / 0x?xFD families by full address; the rest by low byte (OTIR
    // to the DMA port #0B walks B through 256 addresses and filled the table)
    if ((port & 0xFF) != 0x3B && (port & 0xFF) != 0xFD) port &= 0xFF;
    s_io_n[out] = s_io_n[out] + 1; s_io_us[out] = s_io_us[out] + us;
    for (int i = 0; i < 32; i++) {
        NxIoStat& e = s_io[i];
        if (e.n && (e.port != port || e.out != out)) continue;
        e.port = port; e.out = out; e.n++; e.us += us; return;
    }
}
#endif
uint8_t  NextReg::contendBanks = 0;
uint32_t NextReg::portEn = ~0u;   // all decoded; NextReg::reset / CPU::reset own it

uint8_t  NextReg::resetPending = 0;
uint8_t  NextReg::portFF = 0;
bool     NextReg::portFFIntDisable = false;

// NR 0x03 sub-fields (zxnext.vhd:1099-1103 defaults; write rules 5121-5151,
// read mux 5894 — semantics pinned via zx_go WireMachineType). The flat
// reg[0x03] byte is not enough: writes update the fields independently and
// reads RECONSTRUCT the byte — NextZXOS's post-boot `NEXTREG 0x03,0xB0`
// (bits 2:0 = 000) must leave machine_type intact.
static uint8_t s_machineType   = 0x03;
static uint8_t s_machineTiming = 0x03;
static uint8_t s_userDTLock    = 0;
// Read-back (zxnext.vhd 5894): nr_palette_sub_idx & timing & user_dt_lock &
// machine_type — bit 7 is the 9-bit palette write's "second byte pending"
// (NR 0x44), not the config-mode bit of the last write (which is what we
// showed until 2026-10-15: SprBig4b's `[B3:..]` against the board's `[13:..]`).
static uint8_t reg03Compose() {
    return (uint8_t)(((NEXTVID::pal9Pending() ? 1 : 0) << 7) | ((s_machineTiming & 7) << 4) |
                     ((s_userDTLock & 1) << 3) | (s_machineType & 7));
}

#if NEXT_TRACE
// --- NEXT_TRACE ring (see NextReg.h for the design rules) ---
struct NextTraceEnt { char kind; uint8_t reg, val; uint16_t pc; uint16_t rep; };
static NextTraceEnt s_nt_ring[64];
static uint32_t s_nt_w = 0;                  // total writes; ring index = w & 63
static uint32_t s_nt_pal = 0, s_nt_cop = 0, s_nt_spr = 0;   // flood counters
extern volatile uint32_t g_nx_palw;                           // NEXTVID::nrHot
volatile int32_t  g_nx_idle_min = 0x7FFFFFFF;   // ESPectrum::loop: worst frame idle per NEXT line
volatile uint32_t g_nx_worst[7];
volatile uint32_t g_nx_worst_trace_us;
volatile uint32_t g_nx_worst_post[5];
volatile uint32_t g_nx_worst_bg[6];
volatile uint32_t g_nx_idle_neg = 0;            // frames with negative idle
// NMI one-shot trace (2026-10-07, esxDOS NMI on Next): from a divMMC NMI
// request on, the next events go straight to the UART instead of the ring,
// which the per-frame IM1 automap floods within a second. h= is a checksum of
// divMMC bank 0 0x2000-0x2152 (esxDOS's NMI/ISR dispatch), found zeroed in
// the post-crash dump.
uint16_t g_nmi_trace_left = 0;
bool g_nx_copper_wr = false;   // NEXTVID::copperRun: the write in progress is the copper's
uint16_t next_div0_sum() {
    const uint8_t* b = DivMMC::bank_ptr[0];
    if (!b) return 0xFFFF;
    uint16_t s = 0;
    for (int i = 0; i < 0x153; i++) s = (uint16_t)((s << 1 | s >> 15) ^ b[i]);
    return s;
}
void next_trace_rst8() {   // esxDOS API call: the function byte follows the RST 8
    const uint16_t sp = Z80::getRegSP();
    const uint16_t ra = (uint16_t)(MemESP::readbyte(sp) | (MemESP::readbyte((uint16_t)(sp + 1)) << 8));
    next_trace_ev('8', MemESP::readbyte(ra), (uint8_t)(ra >> 8));
}
void next_trace_ev(char kind, uint8_t r, uint8_t v) {
    if (g_nmi_trace_left) {
        const uint16_t tpc = Z80::getRegPC();
        // esxDOS's own bank juggling (copy loop 0x051C/0x0521, the RST 8
        // dispatcher 0x09AE/0x09C0, the SD driver 0x1E00-0x1FFF) floods the
        // budget; drop those #E3 writes, keep everything else.
        if (kind == 'E' && ((tpc >= 0x0500 && tpc < 0x0530) || (tpc >= 0x09A0 && tpc < 0x09C8) ||
                            (tpc >= 0x0B40 && tpc < 0x0B80) || (tpc >= 0x1E00 && tpc < 0x2000)))
            goto ring;
        g_nmi_trace_left--;
        Debug::log("[NMIT] %c %02X %02X pc=%04X sp=%04X af=%04X hl=%04X am=%u cm=%u bk=%u",
                   kind, r, v, tpc, Z80::getRegSP(), Z80::getRegAF(), Z80::getRegHL(),
                   (unsigned)DivMMC::automap, (unsigned)DivMMC::conmem,
                   (unsigned)DivMMC::bank);
    }
ring:
    // Consecutive duplicates collapse into one entry with a repeat count —
    // a file loader hammering NR 0x04 with the same page (2600+/s, hw
    // 2026-08-09) purged the whole ring and hid the one interesting write.
    if (s_nt_w) {
        NextTraceEnt& last = s_nt_ring[(s_nt_w - 1) & 63];
        uint16_t pc = Z80::getRegPC();
        if (last.kind == kind && last.reg == r && last.val == v &&
            last.pc == pc && last.rep < 0xFFFF) {
            last.rep++;
            return;
        }
    }
    NextTraceEnt& e = s_nt_ring[s_nt_w & 63];
    e.kind = kind; e.reg = r; e.val = v;
    e.pc = Z80::getRegPC();
    e.rep = 1;
    s_nt_w++;
}
void next_trace_dump(uint32_t frames) {
    extern bool g_next_automap_armed;
    extern volatile uint32_t g_sd_stall_us, g_sd_ops, g_sd_wr_ops;   // DivMMC.cpp
    extern uint32_t g_nextvid_frames;
    extern volatile uint32_t g_nextvid_us;              // NextVideo.cpp
    extern volatile uint32_t g_nextvid_layer_us[7];     // ula / tilemap / L2 / sprites / mix / out
    extern volatile uint32_t g_nx_rows;
    // Who REALLY owns slot 0 right now — the flags can say "bootrom" while a
    // missed replug hands the CPU something else entirely (2026-08-09: flags
    // said boot=1 while the machine visibly ran a machine ROM).
    const uint8_t* s0 = MemESP::ramCurrent[0];
    char own = '?';
    if (s0 == (const uint8_t*)gb_rom_tbblue_loader) own = 'B';       // bootrom
    else if (MemESP::nextRomBase && s0 >= MemESP::nextRomBase &&
             s0 < MemESP::nextRomBase + 0x10000) own = 'N';          // enNextZX
    else if (s0 == MemESP::rom[MemESP::romInUse & 1].direct()) own = 'F'; // 128K fallback
    else if (s0 >= (const uint8_t*)0x20000000 &&
             s0 <  (const uint8_t*)0x20082000) own = 'S';            // SRAM page
    else if (s0 >= (const uint8_t*)0x11000000 &&
             s0 <  (const uint8_t*)0x12000000) own = 'P';            // butter page
    Debug::log("NEXT div0 h=%04X", next_div0_sum());
    Debug::log("NEXT: pc=%04X s0=%08X%c s1=%08X spd=%u/%u"
               " mmu=%02X.%02X.%02X.%02X.%02X.%02X.%02X.%02X"
               " rom=%u boot=%c cfg=%c pg=%02X am=%u 7FFD=%02X 1FFD=%02X DFFD=%02X"
               " FF=%02X fps=%u pal/s=%u cop/s=%u spr/s=%u sd=%ums/%u vid=%ums",
        (unsigned)Z80::getRegPC(), (unsigned)(uintptr_t)s0, own,
        (unsigned)(uintptr_t)MemESP::ramCurrent[1],
        (unsigned)ESPectrum::multiplicator, (unsigned)(NextReg::reg[0x07] & 3),
        MemESP::mmu[0], MemESP::mmu[1], MemESP::mmu[2], MemESP::mmu[3],
        MemESP::mmu[4], MemESP::mmu[5], MemESP::mmu[6], MemESP::mmu[7],
        (unsigned)MemESP::romInUse,
        MemESP::nextBootRomActive ? '1' : '0', MemESP::nextConfigMode ? '1' : '0',
        MemESP::nextConfigPage, (unsigned)DivMMC::automap,
        NextReg::port7FFD, NextReg::port1FFD, NextReg::portDFFD, NextReg::portFF,
        (unsigned)frames, (unsigned)(s_nt_pal + g_nx_palw), (unsigned)s_nt_cop, (unsigned)s_nt_spr,
        (unsigned)(g_sd_stall_us / 1000u), (unsigned)g_sd_ops,
        (unsigned)(g_nextvid_us / 1000u));
    // Frame cost on its own line (the one above is near Debug::log's 256-byte
    // cap). VIDEO::EndFrame returns before its PERF block on the Next, so this
    // is the only place the CPU::loop time shows: cpu = ms per emulated frame
    // (Z80 + rendering + DMA, the whole CPU::loop), vid = the renderer's share,
    // li = line interrupt armed (the frame then runs execute() per instruction).
    {
        extern volatile uint32_t cpu_frame_us;          // CPU.cpp, never reset on Next
        static uint32_t s_prev_cpu = 0;
        const uint32_t c = cpu_frame_us, d = c - s_prev_cpu;
        s_prev_cpu = c;
        const uint32_t f = frames ? frames : 1;
        // Core1 line-ring occupancy since the last dump (NextVideo.cpp): the
        // worst backlog of the window and the depth right now, against the 320
        // the ring holds. A max pinned at 320 = the poster waited on a full
        // ring; a low max is what would let NEXT_C1_RING shrink.
        extern volatile uint32_t g_nx_ring_max, g_nx_ring_now, g_nx_ring_size;
        const uint32_t rmax = g_nx_ring_max, rnow = g_nx_ring_now, rsz = g_nx_ring_size;
        g_nx_ring_max = 0;
        Debug::log("NEXT cpu: %u.%ums/fr vid=%u.%ums/fr li=%u line=%u clk=%u MHz fps=%u draw=%u max=%u ring=%u/%u(now %u)",
            (unsigned)(d / f / 1000u), (unsigned)(d / f % 1000u / 100u),
            (unsigned)(g_nextvid_us / f / 1000u), (unsigned)(g_nextvid_us / f % 1000u / 100u),
            (unsigned)NextInt::lineEnabled(), (unsigned)NextInt::lineNumber(),
            (unsigned)(ESPectrum::multiplicator == 0 ? 3 : 7u << (ESPectrum::multiplicator - 1)),
            (unsigned)frames, (unsigned)g_nextvid_frames,
            (unsigned)ESPectrum::maxSpeed, (unsigned)rmax, (unsigned)rsz, (unsigned)rnow);
        // Printed right after `NEXT cpu:`: the 1 Hz burst overflows the UART ring
        // and the lines at its END are the ones lost (2026-10-11, NXModPlayer).
        {
            // Where core0's frame goes: fast memory path state, Z80 fetches per
            // frame (Z80::regR counts every M1) and their average cost.
            extern uint8_t g_ts_fastrd;   // g_ts_fastmem: MemGates.h (TsFastMem.h)
            extern volatile uint32_t cpu_frame_us;
            static uint32_t s_pr = 0, s_pc = 0;
            const uint32_t rr = Z80::getFetchCounter(), cc = cpu_frame_us;
            const uint32_t dr = rr - s_pr, dc = cc - s_pc;
            s_pr = rr; s_pc = cc;
            volatile uint32_t* xh = (volatile uint32_t*)(XIP_CTRL_BASE + XIP_CTR_HIT_OFFSET);
            volatile uint32_t* xa = (volatile uint32_t*)(XIP_CTRL_BASE + XIP_CTR_ACC_OFFSET);
            const uint32_t hit = *xh, acc = *xa;
            *xh = 0; *xa = 0;                 // write clears
            extern volatile uint32_t g_nx_rlff_n, g_nx_rlff_t;
            Debug::log("NEXT rlff: %u/fr skipped %u%% of the frame | idle loop %u/fr skipped %u%%",
                (unsigned)(g_nx_rlff_n / f),
                (unsigned)((uint64_t)g_nx_rlff_t * 100u / ((uint64_t)CPU::statesInFrame * f)),
                (unsigned)(NextIdle::g_nx_idle_n / f),
                (unsigned)((uint64_t)NextIdle::g_nx_idle_t * 100u / ((uint64_t)CPU::statesInFrame * f)));
            g_nx_rlff_n = g_nx_rlff_t = 0;
            NextIdle::g_nx_idle_n = NextIdle::g_nx_idle_t = 0;
            Debug::log("NEXT z80: fm=%u rd=%u m1=%u/fr %uns/m1 act=%u%% T/fr=%u xip=%u/fr miss=%u/fr",
                (unsigned)g_ts_fastmem, (unsigned)g_ts_fastrd, (unsigned)(dr / f),
                dr ? (unsigned)((uint64_t)dc * 1000u / dr) : 0u,
                CPU::tstates_frame ? (unsigned)((uint64_t)CPU::tstates_active * 100u / CPU::tstates_frame) : 0u,
                (unsigned)CPU::statesInFrame, (unsigned)(acc / f), (unsigned)((acc - hit) / f));
        }
        {   // DACs A..D: writes / steps of 64+ / min..max per frame, writes dropped
            // while NR 08 b3 = 0; then each AY: mixer reg 7, volumes A/B/C, tone A
            extern volatile uint32_t g_nx_dac_n[4], g_nx_dac_jump[4], g_nx_dac_off;
            extern volatile uint8_t g_nx_dac_min[4], g_nx_dac_max[4];
            char ay[96]; int ao = 0;
            for (int i = 0; i < 3; i++) {
                if (!chips[i]) { ao += snprintf(ay + ao, sizeof ay - ao, " -"); continue; }
                const AySound* c = chips[i];
                ao += snprintf(ay + ao, sizeof ay - ao, " %02X/%X%X%X/%03X", c->reg(7),
                               c->reg(8) & 0x1F, c->reg(9) & 0x1F, c->reg(10) & 0x1F,
                               (c->reg(1) & 15) << 8 | c->reg(0));
                if (ao >= (int)sizeof ay) ao = sizeof ay - 1;
            }
            const unsigned fr = f ? f : 1;
            Debug::uartFlushSync();   // the 1 Hz block outruns the 4 KB ring: drain it in parts
            Debug::log("NEXT dac: A %u/%u %02X..%02X B %u/%u %02X..%02X C %u/%u %02X..%02X D %u/%u %02X..%02X off %u | nr08=%02X ay%s",
                (unsigned)(g_nx_dac_n[0] / fr), (unsigned)(g_nx_dac_jump[0] / fr), g_nx_dac_min[0], g_nx_dac_max[0],
                (unsigned)(g_nx_dac_n[1] / fr), (unsigned)(g_nx_dac_jump[1] / fr), g_nx_dac_min[1], g_nx_dac_max[1],
                (unsigned)(g_nx_dac_n[2] / fr), (unsigned)(g_nx_dac_jump[2] / fr), g_nx_dac_min[2], g_nx_dac_max[2],
                (unsigned)(g_nx_dac_n[3] / fr), (unsigned)(g_nx_dac_jump[3] / fr), g_nx_dac_min[3], g_nx_dac_max[3],
                (unsigned)(g_nx_dac_off / fr), NextReg::reg[0x08], ay);
            for (int i = 0; i < 4; i++) { g_nx_dac_n[i] = g_nx_dac_jump[i] = 0; g_nx_dac_min[i] = 255; g_nx_dac_max[i] = 0; }
            g_nx_dac_off = 0;
        }
        {
            extern volatile uint32_t g_nx_drain_n, g_nx_drain_us, g_nx_c1_us, g_nx_idle_neg;
            extern volatile int32_t g_nx_idle_min;
            extern volatile uint32_t g_nx_drain_src[8];
            Debug::log("NEXT perf: idl_min=%d neg=%u/%u c1=%u.%ums/fr drain=%u x %u.%ums/fr (oth %u pal %u lut %u seg %u dma %u sprlog %u)",
                (int)g_nx_idle_min, (unsigned)g_nx_idle_neg, (unsigned)frames,
                (unsigned)(g_nx_c1_us / f / 1000u), (unsigned)(g_nx_c1_us / f % 1000u / 100u),
                (unsigned)g_nx_drain_n,
                (unsigned)(g_nx_drain_us / f / 1000u), (unsigned)(g_nx_drain_us / f % 1000u / 100u),
                (unsigned)g_nx_drain_src[0], (unsigned)g_nx_drain_src[1],
                (unsigned)g_nx_drain_src[2], (unsigned)g_nx_drain_src[3],
                (unsigned)g_nx_drain_src[4], (unsigned)g_nx_drain_src[5]);
            for (int i = 0; i < 8; i++) g_nx_drain_src[i] = 0;
            {
                extern volatile uint32_t g_nx_drain_ra, g_nx_drain_ra_us, g_nx_drain_ra_n;
                Debug::log("NEXT drain: longest %uus from lr=%08X (x%u)", (unsigned)g_nx_drain_ra_us,
                           (unsigned)g_nx_drain_ra, (unsigned)g_nx_drain_ra_n);
                g_nx_drain_ra = g_nx_drain_ra_us = g_nx_drain_ra_n = 0;
                extern volatile uint32_t g_nx_l2band[3];
                Debug::log("NEXT l2band: fill %u/fr borrowed %u/fr wait gave up %u",
                           (unsigned)(g_nx_l2band[0] / f), (unsigned)(g_nx_l2band[1] / f), (unsigned)g_nx_l2band[2]);
                g_nx_l2band[0] = g_nx_l2band[1] = g_nx_l2band[2] = 0;
            }
            {
                extern volatile uint32_t g_nx_dmaw[4][3], g_nx_dmaw_key[4][2], g_nx_dmaw_sh;
                char kb[96]; int kn = 0; kb[0] = 0;
                for (int k = 0; k < 4 && g_nx_dmaw_key[k][0]; k++)
                    kn += snprintf(kb + kn, sizeof kb - kn, " b%u:%02X..%02XFF x%u",
                        (unsigned)(g_nx_dmaw_key[k][0] >> 16), (unsigned)((g_nx_dmaw_key[k][0] >> 8) & 0xFF),
                        (unsigned)(g_nx_dmaw_key[k][0] & 0xFF), (unsigned)g_nx_dmaw_key[k][1]);
                Debug::log("NEXT dmaw: copied %u | l2 %u/%ums map %u/%ums defs %u/%ums ula %u/%ums avg lines %u |%s",
                    (unsigned)g_nx_dmaw_sh,
                    (unsigned)g_nx_dmaw[0][0], (unsigned)(g_nx_dmaw[0][1] / 1000u),
                    (unsigned)g_nx_dmaw[1][0], (unsigned)(g_nx_dmaw[1][1] / 1000u),
                    (unsigned)g_nx_dmaw[2][0], (unsigned)(g_nx_dmaw[2][1] / 1000u),
                    (unsigned)g_nx_dmaw[3][0], (unsigned)(g_nx_dmaw[3][1] / 1000u),
                    (unsigned)((g_nx_dmaw[0][2] + g_nx_dmaw[1][2] + g_nx_dmaw[2][2] + g_nx_dmaw[3][2]) /
                               (g_nx_dmaw[0][0] + g_nx_dmaw[1][0] + g_nx_dmaw[2][0] + g_nx_dmaw[3][0] + 1)), kb);
                memset((void*)g_nx_dmaw, 0, sizeof g_nx_dmaw);
                memset((void*)g_nx_dmaw_key, 0, sizeof g_nx_dmaw_key);
                g_nx_dmaw_sh = 0;
            }
            {
                // the row queue's two renderers: core0 helps while it waits for core1
                extern volatile uint32_t g_nx_c0_us, g_nx_c0_rows, g_nx_c1_rows, g_nx_claim_blk;
                Debug::log("NEXT rows: core0 %u/fr %u.%ums/fr, core1 %u/fr, claim blocked %u/fr",
                    (unsigned)(g_nx_c0_rows / f), (unsigned)(g_nx_c0_us / f / 1000u),
                    (unsigned)(g_nx_c0_us / f % 1000u / 100u), (unsigned)(g_nx_c1_rows / f),
                    (unsigned)(g_nx_claim_blk / f));
                g_nx_c0_us = g_nx_c0_rows = g_nx_c1_rows = g_nx_claim_blk = 0;
                extern volatile uint32_t g_nx_mix[5];
                Debug::log("NEXT mix: rows L2 %u/fr spr %u/fr full %u/fr | 640 second pass %u rows/fr, %u px/row",
                    (unsigned)(g_nx_mix[0] / f), (unsigned)(g_nx_mix[1] / f), (unsigned)(g_nx_mix[2] / f),
                    (unsigned)(g_nx_mix[3] / f), (unsigned)(g_nx_mix[3] ? g_nx_mix[4] / g_nx_mix[3] : 0));
                for (int i = 0; i < 5; i++) g_nx_mix[i] = 0;
                extern volatile uint32_t g_nx_mix_span, g_nx_mix_spanRows;
                Debug::log("NEXT mixspan: %u rows/fr, %u px/row",
                    (unsigned)(g_nx_mix_spanRows / f),
                    (unsigned)(g_nx_mix_spanRows ? g_nx_mix_span / g_nx_mix_spanRows : 0));
                g_nx_mix_span = g_nx_mix_spanRows = 0;
                extern volatile uint32_t g_nx_spr[4];
                Debug::log("NEXT spr: resolve %u/fr (span %u) | tested %u/fr drawn %u/fr",
                    (unsigned)(g_nx_spr[0] / f), (unsigned)(g_nx_spr[0] ? g_nx_spr[1] / g_nx_spr[0] : 0),
                    (unsigned)(g_nx_spr[2] / f), (unsigned)(g_nx_spr[3] / f));
                for (int i = 0; i < 4; i++) g_nx_spr[i] = 0;
                extern volatile uint32_t g_nx_seg[10];
                Debug::log("NEXT seg: rows %u/fr (drain %u/fr) deltas %u/fr passes %u/fr pal %02X x %u..%u | left %u right %u /fr, border-only rows %u/fr",
                    (unsigned)(g_nx_seg[0] / f), (unsigned)(g_nx_seg[1] / f), (unsigned)(g_nx_seg[2] / f),
                    (unsigned)(g_nx_seg[3] / f), (unsigned)g_nx_seg[4],
                    (unsigned)(g_nx_seg[5] == 0xFFFF ? 0 : g_nx_seg[5]), (unsigned)g_nx_seg[6],
                    (unsigned)(g_nx_seg[7] / f), (unsigned)(g_nx_seg[8] / f), (unsigned)(g_nx_seg[9] / f));
                for (int i = 0; i < 10; i++) g_nx_seg[i] = 0;
                g_nx_seg[5] = 0xFFFF;
            }
            // Printed here, right after perf: the later lines are the ones a
            // saturated UART drops (NXModPlayer, hw 2026-10-10: no vid line at all).
            {
                extern volatile uint32_t g_nx_logapply_spr, g_nx_logapply_pal;
                Debug::log("NEXT vidpre: logapply=%u.%u ms/fr sprlog=%u/fr pallog=%u/fr",
                    (unsigned)(g_nextvid_layer_us[6] / f / 1000u), (unsigned)(g_nextvid_layer_us[6] / f % 1000u / 100u),
                    (unsigned)(g_nx_logapply_spr / f), (unsigned)(g_nx_logapply_pal / f));
                g_nx_logapply_spr = g_nx_logapply_pal = 0;
            }
            Debug::log("NEXT vid: ula=%u.%u tm=%u.%u l2=%u.%u spr=%u.%u mix=%u.%u out=%u.%u ms/fr rows=%u/fr",
                (unsigned)(g_nextvid_layer_us[0] / f / 1000u), (unsigned)(g_nextvid_layer_us[0] / f % 1000u / 100u),
                (unsigned)(g_nextvid_layer_us[1] / f / 1000u), (unsigned)(g_nextvid_layer_us[1] / f % 1000u / 100u),
                (unsigned)(g_nextvid_layer_us[2] / f / 1000u), (unsigned)(g_nextvid_layer_us[2] / f % 1000u / 100u),
                (unsigned)(g_nextvid_layer_us[3] / f / 1000u), (unsigned)(g_nextvid_layer_us[3] / f % 1000u / 100u),
                (unsigned)(g_nextvid_layer_us[4] / f / 1000u), (unsigned)(g_nextvid_layer_us[4] / f % 1000u / 100u),
                (unsigned)(g_nextvid_layer_us[5] / f / 1000u), (unsigned)(g_nextvid_layer_us[5] / f % 1000u / 100u),
                (unsigned)(g_nx_rows / f));
            g_nx_rows = 0;
            {
                Debug::uartFlushSync();
                // Host PCs of the worst frame (100 us samples on core0), 64-byte
                // buckets; resolve against the ELF.
                extern uint32_t* g_nxprof_worst; extern volatile uint32_t g_nxprof_worst_n;
                const uint32_t n = g_nxprof_worst ? g_nxprof_worst_n : 0;
                uint32_t pk[24]; uint16_t pc[24]; int np = 0;
                for (uint32_t i = 0; i < n; i++) {
                    const uint32_t k = g_nxprof_worst[i] & ~63u;
                    int j = 0; while (j < np && pk[j] != k) j++;
                    if (j == np) { if (np == 24) continue; pk[np] = k; pc[np] = 0; np++; }
                    pc[j]++;
                }
                for (int part = 0; part < 2; part++) {
                    char pb[220]; int po = snprintf(pb, sizeof pb, part ? "NEXT hostpc+" : "NEXT hostpc %u:", (unsigned)n);
                    for (int t = 0; t < 8; t++) {
                        int b = -1; for (int j = 0; j < np; j++) if (pc[j] && (b < 0 || pc[j] > pc[b])) b = j;
                        if (b < 0) break;
                        po += snprintf(pb + po, sizeof pb - po, " %08X:%u", (unsigned)pk[b], (unsigned)pc[b]);
                        pc[b] = 0;
                    }
                    Debug::log("%s", pb);
                }
            }
            {
                // core1 (the renderer): the last NXPROF1_N samples, 64-byte buckets
                const uint32_t w = g_nxprof1 ? g_nxprof1_w : 0;
                const uint32_t n = w < NXPROF1_N ? w : NXPROF1_N;
                uint32_t pk[32]; uint16_t pc[32]; int np = 0;
                for (uint32_t i = 0; i < n; i++) {
                    const uint32_t k = g_nxprof1[i] & ~63u;
                    int j = 0; while (j < np && pk[j] != k) j++;
                    if (j == np) { if (np == 32) continue; pk[np] = k; pc[np] = 0; np++; }
                    pc[j]++;
                }
                for (int part = 0; part < 2; part++) {
                    char pb[220]; int po = snprintf(pb, sizeof pb, part ? "NEXT hostpc1+" : "NEXT hostpc1 %u:", (unsigned)n);
                    for (int t = 0; t < 8; t++) {
                        int b = -1; for (int j = 0; j < np; j++) if (pc[j] && (b < 0 || pc[j] > pc[b])) b = j;
                        if (b < 0) break;
                        po += snprintf(pb + po, sizeof pb - po, " %08X:%u", (unsigned)pk[b], (unsigned)pc[b]);
                        pc[b] = 0;
                    }
                    Debug::log("%s", pb);
                }
            }
            extern volatile uint32_t g_nx_worst[7];
            Debug::log("NEXT worst frame: before cpu %uus, CPU::loop %uus (sd %u drain %u queue %u sdwait %u), after %uus (trace %uus)",
                (unsigned)g_nx_worst[0], (unsigned)g_nx_worst[1], (unsigned)g_nx_worst[3], (unsigned)g_nx_worst[4],
                (unsigned)g_nx_worst[5], (unsigned)g_nx_worst[6], (unsigned)g_nx_worst[2], (unsigned)g_nx_worst_trace_us);
            Debug::log("NEXT worst post: background %u storage %u audio %u keyboard %u tail %uus",
                (unsigned)g_nx_worst_post[0], (unsigned)g_nx_worst_post[1], (unsigned)g_nx_worst_post[2],
                (unsigned)g_nx_worst_post[3], (unsigned)g_nx_worst_post[4]);
            Debug::log("NEXT worst bg: zifi %u rtc %u overlays %u nvram %u mouse %u net/uart %uus",
                (unsigned)g_nx_worst_bg[0], (unsigned)g_nx_worst_bg[1], (unsigned)g_nx_worst_bg[2],
                (unsigned)g_nx_worst_bg[3], (unsigned)g_nx_worst_bg[4], (unsigned)g_nx_worst_bg[5]);
            extern volatile uint32_t g_nx_emit_us;
            {
                extern volatile uint32_t g_nx_rowprobe[2][8];
                for (int k = 0; k < 2; k++)
                    Debug::log("NEXT row%u: r17=%02X sy=%u opaque L2=%u spr=%u ula=%u hash L2=%08X spr=%08X ula=%08X",
                        k ? 191u : 0u, (unsigned)g_nx_rowprobe[k][0], (unsigned)g_nx_rowprobe[k][1],
                        (unsigned)g_nx_rowprobe[k][2], (unsigned)g_nx_rowprobe[k][3], (unsigned)g_nx_rowprobe[k][4],
                        (unsigned)g_nx_rowprobe[k][5], (unsigned)g_nx_rowprobe[k][6], (unsigned)g_nx_rowprobe[k][7]);
            }
            {
                // Top ports by time, then the hottest PC blocks / MMU pages.
                char buf[200]; int o = 0;
                o += snprintf(buf + o, sizeof buf - o, "NEXT io: in %u/fr %uus/fr out %u/fr %uus/fr |",
                    (unsigned)(s_io_n[0] / f), (unsigned)(s_io_us[0] / f), (unsigned)(s_io_n[1] / f), (unsigned)(s_io_us[1] / f));
                for (int k = 0; k < 6; k++) {
                    int b = -1;
                    for (int i = 0; i < 32; i++) if (s_io[i].n && (b < 0 || s_io[i].us > s_io[b].us)) b = i;
                    if (b < 0) break;
                    o += snprintf(buf + o, sizeof buf - o, " %04X%c%u/%u", s_io[b].port, s_io[b].out ? 'w' : 'r',
                                  (unsigned)(s_io[b].n / f), (unsigned)(s_io[b].us / f));
                    s_io[b].us = 0; s_io[b].n = 1;   // taken (cleared below)
                    if (o > (int)sizeof buf - 24) break;
                }
                Debug::log("%s", buf);
                memset(s_io, 0, sizeof s_io); s_io_n[0] = s_io_n[1] = s_io_us[0] = s_io_us[1] = 0;
                extern volatile uint16_t* g_nx_pcs;
                extern volatile uint16_t* g_nx_pcpg;
                uint32_t tot = 0; if (g_nx_pcs) for (int i = 0; i < 2048; i++) tot += g_nx_pcs[i];
                o = snprintf(buf, sizeof buf, "NEXT pc: n=%u", (unsigned)tot);
                for (int k = 0; k < 8 && tot; k++) {
                    int b = 0;
                    for (int i = 1; i < 2048; i++) if (g_nx_pcs[i] > g_nx_pcs[b]) b = i;
                    if (!g_nx_pcs[b]) break;
                    o += snprintf(buf + o, sizeof buf - o, " %04X:%u%%", b << 5, (unsigned)(g_nx_pcs[b] * 100u / tot));
                    g_nx_pcs[b] = 0;
                }
                o += snprintf(buf + o, sizeof buf - o, " | pg");
                for (int k = 0; k < 5; k++) {
                    int b = 0;
                    for (int i = 1; i < 256; i++) if (g_nx_pcpg[i] > g_nx_pcpg[b]) b = i;
                    if (!g_nx_pcpg[b] || !tot) break;
                    o += snprintf(buf + o, sizeof buf - o, " %02X:%u%%", b, (unsigned)(g_nx_pcpg[b] * 100u / tot));
                    g_nx_pcpg[b] = 0;
                }
                Debug::log("%s", buf);
                if (g_nx_pcs) {
                    for (int i = 0; i < 2048; i++) g_nx_pcs[i] = 0;
                    for (int i = 0; i < 256; i++) g_nx_pcpg[i] = 0;
                }
            }
            {
                extern volatile uint32_t (*g_nx_dmav)[3];
                char vb[200]; int vo = snprintf(vb, sizeof vb, "NEXT dmav (page:off/256 n bytes):");
                for (int k = 0; g_nx_dmav && k < 8 && g_nx_dmav[k][0]; k++)
                    vo += snprintf(vb + vo, sizeof vb - vo, " %02X:%02X %u %u",
                        (unsigned)((g_nx_dmav[k][0] >> 8) & 0xFF), (unsigned)(g_nx_dmav[k][0] & 0xFF),
                        (unsigned)(g_nx_dmav[k][1] / f), (unsigned)(g_nx_dmav[k][2] / f));
                Debug::log("%s | 6E=%02X 6F=%02X 12=%02X", vb, NextReg::reg[0x6E], NextReg::reg[0x6F], NextReg::reg[0x12]);
                for (int k = 0; g_nx_dmav && k < 8; k++) g_nx_dmav[k][0] = g_nx_dmav[k][1] = g_nx_dmav[k][2] = 0;
            }
            {
                extern volatile uint32_t g_sdk[5], g_sd_ring[12], g_sd_ring_w;
                extern volatile uint32_t g_nx_qwait_us, g_nx_qwait_max;
                Debug::log("NEXT sdw: c1 read %ums max %uus | c0 wait sd %ums max %uus | queue wait %ums max %uus",
                    (unsigned)(g_sd_as_c1_us / 1000u), (unsigned)g_sd_as_c1_max,
                    (unsigned)(g_sd_as_c0wait_us / 1000u), (unsigned)g_sd_as_c0wait_max,
                    (unsigned)(g_nx_qwait_us / 1000u), (unsigned)g_nx_qwait_max);
                g_sd_as_c1_us = g_sd_as_c1_max = g_sd_as_c0wait_us = g_sd_as_c0wait_max = 0;
                g_nx_qwait_us = g_nx_qwait_max = 0;
                char sb[220]; int so = snprintf(sb, sizeof sb, "NEXT sd: hit %u async %u burst %u single %u posted %u | last:",
                    (unsigned)g_sdk[0], (unsigned)g_sdk[1], (unsigned)g_sdk[2], (unsigned)g_sdk[3], (unsigned)g_sdk[4]);
                const uint32_t w = g_sd_ring_w;
                for (uint32_t k = (w > 12 ? w - 12 : 0); k < w && so < (int)sizeof sb - 14; k++) {
                    const uint32_t e = g_sd_ring[k % 12];
                    so += snprintf(sb + so, sizeof sb - so, " %c%lu", "HABS"[e >> 30], (unsigned long)(e & 0x3FFFFFFF));
                }
                Debug::log("%s", sb);
                for (int k = 0; k < 5; k++) g_sdk[k] = 0;
            }
            Debug::log("NEXT core0: emit=%u.%ums/fr | L2 12=%02X 13=%02X 16=%02X 17=%02X 70=%02X 71=%02X clip=%u,%u,%u,%u spr=%u,%u,%u,%u 15=%02X 68=%02X 14=%02X 4A=%02X 4B=%02X 6A=%02X 6B=%02X",
                (unsigned)(g_nx_emit_us / f / 1000u), (unsigned)(g_nx_emit_us / f % 1000u / 100u),
                NextReg::reg[0x12], NextReg::reg[0x13], NextReg::reg[0x16], NextReg::reg[0x17], NextReg::reg[0x70], NextReg::reg[0x71],
                NEXTVID::clip[0][0], NEXTVID::clip[0][1], NEXTVID::clip[0][2], NEXTVID::clip[0][3],
                NEXTVID::clip[1][0], NEXTVID::clip[1][1], NEXTVID::clip[1][2], NEXTVID::clip[1][3],
                NextReg::reg[0x15], NextReg::reg[0x68], NextReg::reg[0x14], NextReg::reg[0x4A], NextReg::reg[0x4B],
                NextReg::reg[0x6A], NextReg::reg[0x6B]);
            g_nx_emit_us = 0;
            g_nx_drain_n = g_nx_drain_us = g_nx_c1_us = g_nx_idle_neg = 0;
            g_nx_idle_min = 0x7FFFFFFF;
        }
        // Which layer the renderer's time goes to (same sampled lines, the
        // paper rows only — border rows are in vid but not split here).
        {
            extern volatile uint32_t g_dma_n, g_dma_bytes, g_dma_endframes, g_dma_maxlen,
                                     g_dma_max_pc, g_dma_max_src, g_dma_max_dst, g_dma_max_fl;
            // fl: 100 A->B, 80 auto-restart, 40 #0B (Zilog) mode, 2 src IO,
            // 1 dst IO, bits 12+ = prescaler
            Debug::log("NEXT dma: n=%u bytes=%u endfr=%u max=%u pc=%04X %04X->%04X fl=%X",
                (unsigned)g_dma_n, (unsigned)g_dma_bytes, (unsigned)g_dma_endframes,
                (unsigned)g_dma_maxlen, (unsigned)g_dma_max_pc, (unsigned)g_dma_max_src,
                (unsigned)g_dma_max_dst, (unsigned)g_dma_max_fl);
            g_dma_n = g_dma_bytes = g_dma_endframes = g_dma_maxlen = 0;
        }
        {   // line interrupt: raised / missed (pulse over when seen, by T) / INT acks;
            // event position and frame length in 3.5 MHz T; longest DMA bus hold
            extern volatile uint32_t g_li_raise, g_li_late, g_li_late_by, g_int_ack;
            extern volatile uint32_t g_dma_hold_max, g_dma_hold_t0, g_dma_hold_len, g_vk_read;
            extern uint32_t usb_kbd_report_count();
            static uint32_t s_prevRep = 0;
            const uint32_t rep = usb_kbd_report_count();
            Debug::log("NEXT int: li raise=%u late=%u ack=%u pos=%dT frame=%uT | dma hold max=%uT at %uT len=%u | kbd usb=%u vk=%u",
                (unsigned)g_li_raise, (unsigned)g_li_late, (unsigned)g_int_ack,
                (int)NextInt::linePosT(), (unsigned)(CPU::statesInFrame >> ESPectrum::multiplicator),
                (unsigned)g_dma_hold_max, (unsigned)g_dma_hold_t0, (unsigned)g_dma_hold_len,
                (unsigned)(rep - s_prevRep), (unsigned)g_vk_read);
            s_prevRep = rep;
            g_vk_read = 0;
            g_li_raise = g_li_late = g_li_late_by = g_int_ack = 0;
            g_dma_hold_max = g_dma_hold_t0 = g_dma_hold_len = 0;
        }
#if PERF_TRACE && PERF_HIST
        // The TS-Conf opcode histogram (Z80_JLS.cpp, exec_nocheck), printed here
        // because VIDEO::EndFrame's PERF block never runs on the Next. ns/instr is
        // against this second's CPU::loop time — pessimistic by the histogram's own
        // cost, as on TS-Conf. Prefixes count as themselves (ED = IN/OUT/block ops).
        {
            extern uint32_t z80_op_hist[256];
            uint64_t total = 0;
            for (int i = 0; i < 256; i++) total += z80_op_hist[i];
            char hl[200]; int hp = 0; uint8_t used[256] = {0};
            for (int k = 0; k < 16 && hp < (int)sizeof(hl) - 12; k++) {
                int best = -1; uint32_t bv = 0;
                for (int i = 0; i < 256; i++)
                    if (!used[i] && z80_op_hist[i] > bv) { bv = z80_op_hist[i]; best = i; }
                if (best < 0 || bv == 0) break;
                used[best] = 1;
                hp += snprintf(hl + hp, sizeof(hl) - hp, " %02X:%u", best,
                               (unsigned)(total ? (uint64_t)bv * 100 / total : 0));
            }
            hl[hp] = 0;
            Debug::log("NEXT z80: %u instr/fr %u ns/instr top%%:%s",
                (unsigned)(total / f), (unsigned)(total ? (uint64_t)d * 1000 / total : 0), hl);
            for (int i = 0; i < 256; i++) z80_op_hist[i] = 0;
            extern void pcprof_dump();
            pcprof_dump();
        }
#endif
    }
    s_nt_pal = s_nt_cop = s_nt_spr = 0;
    g_nx_palw = 0;
    g_sd_stall_us = 0;
    g_sd_ops = 0;
    g_sd_wr_ops = 0;
    g_nextvid_us = 0;
    g_nextvid_frames = 0;
    for (int i = 0; i < 7; i++) g_nextvid_layer_us[i] = 0;
    // Ring line: newest LAST, budget spent from the newest end backwards, a
    // leading ".." marks dropped older entries (never drop the newest — the
    // NeoGS ring printed oldest-first once and hid every wedge it saw).
    char line[200];
    uint32_t n = s_nt_w < 64 ? s_nt_w : 64;
    int pos = sizeof(line);
    line[--pos] = 0;
    uint32_t shown = 0;
    for (uint32_t i = 0; i < n; i++) {
        const NextTraceEnt& e = s_nt_ring[(s_nt_w - 1 - i) & 63];
        char ent[24];
        int el;
        if (e.rep > 1)
            el = snprintf(ent, sizeof(ent), "%c%02X:%02X@%04X*%u ",
                          e.kind, e.reg, e.val, e.pc, (unsigned)e.rep);
        else
            el = snprintf(ent, sizeof(ent), "%c%02X:%02X@%04X ", e.kind, e.reg, e.val, e.pc);
        if (pos - el < 2) break;
        pos -= el;
        memcpy(line + pos, ent, el);
        shown++;
    }
    Debug::log("NEXT ev:%s%s", shown < s_nt_w ? " .." : "", line + pos);
}
#define NT_EV(k, r, v) next_trace_ev((k), (r), (v))
#else
#define NT_EV(k, r, v) ((void)0)
#endif

static void applyRomSelect();

void NextReg::reset(bool hard) {
    // The zxnext.vhd reset block (~4900-5111) runs on BOTH reset types — a
    // soft reset restores the nextreg defaults too (zx_go pins this with
    // TestResetRestoresDefaults). hw 2026-08-10: Alt-F11 after a 48K .tap
    // game used to leave the game's Layer2/sprite/alt-ROM latches standing —
    // NextZXOS rebooted into garbage and only the full MCU reboot recovered.
    //
    // Alt-ROM first: reset PROMOTES the staged nibble (bits 3:0) into the
    // active one (bits 7:4) — zxnext.vhd:2255, both reset types.
    uint8_t altStaged = reg[0x8C] & 0x0F;
    // Peripheral setup the BIOS writes before its soft reset into a
    // personality: zxnext.vhd's reset block leaves NR 0x05/0x06/0x08/0x09/0x0A
    // and the port-decode enables 0x82-0x85 alone apart from a few bits
    // (hotkeys, contention-disable, sprite tie; 0x82-0x85 only when 0x85 bit 7
    // asks for it). Zeroing them on a soft reset threw the BIOS setup away —
    // the divMMC automap enable among it.
    static const uint8_t kKeep[] = { 0x05, 0x06, 0x08, 0x09, 0x0A, 0x82, 0x83, 0x84, 0x85,
                                     0x86, 0x87, 0x88, 0x89 };
    uint8_t kept[sizeof(kKeep)];
    for (unsigned i = 0; i < sizeof(kKeep); i++) kept[i] = reg[kKeep[i]];
    // NR 0x7F is not in the VHDL reset block: FF at power-on only
    const uint8_t old80 = reg[0x80];
    static bool s_powered = false;
    const uint8_t user7F = s_powered ? reg[0x7F] : 0xFF;
    s_powered = true;
    for (int i = 0; i < 256; i++) reg[i] = 0;
    reg[0x7F] = user7F;
    reg[0x80] = (uint8_t)((old80 << 4) | (old80 & 0x0F));   // zxnext.vhd 2186: 7:4 <= 3:0
    reg[0x0B] = 0x01;                                       // zxnext.vhd 4939-4941
    s_ioTrapCause = 0;
    selected = 0;
    reg[0x8C] = (uint8_t)((altStaged << 4) | altStaged);
    MemESP::altrom_rd  = (reg[0x8C] & 0x80) && !(reg[0x8C] & 0x40);
    MemESP::altrom_wr  = (reg[0x8C] & 0x80) && (reg[0x8C] & 0x40);
    if (hard) {
        // TBBLUE power-on: bootrom readable + config_mode = 1 — but only when
        // the boot chain is armed (TBBLUE.FW found on SD, see ESPectrum::setup).
        // The direct enNextZX boot never enters config mode: nothing would
        // exit it (boot.bin's first NR 0x03 write is the exit on real hw).
        MemESP::nextBootRomActive = (MemESP::nextBootRom != nullptr);
        MemESP::nextConfigMode = MemESP::nextBootRomActive;
        MemESP::nextConfigPage = 0;
        s_machineType = 0x03; s_machineTiming = 0x03;
        s_userDTLock = 0;
        reg[0x8C] = 0;                 // power-on: alt-ROM fully off
        MemESP::altrom_rd = MemESP::altrom_wr = false;
        MemESP::altrom_sel = 0;
    }
    // Soft reset: bootrom/config flags survive (NextZXOS soft-resets between
    // boot stages with config mode already off; re-arming would loop the boot).
    portFF = 0;
    portFFIntDisable = false;
    applyTimexMode(0);   // leave Timex hi-res with the latch
    s_hiresSeen = false;
    VIDEO::timexHiresRequest(false);
    port7FFD = 0;
    portDFFD = 0;
    port1FFD = 0;
    port123B = 0;
    l2Offset = 0;
    MemESP::wr_overlay_mask = 0;
    MemESP::l2_rd_mask = 0;
    NextMf::reset();        // multiface.vhd: reset on either reset type
    NextInt::reset(hard);   // interrupt block + CTC: both reset types (reg[0xC0-0xCE] too)
    // MMU defaults: ROM in slots 0/1, banks 5,2,0 in RAM slots
    reg[0x50] = 0xFF; reg[0x51] = 0xFF;
    reg[0x52] = 10;   reg[0x53] = 11;
    reg[0x54] = 4;    reg[0x55] = 5;
    reg[0x56] = 0;    reg[0x57] = 1;
    reg[0x07] = 0;    // 3.5 MHz
    // Reset defaults pinned to the FPGA VHDL (zx_go VHDL_CONFORMANCE.md)
    if (hard) {
        reg[0x05] = 0x41; // periph1: joy0 Kempston 1, joy1 Sinclair 2, 50 Hz, scandoubler
                          // (zxnext.vhd nr_05_joy0 := "001", joy1 := "000")
        reg[0x06] = 0xA0; // periph2: hotkeys enabled
        reg[0x08] = 0x10; // periph3: internal speaker on (nr_08_internal_speaker_en)
        // DivMMC automap enable: power-on 0, set by the BIOS (TBBLUE.FW
        // PERIPH5 bit 4). The direct enNextZX boot has no BIOS to do it.
        reg[0x0A] = MemESP::nextBootRom ? 0x00 : 0x10;
        // Sound: power-on is YM mode, TurboSound and DACs off (zxnext.vhd
        // 1112-1119); TBBLUE.FW then writes its config.ini defaults — psgmode 1
        // (AY), turbosound 1, dac 1, stereomode 0 (app/src/config.c
        // settingDefaults, boot.c PERIPH2/3). The direct enNextZX boot has no
        // BIOS to do it.
        if (!MemESP::nextBootRom) { reg[0x06] |= 0x01; reg[0x08] |= 0x0A; }
        reg[0x82] = reg[0x83] = reg[0x84] = 0xFF;   // every internal port on
        reg[0x85] = 0x8F;
        reg[0x86] = reg[0x87] = reg[0x88] = 0xFF;   // expansion-bus port enables
        reg[0x89] = 0x8F;                           // (zxnext.vhd 1231-1235)
    } else {
        for (unsigned i = 0; i < sizeof(kKeep); i++) reg[kKeep[i]] = kept[i];
        reg[0x06] |= 0xA0;                // hotkey CPU speed / 50-60 Hz re-enabled
        reg[0x08] &= (uint8_t)~0x40;      // contention disable cleared
        reg[0x09] &= (uint8_t)~0x10;      // sprite tie cleared
        if (reg[0x85] & 0x80) {           // "reset the enables on soft reset"
            reg[0x82] = reg[0x83] = reg[0x84] = 0xFF;
            reg[0x85] = 0x8F;
        }
        if (!(reg[0x89] & 0x80)) {        // zxnext.vhd 5061: bus enables on reset_type 0
            reg[0x86] = reg[0x87] = reg[0x88] = 0xFF;
            reg[0x89] |= 0x0F;
        }
    }
    portEnRecalc();
    NextPorts::audioReset();   // turbosound select/pans, DACs (both reset types)
    NextPorts::uartReset(hard);
    NextPorts::ulapReset();
    reg[0x12] = 0x08; // Layer 2 bank
    reg[0x13] = 0x0B; // Layer 2 shadow bank
    reg[0x42] = 0x07; // ULANext attribute format
    reg[0x6E] = 0x2C; // tilemap map base
    reg[0x6F] = 0x0C; // tilemap tiles base
    // divMMC programmable entry points (esxdos-compatible defaults):
    // 0xB8 = RST-trap mask (0x00/0x08/0x38), 0xB9 = "always" mask (0x0000
    // stays always-on), 0xBB = 0066/04C6/0562/3Dxx/off-region set
    reg[0x02] = 0x01; // read-back: bit0 = "last reset was soft" (VHDL default)
    reg[0xB8] = 0x83;
    reg[0xB9] = 0x01;
    reg[0xBA] = 0x00;
    reg[0xBB] = 0xCD;
    Debug::log("NextReg: programmable divMMC traps armed (B8=83 B9=01 BB=CD)");
    reg[0x14] = 0xE3; // global transparency colour
    reg[0x4A] = 0xE3; // fallback colour (VHDL reset value)
    reg[0x4B] = 0xE3; // sprite transparency index
    reg[0x4C] = 0x0F; // tilemap transparency index
    reg[0x03] = reg03Compose();
    reg[0x04] = MemESP::nextConfigPage;
    // ROM index / ROM3 / Alt-ROM half from the reset latches (7FFD/1FFD = 0)
    MemESP::romLatch = 0;
    applyRomSelect();
    MemESP::applyNextConfigWindow();
    contendRecalc();
    NextRtc::reset();   // zxnext.vhd: i2c_scl_o / i2c_sda_o <= '1' on reset
    NextBoard::reset(hard);   // NR 0xF0 soft reset = 0x80 (select mode)
}

// Raster line for reg 0x1E/0x1F and the line interrupt. Line 0 is the first
// line of the pixel area, matching the Next convention. tstates run 2^m
// faster in turbo, so scale back to base-clock units first.
uint16_t NextReg::activeLine() {
    uint32_t ts = CPU::tstates >> ESPectrum::multiplicator;
    uint32_t line = ts / VIDEO::tStatesPerLine;
    // ULA paper starts 64 lines into the frame on 128K timing (63 border
    // lines + sync); the Next counts video lines from paper start. At 60 Hz
    // the top border is 26 lines shorter (NEXTVID::computeGeometry).
    const uint32_t paperStart = paperLine();
    uint32_t total = CPU::statesInFrame >> ESPectrum::multiplicator;
    uint32_t lines = total / VIDEO::tStatesPerLine;
    // zxnext.vhd 5983-5986 read cvc: offset by NR 0x64 and stepping at hc 128
    // (64 T into the line, the paper's left edge — zxula_timing.vhd 455-472)
    if (ts % VIDEO::tStatesPerLine < 64) line += lines - 1;
    return (uint16_t)((line + lines - paperStart + reg[0x64]) % lines);
}

// A raster-line wait (Warhawk 906D: IN A,(C) / CP #BF / JP NZ on NR 0x1F, ~8000
// reads a frame = 82% of guest time; each read is a flash port handler, 6 ms of
// core0 per frame — hw 2026-10-09). The second read of NR 0x1E/0x1F by the same
// instruction within 128 T skips guest time to the moment the value next changes
// (hc 128 of the next line, see activeLine), rendering the lines on the way —
// the TS-Conf DMAStatus fast-forward. Bounded by the running slice (stFrame:
// interrupt / DMA events end it) and the frame tail, never in the INT window.
#if NEXT_TRACE
volatile uint32_t g_nx_rlff_n = 0, g_nx_rlff_t = 0;
volatile uint32_t (*g_nx_dmav)[3] = nullptr;   // [8][3], in the NEXTVID profile block
#endif
// Kept per register: TX-1696 reads 0x1E then 0x1F from two places in one loop
// (B14A: CALL 8A48 / CP L / JP NC), which one shared slot never saw repeat.
void NextReg::rasterPollFF(uint8_t k) {
    // Only with the frame INT alone. With a line interrupt / CTC / hardware IM2
    // armed (NextInt::ex) the skip made TX-1696's bottom sprites stick and jump
    // every few seconds (hw 2026-10-09; gone with the skip off) — its main loop
    // waits for line 193 right behind the line-192 interrupt, with the sample
    // CTC and DMA-yields-to-interrupts set. Those polls go through the cheap
    // port path in Ports::input instead.
    // With them armed it now runs too, but only inside the unchecked slice:
    // there CPU::stFrame IS the next interrupt / DMA event (CPU::frameRest), so
    // the skip below can never pass one.
    //
    // A skip is safe only when the loop does NOTHING between two reads but read
    // again: the same PC, the same value read last time, and none of the main
    // registers moved. The value test alone was the "start takes minutes" bug
    // (hw 2026-10-10, Bomb Jack): its delay is "wait for line 250, HL times"
    // (#D961: OUT #243B,#1F / IN / CP #FA / JR NZ / DEC HL / JR NZ), and at line
    // 250 the loop reads #FA ~34 times within that one line, decrementing HL
    // each time — a skip to the next line change ate those, so every DEC HL cost
    // a whole frame.
    extern bool z80_in_nocheck;
    if (NextInt::ex && !z80_in_nocheck) return;
    static uint16_t s_pc[2] = { 0xFFFF, 0xFFFF };
    static uint32_t s_t[2] = { 0, 0 };
    static uint32_t s_r[2][3];
    static uint16_t s_v[2] = { 0xFFFF, 0xFFFF };
    const uint32_t now = CPU::tstates;
    const uint16_t pc = Z80::getRegPC();
    const uint8_t m = ESPectrum::multiplicator;
    const uint16_t line = activeLine();
    const uint16_t v = k ? (line & 0xFF) : ((line >> 8) & 1);
    const uint32_t r0 = ((uint32_t)Z80::getRegBC() << 16) | Z80::getRegDE();
    const uint32_t r1 = ((uint32_t)Z80::getRegHL() << 16) | Z80::getRegSP();
    const uint32_t r2 = ((uint32_t)Z80::getRegIX() << 16) | Z80::getRegIY();
    const bool again = pc == s_pc[k] && (uint32_t)(now - s_t[k]) < (128u << m) &&
                       v == s_v[k] && r0 == s_r[k][0] && r1 == s_r[k][1] && r2 == s_r[k][2];
    s_pc[k] = pc; s_v[k] = v;
    s_r[k][0] = r0; s_r[k][1] = r1; s_r[k][2] = r2;
    if (!again || (int32_t)now < CPU::IntEnd) { s_t[k] = now; return; }
    const uint32_t tpl = VIDEO::tStatesPerLine;
    const uint32_t cur = now >> m, inl = cur % tpl;
    const uint32_t change = cur - inl + (inl < 64 ? 64 : tpl + 64);
    uint32_t tgt = change << m;
    uint32_t lim = CPU::statesInFrame - (uint32_t)CPU::IntEnd;
    if (CPU::stFrame < lim) lim = CPU::stFrame;
    if (tgt > lim) tgt = lim;
    if (tgt > now) {
        NEXTVID::haltAdvanceTo(tgt);
#if NEXT_TRACE
        g_nx_rlff_n = g_nx_rlff_n + 1;
        g_nx_rlff_t = g_nx_rlff_t + (CPU::tstates - now);
#endif
    }
    s_t[k] = CPU::tstates;
}

static void applyRomSelect();
uint8_t NextReg::machineType() { return s_machineType; }
// SRAM (a few bytes): NextInt sync()/pulseUp() read it on every interrupt check.
__not_in_flash("next") uint8_t NextReg::machineTiming() { return s_machineTiming; }
uint16_t NextReg::paperLineV = 63;

uint8_t NextReg::rasterKey() {
    const uint8_t t = s_machineTiming;
    const uint8_t k = (t <= 1) ? 0 : (t == 4) ? 2 : 1;      // 48K / 128K,+3 / Pentagon
    const bool h = (reg[0x05] & 0x04) && k != 2;           // Pentagon is 50 Hz only
    return (uint8_t)(k | (h ? 4 : 0));
}

NextReg::Raster NextReg::raster() {
    const uint8_t key = rasterKey();
    const bool h = key & 4;
    Raster r;
    r.hz60 = h;
    // ULA INT pulse = CPU cycles at ANY speed (zxnext.vhd ~2033, pulse_count on
    // i_CLK_CPU): 32 for the 48K and +3 timings, 36 for 128K and Pentagon. The
    // Next's own figures, not the classic machines' (Pentagon here is 36).
    r.intEnd = (s_machineTiming <= 1 || s_machineTiming == 3) ? 32 : 36;
    switch (key & 3) {
        case 0:  // 48K: 224 T x 312 (50) / 262 (60) — 3.5 MHz frames
            r.kind = 0; r.tpl = 224; r.lines = h ? 262 : 312;
            r.micros = h ? MICROS_PER_FRAME_EVO60 : MICROS_PER_FRAME_48;
            break;
        case 2:  // Pentagon: 224 T x 320, 50 Hz only
            r.kind = 2; r.tpl = 224; r.lines = 320;
            r.micros = MICROS_PER_FRAME_PENTAGON;
            break;
        default: // 128K / +3: 228 T x 311 (50) / 261 (60)
            r.kind = 1; r.tpl = 228; r.lines = h ? 261 : 311;
            // 60 Hz: 59508 T at the 128K pacing rate (70908 T per 19992 us)
            r.micros = h ? 16778 : MICROS_PER_FRAME_128;
            break;
    }
    return r;
}

void NextReg::applyRasterPaper() {
    const Raster r = raster();
    const uint16_t p = (r.kind == 0) ? 64 : (r.kind == 2) ? 80 : 63;
    paperLineV = (uint16_t)(r.hz60 ? p - 26 : p);
}
// zxnext.vhd 2091 (nmi_assert_divmmc: hotkey or NR 02 bit 2, AND NR 06 bit 4),
// 6349 (hotkey_drive needs port_divmmc_io_en = NR 83 bit 0), 2102 (none in
// config mode), 2106-2111 + the nmi_state HOLD (no new NMI while divmmc_nmi_hold
// = automap or the latched button). divmmc.vhd: button_nmi set by the NMI,
// dropped once automap is held — so an NMI taken while already automapped
// leaves no button behind.
uint8_t NextReg::divNmiRequest(bool hotkey) {
    if (!DivMMC::enabled || MemESP::nextConfigMode || MemESP::nextBootRomActive) return 1;
    if (!(reg[0x06] & 0x10)) return 1;
    if (NextMf::active()) return 2;      // zxnext.vhd 2109: not while the MF is master
    if (hotkey && !(reg[0x83] & 0x01)) return 1;
    if (DivMMC::next_nmi_hold) {
        if (DivMMC::next_nmi_btn || DivMMC::automap) return 2;
        DivMMC::next_nmi_hold = false;
    }
    DivMMC::next_nmi_btn = !DivMMC::automap;
    DivMMC::next_nmi_hold = true;
    Z80::triggerNMI();
    // Take it at the next instruction boundary, not at the end of the running
    // slice (the next INT/DMA event or the frame tail), as NextMf::request does.
    // Atic Atac Remake plays its sound from a raster NMI the copper raises with
    // NR 0x02 = 4 on every line (4 DAC samples, then RETN): with the NMI left
    // pending until the slice ended, a frame's ~300 requests collapsed into ~2
    // and the game's sample-driven streaming crawled (the intro "loading" for
    // minutes over a garbled screen, hw 2026-10-16).
    NextInt::wake();
#if NEXT_TRACE
    // Only the hot key arms the one-shot UART trace: a guest raising the NMI
    // itself (NR 0x02 bit 2, a copper raster NMI ~300 times a frame) re-armed
    // 1500 lines per NMI and the trace build ran at 0-1 FPS.
    { extern uint16_t g_nmi_trace_left; if (hotkey) g_nmi_trace_left = 1500;
      next_trace_ev('M', DivMMC::next_nmi_btn, reg[0xBB]); }
#endif
    return 0;
}

bool NextReg::osTapeTrapArmed() {
    return (reg[0x0A] & 0x10) && (reg[0x83] & 0x01) && (reg[0xBB] & 0x20) &&
           (MemESP::next_rom_mask & 1) && MemESP::next_rom3 &&
           !(MemESP::l2_rd_mask & 1);   // zxnext.vhd 3138: not under a Layer 2 read mapping
}
static void applyBankC000();

// The EFFECTIVE speed is capped by Config::next_clk_cap (fixed at 3 = 28 MHz,
// i.e. no cap; the menu row was removed).
// reg[0x07] keeps the programmed value and the read-back reports the actual
// speed in bits 5:4, which is how real boards expose "asked for more than I
// can do". Same idea as TS-Conf's tsconf_clk_cap.
void NextReg::applySpeed() {
    uint8_t m = reg[0x07] & 0x03;
    if (m > Config::next_clk_cap) m = Config::next_clk_cap;
    uint8_t old = ESPectrum::multiplicator;
    if (old == m) return;
    // The switch happens MID-FRAME: everything measured in T-states (tstates,
    // statesInFrame, the NEXTVID line boundary) must move to the new clock
    // domain together, or the end-of-frame `tstates -= statesInFrame`
    // underflows and the INT window is never hit again (hw: NextZXOS set
    // 28MHz and froze on the next HALT with tstates = 0xFFFFFFFF).
    ESPectrum::multiplicator = m;
    if (m > old) CPU::tstates <<= (m - old);
    else         CPU::tstates >>= (old - m);
    ESPectrum::beeperClockRescale(old, m);   // the beeper's own T-state cursor
    Z80DMA::nxClockRescale(old, m);           // a paced DMA's next byte
    CPU::updateStatesInFrame();
    NEXTVID::onClockChange();
    contendRecalc();   // contention exists at 3.5 MHz only
    // The running slice's bound (CPU::stFrame) is in the OLD units: end the
    // slice so CPU::loop re-derives it from the rescaled frame.
    NextInt::wake();
}

void NextReg::contendRecalc() {
    const uint8_t t = s_machineTiming;   // 1 48K, 2 128K, 3 +3, 4 Pentagon
    const bool on = !(reg[0x08] & 0x40) && t != 4 && ESPectrum::multiplicator == 0;
    contendBanks = !on ? 0 : t == 1 ? 0x20 : t == 2 ? 0xAA : 0xF0;
    NEXTVID::contend = !on ? 0 : (t == 3 ? 1 : 3);
    static const uint8_t k48[8] = {6, 5, 4, 3, 2, 1, 0, 0};
    static const uint8_t kP3[8] = {1, 0, 7, 6, 5, 4, 3, 2};
    memcpy(NEXTVID::contWait, t == 3 ? kP3 : k48, 8);
    const int tpl = VIDEO::tStatesPerLine;
    if (tpl > 0) {   // early boot: NextReg::reset may run before VIDEO::Reset
        const int a = (int)VIDEO::tStatesScreen - (raster().hz60 ? 26 * tpl : 0);
        NEXTVID::contLine = (int16_t)(a / tpl);
        NEXTVID::contCol  = (int16_t)(a % tpl);
    } else {
        NEXTVID::contend = 0;
    }
    MemESP::nextContendRefresh();
    VIDEO::tsFastMemRecalc();
    Ports::ulaIoContendRecalc();
}

NX_OVL_CODE void NextReg::writeHot(uint8_t r, uint8_t v) {
    if (NEXTVID::nrHot(r, v)) return;
#if !NEXT_TRACE   // a trace build keeps every NR write in its event ring
    if ((uint8_t)(r - 0x50) < 8) {   // write()'s MMU case: latch + applyMMU, no tail work
        reg[r] = v;
        MemESP::applyMMU(r - 0x50, v);
        return;
    }
#endif
    write(r, v);
}

void NextReg::write(uint8_t r, uint8_t v) {
    const uint8_t old = reg[r];
    reg[r] = v;
#if NEXT_TRACE
    // Flood sources are counted, not recorded (palette / copper / sprite
    // streams run thousands of writes a second and would purge the ring).
    if (r == 0x40 || r == 0x41 || r == 0x44) s_nt_pal++;
    else if ((r >= 0x60 && r <= 0x63) || g_nx_copper_wr) s_nt_cop++;   // a copper raster NMI writes NR 02 every line
    else if ((r >= 0x34 && r <= 0x39) || (r >= 0x75 && r <= 0x79)) s_nt_spr++;
    else NT_EV('N', r, v);
    {
        extern void nxNrNote(uint8_t r, uint8_t v);
        if (r == 0x02 || r == 0x06 || r == 0x50 || r == 0x51 || r == 0x8E || r == 0xC0 ||
            r == 0xC2 || r == 0xC3 || r == 0x03 || r == 0x8C || r == 0x0A || r == 0xBB)
            nxNrNote(r, v);
    }
#endif
    switch (r) {
        case 0x03: { // Machine type / display timing / config mode
            bool bootromWasActive = MemESP::nextBootRomActive;
            // machine_timing: only with bit7 set, user lock clear, bit3 clear
            if ((v & 0x80) && !s_userDTLock && !(v & 0x08)) {
                uint8_t t = (v >> 4) & 7;
                s_machineTiming = (t <= 1) ? 1 : (t <= 4 ? t : 3);
            }
            s_userDTLock ^= (v >> 3) & 1;   // XOR-toggles on every bit3=1 write
            // machine_type: updates only in config mode and only to 1..4
            if (MemESP::nextConfigMode) {
                uint8_t t = v & 7;
                if (t >= 1 && t <= 4) s_machineType = t;
            }
            applyRomSelect();   // the machine type decides the ROM / ROM3 rule
            // config_mode: 111 re-enters, other non-zero exits, 000 no change
            switch (v & 7) {
                case 0: break;
                case 7: MemESP::nextConfigMode = true;  break;
                default: MemESP::nextConfigMode = false; break;
            }
            // The bootrom read overlay dies on the very first NR 0x03 write
            // of any value (zxnext.vhd:5122)
            if (bootromWasActive) MemESP::nextBootRomActive = false;
            MemESP::applyNextConfigWindow();
            reg[0x03] = reg03Compose();
            break;
        }
        case 0x04: // Config-mode RAMPAGE: 16K page at 0x0000-0x3FFF; bit 7
                   // only on Issue 5 (zxnext.vhd gen_romram_234 / gen_romram_5)
            MemESP::nextConfigPage = NextBoard::issue5() ? v : (uint8_t)(v & 0x7F);
            MemESP::applyNextConfigWindow();
            break;
        case 0x0A: // zxnext.vhd 5191-5198: MF type (7:6) and SD swap (5) only in
                   // config mode; automap, mouse button reverse and DPI always
            if (!MemESP::nextConfigMode) reg[0x0A] = (uint8_t)((old & 0xE0) | (v & 0x1F));
            break;
        case 0x02: // Reset request: bit1 = hard, bit0 = soft. NextZXOS uses a
                   // soft reset to restart into the freshly configured state
                   // at the end of each boot stage — latching only left the
                   // boot in an endless crash-restart loop (hw: white flash
                   // blinking). Deferred to the frame boundary.
            if (!(v & 0x10)) s_ioTrapCause = 0;   // bit 4 = 0 clears the I/O trap (zxnext.vhd 3879)
            if (v & 0x04) divNmiRequest(false);   // generate divMMC NMI
            NextMf::nr02(v & 0x08);               // bit 3: generate Multiface NMI (NextMf.h)
            if (v & 0x02) resetPending = 2;
            else if (v & 0x01) resetPending = 1;
            if (v & 0x03) {
                // On hardware the OUT asserts /RESET at once: not one more
                // instruction runs. We reset at the frame boundary, so park the
                // CPU the way HALT does (halted + end the slice) with interrupts
                // off. Letting it run on was a real bug: TBBLUE.FW's video test
                // writes NR 02 = 2 after the user's Y and then, in the very next
                // loop iteration, re-writes its "VideoTest" resume signature for
                // the following mode (0x618F -> 0xD5C1) — the reboot found it and
                // started the test again, for ever (hw 2026-09-25).
                Z80::setHalted(true);
                Z80::setIFF1(false);
                Z80::setIFF2(false);
                CPU::stFrame = 0;
            }
            break;
        case 0x09: // Peripheral 4: bit 3 = reset the divMMC MAPRAM bit
                   // (zxnext.vhd: nr_09_we and nr_wr_dat(3) -> port_e3_reg(6) <= 0)
            if ((v & 0x08) && DivMMC::mapram) {
                DivMMC::mapram = false;
                DivMMC::applyMapping();
            }
            break;
        case 0x05: // Peripheral 1: bit 2 = 50/60 Hz — the frame changes at the
                   // next frame boundary (NEXTVID::EndFrame -> ESPectrum::nextRasterApply).
                   // Bits 7:6/3 and 5:4/1 = joystick modes (NextPorts::joyMode): the
                   // pad's Kempston byte may move between 0x1F and 0x37, start both idle.
            Ports::port[0x1F] = 0;
            Ports::port[0x37] = 0;
            break;
        case 0x07: // CPU speed: 0=3.5, 1=7, 2=14, 3=28 MHz
            applySpeed();
            break;
        case 0x20: case 0x22: case 0x23: case 0x64:   // interrupts (NextInt.cpp)
        case 0xC0: case 0xC4: case 0xC5: case 0xC8: case 0xC9:
            NextInt::nrWrite(r, v);
            break;
        case 0x12: case 0x13: // Layer 2 active/shadow bank
            updateLayer2Window();
            break;
        case 0x69: { // Display Control 1: bit7 = Layer 2 enable (read by the
                     // renderer), bit6 = ULA shadow display — the same latch
                     // as port 7FFD bit 3 (hw: NextZXOS drew its boot screen
                     // into bank 7 via this and we kept showing blank bank 5)
            applyTimexMode(v & 0x3F);
            // bits 5:0 and port FF bits 5:0 are ONE latch too: IN (#FF) reads
            // them back (MrKWatkins NReg0x69: the two write tests with Timex
            // bits set read a stale portFF, hw 2026-10-15). Bit 6 of portFF is
            // the INT-disable latch (NR 0x22 bit 2), not NR 0x69's shadow bit.
            portFF = (uint8_t)((portFF & 0x40) | (v & 0x3F));
            // bit 7 and port 0x123B bit 1 are ONE latch (layer2_en)
            port123B = (uint8_t)((port123B & ~0x02) | ((v >> 6) & 0x02));
            uint8_t vl = (v >> 6) & 1;
            if (MemESP::videoLatch != vl) {
                NEXTVID::screenFlipMark();   // the row on the beam keeps the old page
                MemESP::videoLatch = vl;
                VIDEO::grmem = vl ? MemESP::ram[7].direct() : MemESP::ram[5].direct();
                VIDEO::gigascreenAutoFlip();   // Gigascreen Auto (NEXTVID::gigsFrame)
            }
            break;
        }
        case 0x18: case 0x19: case 0x1A: case 0x1B: // clip windows
            NEXTVID::clipWrite(r - 0x18, v);
            break;
        case 0x1C: // clip window index reset
            NEXTVID::clipIndexReset(v);
            break;
        case 0x34: // sprite number (own index unless lockstep, NR 0x09 bit 4)
            NEXTVID::spriteNrSelect(v);
            break;
        case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
            NEXTVID::spriteAttrDirect(r - 0x35, v);
            break;
        case 0x75: case 0x76: case 0x77: case 0x78: case 0x79:
            NEXTVID::spriteAttrDirect(r - 0x75, v, true);   // + sprite number
            break;
        case 0x40: NEXTVID::palIndex(v);   break;
        case 0x41: NEXTVID::palValue8(v);  break;
        case 0x43: NEXTVID::palControl(v); break;
        case 0x44: NEXTVID::palValue9(v);  break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
#if NEXT_TRACE
            if (Z80DMA::nxPending) Z80DMA::nxHeldNote('M', r, v);
#endif
            MemESP::applyMMU(r - 0x50, v);
            break;
        case 0x8C: { // Alt-ROM: bit7 enable, bit6 write-only, bit5 lock ROM1,
                     // bit4 lock ROM0; bits 3:0 staged (promoted on soft reset)
            MemESP::altrom_rd = (v & 0x80) && !(v & 0x40);
            MemESP::altrom_wr = (v & 0x80) && (v & 0x40);
            applyRomSelect();   // lock bits move ROM / ROM3 / the Alt-ROM half
            break;
        }
        case 0x85: // only bit 7 (reset type) and bits 3:0 exist; read-back 6:4 = 0
            reg[0x85] = v & 0x8F;
            /* fall through */
        case 0x82: case 0x83: case 0x84:
            portEnRecalc();
            if (!portOn(PE_MF)) NextMf::reset();   // multiface.vhd reset <= not enable_i
            break;
        case 0x60: NEXTVID::copperDataWrite(v);   break;
        case 0x61: NEXTVID::copperIndexLo(v);     break;
        case 0x62: NEXTVID::copperControl(v);     break;
        case 0x63: NEXTVID::copperData16Write(v); break;
        case 0x08: // bit 7 = unlock 0x7FFD paging (clears 0x7FFD bit 5, 3654)
            if (v & 0x80) {
                port7FFD &= ~0x20;
                MemESP::pagingLock = 0;
            }
            break;
        case 0x8E: { // Spectrum memory mapping (atomic 0x7FFD/0x1FFD write)
            // zxnext.vhd 3662-3670 / 3696-3703 / 3726-3734, never locked:
            // bit 3 = apply bank bits 7:4 (7FFD 2:0, DFFD bit 0 <- d7, DFFD
            // bit 3 cleared); 1FFD bit 2 <- d1, bit 1 <- d0, bit 0 <- d2
            // (special); with d2 = 0 also 0x7FFD bit 4 <- d0 (ROM select).
            if (v & 0x08) {
                port7FFD = (port7FFD & ~0x07) | ((v >> 4) & 0x07);
                portDFFD = (portDFFD & ~0x0F) | ((v >> 7) & 0x01);
                applyBankC000();
            }
            if (!(v & 0x04)) {
                port7FFD = (uint8_t)((port7FFD & ~0x10) | ((v & 0x01) << 4));
                MemESP::romLatch = v & 0x01;
            }
            write1FFD((uint8_t)(((v >> 2) & 0x01) | ((v & 0x03) << 1)));
            break;
        }
        case 0xF0: case 0xF8: case 0xF9: case 0xFA:   // XDEV / XADC (KS2/KS3)
            NextBoard::write(r, v);
            break;
        default:
            // Remaining registers are latched; video/sprite/palette consumers
            // read NextReg::reg[] directly as they are implemented.
            break;
    }
    if (r == 0x03 || r == 0x08) contendRecalc();   // timing / contention disable
    // Sound: PSG mode, TurboSound / DAC enables, stereo and mono (NextPorts.cpp)
    if (r == 0x06 || r == 0x08 || r == 0x09) NextPorts::audioRegWrite(r, old);
    // DAC nextreg mirrors (soundrive.vhd): 0x2C left, 0x2D mono, 0x2E right
    if (r >= 0x2C && r <= 0x2E) NextPorts::dacNr(r, v);
}

uint8_t NextReg::read(uint8_t r) {
    switch (r) {
        case 0x00: return 10;    // machine ID: ZX Spectrum Next
        case 0x01: return 0x32;  // core major.minor = 3.2
        case 0x03: return reg03Compose();
        case 0x02: return (uint8_t)((reg[0x02] & ~0x78) | NextMf::nr02Bit() | (s_ioTrapCause ? 0x10 : 0));
        case 0xD8: return reg[0xD8] & 0x01;
        case 0xDA: return s_ioTrapCause;
        case 0x0E: return 0x03;  // core sub-minor: report 3.02.03 — the
                                  // current NextZXOS distro gates boot on
                                  // >= 3.02.03 (hw: 3.02.00 failed the check
                                  // at ROM PC=0E13 → 1.4s restart loop);
                                  // same stable version zx_go settled on
        case 0x2C: case 0x2D: case 0x2E:
            return 0x00;   // Pi I2S sample bits (zxnext.vhd 6008-6015) — no Pi here
        case 0x68: return reg[0x68] & 0xFD;   // bit 1 reads 0 (zxnext.vhd NR 0x68 read)
        case 0x0B: return reg[0x0B] & 0xB1;
        case 0xB0: // ; " , . UP DOWN LEFT RIGHT (zxnext.vhd 6206-6208)
            return (uint8_t)((((extKeys >> 8) & 1) << 7) | (((extKeys >> 9) & 1) << 6) |
                             (((extKeys >> 10) & 1) << 5) | (((extKeys >> 11) & 1) << 4) |
                             (((extKeys >> 1) & 1) << 3) | (((extKeys >> 15) & 1) << 2) |
                             (((extKeys >> 14) & 1) << 1) | ((extKeys >> 13) & 1));
        case 0xB1: // DELETE EDIT BREAK INV TRUE GRAPH CAPSLOCK EXTEND (6210-6212)
            return (uint8_t)((((extKeys >> 12) & 1) << 7) | (((extKeys >> 2) & 0x3F) << 1) |
                             (extKeys & 1));
        case 0x09: return reg[0x09] & 0xF7;   // bit 3 (mapram reset strobe) reads 0
        case 0x6E: case 0x6F: return reg[r] & 0xBF;   // bit 6 reads 0
        case 0x89: return reg[0x89] & 0x8F;
        case 0x40: case 0x61: case 0x62: return (uint8_t)NEXTVID::nrRead(r);
        case 0x8E: // zxnext.vhd 6158: DFFD 0, 7FFD 2:0, 1, 1FFD 0, 1FFD 2, ROM bit
            return (uint8_t)(((portDFFD & 1) << 7) | ((port7FFD & 7) << 4) | 0x08 |
                             ((port1FFD & 1) << 2) | (((port1FFD >> 2) & 1) << 1) |
                             ((((port7FFD >> 4) & 1) & ~port1FFD & 1) |
                              (((port1FFD >> 1) & 1) & port1FFD & 1)));
        case 0x08: // bit 7 reads "not locked" (zxnext.vhd 5906)
            return (uint8_t)((reg[0x08] & 0x7F) | (MemESP::pagingLock ? 0 : 0x80));
        case 0x07: { // bits 5:4 = actual speed, 1:0 = programmed
            uint8_t actual = reg[0x07] & 3;
            if (actual > Config::next_clk_cap) actual = Config::next_clk_cap;
            return (actual << 4) | (reg[0x07] & 3);
        }
        case 0x18: case 0x19: case 0x1A: case 0x1B: {
            // Read returns the CURRENT coordinate (index not advanced) —
            // zxnext.vhd read mux 5947-5975 via zx_go's conformance notes
            uint8_t w = r - 0x18;
            return NEXTVID::clip[w][NEXTVID::clipIdx[w] & 3];
        }
        case 0x1C: // packed clip indices read-back
            return (uint8_t)((NEXTVID::clipIdx[3] & 3) << 6) |
                   (uint8_t)((NEXTVID::clipIdx[2] & 3) << 4) |
                   (uint8_t)((NEXTVID::clipIdx[1] & 3) << 2) |
                   (uint8_t)(NEXTVID::clipIdx[0] & 3);
        case 0x41: return NEXTVID::palRead8();
        case 0x44: return NEXTVID::palRead9();
        case 0x1E: rasterPollFF(0); return (activeLine() >> 8) & 0x01;
        case 0x1F: rasterPollFF(1); return activeLine() & 0xFF;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            return MemESP::mmu[r - 0x50];
        case 0x20: case 0x22: case 0x23:
        case 0xC0: case 0xC4: case 0xC5: case 0xC6: case 0xC8:
        case 0xC9: case 0xCA: case 0xCC: case 0xCE: {
            uint8_t o;
            return NextInt::nrRead(r, o) ? o : reg[r];
        }
        // Read masks for the registers kept raw in reg[] (zxnext.vhd read mux
        // 5880-6290): bits a register does not hold read 0.
        case 0x0A: return reg[0x0A] & 0xFB;
        case 0x0F: return NextBoard::boardId();   // Config::next_board (KS1/KS2/KS3)
        case 0xF0: case 0xF9: case 0xFA: return NextBoard::read(r);
        case 0x11: return reg[0x11] & 0x07;
        case 0x12: case 0x13: return reg[r] & 0x7F;
        case 0x2F: return reg[0x2F] & 0x03;
        case 0x34: // sprite_mirror_id: the live nextreg sprite index (auto-increments)
            return NEXTVID::spriteNrIndex();
        case 0x4C: return reg[0x4C] & 0x0F;
        case 0x6A: return reg[0x6A] & 0x3F;
        case 0x70: return reg[0x70] & 0x3F;
        case 0x71: return reg[0x71] & 0x01;
        case 0x81: return reg[0x81] & 0x78;   // speed bits forced 00, no bus ROMCS
        case 0x85: return reg[0x85] & 0x8F;
        case 0x8A: return reg[0x8A] & 0x3F;
        case 0x8F: return reg[0x8F] & 0x03;
        case 0x90: return reg[0x90] & 0xFC;
        case 0x93: return reg[0x93] & 0x0F;
        case 0xA0: return reg[0xA0] & 0x39;
        case 0xA2: return (uint8_t)((reg[0xA2] & 0xDD) | 0x02);
        case 0xA8: return reg[0xA8] & 0x01;
        case 0xF8: return NextBoard::read(0xF8);
        case 0x98: case 0x99: case 0x9A: case 0x9B:   // Pi GPIO inputs: no Pi
        case 0xA9:                                    // ESP GPIO inputs
        case 0xB2:                                    // MD extra buttons: none
            return 0x00;
        default: {
            // Registers missing from the read mux read 0 ("when others").
            static const uint8_t kReadable[32] = { 0xEF, 0xCF, 0xFF, 0xDF, 0xCD, 0xF1, 0x1F, 0x00, 0x1F, 0x1C, 0xFF, 0x00, 0x16, 0xDF, 0x03, 0x80, 0xFF, 0xD7, 0x0F, 0x0F, 0x05, 0x03, 0x07, 0x0F, 0x7D, 0x77, 0x00, 0x07, 0x00, 0x00, 0x01, 0x07 };
            return (kReadable[r >> 3] >> (r & 7)) & 1 ? reg[r] : 0x00;
        }
    }
}

// ===== Layer 2 access port (0x123B) =====

extern void ts_fastrd_recalc();   // Video.cpp: the Next inline read gate

void NextReg::writeLayer2Port(uint8_t v) {
    // zxnext.vhd ~5410: with bit 4 set the write ONLY loads the bank offset
    // (bits 2:0); with it clear it loads the control latches and leaves the
    // offset alone. Read-back (port_123b_dat) = segment & 00 & shadow & map_rd
    // & layer2_en & map_wr, the offset is not readable. layer2_en is one latch
    // with NR 0x69 bit 7.
    if (v & 0x10) {
        l2Offset = v & 0x07;
    } else {
        port123B = v & 0xCF;
        reg[0x69] = (uint8_t)((reg[0x69] & 0x7F) | ((v & 0x02) << 6));
    }
    updateLayer2Window();
}

// Timex/SCLD port 0xFF. bit6 = ULA frame INT disable (consumed by
// Z80Ops::isActiveINT alongside nextreg 0x22 bit 2); bits 5:0 mirror into
// nextreg 0x69, which is what NEXTVID reads for the screen-mode bits.
// Screen-mode bits 5:0 (port FF and NR 0x69 are one latch): hand the Timex
// mode to VIDEO's hi-res machinery, which switches the driver in vblank.
// The packed-pair driver switch is per FRAME (vblank), the mode is per LINE
// (the copper sets NR 0x69 per region — MrKWatkins NReg0x69 puts two hi-res
// rows in the middle of a classic screen). So a write only records that a
// hi-res mode was SEEN; timexHiresFramePoll (EndFrame, before the switch) asks
// for the pair driver when any line of the frame used hi-res, and NEXTVID
// renders each line by its own NR 0x69 bits (a classic line in a pair frame
// comes out as solid pairs). The last write of the frame used to decide, so a
// copper that switched hi-res on and off again never got the driver.
static void applyTimexMode(uint8_t v) {
    const uint8_t pi = VIDEO::timex_hires_ink;
    VIDEO::timex_mode = v & 0x07;
    VIDEO::timex_hires_ink = (v >> 3) & 0x07;
    // zxula.vhd: mode bit 2 is hi-res (4-7), not just 6
    if (VIDEO::timex_mode & 4) s_hiresSeen = true;
    // core1 takes the mode from each line's snapshot (r69); the only thing it
    // reads live is the hi-res LUT, so wait for it only before a rebuild. An
    // unconditional drain here cost a whole queue per NR 0x69 write — Atic Atac
    // Remake writes it every frame (hw 2026-10-19: 10 ms/frame, 38 FPS).
    if (VIDEO::timex_hires_ink != pi && VIDEO::timex_hires_live) {
        NEXTVID::c1Drain();
        VIDEO::timexHiresColour();
    }
}
void NextReg::timexHiresFramePoll() {
    VIDEO::timexHiresRequest(s_hiresSeen || (VIDEO::timex_mode & 4) != 0);
    s_hiresSeen = false;
}

void NextReg::writePortFF(uint8_t v) {
    portFF = v;
    portFFIntDisable = (v & 0x40) != 0;
    reg[0x69] = (uint8_t)((reg[0x69] & 0xC0) | (v & 0x3F));
    applyTimexMode(v);
    NT_EV('F', 0xFF, v);
}

void NextReg::updateLayer2Window() {
    // zxnext.vhd ~3030-3110 (layer2_active_bank_offset / sram_layer2_map_en):
    //   segment 0-2 -> 0x0000-0x3FFF shows bank (NR 0x12, or NR 0x13 with the
    //                  shadow bit) + segment + offset;
    //   segment 3   -> 0x0000-0xBFFF, each 16K by A15:14 (the 48K mode).
    // Writes go there with map_wr (bit 0), reads with map_rd (bit 2); the
    // divMMC/MF window still wins over both (handled before the overlay in
    // MemESP). Neither bit set = the MMU sees everything, as before.
    // The TBBLUE config window owns slots 0/1 while it is up; applyNextConfig-
    // Window() calls back here the moment it closes.
    const bool cfg = MemESP::nextBootRomActive || MemESP::nextConfigMode;
    const uint8_t oldRd = MemESP::l2_rd_mask;
    uint8_t wr = 0, rd = 0, pass = 0;
    const bool wrEn = port123B & 0x01, rdEn = port123B & 0x04;
    if ((wrEn || rdEn) && MemESP::nextRamReady) {
        const uint8_t seg = port123B >> 6;
        const uint8_t bank = (port123B & 0x08) ? reg[0x13] : reg[0x12];
        const uint8_t nSlots = (seg == 3) ? 6 : 2;
        for (uint8_t s = 0; s < nSlots; s++) {
            const uint8_t pre = (seg == 3) ? (uint8_t)(s >> 1) : seg;
            const uint16_t page = (uint16_t)((uint8_t)(bank + pre + l2Offset) & 0x7F) * 2 + (s & 1);
            uint8_t* lp = MemESP::nextPgPtr(page);
            if (rdEn) {
                MemESP::l2RdPtr[s] = lp;           // null = open bus (applyMMU)
                rd |= (uint8_t)(1u << s);
            }
            if (wrEn) {
                if (lp) { MemESP::wrOverlay[s] = lp; wr |= (uint8_t)(1u << s); }
            } else {
                // read mapped, write not: ramCurrent now shows Layer 2, so the
                // write needs the MMU's own RAM page — applyMMU fills it in
                // (and keeps it current across MMU writes; ROM slots drop it)
                pass |= (uint8_t)(1u << s);
            }
        }
    }
    if (cfg) { wr &= (uint8_t)~3; rd &= (uint8_t)~3; pass &= (uint8_t)~3; }
    if (NextMf::memOn()) wr &= (uint8_t)~3;      // MF wins (zxnext.vhd ~3030)
    MemESP::wr_overlay_mask = (uint8_t)((cfg ? (MemESP::wr_overlay_mask & 3) : 0) | wr);
    MemESP::l2_rd_mask = rd;
    MemESP::l2_wr_mmu_mask = pass;
    // Re-plug every slot whose read mapping moved (or that shows Layer 2 and
    // may now show another bank of it).
    const uint8_t touch = (uint8_t)(oldRd | rd);
    for (uint8_t s = 0; s < 6; s++)
        if ((touch >> s) & 1) MemESP::applyMMU(s, MemESP::mmu[s]);
    // The Next fast path writes 0x4000+ straight through ramCurrent; while
    // Layer 2 covers those slots every write must see the overlay.
    const bool high = ((wr | rd) & 0x3C) != 0;
    if (high != l2MapHigh) { l2MapHigh = high; VIDEO::tsFastMemRecalc(); }
    if (touch) ts_fastrd_recalc();
}

// ===== I/O traps (zxnext.vhd 2601-2602, 3835-3895) =====
// With NR 0xD8 bit 0 the +3 FDC ports 0x2FFD (read) and 0x3FFD (read/write)
// raise a Multiface NMI instead of reaching a controller: NextZXOS emulates
// the +3 disk that way. The cause is latched only while no NMI is in progress
// (nmi_accept_cause); NR 0xD9 keeps the byte a 0x3FFD write carried.
bool NextReg::ioTrap(uint16_t address, bool write, uint8_t data) {
    if (!(reg[0xD8] & 0x01) || (address & 0xC003) != 0x0001) return false;
    const uint8_t a = (address >> 12) & 3;           // A13:12
    if (a < 2 || (a == 2 && write)) return false;    // 0x2FFD is trapped on read only
    if (!NextMf::active()) {
        s_ioTrapCause = (uint8_t)(a == 2 ? 1 : write ? 3 : 2);
        if (write) reg[0xD9] = data;
    }
    NextMf::request();
    return true;
}

// ===== Legacy Spectrum paging, Next-style (implemented on top of the MMU) =====

static void applyRomSelect() {
    // zxnext.vhd "rom address computation": which 16K ROM, whether it is
    // "ROM3" (the 48K BASIC — gates the divMMC tape/RST entry points) and which
    // Alt-ROM half (alt_128_n: 1 = the 48K half) all follow the MACHINE TYPE
    // (nextreg 0x03, set in config mode), with the 0x8C lock bits overriding:
    //   48K       : ROM 0 always, ROM3 = 1
    //   +3 / Next : ROM = 1FFD.2:7FFD.4, ROM3 = both set
    //   128K/Pent : ROM = 0:7FFD.4,      ROM3 = 7FFD.4
    // NextZXOS's TAP Loader (48K/128K/Pentagon modes) runs 48 BASIC as ROM1 of
    // a 128K-type machine; testing "ROM index == 3" never armed its #056A tape
    // trap there and every .tap loaded in real time (hw 2026-10-07).
    const uint8_t a   = NextReg::reg[0x8C];
    const bool    l1  = a & 0x20, l0 = a & 0x10;       // lock ROM1 / lock ROM0
    const uint8_t r7  = MemESP::romLatch & 0x01;         // 7FFD bit 4
    const uint8_t r1  = (NextReg::port1FFD >> 2) & 0x01; // 1FFD bit 2
    const uint8_t mt  = NextReg::machineType();
    uint8_t rom; bool rom3, alt128n;
    if (mt <= 1) {                                       // 48K
        rom = 0; rom3 = true; alt128n = !(!l1 && l0);
    } else if (mt == 2 || mt == 4) {                     // 128K / Pentagon
        if (l1 || l0) { rom = l1 ? 1 : 0; rom3 = l1; alt128n = l1; }
        else          { rom = r7;         rom3 = r7; alt128n = r7; }
    } else {                                             // +3 / Next
        if (l1 || l0) { rom = (l1 ? 2 : 0) | (l0 ? 1 : 0); rom3 = l1 && l0; alt128n = l1; }
        else          { rom = (r1 << 1) | r7;              rom3 = r1 && r7; alt128n = r7; }
    }
    // Without the NextZXOS set only the two-ROM 128K fallback exists.
    if (!MemESP::nextRomBase) { rom &= 1; rom3 = rom == 1; }
    MemESP::romInUse = rom;
    MemESP::altrom_sel = alt128n ? 1 : 0;
    // divMMC automap "rom3" entry points: the Alt-ROM stands in for ROM3 when
    // it is the 48K half (sram_divmmc_automap_rom3_en).
    MemESP::next_rom3 = MemESP::altrom_rd ? alt128n : rom3;
    if (MemESP::mmu[0] >= 0xE0) MemESP::applyMMU(0, MemESP::mmu[0]);
    if (MemESP::mmu[1] >= 0xE0) MemESP::applyMMU(1, MemESP::mmu[1]);
}

static void applyBankC000() {
    uint8_t bank = (NextReg::port7FFD & 0x07) | ((NextReg::portDFFD & 0x0F) << 3);
    if (bank < MemESP::NEXT_PAGES / 2) {
        MemESP::applyMMU(6, bank * 2);
        MemESP::applyMMU(7, bank * 2 + 1);
    }
}

void NextReg::write7FFD(uint8_t v) {
    // 0x7FFD bit 5 locks 0x7FFD, 0x1FFD and 0xDFFD (zxnext.vhd 3651, 3691,
    // 3718); only a NEXTREG 0x08 write with bit 7 set unlocks it (3654). It
    // used to test NR 0x08 bit 6 — that is the contention disable.
    if (MemESP::pagingLock) return;
    NT_EV('7', 0x7F, v);
    port7FFD = v;
    applyBankC000();
    uint8_t vl = (v >> 3) & 1;
    if (MemESP::videoLatch != vl) {
        NEXTVID::screenFlipMark();   // the row on the beam keeps the old page
        MemESP::videoLatch = vl;
        VIDEO::grmem = vl ? MemESP::ram[7].direct() : MemESP::ram[5].direct();
        VIDEO::gigascreenAutoFlip();   // Gigascreen Auto (NEXTVID::gigsFrame)
    }
    reg[0x69] = (uint8_t)((reg[0x69] & ~0x40) | (vl << 6));
    MemESP::romLatch = (v >> 4) & 1;
    if (!(port1FFD & 0x01)) applyRomSelect();
    if (v & 0x20) MemESP::pagingLock = 1;
}

// zxnext.vhd 4636-4644 + ports.txt: "a write to 0x7ffd/0xdffd/0x1ffd/0xeff7
// sets mmu0=0xff and mmu1=0xff" (RAM 0/1 instead while #EFF7 bit 3 is set) —
// the paging ports always reveal the ROM in the bottom 16K. A locked write is
// dropped and changes nothing (the 7FFD/1FFD/DFFD terms of
// port_memory_change_dly carry "and not port_7ffd_locked"). Aliens
// Neoplasma II relies on it: its file loader writes #7FFD and calls RST 8
// with its own data page still in MMU0 (hw 2026-10-16: hang at start).
static void lowSlotsFromPorts() {
    const bool ram0 = NextPorts::eff7() & 0x08;
    if (NextReg::port1FFD & 0x01) {
        // +3 all-RAM overlay owns the slots: latch only, write1FFD re-applies
        // mmu[] when the overlay goes away.
        MemESP::mmu[0] = ram0 ? 0x00 : 0xFF;
        MemESP::mmu[1] = ram0 ? 0x01 : 0xFF;
        return;
    }
    NextReg::write(0x50, ram0 ? 0x00 : 0xFF);
    NextReg::write(0x51, ram0 ? 0x01 : 0xFF);
}

void NextReg::write7FFDPort(uint8_t v) {
    if (MemESP::pagingLock) return;
    write7FFD(v);
    lowSlotsFromPorts();
}

void NextReg::writeDFFDPort(uint8_t v) {
    if (MemESP::pagingLock) return;
    writeDFFD(v);
    lowSlotsFromPorts();
}

void NextReg::write1FFDPort(uint8_t v) {
    if (MemESP::pagingLock) return;
    write1FFD(v);
    lowSlotsFromPorts();
}

void NextReg::writeDFFD(uint8_t v) {
    NT_EV('D', 0xDF, v);
    portDFFD = v & 0x0F;
    applyBankC000();
}

void NextReg::write1FFD(uint8_t v) {
    NT_EV('1', 0x1F, v);
    uint8_t prev = port1FFD;
    port1FFD = v;
    if (v & 0x01) {
        // +3 special all-RAM configurations: an overlay on top of the MMU —
        // the MMU registers themselves are NOT modified (hw: routing this
        // through applyMMU corrupted mmu[] and the exit path then force-reset
        // slots 2-5 to the 128K default, yanking the OS's own mapping out
        // from under it on every NEXTREG 0x8E ROM-swap trampoline — restart
        // loop right after the loader's first cross-ROM call).
        static const uint8_t maps[4][4] = {
            { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 4, 5, 6, 3 }, { 4, 7, 6, 3 }
        };
        const uint8_t* m = maps[(v >> 1) & 3];
        for (int s = 0; s < 4; s++) {
            uint8_t page = (uint8_t)(m[s] * 2);
            uint8_t* p0 = MemESP::nextPgPtr(page);
            uint8_t* p1 = MemESP::nextPgPtr((unsigned)page + 1);
            if (p0) { MemESP::ramCurrent[s * 2] = p0; MemESP::ramContended[s * 2] = false; }
            if (p1) { MemESP::ramCurrent[s * 2 + 1] = p1; MemESP::ramContended[s * 2 + 1] = false; }
        }
        MemESP::next_rom_mask = 0;   // all-RAM: page 0 is writable RAM
    } else {
        if (prev & 0x01) {
            // Leaving all-RAM: restore EXACTLY what the MMU registers say
            for (int s = 0; s < 8; s++)
                MemESP::applyMMU(s, MemESP::mmu[s]);
        }
        // Normal mode: 1FFD only contributes the ROM select high bit
        applyRomSelect();
    }
}




// ===== .pss snapshot (PSNX) =====
// Flash code, run from the menu / a hot key / the boot resume only. The record is
// the state NextReg::reset() re-derives everything else from, plus the MemESP
// latches it does not own but rebuilds (mmu[], the config window, Alt-ROM).
#include "app/SnapPack.h"
extern bool g_next_automap_armed;   // DivMMC.cpp
static constexpr uint8_t NX_SNAP_VER = 1;

uint32_t NextReg::snapSave(uint8_t* out) {
    SnapOut o(out);
    o.u8(NX_SNAP_VER);
    o.raw(reg, 256);
    o.u8(selected);
    o.u8(port7FFD); o.u8(portDFFD); o.u8(port1FFD); o.u8(port123B); o.u8(l2Offset);
    o.u8(s_ioTrapCause); o.u8(portFF); o.u8(portFFIntDisable ? 1 : 0);
    o.u8(s_machineType); o.u8(s_machineTiming); o.u8(s_userDTLock);
    o.u8(s_hiresSeen ? 1 : 0); o.u8(resetPending);
    o.raw(MemESP::mmu, 8);
    o.u8(MemESP::romLatch); o.u8(MemESP::videoLatch); o.u8(MemESP::pagingLock);
    o.u8((MemESP::altrom_rd ? 1 : 0) | (MemESP::altrom_wr ? 2 : 0));
    o.u8((MemESP::nextBootRomActive ? 1 : 0) | (MemESP::nextConfigMode ? 2 : 0));
    o.u8(MemESP::nextConfigPage);
    // Which images the ROM region holds (TBBLUE.FW streams a personality's ROMs
    // there; the region itself travels as pages 224.. of the snapshot).
    o.u8((MemESP::nextRomBase ? 1 : 0) | (MemESP::nextDivRomBase ? 2 : 0) | (MemESP::mfRomBase ? 4 : 0));
    o.u8(ESPectrum::multiplicator);
    o.u8((DivMMC::next_nmi_btn ? 1 : 0) | (DivMMC::next_nmi_hold ? 2 : 0) | (g_next_automap_armed ? 4 : 0));
    return o.n;
}

bool NextReg::snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn s(in, n);
    if (s.u8() < 1) return false;
    uint8_t r[256];
    s.raw(r, 256);
    const uint8_t sel = s.u8();
    const uint8_t p7 = s.u8(), pD = s.u8(), p1 = s.u8(), p12 = s.u8(), l2o = s.u8();
    const uint8_t trap = s.u8(), pff = s.u8(), pffDis = s.u8();
    const uint8_t mt = s.u8(), tm = s.u8(), dtl = s.u8(), hires = s.u8(), rp = s.u8();
    uint8_t mmu[8];
    s.raw(mmu, 8);
    const uint8_t romL = s.u8(), vidL = s.u8(), lock = s.u8();
    const uint8_t alt = s.u8(), cfg = s.u8(), cfgPage = s.u8(), roms = s.u8();
    const uint8_t mult = s.u8(), div = s.u8();
    if (!s.ok || !MemESP::nextRamReady) return false;

    memcpy(reg, r, 256);
    selected = sel;
    port7FFD = p7; portDFFD = pD; port1FFD = p1; port123B = p12; l2Offset = l2o;
    s_ioTrapCause = trap; portFF = pff; portFFIntDisable = pffDis != 0;
    s_machineType = mt; s_machineTiming = tm; s_userDTLock = dtl;
    s_hiresSeen = hires != 0; resetPending = rp;
    MemESP::romLatch = romL & 1; MemESP::videoLatch = vidL & 1; MemESP::pagingLock = lock;
    MemESP::altrom_rd = alt & 1; MemESP::altrom_wr = (alt & 2) != 0;
    // The bootrom read overlay exists only where the boot chain is armed.
    MemESP::nextBootRomActive = (cfg & 1) && MemESP::nextBootRom;
    MemESP::nextConfigMode = (cfg & 2) != 0;
    MemESP::nextConfigPage = cfgPage;
    uint8_t* rr = MemESP::nextRomRegion();
    MemESP::nextRomBase    = (roms & 1) ? rr : nullptr;
    MemESP::nextDivRomBase = (roms & 2) ? rr + 0x10000 : nullptr;
    MemESP::mfRomBase      = (roms & 4) ? MemESP::altRomBase + 0x8000 : nullptr;
    DivMMC::next_nmi_btn  = div & 1;
    DivMMC::next_nmi_hold = (div & 2) != 0;
    g_next_automap_armed  = (div & 4) != 0;
    portEnRecalc();

    // The clock first: the raster below derives statesInFrame, the audio set and
    // the contention from it (set directly, as the TS-Conf snapshot does — no
    // tstates rescale: Pss restores CPU::tstates afterwards).
    ESPectrum::multiplicator = (uint8_t)((mult & 3) <= Config::next_clk_cap ? (mult & 3) : Config::next_clk_cap);

    // Memory map: ROM select (machine type, 7FFD/1FFD, the 0x8C locks), every
    // MMU slot, the +3 all-RAM overlay, the TBBLUE window, Layer 2.
    MemESP::wr_overlay_mask = 0;
    MemESP::l2_rd_mask = 0;
    applyRomSelect();
    for (uint8_t sl = 0; sl < 8; sl++) MemESP::applyMMU(sl, mmu[sl]);
    if (port1FFD & 0x01) write1FFD(port1FFD);
    MemESP::applyNextConfigWindow();
    updateLayer2Window();
    VIDEO::grmem = MemESP::videoLatch ? MemESP::ram[7].direct() : MemESP::ram[5].direct();
    applyTimexMode(reg[0x69] & 0x3F);

    // The frame: timing (NR 0x03), 50/60 Hz (NR 0x05) and the clock.
    ESPectrum::nextRasterApply();
    applyRasterPaper();
    contendRecalc();
    return true;
}
