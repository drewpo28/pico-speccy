// FmTables — the two heap tables the OPL-family cores (OplFm = YMF262,
// OpllFm = YM2413) share. Both are ports of the same Burczynski lineage with
// the same ENV_STEP/SIN_LEN/TL_RES_LEN, so their tables are ONE table:
//   sin0[1024]   waveform 0 in the chip's log domain (value*2 + sign bit),
//                byte-identical between ymf262.cpp and ym2413.cpp init math;
//   tl_base[256] the x = 0..255 row of MAME's tl_tab, UNSHIFTED (the OPLL
//                form). The OPL3 row is exactly this << 1, applied at the
//                fetch — floor((2n) >> k) == floor(n << 1 >> k), so bit-exact.
// Checked exhaustively before the merge (0 mismatches over both tables) and
// at the render level by tools/vgm_render_crc.cpp. Saves 2.5 KB whenever both
// chips are on. Reference counted: every OplFm/OpllFm instance acquires in
// its constructor and releases in its destructor.
#ifndef FMTABLES_H
#define FMTABLES_H
#include <stdint.h>

namespace FmTab {
    extern uint16_t* tl_base;   // 256 entries
    extern uint16_t* sin0;      // 1024 entries
    // Bump the refcount and build on the first user. Returns false when the
    // heap refused (tryMalloc — no panic); release() must still pair with it.
    bool acquire();
    void release();
    inline bool ready() { return tl_base && sin0; }
}
#endif
