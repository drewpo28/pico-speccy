// See PerfFdc.h. Flash code: PERF_TRACE builds only.
#include "app/PerfFdc.h"

#if PERF_TRACE
#include <stdio.h>
#include <string.h>
#include "app/Debug.h"
#include "app/ESPectrum.h"
#include "speccy/z80/CPU.h"
#include "speccy/z80/z80.h"
#include "speccy/core/MemESP.h"

namespace PerfFdc {

static uint64_t s_last = 0;
static uint16_t s_lines = 0;
static const uint16_t kMaxLines = 1500;

static inline uint64_t nowBase() {
    // Guest T-states in 3.5 MHz units, so machines at different clocks compare.
    return (CPU::global_tstates + CPU::tstates) >> ESPectrum::multiplicator;
}

void cmd(uint8_t c, uint8_t trk, uint8_t sec, uint8_t drv) {
    const uint64_t t = nowBase();
    const uint32_t dt = s_last ? (uint32_t)(t - s_last) : 0;
    s_last = t;
    if (s_lines >= kMaxLines) return;
    s_lines++;
    // EvoProfROM's FDD driver (ROM page 17) keeps its state in #BFC8..#BFEF of the
    // work page mapped at window 2: #BFD8..#BFDB = per-drive motor countdown
    // (0 / #FF = motor off -> a ~103 ms spin-up delay before the next sector),
    // #BFDC = flags (bit 4 motor on, bit 7 ...).
    const uint8_t* w2 = MemESP::ramCurrent[2];
    Debug::log("[FDC] +%luT (%lums) cmd=%02X tr=%u sec=%u drv=%u pc=%04X dos=%d mt=%02X%02X%02X%02X fl=%02X sp=%04X",
               (unsigned long)dt, (unsigned long)(dt / 3500u), c, trk, sec, drv,
               Z80::getRegPC(), (int)ESPectrum::trdos,
               w2[0x3FD8], w2[0x3FD9], w2[0x3FDA], w2[0x3FDB], w2[0x3FDC], Z80::getRegSP());
}

struct Ent { uint16_t pc; uint8_t port, w; uint32_t n; };
static Ent s_tab[16];
static uint32_t s_total = 0, s_dropped = 0;

// Watch EvoProfROM's FDD-driver state in its work page (RAM #FB, mapped at #8000
// by #B7F7 = 04): #BFC8..#BFDF, offsets #3FC8.. in the page. Logged on change with
// the PC, from every FDC port access and once per frame.
static uint8_t s_ws[24]; static bool s_wsv = false; static uint16_t s_wsn = 0;
static void watch(const char* where) {
    if (MEM_PG_CNT < 256) return;
    const uint8_t* q = MemESP::ram[0xFB].direct();
    if (!q) return;
    q += 0x3FC8;
    if (s_wsv && memcmp(q, s_ws, sizeof s_ws) == 0) return;
    memcpy(s_ws, q, sizeof s_ws); s_wsv = true;
    if (s_wsn >= 400) return;
    s_wsn++;
    char b[80]; int bp = 0;
    for (int i = 0; i < 24; i++) bp += snprintf(b + bp, sizeof b - bp, "%02X", q[i]);
    Debug::log("[FDCW] %s pc=%04X w2=%s C8:%s", where, Z80::getRegPC(),
               MemESP::ramCurrent[2] == MemESP::ram[0xFB].direct() ? "FB" : "--", b);
}
void frame() { watch("frm"); }

void port(uint8_t p, bool w) {
    watch("port");
    const uint16_t pc = Z80::getRegPC();
    s_total++;
    int freeI = -1, minI = 0;
    for (int i = 0; i < 16; i++) {
        Ent& e = s_tab[i];
        if (e.n && e.pc == pc && e.port == p && e.w == (uint8_t)w) { e.n++; return; }
        if (!e.n && freeI < 0) freeI = i;
        if (e.n < s_tab[minI].n) minI = i;
    }
    if (freeI < 0) { s_dropped++; if (s_tab[minI].n > 1) { s_tab[minI].n--; return; } freeI = minI; }
    s_tab[freeI] = { pc, p, (uint8_t)w, 1 };
}

void dump() {
    if (!s_total) return;
    // Sort by count, print the top 8.
    Ent t[16];
    for (int i = 0; i < 16; i++) t[i] = s_tab[i];
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++)
            if (t[j].n > t[i].n) { Ent x = t[i]; t[i] = t[j]; t[j] = x; }
    char b[200]; int bp = 0;
    for (int i = 0; i < 8 && t[i].n && bp < 180; i++)
        bp += snprintf(b + bp, sizeof b - bp, " %04X:%c%02X x%lu", t[i].pc, t[i].w ? 'W' : 'R',
                       t[i].port, (unsigned long)t[i].n);
    b[bp] = 0;
    Debug::log("[PERF] fdc: acc=%lu drop=%lu mult=%u dos=%d%s", (unsigned long)s_total,
               (unsigned long)s_dropped, (unsigned)ESPectrum::multiplicator, (int)ESPectrum::trdos, b);
    for (int i = 0; i < 16; i++) s_tab[i].n = 0;
    s_total = s_dropped = 0;
}

void reset() { s_lines = 0; s_last = 0; s_wsn = 0; }

} // namespace PerfFdc
#endif
