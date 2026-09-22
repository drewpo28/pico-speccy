// FmTables — see FmTables.h. Byte-for-byte MAME ymf262.cpp / ym2413.cpp
// init_tables() math (mame0220, GPL-2.0+); the two files' formulas differ only
// in spelling ((m > 0 ? 1 : -1) / m vs 1 / fabs(m), which IEEE makes identical)
// and in the OPL3 baking a << 1 into tl_tab, which OplFm applies at the fetch.
#include "FmTables.h"
#include "TryAlloc.h"
#include <math.h>
#include <stdlib.h>

namespace FmTab {

uint16_t* tl_base = nullptr;
uint16_t* sin0    = nullptr;
static int s_refs = 0;

static const double ENV_STEP = 128.0 / 1024.0;   // ENV_BITS 10
enum { SIN_LEN = 1024, TL_RES_LEN = 256 };

bool acquire() {
    if (s_refs++ > 0) return ready();
    // tryMalloc, not malloc: pico_malloc panics on NULL, and the subsystems'
    // tablesReady() tests exist so a thin heap degrades the chip to Off.
    tl_base = (uint16_t*)tryMalloc(TL_RES_LEN * sizeof(uint16_t));
    sin0    = (uint16_t*)tryMalloc(SIN_LEN * sizeof(uint16_t));
    if (!tl_base || !sin0) {
        free(tl_base); tl_base = nullptr;
        free(sin0);    sin0 = nullptr;
        return false;
    }
    for (int x = 0; x < TL_RES_LEN; x++) {
        double m = floor((1 << 16) / pow(2, (x + 1) * (ENV_STEP / 4.0) / 8.0));
        int n = ((int)m) >> 4;
        n = (n & 1) ? (n >> 1) + 1 : n >> 1;
        tl_base[x] = (uint16_t)n;
    }
    for (int i = 0; i < SIN_LEN; i++) {
        double m = sin(((i * 2) + 1) * M_PI / SIN_LEN);
        double o = 8 * log(1.0 / fabs(m)) / log(2.0);
        o = o / (ENV_STEP / 4);
        int n = (int)(2.0 * o);
        n = (n & 1) ? (n >> 1) + 1 : n >> 1;
        sin0[i] = (uint16_t)(n * 2 + (m >= 0.0 ? 0 : 1));
    }
    return true;
}

void release() {
    if (--s_refs > 0) return;
    s_refs = 0;
    free(tl_base); tl_base = nullptr;
    free(sin0);    sin0 = nullptr;
}

} // namespace FmTab
