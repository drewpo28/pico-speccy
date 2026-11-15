// ZX Spectrum Next interrupt controller + CTC — see NextInt.h.
#include "NextInt.h"
#include "NextReg.h"
#include "NextMf.h"
#include "speccy/devices/storage/DivMMC.h"
#include "app/ESPectrum.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"
#include "speccy/video/Video.h"
#include "speccy/core/MemESP.h"
#include "speccy/devices/Z80DMA.h"
#include <cstdint>
#include "pico.h"
#if (PERF_TRACE && PERF_HIST) || NEXT_LINE_DIAG
#include <cstdio>
#include "app/Debug.h"
#endif

// The per-interrupt path (a CTC sample timer raises ~600 interrupts a frame:
// NXModPlayer, hw 2026-10-10 — this code from flash was ~5 ms of core0) lives in
// the Next code overlay window (.nxovl, SRAM on a Next boot only).
#if NEXT_CODE_OVERLAY
#define NXI_HOT __not_in_flash("nxint")
#else
#define NXI_HOT
#endif

#if NEXT_TRACE
// NEXT cpu: trace (NextReg.cpp): line pulses raised / already over when seen /
// by how many 3.5 MHz T, INT acknowledges, the line event's frame position.
volatile uint32_t g_li_raise = 0, g_li_late = 0, g_li_late_by = 0, g_int_ack = 0;
#endif
namespace NextInt {
uint8_t ex = 0;
bool retnArmed = false;
}

namespace {

enum { D_LINE = 0, D_CTC0 = 3, D_ULA = 11, D_NUM = 14 };
enum : uint8_t { S_0, S_REQ, S_ACK, S_ISR };              // im2_device.vhd
enum : uint8_t { C_RESET, C_RESET_TC, C_TRIGGER, C_RUN, C_RUN_TC };   // ctc_chan.vhd

constexpr int64_t kNever = INT64_MAX / 4;

uint8_t  s_st[D_NUM];      // im2_device state per source
uint16_t s_active;         // bit d = s_st[d] != S_0 (presented()'s early exit)
uint16_t s_req;            // im2_int_req (hardware IM2 pending, cleared at RETI)
uint16_t s_status;         // int_status (NR 0x20 / 0xC8-0xCA), cleared by writes
uint8_t  s_c0;             // NR 0xC0: 7:5 vector, 3 stackless NMI, 0 hw IM2
uint8_t  s_c4hi;           // NR 0xC4 bit 7 (expansion bus INT enable, stored)
bool     s_lineEn;
uint16_t s_lineNum;        // NR 0x22 bit 0 : NR 0x23
int64_t  s_linePos28 = -1; // frame-relative 28 MHz position of the line event, -1 none

int64_t  s_base28;         // absolute 28 MHz clock at frame-relative T 0
int64_t  s_last28;         // absolute time of the last sync
int64_t  s_pulse = -kNever;   // start of the newest pulse (pulse mode)
bool     s_unqPulse;       // an NR 0x20 pulse keeps ex up until it ends

struct Ctc {
    uint8_t  st, cw;       // state, control word bits 7..3
    uint16_t tc;           // time constant 1..256
    uint16_t cnt;          // down counter 1..256 (timer: as of ref)
    int64_t  run0;         // entry into RUN = prescaler phase origin
    int64_t  ref;          // timer: time cnt refers to
};
Ctc s_ctc[4];

struct Stream { int64_t first, period; int64_t n; };   // n ZC/TO pulses, first, first+period, ...

__attribute__((always_inline)) inline uint32_t f28() { return 8u >> ESPectrum::multiplicator; }
__attribute__((always_inline)) inline int64_t now28() { return s_base28 + (int64_t)CPU::tstates * f28(); }
inline bool im2Hw() { return s_c0 & 1; }
inline bool cpuIm2() { return Z80::getIM() == Z80::IM2; }

// zxnext.vhd 2030: pulse_count_end = count(5) and (48K or +3 or count(2)) —
// 32 CPU cycles in 48K/+3 timing, 36 in 128K/Pentagon.
__attribute__((always_inline)) inline int64_t pulseLen28() {
    const uint8_t t = NextReg::machineTiming();   // 1 48K, 2 128K, 3 +3, 4 Pentagon
    return (int64_t)((t <= 1 || t == 3) ? 32 : 36) * f28();   // t 0 = unset = 48K, as rasterKey()
}

inline int64_t fdiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The classic frame-INT window, exactly as Z80Ops::isActiveINT tests it.
NXI_HOT bool classicUla() {
    if (NextReg::portFFIntDisable) return false;
    int32_t tmp = (int32_t)CPU::tstates + CPU::latetiming;
    if (tmp >= (int32_t)CPU::statesInFrame) tmp -= CPU::statesInFrame;
    return tmp >= CPU::IntStart && tmp < CPU::IntEnd;
}

NXI_HOT void exRecalc() {
    bool ctcInt = false;
    for (auto& c : s_ctc) ctcInt |= (c.cw & 0x80) != 0;
    const uint8_t was = NextInt::ex;
    NextInt::ex = (s_lineEn || im2Hw() || ctcInt || s_unqPulse) ? 1 : 0;
    if (NextInt::ex && !was) NextInt::wake();
}

#if NEXT_LINE_DIAG
// [LDIAG] (NEXT_LINE_DIAG builds): a line pulse that no INT acknowledge
// followed. Recorded when the loop first SAW the event (sync), i.e. with the
// CPU state of that moment: IFF1 0 there = a handler (or DI code) was still
// running, lost on the board too; IFF1 1 + "late" = the loop saw the event
// after the pulse had ended (a slice / batch / DMA ran past it). One summary
// line per ~5 s. Flash, called from the window only under this option.
struct LineDiag { int64_t t, seen; uint16_t line, pc, sp, ret; uint8_t iff, taken, valid, dma; };
LineDiag s_ld;
uint32_t s_ldRaise, s_ldAck, s_ldLost0, s_ldLost1, s_ldAckMax;
__attribute__((noinline)) void ldReport() {
    const LineDiag& d = s_ld;
    const int64_t f = d.t - s_base28;
    Debug::log("[LDIAG] line %u pulse not taken: seen +%luT iff=%u pc=%04X sp=%04X (%04X) dma=%u frT=%lu now pc=%04X iff=%u\n",
               (unsigned)d.line, (unsigned long)((d.seen - d.t) / 8), (unsigned)d.iff, (unsigned)d.pc,
               (unsigned)d.sp, (unsigned)d.ret, (unsigned)d.dma, (unsigned long)(f >= 0 ? f / 8 : 0),
               (unsigned)Z80::getRegPC(), (unsigned)Z80::isIFF1());
}
__attribute__((noinline)) void ldRaise(int64_t t, int64_t now) {
    if (s_ld.valid && !s_ld.taken) {
        if (s_ld.iff) s_ldLost1++; else s_ldLost0++;
        ldReport();
    }
    const uint16_t sp = Z80::getRegSP();
    s_ld.t = t; s_ld.seen = now; s_ld.line = s_lineNum;
    s_ld.pc = Z80::getRegPC(); s_ld.sp = sp;
    s_ld.ret = (uint16_t)(MemESP::dbgPeek(sp) | (MemESP::dbgPeek((uint16_t)(sp + 1)) << 8));
    s_ld.iff = Z80::isIFF1(); s_ld.taken = 0; s_ld.valid = 1;
    s_ld.dma = (uint8_t)((Z80DMA::nxPending ? 1 : 0) | (Z80DMA::nxStall ? 2 : 0));
    if (++s_ldRaise >= 1500) {
        Debug::log("[LDIAG] %lu pulses, %lu acks, lost iff0 %lu iff1 %lu, ack delay max %luT\n",
                   (unsigned long)s_ldRaise, (unsigned long)s_ldAck, (unsigned long)s_ldLost0,
                   (unsigned long)s_ldLost1, (unsigned long)s_ldAckMax);
        s_ldRaise = s_ldAck = s_ldLost0 = s_ldLost1 = s_ldAckMax = 0;
    }
}
__attribute__((noinline)) void ldAck(int64_t now) {
    s_ldAck++;
    if (!s_ld.valid || s_ld.taken) return;
    s_ld.taken = 1;
    const uint32_t dl = (uint32_t)((now - s_ld.t) / 8);
    if (dl > s_ldAckMax) s_ldAckMax = dl;
}
#endif

// A source fired at absolute time t. `pulse` false = the ULA frame event, whose
// pulse is the classic window.
NXI_HOT void raise(int d, int64_t t, bool pulse) {
    s_status |= (uint16_t)(1u << d);
    if (im2Hw()) {
        s_req |= (uint16_t)(1u << d);
        if (s_st[d] == S_0) { s_st[d] = S_REQ; s_active |= (uint16_t)(1u << d); }
    } else if (pulse && t > s_pulse) {
        s_pulse = t;
    }
}

// --- CTC (ctc_chan.vhd). Prescaler: p_count counts 28 MHz cycles from RUN entry,
// a tick every 16 (or 256) cycles at p_count = 15 (255); the counter reloads from
// the time constant at ZC/TO. Counter mode counts the previous channel's ZC/TO
// (channel 0 counts channel 3's). ---
inline bool running(const Ctc& c) { return c.st == C_RUN || c.st == C_RUN_TC; }
inline bool timer(const Ctc& c) { return !(c.cw & 0x40); }
inline int64_t presc(const Ctc& c) { return (c.cw & 0x20) ? 256 : 16; }
// The prescaler is 16 or 256: a floor division by it is an arithmetic shift.
// fdiv() here was a libgcc __aeabi_ldivmod in flash, twice per channel per
// nextEventT — with a CTC sample timer that is every slice (NXModPlayer:
// ~4 ms of core0 a frame, hw 2026-10-10).
inline int presShift(const Ctc& c) { return (c.cw & 0x20) ? 8 : 4; }
inline int64_t ticksTo(const Ctc& c, int64_t t) { return (t - c.run0 + 1) >> presShift(c); }
// n / d and n % d for 0 <= n, 1 <= d <= 256 — a 32-bit hardware divide when n fits.
inline int64_t udiv64s(int64_t n, uint32_t d) {
    return ((uint64_t)n >> 32) ? n / d : (int64_t)((uint32_t)n / d);
}
inline uint32_t umod64s(int64_t n, uint32_t d) {
    return ((uint64_t)n >> 32) ? (uint32_t)(n % d) : (uint32_t)n % d;
}

NXI_HOT void advanceTimer(Ctc& c, int64_t t, Stream& s) {
    s.n = 0;
    if (t <= c.ref) return;
    const int64_t P = presc(c);
    const int64_t k0 = ticksTo(c, c.ref);
    const int64_t n = ticksTo(c, t) - k0;
    if (n < c.cnt) { c.cnt = (uint16_t)(c.cnt - n); c.ref = t; return; }
    s.first = c.run0 + (k0 + c.cnt) * P - 1;
    const int64_t rem = n - c.cnt;
    s.n = 1 + udiv64s(rem, c.tc);
    s.period = (int64_t)c.tc * P;
    c.cnt = (uint16_t)(c.tc - umod64s(rem, c.tc));
    c.ref = t;
}

// One channel's counter to t, nothing emitted: for a caller that knows no ZC is
// due (ctcWrite's fast path, right after sync()).
inline void advanceQuiet(Ctc& c, int64_t t) { Stream s; advanceTimer(c, t, s); }

NXI_HOT void counterFrom(Ctc& c, const Stream& src, Stream& s) {
    s.n = 0;
    if (src.n <= 0) return;
    if (src.n < c.cnt) { c.cnt = (uint16_t)(c.cnt - src.n); return; }
    s.first = src.first + (int64_t)(c.cnt - 1) * src.period;
    const int64_t rem = src.n - c.cnt;
    s.n = 1 + udiv64s(rem, c.tc);
    s.period = (int64_t)c.tc * src.period;
    c.cnt = (uint16_t)(c.tc - umod64s(rem, c.tc));
}

NXI_HOT void emit(int ch, const Stream& s) {
    if (s.n <= 0) return;
    const int d = D_CTC0 + ch;
    s_status |= (uint16_t)(1u << d);
    if (!(s_ctc[ch].cw & 0x80)) return;
    raise(d, s.first + (s.n - 1) * s.period, true);
}

NXI_HOT void propagate(int i, Stream s, bool* done, int64_t t) {
    int j = (i + 1) & 3;
    for (int k = 0; k < 3 && s.n > 0; k++, j = (j + 1) & 3) {
        if (done[j]) break;
        Ctc& c = s_ctc[j];
        Stream o{0, 0, 0};
        if (running(c) && !timer(c)) {
            counterFrom(c, s, o);
        } else if (c.st == C_TRIGGER && timer(c) && (c.cw & 0x08)) {
            c.st = C_RUN; c.run0 = c.ref = s.first; c.cnt = c.tc;
            advanceTimer(c, t, o);
        } else break;
        done[j] = true;
        emit(j, o);
        s = o;
    }
}

NXI_HOT void ctcAdvance(int64_t t) {
    bool done[4] = {false, false, false, false};
    for (int i = 0; i < 4; i++) {
        Ctc& c = s_ctc[i];
        if (done[i] || !running(c) || !timer(c)) continue;
        Stream s;
        advanceTimer(c, t, s);
        done[i] = true;
        emit(i, s);
        propagate(i, s, done, t);
    }
}

// Earliest ZC/TO of any channel after the last advance; sync() skips the CTC
// until then. -kNever = recompute (any change to the CTC state sets it).
int64_t s_ctcNext = -kNever;
// The soonest ZC/TO among channels with their interrupt ON (control word bit 7),
// computed by sync() in the same pass as s_ctcNext and valid exactly as long as
// it: nextEventT() used to re-derive it per channel on every slice, and a CTC
// sample timer (NXModPlayer: ~630 interrupts a frame) makes that every slice.
int64_t s_ctcNextInt = kNever;
// Set when every active CTC channel (state >= C_TRIGGER) is a RUNNING TIMER:
// nothing can then propagate (no counter channel, no channel waiting for a
// trigger), so each channel's ZC train is its own arithmetic and s_ctcZc[ch]
// (kNever for an idle channel) is its next ZC. Derived by the full ctcRefresh
// below and valid as long as s_ctcNext is (any CTC state change sets
// s_ctcNext = -kNever, which forces the full path again). NXModPlayer runs
// three such timers (31250 / 3125 / 437.5 Hz, ~706 interrupts a frame).
bool s_ctcIndep = false;
int64_t s_ctcZc[4];
#if PERF_TRACE && PERF_HIST
uint32_t s_pf_fast, s_pf_full, s_pf_wcw[4], s_pf_wtc[4], s_pf_rd[4], s_pf_inv;
#define CTC_PF(x) (x)
#else
#define CTC_PF(x) ((void)0)
#endif

// The counters as of now (ctcRead / ctcWrite), without crossing an event: sync()
// already advanced through every ZC/TO up to now.
void ctcCatchUp() {
    for (auto& c : s_ctc)
        if (c.st >= C_TRIGGER) { ctcAdvance(s_last28); break; }
}

// k-th ZC/TO of channel ch from the synced state, kNever if none can come.
NXI_HOT int64_t ctcNextZc(int ch, int64_t k, int depth) {
    if (depth > 4) return kNever;
    const Ctc& c = s_ctc[ch];
    if (running(c) && timer(c))
        return c.run0 + (ticksTo(c, c.ref) + c.cnt + (k - 1) * c.tc) * presc(c) - 1;
    const int src = (ch + 3) & 3;
    if (running(c))
        return ctcNextZc(src, c.cnt + (k - 1) * c.tc, depth + 1);
    if (c.st == C_TRIGGER && timer(c) && (c.cw & 0x08)) {
        const int64_t s = ctcNextZc(src, 1, depth + 1);
        return s >= kNever ? kNever : s + (int64_t)c.tc * k * presc(c) - 1;
    }
    return kNever;
}

// Advance the CTC to `now` and re-derive the next ZC/TO overall (s_ctcNext) and
// among interrupt-enabled channels (s_ctcNextInt).
NXI_HOT void ctcRefresh(int64_t now) {
#ifndef NXI_NO_SOLO   // host differential test switch (tools/nextint_test.cpp)
    if (s_ctcIndep && s_ctcNext != -kNever && now >= s_ctcNext) {
        int64_t nx = kNever, nxInt = kNever;
        for (int ch = 0; ch < 4; ch++) {
            int64_t z = s_ctcZc[ch];
            if (z >= kNever) continue;
            Ctc& c = s_ctc[ch];
            const uint32_t P = (uint32_t)c.tc << presShift(c);   // <= 65536
            if (now >= z) {
                const int64_t d = now - z;
                const int64_t n = d < (int64_t)P ? 1 : 1 + udiv64s(d, P);
                const int64_t tz = z + (n - 1) * (int64_t)P;
                emit(ch, Stream{z, (int64_t)P, n});
                // The state advanceTimer would leave, re-based on the last ZC:
                // the counter has just reloaded (ticksTo(tz) is a whole tick).
                c.ref = tz; c.cnt = c.tc;
                z = tz + (int64_t)P;
                s_ctcZc[ch] = z;
            }
            if (z < nx) nx = z;
            if ((c.cw & 0x80) && z < nxInt) nxInt = z;
        }
        s_ctcNext = nx; s_ctcNextInt = nxInt;
        CTC_PF(s_pf_fast++);
        return;
    }
#endif
    CTC_PF(s_pf_full++);
    CTC_PF(s_ctcNext == -kNever ? s_pf_inv++ : 0);
    s_ctcIndep = false;
    bool any = false;
    for (auto& c : s_ctc) if (c.st >= C_TRIGGER) { any = true; break; }
    s_ctcNext = s_ctcNextInt = kNever;
    if (!any) return;
    ctcAdvance(now);
    for (int ch = 0; ch < 4; ch++) {
        const int64_t t = ctcNextZc(ch, 1, 0);
        if (t > now && t < s_ctcNext) s_ctcNext = t;
        if ((s_ctc[ch].cw & 0x80) && t > now && t < s_ctcNextInt) s_ctcNextInt = t;
    }
    bool indep = true;
    for (int ch = 0; ch < 4; ch++) {
        const Ctc& c = s_ctc[ch];
        if (c.st < C_TRIGGER) { s_ctcZc[ch] = kNever; continue; }
        if (!running(c) || !timer(c)) { indep = false; break; }
        s_ctcZc[ch] = ctcNextZc(ch, 1, 0);
        if (s_ctcZc[ch] <= now) { indep = false; break; }   // not reachable after ctcAdvance
    }
    s_ctcIndep = indep && s_ctcNext < kNever;
}

NXI_HOT void sync() {
    const int64_t now = now28();
    if (now <= s_last28) {
        if (now < s_last28) { s_last28 = now; s_ctcNext = -kNever; s_ctcNextInt = kNever; }   // CPU::reset moved tstates back
        // No time passed, but a CTC write may have invalidated the cached next
        // events (s_ctcNext = -kNever): re-derive them before nextEventT reads.
        else if (s_ctcNext == -kNever) ctcRefresh(now);
        return;
    }
    const int64_t relPrev = s_last28 - s_base28, relNow = now - s_base28;
    if (s_lineEn && s_linePos28 >= 0 && s_linePos28 > relPrev && s_linePos28 <= relNow) {
        raise(D_LINE, s_base28 + s_linePos28, true);
#if NEXT_LINE_DIAG
        ldRaise(s_base28 + s_linePos28, now);
#endif
#if NEXT_TRACE
        g_li_raise++;
        // A pulse already over by the time the loop saw it: the CPU could not
        // take it (bus held by a DMA, or a slice that ran past the event).
        // With interrupts disabled nothing stops the slice there, and the
        // pulse is lost on the board too: not counted (RAMTEST runs DI).
        if (!im2Hw() && Z80::isIFF1() && now >= s_base28 + s_linePos28 + pulseLen28()) {
            g_li_late++;
            g_li_late_by = (uint32_t)((now - s_base28 - s_linePos28) / 8);
        }
#endif
    }
    // CTC: only when a ZC/TO can have happened since the last advance. Between
    // events the counters are bookkeeping that ctcRead / ctcWrite catch up on
    // (ctcCatchUp); advancing here on every sync cost several 64-bit divisions
    // per DMA batch / INT check — TX-1696 (CTC running) spent ~10 ms of a frame
    // in it, hw 2026-10-09.
    if (now >= s_ctcNext) ctcRefresh(now);
    s_last28 = now;
    if (s_unqPulse && now >= s_pulse + pulseLen28()) { s_unqPulse = false; exRecalc(); }
}

NXI_HOT bool pulseUp() { return now28() - s_pulse < pulseLen28() && now28() >= s_pulse; }

// The source currently pulling /INT in hardware IM2 mode, -1 none. Daisy chain:
// a source in service (ACK/ISR) blocks every lower one; one requesting blocks
// them too (im2_device: ieo = iei and reti_decode in S_REQ).
NXI_HOT int presented() {
    // Every source below the lowest one that is requesting or not idle is S_0
    // with no request: the loop would only step over it. With nothing pending
    // (between interrupts — most slices) that is the whole answer.
    const uint32_t any = (uint32_t)s_req | s_active;
    if (!any) return -1;
    for (int d = __builtin_ctz(any); d < D_NUM; d++) {
        if (s_st[d] == S_0 && ((s_req >> d) & 1)) { s_st[d] = S_REQ; s_active |= (uint16_t)(1u << d); }   // im2_device S_0 -> S_REQ on i_int_req
        if (s_st[d] == S_REQ) return d;
        if (s_st[d] != S_0) return -1;
    }
    return -1;
}

// NR 0x22 bit 7: the pulse generator's own /INT (not pulse_int_n), i.e. the
// pulse sources — hardware IM2 requests do not show there.
bool pulseLine() {
    if (im2Hw()) return !cpuIm2() && classicUla();
    return classicUla() || pulseUp();
}

void ctcClockEdge(int ch, int64_t now) {
    Ctc& c = s_ctc[ch];
    if (running(c) && !timer(c)) {
        if (--c.cnt == 0) {
            c.cnt = c.tc;
            bool done[4] = {false, false, false, false};
            done[ch] = true;
            const Stream s{now, 1, 1};
            emit(ch, s);
            propagate(ch, s, done, now);
        }
    } else if (c.st == C_TRIGGER && timer(c) && (c.cw & 0x08)) {
        c.st = C_RUN; c.run0 = c.ref = now; c.cnt = c.tc;
    }
}

} // namespace

namespace NextInt {

void reset(bool) {
    // The board resets the whole interrupt block and the CTC on soft and hard
    // reset alike (zxnext_top_issue5.vhd:880 reset <= hard or soft;
    // zxnext.vhd 5092-5105 for the NR 0xC0-0xCE defaults).
    for (auto& s : s_st) s = S_0;
    s_active = 0;
    s_req = s_status = 0;
    s_c0 = 0; s_c4hi = 0x80;
    s_lineEn = false; s_lineNum = 0;
    retnArmed = false;
    s_unqPulse = false;
    s_pulse = -kNever;
    for (auto& c : s_ctc) { c.st = C_RESET; c.cw = 0; c.tc = 256; c.cnt = 256; c.run0 = c.ref = 0; }
    s_ctcNext = -kNever;
    for (uint8_t r = 0xC0; r <= 0xCE; r++) NextReg::reg[r] = 0;
    NextReg::reg[0xC4] = 0x80;
    s_last28 = now28();
    lineRecalc();
    exRecalc();
}

void lineRecalc() {
    s_linePos28 = -1;
    const uint32_t tpl = VIDEO::tStatesPerLine;
    if (!tpl) return;
    const uint32_t lines = (CPU::statesInFrame >> ESPectrum::multiplicator) / tpl;
    if (!lines || s_lineNum >= lines) return;   // cvc never reaches it
    // zxula_timing.vhd 561-585: the line interrupt fires at hc_ula = 255 (the
    // right edge of the paper, 12 pixels ahead of the display) on the line
    // BEFORE the programmed one, counted by the copper counter cvc, which NR 0x64
    // offsets at the paper top. Rows are counted from the frame INT exactly as
    // the renderer does (NextReg::paperLine); the column is the VHDL's distance
    // from the ULA interrupt (c_int_h) to hc_ula 255, in T-states.
    const uint32_t off = NextReg::reg[0x64] % lines;
    const uint32_t rel = (s_lineNum + 2 * lines - 1 - off) % lines;
    const uint32_t row = (NextReg::paperLine() + rel) % lines;
    const uint8_t t = NextReg::machineTiming();
    const uint32_t hoff = t == 1 ? 128 : t == 3 ? 127 : t == 4 ? 190 : 126;
    s_linePos28 = ((int64_t)row * tpl + hoff) * 8;
}

bool lineEnabled() { return s_lineEn; }
int32_t linePosT() { return s_linePos28 < 0 ? -1 : (int32_t)(s_linePos28 / 8); }
uint16_t lineNumber() { return s_lineNum; }

NXI_HOT void wake() {
    if (CPU::stFrame > CPU::tstates + 1) CPU::stFrame = CPU::tstates + 1;
}

NXI_HOT void wakeNow() {
    const uint32_t t = CPU::tstates ? CPU::tstates : 1;   // stFrame 0 = HALT to the loop
    if (CPU::stFrame > t) CPU::stFrame = t;
}

// EI and RETI on the Next. CPU::loop runs an interrupt handler (IFF1 = 0) as
// one slice to the frame's end, and EI / RETI used to end it at once (wake) so
// the loop could re-derive the slice end with IFF1 = 1 / the daisy chain
// released. That is a slice exit + re-entry and two more syncs per EI and per
// RETI — ~940 a frame under a CTC "SID" player, ~4 slices per interrupt
// (Fluid, host count 2026-11-15). Instead put the slice end where the loop
// would put it now, and end it at once only when the loop has to act at once:
// /INT is up, the zxnDMA holds the bus or a DMA byte is already due. The
// target is the loop's own: next interrupt event (IFF1 set) and the next paced
// DMA byte unless an interrupt in service delays the DMA.
// atRetI: from RETI, whose release of the daisy chain counts at once (wakeNow:
// a request it unblocks is taken right after the RETI); EI delays acceptance
// by one instruction (wake). DELIBERATE CHANGE (2026-11-15): the old RETI used
// wake(), so a second request released by RETI was taken one instruction AFTER
// the RETI (host acknowledge log, Fluid: CTC1 waiting while CTC0's ISR ran).
// im2_control.vhd:234 raises o_reti_seen at T3/T4 of the 4D opcode fetch,
// im2_device.vhd S_ISR -> S_0 on it, and the next device in S_REQ pulls /INT
// at once (o_int_n) — before RETI's two stack reads, so the Z80, sampling /INT
// in the instruction's last T, takes it right after the RETI.
// Everything else in the acknowledge log was identical up to that point.
NXI_HOT void reschedule(bool atRetI) {
#define NXI_ACT() do { if (atRetI) wakeNow(); else wake(); return; } while (0)
    if (Z80DMA::nxPending && Z80DMA::nxStall) NXI_ACT();
    uint32_t t = CPU::stFrame;
    if (Z80::isIFF1()) {
        if (intLine()) NXI_ACT();
        t = nextEventTSynced(t);
    }
    if (Z80DMA::nxPending && !dmaDelay()) {
        const uint32_t d = Z80DMA::nxNextT;
        if (d <= CPU::tstates) NXI_ACT();
        if (d < t) t = d;
    }
    if (t < CPU::stFrame) { CPU::stFrame = t; slEnd = t; }
#undef NXI_ACT
}
// The running slice's end as CPU::loop set it, lowered by reschedule(): a HALT
// later in the slice (stFrame = 0) sleeps to here, not to the stale target.
uint32_t slEnd = 0;

NXI_HOT bool intLine() {
    sync();
    if (im2Hw()) return cpuIm2() ? presented() >= 0 : classicUla();
    return classicUla() || pulseUp();
}

NXI_HOT uint32_t nextEventT(uint32_t limit) {
    sync();
    return nextEventTSynced(limit);
}

// nextEventT for a caller that has just synced (CPU::loop: intLine() right
// before it) — the second sync() only re-derived now28() to find nothing to do.
NXI_HOT uint32_t nextEventTSynced(uint32_t limit) {
    const int64_t now = s_last28;
    int64_t best = kNever;
    if (s_lineEn && s_linePos28 >= 0 && s_base28 + s_linePos28 > now) best = s_base28 + s_linePos28;
    if (s_ctcNextInt > now && s_ctcNextInt < best) best = s_ctcNextInt;   // sync() keeps it
    const uint32_t lo = CPU::tstates + 1;
    if (best >= kNever) return limit > lo ? limit : lo;
    const uint32_t f = f28();                       // 8 >> multiplicator: a shift
    const int64_t rel = best - s_base28;
    const int64_t T = (rel + f - 1) >> (3 - ESPectrum::multiplicator);
    if (T >= (int64_t)limit) return limit > lo ? limit : lo;
    return (uint32_t)T > lo ? (uint32_t)T : lo;
}

NXI_HOT uint8_t ack() {
    sync();
    if (uint16_t* const h = MemESP::nextHotHits) {   // where the CPU was: the hot pool's ranking
        const uint8_t pp = MemESP::mmu[Z80::getRegPC() >> 13];
        if (pp < MemESP::NEXT_PAGES && h[pp] < 0xFFFF) h[pp]++;
    }
#if NEXT_TRACE
    g_int_ack++;
#endif
#if NEXT_LINE_DIAG
    ldAck(s_last28);
#endif
    if (!im2Hw() || !cpuIm2()) return 0xFF;
    const int d = presented();
    if (d < 0) return 0xFF;
    s_st[d] = S_ISR;   // S_ACK, then S_ISR as soon as /M1 rises
    return (uint8_t)((s_c0 & 0xE0) | (d << 1));
}

NXI_HOT void reti() {
    if (!im2Hw() || !cpuIm2()) return;
    sync();
    for (int d = 0; d < D_NUM; d++) {
        if (s_st[d] == S_ACK || s_st[d] == S_ISR) {
            s_st[d] = S_0;
            s_active &= (uint16_t)~(1u << d);
            s_req &= (uint16_t)~(1u << d);
            break;
        }
    }
    reschedule(true);
}

void frameEnd() {
    sync();
    s_base28 += (int64_t)CPU::statesInFrame * f28();
    lineRecalc();
    // The ULA frame interrupt of the new frame: its status bit, and in hardware
    // IM2 mode its latched request (in pulse mode the pulse is the classic window).
    if (!NextReg::portFFIntDisable) raise(D_ULA, s_base28, false);
}

static void setUlaDisable(bool dis) {
    NextReg::portFFIntDisable = dis;
    NextReg::portFF = (uint8_t)((NextReg::portFF & ~0x40) | (dis ? 0x40 : 0));
}

void nrWrite(uint8_t r, uint8_t v) {
    sync();
    switch (r) {
        case 0x20:   // generate interrupts in software (im2_int_unq): status, IM2 latch or a pulse
            if (v & 0x80) raise(D_LINE, s_last28, true);
            if (v & 0x40) raise(D_ULA, s_last28, true);
            for (int ch = 0; ch < 4; ch++) if (v & (1 << ch)) raise(D_CTC0 + ch, s_last28, true);
            if ((v & 0xCF) && !im2Hw()) s_unqPulse = true;
            break;
        case 0x22:   // bit 2 = port #FF bit 6 (one latch), bit 1 line enable, bit 0 line MSB
            setUlaDisable(v & 0x04);
            s_lineEn = v & 0x02;
            s_lineNum = (uint16_t)((s_lineNum & 0xFF) | ((v & 0x01) << 8));
            lineRecalc();
            break;
        case 0x23:
            s_lineNum = (uint16_t)((s_lineNum & 0x100) | v);
            lineRecalc();
            break;
        case 0x64:
            lineRecalc();
            break;
        case 0xC0:
            s_c0 = v & 0xE9;
            if (!(v & 0x01)) { for (auto& s : s_st) s = S_0; s_active = 0; s_req = 0; }   // im2_reset_n
            if (!(v & 0x08)) retnArmed = false;
            break;
        case 0xC4:   // bit 7 expansion bus, bit 1 line (= NR 0x22 bit 1), bit 0 ULA (= NOT #FF bit 6)
            s_c4hi = v & 0x80;
            s_lineEn = v & 0x02;
            setUlaDisable(!(v & 0x01));
            lineRecalc();
            break;
        case 0xC5:   // CTC channel interrupt enables = control word bit 7
            for (int ch = 0; ch < 4; ch++)
                s_ctc[ch].cw = (uint8_t)((s_ctc[ch].cw & 0x7F) | ((v >> ch) & 1 ? 0x80 : 0));
            s_ctcNext = -kNever;   // the next sync() re-derives s_ctcNextInt
            break;
        case 0xC8:
            if (v & 0x02) s_status &= (uint16_t)~(1u << D_LINE);
            if (v & 0x01) s_status &= (uint16_t)~(1u << D_ULA);
            break;
        case 0xC9:
            s_status &= (uint16_t)~((uint16_t)v << D_CTC0);
            break;
        // 0xC2/0xC3 (NMI return address), 0xC6/0xCA (UART), 0xCC-0xCE (DMA
        // interruption enables) live in NextReg::reg.
        default: break;
    }
    exRecalc();
}

bool nrRead(uint8_t r, uint8_t& out) {
    switch (r) {
        case 0x20: {
            sync();
            const uint16_t s = s_status | s_req;
            out = (uint8_t)(((s >> D_LINE) & 1) << 7 | ((s >> D_ULA) & 1) << 6 | ((s >> D_CTC0) & 0x0F));
            return true;
        }
        case 0x22:
            sync();
            out = (uint8_t)((pulseLine() ? 0x80 : 0) | (NextReg::portFFIntDisable ? 0x04 : 0) |
                            (s_lineEn ? 0x02 : 0) | (s_lineNum >> 8));
            return true;
        case 0x23: out = (uint8_t)s_lineNum; return true;
        case 0xC0: {
            const Z80::IntMode m = Z80::getIM();
            const uint8_t im = m == Z80::IM2 ? 2 : m == Z80::IM1 ? 1 : 0;
            out = (uint8_t)((s_c0 & 0xE9) | (im << 1));
            return true;
        }
        case 0xC4: out = (uint8_t)(s_c4hi | (s_lineEn ? 0x02 : 0) | (NextReg::portFFIntDisable ? 0 : 0x01)); return true;
        case 0xC5: {
            uint8_t e = 0;
            for (int ch = 0; ch < 4; ch++) if (s_ctc[ch].cw & 0x80) e |= (uint8_t)(1 << ch);
            out = e;
            return true;
        }
        case 0xC6: case 0xCE: out = NextReg::reg[r] & 0x77; return true;   // zxnext.vhd 6263
        case 0xCC: out = NextReg::reg[r] & 0x83; return true;
        case 0xC8: {
            sync();
            const uint16_t s = s_status | s_req;
            out = (uint8_t)(((s >> D_LINE) & 1) << 1 | ((s >> D_ULA) & 1));
            return true;
        }
        case 0xC9: {
            sync();
            out = (uint8_t)(((s_status | s_req) >> D_CTC0) & 0x0F);
            return true;
        }
        case 0xCA: out = 0; return true;   // no UART
        default: return false;
    }
}

NXI_HOT bool dmaDelay() {
    const uint8_t cc = NextReg::reg[0xCC], cd = NextReg::reg[0xCD], ce = NextReg::reg[0xCE];
    // zxnext.vhd 2007: nmi_activated, which drops at S_NMI_END — i.e. once the
    // divMMC hold (button latched or automap held) is gone. next_nmi_hold alone
    // stays set until the NEXT request, so testing it held every DMA for good
    // after the first NMI (Atic Atac Remake: copper NMI every line, NR CC = 80,
    // the sprite pattern DMA never ran, hw 2026-10-16).
    if ((cc & 0x80) && (NextMf::active() || DivMMC::next_nmi_btn ||
                        (DivMMC::next_nmi_hold && DivMMC::automap))) return true;
    if (!im2Hw()) return false;
    // im2_dma_int_en(13:0), zxnext.vhd 1957: 13 UART1 TX, 12 UART0 TX, 11 ULA,
    // 10:3 CTC 7..0, 2 UART1 RX, 1 UART0 RX, 0 line.
    const uint16_t en = (uint16_t)(((ce >> 6) & 1) << 13 | ((ce >> 2) & 1) << 12 | (cc & 1) << 11 |
                                   (uint16_t)cd << 3 | ((ce & 0x30) ? 1 : 0) << 2 |
                                   ((ce & 0x03) ? 1 : 0) << 1 | ((cc >> 1) & 1));
    if (!en) return false;
    sync();
    for (int d = 0; d < D_NUM; d++)
        if (((en >> d) & 1) && (s_st[d] != S_0 || ((s_req >> d) & 1))) return true;
    return false;
}

uint8_t ctcRead(uint8_t ch) {
    if (ch > 3) return 0;   // channels 4..7 are not instantiated (zxnext.vhd 4064)
    CTC_PF(s_pf_rd[ch]++);
    sync();
    ctcCatchUp();
    const Ctc& c = s_ctc[ch];
    return (uint8_t)((c.st == C_RUN || c.st == C_RUN_TC) ? c.cnt : c.tc);
}

// The general CTC write (flash): everything ctcWrite's fast path does not take.
static __attribute__((noinline)) void ctcWriteSlow(uint8_t ch, uint8_t v);

// SRAM (.nxovl): ~1000 writes a frame under a CTC player, nearly all of them the
// fast path below; the general path is a call to flash.
NXI_HOT void ctcWrite(uint8_t ch, uint8_t v) {
    if (ch > 3) return;
    sync();
#ifndef NXI_NO_WFAST   // host differential test switch (tools/nextint_wfast_test.cpp)
    // Independent running timers (s_ctcIndep, the state ctcRefresh's fast path
    // keeps): a write that leaves this channel a plain running timer, or stops
    // it, moves only ITS next ZC — no catch-up of the other channels and no
    // full re-derivation at the next sync. The shapes a CTC "SID" player uses
    // in every interrupt (Fluid: control word #85 "TC follows", then the time
    // constant, on three channels — ~1000 writes a frame at ~4 us each through
    // the general path, hw 2026-11-15):
    //  - time constant for a running channel (C_RUN_TC): used at the next
    //    reload, the pending ZC does not move;
    //  - control word with software reset: the channel stops;
    //  - time constant after that reset, timer mode without a trigger: the
    //    channel starts now.
    //  - control word without reset for a running timer (below).
    // Anything else (counter mode, trigger, an edge change, a control word for
    // a channel waiting for its constant) takes the general path below.
    if (s_ctcIndep && s_ctcNext != -kNever) {
        Ctc& c = s_ctc[ch];
        bool done = false;
        if (c.st == C_RUN_TC) {
            c.tc = v ? v : 256;
            c.st = C_RUN;
            done = true;
        } else if (c.st == C_RESET_TC) {
            if (timer(c) && !(c.cw & 0x08)) {
                c.tc = v ? v : 256;
                c.cnt = c.tc;
                c.st = C_RUN; c.run0 = c.ref = s_last28;
                s_ctcZc[ch] = ctcNextZc(ch, 1, 0);
                done = true;
            }
        } else if ((v & 0x03) == 0x03) {
            c.cw = v & 0xF8;
            c.ref = s_last28;
            c.st = (v & 0x04) ? C_RESET_TC : C_RESET;
            c.cnt = c.tc;
            s_ctcZc[ch] = kNever;
            done = true;
        } else if ((v & 0x03) == 0x01 && c.st == C_RUN && timer(c) && !((v ^ c.cw) & 0x10) && !(v & 0x40)) {
            // Control word without reset for a running timer that stays a timer,
            // same edge (Fluid's actual shape: #85 "TC follows", then the constant):
            // this channel's counter to now, exactly as ctcCatchUp would leave it
            // (no ZC can be due — sync() has just passed them all), then the same
            // re-base the general path does.
            advanceQuiet(c, s_last28);
            c.cw = v & 0xF8;
            c.ref = s_last28;
            if (v & 0x04) c.st = C_RUN_TC;
            s_ctcZc[ch] = ctcNextZc(ch, 1, 0);
            done = true;
        }
        if (done) {
            int64_t nx = kNever, nxInt = kNever;
            for (int i = 0; i < 4; i++) {
                const int64_t z = s_ctcZc[i];
                if (z < nx) nx = z;
                if ((s_ctc[i].cw & 0x80) && z < nxInt) nxInt = z;
            }
            s_ctcNext = nx; s_ctcNextInt = nxInt;
            exRecalc();
            return;
        }
    }
#endif
    ctcWriteSlow(ch, v);
}

static __attribute__((noinline)) void ctcWriteSlow(uint8_t ch, uint8_t v) {
    ctcCatchUp();
    s_ctcNext = -kNever;
    const int64_t now = s_last28;
    Ctc& c = s_ctc[ch];
    CTC_PF((c.st == C_RESET_TC || c.st == C_RUN_TC) ? s_pf_wtc[ch]++ : s_pf_wcw[ch]++);
    if (c.st == C_RESET_TC || c.st == C_RUN_TC) {          // time constant
        c.tc = v ? v : 256;
        if (c.st == C_RESET_TC) {
            c.cnt = c.tc;
            if (timer(c) && (c.cw & 0x08)) c.st = C_TRIGGER;
            else { c.st = C_RUN; c.run0 = c.ref = now; }
        } else {
            c.st = C_RUN;                                   // used at the next reload
        }
    } else if (v & 0x01) {                                  // control word
        const bool edgeChg = ((v ^ c.cw) & 0x10) != 0;
        c.cw = v & 0xF8;
        c.ref = now;
        if (v & 0x02) {                                     // software reset
            c.st = (v & 0x04) ? C_RESET_TC : C_RESET;
            c.cnt = c.tc;
        } else {
            if (v & 0x04) {
                if (c.st == C_RESET || c.st == C_TRIGGER) c.st = C_RESET_TC;
                else if (c.st == C_RUN) c.st = C_RUN_TC;
            }
            if (edgeChg) ctcClockEdge(ch, now);             // counts as a clock edge
        }
    }
    // bit 0 = 0: interrupt vector — the Next takes vectors from NR 0xC0 (open in the VHDL)
    exRecalc();
}

bool nmiStackless(uint16_t pc) {
    NextReg::reg[0xC2] = (uint8_t)pc;
    NextReg::reg[0xC3] = (uint8_t)(pc >> 8);
    if (!(s_c0 & 0x08)) return false;
    retnArmed = true;
    return true;
}

uint16_t stacklessRetn() {
    retnArmed = false;
    return (uint16_t)(NextReg::reg[0xC2] | (NextReg::reg[0xC3] << 8));
}

#if PERF_TRACE && PERF_HIST
void ctcProfDump() {
    char b[240]; int n = snprintf(b, sizeof b, "NXPROF ctc: fast=%u full=%u (inval %u) indep=%d |",
        (unsigned)s_pf_fast, (unsigned)s_pf_full, (unsigned)s_pf_inv, (int)s_ctcIndep);
    for (int ch = 0; ch < 4; ch++)
        n += snprintf(b + n, sizeof b - n, " ch%d st%u cw%02X tc%u wcw%u wtc%u rd%u", ch,
            (unsigned)s_ctc[ch].st, (unsigned)s_ctc[ch].cw, (unsigned)s_ctc[ch].tc,
            (unsigned)s_pf_wcw[ch], (unsigned)s_pf_wtc[ch], (unsigned)s_pf_rd[ch]);
    Debug::log("%s", b);
    s_pf_fast = s_pf_full = s_pf_inv = 0;
    for (int ch = 0; ch < 4; ch++) s_pf_wcw[ch] = s_pf_wtc[ch] = s_pf_rd[ch] = 0;
}
#endif
}

// ===== .pss snapshot (PSNI) =====
// Flash code. Absolute 28 MHz times are written relative to the frame start
// (s_base28); the load puts the frame start at 0 and re-derives `now` from the
// restored CPU::tstates and clock, so every event keeps its place in the frame.
#include "app/SnapPack.h"
namespace NextInt {

static constexpr uint8_t NXI_SNAP_VER = 1;

uint32_t snapSave(uint8_t* out) {
    sync();
    ctcCatchUp();
    SnapOut o(out);
    o.u8(NXI_SNAP_VER);
    o.raw(s_st, D_NUM);
    o.u16(s_active); o.u16(s_req); o.u16(s_status);
    o.u8(s_c0); o.u8(s_c4hi); o.u8(s_lineEn ? 1 : 0); o.u16(s_lineNum);
    o.u8(s_unqPulse ? 1 : 0); o.u8(retnArmed ? 1 : 0);
    const bool pul = s_pulse > -kNever / 2;
    o.u8(pul ? 1 : 0); o.u64((uint64_t)(pul ? s_pulse - s_base28 : 0));
    for (const Ctc& c : s_ctc) {
        o.u8(c.st); o.u8(c.cw); o.u16(c.tc); o.u16(c.cnt);
        o.u64((uint64_t)(c.run0 - s_base28)); o.u64((uint64_t)(c.ref - s_base28));
    }
    return o.n;
}

void snapLoad(const uint8_t* in, uint32_t n) {
    SnapIn s(in, n);
    if (s.u8() < 1) return;
    uint8_t st[D_NUM];
    s.raw(st, D_NUM);
    const uint16_t act = s.u16(), req = s.u16(), status = s.u16();
    const uint8_t c0 = s.u8(), c4 = s.u8(), lineEn = s.u8();
    const uint16_t lineNum = s.u16();
    const uint8_t unq = s.u8(), retn = s.u8(), pul = s.u8();
    const int64_t pulse = (int64_t)s.u64();
    Ctc ctc[4];
    for (Ctc& c : ctc) {
        c.st = s.u8(); c.cw = s.u8(); c.tc = s.u16(); c.cnt = s.u16();
        c.run0 = (int64_t)s.u64(); c.ref = (int64_t)s.u64();
        if (c.st > C_RUN_TC) c.st = C_RESET;
        if (!c.tc || c.tc > 256) c.tc = 256;
        if (!c.cnt || c.cnt > 256) c.cnt = c.tc;
    }
    if (!s.ok) return;
    s_base28 = 0;
    s_last28 = now28();
    for (int d = 0; d < D_NUM; d++) s_st[d] = st[d] <= S_ISR ? st[d] : S_0;
    s_active = act & ((1u << D_NUM) - 1); s_req = req; s_status = status;
    s_c0 = c0 & 0xE9; s_c4hi = c4 & 0x80;
    s_lineEn = lineEn != 0; s_lineNum = lineNum & 0x1FF;
    s_unqPulse = unq != 0; retnArmed = retn != 0;
    s_pulse = pul ? pulse : -kNever;
    for (int ch = 0; ch < 4; ch++) {
        s_ctc[ch] = ctc[ch];
        // A time ahead of `now` cannot have been recorded; clamp so advanceTimer
        // never runs backwards.
        if (s_ctc[ch].ref > s_last28) s_ctc[ch].ref = s_last28;
        if (s_ctc[ch].run0 > s_ctc[ch].ref) s_ctc[ch].run0 = s_ctc[ch].ref;
    }
    s_ctcNext = -kNever; s_ctcNextInt = kNever; s_ctcIndep = false;
    lineRecalc();
    exRecalc();
    wake();
}

} // namespace NextInt
