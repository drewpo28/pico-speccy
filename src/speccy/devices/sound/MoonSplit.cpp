// pico-speccy — the ZXM-MoonSound (OPL4) on core1. See MoonSplit.h.

#include "MoonSplit.h"

#if MOONSOUND

#include "MoonPcm.h"
#include "OplFm.h"
#include "app/TryAlloc.h"
#include "app/Debug.h"
#include "app/ESPectrum.h"
#include <pico/sync.h>
#include <pico/time.h>
#include <string.h>

bool g_moon_c1_live = false;
volatile uint32_t MoonSplit::safe = 0;

namespace {

constexpr uint32_t RING  = 512;                          // entries, power of two
constexpr int      BUF   = ESP_AUDIO_SAMPLES_PENTAGON;   // the OPL buffer's own length
constexpr uint32_t CHUNK = 32;                           // samples per core1 step

constexpr uint32_t RV_FM = 1u << 16;                     // entry: an FM port write (reg = port & 3)

struct Entry { uint32_t pos; uint32_t rv; };             // reg | value << 8 | RV_FM
struct Blk {
    Entry   q[RING];
    int16_t L[BUF];
    int16_t R[BUF];
};

Blk*         s_b    = nullptr;
spin_lock_t* s_lock = nullptr;
volatile uint32_t s_done = 0;   // samples generated this frame (written under the lock)
volatile uint32_t s_qw   = 0;   // ring write index, core0
uint32_t s_qr   = 0;            // ring read index, under the lock
uint32_t s_end  = 0;            // this frame's sample count
bool     s_sounded  = false;    // the wave half output anything this frame
bool     s_finished = false;    // frameJoin ran for this frame
// The FM half, attached per frame by frameBoundary (nullptr = wave only):
OplFm*   s_fm  = nullptr;
int16_t* s_fmL = nullptr;
int16_t* s_fmR = nullptr;
#if OPL_PERF_TRACE
volatile uint32_t s_c1fm = 0, s_c1wave = 0, s_joinus = 0;
#endif

inline void lock()   { spin_lock_unsafe_blocking(s_lock); }
inline void unlock() { spin_unlock_unsafe(s_lock); }

inline void applyEntry(const Entry& e) {
    if (e.rv & RV_FM) { if (s_fm) s_fm->write((int)(e.rv & 3), (uint8_t)(e.rv >> 8)); }
    else moonpcm->write((uint8_t)e.rv, (uint8_t)(e.rv >> 8));
}

// Apply every queued write due at or before s_done.
inline void applyDue() {
    while (s_qr != s_qw) {
        __dmb();
        const Entry& e = s_b->q[s_qr & (RING - 1)];
        if (e.pos > s_done) break;
        applyEntry(e);
        s_qr++;
    }
}

// Generate [s_done, to) with the queued writes applied at their positions:
// the FM half accumulates into the OPL buffer (zeroed per frame), the wave
// half replaces its own. Under the lock.
void genTo(uint32_t to) {
    if (to > s_end) to = s_end;
    applyDue();
    while (s_done < to) {
        uint32_t next = to;
        if (s_qr != s_qw) {
            __dmb();
            const uint32_t p = s_b->q[s_qr & (RING - 1)].pos;
            if (p < next) next = p;
        }
        const int n = (int)(next - s_done), off = (int)s_done;
#if OPL_PERF_TRACE
        const uint32_t t0 = time_us_32();
#endif
        if (s_fm) s_fm->gen(s_fmL, s_fmR, n, off);
#if OPL_PERF_TRACE
        const uint32_t t1 = time_us_32();
        if (get_core_num() == 1) s_c1fm += t1 - t0;
#endif
        if (moonpcm->genPcm(s_b->L, s_b->R, n, off)) s_sounded = true;
#if OPL_PERF_TRACE
        if (get_core_num() == 1) s_c1wave += time_us_32() - t1;
#endif
        s_done = next;
        applyDue();
    }
}

} // namespace

bool MoonSplit::init() {
    if (s_b || !moonpcm) return s_b != nullptr;
    const int id = spin_lock_claim_unused(false);
    if (id < 0) { Debug::log("[MOON] no spinlock for the core1 chip"); return false; }
    s_b = (Blk*)tryMalloc(sizeof(Blk));
    if (!s_b) { spin_lock_unclaim(id); Debug::log("[MOON] no heap for the core1 chip (%u B)", (unsigned)sizeof(Blk)); return false; }
    memset(s_b, 0, sizeof(Blk));
    s_lock = spin_lock_init(id);
    s_done = 0; s_qr = s_qw = 0; s_end = 0; safe = 0;
    __dmb();
    g_moon_c1_live = true;
    Debug::log("[MOON] OPL4 FM + wave on core1 (%u B)", (unsigned)sizeof(Blk));
    return true;
}

bool MoonSplit::on() { return s_b != nullptr; }

static inline void ringPut(uint32_t pos, uint32_t rv) {
    Entry& e = s_b->q[s_qw & (RING - 1)];
    e.pos = pos;
    e.rv  = rv;
    __dmb();
    s_qw = s_qw + 1;
}

void MoonSplit::waveWrite(uint32_t pos, uint8_t reg, uint8_t v) {
    if (s_qw - s_qr >= RING) sync(pos);   // full: drain on core0 (s_qr may read stale = fuller, harmless)
    ringPut(pos, (uint32_t)reg | ((uint32_t)v << 8));
}

void MoonSplit::fmWrite(uint32_t pos, uint8_t port, uint8_t v) {
    if (s_qw - s_qr >= RING) sync(pos);
    ringPut(pos, RV_FM | (uint32_t)(port & 3) | ((uint32_t)v << 8));
}

void MoonSplit::sync(uint32_t pos) {
    lock();
    genTo(pos);
    unlock();
}

void MoonSplit::hold()    { lock(); }
void MoonSplit::release() { unlock(); }

void MoonSplit::fmDetach() {
    lock();
    s_fm = nullptr; s_fmL = s_fmR = nullptr;
    unlock();
}

void MoonSplit::fmAttach(OplFm* fm, int16_t* fmL, int16_t* fmR) {
    lock();
    s_fm = fm; s_fmL = fmL; s_fmR = fmR;
    if (s_fm && s_done < s_end) {   // the rest of this frame accumulates onto zeros
        memset(s_fmL + s_done, 0, (s_end - s_done) * sizeof(int16_t));
        memset(s_fmR + s_done, 0, (s_end - s_done) * sizeof(int16_t));
    }
    unlock();
}

void MoonSplit::frameBoundary(int samples, OplFm* fm, int16_t* fmL, int16_t* fmR) {
    lock();
    if (!s_finished) {
        // an unmixed frame (maxSpeed, pause): nothing is lost, the writes land
        while (s_qr != s_qw) {
            __dmb();
            applyEntry(s_b->q[s_qr & (RING - 1)]);
            s_qr++;
        }
    }
    s_end = (uint32_t)(samples > BUF ? BUF : samples);
    s_done = 0;
    safe = 0;
    s_sounded = false;
    s_finished = false;
    s_fm = fm; s_fmL = fmL; s_fmR = fmR;
    if (s_fm) {   // OplFm::gen accumulates
        memset(s_fmL, 0, s_end * sizeof(int16_t));
        memset(s_fmR, 0, s_end * sizeof(int16_t));
    }
    unlock();
}

bool MoonSplit::frameJoin(int samples) {
#if OPL_PERF_TRACE
    const uint32_t t0 = time_us_32();
#endif
    if (samples > BUF) samples = BUF;
    safe = (uint32_t)samples;
    lock();
    genTo((uint32_t)samples);
    // writes stamped at or past the end belong before whatever comes next
    while (s_qr != s_qw) {
        __dmb();
        applyEntry(s_b->q[s_qr & (RING - 1)]);
        s_qr++;
    }
    const bool sounded = s_sounded;
    s_finished = true;
    unlock();
#if OPL_PERF_TRACE
    s_joinus += time_us_32() - t0;
#endif
    return sounded;
}

void MoonSplit::frameSum(int16_t* L, int16_t* R, int samples) {
    if (samples > BUF) samples = BUF;
    const int32_t fl = moonpcm->fmMixL(), fr = moonpcm->fmMixR();
    if (!s_sounded && fl == 4096 && fr == 4096) return;
    const int16_t* wL = s_b->L;
    const int16_t* wR = s_b->R;
    for (int i = 0; i < samples; i++) {
        int32_t l = ((L[i] * fl) >> 12) + wL[i];
        int32_t r = ((R[i] * fr) >> 12) + wR[i];
        L[i] = l > 32767 ? 32767 : l < -32768 ? -32768 : (int16_t)l;
        R[i] = r > 32767 ? 32767 : r < -32768 ? -32768 : (int16_t)r;
    }
}

void MoonSplit::resetChip(uint32_t rate) {
    lock();
    s_qr = s_qw;
    moonpcm->reset();
    moonpcm->setRate(rate);
    unlock();
}

void MoonSplit::core1Pump() {
    uint32_t to = safe;
    if (to > s_end) to = s_end;
    if (s_done >= to) return;
    lock();
    // re-read under the lock: a frameBoundary in between has moved the cursors
    to = safe;
    if (to > s_end) to = s_end;
    if (to > s_done + CHUNK) to = s_done + CHUNK;
    if (s_done < to) genTo(to);
    unlock();
}

void MoonSplit::perfTake(uint32_t& c1fm, uint32_t& c1wave, uint32_t& joinus) {
#if OPL_PERF_TRACE
    c1fm = s_c1fm; s_c1fm = 0;
    c1wave = s_c1wave; s_c1wave = 0;
    joinus = s_joinus; s_joinus = 0;
#else
    c1fm = c1wave = joinus = 0;
#endif
}

#endif // MOONSOUND
